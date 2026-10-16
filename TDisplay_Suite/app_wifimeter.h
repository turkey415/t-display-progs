// Wi-Fi Meter
// Everything about the Wi-Fi the device is on, mainly how strong it is.
//   1 METER    : needle gauge, dBm, quality, min / max / average, the last 2 2/3 minutes
//   2 LINK     : network, access point, channel and width, 802.11 modes, security, country
//   3 NETWORK  : IP, mask, gateway, DNS, MAC, hostname, public IP and ISP
//   4 PING     : latency and loss to the gateway, the DNS server and the internet
//   5 CHANNELS : how crowded each channel is, and the least crowded of 1 / 6 / 11
// Keys: BTN_B next page, BTN_A reset (meter, ping) or refresh (the others).
#pragma once
#include <esp_wifi.h>
#include <ESP32Ping.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace wifimeter {

const int NUM_PAGES = 5;
const char* TITLES[NUM_PAGES] = {"SIGNAL", "LINK", "NETWORK", "PING", "CHANNELS"};
const int HIST = 160;                 // one reading a second, 2 px each

int page = 0;

// ---------- Signal ----------
int8_t   hist[HIST];
int      histN = 0;
int      minR = 0, maxR = -127;
long     sumR = 0, cntR = 0;
float    needle = -100;               // eased towards the reading, so it swings rather than jumps
uint32_t lastSample = 0;
uint32_t connectedAt = 0;
uint32_t seenEpoch = 0;

void resetSignal() { histN = 0; minR = 0; maxR = -127; sumR = 0; cntR = 0; }

void sample() {
  if (!wifiUp()) return;
  int r = WiFi.RSSI();
  if (r >= 0) return;                 // 0 means no reading
  if (histN < HIST) hist[histN++] = r;
  else { memmove(hist, hist + 1, HIST - 1); hist[HIST - 1] = r; }
  minR = cntR ? min(minR, r) : r;
  maxR = max(maxR, r);
  sumR += r; cntR++;
}

const char* quality(int r) {
  return r >= -55 ? "Excellent" : r >= -67 ? "Good" : r >= -75 ? "Fair" : r >= -85 ? "Weak" : "Unusable";
}

int pct(int r) { return constrain(2 * (r + 100), 0, 100); }

// ---------- Ping ----------
struct Target { const char* label; IPAddress ip; float last, avg; int sent, lost; float hist[40]; int n; };
Target targets[3] = {{"Gateway"}, {"DNS server"}, {"Internet 1.1.1.1"}};
int    nextTarget = 0;
uint32_t lastPing = 0;

void resetPing() {
  for (auto& t : targets) { t.last = -1; t.avg = 0; t.sent = t.lost = 0; t.n = 0; }
}

void pingOne() {
  targets[0].ip = WiFi.gatewayIP();
  targets[1].ip = WiFi.dnsIP(0);
  targets[2].ip = IPAddress(1, 1, 1, 1);
  Target& t = targets[nextTarget];
  nextTarget = (nextTarget + 1) % 3;
  if (!wifiUp() || t.ip == IPAddress(0, 0, 0, 0)) return;
  bool ok = Ping.ping(t.ip, 1);
  float ms = ok ? Ping.averageTime() : -1;
  t.sent++;
  if (!ok) t.lost++;
  else t.avg = t.avg ? t.avg * 0.8f + ms * 0.2f : ms;
  t.last = ms;
  if (t.n < 40) t.hist[t.n++] = ms;
  else { memmove(t.hist, t.hist + 1, 39 * sizeof(float)); t.hist[39] = ms; }
}

// ---------- Public IP ----------
String publicIp, isp;
bool   triedPublic = false;

void fetchPublic() {
  triedPublic = true;
  if (!wifiUp()) return;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(client, "http://ip-api.com/json/?fields=status,query,isp")) return;
  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && strcmp(doc["status"] | "", "success") == 0) {
      publicIp = doc["query"] | "";
      isp = doc["isp"] | "";
    }
  }
  http.end();
}

// ---------- Channels ----------
int      onCh[14], nearCh[14], sameNet = 0, bestOther = -127, numSeen = 0;
bool     chScanning = false, chEver = false;
uint32_t chAt = 0;

void channelScan(bool now = false) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (chScanning && n >= 0) {
    memset(onCh, 0, sizeof onCh);
    memset(nearCh, 0, sizeof nearCh);
    sameNet = 0; bestOther = -127; numSeen = n;
    String mine = WiFi.SSID();
    uint8_t* myAp = WiFi.BSSID();
    for (int i = 0; i < n; i++) {
      int c = WiFi.channel(i);
      if (c < 1 || c > 13) continue;
      onCh[c]++;
      for (int k = max(1, c - 4); k <= min(13, c + 4); k++) if (k != c) nearCh[k]++;   // 20 MHz overlaps 4 either side
      if (WiFi.SSID(i) == mine) {
        sameNet++;
        if (memcmp(WiFi.BSSID(i), myAp, 6) != 0) bestOther = max(bestOther, (int)WiFi.RSSI(i));
      }
    }
    WiFi.scanDelete();
    chScanning = false;
    chEver = true;
    chAt = millis();
    Serial.printf("wifimeter: %d networks, %d on ch %d\n", n, onCh[WiFi.channel()], WiFi.channel());
  }
  if (!chScanning && (now || !chEver || millis() - chAt > 20000)) {
    WiFi.scanNetworks(true, true);
    chScanning = true;
  }
}

// ---------- Drawing ----------
void row(int i, const char* k, const String& v, uint16_t c = C_TEXT) {
  int y = 24 + i * 16;
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(k, 8, y);
  spr.setTextColor(c, C_BG);
  spr.drawString(fit(v, 220, 2), 92, y);
}

void drawMeter() {
  int r = WiFi.RSSI();
  const int cx = 88, cy = 118, ro = 72, ri = 58;
  auto ang = [](float dbm) { return (uint32_t)(90 + (constrain(dbm, -100.0f, -30.0f) + 100) * 180 / 70); };

  // coloured bands, dim, then lit up to the reading
  struct Band { int from, to; uint16_t c; } bands[] = {{-100, -80, C_BAD}, {-80, -70, C_SLOW}, {-70, -60, C_WARN}, {-60, -30, C_GOOD}};
  for (auto& b : bands) {
    spr.drawArc(cx, cy, ro, ri, ang(b.from), ang(b.to), spr.alphaBlend(60, b.c, C_BG), C_BG, false);
    if (needle > b.from) spr.drawArc(cx, cy, ro, ri, ang(b.from), ang(min((float)b.to, needle)), b.c, C_BG, false);
  }
  // ticks every 10 dBm, labels every 20
  spr.setTextFont(1);
  spr.setTextColor(C_DIM, C_BG);
  for (int d = -100; d <= -30; d += 10) {
    float a = ang(d) * PI / 180;
    spr.drawLine(cx - (ri - 2) * sinf(a), cy + (ri - 2) * cosf(a), cx - (ri - 7) * sinf(a), cy + (ri - 7) * cosf(a), C_DIM);
    if ((d + 100) % 20 == 0 || d == -30) {
      spr.setTextDatum(MC_DATUM);
      spr.drawString(String(d), cx - (ri - 16) * sinf(a), cy + (ri - 16) * cosf(a));
    }
  }
  // needle
  float a = ang(needle) * PI / 180;
  int tx = cx - (ro - 2) * sinf(a), ty = cy + (ro - 2) * cosf(a);
  int bx = 4 * cosf(a), by = 4 * sinf(a);
  spr.fillTriangle(cx + bx, cy + by, cx - bx, cy - by, tx, ty, C_TEXT);
  spr.fillCircle(cx, cy, 6, C_TEXT);
  spr.fillCircle(cx, cy, 3, C_HDR);

  // the numbers
  uint16_t c = rssiColor(r);
  spr.setTextDatum(TR_DATUM);
  spr.setTextFont(7);
  spr.setTextColor(c, C_BG);
  spr.drawString(String(r), 290, 26);
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("dBm", 293, 58);
  spr.setTextDatum(TR_DATUM);
  spr.setTextFont(4);
  spr.drawString(quality(r), 314, 80);
  spr.setTextFont(2);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(String(pct(r)) + "% quality", 314, 104);
  if (cntR) {
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("min " + String(minR) + "  max " + String(maxR) + "  avg " + String(sumR / cntR), 314, 122);
  }

  // history strip: one bar a second, coloured by strength
  const int top = 134, h = 34;
  spr.drawFastHLine(0, top - 1, 320, C_HDR);
  for (int i = 0; i < histN; i++) {
    int v = hist[i];
    int bh = max(1, (constrain(v, -100, -30) + 100) * h / 70);
    spr.fillRect(320 - (histN - i) * 2, top + h - bh, 2, bh, spr.alphaBlend(170, rssiColor(v), C_BG));
  }
  spr.setTextFont(1);
  spr.setTextColor(C_DIM);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(String(histN / 60) + ":" + (histN % 60 < 10 ? "0" : "") + String(histN % 60), 2, top + 1);
}

void drawLink() {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { centered("No link info", 90); return; }
  char bssid[18];
  snprintf(bssid, sizeof bssid, "%02X:%02X:%02X:%02X:%02X:%02X", ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
  int mhz = ap.primary == 14 ? 2484 : 2407 + 5 * ap.primary;
  String width = ap.second == WIFI_SECOND_CHAN_ABOVE ? "40 MHz (+)" : ap.second == WIFI_SECOND_CHAN_BELOW ? "40 MHz (-)" : "20 MHz";
  String modes;
  if (ap.phy_11b) modes += "b/";
  if (ap.phy_11g) modes += "g/";
  if (ap.phy_11n) modes += "n/";
  if (ap.phy_11ax) modes += "ax/";
  if (modes.length()) modes.remove(modes.length() - 1);
  String sec = String(authName(ap.authmode)) + ", " + cipherName(ap.pairwise_cipher);
  char cc[4] = {ap.country.cc[0], ap.country.cc[1], 0, 0};
  String country = cc[0] >= 'A' ? String(cc) + ", ch " + String(ap.country.schan) + "-" + String(ap.country.schan + ap.country.nchan - 1) : String("not sent");
  wifi_power_t tx = WiFi.getTxPower();

  row(0, "Network", WiFi.SSID(), C_ACC);
  row(1, "Access pt", bssid);
  row(2, "Channel", String(ap.primary) + "  (" + String(mhz) + " MHz)");
  row(3, "Width", width);
  row(4, "Mode", "802.11" + modes + "  2.4 GHz");
  row(5, "Security", sec, ap.authmode == WIFI_AUTH_OPEN || ap.authmode == WIFI_AUTH_WEP ? C_BAD : C_TEXT);
  row(6, "Country", country);
  row(7, "TX power", String((int)tx / 4.0f, 1) + " dBm");
  row(8, "Connected", connectedAt ? fmtDur((millis() - connectedAt) / 1000) + (wifiEpoch ? "  (" + String(wifiEpoch) + " drop" + (wifiEpoch == 1 ? ")" : "s)") : "") : "-");
}

void drawNetwork() {
  if (!triedPublic) fetchPublic();
  row(0, "IP", WiFi.localIP().toString(), C_ACC);
  row(1, "Mask", WiFi.subnetMask().toString());
  row(2, "Gateway", WiFi.gatewayIP().toString());
  row(3, "DNS 1", WiFi.dnsIP(0).toString());
  row(4, "DNS 2", WiFi.dnsIP(1).toString());
  row(5, "MAC", WiFi.macAddress());
  row(6, "Hostname", WiFi.getHostname());
  row(7, "Public IP", publicIp.length() ? publicIp : "-");
  row(8, "ISP", isp.length() ? isp : "-");
}

void drawPing() {
  for (int i = 0; i < 3; i++) {
    Target& t = targets[i];
    int y = 24 + i * 48;
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(t.label, 6, y);
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(t.ip.toString(), 6, y + 17);
    int loss = t.sent ? t.lost * 100 / t.sent : 0;
    spr.setTextColor(loss ? C_BAD : C_DIM, C_BG);
    spr.drawString(t.sent ? "loss " + String(loss) + "% of " + String(t.sent) : "waiting", 6, y + 28);

    spr.setTextDatum(TR_DATUM);
    spr.setTextFont(4);
    uint16_t c = t.last < 0 ? C_BAD : t.last < 20 ? C_GOOD : t.last < 100 ? C_WARN : C_SLOW;
    spr.setTextColor(t.sent ? c : C_DIM, C_BG);
    spr.drawString(!t.sent ? "-" : t.last < 0 ? "lost" : String(t.last, 0) + " ms", 314, y + 2);
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, C_BG);
    if (t.avg) spr.drawString("avg " + String(t.avg, 1) + " ms", 314, y + 28);

    // the last 40 tries: a bar each, red where lost
    float top = 1;
    for (int k = 0; k < t.n; k++) top = max(top, t.hist[k]);
    for (int k = 0; k < t.n; k++) {
      int x = 110 + k * 3;
      if (t.hist[k] < 0) { spr.fillRect(x, y + 6, 2, 30, C_BAD); continue; }
      int bh = max(1, (int)(t.hist[k] / top * 30));
      spr.fillRect(x, y + 36 - bh, 2, bh, C_ACC);
    }
    if (i < 2) spr.drawFastHLine(0, y + 45, 320, C_HDR);
  }
}

void drawChannels() {
  if (!chEver) { centered("Scanning channels...", 90, 2); return; }
  int ch = WiFi.channel();
  int top = 1;
  for (int c = 1; c <= 13; c++) top = max(top, onCh[c] + nearCh[c]);
  const int base = 118, h = 84;
  for (int c = 1; c <= 13; c++) {
    int x = 8 + (c - 1) * 24;
    int hOn = onCh[c] * h / top, hNear = nearCh[c] * h / top;
    bool me = c == ch;
    spr.fillRect(x, base - hOn - hNear, 18, hNear, spr.alphaBlend(70, me ? C_ACC : C_DIM, C_BG));
    spr.fillRect(x, base - hOn, 18, hOn, me ? C_ACC : C_TEXT);
    spr.setTextFont(1);
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(me ? C_ACC : C_DIM, C_BG);
    spr.drawString(String(c), x + 9, base + 3);
    if (onCh[c]) {
      spr.setTextDatum(BC_DATUM);
      spr.setTextColor(C_TEXT, C_BG);
      spr.drawString(String(onCh[c]), x + 9, base - hOn - hNear - 1);
    }
  }
  // the least crowded of the three that do not overlap
  int best = 1;
  for (int c : {6, 11}) if (onCh[c] + nearCh[c] < onCh[best] + nearCh[best]) best = c;
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString("You: ch " + String(ch) + ", " + String(onCh[ch]) + " on it, " + String(nearCh[ch]) + " overlap", 6, 134);
  spr.setTextColor(best == ch ? C_GOOD : C_WARN, C_BG);
  spr.drawString(best == ch ? "Already on the clearest of 1/6/11" : "Clearest of 1/6/11: ch " + String(best), 6, 151);
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(String(sameNet) + " AP" + (sameNet == 1 ? "" : "s") + " for this network", 314, 134);
  if (bestOther > -127) spr.drawString("next best " + String(bestOther) + " dBm", 314, 151);
}

void render() {
  spr.fillSprite(C_BG);
  header(String(TITLES[page]) + "  " + (wifiUp() ? fit(WiFi.SSID(), 190, 2) : String("")), String(page + 1) + "/" + String(NUM_PAGES));
  if (!wifiUp()) centered("Not connected", 90);
  else switch (page) {
    case 0: drawMeter(); break;
    case 1: drawLink(); break;
    case 2: drawNetwork(); break;
    case 3: drawPing(); break;
    default: drawChannels(); break;
  }
  spr.pushSprite(0, 0);
}

// ---------- App ----------
void start() {
  if (!connectedAt && wifiUp()) connectedAt = millis();
  render();
}

void run() {
  if (seenEpoch != wifiEpoch) { seenEpoch = wifiEpoch; connectedAt = millis(); }   // came back after a drop
  if (wifiUp() && !connectedAt) connectedAt = millis();
  if (millis() - lastSample > 1000) { lastSample = millis(); sample(); }
  if (page == 3 && millis() - lastPing > 700) { pingOne(); lastPing = millis(); }
  if (page == 4) channelScan();

  if (wifiUp()) needle += (WiFi.RSSI() - needle) * 0.25f;
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > (page == 0 ? 60 : 500)) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) page = (page + 1) % NUM_PAGES;
  else switch (page) {
    case 0: resetSignal(); break;
    case 2: fetchPublic(); break;
    case 3: resetPing(); break;
    case 4: channelScan(true); break;
  }
  render();
}

}  // namespace wifimeter
