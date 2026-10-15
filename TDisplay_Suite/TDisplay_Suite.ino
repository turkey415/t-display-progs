/*
  T-Display Suite  --  LilyGO T-Display-S3 (ESP32-S3, 1.9" 170x320 ST7789)
  ------------------------------------------------------------------------
  One firmware, several apps, picked from a menu on the device:
    Weather Clock, Ham Shack Clock, LAN Sentinel, GXL Firewall Monitor

  Wi-Fi: tries the last network set up from a phone, then the home network in secrets.h
  (if there is one). If neither is in range it opens the "TDisplay-Setup" hotspot: join it
  from a phone, pick the local Wi-Fi, and fill in the settings (GXL server, callsign, ...).
  Losing Wi-Fi for 2 minutes restarts the device and goes through those steps again.

  Keys (top = BTN_B, GPIO14; bottom = BTN_A, the BOOT key, GPIO0):
    hold bottom      : app menu (top = next, bottom = pick)
    hold top         : Wi-Fi & settings hotspot
    hold top at power-on : Wi-Fi & settings hotspot
    short presses    : up to the app (usually top = next page, bottom = refresh)

  Libraries: TFT_eSPI (Setup206_LilyGo_T_Display_S3 selected), ArduinoJson 7,
             WiFiManager (tzapu), ESP32Ping
*/

#include <WiFiManager.h>
#include <Preferences.h>
#include "common.h"
#include "app_weather.h"
#include "app_ham.h"
#include "app_sentinel.h"
#include "app_gxl.h"

#if __has_include("secrets.h")
#include "secrets.h"            // your home Wi-Fi and defaults; kept out of git
#endif
#ifndef HOME_SSID
#define HOME_SSID ""
#define HOME_PASS ""
#endif
#ifndef DEFAULT_GXL_SERVER
#define DEFAULT_GXL_SERVER ""
#endif
#ifndef DEFAULT_GXL_KEY
#define DEFAULT_GXL_KEY ""
#endif
#ifndef DEFAULT_APP
#define DEFAULT_APP 0           // the app at first power-on, before one is picked from the menu
#endif
#ifndef DEFAULT_CALLSIGN
#define DEFAULT_CALLSIGN "N0CALL"
#endif

const char* AP_NAME = "TDisplay-Setup";
const char* DEFAULT_TZ = "CST6CDT,M3.2.0,M11.1.0";      // US Central with DST
const uint32_t WIFI_LOST_MS = 2UL * 60UL * 1000UL;
const uint32_t PORTAL_SECS  = 300;
const uint32_t MENU_IDLE_MS = 30UL * 1000UL;

App apps[] = {
  {"Weather Clock",   "Time, weather, forecast",  weather::start,  weather::run,  weather::press},
  {"Ham Shack Clock", "UTC, solar, HF bands",     ham::start,      ham::run,      ham::press},
  {"LAN Sentinel",    "Ping hosts, Wi-Fi info",   sentinel::start, sentinel::run, sentinel::press},
  {"GXL Firewall",    "Firewall + cache Mbps",    gxl::start,      gxl::run,      gxl::press},
};
const int NUM_APPS = sizeof(apps) / sizeof(apps[0]);

Preferences prefs;
WiFiManager wm;
Button btnA = {BTN_A}, btnB = {BTN_B};

// ---------- Settings ----------
void loadSettings() {
  prefs.begin("suite", true);
  cfg.server     = prefs.getString("server", DEFAULT_GXL_SERVER);
  cfg.key        = prefs.getString("key", DEFAULT_GXL_KEY);
  cfg.callsign   = prefs.getString("call", DEFAULT_CALLSIGN);
  cfg.tz         = prefs.getString("tz", DEFAULT_TZ);
  cfg.hosts      = prefs.getString("hosts", "");
  cfg.fahrenheit = prefs.getBool("fahr", true);
  cfg.clock24    = prefs.getBool("h24", true);
  cfg.app        = constrain(prefs.getInt("app", DEFAULT_APP), 0, NUM_APPS - 1);
  prefs.end();
}

void saveApp() {
  prefs.begin("suite", false);
  prefs.putInt("app", cfg.app);
  prefs.end();
}

// The setup page's fields. Built when the hotspot opens, read back when it saves.
WiFiManagerParameter* pServer;
WiFiManagerParameter* pKey;
WiFiManagerParameter* pCall;
WiFiManagerParameter* pTz;
WiFiManagerParameter* pHosts;
WiFiManagerParameter* pUnits;
WiFiManagerParameter* pClock;

void saveParams() {
  cfg.server   = pServer->getValue();  cfg.server.trim();
  cfg.key      = pKey->getValue();     cfg.key.trim();
  cfg.callsign = pCall->getValue();    cfg.callsign.trim();
  cfg.tz       = pTz->getValue();      cfg.tz.trim();
  cfg.hosts    = pHosts->getValue();   cfg.hosts.trim();
  String u = pUnits->getValue(), c = pClock->getValue();
  cfg.fahrenheit = !(u.startsWith("C") || u.startsWith("c"));
  cfg.clock24    = !c.startsWith("12");
  if (!cfg.tz.length()) cfg.tz = DEFAULT_TZ;
  if (cfg.server.length() && !cfg.server.startsWith("http")) cfg.server = "http://" + cfg.server;

  prefs.begin("suite", false);
  prefs.putString("server", cfg.server);
  prefs.putString("key", cfg.key);
  prefs.putString("call", cfg.callsign);
  prefs.putString("tz", cfg.tz);
  prefs.putString("hosts", cfg.hosts);
  prefs.putBool("fahr", cfg.fahrenheit);
  prefs.putBool("h24", cfg.clock24);
  prefs.end();
  Serial.println("settings saved");
}

// ---------- Wi-Fi ----------
void splash(const String& msg) {
  spr.fillSprite(C_BG);
  centered("T-DISPLAY SUITE", 62, 4, C_TEXT);
  centered(apps[cfg.app].name, 90, 2, C_ACC);
  centered(msg, 118, 2);
  spr.pushSprite(0, 0);
}

bool waitConnect(const String& what, uint32_t ms) {
  uint32_t t0 = millis();
  while (!wifiUp() && millis() - t0 < ms) {
    splash(what + "  " + String((millis() - t0) / 1000) + "s");
    delay(250);
  }
  return wifiUp();
}

void showSetup() {
  spr.fillSprite(C_BG);
  header("WI-FI & SETTINGS", "");
  spr.setTextDatum(TC_DATUM);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("On your phone, join this Wi-Fi:", 160, 28);
  spr.setTextFont(4);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(AP_NAME, 160, 48);
  spr.setTextFont(2);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString("A setup page opens (or go to 192.168.4.1)", 160, 82);
  spr.drawString("Configure WiFi: network + settings", 160, 100);
  spr.drawString("Setup: settings only", 160, 118);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("Restarts after saving, or in 5 min", 160, 148);
  spr.pushSprite(0, 0);
}

// Opens the hotspot with the setup page. Restarts afterwards, so new settings take effect cleanly.
void runPortal() {
  String key = cfg.key;
  pServer = new WiFiManagerParameter("server", "GXL Event Server (e.g. http://10.0.0.5:8000)", cfg.server.c_str(), 80);
  pKey    = new WiFiManagerParameter("key", "GXL agent key", key.c_str(), 64, "type='password'");
  pCall   = new WiFiManagerParameter("call", "Callsign (Ham Shack Clock)", cfg.callsign.c_str(), 12);
  pTz     = new WiFiManagerParameter("tz", "Local time zone, POSIX (Ham Shack Clock)", cfg.tz.c_str(), 48);
  pHosts  = new WiFiManagerParameter("hosts", "LAN Sentinel hosts: Label=IP,Label=IP (GW = gateway, up to 6)", cfg.hosts.c_str(), 200);
  pUnits  = new WiFiManagerParameter("units", "Units: F or C", cfg.fahrenheit ? "F" : "C", 2);
  pClock  = new WiFiManagerParameter("clock", "Clock: 24 or 12", cfg.clock24 ? "24" : "12", 3);
  for (auto* p : {pServer, pKey, pCall, pTz, pHosts, pUnits, pClock}) wm.addParameter(p);

  std::vector<const char*> menu = {"wifi", "param", "info", "restart"};
  wm.setMenu(menu);
  wm.setTitle("T-Display");
  wm.setSaveParamsCallback(saveParams);
  wm.setBreakAfterConfig(true);
  wm.setConfigPortalTimeout(PORTAL_SECS);
  showSetup();
  wm.startConfigPortal(AP_NAME);
  ESP.restart();
}

void connectWifi(bool forceSetup) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (!forceSetup) {
    if (wm.getWiFiIsSaved()) {
      WiFi.begin();
      if (waitConnect("Joining " + wm.getWiFiSSID(true), 15000)) return;
    }
    if (strlen(HOME_SSID)) {
      WiFi.persistent(false);                      // keep the phone-set network saved
      WiFi.begin(HOME_SSID, HOME_PASS);
      bool ok = waitConnect("Joining " + String(HOME_SSID), 15000);
      WiFi.persistent(true);
      if (ok) return;
      WiFi.disconnect();
    }
  }
  runPortal();
}

// ---------- Menu ----------
bool inMenu = false;
int  menuPick = 0;
uint32_t menuSince = 0;

void drawMenu() {
  spr.fillSprite(C_BG);
  header("PICK AN APP", wifiUp() ? WiFi.localIP().toString() : "Wi-Fi DOWN", C_ACC, wifiUp() ? C_DIM : C_BAD);
  int rows = NUM_APPS + 1;
  for (int i = 0; i < rows; i++) {
    int y = 23 + i * 29;
    bool sel = i == menuPick;
    if (sel) spr.fillRoundRect(2, y, 316, 27, 4, C_HDR);
    if (i == cfg.app) spr.fillCircle(10, y + 13, 3, C_GOOD);     // the one running now
    spr.setTextDatum(TL_DATUM);
    spr.setTextFont(2);
    spr.setTextColor(sel ? C_ACC : C_TEXT, sel ? C_HDR : C_BG);
    spr.drawString(i < NUM_APPS ? apps[i].name : "Wi-Fi & settings", 20, y + 5);
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, sel ? C_HDR : C_BG);
    spr.setTextDatum(MR_DATUM);
    spr.drawString(i < NUM_APPS ? apps[i].blurb : "Setup hotspot", 312, y + 14);
  }
  spr.pushSprite(0, 0);
}

void openMenu() {
  inMenu = true;
  menuPick = cfg.app;
  menuSince = millis();
  drawMenu();
}

void menuPress(int pin) {
  menuSince = millis();
  if (pin == BTN_B) { menuPick = (menuPick + 1) % (NUM_APPS + 1); drawMenu(); return; }
  inMenu = false;
  if (menuPick == NUM_APPS) { runPortal(); return; }
  if (menuPick != cfg.app) { cfg.app = menuPick; saveApp(); }
  apps[cfg.app].start();
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

  loadSettings();
  bool forceSetup = digitalRead(BTN_B) == LOW;     // top key held at power-up
  connectWifi(forceSetup);
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");

  apps[cfg.app].start();
}

void loop() {
  Press a = poll(btnA), b = poll(btnB);

  if (inMenu) {
    if (a == SHORT) menuPress(BTN_A);
    if (b == SHORT) menuPress(BTN_B);
    if (inMenu && millis() - menuSince > MENU_IDLE_MS) { inMenu = false; apps[cfg.app].start(); }
  } else {
    if (a == LONG)  { openMenu(); return; }
    if (b == LONG)  { runPortal(); return; }
    if (a == SHORT) apps[cfg.app].press(BTN_A);
    if (b == SHORT) apps[cfg.app].press(BTN_B);
    apps[cfg.app].run();
  }

  // Wi-Fi watchdog: a long outage usually means the device was moved, so restart and run
  // the saved / home / phone-setup sequence again.
  static bool     wasUp = true;
  static uint32_t downSince = 0;
  bool up = wifiUp();
  if (!up) {
    if (wasUp) downSince = millis();
    if (millis() - downSince > WIFI_LOST_MS) ESP.restart();
  } else if (!wasUp) {
    wifiEpoch++;
  }
  wasUp = up;
  delay(10);
}
