// Weather Clock
//   Page 1: local time, current conditions, feels-like / humidity / wind / hi-lo
//   Page 2: 5-day forecast
//   Page 3: sunrise / sunset with the sun's place on its arc, and the moon's phase
//   Page 4: weather alerts from the US National Weather Service (weather.gov), every 5 min.
//           While one is active the header takes turns showing it, and a warning
//           (severe or extreme) flashes a red border round every page.
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
const uint32_t ALERT_EVERY_MS = 5UL * 60UL * 1000UL;
const int      NUM_PAGES    = 4;
const int      MAX_ALERTS   = 6;

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
  int      dRise[5], dSet[5];   // minutes after local midnight, -1 if none
  int      dDay[5];             // seconds of daylight
  bool     fahrenheit;   // the units it was fetched in
} wx;

// Active alerts, one row per kind of alert (three Flood Warnings are one row, count 3).
struct Alert {
  String event, severity;
  String until;      // "Sun 09:40", local to the alert
  String endIso;     // for keeping the latest end of several
  int    rank;       // 4 extreme, 3 severe, 2 moderate, 1 minor, 0 unknown
  int    count;
};
struct Alerts {
  bool     checked;     // asked weather.gov at least once since the location was found
  bool     available;   // weather.gov answers for this place (US only)
  int      n;
  Alert    a[MAX_ALERTS];
  uint32_t fetchedAt;
} alerts;

int      page = 0;
String   status = "Finding location";
uint32_t lastLoc = 0, lastTry = 0, seenEpoch = 0, lastAlertTry = 0;
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

// "2026-10-15T07:21" (or with seconds and offset) -> 441 minutes after midnight; -1 if not a time.
int minutesOf(const char* iso) {
  int h, m;
  const char* t = strchr(iso, 'T');
  if (!t || sscanf(t + 1, "%d:%d", &h, &m) != 2) return -1;
  return h * 60 + m;
}

String hhmm(int minutes) {
  if (minutes < 0) return "--";
  int h = minutes / 60, m = minutes % 60;
  char buf[12];
  if (cfg.clock24) snprintf(buf, sizeof buf, "%02d:%02d", h, m);
  else snprintf(buf, sizeof buf, "%d:%02d%s", h % 12 ? h % 12 : 12, m, h < 12 ? "a" : "p");
  return buf;
}

int severityRank(const String& s) {
  if (s == "Extreme")  return 4;
  if (s == "Severe")   return 3;
  if (s == "Moderate") return 2;
  if (s == "Minor")    return 1;
  return 0;
}

uint16_t severityColor(int rank) {
  static const uint16_t c[] = {C_DIM, C_WARN, C_SLOW, C_BAD, 0xF81F};
  return c[constrain(rank, 0, 4)];
}

bool haveLocalTime(struct tm& out);

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
               "precipitation_probability_max,sunrise,sunset,daylight_duration"
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
    wx.dRise[i] = minutesOf(d["sunrise"][i] | "");
    wx.dSet[i]  = minutesOf(d["sunset"][i] | "");
    wx.dDay[i]  = (int)(d["daylight_duration"][i] | 0.0f);
  }
  wx.ok = true;
  wx.fahrenheit = cfg.fahrenheit;
  wx.fetchedAt = millis();
  Serial.printf("weather %.1f, code %d, offset %d\n", wx.temp, wx.code, wx.utcOffset);
  return true;
}

// Active alerts for this spot from weather.gov. Only the few fields shown are kept, so a long
// alert text does not have to fit in memory.
bool fetchAlerts() {
  if (!wifiUp() || !loc.ok) return false;
  String url = "https://api.weather.gov/alerts/active?point=" + String(loc.lat, 3) + "," + String(loc.lon, 3);
  WiFiClientSecure client;
  client.setInsecure();                 // public, read-only data
  HTTPClient http;
  http.useHTTP10(true);                 // no chunked reply, so the JSON can be read as it arrives
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  http.setUserAgent("TDisplaySuite (github.com/turkey415/t-display-progs)");   // weather.gov asks for one
  http.addHeader("Accept", "application/geo+json");
  int code = http.GET();
  alerts.checked = true;
  if (code != 200) {
    Serial.printf("alerts HTTP %d\n", code);
    http.end();
    if (code >= 400 && code < 500) { alerts.available = false; alerts.n = 0; }   // outside the US
    return false;
  }
  JsonDocument filter;
  JsonObject f = filter["features"][0]["properties"].to<JsonObject>();
  f["event"] = true; f["severity"] = true; f["ends"] = true; f["expires"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("alerts parse: %s\n", err.c_str()); return false; }

  alerts.available = true;
  alerts.n = 0;
  struct tm today;
  bool haveToday = haveLocalTime(today);
  for (JsonObject feat : doc["features"].as<JsonArray>()) {
    JsonObject p = feat["properties"];
    String event = p["event"] | "Alert";
    String sev   = p["severity"] | "Unknown";
    const char* end = p["ends"].isNull() ? (p["expires"] | "") : (p["ends"] | "");

    String until = "";
    int y, mo, d;
    if (sscanf(end, "%d-%d-%d", &y, &mo, &d) == 3) {
      until = hhmm(minutesOf(end));
      if (!haveToday || d != today.tm_mday) until = String(WDAY[weekday(y, mo, d)]) + " " + until;
    }

    int i = 0;
    while (i < alerts.n && alerts.a[i].event != event) i++;
    if (i < alerts.n) {                 // another of the same kind: count it, keep the latest end
      alerts.a[i].count++;
      if (String(end) > alerts.a[i].endIso) { alerts.a[i].until = until; alerts.a[i].endIso = end; }
      continue;
    }
    Alert al = {event, sev, until, String(end), severityRank(sev), 1};
    if (alerts.n < MAX_ALERTS) alerts.a[alerts.n++] = al;
    else if (al.rank > alerts.a[MAX_ALERTS - 1].rank) alerts.a[MAX_ALERTS - 1] = al;
    for (int j = alerts.n - 1; j > 0 && alerts.a[j].rank > alerts.a[j - 1].rank; j--) {   // worst first
      Alert t = alerts.a[j]; alerts.a[j] = alerts.a[j - 1]; alerts.a[j - 1] = t;
    }
  }
  alerts.fetchedAt = millis();
  Serial.printf("alerts: %d kind(s)\n", alerts.n);
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

// The page's own header, except that while an alert is active it takes turns (3 s each) with
// a banner in the alert's colour.
void wxHeader(const String& left, const String& right, uint16_t rightCol = C_DIM) {
  if (alerts.n && (millis() / 3000) % 2) {
    const Alert& a = alerts.a[0];
    uint16_t c = severityColor(a.rank);
    spr.fillRect(0, 0, 320, 20, c);
    spr.setTextFont(2);
    spr.setTextColor(C_BG, c);
    spr.setTextDatum(TL_DATUM);
    String ev = a.event;
    ev.toUpperCase();
    spr.drawString(ev + (a.count > 1 ? " x" + String(a.count) : ""), 6, 2);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(alerts.n > 1 ? "+" + String(alerts.n - 1) + " more" : a.until.length() ? "until " + a.until : "", 314, 2);
    return;
  }
  header(left, right, C_ACC, rightCol);
}

// A warning flashes a red border round the screen, once a second.
void alertBorder() {
  if (!alerts.n || alerts.a[0].rank < 3 || millis() % 1000 > 500) return;
  uint16_t c = severityColor(alerts.a[0].rank);
  for (int i = 0; i < 3; i++) spr.drawRect(i, i, 320 - 2 * i, 170 - 2 * i, c);
}

void drawWaiting() {
  wxHeader("WEATHER CLOCK", wifiUp() ? "" : "Wi-Fi DOWN", C_BAD);
  centered(status, 90);
}

void drawNow() {
  struct tm t;
  if (!haveLocalTime(t)) { drawWaiting(); return; }

  char buf[32];
  if (wifiUp()) {
    snprintf(buf, sizeof buf, "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
    wxHeader(loc.name, buf);
  } else {
    wxHeader(loc.name, "Wi-Fi DOWN", C_BAD);
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
  wxHeader("5-DAY FORECAST", loc.name);

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

// ---------- Sun & moon ----------
const double SYNODIC = 29.530588853;              // days from new moon to new moon

double moonAge(time_t now) {                      // days since the last new moon
  double days = (now - 947182440.0) / 86400.0;    // a new moon: 2000-01-06 18:14 UTC
  double age = fmod(days, SYNODIC);
  return age < 0 ? age + SYNODIC : age;
}

const char* moonName(double age) {
  static const char* names[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                "Full moon", "Waning gibbous", "Last quarter", "Waning crescent"};
  return names[(int)floor(age / SYNODIC * 8 + 0.5) % 8];
}

// The moon as seen from the north: lit from the right while waxing, from the left while waning.
void drawMoon(int cx, int cy, int r, double age) {
  double k = cos(2 * PI * age / SYNODIC);         // 1 at new moon, -1 at full
  bool waxing = age < SYNODIC / 2;
  spr.fillCircle(cx, cy, r, 0x2124);
  for (int dy = -r; dy <= r; dy++) {
    float w = sqrtf(r * r - dy * dy);
    int x0 = waxing ? lroundf(w * k) : lroundf(-w);
    int x1 = waxing ? lroundf(w) : lroundf(-w * k);
    if (x1 > x0) spr.drawFastHLine(cx + x0, cy + dy, x1 - x0 + 1, 0xE71C);
  }
  spr.drawCircle(cx, cy, r, C_DIM);
}

String hm(int secs) { return String(secs / 3600) + "h " + String((secs % 3600) / 60) + "m"; }

void drawSky() {
  struct tm t;
  if (!haveLocalTime(t)) { drawWaiting(); return; }
  char buf[24];
  snprintf(buf, sizeof buf, "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
  wxHeader("SUN & MOON", buf);

  // The sun's arc from sunrise (left) to sunset (right), and where it is now.
  const int cx = 80, cy = 112, r = 62;
  int rise = wx.dRise[0], set = wx.dSet[0], now = t.tm_hour * 60 + t.tm_min;
  for (int a = 0; a <= 180; a += 6) {
    float rad = a * PI / 180;
    spr.fillCircle(cx - r * cosf(rad), cy - r * sinf(rad), 1, 0x4208);
  }
  spr.drawFastHLine(8, cy, 144, C_DIM);
  if (rise >= 0 && set > rise && now >= rise && now <= set) {
    float f = (float)(now - rise) / (set - rise);
    int sx = cx - r * cosf(PI * f), sy = cy - r * sinf(PI * f);
    spr.fillCircle(sx, sy, 9, C_SUN);
    spr.drawCircle(sx, sy, 12, C_SUN);
  } else {
    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(now < rise ? "before sunrise" : "after sunset", cx, cy - 26);
  }

  spr.setTextFont(4);
  spr.setTextColor(C_SUN, C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(hhmm(rise), 6, cy + 6);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(hhmm(set), 156, cy + 6);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("Day " + hm(wx.dDay[0]), 6, 150);
  if (wx.days > 1 && wx.dDay[1] && wx.dDay[0]) {
    int diff = (wx.dDay[1] - wx.dDay[0]) / 60;
    spr.setTextDatum(TR_DATUM);
    spr.drawString(String(diff >= 0 ? "+" : "") + String(diff) + "m tmrw", 156, 150);
  }
  spr.drawFastVLine(164, 26, 138, C_HDR);

  // The moon.
  double age = moonAge(time(nullptr));
  drawMoon(242, 64, 34, age);
  int lit = lroundf((1 - cos(2 * PI * age / SYNODIC)) / 2 * 100);
  spr.setTextDatum(TC_DATUM);
  spr.setTextFont(2);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(moonName(age), 242, 104);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(String(lit) + "% lit", 242, 122);
  double toFull = fmod(SYNODIC / 2 - age + SYNODIC, SYNODIC), toNew = SYNODIC - age;
  bool fullNext = toFull < toNew;
  int days = lround(fullNext ? toFull : toNew);
  spr.drawString(String(fullNext ? "Full" : "New") + (days ? " in " + String(days) + (days == 1 ? " day" : " days") : " today"), 242, 140);
}

// ---------- Alerts page ----------
void drawAlerts() {
  wxHeader("WEATHER ALERTS", loc.name);
  if (!alerts.checked) { centered("Checking weather.gov", 90); return; }
  if (!alerts.available) {
    centered("No alerts here", 76, 4);
    centered("weather.gov covers the US only", 108, 2);
    return;
  }
  if (!alerts.n) {
    centered("No active alerts", 76, 4, C_GOOD);
    centered("National Weather Service, checked every 5 min", 108, 2);
    return;
  }
  int rows = min(alerts.n, 4);
  for (int i = 0; i < rows; i++) {
    const Alert& a = alerts.a[i];
    int y = 24 + i * 34;
    uint16_t c = severityColor(a.rank);
    spr.fillRoundRect(4, y + 2, 6, 28, 2, c);
    spr.setTextDatum(TL_DATUM);
    spr.setTextFont(4);
    spr.setTextColor(c, C_BG);
    String ev = a.event + (a.count > 1 ? " x" + String(a.count) : "");
    if (spr.textWidth(ev, 4) > 300) spr.setTextFont(2);
    spr.drawString(ev, 16, y + 1);
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(a.severity + (a.until.length() ? "  -  until " + a.until : ""), 16, y + 24);
  }
  if (alerts.n > rows) {
    spr.setTextDatum(BR_DATUM);
    spr.setTextFont(1);
    spr.drawString("+" + String(alerts.n - rows) + " more", 314, 168);
  }
}

void render() {
  spr.fillSprite(C_BG);
  switch (page) {
    case 0: drawNow(); break;
    case 1: drawForecast(); break;
    case 2: drawSky(); break;
    default: drawAlerts(); break;
  }
  alertBorder();
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
      if (loc.name != before) alerts.checked = false;
      if (!wx.ok || loc.name != before) { lastTry = millis(); if (!fetchWeather()) status = "Weather failed, retrying"; }
    } else {
      status = "Location failed, retrying";
    }
    render();
  }

  if (up && loc.ok && (!alerts.checked || millis() - lastAlertTry > ALERT_EVERY_MS)) {
    lastAlertTry = millis();
    int before = alerts.n ? alerts.a[0].rank : -1;
    fetchAlerts();
    if (alerts.n && alerts.a[0].rank >= 3 && alerts.a[0].rank > before) page = 3;   // a new warning: show it
    render();
  }

  uint32_t due = wx.ok ? WX_EVERY_MS : WX_RETRY_MS;
  if (up && loc.ok && millis() - lastTry > due && (!wx.ok || millis() - wx.fetchedAt > due)) {
    lastTry = millis();
    if (!fetchWeather() && !wx.ok) status = "Weather failed, retrying";
  }

  static uint32_t lastDraw = 0;         // 4 times a second, for the flashing border
  if (millis() - lastDraw > 250) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) page = (page + 1) % NUM_PAGES;
  else { fetchWeather(); fetchAlerts(); }
  render();
}

}  // namespace weather
