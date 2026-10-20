// Shared by every app: the screen, colours, settings, buttons and a few drawing helpers.
#pragma once

#include <WiFi.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <Preferences.h>

// ---------- Board pins ----------
// Two boards share this firmware. The default is the LilyGO T-Display-S3. Building with
// -DBOARD_CYD (see tools/build.sh) makes it for the Freenove 2.8" "Cheap Yellow Display"
// (FNK0103B: classic ESP32, 240x320 ST7789, resistive touch). The CYD has no second key, so
// the apps keep their 320x170 screen and the 70 spare pixels become two touch keys.
#ifdef BOARD_CYD
#define BOARD_NAME   "CYD SUITE"
#define PIN_LCD_BL   21   // backlight
#define PIN_BAT      34   // battery voltage (through 1/2 divider)
#define BTN_A        0    // BOOT key
#define BTN_B        100  // no such pin: the touchscreen
#define PIN_TOUCH_IRQ 36  // low while the screen is touched (wakes it from power off)
#define HINT_SETUP   "Hold the screen for Wi-Fi setup"
#define HINT_WAKE    "Tap the screen to turn on"
#else
#define BOARD_NAME   "T-DISPLAY SUITE"
#define PIN_POWER_ON 15   // LCD + battery power enable (must be HIGH on battery)
#define PIN_LCD_BL   38   // backlight
#define PIN_BAT      4    // battery voltage (through 1/2 divider)
#define BTN_A        0    // BOOT key
#define BTN_B        14   // the other key
#define HINT_SETUP   "Hold the top key for Wi-Fi setup"
#define HINT_WAKE    "Press the TOP key to turn on"
#endif

// ---------- Colors (RGB565) ----------
#define C_BG     0x0000
// Very dark greys (grid lines, row dividers) go black in the CYD's 8-bit buffer; FAINT(c)
// keeps c on the T-Display and uses the darkest grey that survives on the CYD.
#ifdef BOARD_CYD
#define FAINT(c) 0x2108
#else
#define FAINT(c) (c)
#endif
#ifdef BOARD_CYD
#define C_HDR    0x2108   // the CYD's 8-bit screen buffer turns 0x18E3 black; this stays a dark blue-grey
#else
#define C_HDR    0x18E3
#endif
#define C_TEXT   0xFFFF
#define C_DIM    0x8410
#define C_ACC    0x5DDF
#define C_GOOD   0x07E0
#define C_WARN   0xFFE0
#define C_SLOW   0xFD20
#define C_BAD    0xF800

TFT_eSPI    tft;
TFT_eSprite spr(&tft);
// The screen buffer's colour depth. The CYD's classic ESP32 has no PSRAM: a 16-bit 320x170
// buffer (109 KB) leaves too little memory for HTTPS (Coinbase, GitHub updates), so it uses
// 8-bit colour (RGB332, 54 KB). Colours are rounded a little; very dark ones go black.
#ifdef BOARD_CYD
#define SPR_DEPTH 8
#else
#define SPR_DEPTH 16
#endif

// ---------- Settings (set from the Wi-Fi setup page, kept in flash) ----------
struct Settings {
  String server;      // GXL Event Server, e.g. http://10.0.0.5:8000
  String key;         // the event's agent key
  String callsign;
  String tz;          // POSIX time zone for the Ham Shack Clock's local time
  String hosts;       // LAN Sentinel: "Label=target,Label=target"; target "GW" = the gateway
  String shelly;      // Weather Clock: a Shelly's IP/host for a local temperature, "" = off
  bool   fahrenheit;
  bool   clock24;
  bool   autoUpdate;  // install new releases from GitHub by itself
  int    bright;      // backlight step, 0 = brightest
  String app;         // the app that runs at power-on, by name
} cfg;

// ---------- Backlight ----------
const uint8_t BRIGHT[] = {255, 150, 80, 35, 10};
const int     NUM_BRIGHT = sizeof(BRIGHT);

void applyBrightness() {
  static bool attached = false;
  if (!attached) { ledcAttach(PIN_LCD_BL, 5000, 8); attached = true; }
  ledcWrite(PIN_LCD_BL, BRIGHT[constrain(cfg.bright, 0, NUM_BRIGHT - 1)]);
}

void stepBrightness() {
  cfg.bright = (cfg.bright + 1) % NUM_BRIGHT;
  applyBrightness();
  Preferences p;
  p.begin("suite", false);
  p.putInt("bright", cfg.bright);
  p.end();
}

// Bumped each time Wi-Fi comes back after a drop, so apps can re-check things.
uint32_t wifiEpoch = 0;

// ---------- Screen orientation ----------
// Every app is landscape (320x170, USB on the left) except the ones that ask for portrait
// (170x320, USB at the bottom). The menu, setup and update screens are always landscape.
const int ROT_LANDSCAPE = 1;
const int ROT_PORTRAIT  = 0;    // USB at the bottom (2 puts it at the top)
bool portrait = false;

// ---------- Touch (CYD only) ----------
// The XPT2046 touch chip has its own pins, read by bit-banging. The CYD's only readable button
// is BOOT (the other one is RESET, wired to the chip), so the touchscreen stands in for the
// T-Display's top key: a tap anywhere = top key, holding = hold top. BOOT = the bottom key.
// The 70 pixels the 320x170 apps don't use hold a status bar (status::, at the end).
#ifdef BOARD_CYD
namespace touch {
const int T_CLK = 25, T_DIN = 32, T_DO = 39, T_CS = 33;
// Raw readings at the screen edges in landscape, from tapping four corners (2026-10-19).
// Screen x follows the raw Y channel, screen y the raw X channel.
const int RY_LEFT = 211, RY_RIGHT = 3716;   // raw Y at landscape x = 0 and x = 320
const int RX_TOP  = 307, RX_BOT   = 3948;   // raw X at landscape y = 0 and y = 240
const int Z_ON = 150;                        // pressure that counts as a touch

bool down = false;           // a finger is on the screen
int  x = 0, y = 0;           // where, in the screen's current rotation
bool tapped = false;         // a short tap on the canvas just ended, at tapX, tapY
int  tapX, tapY;
uint32_t downAt = 0;

void clk() { digitalWrite(T_CLK, HIGH); digitalWrite(T_CLK, LOW); }

// One 12-bit conversion. The commands keep power-down mode 00, so the chip's pen interrupt
// stays on between readings (power off relies on it).
int read(uint8_t cmd) {
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) { digitalWrite(T_DIN, (cmd >> i) & 1); clk(); }
  clk();
  int d = 0;
  for (int i = 11; i >= 0; i--) { d |= digitalRead(T_DO) << i; clk(); }
  clk(); clk(); clk();
  digitalWrite(T_CS, HIGH);
  return d;
}

void begin() {
  pinMode(T_CLK, OUTPUT); pinMode(T_DIN, OUTPUT); pinMode(T_CS, OUTPUT); pinMode(T_DO, INPUT);
  digitalWrite(T_CS, HIGH);
  pinMode(PIN_TOUCH_IRQ, INPUT);
  read(0x90);
}

void update() {
  static uint32_t last = 0;
  if (millis() - last < 8) return;
  last = millis();
  bool on = read(0xB0) > Z_ON;
  if (on) {
    int rx = 0, ry = 0;
    for (int i = 0; i < 4; i++) { rx += read(0xD0); ry += read(0x90); }
    rx /= 4; ry /= 4;
    on = read(0xB0) > Z_ON;          // still pressed, so the reading wasn't taken mid-lift
    int lx = constrain(map(ry, RY_LEFT, RY_RIGHT, 0, 320), 0, 319);
    int ly = constrain(map(rx, RX_TOP, RX_BOT, 0, 240), 0, 239);
    if (on) {
      if (portrait) { x = 239 - ly; y = lx; } else { x = lx; y = ly; }
    }
  }
  if (on == down) return;
  down = on;
  if (on) downAt = millis();
  else if (millis() - downAt < 600) { tapped = true; tapX = x; tapY = y; }
}

// A finished short tap, where it was in the current rotation (= sprite coordinates).
bool tap(int& px, int& py) {
  update();
  if (!tapped) return false;
  tapped = false;
  px = tapX; py = tapY;
  return true;
}

void forget() { tapped = false; }
}  // namespace touch
#endif

// Is a key held down? On the CYD the "top key" is the whole touchscreen and the bottom key
// is the BOOT button.
bool keyDown(int pin) {
#ifdef BOARD_CYD
  if (pin == BTN_B) { touch::update(); return touch::down; }
  return digitalRead(pin) == LOW;
#else
  return digitalRead(pin) == LOW;
#endif
}

// ---------- Buttons ----------
// A short press fires when the key is let go; holding fires LONG once and swallows the release.
enum Press { NONE, SHORT, LONG };
const uint32_t HOLD_MS = 900;

struct Button {
  int      pin;
  bool     down;
  bool     held;
  uint32_t changed, since;
};

Press poll(Button& b) {
  bool d = keyDown(b.pin);
  uint32_t now = millis();
  if (d != b.down && now - b.changed > 30) {
    b.changed = now;
    b.down = d;
    if (d) { b.since = now; b.held = false; return NONE; }
    return b.held ? NONE : SHORT;
  }
  if (b.down && !b.held && now - b.since > HOLD_MS) { b.held = true; return LONG; }
  return NONE;
}

#ifdef BOARD_CYD
namespace status { void draw(bool full); }
#endif

void setPortrait(bool p) {
  if (p == portrait) return;
  portrait = p;
  tft.setRotation(p ? ROT_PORTRAIT : ROT_LANDSCAPE);
  tft.fillScreen(C_BG);
  spr.deleteSprite();          // the same size either way, so the memory is reused at once
  spr.setColorDepth(SPR_DEPTH);
  spr.createSprite(p ? 170 : 320, p ? 320 : 170);
#ifdef BOARD_CYD
  status::draw(true);
#endif
}

// ---------- Battery ----------
// The battery sits behind a 1/2 divider on GPIO4 (PIN_POWER_ON must be HIGH to read it).
// Readings are smoothed, since Wi-Fi bursts make the voltage dip for a moment.
//
// The board has no pin that senses USB power, so onUSB() checks three things:
//  1. A computer is attached (HWCDC::isPlugged). Certain, but a power bank or wall charger
//     never talks to the board, so it only covers computers.
//  2. The pin reads over 4.3 V. That happens with no battery, or a full one the charger has
//     stopped feeding.
//  3. The voltage says a charger is feeding the cell: it jumps up when one is plugged in,
//     creeps up while charging, and drops when it is unplugged. With a battery in, a charger
//     holds the pin at 3.7-4.2 V, so check 2 alone misses it (and a weak USB port can keep
//     even a no-battery reading under 4.3 V).
const float BAT_USB_V   = 4.30f;
const float BAT_EMPTY_V = 3.30f;
const float STEP_UP_V   = 0.05f;   // a jump this big over the last minute = plugged in
const float STEP_DOWN_V = 0.08f;   // a drop this big = unplugged (Wi-Fi dips are shorter)
const float TREND_V     = 0.04f;   // a rise this big over 4 minutes = charging
float batV = 0;                    // smoothed volts, 0 until the first reading
float slowV = 0;                   // about a one-minute average, to spot jumps against
bool charging = false;             // check 3's answer

float batteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT);
  return mv / 8 * 2.0f / 1000.0f;
}

void updateBattery() {
  static uint32_t last = 0, lastTrend = 0;
  static float trend[8];           // one reading every 30 s: the last 4 minutes
  static int trendN = 0, upFor = 0, downFor = 0;
  if (batV && millis() - last < 1000) return;
  last = millis();
  float v = batteryVolts();
  // Jump straight there on the first reading or when it crosses the 4.3 V line.
  if (!batV || (v > BAT_USB_V) != (batV > BAT_USB_V)) {
    batV = slowV = v;
    trendN = 0;
  } else {
    batV += (v - batV) * 0.15f;
    slowV += (batV - slowV) * 0.02f;
  }

  // A jump up or down that lasts a few seconds.
  upFor   = batV - slowV > STEP_UP_V   ? upFor + 1   : 0;
  downFor = slowV - batV > STEP_DOWN_V ? downFor + 1 : 0;
  if (upFor >= 3)   { charging = true;  slowV = batV; trendN = 0; }
  if (downFor >= 3) { charging = false; slowV = batV; trendN = 0; }

  // The slow climb while charging (catches a charger that was in before power-on), and the
  // slow fall on battery.
  if (millis() - lastTrend >= 30000) {
    lastTrend = millis();
    if (trendN == 8) { memmove(trend, trend + 1, 7 * sizeof(float)); trendN = 7; }
    trend[trendN++] = batV;
    if (trendN == 8) {
      float lo = trend[0], hi = trend[0];
      for (int i = 1; i < 8; i++) { lo = min(lo, trend[i]); hi = max(hi, trend[i]); }
      if (batV - lo > TREND_V && batV >= hi - 0.01f) charging = true;
      if (hi - batV > TREND_V && batV <= lo + 0.01f) charging = false;
    }
  }
}

// Check 1. The CYD's USB goes through a CH340 chip, so the ESP32 can't tell.
bool computerPlugged() {
#ifdef BOARD_CYD
  return false;
#else
  return HWCDC::isPlugged();
#endif
}

bool onUSB() {
  updateBattery();
  return computerPlugged() || batV > BAT_USB_V || charging;
}

// A Li-ion cell's charge (0-100) from its voltage, at light load.
int batteryPct() {
  static const float V[] = {3.30, 3.50, 3.61, 3.69, 3.73, 3.77, 3.80, 3.84, 3.87, 3.95, 4.02, 4.11, 4.20};
  static const int   P[] = {0,    2,    5,    10,   20,   30,   40,   50,   60,   70,   80,   90,   100};
  const int N = sizeof(P) / sizeof(P[0]);
  updateBattery();
  if (batV <= V[0]) return 0;
  for (int i = 1; i < N; i++)
    if (batV < V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (batV - V[i - 1]) / (V[i] - V[i - 1]);
  return 100;
}

// The badge in the top-right corner: "87%" and a battery that empties, or a charging bolt on
// USB. It is BADGE_W wide; header() keeps its right-hand text clear of it.
const int BADGE_W = 52;

int hdrRight() { return spr.width() - BADGE_W - 4; }

void drawBattery(int y = 5) {
  int x = spr.width() - 23;                    // the battery body: 18 x 10, nub on the right
  bool usb = onUSB();
  int pct = batteryPct();
  uint16_t c = usb ? C_GOOD : pct > 50 ? C_GOOD : pct > 20 ? C_WARN : C_BAD;
  spr.fillRoundRect(x - 29, y - 2, 52, 14, 3, C_BG);   // keeps it readable on any background
  spr.drawRect(x, y, 18, 10, C_TEXT);
  spr.fillRect(x + 18, y + 3, 2, 4, C_TEXT);
  if (usb) {
    spr.fillTriangle(x + 10, y + 1, x + 5, y + 6, x + 9, y + 5, c);    // lightning bolt
    spr.fillTriangle(x + 8, y + 9, x + 13, y + 4, x + 9, y + 5, c);
  } else {
    spr.fillRect(x + 2, y + 2, max(1, 14 * pct / 100), 6, c);
  }
  spr.setTextFont(1);
  spr.setTextDatum(MR_DATUM);
  spr.setTextColor(usb ? C_DIM : c, C_BG);
  spr.drawString(usb ? "USB" : String(pct) + "%", x - 3, y + 5);
}

// Every screen goes out through here, so the battery shows everywhere.
// Big Clock's digits fill the top, so it asks for the badge lower down.
void show(int batteryY = 5) {
  drawBattery(batteryY);
  spr.pushSprite(0, 0);
}

// ---------- Helpers ----------
String fit(String s, int w, int font);

void header(const String& left, const String& right, uint16_t leftCol = C_ACC, uint16_t rightCol = C_DIM) {
  int lx = portrait ? 4 : 6;
  spr.fillRect(0, 0, spr.width(), 20, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(leftCol, C_HDR);
  spr.setTextDatum(TL_DATUM);
  int room = hdrRight() - lx - (right.length() ? spr.textWidth(right, 2) + 8 : 0);
  spr.drawString(fit(left, max(room, 40), 2), lx, 2);
  spr.setTextColor(rightCol, C_HDR);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(right, hdrRight(), 2);
}

void centered(const String& msg, int y, int font = 4, uint16_t col = C_DIM) {
  spr.setTextFont(font);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(col, C_BG);
  spr.drawString(msg, spr.width() / 2, y);
}

// Cuts a string with ".." so it fits in w pixels in the given font.
String fit(String s, int w, int font) {
  if (spr.textWidth(s, font) <= w) return s;
  while (s.length() > 1 && spr.textWidth(s + "..", font) > w) s.remove(s.length() - 1);
  return s + "..";
}

// Signal bars (0-4) for an RSSI, in a w x h box with its bottom-left corner at (x, y).
int bars(int rssi) { return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0; }

uint16_t rssiColor(int rssi) { return rssi >= -60 ? C_GOOD : rssi >= -70 ? C_WARN : rssi >= -80 ? C_SLOW : C_BAD; }

void drawBars(int x, int y, int rssi) {
  int n = bars(rssi);
  uint16_t c = rssiColor(rssi);
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    spr.fillRect(x + i * 3, y - h, 2, h, i < n ? c : 0x2945);
  }
}

// Wi-Fi security and cipher names, as the driver reports them.
const char* authName(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "Open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "Other";
  }
}

const char* cipherName(int c) {
  switch (c) {
    case WIFI_CIPHER_TYPE_NONE:      return "none";
    case WIFI_CIPHER_TYPE_WEP40:     return "WEP-40";
    case WIFI_CIPHER_TYPE_WEP104:    return "WEP-104";
    case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:      return "AES (CCMP)";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP + AES";
    case WIFI_CIPHER_TYPE_GCMP:      return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256:   return "GCMP-256";
    default:                         return "other";
  }
}

String fmtDur(uint32_t s) {
  if (s < 60)    return String(s) + "s";
  if (s < 3600)  return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h" + String((s % 3600) / 60) + "m";
  return String(s / 86400) + "d" + String((s % 86400) / 3600) + "h";
}


bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

// Each app: start (when picked from the menu or at power-on), run (every pass of loop),
// and the short presses of the two keys. Holding a key is taken by the menu and setup.
struct App {
  const char* name;
  const char* blurb;
  void (*start)();
  void (*run)();
  void (*press)(int pin);
};

// ---------- Status bar (CYD only) ----------
// In the 70 pixels the apps leave free: below them in landscape, to their right in portrait.
// Drawn straight to the screen (the sprite never covers it), changed parts once a second.
#ifdef BOARD_CYD
namespace status {
const uint16_t C_LINE = 0x2124;

String clockText(const tm& t) {
  char b[8];
  if (cfg.clock24) strftime(b, sizeof b, "%H:%M", &t);
  else { strftime(b, sizeof b, "%I:%M", &t); if (b[0] == '0') memmove(b, b + 1, strlen(b)); }
  return b;
}

// Draws text with a background box as wide as `pad`, so a shorter value wipes the old one.
void text(const String& s, int x, int y, int font, uint16_t col, uint8_t datum, int pad) {
  tft.setTextDatum(datum);
  tft.setTextColor(col, C_BG);
  tft.setTextPadding(pad);
  tft.drawString(s, x, y, font);
  tft.setTextPadding(0);
}

void wifiBars(int x, int y, int rssi, bool up) {        // 4 bars, bottom-left at (x, y)
  int n = up ? bars(rssi) : 0;
  uint16_t c = up ? rssiColor(rssi) : C_BAD;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 4;
    tft.fillRect(x + i * 5, y - 16, 3, 16 - h, C_BG);
    tft.fillRect(x + i * 5, y - h, 3, h, i < n ? c : 0x2945);
  }
}

String batText() {
  return onUSB() ? String("USB power") : "Battery " + String(batteryPct()) + "%";
}
uint16_t batColor() {
  int p = batteryPct();
  return onUSB() ? C_DIM : p > 50 ? C_GOOD : p > 20 ? C_WARN : C_BAD;
}

void draw(bool full) {
  static uint32_t last = 0;
  if (!full && millis() - last < 1000) return;
  last = millis();
  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  bool synced = now > 1700000000;
  bool up = wifiUp();
  int rssi = up ? WiFi.RSSI() : 0;
  char date[16];
  strftime(date, sizeof date, "%a %b %e", &t);

  if (!portrait) {
    if (full) { tft.fillRect(0, 170, 320, 70, C_BG); tft.drawFastHLine(0, 171, 320, C_LINE); }
    text(synced ? clockText(t) : String("--:--"), 8, 174, 6, C_TEXT, TL_DATUM, 130);
    text(synced ? String(date) : String(""), 10, 231, 2, C_DIM, ML_DATUM, 120);
    wifiBars(296, 196, rssi, up);
    text(up ? String(rssi) : String(""), 290, 189, 2, up ? rssiColor(rssi) : C_BAD, MR_DATUM, 34);
    text(up ? WiFi.SSID() : String("Wi-Fi down"), 312, 206, 2, up ? C_TEXT : C_BAD, MR_DATUM, 170);
    text(up ? WiFi.localIP().toString() : String(""), 312, 222, 2, C_DIM, MR_DATUM, 170);
    text(batText(), 250, 189, 2, batColor(), MR_DATUM, 100);
  } else {
    if (full) { tft.fillRect(170, 0, 70, 320, C_BG); tft.drawFastVLine(171, 0, 320, C_LINE); }
    text(synced ? clockText(t) : String("--:--"), 206, 20, 4, C_TEXT, MC_DATUM, 66);
    strftime(date, sizeof date, "%b %e", &t);
    text(synced ? String(date) : String(""), 206, 44, 2, C_DIM, MC_DATUM, 66);
    wifiBars(196, 90, rssi, up);
    text(up ? String(rssi) + " dBm" : String("no Wi-Fi"), 206, 102, 1, up ? rssiColor(rssi) : C_BAD, MC_DATUM, 66);
    text(onUSB() ? String("USB") : String(batteryPct()) + "%", 206, 130, 2, batColor(), MC_DATUM, 66);
  }
}
}  // namespace status
#endif
