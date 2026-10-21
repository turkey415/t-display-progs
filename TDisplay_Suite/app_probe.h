// Probe Viewer
// Shows what a network probe (Probe Deck on a Radxa ZERO 3E, github.com/turkey415/probe-deck)
// found on the switch port it is plugged into: the switch, port and VLAN from CDP/LLDP, link,
// DHCP, gateway ping, DNS, internet, and its last speed test.
//
// Two ways in, and the Paths page says which ones work (that is a test of the network too):
//  - UDP: probes broadcast the probe message on port 47474 every ~9 s. Wi-Fi controllers often
//    don't pass broadcasts to wireless clients, so this may never arrive over Wi-Fi.
//  - HTTP: probes announce _probe-deck._tcp over mDNS; found ones are polled at /api/probe.
// Several probes can be out at once. A probe whose port or link changes (it was just plugged
// into another jack) is jumped to, so walking one around needs no key presses.
//
// Keys: BTN_B next page (Port, Tests, Paths), BTN_A next probe.
#pragma once
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mdns.h"
#include "lwip/sockets.h"

namespace probeview {

const int      MAX_PROBES = 6;
const uint16_t PROBE_PORT = 47474;
const uint32_t STALE_MS   = 25000;     // grey after this long without news
const uint32_t POLL_MS    = 3000;      // HTTP poll per probe
const uint32_t FIND_MS    = 30000;     // mDNS query this often (sooner while none found)
const int      PAGES      = 3;

struct Probe {
  char id[13], name[33];
  char sw[48], port[40], vlan[8], plat[28], mgmt[16], via[10];
  char ip[16];            // the probe's own address (from the message)
  char httpIp[16];        // where to poll it over HTTP ("" = not found by mDNS)
  bool hasCdp, linkUp;
  int  speed, flaps;
  char duplex[6];
  float ipt;              // seconds from cable-in to an address, -1 unknown
  int8_t gwOk, dns, inet; // 1 ok, 0 failed, -1 unknown
  float gwMs, gwLoss, temp;
  double down, up;        // last speed test, bps (0 = none)
  uint32_t upSecs, seq;
  uint32_t heardUdp, heardHttp, seen, changedAt;   // millis(), 0 = never
};
Probe probes[MAX_PROBES];
int numProbes = 0, cur = 0, page = 0;

int sock = -1;
bool mdnsReady = false;
mdns_search_once_t* search = nullptr;
uint32_t lastFind = 0, lastPoll = 0;
int pollIdx = 0;
bool started = false;

// ---------- receiving ----------
void openSock() {
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return;
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(PROBE_PORT);
  a.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock, (struct sockaddr*)&a, sizeof a) < 0) { close(sock); sock = -1; return; }
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
}

int8_t tri(JsonVariant v) { return v.isNull() ? -1 : (v.as<bool>() ? 1 : 0); }

// Shorten Cisco interface names so they fit big: GigabitEthernet0/7 -> Gi0/7.
void shortPort(const char* in, char* out, size_t n) {
  static const char* LONG[]  = {"TwentyFiveGigE", "TenGigabitEthernet", "GigabitEthernet", "FastEthernet", "Ethernet", "Port-channel"};
  static const char* SHORT[] = {"Twe", "Te", "Gi", "Fa", "Eth", "Po"};
  for (int i = 0; i < 6; i++) {
    size_t l = strlen(LONG[i]);
    if (!strncmp(in, LONG[i], l)) { snprintf(out, n, "%s%s", SHORT[i], in + l); return; }
  }
  strlcpy(out, in, n);
}

// Parse one probe message (the same JSON over UDP and HTTP). viaHttp says which path brought it.
void take(const char* body, size_t len, bool viaHttp, const char* httpIp) {
  JsonDocument doc;
  if (deserializeJson(doc, body, len)) return;
  if (strcmp(doc["app"] | "", "probe") != 0) return;
  const char* id = doc["id"] | "";
  if (!*id) return;

  int i = 0;
  while (i < numProbes && strcmp(probes[i].id, id)) i++;
  bool fresh = i == numProbes;
  if (fresh) {
    if (numProbes >= MAX_PROBES) return;
    numProbes++;
    probes[i] = Probe();
    strlcpy(probes[i].id, id, sizeof probes[i].id);
  }
  Probe& p = probes[i];
  char oldPort[40];
  strlcpy(oldPort, p.port, sizeof oldPort);
  bool oldLink = p.linkUp;

  strlcpy(p.name, doc["name"] | id, sizeof p.name);
  strlcpy(p.ip, doc["ip"] | "", sizeof p.ip);
  if (httpIp) strlcpy(p.httpIp, httpIp, sizeof p.httpIp);
  JsonObject link = doc["link"];
  p.linkUp = link["up"] | false;
  p.speed  = link["speed"] | 0;
  p.flaps  = link["flaps"] | 0;
  strlcpy(p.duplex, link["duplex"] | "", sizeof p.duplex);
  JsonObject cdp = doc["cdp"];
  p.hasCdp = !cdp.isNull();
  if (p.hasCdp) {
    shortPort(cdp["port"] | "?", p.port, sizeof p.port);
    strlcpy(p.sw, cdp["sw"] | "", sizeof p.sw);
    char* dot = strchr(p.sw, '.');           // drop the domain: c3560-16.lab.example -> c3560-16
    if (dot) *dot = 0;
    strlcpy(p.vlan, cdp["vlan"] | "", sizeof p.vlan);
    strlcpy(p.plat, cdp["plat"] | "", sizeof p.plat);
    strlcpy(p.mgmt, cdp["mgmt"] | "", sizeof p.mgmt);
    strlcpy(p.via, cdp["via"] | "", sizeof p.via);
  } else {
    p.port[0] = p.sw[0] = p.vlan[0] = p.plat[0] = p.mgmt[0] = p.via[0] = 0;
  }
  p.ipt = doc["ipt"].isNull() ? -1 : doc["ipt"].as<float>();
  JsonObject gw = doc["gw"];
  p.gwOk   = tri(gw["ok"]);
  p.gwMs   = gw["ms"].isNull() ? -1 : gw["ms"].as<float>();
  p.gwLoss = gw["loss"].isNull() ? -1 : gw["loss"].as<float>();
  p.dns    = tri(doc["dns"]);
  p.inet   = tri(doc["inet"]);
  p.temp   = doc["temp"].isNull() ? -1 : doc["temp"].as<float>();
  p.down   = doc["speed"]["down"] | 0.0;
  p.up     = doc["speed"]["up"] | 0.0;
  p.upSecs = doc["up"] | 0;
  p.seq    = doc["seq"] | 0;

  uint32_t now = millis();
  (viaHttp ? p.heardHttp : p.heardUdp) = now;
  p.seen = now;
  // Follow mode: a probe that just moved (new port, or the link came back) takes the screen.
  if (!fresh && (strcmp(oldPort, p.port) || oldLink != p.linkUp)) {
    p.changedAt = now;
    cur = i;
    page = 0;
  }
  if (fresh && numProbes == 1) cur = 0;
}

void recvUdp() {
  if (sock < 0) return;
  static char buf[1500];
  for (int guard = 0; guard < 4; guard++) {
    int n = recv(sock, buf, sizeof buf - 1, 0);
    if (n <= 0) break;
    take(buf, n, false, nullptr);
  }
}

// mDNS: find probes announcing _probe-deck._tcp and remember where to poll them.
void findProbes() {
  if (!mdnsReady) {
    esp_err_t e = mdns_init();
    mdnsReady = e == ESP_OK || e == ESP_ERR_INVALID_STATE;   // another app may have started it
    if (!mdnsReady) return;
  }
  if (search) {
    mdns_result_t* res = nullptr;
    uint8_t n = 0;
    if (!mdns_query_async_get_results(search, 0, &res, &n)) return;   // still asking
    for (mdns_result_t* r = res; r; r = r->next) {
      if (!r->addr || r->addr->addr.type != ESP_IPADDR_TYPE_V4) continue;
      esp_ip4_addr_t a = r->addr->addr.u_addr.ip4;
      char ip[16];
      snprintf(ip, sizeof ip, "%u.%u.%u.%u", esp_ip4_addr1_16(&a), esp_ip4_addr2_16(&a), esp_ip4_addr3_16(&a), esp_ip4_addr4_16(&a));
      bool known = false;
      for (int i = 0; i < numProbes; i++) if (!strcmp(probes[i].httpIp, ip) || !strcmp(probes[i].ip, ip)) {
        strlcpy(probes[i].httpIp, ip, sizeof probes[i].httpIp);
        known = true;
      }
      if (!known && numProbes < MAX_PROBES) {
        // Not heard yet: a placeholder until its first poll answers.
        Probe& p = probes[numProbes++];
        p = Probe();
        snprintf(p.id, sizeof p.id, "mdns%u", (unsigned)numProbes);
        strlcpy(p.name, r->hostname ? r->hostname : ip, sizeof p.name);
        strlcpy(p.httpIp, ip, sizeof p.httpIp);
        strlcpy(p.ip, ip, sizeof p.ip);
      }
    }
    mdns_query_results_free(res);
    mdns_query_async_delete(search);
    search = nullptr;
    return;
  }
  uint32_t every = numProbes ? FIND_MS : 6000;
  if (wifiUp() && (!lastFind || millis() - lastFind > every)) {
    lastFind = millis();
    search = mdns_query_async_new(NULL, "_probe-deck", "_tcp", MDNS_TYPE_PTR, 2000, 8, NULL);
  }
}

// A placeholder made from mDNS is replaced by the real probe once its message arrives.
void dropPlaceholders() {
  for (int i = 0; i < numProbes; i++) {
    if (strncmp(probes[i].id, "mdns", 4) || !probes[i].httpIp[0]) continue;
    for (int j = 0; j < numProbes; j++) {
      if (j != i && strncmp(probes[j].id, "mdns", 4) && !strcmp(probes[j].httpIp, probes[i].httpIp)) {
        for (int k = i; k < numProbes - 1; k++) probes[k] = probes[k + 1];
        numProbes--;
        if (cur >= numProbes) cur = max(0, numProbes - 1);
        else if (cur > i) cur--;
        i--;
        break;
      }
    }
  }
}

void pollHttp() {
  if (!numProbes || !wifiUp() || millis() - lastPoll < POLL_MS / max(1, numProbes)) return;
  lastPoll = millis();
  pollIdx = (pollIdx + 1) % numProbes;
  Probe& p = probes[pollIdx];
  if (!p.httpIp[0]) return;
  char ip[16];
  strlcpy(ip, p.httpIp, sizeof ip);
  WiFiClient c;
  HTTPClient http;
  http.setConnectTimeout(800);
  http.setTimeout(1200);
  if (!http.begin(c, String("http://") + ip + "/api/probe")) return;
  if (http.GET() == 200) {
    String body = http.getString();
    take(body.c_str(), body.length(), true, ip);
    dropPlaceholders();
  }
  http.end();
}

// ---------- drawing ----------
const uint16_t C_AMBER = 0xFD80;

String ago(uint32_t t) {
  if (!t) return "never";
  uint32_t s = (millis() - t) / 1000;
  return s < 90 ? String(s) + " s ago" : String(s / 60) + " min ago";
}

// Overall state of a probe: stale (grey), something failing (red), all good (green).
uint16_t state(const Probe& p) {
  if (!p.seen || millis() - p.seen > STALE_MS) return C_DIM;
  if (!p.linkUp || p.gwOk == 0 || p.dns == 0 || p.inet == 0) return C_BAD;
  if (p.gwLoss > 0) return C_WARN;
  return C_GOOD;
}

void dot(int x, int y, int8_t ok) {
  spr.fillCircle(x, y, 4, ok < 0 ? C_DIM : ok ? C_GOOD : C_BAD);
}

String mbps(double bps) {
  if (bps <= 0) return "-";
  return bps >= 1e9 ? String(bps / 1e9, 2) + " G" : String((int)(bps / 1e6 + 0.5));
}

void row(int y, const char* label, int8_t ok, const String& value, uint16_t col = C_TEXT) {
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(label, 22, y);
  dot(10, y + 8, ok);
  spr.setTextColor(col, C_BG);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(fit(value, 220, 2), 314, y);
}

void pagePort(const Probe& p) {
  bool live = p.seen && millis() - p.seen < STALE_MS;
  uint16_t big = live ? C_AMBER : C_DIM;
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(1); spr.setTextColor(C_DIM, C_BG);
  spr.drawString("SWITCH PORT", 8, 25);
  spr.setTextDatum(TR_DATUM);
  spr.drawString("VLAN", 312, 25);

  if (!p.seen) {
    centered("Waiting for the probe...", 70, 2, C_DIM);
    return;
  }
  spr.setTextDatum(TL_DATUM);
  if (!p.linkUp) {
    spr.setTextFont(4); spr.setTextColor(C_BAD, C_BG);
    spr.drawString("NO LINK", 8, 36);
  } else if (!p.hasCdp) {
    spr.setTextFont(4); spr.setTextColor(C_DIM, C_BG);
    spr.drawString("--", 8, 36);
    spr.setTextFont(1);
    spr.drawString("waiting for CDP / LLDP (up to 60 s)", 8, 66);
  } else {
    spr.setTextFont(4); spr.setTextColor(big, C_BG);
    spr.setTextSize(fit(p.port, 100, 4) == p.port ? 2 : 1);   // big when it fits
    spr.drawString(p.port, 8, 35);
    spr.setTextSize(1);
  }
  // VLAN, right
  spr.setTextDatum(TR_DATUM);
  spr.setTextFont(4); spr.setTextColor(p.vlan[0] && live ? C_TEXT : C_DIM, C_BG);
  spr.setTextSize(p.vlan[0] && strlen(p.vlan) <= 3 ? 2 : 1);
  spr.drawString(p.vlan[0] ? p.vlan : "-", 312, 35);
  spr.setTextSize(1);

  int y = 92;
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(2); spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(fit(p.hasCdp ? String(p.sw) : String("no CDP / LLDP neighbor"), 304, 2), 8, y);
  spr.setTextFont(1); spr.setTextColor(C_DIM, C_BG);
  String sub = String(p.plat);
  if (p.mgmt[0]) sub += String(sub.length() ? "  " : "") + p.mgmt;
  if (p.via[0]) sub += String("  ") + p.via;
  spr.drawString(fit(sub, 304, 1), 8, y + 18);

  // link line
  y = 124;
  spr.drawFastHLine(8, y - 4, 304, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(p.linkUp ? C_GOOD : C_BAD, C_BG);
  String l = p.linkUp ? String(p.speed) + " " + p.duplex : String("link down");
  spr.drawString(l, 8, y);
  spr.setTextColor(p.flaps ? C_WARN : C_DIM, C_BG);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(String(p.flaps) + (p.flaps == 1 ? " drop" : " drops"), 312, y);
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(p.ipt >= 0 ? "IP in " + String(p.ipt, 1) + " s" : String(p.ip), 170, y);
}

void pageTests(const Probe& p) {
  int y = 24, h = 20;
  row(y, "Link", p.linkUp, p.linkUp ? String(p.speed) + " Mbps " + p.duplex : String("down")); y += h;
  row(y, "Address", p.ip[0] ? 1 : 0, p.ip[0] ? String(p.ip) + (p.ipt >= 0 ? "  (" + String(p.ipt, 1) + " s)" : "") : String("none")); y += h;
  String g = p.gwMs >= 0 ? String(p.gwMs, p.gwMs < 10 ? 2 : 1) + " ms" : String("-");
  if (p.gwLoss > 0) g += "  " + String(p.gwLoss, 0) + "% loss";
  row(y, "Gateway", p.gwOk, g, p.gwLoss > 0 ? C_WARN : C_TEXT); y += h;
  row(y, "DNS", p.dns, p.dns < 0 ? "-" : p.dns ? "OK" : "FAILED", p.dns == 0 ? C_BAD : C_TEXT); y += h;
  row(y, "Internet", p.inet, p.inet < 0 ? "-" : p.inet ? "OK" : "FAILED", p.inet == 0 ? C_BAD : C_TEXT); y += h;
  row(y, "Speed", p.down > 0 || p.up > 0 ? 1 : -1, p.down > 0 || p.up > 0 ? "down " + mbps(p.down) + "  up " + mbps(p.up) + " Mbps" : String("not tested")); y += h;
  row(y, "Probe", -1, (p.temp >= 0 ? String(p.temp, 0) + " C  " : "") + "up " + String(p.upSecs / 60) + " min", C_DIM);
}

void pagePaths(const Probe& p) {
  int y = 24, h = 20;
  bool udp = p.heardUdp && millis() - p.heardUdp < STALE_MS, web = p.heardHttp && millis() - p.heardHttp < STALE_MS;
  row(y, "Broadcast", p.heardUdp ? udp : 0, ago(p.heardUdp), udp ? C_TEXT : C_DIM); y += h;
  row(y, "HTTP", p.heardHttp ? web : (p.httpIp[0] ? 0 : -1), p.httpIp[0] ? ago(p.heardHttp) : String("not found by mDNS"), web ? C_TEXT : C_DIM); y += h;
  row(y, "Radio", -1, "ESP-NOW: not yet", C_DIM); y += h;
  row(y, "Web page", -1, p.ip[0] ? String("http://") + p.ip + "/" : String("-"), C_ACC); y += h;
  spr.setTextFont(1); spr.setTextDatum(TL_DATUM); spr.setTextColor(C_DIM, C_BG);
  y += 6;
  if (p.heardUdp == 0 && p.heardHttp)
    spr.drawString("No broadcast over Wi-Fi: the AP or controller", 8, y),
    spr.drawString("is not passing broadcasts. HTTP still works.", 8, y + 11);
  else
    spr.drawString("id " + String(p.id) + "   message #" + String(p.seq), 8, y);
}

void render() {
  spr.fillSprite(C_BG);
  static const char* NAMES[] = {"PORT", "TESTS", "PATHS"};
  if (!numProbes) {
    header("PROBE VIEWER", wifiUp() ? "" : "no Wi-Fi", C_ACC, C_BAD);
    centered("Looking for probes...", 62, 4, C_DIM);
    centered("broadcast on UDP 47474", 98, 2, C_DIM);
    centered("or mDNS _probe-deck._tcp", 116, 2, C_DIM);
    spr.setTextFont(1); spr.setTextDatum(BC_DATUM); spr.setTextColor(C_DIM, C_BG);
    spr.drawString(wifiUp() ? "this display: " + WiFi.localIP().toString() : String(""), 160, 166);
    show();
    return;
  }
  Probe& p = probes[cur];
  String right = String(p.name);
  if (numProbes > 1) right += "  " + String(cur + 1) + "/" + String(numProbes);
  header(String("PROBE  ") + NAMES[page], right, C_ACC, state(p));
  if (page == 0) pagePort(p);
  else if (page == 1) pageTests(p);
  else pagePaths(p);

  // footer: page dots + freshness
  for (int i = 0; i < PAGES; i++) spr.fillCircle(150 + i * 10, 164, 2, i == page ? C_TEXT : C_HDR);
  spr.setTextFont(1); spr.setTextDatum(BR_DATUM);
  spr.setTextColor(p.seen && millis() - p.seen < STALE_MS ? C_DIM : C_WARN, C_BG);
  spr.drawString(p.seen ? ago(p.seen) : "", 314, 168);
  if (p.changedAt && millis() - p.changedAt < 4000) {   // just moved: say so
    spr.setTextDatum(BL_DATUM); spr.setTextColor(C_AMBER, C_BG);
    spr.drawString("NEW PORT", 6, 168);
  }
  show();
}

void start() {
  if (sock < 0) openSock();
  started = true;
  lastFind = 0;
  render();
}

void run() {
  recvUdp();
  findProbes();
  pollHttp();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 500) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) page = (page + 1) % PAGES;
  else if (pin == BTN_A && numProbes) { cur = (cur + 1) % numProbes; page = 0; }
  render();
}

}  // namespace probeview
