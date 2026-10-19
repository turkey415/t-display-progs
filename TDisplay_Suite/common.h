// Shared by every app: the screen, colours, settings, buttons and a few drawing helpers.
#pragma once

#include <WiFi.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <Preferences.h>

// ---------- Board pins ----------
#define PIN_POWER_ON 15   // LCD + battery power enable (must be HIGH on battery)
#define PIN_LCD_BL   38   // backlight
#define PIN_BAT      4    // battery voltage (through 1/2 divider)
#define BTN_A        0    // BOOT key
#define BTN_B        14   // the other key

// ---------- Colors (RGB565) ----------
#define C_BG     0x0000
#define C_HDR    0x18E3
#define C_TEXT   0xFFFF
#define C_DIM    0x8410
#define C_ACC    0x5DDF
#define C_GOOD   0x07E0
#define C_WARN   0xFFE0
#define C_SLOW   0xFD20
#define C_BAD    0xF800

TFT_eSPI    tft;
TFT_eSprite spr(&tft);

// ---------- Settings (set from the Wi-Fi setup page, kept in flash) ----------
struct Settings {
  String server;      // GXL Event Server, e.g. http://10.0.0.5:8000
  String key;         // the event's agent key
  String callsign;
  String tz;          // POSIX time zone for the Ham Shack Clock's local time
  String hosts;       // LAN Sentinel: "Label=target,Label=target"; target "GW" = the gateway
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
  bool d = digitalRead(b.pin) == LOW;
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

// ---------- Screen orientation ----------
// Every app is landscape (320x170, USB on the left) except the ones that ask for portrait
// (170x320, USB at the bottom). The menu, setup and update screens are always landscape.
const int ROT_LANDSCAPE = 1;
const int ROT_PORTRAIT  = 0;    // USB at the bottom (2 puts it at the top)
bool portrait = false;

void setPortrait(bool p) {
  if (p == portrait) return;
  portrait = p;
  tft.setRotation(p ? ROT_PORTRAIT : ROT_LANDSCAPE);
  tft.fillScreen(C_BG);
  spr.deleteSprite();
  spr.createSprite(p ? 170 : 320, p ? 320 : 170);
}

// ---------- Battery ----------
// The battery sits behind a 1/2 divider on GPIO4 (PIN_POWER_ON must be HIGH to read it).
// Readings are smoothed, since Wi-Fi bursts make the voltage dip for a moment.
// On USB the pin reads the charger's output, above a full cell, so "over 4.3 V" means USB.
const float BAT_USB_V   = 4.30f;
const float BAT_EMPTY_V = 3.30f;
float batV = 0;                 // smoothed volts, 0 until the first reading

float batteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT);
  return mv / 8 * 2.0f / 1000.0f;
}

void updateBattery() {
  static uint32_t last = 0;
  if (batV && millis() - last < 1000) return;
  last = millis();
  float v = batteryVolts();
  // Jump straight there on the first reading or when USB goes in or out.
  if (!batV || (v > BAT_USB_V) != (batV > BAT_USB_V)) batV = v;
  else batV += (v - batV) * 0.15f;
}

bool onUSB() { updateBattery(); return batV > BAT_USB_V; }

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
