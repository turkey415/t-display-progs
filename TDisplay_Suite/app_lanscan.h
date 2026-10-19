// LAN Scanner
// Sweeps the local subnet and lists every device that answers: IP, MAC and the maker (from the
// MAC's first three bytes). New devices since the scan started are marked. Good for "what is on
// this network?" and for spotting a device that shouldn't be there.
//
// It works by ARP: it asks "who has this IP?" for each address in turn and reads the replies out
// of the ARP cache. ARP only reaches the local subnet, so it scans /24 (254 addresses) around our
// own IP. It sends nothing but ARP requests.
//
// Portrait (USB at the bottom) for a long list.
// Keys: BTN_B next page, BTN_A scan again.
#pragma once
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

namespace lanscan {

const int ROW_H = 19, TOP = 22, ROWS = 14;
const int MAX_DEV = 120;

struct Dev { uint32_t ip; uint8_t mac[6]; bool isNew; };
Dev devs[MAX_DEV];
int numDevs = 0;

uint32_t base = 0, mask = 0;     // network base and mask (host-order)
int  probe = 1;                  // next host octet to ask about (1..254)
bool sweeping = false, everSwept = false;
int  sweeps = 0, pageNo = 0;
uint32_t lastProbe = 0, lastHarvest = 0;

// A short OUI table for makers common on a home / LAN-party network.
struct Oui { uint8_t b[3]; const char* name; };
const Oui OUIS[] = {
  {{0x00,0x1A,0x11}, "Google"}, {{0xF4,0xF5,0xE8}, "Google"}, {{0xDA,0xA1,0x19}, "Google"},
  {{0x3C,0x5A,0xB4}, "Google"}, {{0x00,0x05,0x1B}, "Apple"},  {{0xAC,0xBC,0x32}, "Apple"},
  {{0xF0,0x18,0x98}, "Apple"},  {{0xDC,0xA6,0x32}, "RaspPi"}, {{0xB8,0x27,0xEB}, "RaspPi"},
  {{0xE4,0x5F,0x01}, "RaspPi"}, {{0x00,0x50,0x56}, "VMware"}, {{0x52,0x54,0x00}, "QEMU/KVM"},
  {{0xD8,0x3A,0xDD}, "RaspPi"}, {{0x00,0x1D,0xD8}, "MS/Xbox"},{{0x7C,0xED,0x8D}, "MS/Xbox"},
  {{0x00,0x15,0x5D}, "Hyper-V"},{{0x00,0xD9,0xD1}, "Sony/PS"},{{0x00,0x04,0x1F}, "Sony/PS"},
  {{0x2C,0xCC,0x44}, "Sony/PS"},{{0x18,0x74,0x2E}, "Valve"},  {{0x90,0x2B,0x34}, "Giga-byte"},
  {{0x1C,0x1B,0x0D}, "Nvidia"}, {{0x48,0x2A,0xE3}, "TP-Link"},{{0xC0,0x25,0xE9}, "TP-Link"},
  {{0x50,0xC7,0xBF}, "TP-Link"},{{0xB0,0x4E,0x26}, "TP-Link"},{{0x00,0x0C,0x29}, "VMware"},
  {{0xEC,0x08,0x6B}, "TP-Link"},{{0x00,0x1B,0x21}, "Intel"},  {{0x00,0x1E,0x67}, "Intel"},
  {{0xA0,0xCE,0xC8}, "Cisco"},  {{0x00,0x0A,0x8A}, "Cisco"},  {{0x00,0x1A,0xA1}, "Cisco"},
  {{0x3C,0xCE,0x73}, "Cisco"},  {{0xB8,0x38,0x61}, "Cisco"},  {{0x00,0x26,0x99}, "Cisco"},
};
const int NUM_OUI = sizeof(OUIS) / sizeof(OUIS[0]);

const char* makerOf(const uint8_t* m) {
  for (int i = 0; i < NUM_OUI; i++)
    if (OUIS[i].b[0] == m[0] && OUIS[i].b[1] == m[1] && OUIS[i].b[2] == m[2]) return OUIS[i].name;
  if (m[0] & 0x02) return "(random)";    // locally-administered / privacy MAC
  return nullptr;
}

struct netif* myNetif() {
  return netif_default;                  // the STA interface while connected
}

void setupRange() {
  IPAddress ip = WiFi.localIP(), nm = WiFi.subnetMask();
  uint32_t ipn = ((uint32_t)ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3];
  uint32_t m   = ((uint32_t)nm[0] << 24) | (nm[1] << 16) | (nm[2] << 8) | nm[3];
  if (m == 0) m = 0xFFFFFF00;            // assume /24 if the mask looks wrong
  mask = m;
  base = ipn & m;
}

bool known(uint32_t ip) {
  for (int i = 0; i < numDevs; i++) if (devs[i].ip == ip) return true;
  return false;
}

// Read the whole ARP cache and add anything new.
void harvest() {
  struct netif* nif = myNetif();
  if (!nif) return;
  for (size_t i = 0; i < 64; i++) {       // more slots than the table holds; extra are empty
    ip4_addr_t* ipa = nullptr;
    struct netif* n = nullptr;
    struct eth_addr* eth = nullptr;
    LOCK_TCPIP_CORE();
    int ok = etharp_get_entry(i, &ipa, &n, &eth);
    UNLOCK_TCPIP_CORE();
    if (!ok) continue;
    if (!ipa || !eth) continue;
    uint32_t host = lwip_ntohl(ipa->addr);
    if ((host & mask) != base) continue;  // only our subnet
    if (known(host)) continue;
    if (numDevs >= MAX_DEV) break;
    Dev& d = devs[numDevs++];
    d.ip = host;
    memcpy(d.mac, eth->addr, 6);
    d.isNew = everSwept;                  // flagged only if it showed up after the first full sweep
  }
}

void sendProbe() {
  struct netif* nif = myNetif();
  if (!nif) return;
  // a few requests per tick so a sweep finishes in a few seconds
  for (int k = 0; k < 6 && probe <= 254; k++, probe++) {
    uint32_t host = base | probe;
    IPAddress me = WiFi.localIP();
    uint32_t myn = ((uint32_t)me[0] << 24) | (me[1] << 16) | (me[2] << 8) | me[3];
    if (host == myn) continue;            // skip ourselves
    ip4_addr_t target;
    target.addr = lwip_htonl(host);
    LOCK_TCPIP_CORE();
    etharp_request(nif, &target);
    UNLOCK_TCPIP_CORE();
  }
  if (probe > 254) {                      // sweep done
    probe = 1;
    sweeping = false;
    everSwept = true;
    sweeps++;
  }
}

void render() {
  spr.fillSprite(C_BG);
  header("LAN SCAN", String(numDevs), C_ACC, C_TEXT);

  if (!numDevs && !everSwept) {
    centered("Scanning...", 150, 2);
    centered(WiFi.localIP().toString() + "/24", 172, 1, C_DIM);
  } else if (!numDevs) {
    centered("No devices found", 150, 2, C_WARN);
  } else {
    int pages = (numDevs + ROWS - 1) / ROWS;
    if (pageNo >= pages) pageNo = 0;
    for (int r = 0; r < ROWS; r++) {
      int i = pageNo * ROWS + r;
      if (i >= numDevs) break;
      const Dev& d = devs[i];
      int y = TOP + r * ROW_H;
      if (d.isNew) spr.fillRect(0, y, 170, ROW_H - 1, 0x0220);
      spr.setTextFont(2); spr.setTextDatum(ML_DATUM);
      spr.setTextColor(C_TEXT, d.isNew ? 0x0220 : C_BG);
      IPAddress ip(d.ip >> 24, (d.ip >> 16) & 0xFF, (d.ip >> 8) & 0xFF, d.ip & 0xFF);
      spr.drawString(String(ip[2]) + "." + String(ip[3]), 3, y + 9);   // last two octets, short
      const char* mk = makerOf(d.mac);
      spr.setTextFont(1); spr.setTextDatum(MR_DATUM); spr.setTextColor(mk ? C_ACC : C_DIM, d.isNew ? 0x0220 : C_BG);
      char mac[18];
      snprintf(mac, sizeof mac, "%02X:%02X:%02X", d.mac[3], d.mac[4], d.mac[5]);
      spr.drawString(mk ? String(mk) : String(mac), 168, y + 9);
    }
  }

  spr.drawFastHLine(0, 305, 170, C_HDR);
  spr.setTextFont(1); spr.setTextColor(C_DIM, C_BG); spr.setTextDatum(BL_DATUM);
  spr.drawString(sweeping ? "scan " + String(probe * 100 / 254) + "%" : "sweep " + String(sweeps), 3, 318);
  if (numDevs) {
    int pages = (numDevs + ROWS - 1) / ROWS;
    spr.setTextDatum(BR_DATUM);
    spr.drawString(String(min(pageNo, pages - 1) + 1) + "/" + String(pages), 167, 318);
  }
  show();
}

void begin() {
  numDevs = 0; probe = 1; sweeps = 0; everSwept = false; pageNo = 0;
  setupRange();
  sweeping = true;
}

void start() {
  setPortrait(true);
  begin();
  render();
}

void run() {
  if (!wifiUp()) return;
  if (sweeping && millis() - lastProbe > 120) { lastProbe = millis(); sendProbe(); }
  if (millis() - lastHarvest > 150) { lastHarvest = millis(); harvest(); }
  // keep sweeping forever so new devices keep turning up
  if (!sweeping && millis() - lastProbe > 3000) { sweeping = true; }
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 400) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) pageNo++;
  else begin();
  render();
}

}  // namespace lanscan
