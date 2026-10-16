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
const int ROT_PORTRAIT  = 2;    // 0 would put the USB at the top
bool portrait = false;

void setPortrait(bool p) {
  if (p == portrait) return;
  portrait = p;
  tft.setRotation(p ? ROT_PORTRAIT : ROT_LANDSCAPE);
  tft.fillScreen(C_BG);
  spr.deleteSprite();
  spr.createSprite(p ? 170 : 320, p ? 320 : 170);
}

// ---------- Helpers ----------
void header(const String& left, const String& right, uint16_t leftCol = C_ACC, uint16_t rightCol = C_DIM) {
  spr.fillRect(0, 0, spr.width(), 20, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(leftCol, C_HDR);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(left, portrait ? 4 : 6, 2);
  spr.setTextColor(rightCol, C_HDR);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(right, spr.width() - (portrait ? 4 : 6), 2);
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

float batteryVolts() {
  return analogReadMilliVolts(PIN_BAT) * 2.0f / 1000.0f;
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
