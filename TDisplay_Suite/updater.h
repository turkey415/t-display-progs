// Updates over Wi-Fi from this repo's GitHub releases.
// Each release carries TDisplay_Suite.bin and version.txt. The device reads version.txt from
// the latest release and, when it is newer than its own, downloads and installs the .bin,
// then restarts. Settings and Wi-Fi are kept (they live in a separate part of the flash).
// Checked at power-on and once a day when auto update is on, and from the menu at any time.
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include "version.h"

namespace updater {

const char* RELEASES = "https://github.com/turkey415/t-display-progs/releases/latest/download/";
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
  spr.pushSprite(0, 0);
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

  screen("Updating to v" + v, "Don't unplug", "Downloading...", C_WARN);
  httpUpdate.onProgress([v](int done, int total) {
    static int lastPct = -1;
    int pct = total > 0 ? done * 100 / total : 0;
    if (pct == lastPct) return;
    lastPct = pct;
    screen("Updating to v" + v, "Don't unplug", String(pct) + "%", C_WARN);
    spr.drawRect(40, 136, 240, 12, C_DIM);
    spr.fillRect(42, 138, 236 * pct / 100, 8, C_WARN);
    spr.pushSprite(0, 0);
  });
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  WiFiClientSecure client;
  client.setInsecure();
  t_httpUpdate_return r = httpUpdate.update(client, String(RELEASES) + "TDisplay_Suite.bin");
  // Only comes back if it failed; on success the device has restarted into the new version.
  String why = r == HTTP_UPDATE_NO_UPDATES ? "No update file" : httpUpdate.getLastErrorString();
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
