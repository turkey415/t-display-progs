// Updates over Wi-Fi from this repo's GitHub releases.
// Each release carries TDisplay_Suite.bin and version.txt. The device reads version.txt from
// the latest release and, when it is newer than its own, downloads and installs the .bin,
// then restarts. Settings and Wi-Fi are kept (they live in a separate part of the flash).
// Checked at power-on and once a day when auto update is on, and from the menu at any time.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "version.h"

namespace updater {

const char* RELEASES = "https://github.com/turkey415/t-display-progs/releases/latest/download/";
#if defined(BOARD_CYD35)
const char* BIN_NAME = "CYD35_Suite.bin";      // the 3.5" CYD
#elif defined(BOARD_CYD)
const char* BIN_NAME = "CYD_Suite.bin";        // the CYD has its own build in each release
#else
const char* BIN_NAME = "TDisplay_Suite.bin";
#endif
const uint32_t EVERY_MS = 24UL * 60UL * 60UL * 1000UL;

uint32_t lastCheck = 0;
bool     drew = false;       // screen() was called

// "1.10.2" > "1.9.9": compared number by number.
bool newer(const String& a, const String& b) {
  int pa[3] = {0}, pb[3] = {0};
  sscanf(a.c_str(), "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b.c_str(), "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (int i = 0; i < 3; i++) if (pa[i] != pb[i]) return pa[i] > pb[i];
  return false;
}

void screen(const String& title, const String& line1, const String& line2 = "", uint16_t col = C_DIM) {
  setPortrait(false);
  drew = true;
  spr.fillSprite(C_BG);
  header("SOFTWARE UPDATE", "v" FW_VERSION);
  centered(title, 60, 4, C_TEXT);
  centered(line1, 96, 2, col);
  centered(line2, 116, 2);
  show();
}

String latest(String& problem) {
  WiFiClientSecure client;
  client.setInsecure();     // see the README: the download is checked as a firmware image, not signed
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub sends release files from another host
  http.setTimeout(8000);
  if (!http.begin(client, String(RELEASES) + "version.txt")) { problem = "Bad update address"; return ""; }
  int code = http.GET();
  String v = code == 200 ? http.getString() : "";
  http.end();
  v.trim();
  if (v.startsWith("v")) v.remove(0, 1);
  if (code != 200) problem = code == 404 ? "No release on GitHub yet" : code < 0 ? "Can't reach GitHub" : "GitHub said HTTP " + String(code);
  return v;
}

// Downloads the release .bin and installs it. Returns why it failed (on success it restarts).
// Done here rather than with httpUpdate: that checks the first byte (0xE9) the moment the
// headers arrive, without waiting for the body, so over TLS from GitHub it often sees nothing
// yet and fails with "Verify Bin Header Failed". This waits for the data first.
String install(const String& v) {
  screen("Updating to v" + v, "Don't unplug", "Downloading...", C_WARN);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, String(RELEASES) + BIN_NAME)) return "Bad update address";
  int code = http.GET();
  if (code != 200) { http.end(); return code == 404 ? "No update file" : code < 0 ? "Can't reach GitHub" : "GitHub said HTTP " + String(code); }
  int total = http.getSize();
  if (total <= 0) { http.end(); return "GitHub sent no file size"; }

  WiFiClient* in = http.getStreamPtr();
  uint32_t t0 = millis();
  while (!in->available() && in->connected() && millis() - t0 < 15000) delay(20);
  if (in->peek() != 0xE9) { http.end(); return in->available() ? "Download is not firmware" : "Download never started"; }

  if (!Update.begin(total, U_FLASH)) { http.end(); return Update.errorString(); }
  Update.onProgress([v](size_t done, size_t all) {
    static int lastPct = -1;
    int pct = all > 0 ? done * 100 / all : 0;
    if (pct == lastPct) return;
    lastPct = pct;
    screen("Updating to v" + v, "Don't unplug", String(pct) + "%", C_WARN);
    spr.drawRect(40, 136, 240, 12, C_DIM);
    spr.fillRect(42, 138, 236 * pct / 100, 8, C_WARN);
    show();
  });
  size_t wrote = Update.writeStream(*in);
  http.end();
  if (wrote != (size_t)total || !Update.end(true)) {
    String why = Update.hasError() ? String(Update.errorString()) : "Download cut off";
    Update.abort();
    return why;
  }
  screen("Updated to v" + v, "Restarting...", "", C_GOOD);
  delay(1000);
  ESP.restart();
  return "";
}

// Checks, and installs if there is something newer. `shown` puts up a result even when there is
// nothing to do (from the menu); a daily check stays quiet unless it is installing.
void check(bool shown) {
  lastCheck = millis();
  if (!wifiUp()) { if (shown) { screen("No Wi-Fi", "Can't check for updates"); delay(2500); } return; }
  if (shown) screen("Checking...", "github.com/turkey415/t-display-progs");

  String problem;
  String v = latest(problem);
  Serial.printf("update: running %s, latest %s %s\n", FW_VERSION, v.c_str(), problem.c_str());
  if (!v.length()) { if (shown) { screen("Can't check", problem, "", C_BAD); delay(3000); } return; }
  if (!newer(v, FW_VERSION)) { if (shown) { screen("Up to date", "v" FW_VERSION " is the newest", "", C_GOOD); delay(2500); } return; }

  String why = install(v);
  // Only comes back if it failed; on success the device has restarted into the new version.
  Serial.printf("update failed: %s\n", why.c_str());
  screen("Update failed", why, "Still running v" FW_VERSION, C_BAD);
  delay(4000);
}

// From loop(): the daily check. True if it put something on the screen (a failed update).
bool tick() {
  if (!cfg.autoUpdate || !wifiUp() || millis() - lastCheck < EVERY_MS) return false;
  drew = false;
  check(false);
  return drew;                            // only when an update was tried and failed
}

}  // namespace updater
