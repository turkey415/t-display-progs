// Ham Shack Clock
//   Page 1: big UTC clock, local time, and SFI / SSN / A / K indices
//   Page 2: HF band conditions (day / night), X-ray, geomag field, noise
// Solar data from hamqsl.com (N0NBH), refreshed every 30 minutes. Callsign and local
// time zone are set on the Wi-Fi setup page.
// Keys: BTN_B switch page, BTN_A refresh solar data now.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

namespace ham {

const char* SOLAR_URL = "https://www.hamqsl.com/solarxml.php";
const uint32_t SOLAR_EVERY_MS = 30UL * 60UL * 1000UL;
const uint32_t SOLAR_RETRY_MS = 2UL  * 60UL * 1000UL;

#define C_FAIR  0xFFE0

struct Solar {
  String sfi, ssn, a, k, xray, geomag, noise, updated;
  String band[4], day[4], night[4];
  int    bands;
  bool   ok;
  uint32_t fetchedAt;
} solar;

int      page = 0;
uint32_t lastTry = 0;
bool     tried = false;

// ---------- Parsing ----------
String tagVal(const String& xml, const char* name) {
  String open = "<" + String(name) + ">", close = "</" + String(name) + ">";
  int a = xml.indexOf(open);
  if (a < 0) return "--";
  a += open.length();
  int b = xml.indexOf(close, a);
  if (b < 0) return "--";
  String v = xml.substring(a, b);
  v.trim();
  return v;
}

void parseBands(const String& xml) {
  solar.bands = 0;
  int cc = xml.indexOf("<calculatedconditions>");
  int end = xml.indexOf("</calculatedconditions>");
  if (cc < 0 || end < 0) return;
  int pos = cc;
  while (true) {
    int s = xml.indexOf("<band name=\"", pos);
    if (s < 0 || s > end) break;
    s += 12;
    String name = xml.substring(s, xml.indexOf('"', s));
    int t = xml.indexOf("time=\"", s) + 6;
    String when = xml.substring(t, xml.indexOf('"', t));
    int v0 = xml.indexOf('>', t) + 1;
    int v1 = xml.indexOf("</band>", v0);
    String val = xml.substring(v0, v1);
    val.trim();
    pos = v1;

    int idx = -1;
    for (int i = 0; i < solar.bands; i++) if (solar.band[i] == name) idx = i;
    if (idx < 0 && solar.bands < 4) { idx = solar.bands++; solar.band[idx] = name; }
    if (idx < 0) continue;
    if (when == "day") solar.day[idx] = val; else solar.night[idx] = val;
  }
}

bool fetchSolar() {
  if (!wifiUp()) return false;
  WiFiClientSecure client;
  client.setInsecure();                 // public, read-only data
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  if (!http.begin(client, SOLAR_URL)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("solar HTTP %d\n", code); return false; }
  String xml = http.getString();
  http.end();

  solar.sfi     = tagVal(xml, "solarflux");
  solar.ssn     = tagVal(xml, "sunspots");
  solar.a       = tagVal(xml, "aindex");
  solar.k       = tagVal(xml, "kindex");
  solar.xray    = tagVal(xml, "xray");
  solar.geomag  = tagVal(xml, "geomagfield");
  solar.noise   = tagVal(xml, "signalnoise");
  solar.updated = tagVal(xml, "updated");
  parseBands(xml);
  solar.ok = true;
  solar.fetchedAt = millis();
  return true;
}

// ---------- Drawing ----------
uint16_t condColor(const String& c) {
  if (c.equalsIgnoreCase("Good")) return C_GOOD;
  if (c.equalsIgnoreCase("Fair")) return C_FAIR;
  if (c.equalsIgnoreCase("Poor")) return C_BAD;
  return C_DIM;
}

uint16_t kColor(const String& k) {
  int v = k.toInt();
  if (k == "--" || k == "") return C_DIM;
  return v <= 2 ? C_GOOD : v <= 4 ? C_FAIR : C_BAD;
}

void statBox(int x, const char* label, const String& val, uint16_t col) {
  spr.drawRoundRect(x + 3, 118, 74, 50, 5, C_HDR);
  spr.setTextDatum(TC_DATUM);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(label, x + 40, 121);
  spr.setTextFont(4);
  spr.setTextColor(col, C_BG);
  spr.drawString(val, x + 40, 139);
}

void drawClock() {
  time_t now = time(nullptr);
  bool synced = now > 1700000000;
  header(cfg.callsign, wifiUp() ? "Wi-Fi OK" : "Wi-Fi DOWN", C_ACC, wifiUp() ? C_DIM : C_BAD);

  if (!synced) {
    centered("Syncing time...", 70);
  } else {
    struct tm utc, loc;
    gmtime_r(&now, &utc);
    localtime_r(&now, &loc);
    char buf[32];

    strftime(buf, sizeof buf, "%H:%M:%S", &utc);
    spr.setTextFont(7);
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(buf, 150, 26);
    spr.setTextFont(2);
    spr.setTextColor(C_ACC, C_BG);
    spr.setTextDatum(TL_DATUM);
    spr.drawString("UTC", 284, 30);

    strftime(buf, sizeof buf, "%a %d %b", &utc);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(buf, 6, 80);

    strftime(buf, sizeof buf, cfg.clock24 ? "Local %H:%M %Z" : "Local %I:%M%p %Z", &loc);
    spr.setTextFont(4);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(buf, 314, 82);
  }

  statBox(0,   "SFI", solar.ok ? solar.sfi : "--", C_TEXT);
  statBox(80,  "SSN", solar.ok ? solar.ssn : "--", C_TEXT);
  statBox(160, "A",   solar.ok ? solar.a   : "--", C_TEXT);
  statBox(240, "K",   solar.ok ? solar.k   : "--", kColor(solar.k));
}

void drawBands() {
  header(cfg.callsign, "HF CONDITIONS");
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("Band", 10, 24);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("Day", 175, 24);
  spr.drawString("Night", 265, 24);

  if (!solar.ok || solar.bands == 0) centered("No data yet", 85);
  for (int i = 0; i < solar.bands; i++) {
    int y = 42 + i * 25;
    spr.setTextFont(4);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(solar.band[i], 10, y);
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(condColor(solar.day[i]), C_BG);
    spr.drawString(solar.day[i], 175, y);
    spr.setTextColor(condColor(solar.night[i]), C_BG);
    spr.drawString(solar.night[i], 265, y);
  }

  spr.drawFastHLine(0, 146, 320, C_HDR);
  spr.setTextFont(2);
  spr.setTextDatum(BL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("X-ray " + solar.xray + "  Geo " + solar.geomag + "  Noise " + solar.noise, 6, 169);
}

void render() {
  spr.fillSprite(C_BG);
  if (page == 0) drawClock(); else drawBands();
  spr.pushSprite(0, 0);
}

// ---------- App ----------
void start() {
  page = 0;
  render();
}

void run() {
  uint32_t due = solar.ok ? SOLAR_EVERY_MS : SOLAR_RETRY_MS;
  if (wifiUp() && (!tried || (millis() - lastTry > due && millis() - solar.fetchedAt > due))) {
    tried = true;
    lastTry = millis();
    fetchSolar();
  }

  static time_t lastSec = 0;
  time_t now = time(nullptr);
  if (now != lastSec) { lastSec = now; render(); }
}

void press(int pin) {
  if (pin == BTN_B) page ^= 1;
  else              fetchSolar();
  render();
}

}  // namespace ham
