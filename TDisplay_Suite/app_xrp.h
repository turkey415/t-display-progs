// XRP Ticker
// XRP's price in US dollars with the 24-hour change, and a price graph over 1 day, 1 week,
// 1 month or 3 months. Free public data from Coinbase, no key: the price every 15 s,
// 24-hour stats every minute, the graph every 5 min.
// Keys: BTN_B next range, BTN_A refresh now.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace xrp {

const char* API = "https://api.exchange.coinbase.com/products/XRP-USD/";
const uint32_t PRICE_MS = 15UL * 1000UL;
const uint32_t STATS_MS = 60UL * 1000UL;
const uint32_t CHART_MS = 5UL * 60UL * 1000UL;
const int      MAX_POINTS = 300;           // Coinbase sends at most 300 candles

struct Range { const char* label; const char* name; int granularity; int points; };
const Range RANGES[] = {
  {"1D", "24 HOURS",  300,   288},
  {"1W", "7 DAYS",    3600,  168},
  {"1M", "30 DAYS",   21600, 120},
  {"3M", "90 DAYS",   86400, 90},
};
const int NUM_RANGES = sizeof(RANGES) / sizeof(RANGES[0]);

#define C_UP    0x2FEA   // #2dff55-ish green
#define C_DOWN  0xF9C7   // #ff3a3a-ish red
#define C_XRP   0xDEFB

float    price = NAN, open24 = NAN, high24 = NAN, low24 = NAN, vol24 = NAN;
float    closes[MAX_POINTS];
int      n = 0, chartRange = -1;
int      range = 0;
uint32_t priceAt = 0, statsAt = 0, chartAt = 0, lastPrice = 0, lastStats = 0, lastChart = 0;
bool     triedPrice = false, triedStats = false, chartNow = true;
String   problem;

// GET a Coinbase path into doc (filtered if a filter is given). Coinbase wants a User-Agent.
bool get(const String& path, JsonDocument& doc) {
  if (!wifiUp()) { problem = "Wi-Fi down"; return false; }
  WiFiClientSecure client;
  client.setInsecure();                 // public, read-only data
  HTTPClient http;
  http.useHTTP10(true);                 // no chunked reply, so the JSON can be read as it arrives
  http.setTimeout(8000);
  if (!http.begin(client, String(API) + path)) return false;
  http.setUserAgent("TDisplaySuite (github.com/turkey415/t-display-progs)");
  int code = http.GET();
  if (code != 200) {
    char tls[80] = "";
    client.lastError(tls, sizeof tls);
    Serial.printf("xrp: GET %s -> %d (%s) tls: %s  heap %u free, %u largest\n", path.c_str(), code,
                  http.errorToString(code).c_str(), tls, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    problem = code < 0 ? "Can't reach Coinbase" : "Coinbase said HTTP " + String(code);
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { problem = "Bad reply from Coinbase"; return false; }
  problem = "";
  return true;
}

float num(JsonVariant v) { return v.is<const char*>() ? atof(v.as<const char*>()) : v.as<float>(); }

bool fetchPrice() {
  JsonDocument doc;
  if (!get("ticker", doc)) return false;
  price = num(doc["price"]);
  priceAt = millis();
  // keep the newest graph point on the live price
  if (n && chartRange == range) closes[n - 1] = price;
  return true;
}

bool fetchStats() {
  JsonDocument doc;
  if (!get("stats", doc)) return false;
  open24 = num(doc["open"]); high24 = num(doc["high"]);
  low24 = num(doc["low"]);   vol24 = num(doc["volume"]);
  statsAt = millis();
  return true;
}

// Candles come newest first as [time, low, high, open, close, volume]; keep the closes, oldest first.
bool fetchChart() {
  const Range& r = RANGES[range];
  JsonDocument doc;
  if (!get("candles?granularity=" + String(r.granularity), doc)) return false;
  JsonArray c = doc.as<JsonArray>();
  n = min((int)c.size(), r.points);
  for (int i = 0; i < n; i++) closes[n - 1 - i] = c[i][4] | 0.0f;
  chartRange = range;
  chartAt = millis();
  Serial.printf("xrp: %s %d points, price %.4f\n", r.label, n, price);
  return true;
}

// ---------- Drawing ----------
String money(float v) {                 // 4 decimals under $10, then 3, then 2
  if (isnan(v)) return "-";
  return String(v, v < 10 ? 4 : v < 100 ? 3 : 2);
}

String big(float v) {                   // 63754776 -> "63.8M"
  if (isnan(v)) return "-";
  if (v >= 1e9) return String(v / 1e9, 2) + "B";
  if (v >= 1e6) return String(v / 1e6, 1) + "M";
  if (v >= 1e3) return String(v / 1e3, 1) + "K";
  return String(v, 0);
}

void arrow(int x, int y, bool up, uint16_t c) {
  if (up) spr.fillTriangle(x, y + 12, x + 14, y + 12, x + 7, y, c);
  else    spr.fillTriangle(x, y, x + 14, y, x + 7, y + 12, c);
}

void chart(int top, int h) {
  int bottom = top + h - 1;
  for (int i = 1; i < 4; i++) spr.drawFastHLine(0, top + i * h / 4, 320, FAINT(0x10A2));
  if (n < 2 || chartRange != range) { centered("Loading chart...", top + h / 2, 2); return; }

  float lo = closes[0], hi = closes[0];
  int loAt = 0, hiAt = 0;
  for (int i = 1; i < n; i++) {
    if (closes[i] < lo) { lo = closes[i]; loAt = i; }
    if (closes[i] > hi) { hi = closes[i]; hiAt = i; }
  }
  float pad = max((hi - lo) * 0.08f, hi * 0.0005f);
  float vlo = lo - pad, vhi = hi + pad;
  bool rising = closes[n - 1] >= closes[0];
  uint16_t c = rising ? C_UP : C_DOWN;

  auto px = [&](int i) { return i * 319 / (n - 1); };
  auto py = [&](float v) { return bottom - (int)lroundf((v - vlo) / (vhi - vlo) * (h - 1)); };

  // the area under the line, fading towards the bottom
  for (int i = 1; i < n; i++) {
    int x0 = px(i - 1), x1 = px(i), y0 = py(closes[i - 1]), y1 = py(closes[i]);
    for (int x = x0; x <= x1; x++) {
      int y = x1 == x0 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
      for (int yy = y; yy <= bottom; yy++)
        spr.drawPixel(x, yy, spr.alphaBlend(70 * (bottom - yy) / max(1, bottom - y), c, C_BG));
    }
  }
  for (int i = 1; i < n; i++) {
    spr.drawLine(px(i - 1), py(closes[i - 1]), px(i), py(closes[i]), c);
    spr.drawLine(px(i - 1), py(closes[i - 1]) - 1, px(i), py(closes[i]) - 1, c);
  }
  spr.fillCircle(px(n - 1) - 2, py(closes[n - 1]), 3, C_TEXT);

  // high and low of the range, by where they happened
  spr.setTextFont(1);
  spr.setTextColor(C_TEXT);
  spr.setTextDatum(hiAt > n / 2 ? TR_DATUM : TL_DATUM);
  spr.drawString(money(hi), constrain(px(hiAt), 2, 317), max(top + 1, py(hi) - 10));
  spr.setTextDatum(loAt > n / 2 ? BR_DATUM : BL_DATUM);
  spr.drawString(money(lo), constrain(px(loAt), 2, 317), min(bottom, py(lo) + 11));
}

void render() {
  spr.fillSprite(C_BG);
  const Range& r = RANGES[range];

  // range change, from the first point of the graph to the live price
  float first = (n && chartRange == range) ? closes[0] : NAN;
  float rc = !isnan(first) && !isnan(price) && first > 0 ? (price - first) / first * 100 : NAN;
  bool stale = !isnan(price) && millis() - priceAt > 3 * PRICE_MS;
  if (stale)
    header("XRP / USD", "STALE " + fmtDur((millis() - priceAt) / 1000) + (problem.length() ? " - " + problem : ""), C_XRP, C_BAD);
  else
    header("XRP / USD", isnan(rc) ? String(r.name) : String(r.name) + "  " + (rc >= 0 ? "+" : "") + String(rc, 2) + "%",
           C_XRP, isnan(rc) ? C_DIM : rc >= 0 ? C_UP : C_DOWN);

  if (isnan(price)) {
    centered(problem.length() ? problem : "Getting price...", 85);
    show();
    return;
  }

  // price
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(4);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("$", 4, 28);
  spr.setTextFont(7);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(money(price), 20, 26);

  // 24-hour change
  if (!isnan(open24) && open24 > 0) {
    float ch = price - open24, pct = ch / open24 * 100;
    uint16_t c = ch >= 0 ? C_UP : C_DOWN;
    spr.setTextFont(4);
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(c, C_BG);
    String p = String(pct >= 0 ? "+" : "") + String(pct, 2) + "%";
    spr.drawString(p, 314, 28);
    arrow(314 - spr.textWidth(p, 4) - 18, 33, ch >= 0, c);
    spr.setTextFont(2);
    spr.drawString(String(ch >= 0 ? "+" : "-") + money(fabsf(ch)) + " 24h", 314, 56);
  }

  chart(80, 78);

  // 24-hour stats and the range buttons along the bottom
  spr.setTextFont(1);
  spr.setTextDatum(BL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("24h H " + money(high24) + "  L " + money(low24) + "  Vol " + big(vol24), 4, 169);
  for (int i = 0; i < NUM_RANGES; i++) {
    int x = 222 + i * 24;
    bool on = i == range;
    if (on) spr.fillRoundRect(x - 2, 160, 22, 10, 2, C_XRP);
    spr.setTextDatum(BC_DATUM);
    spr.setTextColor(on ? C_BG : C_DIM, on ? C_XRP : C_BG);
    spr.drawString(RANGES[i].label, x + 9, 169);
  }
  show();
}

// ---------- App ----------
void start() {
  render();
}

void run() {
  bool drew = false;
  if (!triedPrice || millis() - lastPrice > PRICE_MS) { triedPrice = true; lastPrice = millis(); fetchPrice(); drew = true; }
  if (!triedStats || millis() - lastStats > STATS_MS) { triedStats = true; lastStats = millis(); fetchStats(); drew = true; }
  if (chartNow || (chartRange != range && millis() - lastChart > 15000) || millis() - lastChart > CHART_MS) {
    chartNow = false;
    lastChart = millis();
    if (!fetchChart()) chartRange = -1;     // tried again in 15 s
    drew = true;
  }
  static uint32_t lastDraw = 0;
  if (drew || millis() - lastDraw > 1000) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) { range = (range + 1) % NUM_RANGES; chartNow = true; render(); }
  else { fetchPrice(); fetchStats(); fetchChart(); render(); }
}

}  // namespace xrp
