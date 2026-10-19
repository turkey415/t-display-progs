// Wireless Sniffer
// Portrait (USB at the bottom) for a long list. Two views:
//   Wi-Fi     : every network in range, strongest first: signal, name, channel, security
//   Bluetooth : every BLE device advertising nearby: signal, name or maker, address
// The ESP32-S3 hears 2.4 GHz Wi-Fi only (no 5 GHz) and Bluetooth Low Energy only (no Classic).
// Only listens; it never joins or connects to anything it finds.
// Keys: BTN_B Wi-Fi / Bluetooth, BTN_A next page of the list.
#pragma once
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

namespace sniffer {

const int ROW_H = 19, TOP = 22, ROWS = 14;   // 14 rows of 19 px under the header
const int MAX_NETS = 80, MAX_DEVS = 80;
const uint32_t BLE_FORGET_MS = 30UL * 1000UL;  // a device not heard for this long is dropped

int view = 0;      // 0 Wi-Fi, 1 Bluetooth
int pageNo = 0;

// ---------- Wi-Fi ----------
struct Net { char ssid[33]; uint8_t bssid[6]; int rssi, ch, auth; };
Net      nets[MAX_NETS];
int      numNets = 0;
uint32_t scannedAt = 0, scanStart = 0;
bool     scanning = false, everScanned = false;

void wifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (scanning && n >= 0) {
    numNets = 0;
    for (int i = 0; i < n && numNets < MAX_NETS; i++) {
      Net& t = nets[numNets++];
      strlcpy(t.ssid, WiFi.SSID(i).c_str(), sizeof t.ssid);
      memcpy(t.bssid, WiFi.BSSID(i), 6);
      t.rssi = WiFi.RSSI(i);
      t.ch = WiFi.channel(i);
      t.auth = WiFi.encryptionType(i);
    }
    std::sort(nets, nets + numNets, [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
    scanning = false;
    everScanned = true;
    scannedAt = millis();
    Serial.printf("sniffer: %d Wi-Fi networks in %lu ms\n", numNets, millis() - scanStart);
  }
  if (!scanning && millis() - scannedAt > 1000) {     // start the next sweep a second later
    WiFi.scanNetworks(true, true);                     // in the background, hidden networks too
    scanning = true;
    scanStart = millis();
  }
}

// ---------- Bluetooth ----------
struct Dev { char addr[18]; char name[24]; int rssi; uint16_t maker; bool hasMaker; uint32_t seen; };
Dev      devs[MAX_DEVS];
int      numDevs = 0;
portMUX_TYPE devLock = portMUX_INITIALIZER_UNLOCKED;
bool     bleReady = false;
volatile bool bleScanning = false;
BLEScan* ble = nullptr;

// Bluetooth SIG company IDs for makers that show up a lot.
const char* makerName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0087: return "Garmin";
    case 0x0171: return "Amazon";
    case 0x0059: return "Nordic";
    case 0x02E5: return "Espressif";
    case 0x000F: return "Broadcom";
    case 0x01DA: return "Logitech";
    default:     return nullptr;
  }
}

// Called from the Bluetooth task for every advertisement heard (repeats too, for fresh RSSI).
class Heard : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String addr = d.getAddress().toString().c_str();
    String name = d.haveName() ? String(d.getName().c_str()) : String();
    bool hasMaker = false;
    uint16_t maker = 0;
    if (d.haveManufacturerData()) {
      String m = d.getManufacturerData();
      if (m.length() >= 2) { maker = (uint8_t)m[0] | ((uint8_t)m[1] << 8); hasMaker = true; }
    }
    int rssi = d.getRSSI();
    portENTER_CRITICAL(&devLock);
    int i = 0;
    while (i < numDevs && strcmp(devs[i].addr, addr.c_str()) != 0) i++;
    if (i == numDevs && numDevs < MAX_DEVS) { numDevs++; devs[i] = Dev(); strlcpy(devs[i].addr, addr.c_str(), sizeof devs[i].addr); }
    if (i < numDevs) {
      Dev& t = devs[i];
      t.rssi = rssi;
      t.seen = millis();
      if (name.length()) strlcpy(t.name, name.c_str(), sizeof t.name);
      if (hasMaker) { t.maker = maker; t.hasMaker = true; }
    }
    portEXIT_CRITICAL(&devLock);
  }
};
Heard heard;

void bleDone(BLEScanResults) { bleScanning = false; }

void bleScan() {
  if (!bleReady) {
    BLEDevice::init("");
    ble = BLEDevice::getScan();
    ble->setAdvertisedDeviceCallbacks(&heard, true);
    ble->setActiveScan(true);        // ask for scan responses, which is where most names are
    ble->setInterval(100);
    ble->setWindow(99);
    bleReady = true;
  }
  if (!bleScanning) {
    Serial.printf("sniffer: %d Bluetooth devices\n", numDevs);
    ble->clearResults();
    bleScanning = ble->start(5, bleDone, false);
  }
  // forget devices gone quiet
  portENTER_CRITICAL(&devLock);
  for (int i = 0; i < numDevs;) {
    if (millis() - devs[i].seen > BLE_FORGET_MS) devs[i] = devs[--numDevs];
    else i++;
  }
  portEXIT_CRITICAL(&devLock);
}

// ---------- Drawing ----------
void footer(const String& left, int total) {
  int pages = max(1, (total + ROWS - 1) / ROWS);
  spr.drawFastHLine(0, 305, 170, C_HDR);
  spr.setTextFont(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(BL_DATUM);
  spr.drawString(left, 3, 318);
  spr.setTextDatum(BR_DATUM);
  spr.drawString(String(min(pageNo, pages - 1) + 1) + "/" + String(pages), 167, 318);
}

void drawWifi() {
  header("WI-FI", String(numNets) + (scanning ? " ~" : ""), C_ACC, C_TEXT);
  if (!everScanned) { centered("Scanning...", 150, 2); footer("2.4 GHz only", 0); return; }
  if (!numNets) { centered("No networks", 150, 2); footer("2.4 GHz only", 0); return; }
  int pages = (numNets + ROWS - 1) / ROWS;
  if (pageNo >= pages) pageNo = 0;
  uint8_t mine[6] = {0};
  if (wifiUp()) memcpy(mine, WiFi.BSSID(), 6);
  for (int r = 0; r < ROWS; r++) {
    int i = pageNo * ROWS + r;
    if (i >= numNets) break;
    const Net& t = nets[i];
    int y = TOP + r * ROW_H;
    bool connected = wifiUp() && memcmp(t.bssid, mine, 6) == 0;
    if (connected) spr.fillRect(0, y, 170, ROW_H - 1, 0x0220);       // the one we're on
    drawBars(2, y + 14, t.rssi);
    bool hidden = !t.ssid[0];
    uint16_t c = hidden ? C_DIM : t.auth == WIFI_AUTH_OPEN ? C_WARN : t.auth == WIFI_AUTH_WEP ? C_BAD : C_TEXT;
    spr.setTextFont(2);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(c);
    spr.drawString(fit(hidden ? "(hidden)" : t.ssid, 98, 2), 17, y + 9);
    spr.setTextFont(1);
    spr.setTextDatum(MR_DATUM);
    spr.setTextColor(C_DIM);
    spr.drawString(String(t.ch), 136, y + 9);
    spr.setTextFont(2);
    spr.setTextColor(rssiColor(t.rssi));
    spr.drawString(String(t.rssi), 168, y + 9);
  }
  int open = 0;
  for (int i = 0; i < numNets; i++) open += nets[i].auth == WIFI_AUTH_OPEN;
  footer("ch | dBm  " + String(open) + " open", numNets);
}

void drawBle() {
  static Dev copy[MAX_DEVS];     // not on the stack: 80 devices is about 4.5 KB
  portENTER_CRITICAL(&devLock);
  int n = numDevs;
  memcpy(copy, devs, n * sizeof(Dev));
  portEXIT_CRITICAL(&devLock);
  std::sort(copy, copy + n, [](const Dev& a, const Dev& b) { return a.rssi > b.rssi; });

  header("BLUETOOTH", String(n), 0x5D1F, C_TEXT);
  if (!n) { centered(bleReady ? "Listening..." : "Starting...", 150, 2); footer("BLE only", 0); return; }
  int pages = (n + ROWS - 1) / ROWS;
  if (pageNo >= pages) pageNo = 0;
  for (int r = 0; r < ROWS; r++) {
    int i = pageNo * ROWS + r;
    if (i >= n) break;
    const Dev& d = copy[i];
    int y = TOP + r * ROW_H;
    drawBars(2, y + 14, d.rssi);
    const char* maker = d.hasMaker ? makerName(d.maker) : nullptr;
    String label = d.name[0] ? String(d.name) : maker ? String(maker) : String(d.addr).substring(0, 14);
    spr.setTextFont(2);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(d.name[0] ? C_TEXT : maker ? C_ACC : C_DIM);
    spr.drawString(fit(label, 118, 2), 17, y + 9);
    spr.setTextDatum(MR_DATUM);
    spr.setTextColor(rssiColor(d.rssi));
    spr.drawString(String(d.rssi), 168, y + 9);
  }
  int named = 0;
  for (int i = 0; i < n; i++) named += copy[i].name[0] != 0;
  footer(String(named) + " named, BLE only", n);
}

void render() {
  spr.fillSprite(C_BG);
  if (view == 0) drawWifi(); else drawBle();
  show();
}

// ---------- App ----------
void start() {
  setPortrait(true);
  pageNo = 0;
  render();
}

void run() {
  if (view == 0) wifiScan(); else bleScan();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 500) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) {
    view ^= 1;
    pageNo = 0;
  } else {
    pageNo++;                                   // wraps in the drawing
  }
  render();
}

}  // namespace sniffer
