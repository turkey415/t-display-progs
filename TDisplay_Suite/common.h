// Shared by every app: the screen, colours, settings, buttons and a few drawing helpers.
#pragma once

#include <WiFi.h>
#include <TFT_eSPI.h>
#include <time.h>

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
  int    app;         // which app runs at power-on
} cfg;

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

// ---------- Helpers ----------
void header(const String& left, const String& right, uint16_t leftCol = C_ACC, uint16_t rightCol = C_DIM) {
  spr.fillRect(0, 0, 320, 20, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(leftCol, C_HDR);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(left, 6, 2);
  spr.setTextColor(rightCol, C_HDR);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(right, 314, 2);
}

void centered(const String& msg, int y, int font = 4, uint16_t col = C_DIM) {
  spr.setTextFont(font);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(col, C_BG);
  spr.drawString(msg, 160, y);
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
