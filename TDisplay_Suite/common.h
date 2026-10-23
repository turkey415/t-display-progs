// Shared by every app: the screen, colours, settings, buttons and a few drawing helpers.
#pragma once

#include <WiFi.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <Preferences.h>
#include <Wire.h>

// ---------- Board pins ----------
// Two boards share this firmware. The default is the LilyGO T-Display-S3. Building with
// -DBOARD_CYD (see tools/build.sh) makes it for the Freenove 2.8" "Cheap Yellow Display"
// (FNK0103B: classic ESP32, 240x320 ST7789, resistive touch). The CYD has no second key, so
// the apps keep their 320x170 screen and the 70 spare pixels become two touch keys.
#if defined(BOARD_CYD35)
// -DBOARD_CYD35 (with BOARD_CYD) is the 3.5" ESP32-3248S035C: 480x320 ST7796, GT911
// capacitive touch, no battery input. The apps' 320x170 canvas is drawn 1.5x bigger, so it
// fills the screen above a status bar, and an app can ask for a native 480x320 canvas
// (setNative). See pushScaled().
#define BOARD_NAME   "CYD SUITE"
#define PIN_LCD_BL   27   // backlight
#define BTN_A        0    // BOOT key
#define BTN_B        100  // no such pin: the touchscreen
#define NO_BATTERY        // GPIO34 is a light sensor here
#define HINT_SETUP   "Hold the screen for Wi-Fi setup"
#define HINT_WAKE    "Press BOOT to turn on"
const int LED_PINS[] = {4, 16, 17};   // RGB LED, active low
#elif defined(BOARD_CYD)
#define BOARD_NAME   "CYD SUITE"
#define PIN_LCD_BL   21   // backlight
#define PIN_BAT      34   // battery voltage (through 1/2 divider)
#define BTN_A        0    // BOOT key
#define BTN_B        100  // no such pin: the touchscreen
#define PIN_TOUCH_IRQ 36  // low while the screen is touched (wakes it from power off)
#define HINT_SETUP   "Hold the screen for Wi-Fi setup"
#define HINT_WAKE    "Tap the screen to turn on"
const int LED_PINS[] = {22, 16, 17};  // RGB LED, active low
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
  String switches;    // Port Map: "Name=IP,IP" switches to read over SNMP
  String community;   // Port Map: SNMP v2c read community
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

void screenRotation(int r) {
  tft.setRotation(r);
}

#ifdef BOARD_CYD35
// The 3.5" screen shows the canvas scaled by layNum/layDen with its corner at (layX, layY)
// (set by applyLayout), or 1:1 when an app asked for the native 480x320 canvas.
const int SCR_W = 480, SCR_H = 320, SB = 65;    // SB: the status bar's depth
int layNum = 480, layDen = 320, layX = 0, layY = 0;
#endif

// ---------- Touch (CYD only) ----------
// The XPT2046 touch chip has its own pins, read by bit-banging. The CYD's only readable button
// is BOOT (the other one is RESET, wired to the chip), so the touchscreen stands in for the
// T-Display's top key: a tap anywhere = top key, holding = hold top. BOOT = the bottom key.
// The 70 pixels the 320x170 apps don't use hold a status bar (status::, at the end).
#ifdef BOARD_CYD
namespace touch {
bool down = false;           // a finger is on the screen
int  x = 0, y = 0;           // where, in the screen's current rotation
bool tapped = false;         // a short tap on the canvas just ended, at tapX, tapY
int  tapX, tapY;
uint32_t downAt = 0;

void setDown(bool on) {
  if (on == down) return;
  down = on;
  if (on) downAt = millis();
  else if (millis() - downAt < 600) { tapped = true; tapX = x; tapY = y; }
}

#ifdef BOARD_CYD35
// GT911 capacitive touch on I2C, in its portrait frame: landscape screen x follows its raw Y,
// and screen y its raw X, reversed. From corner and port taps (2026-10-22): raw y 20..460 =
// screen x 15..465, raw x 300..10 = screen y 15..305.
// x, y end up in canvas coordinates (the status bar maps to just past the canvas, as on the
// 2.8" CYD).
const int T_SDA = 33, T_SCL = 32, T_INT = 21, T_RST = 25;
const uint8_t ADDR = 0x5D;

bool rd(uint16_t reg, uint8_t* b, int n) {
  Wire.beginTransmission(ADDR); Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(ADDR, (uint8_t)n) != n) return false;
  for (int i = 0; i < n; i++) b[i] = Wire.read();
  return true;
}

void clearStatus() {
  Wire.beginTransmission(ADDR); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission();
}

void begin() {
  // INT held low through reset picks address 0x5D
  pinMode(T_INT, OUTPUT); pinMode(T_RST, OUTPUT);
  digitalWrite(T_INT, LOW); digitalWrite(T_RST, LOW); delay(20);
  digitalWrite(T_RST, HIGH); delay(10);
  pinMode(T_INT, INPUT); delay(60);
  Wire.begin(T_SDA, T_SCL, 400000);
}

void update() {
  static uint32_t last = 0, lastSeen = 0;
  if (millis() - last < 8) return;
  last = millis();
  uint8_t st;
  bool on = down;
  if (rd(0x814E, &st, 1) && (st & 0x80)) {
    on = (st & 0x0F) > 0;
    if (on) {
      uint8_t p[4];
      if (rd(0x8150, p, 4)) {
        int rx = p[0] | p[1] << 8, ry = p[2] | p[3] << 8;
        int lx = constrain(map(ry, 20, 460, 15, 465), 0, SCR_W - 1);
        int ly = constrain(map(rx, 300, 10, 15, 305), 0, SCR_H - 1);
        int px = portrait ? SCR_H - 1 - ly : lx, py = portrait ? lx : ly;
        x = max(0, px - layX) * layDen / layNum;
        y = max(0, py - layY) * layDen / layNum;
      }
      lastSeen = millis();
    }
    clearStatus();
  } else if (down && millis() - lastSeen > 250) {
    on = false;                          // missed the release report
  }
  setDown(on);
}
#else
const int T_CLK = 25, T_DIN = 32, T_DO = 39, T_CS = 33;
// Raw readings at the screen edges in landscape, from tapping four corners (2026-10-19).
// Screen x follows the raw Y channel, screen y the raw X channel.
const int RY_LEFT = 211, RY_RIGHT = 3716;   // raw Y at landscape x = 0 and x = 320
const int RX_TOP  = 307, RX_BOT   = 3948;   // raw X at landscape y = 0 and y = 240
const int Z_ON = 150;                        // pressure that counts as a touch


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
  setDown(on);
}

#endif

// A finished short tap, where it was in the current rotation (= sprite coordinates).
bool tap(int& px, int& py) {
  update();
  if (!tapped) return false;
  tapped = false;
  px = tapX; py = tapY;
  return true;
}

void forget() { tapped = false; }

// The tap behind this pass's top-key press, if it was one (set by the main loop), so an app's
// press() can tell where on the screen it landed.
bool pressTap = false;
int  pressX = 0, pressY = 0;
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

// Set by the main loop for one pass after something outside the app (the CYD's status bar
// sliding away) uncovered or resized its screen: apps that redraw on a timer redraw at once.
bool redrawNow = false;

#ifdef BOARD_CYD
// Apps draw on a "canvas": 320x170 (landscape) or 170x320 (portrait), the size of the screen
// buffer. An app can ask for more of the CYD's 320x240:
//   setFullScreen() : the whole 320x240, no status bar (Precision Clock)
//   allowWide()     : in portrait, a tap on the status bar slides it away and the canvas
//                     widens to 240 (mDNS Browser); a tap on the right edge brings it back
// A buffer for the whole screen (77 KB) leaves too little memory for HTTPS, so a bigger canvas
// is drawn in two strips: frame() runs the app's drawing twice, shifted, and show() sends each
// strip to its place. Apps just draw in canvas coordinates; anything off the strip is clipped.
bool fullScreen = false, wideOK = false, statusHidden = false;
bool native = false;                   // 3.5" CYD: the app draws at 480x320 (setNative)
bool sprNative = false;                // the screen buffer is the native strip
int  canvasW = 320, canvasH = 170;
int  band = 0;                         // which strip frame() is drawing
namespace status { void draw(bool full); bool visible(); }

void applyLayout() {
  int cw = portrait ? (wideOK && statusHidden ? 240 : 170) : 320;
  int ch = portrait ? 320 : (fullScreen ? 240 : 170);
#ifdef BOARD_CYD35
  if (native) { cw = SCR_W; ch = SCR_H; }
#endif
  if (cw == canvasW && ch == canvasH) return;
  canvasW = cw; canvasH = ch;
#ifdef BOARD_CYD35
  // The biggest scale that fits beside the status bar, centred.
  int sw = portrait ? SCR_H : SCR_W, sh = portrait ? SCR_W : SCR_H;
  bool bar = status::visible();
  int aw = sw - (bar && portrait ? SB : 0), ah = sh - (bar && !portrait ? SB : 0);
  if (aw * ch <= ah * cw) { layNum = aw; layDen = cw; } else { layNum = ah; layDen = ch; }
  layX = (aw - cw * layNum / layDen) / 2;
  layY = (ah - ch * layNum / layDen) / 2;
  tft.fillScreen(C_BG);
#endif
  status::draw(true);                   // covers whatever the canvas gave up
}

void setFullScreen() { fullScreen = true; applyLayout(); }
void allowWide()     { wideOK = true;     applyLayout(); }
int  canvasWidth()   { return canvasW; }
int  canvasHeight()  { return canvasH; }

void setPortrait(bool p);
#ifdef BOARD_CYD35
// Gives the app a 480x320 canvas in full 16-bit colour, landscape, no status bar. It is drawn
// in 56-pixel strips (480x56x2 bytes fits where the 8-bit 320x170 buffer was).
const int NATIVE_STRIP = 56;
void setNative() {
  setPortrait(false);
  native = true;
  spr.deleteSprite();
  spr.setColorDepth(16);
  spr.createSprite(SCR_W, NATIVE_STRIP);
  sprNative = true;
  canvasW = canvasH = 0;
  applyLayout();
}
#else
void setNative() { setFullScreen(); }   // the 2.8" CYD: its whole 320x240
#endif

// Draws a frame with draw(), which ends with show(), in one or more strips.
void frame(void (*draw)()) {
#ifdef BOARD_CYD35
  if (sprNative) {
    for (band = 0; band * NATIVE_STRIP < canvasH; band++) {
      spr.setOrigin(0, -NATIVE_STRIP * band);
      draw();
    }
    band = 0;
    spr.setOrigin(0, 0);
    return;
  }
#endif
  int bands = canvasW > spr.width() || canvasH > spr.height() ? 2 : 1;
  for (band = 0; band < bands; band++) {
    if (portrait) spr.setOrigin(-170 * band, 0); else spr.setOrigin(0, -170 * band);
    draw();
  }
  band = 0;
  spr.setOrigin(0, 0);
}
#else
void setFullScreen() {}
void allowWide() {}
int  canvasWidth()  { return spr.width(); }
int  canvasHeight() { return spr.height(); }
void frame(void (*draw)()) { draw(); }
void setNative() {}
#endif

void setPortrait(bool p) {
#ifdef BOARD_CYD
  if (p == portrait && !sprNative) { applyLayout(); return; }
#else
  if (p == portrait) return;
#endif
  portrait = p;
  screenRotation(p ? ROT_PORTRAIT : ROT_LANDSCAPE);
  tft.fillScreen(C_BG);
  spr.deleteSprite();          // the same size either way, so the memory is reused at once
  spr.setColorDepth(SPR_DEPTH);
  spr.createSprite(p ? 170 : 320, p ? 320 : 170);
#ifdef BOARD_CYD
  sprNative = native = false;            // setNative() turns it back on after this
  canvasW = canvasH = 0;
  applyLayout();
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
#ifdef NO_BATTERY
  return 5.0f;                     // always "on USB"
#else
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT);
  return mv / 8 * 2.0f / 1000.0f;
#endif
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

#ifdef NO_BATTERY
int hdrRight() { return canvasWidth() - 6; }
#else
int hdrRight() { return canvasWidth() - BADGE_W - 4; }
#endif

void drawBattery(int y = 5) {
#ifdef NO_BATTERY
  return;                                        // always on USB: nothing worth showing
#endif
  int x = canvasWidth() - 23;                    // the battery body: 18 x 10, nub on the right
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
uint32_t pushUs = 0;             // how long the last frame (its first strip) took to reach the screen

#ifdef BOARD_CYD35
// Sends a strip of the 8-bit canvas to the screen, scaled by layNum/layDen (nearest pixel):
// (bx, by, bw, bh) is the part of the canvas the screen buffer holds.
void pushScaled(int bx, int by, int bw, int bh) {
  static uint16_t lut[256];                      // RGB332 -> RGB565 as TFT_eSPI does it, bytes ready to send
  static bool lutReady = false;
  if (!lutReady) {
    const uint8_t blue[] = {0, 11, 21, 31};
    for (int c = 0; c < 256; c++) {
      uint8_t msb = (c & 0x1C) >> 2 | (c & 0xC0) >> 3 | (c & 0xE0);
      uint8_t lsb = (c & 0x1C) << 3 | blue[c & 0x03];
      lut[c] = lsb << 8 | msb;
    }
    lutReady = true;
  }
  const int N = layNum, D = layDen;
  int x0 = (bx * N + D - 1) / D, x1 = ((bx + bw) * N + D - 1) / D;   // screen pixels whose source is in the strip
  int y0 = (by * N + D - 1) / D, y1 = ((by + bh) * N + D - 1) / D;
  int w = x1 - x0;
  if (w <= 0 || y1 <= y0) return;
  static uint16_t line[SCR_W], srcX[SCR_W];
  for (int i = 0; i < w; i++) srcX[i] = (x0 + i) * D / N - bx;
  const uint8_t* img = (const uint8_t*)spr.getPointer();
  int sw = spr.width();
  bool swap = tft.getSwapBytes();
  tft.setSwapBytes(false);
  tft.startWrite();
  tft.setAddrWindow(layX + x0, layY + y0, w, y1 - y0);
  int last = -1;
  for (int y = y0; y < y1; y++) {
    int sy = y * D / N - by;
    if (sy != last) {
      const uint8_t* row = img + sy * sw;
      for (int i = 0; i < w; i++) line[i] = lut[row[srcX[i]]];
      last = sy;
    }
    tft.pushPixels(line, w);
  }
  tft.endWrite();
  tft.setSwapBytes(swap);
}
#endif

void show(int batteryY = 5) {
  drawBattery(batteryY);
  int64_t t0 = esp_timer_get_time();
#if defined(BOARD_CYD35)
  if (sprNative)     spr.pushSprite(0, band * NATIVE_STRIP);
  else if (band == 0) pushScaled(0, 0, spr.width(), spr.height());
  else if (portrait) pushScaled(170, 0, canvasW - 170, 320);
  else               pushScaled(0, 170, 320, canvasH - 170);
  if (band == 0) pushUs = esp_timer_get_time() - t0;
#elif defined(BOARD_CYD)
  if (band == 0)     spr.pushSprite(0, 0);
  else if (portrait) spr.pushSprite(170, 0, 0, 0, canvasW - 170, 320);   // the right-hand strip
  else               spr.pushSprite(0, 170, 0, 0, 320, canvasH - 170);   // the bottom strip
  if (band == 0) pushUs = esp_timer_get_time() - t0;
#else
  spr.pushSprite(0, 0);
  pushUs = esp_timer_get_time() - t0;
#endif
}

// ---------- Helpers ----------
String fit(String s, int w, int font);

void header(const String& left, const String& right, uint16_t leftCol = C_ACC, uint16_t rightCol = C_DIM) {
  int lx = portrait ? 4 : 6;
  spr.fillRect(0, 0, canvasWidth(), 20, C_HDR);
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
  spr.drawString(msg, canvasWidth() / 2, y);
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
#ifdef BOARD_CYD35
const int COL_X = 255, COL_W = 65, COL_H = 480;    // the portrait column
#else
const int COL_X = 170, COL_W = 70, COL_H = 320;
#endif

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

bool visible() { return !fullScreen && !native && !(portrait && wideOK && statusHidden); }

void paint(bool full);

void draw(bool full) {
  static uint32_t last = 0;
  if (!visible()) return;
  if (!full && millis() - last < 1000) return;
  last = millis();
  paint(full);
}

// Is (x, y) a tap that should slide the status bar away or back?
bool isToggleTap(int x, int y) {
  if (!portrait || !wideOK) return false;
  return statusHidden ? x >= 240 - 30 : x >= 170;
}

// Slides the portrait status column off to the right (or back in), then resizes the canvas.
void toggle() {
  bool away = !statusHidden;
  for (int i = 0; i <= 7; i++) {
    int s = COL_W * i / 7;
    int off = away ? s : COL_W - s;
    tft.setViewport(COL_X, 0, COL_W, COL_H, false);   // clip to the column
    tft.setOrigin(off, 0);                             // ...and draw it shifted right
    paint(true);
    tft.resetViewport();
    if (off) tft.fillRect(COL_X, 0, off, COL_H, C_BG);
    delay(15);
  }
  statusHidden = away;
  applyLayout();
  redrawNow = true;
}

void paint(bool full) {
  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  bool synced = now > 1700000000;
  bool up = wifiUp();
  int rssi = up ? WiFi.RSSI() : 0;
  char date[24];
  strftime(date, sizeof date, "%a %b %e", &t);

#ifdef BOARD_CYD35
  if (!portrait) {                       // a 480x65 bar along the bottom
    const int Y = SCR_H - SB;
    if (full) { tft.fillRect(0, Y, SCR_W, SB, C_BG); tft.drawFastHLine(0, Y + 1, SCR_W, C_LINE); }
    text(synced ? clockText(t) : String("--:--"), 10, Y + 9, 6, C_TEXT, TL_DATUM, 140);
    text(synced ? String(date) : String(""), 158, Y + 22, 2, C_DIM, ML_DATUM, 110);
    text(String("v" FW_VERSION), 158, Y + 42, 2, 0x4A49, ML_DATUM, 110);
    wifiBars(452, Y + 26, rssi, up);
    text(up ? String(rssi) + " dBm" : String(""), 444, Y + 18, 2, up ? rssiColor(rssi) : C_BAD, MR_DATUM, 70);
    text(up ? WiFi.SSID() : String("Wi-Fi down"), 472, Y + 38, 2, up ? C_TEXT : C_BAD, MR_DATUM, 200);
    text(up ? WiFi.localIP().toString() : String(""), 472, Y + 54, 2, C_DIM, MR_DATUM, 200);
  } else {                               // a 65x480 column on the right
    const int X = COL_X, CX = COL_X + COL_W / 2 + 1;
    if (full) { tft.fillRect(X, 0, COL_W, COL_H, C_BG); tft.drawFastVLine(X + 1, 0, COL_H, C_LINE); }
    text(synced ? clockText(t) : String("--:--"), CX, 20, 4, C_TEXT, MC_DATUM, COL_W - 4);
    strftime(date, sizeof date, "%b %e", &t);
    text(synced ? String(date) : String(""), CX, 44, 2, C_DIM, MC_DATUM, COL_W - 4);
    wifiBars(CX - 10, 90, rssi, up);
    text(up ? String(rssi) + " dBm" : String("no Wi-Fi"), CX, 102, 1, up ? rssiColor(rssi) : C_BAD, MC_DATUM, COL_W - 4);
  }
  return;
#endif
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
