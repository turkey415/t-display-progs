/*
  Ham Shack Clock  --  LilyGO T-Display-S3
  ----------------------------------------
  Page 1: big UTC clock, local time, and SFI / SSN / A / K indices
  Page 2: HF band conditions (day / night), X-ray, geomag field, noise
  Solar data from hamqsl.com (N0NBH), refreshed every 30 minutes.

  Buttons:
    BTN_B (GPIO14) : switch page
    BTN_A (GPIO0)  : refresh solar data now

  Libraries: TFT_eSPI (Setup206_LilyGo_T_Display_S3 selected)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <time.h>

#define PIN_POWER_ON 15
#define PIN_LCD_BL   38
#define BTN_A        0
#define BTN_B        14

// ---------- Your settings ----------
const char* WIFI_SSID = "your-wifi";
const char* WIFI_PASS = "your-password";
const char* CALLSIGN  = "N0CALL";
const char* TZ_LOCAL  = "CST6CDT,M3.2.0,M11.1.0";   // US Central w/ DST
const char* SOLAR_URL = "https://www.hamqsl.com/solarxml.php";

const uint32_t SOLAR_EVERY_MS = 30UL * 60UL * 1000UL;
const uint32_t SOLAR_RETRY_MS = 2UL  * 60UL * 1000UL;

// ---------- Colors ----------
#define C_BG    0x0000
#define C_HDR   0x18E3
#define C_TEXT  0xFFFF
#define C_DIM   0x8410
#define C_ACC   0x5DDF
#define C_GOOD  0x07E0
#define C_FAIR  0xFFE0
#define C_POOR  0xF800

TFT_eSPI    tft;
TFT_eSprite spr(&tft);

struct Solar {
  String sfi, ssn, a, k, xray, geomag, noise, updated;
  String band[4], day[4], night[4];
  int    bands;
  bool   ok;
  uint32_t fetchedAt;
} solar;

int page = 0;

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
  if (WiFi.status() != WL_CONNECTED) return false;
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
  if (c.equalsIgnoreCase("Poor")) return C_POOR;
  return C_DIM;
}

uint16_t kColor(const String& k) {
  int v = k.toInt();
  if (k == "--") return C_DIM;
  return v <= 2 ? C_GOOD : v <= 4 ? C_FAIR : C_POOR;
}

void header(const char* right) {
  spr.fillRect(0, 0, 320, 20, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(C_ACC, C_HDR);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(CALLSIGN, 6, 2);
  spr.setTextColor(C_DIM, C_HDR);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(right, 314, 2);
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
  header(WiFi.status() == WL_CONNECTED ? "Wi-Fi OK" : "Wi-Fi DOWN");

  if (!synced) {
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("Syncing time...", 160, 70);
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

    strftime(buf, sizeof buf, "Local %H:%M %Z", &loc);
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
  header("HF CONDITIONS");
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("Band", 10, 24);
  spr.setTextDatum(TC_DATUM);
  spr.drawString("Day", 175, 24);
  spr.drawString("Night", 265, 24);

  if (!solar.ok || solar.bands == 0) {
    spr.setTextFont(4);
    spr.setTextDatum(MC_DATUM);
    spr.drawString("No data yet", 160, 85);
  }
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

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { render(); delay(250); }

  configTzTime(TZ_LOCAL, "pool.ntp.org", "time.nist.gov");
  fetchSolar();
}

void loop() {
  if (pressed(BTN_B)) { page ^= 1; render(); }
  if (pressed(BTN_A)) { fetchSolar(); render(); }

  uint32_t due = solar.ok ? SOLAR_EVERY_MS : SOLAR_RETRY_MS;
  static uint32_t lastTry = 0;
  if (millis() - lastTry > due && millis() - solar.fetchedAt > due) {
    lastTry = millis();
    fetchSolar();
  }

  static time_t lastSec = 0;
  time_t now = time(nullptr);
  if (now != lastSec) { lastSec = now; render(); }
  delay(20);
}
