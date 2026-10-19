// DHCP Finder
// Asks "who hands out IP addresses here?" by broadcasting a DHCP DISCOVER and listening for every
// OFFER. One server is normal; two or more means a rogue DHCP server (usually someone who plugged
// in a home router), which hands out wrong addresses and breaks the network. A classic LAN-party
// fault, so the screen turns red the moment a second server answers.
//
// It only asks and listens: it never accepts an address or changes the device's own networking.
// Keys: BTN_B (nothing), BTN_A ask again now.
#pragma once
#include "lwip/sockets.h"

namespace dhcpfinder {

const int MAX_SRV = 8;
const uint32_t ASK_GAP_MS = 4000;     // re-send a DISCOVER this often

struct Srv {
  IPAddress id;          // server identifier (option 54), or the source IP
  IPAddress offered;     // the address it offered us (yiaddr)
  IPAddress gw;          // router it handed out (option 3)
  uint8_t   mac[6];
  bool      haveMac;
  uint32_t  lastSeen;
};
Srv srv[MAX_SRV];
int numSrv = 0;

int  sock = -1;
uint32_t lastAsk = 0, startedAt = 0;
uint32_t myXid = 0;
int  asks = 0;

bool openSock() {
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return false;
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);   // share port 68 with the system client
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(68);
  a.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock, (struct sockaddr*)&a, sizeof a) < 0) { close(sock); sock = -1; return false; }
  int fl = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, fl | O_NONBLOCK);
  return true;
}

void sendDiscover() {
  if (sock < 0 || !wifiUp()) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  myXid = esp_random();

  uint8_t p[300];
  memset(p, 0, sizeof p);
  p[0] = 1;            // op: BOOTREQUEST
  p[1] = 1;            // htype: ethernet
  p[2] = 6;            // hlen
  p[4] = (myXid >> 24) & 0xFF; p[5] = (myXid >> 16) & 0xFF;
  p[6] = (myXid >> 8) & 0xFF;  p[7] = myXid & 0xFF;
  p[10] = 0x80;        // flags: broadcast, so OFFERs come back as broadcast (we can hear them)
  memcpy(p + 28, mac, 6);                   // chaddr
  p[236] = 0x63; p[237] = 0x82; p[238] = 0x53; p[239] = 0x63;   // magic cookie
  int o = 240;
  p[o++] = 53; p[o++] = 1; p[o++] = 1;      // DHCP message type = DISCOVER
  p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;  // param request: mask, router, DNS
  p[o++] = 255;                             // end

  struct sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(67);
  dst.sin_addr.s_addr = 0xFFFFFFFF;         // 255.255.255.255
  sendto(sock, p, o, 0, (struct sockaddr*)&dst, sizeof dst);
  asks++;
}

// Walk the options for a given code; returns the value pointer and length.
const uint8_t* opt(const uint8_t* p, int len, uint8_t code, int& outLen) {
  int o = 240;
  while (o + 1 < len) {
    uint8_t c = p[o], l = p[o + 1];
    if (c == 255) break;
    if (c == 0) { o++; continue; }
    if (o + 2 + l > len) break;
    if (c == code) { outLen = l; return p + o + 2; }
    o += 2 + l;
  }
  outLen = 0;
  return nullptr;
}

void recvOffers() {
  if (sock < 0) return;
  uint8_t p[600];
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  for (int guard = 0; guard < 8; guard++) {
    int n = recvfrom(sock, p, sizeof p, 0, (struct sockaddr*)&from, &fl);
    if (n < 240) break;
    if (p[0] != 2) continue;                              // not a BOOTREPLY
    if (p[236] != 0x63 || p[237] != 0x82) continue;       // no magic cookie
    int l;
    const uint8_t* mt = opt(p, n, 53, l);
    if (!mt || l < 1 || mt[0] != 2) continue;             // not an OFFER

    IPAddress id;
    const uint8_t* sid = opt(p, n, 54, l);
    if (sid && l == 4) id = IPAddress(sid[0], sid[1], sid[2], sid[3]);
    else id = IPAddress(from.sin_addr.s_addr);

    int i = 0;
    while (i < numSrv && srv[i].id != id) i++;
    if (i == numSrv && numSrv < MAX_SRV) { numSrv++; srv[i] = Srv(); }
    if (i >= MAX_SRV) continue;
    Srv& s = srv[i];
    s.id = id;
    s.offered = IPAddress(p[16], p[17], p[18], p[19]);    // yiaddr
    const uint8_t* rt = opt(p, n, 3, l);
    if (rt && l >= 4) s.gw = IPAddress(rt[0], rt[1], rt[2], rt[3]);
    memcpy(s.mac, p + 28, 6);
    s.haveMac = true;
    s.lastSeen = millis();
  }
}

void render() {
  spr.fillSprite(C_BG);
  bool rogue = numSrv > 1;
  uint16_t hc = rogue ? C_BAD : numSrv == 1 ? C_GOOD : C_ACC;
  header("DHCP FINDER", String(numSrv) + (numSrv == 1 ? " server" : " servers"), hc, C_DIM);

  if (!numSrv) {
    uint32_t waited = millis() - startedAt;
    centered(waited > 6000 ? "No DHCP server" : "Asking...", 70, 4, waited > 6000 ? C_WARN : C_DIM);
    if (waited > 6000)
      centered("no replies (static VLAN?)", 100, 2, C_DIM);
    show();
    return;
  }

  if (rogue) {
    spr.fillRect(0, 22, 320, 20, C_BAD);
    spr.setTextFont(2); spr.setTextDatum(MC_DATUM); spr.setTextColor(C_TEXT, C_BAD);
    spr.drawString("ROGUE DHCP: " + String(numSrv) + " servers!", 160, 32);
  }

  int top = rogue ? 46 : 26;
  int rowH = 40;
  for (int i = 0; i < numSrv && i < 3; i++) {
    Srv& s = srv[i];
    int y = top + i * rowH;
    spr.fillRoundRect(4, y, 312, rowH - 3, 3, C_HDR);
    spr.setTextFont(2); spr.setTextDatum(TL_DATUM);
    spr.setTextColor(rogue ? (i == 0 ? C_GOOD : C_WARN) : C_GOOD, C_HDR);
    spr.drawString("Server " + String(i + 1) + ":  " + s.id.toString(), 10, y + 3);
    spr.setTextFont(1); spr.setTextColor(C_DIM, C_HDR);
    char m[20];
    snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(s.haveMac ? m : "", 310, y + 4);
    spr.setTextDatum(TL_DATUM);
    spr.drawString("offered " + s.offered.toString() +
                   (s.gw != IPAddress(0,0,0,0) ? "   gw " + s.gw.toString() : ""), 10, y + 22);
  }
  show();
}

void start() {
  numSrv = 0; asks = 0;
  startedAt = millis();
  if (sock < 0) openSock();
  sendDiscover();
  lastAsk = millis();
  render();
}

void run() {
  recvOffers();
  if (millis() - lastAsk > ASK_GAP_MS) { lastAsk = millis(); sendDiscover(); }
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 500) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_A) { numSrv = 0; asks = 0; startedAt = millis(); sendDiscover(); lastAsk = millis(); }
  render();
}

}  // namespace dhcpfinder
