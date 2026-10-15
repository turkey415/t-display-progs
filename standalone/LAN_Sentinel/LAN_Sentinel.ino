/*
  LAN Sentinel  --  LilyGO T-Display-S3 (ESP32-S3, 1.9" 170x320 ST7789)
  ----------------------------------------------------------------------
  Pings up to 6 hosts in rotation and shows live up/down + latency.
  Handy as a "is the event network healthy?" glance screen on a rack or table.

  Buttons:
    BTN_B (GPIO14, right/top key) : toggle Hosts page / Wi-Fi details page
    BTN_A (GPIO0,  BOOT key)       : force an immediate re-check of all hosts

  Libraries:
    TFT_eSPI   (with Setup206_LilyGo_T_Display_S3 selected)
    ESP32Ping  (https://github.com/marian-craciunescu/ESP32Ping)
*/

#include <WiFi.h>
#include <ESP32Ping.h>
#include <TFT_eSPI.h>

// ---------- Board pins ----------
#define PIN_POWER_ON 15   // LCD + battery power enable (must be HIGH on battery)
#define PIN_LCD_BL   38   // backlight
#define PIN_BAT      4    // battery voltage (through 1/2 divider)
#define BTN_A        0
#define BTN_B        14

// ---------- Your settings ----------
const char* WIFI_SSID = "your-wifi";        // 2.4 GHz only
const char* WIFI_PASS = "your-password";

struct Host {
  const char* label;
  const char* target;     // IP, hostname, or "GW" for the DHCP gateway
  IPAddress   ip;
  bool        up;
  bool        checked;
  float       ms;
  uint32_t    since;      // millis() of last state change
};

// Up to 6 rows fit on screen.
Host hosts[] = {
  {"Gateway",  "GW"},
  {"Core SW",  "192.168.1.2"},
  {"Firewall", "192.168.1.254"},
  {"DNS 1",    "1.1.1.1"},
  {"DNS 2",    "8.8.8.8"},
  {"Internet", "google.com"},
};
const int NUM_HOSTS = sizeof(hosts) / sizeof(hosts[0]);

const uint32_t PING_GAP_MS = 400;   // pause between individual host checks

// ---------- Colors ----------
#define C_BG      0x0000
#define C_HDR     0x18E3
#define C_TEXT    0xFFFF
#define C_DIM     0x8410
#define C_GOOD    0x07E0
#define C_WARN    0xFFE0
#define C_SLOW    0xFD20
#define C_BAD     0xF800

TFT_eSPI     tft;
TFT_eSprite  spr(&tft);

int      page = 0;            // 0 = hosts, 1 = wifi details
int      nextHost = 0;
uint32_t lastPing = 0;

// ---------- Helpers ----------
String fmtDur(uint32_t s) {
  if (s < 60)    return String(s) + "s";
  if (s < 3600)  return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h" + String((s % 3600) / 60) + "m";
  return String(s / 86400) + "d" + String((s % 86400) / 3600) + "h";
}

float batteryVolts() {
  return analogReadMilliVolts(PIN_BAT) * 2.0f / 1000.0f;
}

bool resolve(Host& h) {
  if (strcmp(h.target, "GW") == 0) { h.ip = WiFi.gatewayIP(); return h.ip != IPAddress(0,0,0,0); }
  if (h.ip.fromString(h.target)) return true;
  return WiFi.hostByName(h.target, h.ip) == 1;
}

void checkHost(Host& h) {
  bool ok = false;
  float ms = 0;
  if (WiFi.status() == WL_CONNECTED && resolve(h)) {
    ok = Ping.ping(h.ip, 2);
    if (ok) ms = Ping.averageTime();
  }
  if (!h.checked || ok != h.up) h.since = millis();
  h.up = ok;
  h.ms = ms;
  h.checked = true;
}

bool pressed(int pin) {           // simple edge detect with debounce
  static bool last[48];
  static uint32_t t[48];
  bool down = digitalRead(pin) == LOW;
  bool fire = false;
  if (down != last[pin] && millis() - t[pin] > 40) {
    t[pin] = millis();
    if (down) fire = true;
    last[pin] = down;
  }
  return fire;
}

// ---------- Drawing ----------
void drawHeader(const char* title) {
  spr.fillRect(0, 0, 320, 22, C_HDR);
  spr.setTextFont(2);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(title, 6, 3);

  spr.setTextDatum(TR_DATUM);
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    uint16_t c = rssi > -60 ? C_GOOD : rssi > -72 ? C_WARN : C_BAD;
    spr.setTextColor(c, C_HDR);
    spr.drawString(WiFi.SSID() + " " + String(rssi) + "dBm", 314, 3);
  } else {
    spr.setTextColor(C_BAD, C_HDR);
    spr.drawString("Wi-Fi DOWN", 314, 3);
  }
}

void drawHosts() {
  drawHeader("LAN SENTINEL");
  int upCount = 0;

  for (int i = 0; i < NUM_HOSTS && i < 6; i++) {
    Host& h = hosts[i];
    int y = 26 + i * 21;
    uint16_t dot = !h.checked ? C_DIM : (h.up ? C_GOOD : C_BAD);
    if (h.up) upCount++;

    if (i == nextHost) spr.drawRect(2, y, 316, 20, 0x2945);   // "checking" cursor
    spr.fillCircle(12, y + 10, 6, dot);

    spr.setTextFont(2);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(h.label, 24, y + 10);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(h.target, 100, y + 10);

    spr.setTextDatum(MR_DATUM);
    if (!h.checked) {
      spr.setTextColor(C_DIM, C_BG);
      spr.drawString("...", 314, y + 10);
    } else if (h.up) {
      uint16_t c = h.ms < 20 ? C_GOOD : h.ms < 100 ? C_WARN : C_SLOW;
      spr.setTextColor(c, C_BG);
      spr.drawString(String(h.ms, 0) + " ms", 314, y + 10);
    } else {
      spr.setTextColor(C_BAD, C_BG);
      spr.drawString("DOWN " + fmtDur((millis() - h.since) / 1000), 314, y + 10);
    }
  }

  // Footer
  spr.drawFastHLine(0, 152, 320, C_HDR);
  spr.setTextFont(2);
  spr.setTextDatum(BL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(WiFi.localIP().toString(), 6, 169);

  spr.setTextDatum(BC_DATUM);
  spr.setTextColor(upCount == NUM_HOSTS ? C_GOOD : C_BAD, C_BG);
  spr.drawString(String(upCount) + "/" + String(NUM_HOSTS) + " up", 160, 169);

  spr.setTextDatum(BR_DATUM);
  float v = batteryVolts();
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(v > 4.3f ? "USB" : String(v, 2) + "V", 314, 169);
}

void drawWifi() {
  drawHeader("WI-FI DETAILS");
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);

  struct Row { const char* k; String v; };
  Row rows[] = {
    {"SSID",    WiFi.SSID()},
    {"BSSID",   WiFi.BSSIDstr()},
    {"Channel", String(WiFi.channel())},
    {"RSSI",    String(WiFi.RSSI()) + " dBm"},
    {"IP",      WiFi.localIP().toString()},
    {"Mask",    WiFi.subnetMask().toString()},
    {"Gateway", WiFi.gatewayIP().toString()},
    {"DNS",     WiFi.dnsIP(0).toString()},
    {"MAC",     WiFi.macAddress()},
  };
  for (int i = 0; i < 9; i++) {
    int y = 25 + i * 16;
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(rows[i].k, 8, y);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(rows[i].v, 80, y);
  }

  // RSSI bar on the right
  int rssi = constrain(WiFi.RSSI(), -95, -30);
  int h = map(rssi, -95, -30, 0, 120);
  uint16_t c = rssi > -60 ? C_GOOD : rssi > -72 ? C_WARN : C_BAD;
  spr.drawRect(290, 28, 20, 122, C_DIM);
  spr.fillRect(291, 149 - h, 18, h, c);

  spr.setTextDatum(BR_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("up " + fmtDur(millis() / 1000), 280, 169);
}

void render() {
  spr.fillSprite(C_BG);
  if (page == 0) drawHosts(); else drawWifi();
  spr.pushSprite(0, 0);
}

void splash(const String& msg) {
  spr.fillSprite(C_BG);
  spr.setTextFont(4);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString("LAN SENTINEL", 160, 70);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(msg, 160, 105);
  spr.pushSprite(0, 0);
}

// ---------- Main ----------
void setup() {
  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL, OUTPUT);   digitalWrite(PIN_LCD_BL, HIGH);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);           // landscape, USB on the left; use 3 to flip
  tft.fillScreen(C_BG);
  spr.createSprite(320, 170);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    splash("Connecting to " + String(WIFI_SSID) + "  " + String((millis() - t0) / 1000) + "s");
    delay(250);
  }
  render();
}

void loop() {
  if (pressed(BTN_B)) { page ^= 1; render(); }
  if (pressed(BTN_A)) {
    for (int i = 0; i < NUM_HOSTS; i++) hosts[i].checked = false;
    nextHost = 0;
    render();
  }

  if (millis() - lastPing > PING_GAP_MS) {
    checkHost(hosts[nextHost]);           // blocks up to ~2 s on a dead host
    nextHost = (nextHost + 1) % NUM_HOSTS;
    lastPing = millis();
    render();
  }

  static uint32_t lastDraw = 0;           // keep "DOWN 3m" timers ticking
  if (millis() - lastDraw > 1000) { lastDraw = millis(); render(); }
}
