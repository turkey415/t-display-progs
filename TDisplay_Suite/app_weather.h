// Weather Clock
//   Page 1: local time, current conditions, feels-like / humidity / wind / hi-lo
//   Page 2: 5-day forecast
// Location is looked up from the public IP (ip-api.com) at start, every hour and after Wi-Fi
// comes back, so it follows the device around. Weather and the local time zone come from
// Open-Meteo (no API key), every 15 min, so DST and travel across zones are automatic.
// Keys: BTN_B switch page, BTN_A refresh now.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace weather {

// To pin the location instead (e.g. behind a VPN), set both coordinates and a display name.
const float FIXED_LAT  = NAN;
const float FIXED_LON  = NAN;
const char* FIXED_NAME = "";

const uint32_t WX_EVERY_MS  = 15UL * 60UL * 1000UL;
const uint32_t WX_RETRY_MS  = 60UL * 1000UL;
const uint32_t LOC_RETRY_MS = 60UL * 1000UL;
const uint32_t LOC_EVERY_MS = 60UL * 60UL * 1000UL;

#define C_SUN    0xFE60
#define C_CLOUD  0xBDF7
#define C_STORM  0x7BEF
#define C_RAIN   0x3D9F

struct Location {
  float  lat, lon;
  String name;
  bool   ok;
} loc;

struct Weather {
  bool     ok;
  uint32_t fetchedAt;
  int32_t  utcOffset;
  float    temp, feels, wind;
  int      hum, code;
  bool     isDay;
  int      days;
  int      dCode[5], dPop[5], dWday[5];
  float    dHi[5], dLo[5];
  bool     fahrenheit;   // the units it was fetched in
} wx;

int      page = 0;
String   status = "Finding location";
uint32_t lastLoc = 0, lastTry = 0, seenEpoch = 0;
bool     locDue = true;

const char* WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* MON[]  = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int weekday(int y, int m, int d) {          // 0 = Sunday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

const char* wxText(int c) {
  if (c == 0)               return "Clear";
  if (c == 1)               return "Mostly clear";
  if (c == 2)               return "Partly cloudy";
  if (c == 3)               return "Overcast";
  if (c == 45 || c == 48)   return "Fog";
  if (c >= 51 && c <= 57)   return "Drizzle";
  if (c >= 61 && c <= 67)   return "Rain";
  if (c >= 71 && c <= 77)   return "Snow";
  if (c >= 80 && c <= 82)   return "Showers";
  if (c == 85 || c == 86)   return "Snow showers";
  if (c >= 95)              return "Thunderstorm";
  return "--";
}

// ---------- Network ----------
bool getLocation() {
  if (!isnan(FIXED_LAT) && !isnan(FIXED_LON)) {
    loc.lat = FIXED_LAT; loc.lon = FIXED_LON; loc.name = FIXED_NAME; loc.ok = true;
    return true;
  }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "http://ip-api.com/json/?fields=status,city,region,lat,lon")) return false;
  int code = http.GET();
  if (code != 200) { Serial.printf("geo HTTP %d\n", code); http.end(); return false; }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) || strcmp(doc["status"] | "", "success") != 0) {
    Serial.println("geo parse failed: " + body);
    return false;
  }
  loc.lat  = doc["lat"];
  loc.lon  = doc["lon"];
  loc.name = String(doc["city"] | "?") + ", " + String(doc["region"] | "");
  loc.ok   = true;
  Serial.printf("location %s (%.3f, %.3f)\n", loc.name.c_str(), loc.lat, loc.lon);
  return true;
}

bool fetchWeather() {
  if (!wifiUp() || !loc.ok) return false;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(loc.lat, 3) +
               "&longitude=" + String(loc.lon, 3) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
               "weather_code,wind_speed_10m,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
               "precipitation_probability_max"
               "&timezone=auto&forecast_days=5";
  if (cfg.fahrenheit) url += "&temperature_unit=fahrenheit&wind_speed_unit=mph";

  WiFiClientSecure client;
  client.setInsecure();                 // public, read-only data
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { Serial.printf("weather HTTP %d\n", code); http.end(); return false; }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) { Serial.println("weather parse failed"); return false; }

  wx.utcOffset = doc["utc_offset_seconds"] | 0;
  JsonObject cur = doc["current"];
  wx.temp  = cur["temperature_2m"];
  wx.feels = cur["apparent_temperature"];
  wx.hum   = cur["relative_humidity_2m"];
  wx.wind  = cur["wind_speed_10m"];
  wx.code  = cur["weather_code"];
  wx.isDay = (cur["is_day"] | 1) == 1;

  JsonObject d = doc["daily"];
  JsonArray days = d["time"];
  wx.days = min((int)days.size(), 5);
  for (int i = 0; i < wx.days; i++) {
    int y = 0, m = 1, dd = 1;
    sscanf(days[i] | "2000-01-01", "%d-%d-%d", &y, &m, &dd);
    wx.dWday[i] = weekday(y, m, dd);
    wx.dCode[i] = d["weather_code"][i] | 0;
    wx.dHi[i]   = d["temperature_2m_max"][i] | 0.0f;
    wx.dLo[i]   = d["temperature_2m_min"][i] | 0.0f;
    wx.dPop[i]  = d["precipitation_probability_max"][i] | -1;
  }
  wx.ok = true;
  wx.fahrenheit = cfg.fahrenheit;
  wx.fetchedAt = millis();
  Serial.printf("weather %.1f, code %d, offset %d\n", wx.temp, wx.code, wx.utcOffset);
  return true;
}

// ---------- Icons ----------
void sun(int cx, int cy, float s) {
  for (int k = 0; k < 8; k++) {
    float a = k * PI / 4;
    int x0 = cx + cosf(a) * 13 * s, y0 = cy + sinf(a) * 13 * s;
    int x1 = cx + cosf(a) * 18 * s, y1 = cy + sinf(a) * 18 * s;
    spr.drawLine(x0, y0, x1, y1, C_SUN);
    spr.drawLine(x0 + 1, y0, x1 + 1, y1, C_SUN);
  }
  spr.fillCircle(cx, cy, 10 * s, C_SUN);
}

void moon(int cx, int cy, float s) {
  spr.fillCircle(cx, cy, 11 * s, C_SUN);
  spr.fillCircle(cx + 6 * s, cy - 4 * s, 10 * s, C_BG);
}

void cloud(int cx, int cy, float s, uint16_t col) {
  spr.fillCircle(cx - 8 * s, cy + 2 * s, 8 * s, col);
  spr.fillCircle(cx + 1 * s, cy - 4 * s, 11 * s, col);
  spr.fillCircle(cx + 11 * s, cy + 3 * s, 7 * s, col);
  spr.fillRoundRect(cx - 16 * s, cy + 2 * s, 34 * s, 9 * s, 4 * s, col);
}

void drawIcon(int cx, int cy, float s, int code, bool day) {
  if (code == 0) { day ? sun(cx, cy, s) : moon(cx, cy, s); return; }
  if (code <= 2) {
    if (day) sun(cx - 7 * s, cy - 7 * s, s * 0.75f); else moon(cx - 7 * s, cy - 7 * s, s * 0.8f);
    cloud(cx + 3 * s, cy + 3 * s, s * 0.85f, C_CLOUD);
    return;
  }
  if (code == 3) { cloud(cx, cy, s, C_CLOUD); return; }
  if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++)
      spr.fillRoundRect(cx - 16 * s, cy - 11 * s + i * 7 * s, 32 * s, 3 * s + 1, 1, C_DIM);
    return;
  }

  bool storm = code >= 95;
  bool snow  = (code >= 71 && code <= 77) || code == 85 || code == 86;
  cloud(cx, cy - 6 * s, s, storm ? C_STORM : C_CLOUD);
  int by = cy + 8 * s;
  if (storm) {
    spr.fillTriangle(cx + 3 * s, by - 2 * s, cx - 5 * s, by + 7 * s, cx + 1 * s, by + 7 * s, C_SUN);
    spr.fillTriangle(cx - 1 * s, by + 5 * s, cx + 5 * s, by + 5 * s, cx - 4 * s, by + 15 * s, C_SUN);
  } else {
    for (int i = -1; i <= 1; i++) {
      int x = cx + i * 9 * s;
      if (snow) {
        spr.fillCircle(x, by + 5 * s, 2 * s + 1, C_TEXT);
      } else {
        spr.drawLine(x, by, x - 3 * s, by + 9 * s, C_RAIN);
        spr.drawLine(x + 1, by, x + 1 - 3 * s, by + 9 * s, C_RAIN);
      }
    }
  }
}

// ---------- Drawing ----------
// Temperature centered on cx with a drawn degree ring (the built-in fonts lack °).
void drawTemp(int cx, int y, float v, int font, uint16_t col) {
  String num = String((int)lroundf(v));
  int h = spr.fontHeight(font);
  int r = max(2, h / 12);
  int wNum = spr.textWidth(num, font);
  int x = cx - (wNum + 2 * r + 3) / 2;

  spr.setTextFont(font);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(col, C_BG);
  spr.drawString(num, x, y);
  x += wNum + r + 2;
  spr.drawCircle(x, y + r + 1, r, col);
  if (r > 2) spr.drawCircle(x, y + r + 1, r - 1, col);
}

void statBox(int x, const char* label, const String& val) {
  spr.drawRoundRect(x + 3, 124, 74, 45, 5, C_HDR);
  spr.setTextDatum(TC_DATUM);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(label, x + 40, 126);
  int f = val.length() <= 5 ? 4 : 2;
  spr.setTextFont(f);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(val, x + 40, f == 4 ? 142 : 147);
}

bool haveLocalTime(struct tm& out) {
  time_t now = time(nullptr);
  if (now < 1700000000 || !wx.ok) return false;
  now += wx.utcOffset;
  gmtime_r(&now, &out);
  return true;
}

void drawWaiting() {
  header("WEATHER CLOCK", wifiUp() ? "" : "Wi-Fi DOWN", C_ACC, C_BAD);
  centered(status, 90);
}

void drawNow() {
  struct tm t;
  if (!haveLocalTime(t)) { drawWaiting(); return; }

  char buf[32];
  if (wifiUp()) {
    snprintf(buf, sizeof buf, "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
    header(loc.name, buf);
  } else {
    header(loc.name, "Wi-Fi DOWN", C_ACC, C_BAD);
  }

  int hr = t.tm_hour;
  if (!cfg.clock24) { hr %= 12; if (hr == 0) hr = 12; }
  snprintf(buf, sizeof buf, cfg.clock24 ? "%02d:%02d" : "%d:%02d", hr, t.tm_min);
  spr.setTextFont(7);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(buf, 6, 28);
  int x = 6 + spr.textWidth(buf, 7) + 6;
  snprintf(buf, sizeof buf, "%02d", t.tm_sec);
  spr.setTextFont(4);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(buf, x, 28);
  if (!cfg.clock24) {
    spr.setTextFont(2);
    spr.setTextColor(C_ACC, C_BG);
    spr.drawString(t.tm_hour < 12 ? "AM" : "PM", x + 2, 58);
  }

  spr.setTextFont(4);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(wxText(wx.code), 6, 88);

  drawIcon(268, 44, 1.0f, wx.code, wx.isDay);
  drawTemp(268, 72, wx.temp, 6, C_TEXT);

  String spd = wx.fahrenheit ? "m" : "k";
  statBox(0,   "Feels",  String((int)lroundf(wx.feels)));
  statBox(80,  "Humid",  String(wx.hum) + "%");
  statBox(160, "Wind",   String((int)lroundf(wx.wind)) + (wx.wind < 100 ? spd : ""));
  statBox(240, "Hi / Lo", wx.days ? String((int)lroundf(wx.dHi[0])) + "/" + String((int)lroundf(wx.dLo[0])) : "--");
}

void drawForecast() {
  if (!wx.ok) { drawWaiting(); return; }
  header("5-DAY FORECAST", loc.name);

  for (int i = 0; i < wx.days; i++) {
    int cx = 32 + i * 64;
    if (i) spr.drawFastVLine(i * 64, 26, 138, C_HDR);

    spr.setTextFont(2);
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(i == 0 ? C_ACC : C_TEXT, C_BG);
    spr.drawString(i == 0 ? "Today" : WDAY[wx.dWday[i]], cx, 26);

    drawIcon(cx, 66, 0.85f, wx.dCode[i], true);

    drawTemp(cx, 92, wx.dHi[i], 4, C_TEXT);
    drawTemp(cx, 120, wx.dLo[i], 2, C_DIM);

    if (wx.dPop[i] >= 0) {
      spr.setTextFont(2);
      spr.setTextDatum(TC_DATUM);
      spr.setTextColor(wx.dPop[i] >= 50 ? C_RAIN : C_DIM, C_BG);
      spr.drawString(String(wx.dPop[i]) + "%", cx, 144);
    }
  }
}

void render() {
  spr.fillSprite(C_BG);
  if (page == 0) drawNow(); else drawForecast();
  spr.pushSprite(0, 0);
}

// ---------- App ----------
void start() {
  page = 0;
  if (wx.ok && wx.fahrenheit != cfg.fahrenheit) wx.ok = false;   // units changed in setup
  render();
}

void run() {
  bool up = wifiUp();

  // Re-check location hourly and after any reconnect; refetch weather if it moved.
  if (seenEpoch != wifiEpoch) { seenEpoch = wifiEpoch; locDue = true; }
  uint32_t every = loc.ok ? LOC_EVERY_MS : LOC_RETRY_MS;
  if (up && (locDue || millis() - lastLoc > every)) {
    lastLoc = millis();
    locDue = false;
    String before = loc.name;
    if (getLocation()) {
      status = "Getting weather";
      render();
      if (!wx.ok || loc.name != before) { lastTry = millis(); if (!fetchWeather()) status = "Weather failed, retrying"; }
    } else {
      status = "Location failed, retrying";
    }
    render();
  }

  uint32_t due = wx.ok ? WX_EVERY_MS : WX_RETRY_MS;
  if (up && loc.ok && millis() - lastTry > due && (!wx.ok || millis() - wx.fetchedAt > due)) {
    lastTry = millis();
    if (!fetchWeather() && !wx.ok) status = "Weather failed, retrying";
  }

  static time_t lastSec = 0;
  time_t now = time(nullptr);
  if (now != lastSec) { lastSec = now; render(); }
}

void press(int pin) {
  if (pin == BTN_B) page ^= 1;
  else              fetchWeather();
  render();
}

}  // namespace weather
