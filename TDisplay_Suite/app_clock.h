// Big Clock
// Just the time, as big and plain as the screen allows: HH:MM in drawn seven-segment digits
// nearly the full height of the display, with a thin seconds bar along the bottom.
// Always 24-hour. Local time from the time zone on the setup page.
// Keys: BTN_B next colour, BTN_A next brightness (both remembered).
#pragma once

namespace bigclock {

const uint16_t COLORS[] = {0xFFFF, 0xF800, 0xFD20, 0x07E0, 0x07FF, 0x5DDF, 0x001F};  // white red amber green cyan blue dark-blue
const int      NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);
const uint8_t  DIGITS[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  // bits: a b c d e f g

// Digit size: two pairs of 66 px digits around a colon, 146 px tall.
const int W = 66, H = 146, T = 17, GAP = 2, TOP = 8;   // T: segment thickness, was 15
const int X[4] = {8, 82, 172, 246};

int colorIx = 0;
int lastMin = -1;

uint16_t col()   { return COLORS[colorIx]; }
uint16_t ghost() { return spr.alphaBlend(22, col(), C_BG); }   // the faint glow of an unlit segment

// A segment with pointed ends, along x (horizontal) or y (vertical), centred on the line c.
void hseg(int x0, int x1, int c, uint16_t color) {
  int h = T / 2;
  x0 += GAP; x1 -= GAP;
  spr.fillRect(x0 + h, c - h, x1 - x0 - 2 * h + 1, T, color);
  spr.fillTriangle(x0, c, x0 + h, c - h, x0 + h, c + h, color);
  spr.fillTriangle(x1, c, x1 - h, c - h, x1 - h, c + h, color);
}

void vseg(int y0, int y1, int c, uint16_t color) {
  int h = T / 2;
  y0 += GAP; y1 -= GAP;
  spr.fillRect(c - h, y0 + h, T, y1 - y0 - 2 * h + 1, color);
  spr.fillTriangle(c, y0, c - h, y0 + h, c + h, y0 + h, color);
  spr.fillTriangle(c, y1, c - h, y1 - h, c + h, y1 - h, color);
}

void digit(int x, int n) {
  int l = x + T / 2, r = x + W - T / 2, t = TOP + T / 2, m = TOP + H / 2, b = TOP + H - T / 2;
  uint8_t bits = DIGITS[n];
  auto c = [&](int seg) { return bits & (1 << seg) ? col() : ghost(); };
  hseg(l, r, t, c(0));    // a
  vseg(t, m, r, c(1));    // b
  vseg(m, b, r, c(2));    // c
  hseg(l, r, b, c(3));    // d
  vseg(m, b, l, c(4));    // e
  vseg(t, m, l, c(5));    // f
  hseg(l, r, m, c(6));    // g
}

void render() {
  spr.fillSprite(C_BG);
  time_t now = time(nullptr);
  if (now < 1700000000) {
    centered("Setting the time...", 80, 4);
    spr.pushSprite(0, 0);
    return;
  }
  struct tm t;
  localtime_r(&now, &t);
  digit(X[0], t.tm_hour / 10);
  digit(X[1], t.tm_hour % 10);
  digit(X[2], t.tm_min / 10);
  digit(X[3], t.tm_min % 10);
  int s = T - 2;                                       // the colon
  spr.fillRect(160 - s / 2, TOP + H * 30 / 100, s, s, col());
  spr.fillRect(160 - s / 2, TOP + H * 70 / 100 - s, s, s, col());

  int w = t.tm_sec * 320 / 59;                         // the seconds bar
  spr.fillRect(0, 167, 320, 3, ghost());
  spr.fillRect(0, 167, w, 3, spr.alphaBlend(110, col(), C_BG));
  if (!wifiUp()) spr.fillCircle(314, 6, 3, C_BAD);     // a red dot while Wi-Fi is down
  spr.pushSprite(0, 0);
}

void start() {
  Preferences p;
  p.begin("suite", true);
  colorIx = constrain(p.getInt("clkcol", 0), 0, NUM_COLORS - 1);
  p.end();
  render();
}

void run() {
  static time_t lastSec = 0;
  time_t now = time(nullptr);
  if (now != lastSec) { lastSec = now; render(); }
}

void press(int pin) {
  if (pin == BTN_B) {
    colorIx = (colorIx + 1) % NUM_COLORS;
    Preferences p;
    p.begin("suite", false);
    p.putInt("clkcol", colorIx);
    p.end();
  } else {
    stepBrightness();
  }
  render();
}

}  // namespace bigclock
