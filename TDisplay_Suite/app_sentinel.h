// LAN Sentinel
// Pings up to 6 hosts in rotation and shows live up/down + latency: an "is the event network
// healthy?" glance screen. The host list is set on the Wi-Fi setup page as
// "Label=target,Label=target"; a target is an IP, a hostname, or GW for the DHCP gateway.
// Keys: BTN_B hosts page / Wi-Fi details page, BTN_A re-check all hosts now.
#pragma once
#include <ESP32Ping.h>

namespace sentinel {

const char* DEFAULT_HOSTS = "Gateway=GW,DNS 1=1.1.1.1,DNS 2=8.8.8.8,Internet=google.com";
const int   MAX_HOSTS = 6;
const uint32_t PING_GAP_MS = 400;   // pause between individual host checks

struct Host {
  String    label;
  String    target;
  IPAddress ip;
  bool      up;
  bool      checked;
  float     ms;
  uint32_t  since;      // millis() of last state change
};

Host     hosts[MAX_HOSTS];
int      numHosts = 0;
int      page = 0;
int      nextHost = 0;
uint32_t lastPing = 0;
String   loadedFrom;

void loadHosts() {
  String list = cfg.hosts.length() ? cfg.hosts : String(DEFAULT_HOSTS);
  if (list == loadedFrom) return;
  loadedFrom = list;
  numHosts = 0;
  int pos = 0;
  while (pos < (int)list.length() && numHosts < MAX_HOSTS) {
    int comma = list.indexOf(',', pos);
    if (comma < 0) comma = list.length();
    String item = list.substring(pos, comma);
    pos = comma + 1;
    item.trim();
    if (!item.length()) continue;
    int eq = item.indexOf('=');
    Host& h = hosts[numHosts++];
    h = Host();
    h.label  = eq < 0 ? item : item.substring(0, eq);
    h.target = eq < 0 ? item : item.substring(eq + 1);
    h.label.trim();
    h.target.trim();
  }
  nextHost = 0;
}

bool resolve(Host& h) {
  if (h.target.equalsIgnoreCase("GW")) { h.ip = WiFi.gatewayIP(); return h.ip != IPAddress(0, 0, 0, 0); }
  if (h.ip.fromString(h.target)) return true;
  return WiFi.hostByName(h.target.c_str(), h.ip) == 1;
}

void checkHost(Host& h) {
  bool ok = false;
  float ms = 0;
  if (wifiUp() && resolve(h)) {
    ok = Ping.ping(h.ip, 2);
    if (ok) ms = Ping.averageTime();
  }
  if (!h.checked || ok != h.up) h.since = millis();
  h.up = ok;
  h.ms = ms;
  h.checked = true;
}

// ---------- Drawing ----------
void drawHeader(const char* title) {
  if (wifiUp()) {
    int rssi = WiFi.RSSI();
    uint16_t c = rssi > -60 ? C_GOOD : rssi > -72 ? C_WARN : C_BAD;
    header(title, WiFi.SSID() + " " + String(rssi) + "dBm", C_TEXT, c);
  } else {
    header(title, "Wi-Fi DOWN", C_TEXT, C_BAD);
  }
}

void drawHosts() {
  drawHeader("LAN SENTINEL");
  int upCount = 0;

  for (int i = 0; i < numHosts; i++) {
    Host& h = hosts[i];
    int y = 24 + i * 21;
    uint16_t dot = !h.checked ? C_DIM : (h.up ? C_GOOD : C_BAD);
    if (h.up) upCount++;

    if (i == nextHost) spr.drawRect(2, y, 316, 20, 0x2945);   // "checking" cursor
    spr.fillCircle(12, y + 10, 6, dot);

    spr.setTextFont(2);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(h.label, 24, y + 10);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(h.target, 100, y + 10);

    spr.setTextDatum(MR_DATUM);
    if (!h.checked) {
      spr.setTextColor(C_DIM, C_BG);
      spr.drawString("...", 314, y + 10);
    } else if (h.up) {
      uint16_t c = h.ms < 20 ? C_GOOD : h.ms < 100 ? C_WARN : C_SLOW;
      spr.setTextColor(c, C_BG);
      spr.drawString(String(h.ms, 0) + " ms", 314, y + 10);
    } else {
      spr.setTextColor(C_BAD, C_BG);
      spr.drawString("DOWN " + fmtDur((millis() - h.since) / 1000), 314, y + 10);
    }
  }

  spr.drawFastHLine(0, 152, 320, C_HDR);
  spr.setTextFont(2);
  spr.setTextDatum(BL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(WiFi.localIP().toString(), 6, 169);

  spr.setTextDatum(BC_DATUM);
  spr.setTextColor(upCount == numHosts ? C_GOOD : C_BAD, C_BG);
  spr.drawString(String(upCount) + "/" + String(numHosts) + " up", 160, 169);

  spr.setTextDatum(BR_DATUM);
  float v = batteryVolts();
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(v > 4.3f ? "USB" : String(v, 2) + "V", 314, 169);
}

void drawWifi() {
  drawHeader("WI-FI DETAILS");
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);

  struct Row { const char* k; String v; };
  Row rows[] = {
    {"SSID",    WiFi.SSID()},
    {"BSSID",   WiFi.BSSIDstr()},
    {"Channel", String(WiFi.channel())},
    {"RSSI",    String(WiFi.RSSI()) + " dBm"},
    {"IP",      WiFi.localIP().toString()},
    {"Mask",    WiFi.subnetMask().toString()},
    {"Gateway", WiFi.gatewayIP().toString()},
    {"DNS",     WiFi.dnsIP(0).toString()},
    {"MAC",     WiFi.macAddress()},
  };
  for (int i = 0; i < 9; i++) {
    int y = 25 + i * 16;
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(rows[i].k, 8, y);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(rows[i].v, 80, y);
  }

  int rssi = constrain(WiFi.RSSI(), -95, -30);
  int h = map(rssi, -95, -30, 0, 120);
  uint16_t c = rssi > -60 ? C_GOOD : rssi > -72 ? C_WARN : C_BAD;
  spr.drawRect(290, 28, 20, 122, C_DIM);
  spr.fillRect(291, 149 - h, 18, h, c);

  spr.setTextDatum(BR_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("up " + fmtDur(millis() / 1000), 280, 169);
}

void render() {
  spr.fillSprite(C_BG);
  if (page == 0) drawHosts(); else drawWifi();
  spr.pushSprite(0, 0);
}

// ---------- App ----------
void start() {
  page = 0;
  loadHosts();
  render();
}

void run() {
  if (numHosts && millis() - lastPing > PING_GAP_MS) {
    checkHost(hosts[nextHost]);           // blocks up to ~2 s on a dead host
    nextHost = (nextHost + 1) % numHosts;
    lastPing = millis();
    render();
  }

  static uint32_t lastDraw = 0;           // keep "DOWN 3m" timers ticking
  if (millis() - lastDraw > 1000) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) {
    page ^= 1;
  } else {
    for (int i = 0; i < numHosts; i++) hosts[i].checked = false;
    nextHost = 0;
  }
  render();
}

}  // namespace sentinel
