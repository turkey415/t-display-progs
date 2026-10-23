/*
  T-Display Suite  --  LilyGO T-Display-S3 (ESP32-S3, 1.9" 170x320 ST7789)
  ------------------------------------------------------------------------
  One firmware, several apps, picked from a menu on the device:
    Big Clock, Weather Clock, XRP Ticker, Wi-Fi Meter, Wireless Sniffer,
    Ham Shack Clock, LAN Sentinel, GXL Firewall, mDNS Browser, Ping Graph,
    DHCP Finder, LAN Scanner

  Wi-Fi: tries the last network set up from a phone, then the home network in secrets.h
  (if there is one). If neither is in range it opens the "TDisplay-Setup" hotspot: join it
  from a phone, pick the local Wi-Fi, and fill in the settings (GXL server, callsign, ...).
  Losing Wi-Fi for 2 minutes restarts the device and goes through those steps again.

  Updates: installs new releases from github.com/turkey415/t-display-progs by itself
  (at power-on and daily; can be turned off on the setup page), or from the menu.

  Keys (top = BTN_B, GPIO14; bottom = BTN_A, the BOOT key, GPIO0):
    hold bottom      : app menu (top = next, bottom = pick)
    hold top         : Wi-Fi & settings hotspot
    hold top at power-on : Wi-Fi & settings hotspot
    menu > Power off : deep sleep (screen and Wi-Fi off); the top key (CYD: a tap) or RST turns it back on
    short presses    : up to the app (usually top = next page, bottom = refresh)

  Libraries: TFT_eSPI (Setup206_LilyGo_T_Display_S3 selected), ArduinoJson 7,
             WiFiManager (tzapu), ESP32Ping

  The same firmware also builds for the Freenove 2.8" CYD (tools/build.sh cyd). There a tap
  anywhere on the screen is the top key (hold = hold top), the BOOT button is the bottom key,
  menu rows can be tapped, a status bar fills the spare 70 pixels, the hotspot is "CYD-Setup",
  and a tap wakes it from power off.
*/

#include <WiFiManager.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include "version.h"
#include "common.h"
#include "app_clock.h"
#include "app_weather.h"
#include "app_xrp.h"
#include "app_ham.h"
#include "app_sentinel.h"
#include "app_gxl.h"
#include "app_sniffer.h"
#include "app_wifimeter.h"
#include "app_mdns.h"
#include "app_pinggraph.h"
#include "app_dhcp.h"
#include "app_lanscan.h"
#include "app_probe.h"
#ifdef BOARD_CYD
#include "app_preclock.h"
#endif
#include "updater.h"

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
#ifndef DEFAULT_APP_NAME
#define DEFAULT_APP_NAME "Weather Clock"   // the app at first power-on, before one is picked
#endif
#ifndef DEFAULT_CALLSIGN
#define DEFAULT_CALLSIGN "N0CALL"
#endif
#ifndef DEFAULT_SHELLY
#define DEFAULT_SHELLY ""
#endif

#ifdef BOARD_CYD
const char* AP_NAME = "CYD-Setup";
#define PORTAL_TITLE "CYD Suite v" FW_VERSION
#else
const char* AP_NAME = "TDisplay-Setup";
#define PORTAL_TITLE "T-Display v" FW_VERSION
#endif
const char* DEFAULT_TZ = "CST6CDT,M3.2.0,M11.1.0";      // US Central with DST
const uint32_t WIFI_LOST_MS = 2UL * 60UL * 1000UL;
const uint32_t PORTAL_SECS  = 300;
const uint32_t MENU_IDLE_MS = 30UL * 1000UL;

App apps[] = {
  {"Big Clock",       "Just the time, huge",      bigclock::start, bigclock::run, bigclock::press},
  {"Weather Clock",   "Weather, sun, alerts",     weather::start,  weather::run,  weather::press},
  {"XRP Ticker",      "Price + history graph",    xrp::start,      xrp::run,      xrp::press},
  {"Wi-Fi Meter",     "Signal + link details",    wifimeter::start, wifimeter::run, wifimeter::press},
  {"Wireless Sniffer","Wi-Fi + Bluetooth nearby", sniffer::start,  sniffer::run,  sniffer::press},
  {"Ham Shack Clock", "UTC, solar, HF bands",     ham::start,      ham::run,      ham::press},
  {"LAN Sentinel",    "Ping hosts, Wi-Fi info",   sentinel::start, sentinel::run, sentinel::press},
  {"GXL Firewall",    "Firewall + cache Mbps",    gxl::start,      gxl::run,      gxl::press},
  {"mDNS Browser",    "Who's on the network",     mdnsbrowser::start, mdnsbrowser::run, mdnsbrowser::press},
  {"Ping Graph",      "Latency + jitter graph",   pinggraph::start, pinggraph::run, pinggraph::press},
  {"DHCP Finder",     "Find rogue DHCP servers",  dhcpfinder::start, dhcpfinder::run, dhcpfinder::press},
  {"LAN Scanner",     "Devices on the subnet",    lanscan::start,   lanscan::run,   lanscan::press},
  {"Probe Viewer",    "Switch port from a probe", probeview::start, probeview::run, probeview::press},
#ifdef BOARD_CYD
  {"Precision Clock", "NTP to the ms, drift",     preclock::start,  preclock::run,  preclock::press},
#endif
};
const int NUM_APPS = sizeof(apps) / sizeof(apps[0]);
int current = 0;                // index of the running app

Preferences prefs;
WiFiManager wm;
Button btnA = {BTN_A}, btnB = {BTN_B};

// Every app starts in landscape; the ones that want portrait turn the screen in start().
void startApp() {
#ifdef BOARD_CYD
  fullScreen = wideOK = false;          // each app asks again in start()
#endif
  setPortrait(false);
  apps[current].start();
}

int appIndex(const String& name) {
  for (int i = 0; i < NUM_APPS; i++) if (name == apps[i].name) return i;
  return -1;
}

// ---------- Settings ----------
void storeSettings() {
  prefs.begin("suite", false);
  prefs.putString("server", cfg.server);
  prefs.putString("key", cfg.key);
  prefs.putString("call", cfg.callsign);
  prefs.putString("tz", cfg.tz);
  prefs.putString("hosts", cfg.hosts);
  prefs.putString("shelly", cfg.shelly);
  prefs.putBool("fahr", cfg.fahrenheit);
  prefs.putBool("h24", cfg.clock24);
  prefs.putBool("auto", cfg.autoUpdate);
  prefs.end();
}

void loadSettings() {
  prefs.begin("suite", true);
  bool stored = prefs.isKey("server");
  cfg.server     = prefs.getString("server", DEFAULT_GXL_SERVER);
  cfg.key        = prefs.getString("key", DEFAULT_GXL_KEY);
  cfg.callsign   = prefs.getString("call", DEFAULT_CALLSIGN);
  cfg.tz         = prefs.getString("tz", DEFAULT_TZ);
  cfg.hosts      = prefs.getString("hosts", "");
  cfg.shelly     = prefs.getString("shelly", DEFAULT_SHELLY);
  cfg.fahrenheit = prefs.getBool("fahr", true);
  cfg.clock24    = prefs.getBool("h24", true);
  cfg.autoUpdate = prefs.getBool("auto", true);
  cfg.bright     = prefs.getInt("bright", 0);
  cfg.app        = prefs.getString("appn", DEFAULT_APP_NAME);
  prefs.end();
  // The first time, keep the built-in defaults in flash, so an update built without
  // secrets.h (like the GitHub releases) still has them.
  if (!stored) storeSettings();
  current = max(0, appIndex(cfg.app));
}

void saveApp() {
  cfg.app = apps[current].name;
  prefs.begin("suite", false);
  prefs.putString("appn", cfg.app);
  prefs.end();
}

// The setup page's fields. Built when the hotspot opens, read back when it saves.
WiFiManagerParameter* pServer;
WiFiManagerParameter* pKey;
WiFiManagerParameter* pCall;
WiFiManagerParameter* pTz;
WiFiManagerParameter* pHosts;
WiFiManagerParameter* pShelly;
WiFiManagerParameter* pUnits;
WiFiManagerParameter* pClock;
WiFiManagerParameter* pAuto;

void saveParams() {
  cfg.server   = pServer->getValue();  cfg.server.trim();
  cfg.key      = pKey->getValue();     cfg.key.trim();
  cfg.callsign = pCall->getValue();    cfg.callsign.trim();
  cfg.tz       = pTz->getValue();      cfg.tz.trim();
  cfg.hosts    = pHosts->getValue();   cfg.hosts.trim();
  cfg.shelly   = pShelly->getValue();  cfg.shelly.trim();
  String u = pUnits->getValue(), c = pClock->getValue(), a = pAuto->getValue();
  cfg.fahrenheit = !(u.startsWith("C") || u.startsWith("c"));
  cfg.clock24    = !c.startsWith("12");
  cfg.autoUpdate = !(a.startsWith("n") || a.startsWith("N"));
  if (!cfg.tz.length()) cfg.tz = DEFAULT_TZ;
  if (cfg.server.length() && !cfg.server.startsWith("http")) cfg.server = "http://" + cfg.server;
  storeSettings();
  Serial.println("settings saved");
}

// ---------- Wi-Fi ----------
void splash(const String& msg) {
  spr.fillSprite(C_BG);
  centered(BOARD_NAME, 58, 4, C_TEXT);
  centered(apps[current].name, 86, 2, C_ACC);
  centered(msg, 112, 2);
  centered("v" FW_VERSION, 150, 1);
  show();
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
  header("WI-FI & SETTINGS", "v" FW_VERSION);
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
  show();
}

// Opens the hotspot with the setup page. Restarts afterwards, so new settings take effect cleanly.
void runPortal() {
  setPortrait(false);
  pServer = new WiFiManagerParameter("server", "GXL Event Server (e.g. http://10.0.0.5:8000)", cfg.server.c_str(), 80);
  pKey    = new WiFiManagerParameter("key", "GXL agent key", cfg.key.c_str(), 64, "type='password'");
  pCall   = new WiFiManagerParameter("call", "Callsign (Ham Shack Clock)", cfg.callsign.c_str(), 12);
  pTz     = new WiFiManagerParameter("tz", "Local time zone, POSIX (Big Clock, Ham Shack Clock)", cfg.tz.c_str(), 48);
  pHosts  = new WiFiManagerParameter("hosts", "LAN Sentinel hosts: Label=IP,Label=IP (GW = gateway, up to 6)", cfg.hosts.c_str(), 200);
  pShelly = new WiFiManagerParameter("shelly", "Shelly temp sensor IP (Weather Clock 2nd temp; blank = off)", cfg.shelly.c_str(), 40);
  pUnits  = new WiFiManagerParameter("units", "Units: F or C", cfg.fahrenheit ? "F" : "C", 2);
  pClock  = new WiFiManagerParameter("clock", "Weather and Ham clocks: 24 or 12", cfg.clock24 ? "24" : "12", 3);
  pAuto   = new WiFiManagerParameter("auto", "Install updates from GitHub by itself: yes or no", cfg.autoUpdate ? "yes" : "no", 4);
  for (auto* p : {pServer, pKey, pCall, pTz, pHosts, pShelly, pUnits, pClock, pAuto}) wm.addParameter(p);

  std::vector<const char*> menu = {"wifi", "param", "info", "update", "restart"};   // update: upload a .bin
  wm.setMenu(menu);
  wm.setTitle(PORTAL_TITLE);
  wm.setSaveParamsCallback(saveParams);
  wm.setBreakAfterConfig(true);
  wm.setConfigPortalTimeout(PORTAL_SECS);
  showSetup();
  wm.startConfigPortal(AP_NAME);
  ESP.restart();
}

void connectWifi(bool forceSetup) {
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {    // why a join failed, for the serial log
    Serial.printf("wifi: disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (!forceSetup) {
    bool saved = wm.getWiFiIsSaved();
    if (saved) {
      Serial.printf("wifi: trying saved network %s\n", wm.getWiFiSSID(true).c_str());
      WiFi.begin();
      if (waitConnect("Joining " + wm.getWiFiSSID(true), 15000)) return;
      WiFi.disconnect();       // stop trying it, or the next WiFi.begin is refused
      delay(200);
    }
    if (strlen(HOME_SSID)) {
      Serial.printf("wifi: trying home network %s\n", HOME_SSID);
      // Kept in flash only when no phone-set network is, so a phone-set one is never replaced
      // and an update without secrets.h still knows the home network.
      WiFi.persistent(!saved);
      WiFi.begin(HOME_SSID, HOME_PASS);
      bool ok = waitConnect("Joining " + String(HOME_SSID), 15000);
      WiFi.persistent(true);
      if (ok) return;
      WiFi.disconnect();
    }
  }
  runPortal();
}

// ---------- Power ----------
// The board has no power switch (the battery is wired straight to the charger), so "off" is
// deep sleep: screen, backlight and radio off, a tiny trickle of current. The top key wakes
// it (not the bottom one: that is the BOOT key); on the CYD a tap on the screen does.
// Unplug the battery for a true off.
void powerOff(const char* why) {
  setPortrait(false);
  spr.fillSprite(C_BG);
  centered(why, 62, 4, C_TEXT);
  centered(HINT_WAKE, 100, 2, C_DIM);
  show();
  delay(2000);
  while (keyDown(BTN_A) || keyDown(BTN_B)) delay(10);   // let go first
  delay(50);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  tft.writecommand(0x28);            // display off
  tft.writecommand(0x10);            // display sleep
  ledcDetach(PIN_LCD_BL);
  pinMode(PIN_LCD_BL, OUTPUT);   digitalWrite(PIN_LCD_BL, LOW);
  gpio_hold_en((gpio_num_t)PIN_LCD_BL);     // keep it low while asleep
#ifdef BOARD_CYD
  // The touch chip pulls its IRQ pin low when touched; GPIO36 is input-only and has a
  // pull-up on the board, so it wakes the ESP32 straight from it.
  gpio_deep_sleep_hold_en();
#ifdef PIN_TOUCH_IRQ
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_IRQ, 0);
#else
  // The 3.5" CYD's touch interrupt (GPIO21) can't wake the chip, so BOOT does.
  rtc_gpio_pullup_en((gpio_num_t)BTN_A);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_A, 0);
#endif
#else
  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, LOW);   // LCD power off
  gpio_hold_en((gpio_num_t)PIN_POWER_ON);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pullup_en((gpio_num_t)BTN_B);
  rtc_gpio_pulldown_dis((gpio_num_t)BTN_B);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_B, 0);
#endif
  esp_deep_sleep_start();
}

// On battery, turns off when the cell is flat, before it browns out and restarts over and over.
void checkBattery() {
  static uint32_t lowSince = 0, lastLog = 0;
  updateBattery();
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    Serial.printf("battery %.2f V  avg %.2f V  %d%%  %s%s%s\n", batV, slowV, batteryPct(),
                  onUSB() ? "USB" : "", computerPlugged() ? " (computer)" : "",
                  charging ? " (charging)" : "");
  }
  if (onUSB() || batV > BAT_EMPTY_V || millis() < 60000) { lowSince = 0; return; }
  if (!lowSince) lowSince = millis();
  if (millis() - lowSince > 30000) powerOff("Battery empty");
}

// ---------- Menu ----------
// The apps, then three actions. Seven rows show at a time; it scrolls.
const int ROW_H = 21, ROWS_SHOWN = 7;
const int ITEMS = NUM_APPS + 3;      // + "Check for update", "Wi-Fi & settings", "Power off"
bool inMenu = false;
#if defined(BOARD_CYD35)
#define BOARD_WAKE_BLURB "BOOT turns it on"
#elif defined(BOARD_CYD)
#define BOARD_WAKE_BLURB "A tap turns it on"
#else
#define BOARD_WAKE_BLURB "Top key turns it on"
#endif
int  menuPick = 0, menuTop = 0;
uint32_t menuSince = 0;

void drawMenu() {
  spr.fillSprite(C_BG);
  header("PICK AN APP", (wifiUp() ? WiFi.localIP().toString() : String("Wi-Fi DOWN")) + "  v" FW_VERSION,
         C_ACC, wifiUp() ? C_DIM : C_BAD);
  if (menuPick < menuTop) menuTop = menuPick;
  if (menuPick >= menuTop + ROWS_SHOWN) menuTop = menuPick - ROWS_SHOWN + 1;
  for (int r = 0; r < ROWS_SHOWN && menuTop + r < ITEMS; r++) {
    int i = menuTop + r, y = 22 + r * ROW_H;
    bool sel = i == menuPick;
    uint16_t bg = sel ? C_HDR : C_BG;
    if (sel) spr.fillRoundRect(2, y, 312, ROW_H - 1, 4, C_HDR);
    if (i == current) spr.fillCircle(10, y + 10, 3, C_GOOD);     // the one running now
    static const char* ACTIONS[] = {"Check for update", "Wi-Fi & settings", "Power off"};
    static const char* ACTION_BLURBS[] = {"From GitHub releases", "Setup hotspot", BOARD_WAKE_BLURB};
    const char* name  = i < NUM_APPS ? apps[i].name  : ACTIONS[i - NUM_APPS];
    const char* blurb = i < NUM_APPS ? apps[i].blurb : ACTION_BLURBS[i - NUM_APPS];
    spr.setTextDatum(ML_DATUM);
    spr.setTextFont(2);
    spr.setTextColor(sel ? C_ACC : i < NUM_APPS ? C_TEXT : C_DIM, bg);
    spr.drawString(name, 20, y + 10);
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, bg);
    spr.setTextDatum(MR_DATUM);
    spr.drawString(blurb, 306, y + 10);
  }
  if (ITEMS > ROWS_SHOWN) {                      // scroll bar
    int h = 147 * ROWS_SHOWN / ITEMS, y = 22 + (147 - h) * menuTop / (ITEMS - ROWS_SHOWN);
    spr.fillRect(317, y, 3, h, C_DIM);
  }
  show();
}

void openMenu() {
#ifdef BOARD_CYD
  fullScreen = wideOK = false;
#endif
  setPortrait(false);
  inMenu = true;
#ifdef BOARD_CYD
  touch::forget();
#endif
  menuPick = current;
  menuSince = millis();
  drawMenu();
}

void menuPress(int pin) {
  menuSince = millis();
  if (pin == BTN_B) { menuPick = (menuPick + 1) % ITEMS; drawMenu(); return; }
  inMenu = false;
  if (menuPick == NUM_APPS + 2) { powerOff("Powering off"); return; }
  if (menuPick == NUM_APPS + 1) { runPortal(); return; }
  if (menuPick == NUM_APPS) { updater::check(true); startApp(); return; }
  if (menuPick != current) { current = menuPick; saveApp(); }
  startApp();
}

// ---------- Main ----------
void setup() {
  gpio_hold_dis((gpio_num_t)PIN_LCD_BL);        // held low while it was powered off
  gpio_deep_sleep_hold_dis();
  bool woke = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
#ifdef BOARD_CYD
#ifdef PIN_TOUCH_IRQ
  if (woke) rtc_gpio_deinit((gpio_num_t)PIN_TOUCH_IRQ);
#else
  if (woke) rtc_gpio_deinit((gpio_num_t)BTN_A);
#endif
  for (int p : LED_PINS) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }   // RGB LED off
  pinMode(BTN_A, INPUT_PULLUP);
  touch::begin();
#else
  gpio_hold_dis((gpio_num_t)PIN_POWER_ON);
  if (woke) rtc_gpio_deinit((gpio_num_t)BTN_B);  // back to a normal input
  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
#endif
  Serial.begin(115200);

  tft.init();
  screenRotation(ROT_LANDSCAPE);   // landscape, USB on the left; use 3 to flip
  tft.fillScreen(C_BG);
  // Made before Wi-Fi starts, while memory is in one piece. setPortrait swaps it for one of
  // the same size.
  spr.setColorDepth(SPR_DEPTH);
  if (!spr.createSprite(320, 170)) Serial.println("no memory for the screen");
#ifdef BOARD_CYD
  Serial.printf("heap after screen: %u free, %u largest\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif

  loadSettings();
#ifdef START_APP
  current = constrain(START_APP, 0, NUM_APPS - 1);   // test builds: -DSTART_APP=<index in apps[]>
#endif
  applyBrightness();
  // Turned on with the top key: wait for it to be let go, so it doesn't count as "hold top".
  // On the CYD a tap wakes it, so wait for any touch to end too.
  if (woke) { splash("Turning on"); while (keyDown(BTN_B) || keyDown(BTN_A)) delay(10); delay(50); }
#ifdef BOARD_CYD
  touch::forget();
#endif
  bool forceSetup = !woke && keyDown(BTN_B);     // top key (CYD: NEXT) held at power-up
  connectWifi(forceSetup);
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");

  if (cfg.autoUpdate) { splash("Checking for updates"); updater::check(false); }
  startApp();
}

void loop() {
  Press a = poll(btnA), b = poll(btnB);

#ifdef BOARD_CYD
  int tx, ty;
  bool tapped = touch::tap(tx, ty);
  touch::pressTap = tapped; touch::pressX = tx; touch::pressY = ty;
#endif
  if (inMenu) {
#ifdef BOARD_CYD
    // A tap on a row opens it; a tap anywhere else (the status bar) moves down like NEXT.
    if (tapped && ty >= 22 && ty < 22 + ROWS_SHOWN * ROW_H && menuTop + (ty - 22) / ROW_H < ITEMS) {
      menuPick = menuTop + (ty - 22) / ROW_H;
      drawMenu();
      delay(120);
      b = NONE;
      a = SHORT;
    }
#endif
    if (a == SHORT) menuPress(BTN_A);
    if (b == SHORT) menuPress(BTN_B);
    if (inMenu && millis() - menuSince > MENU_IDLE_MS) { inMenu = false; startApp(); }
  } else {
#ifdef BOARD_CYD
    if (tapped && b == SHORT && status::isToggleTap(tx, ty)) { status::toggle(); b = NONE; }
#endif
    if (a == LONG)  { openMenu(); return; }
    if (b == LONG)  { runPortal(); return; }
    if (a == SHORT) apps[current].press(BTN_A);
    if (b == SHORT) apps[current].press(BTN_B);
    apps[current].run();
    redrawNow = false;
    if (updater::tick()) startApp();      // a failed update drew over the app
  }
  checkBattery();
#ifdef BOARD_CYD
  status::draw(false);
#endif

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
