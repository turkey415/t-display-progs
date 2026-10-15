// GXL Firewall Monitor
// The Ops Console's FIREWALL and LAN CACHE banner on the desk: live Mbps into and out of the
// hall and a phosphor trace of the last half hour. Data comes from a GXL Event Server
// (GET /agent/gear, server 0.6.10 or newer), read with the event's agent key, so the device
// needs no staff login. Server address and key are set on the Wi-Fi setup page.
// Keys: BTN_B firewall / LAN cache, BTN_A refresh now.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace gxl {

const uint32_t POLL_MS  = 10UL * 1000UL;   // the server reads the switches every 20 s
const uint32_t STALE_MS = 90UL * 1000UL;
const int      POINTS   = 90;              // 30 min at one point per 20 s, as on the console

// The console's colours for each role: what comes into the hall, and what goes out of it.
struct Look { const char* title; const char* role; uint16_t down, up; const char* downLabel; const char* upLabel; };
const Look LOOKS[2] = {
  {"FIREWALL",  "firewall", 0xF9F1 /* #ff3d8b */, 0xFE87 /* #ffd23f */, "IN",     "OUT"},
  {"LAN CACHE", "cache",    0xB35F /* #b26bff */, 0x3FF5 /* #3dffa8 */, "SERVED", "FILL"},
};
#define C_GRID 0x18C3

struct Totals {
  int    ports;
  String where;
  float  down[POINTS], up[POINTS];
  int    n;
  bool   live;
  float  nowDown, nowUp;
} tot[2];

int      page = 0;
bool     everOk = false;
uint32_t lastOk = 0, lastTry = 0;
bool     tried = false;
String   problem = "Connecting to server";

String num3(float v) {    // three significant figures, like the console's seven-segment readouts
  return v >= 100 ? String(v, 0) : v >= 10 ? String(v, 1) : String(v, 2);
}

float niceMax(float v) {  // 1, 2 or 5 times a power of ten, at or above v
  if (!(v > 0)) return 1;
  float p = powf(10, floorf(log10f(v)));
  for (float m : {1.0f, 2.0f, 5.0f, 10.0f}) if (m * p >= v) return m * p;
  return 10 * p;
}

// ---------- Network ----------
bool fetch() {
  if (!wifiUp()) { problem = "Wi-Fi down"; return false; }
  if (!cfg.server.length() || !cfg.key.length()) { problem = ""; return false; }

  String url = cfg.server;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/agent/gear";

  WiFiClient plain;
  WiFiClientSecure secure;
  secure.setInsecure();     // event servers often have a self-signed certificate
  HTTPClient http;
  http.setTimeout(6000);
  bool ok = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
  if (!ok) { problem = "Bad server address"; return false; }
  http.addHeader("Authorization", "Agent " + cfg.key);
  int code = http.GET();
  if (code != 200) {
    problem = code == 401 ? "Agent key not accepted"
            : code == 404 ? "Server too old (needs 0.6.10+)"
            : code < 0    ? "Can't reach server"
                          : "Server said HTTP " + String(code);
    Serial.printf("gear HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) { problem = "Bad reply from server"; return false; }

  // Each role's ports added up point by point, newest last, like the console's gearTotals.
  for (int r = 0; r < 2; r++) {
    Totals& t = tot[r];
    t.ports = 0; t.where = ""; t.n = 0; t.live = false; t.nowDown = t.nowUp = 0;
    for (int i = 0; i < POINTS; i++) t.down[i] = t.up[i] = 0;
    for (JsonObject g : doc.as<JsonArray>()) {
      if (strcmp(g["role"] | "", LOOKS[r].role) != 0) continue;
      String sw = g["switch"] | "";
      int dot = sw.indexOf('.');
      if (dot > 0) sw = sw.substring(0, dot);
      t.where += (t.ports++ ? ", " : "") + sw + " " + String(g["port"] | "");

      JsonArray h = g["history"];
      int len = min((int)h.size(), POINTS);
      int skip = h.size() - len;
      t.n = max(t.n, len);
      for (int i = 0; i < len; i++) {   // aligned on the newest point, in slot POINTS-1
        t.down[POINTS - len + i] += h[skip + i][1] | 0.0f;
        t.up[POINTS - len + i]   += h[skip + i][2] | 0.0f;
      }
      if ((g["up"] | false) && !g["in_hall_mbps"].isNull()) {
        t.live = true;
        t.nowDown += g["in_hall_mbps"] | 0.0f;
        t.nowUp   += g["out_hall_mbps"] | 0.0f;
      }
    }
  }
  Serial.printf("gear: firewall %d port(s) %.2f/%.2f Mbps, %d points; cache %d port(s)\n",
                tot[0].ports, tot[0].nowDown, tot[0].nowUp, tot[0].n, tot[1].ports);
  everOk = true;
  lastOk = millis();
  problem = "";
  return true;
}

// ---------- Drawing ----------
void downArrow(int x, int y, uint16_t c) { spr.fillTriangle(x, y, x + 10, y, x + 5, y + 8, c); }
void upArrow(int x, int y, uint16_t c)   { spr.fillTriangle(x, y + 8, x + 10, y + 8, x + 5, y, c); }

void readout(int x, bool down, const Look& look, const Totals& t) {
  uint16_t c = down ? look.down : look.up;
  if (down) downArrow(x, 26, c); else upArrow(x, 26, c);
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(c, C_BG);
  spr.drawString(String(down ? look.downLabel : look.upLabel) + "  Mbps", x + 15, 22);
  spr.setTextFont(7);
  spr.drawString(t.live ? num3(down ? t.nowDown : t.nowUp) : "-", x, 40);
}

// The phosphor trace: filled area for traffic into the hall, a line for traffic out of it.
void scope(const Look& look, const Totals& t, int top, int h) {
  int bottom = top + h - 1;
  for (int i = 1; i < 10; i++) spr.drawFastVLine(i * 32, top, h, C_GRID);
  for (int i = 1; i < 4; i++)  spr.drawFastHLine(0, top + i * h / 4, 320, C_GRID);
  spr.drawRect(0, top, 320, h, C_GRID);

  int first = POINTS - t.n;
  float peak = 1;
  for (int i = first; i < POINTS; i++) peak = max(peak, max(t.down[i], t.up[i]));
  float scale = niceMax(peak * 1.1f);

  auto px = [](int i) { return (int)lroundf(i * 319.0f / (POINTS - 1)); };
  auto py = [&](float v) { return bottom - 1 - (int)lroundf(max(0.0f, v) / scale * (h - 4)); };

  if (t.n >= 2) {
    uint16_t fill = spr.alphaBlend(80, look.down, C_BG);
    for (int i = first + 1; i < POINTS; i++) {
      int x0 = px(i - 1), x1 = px(i), y0 = py(t.down[i - 1]), y1 = py(t.down[i]);
      for (int x = x0; x <= x1; x++) {
        int y = x1 == x0 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        spr.drawFastVLine(x, y, bottom - y, fill);
      }
    }
    for (int i = first + 1; i < POINTS; i++) {
      int x0 = px(i - 1), x1 = px(i);
      int d0 = py(t.down[i - 1]), d1 = py(t.down[i]), u0 = py(t.up[i - 1]), u1 = py(t.up[i]);
      spr.drawLine(x0, d0, x1, d1, look.down);
      spr.drawLine(x0, d0 - 1, x1, d1 - 1, look.down);
      spr.drawLine(x0, u0, x1, u1, look.up);
      spr.drawLine(x0, u0 - 1, x1, u1 - 1, look.up);
    }
  } else {
    centered("ACQUIRING SIGNAL...", top + h / 2, 2, look.down);
  }

  spr.setTextFont(1);
  spr.setTextColor(C_DIM);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(num3(scale) + " Mbps", 4, top + 3);
  spr.setTextDatum(TR_DATUM);
  spr.drawString("LAST " + String((t.n * 20 + 30) / 60) + " MIN", 316, top + 3);
}

void render() {
  const Look& look = LOOKS[page];
  const Totals& t = tot[page];
  spr.fillSprite(C_BG);

  bool stale = everOk && millis() - lastOk > STALE_MS;
  String right = !everOk ? "" : stale ? "STALE " + fmtDur((millis() - lastOk) / 1000)
               : t.ports ? t.where : "not marked";
  header(look.title, right, look.down, stale ? C_BAD : C_DIM);

  if (!cfg.server.length() || !cfg.key.length()) {
    centered("Set the GXL server", 60, 4, C_TEXT);
    centered("and agent key", 88, 4, C_TEXT);
    centered("Hold the top key for Wi-Fi setup", 130, 2);
  } else if (!everOk) {
    centered(problem.length() ? problem : "Connecting to server", 70, 4);
    centered(cfg.server, 110, 2);
  } else if (!t.ports) {
    centered(String("No ") + (page ? "LAN cache" : "firewall") + " port marked", 70, 4);
    centered("Mark it in the Ops Console: Switches > Ports", 110, 2);
  } else {
    readout(6, true, look, t);
    readout(166, false, look, t);
    scope(look, t, 92, 78);
    if (problem.length()) {           // a reading failed since the last good one
      spr.setTextFont(1);
      spr.setTextDatum(BC_DATUM);
      spr.setTextColor(C_BAD, C_BG);
      spr.drawString(problem, 160, 168);
    }
  }
  spr.pushSprite(0, 0);
}

// ---------- App ----------
void start() {
  page = 0;
  tried = false;      // fetch straight away
  render();
}

void run() {
  if (!tried || millis() - lastTry > POLL_MS) {
    tried = true;
    lastTry = millis();
    fetch();
    render();
  }
  static uint32_t lastDraw = 0;     // keep the STALE timer ticking
  if (millis() - lastDraw > 1000) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) { page ^= 1; render(); }
  else              { fetch(); render(); }
}

}  // namespace gxl
