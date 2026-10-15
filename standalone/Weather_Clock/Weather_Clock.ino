/*
  Weather Clock  --  LilyGO T-Display-S3
  --------------------------------------
  Page 1: local time, current conditions, feels-like / humidity / wind / hi-lo
  Page 2: 5-day forecast
  Location is looked up from your public IP (ip-api.com) at boot, every hour,
  and whenever Wi-Fi reconnects, so it follows the clock around.
  Weather from Open-Meteo (no API key), every 15 min. The local time zone also
  comes from Open-Meteo, so DST and travel across zones are automatic.

  Wi-Fi: tries the last network set up from a phone, then the built-in home
  network. If neither is in range it opens the "WeatherClock-Setup" hotspot:
  join it from a phone and pick the local Wi-Fi. Losing Wi-Fi for 2 minutes
  restarts the clock and goes through those steps again.

  Buttons:
    BTN_B (GPIO14) : switch page  (hold while powering on: open Wi-Fi setup)
    BTN_A (GPIO0)  : refresh weather now

  Libraries: TFT_eSPI (Setup206_LilyGo_T_Display_S3 selected), ArduinoJson 7,
             WiFiManager (tzapu)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#define PIN_POWER_ON 15
#define PIN_LCD_BL   38
#define BTN_A        0
#define BTN_B        14

// ---------- Your settings ----------
const char* HOME_SSID  = "your-wifi";
const char* HOME_PASS  = "your-password";
const char* AP_NAME    = "WeatherClock-Setup";
const bool  FAHRENHEIT = true;      // false = Celsius + km/h
const bool  CLOCK_24H  = true;

// Location comes from your public IP. To pin it instead (e.g. behind a VPN),
// set both coordinates and a display name.
const float FIXED_LAT  = NAN;
const float FIXED_LON  = NAN;
const char* FIXED_NAME = "";

const uint32_t WX_EVERY_MS  = 15UL * 60UL * 1000UL;
const uint32_t WX_RETRY_MS  = 60UL * 1000UL;
const uint32_t LOC_RETRY_MS = 60UL * 1000UL;
const uint32_t LOC_EVERY_MS = 60UL * 60UL * 1000UL;
const uint32_t WIFI_LOST_MS = 2UL * 60UL * 1000UL;
const uint32_t PORTAL_SECS  = 300;

// ---------- Colors ----------
#define C_BG     0x0000
#define C_HDR    0x18E3
#define C_TEXT   0xFFFF
#define C_DIM    0x8410
#define C_ACC    0x5DDF
#define C_SUN    0xFE60
#define C_CLOUD  0xBDF7
#define C_STORM  0x7BEF
#define C_RAIN   0x3D9F
#define C_BAD    0xF800

TFT_eSPI    tft;
TFT_eSprite spr(&tft);

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
} wx;

int    page = 0;
String status = "Connecting to Wi-Fi";

const char* WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* MON[]  = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ---------- Helpers ----------
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

bool pressed(int pin) {
  static bool last[48];
  static uint32_t t[48];
  bool down = digitalRead(pin) == LOW, fire = false;
  if (down != last[pin] && millis() - t[pin] > 40) {
    t[pin] = millis();
    if (down) fire = true;
    last[pin] = down;
  }
  return fire;
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
  if (WiFi.status() != WL_CONNECTED || !loc.ok) return false;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(loc.lat, 3) +
               "&longitude=" + String(loc.lon, 3) +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
               "weather_code,wind_speed_10m,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
               "precipitation_probability_max"
               "&timezone=auto&forecast_days=5";
  if (FAHRENHEIT) url += "&temperature_unit=fahrenheit&wind_speed_unit=mph";

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
void drawTemp(int cx, int y, float v, int font, uint16_t col, bool unit) {
  String num = String((int)lroundf(v));
  String u = unit ? (FAHRENHEIT ? "F" : "C") : "";
  int h = spr.fontHeight(font);
  int r = max(2, h / 12);
  int wNum = spr.textWidth(num, font);
  int w = wNum + 2 * r + 3 + (u.length() ? spr.textWidth(u, font) : 0);
  int x = cx - w / 2;

  spr.setTextFont(font);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(col, C_BG);
  spr.drawString(num, x, y);
  x += wNum + r + 2;
  spr.drawCircle(x, y + r + 1, r, col);
  if (r > 2) spr.drawCircle(x, y + r + 1, r - 1, col);
  x += r + 2;
  if (u.length()) spr.drawString(u, x, y);
}

void header(const String& left, const String& right, uint16_t rightCol = C_DIM) {
  spr.fillRect(0, 0, 320, 20, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(C_ACC, C_HDR);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(left, 6, 2);
  spr.setTextColor(rightCol, C_HDR);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(right, 314, 2);
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
  header("WEATHER CLOCK", WiFi.status() == WL_CONNECTED ? "" : "Wi-Fi DOWN", C_BAD);
  spr.setTextFont(4);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(status, 160, 90);
}

void drawNow() {
  struct tm t;
  if (!haveLocalTime(t)) { drawWaiting(); return; }

  char buf[32];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof buf, "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
    header(loc.name, buf);
  } else {
    header(loc.name, "Wi-Fi DOWN", C_BAD);
  }

  // Time
  int hr = t.tm_hour;
  if (!CLOCK_24H) { hr %= 12; if (hr == 0) hr = 12; }
  snprintf(buf, sizeof buf, CLOCK_24H ? "%02d:%02d" : "%d:%02d", hr, t.tm_min);
  spr.setTextFont(7);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(buf, 6, 28);
  int x = 6 + spr.textWidth(buf, 7) + 6;
  snprintf(buf, sizeof buf, "%02d", t.tm_sec);
  spr.setTextFont(4);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(buf, x, 28);
  if (!CLOCK_24H) {
    spr.setTextFont(2);
    spr.setTextColor(C_ACC, C_BG);
    spr.drawString(t.tm_hour < 12 ? "AM" : "PM", x + 2, 58);
  }

  // Conditions
  spr.setTextFont(4);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(wxText(wx.code), 6, 88);

  drawIcon(268, 44, 1.0f, wx.code, wx.isDay);
  drawTemp(268, 72, wx.temp, 6, C_TEXT, false);

  // Stats
  String spd = FAHRENHEIT ? "mph" : "km/h";
  statBox(0,   "Feels",  String((int)lroundf(wx.feels)));
  statBox(80,  "Humid",  String(wx.hum) + "%");
  statBox(160, "Wind",   String((int)lroundf(wx.wind)) + (wx.wind < 100 ? spd.substring(0, 1) : ""));
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

    drawTemp(cx, 92, wx.dHi[i], 4, C_TEXT, false);
    drawTemp(cx, 120, wx.dLo[i], 2, C_DIM, false);

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

// ---------- Wi-Fi ----------
WiFiManager wm;
uint32_t    lastLoc = 0;

bool waitConnect(uint32_t ms) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) { render(); delay(250); }
  return WiFi.status() == WL_CONNECTED;
}

void showSetup() {
  spr.fillSprite(C_BG);
  header("WI-FI SETUP", "");
  spr.setTextDatum(TC_DATUM);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("On your phone, join this Wi-Fi:", 160, 28);
  spr.setTextFont(4);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(AP_NAME, 160, 48);
  spr.setTextFont(2);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString("A setup page opens (or go to 192.168.4.1)", 160, 86);
  spr.drawString("Pick the local Wi-Fi and enter its password", 160, 104);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("Retries in 5 min if nothing is set", 160, 142);
  spr.pushSprite(0, 0);
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  bool forceSetup = digitalRead(BTN_B) == LOW;     // top key held at power-up

  if (!forceSetup) {
    if (wm.getWiFiIsSaved()) {
      status = "Joining " + wm.getWiFiSSID(true);
      WiFi.begin();
      if (waitConnect(15000)) return;
    }
    if (strlen(HOME_SSID) && strcmp(HOME_SSID, "your-wifi") != 0) {
      WiFi.persistent(false);                      // keep the phone-set network saved
      status = "Joining " + String(HOME_SSID);
      WiFi.begin(HOME_SSID, HOME_PASS);
      bool ok = waitConnect(15000);
      WiFi.persistent(true);
      if (ok) return;
      WiFi.disconnect();
    }
  }

  showSetup();
  wm.setConfigPortalTimeout(PORTAL_SECS);
  if (!wm.startConfigPortal(AP_NAME)) ESP.restart();
  WiFi.mode(WIFI_STA);
}

// ---------- Main ----------
void setup() {
  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL, OUTPUT);   digitalWrite(PIN_LCD_BL, HIGH);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  spr.createSprite(320, 170);

  connectWifi();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC; offset applied from Open-Meteo

  status = "Finding location";
  render();
  if (getLocation()) {
    status = "Getting weather";
    render();
    if (!fetchWeather()) status = "Weather failed, retrying";
  } else {
    status = "Location failed, retrying";
  }
  lastLoc = millis();
  render();
}

void loop() {
  if (pressed(BTN_B)) { page ^= 1; render(); }
  if (pressed(BTN_A)) { fetchWeather(); render(); }

  // Wi-Fi watchdog: a long outage usually means the clock was moved, so
  // restart and run the saved / home / phone-setup sequence again.
  static bool     wasUp = true, recheck = false;
  static uint32_t downSince = 0;
  bool up = WiFi.status() == WL_CONNECTED;
  if (!up) {
    if (wasUp) downSince = millis();
    if (millis() - downSince > WIFI_LOST_MS) ESP.restart();
  } else if (!wasUp) {
    recheck = true;
  }
  wasUp = up;

  // Re-check location hourly and after any reconnect; refetch weather if it moved.
  static uint32_t lastTry = 0;
  uint32_t locDue = loc.ok ? LOC_EVERY_MS : LOC_RETRY_MS;
  if (up && (recheck || millis() - lastLoc > locDue)) {
    lastLoc = millis();
    recheck = false;
    String before = loc.name;
    if (getLocation()) {
      status = "Getting weather";
      if (!wx.ok || loc.name != before) { lastTry = millis(); fetchWeather(); }
    }
  }

  uint32_t due = wx.ok ? WX_EVERY_MS : WX_RETRY_MS;
  if (loc.ok && millis() - lastTry > due && (!wx.ok || millis() - wx.fetchedAt > due)) {
    lastTry = millis();
    if (!fetchWeather() && !wx.ok) status = "Weather failed, retrying";
  }

  static time_t lastSec = 0;
  time_t now = time(nullptr);
  if (now != lastSec) { lastSec = now; render(); }
  delay(20);
}
