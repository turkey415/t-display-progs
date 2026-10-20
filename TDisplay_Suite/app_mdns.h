// mDNS Browser
// Lists what is announcing itself on the local network over multicast DNS (Bonjour): Chromecasts,
// AirPlay, printers, HomeKit, SSH and web servers, and so on. Grouped by service type, newest
// services expanding the list as answers arrive.
//
// It is also a wireless test: if nothing answers but the network clearly has devices, multicast
// is being dropped (a common Wi-Fi / AP-isolation problem), and the screen says so.
//
// Portrait (USB at the bottom) for a long list.
// Keys: BTN_B next page, BTN_A scan again now.
#pragma once
#include "mdns.h"

namespace mdnsbrowser {

const int ROW_H = 19, TOP = 22, ROWS = 14;
const int MAX_HITS = 60;

// The service types worth asking about at a LAN party / home network, with friendly names.
struct Svc { const char* type; const char* proto; const char* label; };
const Svc SERVICES[] = {
  {"_googlecast", "_tcp", "Cast"},
  {"_airplay",    "_tcp", "AirPlay"},
  {"_raop",       "_tcp", "AirPlay audio"},
  {"_spotify-connect", "_tcp", "Spotify"},
  {"_ipp",        "_tcp", "Printer"},
  {"_ipps",       "_tcp", "Printer"},
  {"_printer",    "_tcp", "Printer"},
  {"_pdl-datastream", "_tcp", "Printer"},
  {"_hap",        "_tcp", "HomeKit"},
  {"_homekit",    "_tcp", "HomeKit"},
  {"_ssh",        "_tcp", "SSH"},
  {"_sftp-ssh",   "_tcp", "SFTP"},
  {"_http",       "_tcp", "Web"},
  {"_https",      "_tcp", "Web"},
  {"_workstation","_tcp", "Computer"},
  {"_smb",        "_tcp", "File share"},
  {"_afpovertcp", "_tcp", "File share"},
  {"_nfs",        "_tcp", "File share"},
  {"_rfb",        "_tcp", "Screen share"},
  {"_daap",       "_tcp", "iTunes"},
  {"_companion-link", "_tcp", "Apple"},
  {"_steam",      "_tcp", "Steam"},
  {"_minecraft",  "_tcp", "Minecraft"},
};
const int NUM_SVC = sizeof(SERVICES) / sizeof(SERVICES[0]);

struct Hit {
  char label[18];     // friendly service name
  char name[26];      // instance (device) name
  char host[22];      // hostname or IP
  uint16_t port;
};
Hit hits[MAX_HITS];
int numHits = 0;

int  svcIdx = 0;          // which service type we are querying now
int  pageNo = 0;
bool started = false, mdnsReady = false;
uint32_t lastQuery = 0, sweepStart = 0;
int  sweeps = 0;          // completed full passes through the service list
mdns_search_once_t* search = nullptr;

bool haveHit(const char* name, const char* label) {
  for (int i = 0; i < numHits; i++)
    if (strcmp(hits[i].name, name) == 0 && strcmp(hits[i].label, label) == 0) return true;
  return false;
}

// Pull any finished async query's results in, then start the next service type.
void pump() {
  if (!mdnsReady || !wifiUp()) return;

  if (search) {
    mdns_result_t* res = nullptr;
    uint8_t n = 0;
    if (!mdns_query_async_get_results(search, 0, &res, &n)) return;   // not done yet
    for (mdns_result_t* r = res; r && numHits < MAX_HITS; r = r->next) {
      const char* nm = r->instance_name ? r->instance_name : (r->hostname ? r->hostname : "?");
      if (haveHit(nm, SERVICES[svcIdx].label)) continue;
      Hit& h = hits[numHits++];
      strlcpy(h.label, SERVICES[svcIdx].label, sizeof h.label);
      strlcpy(h.name, nm, sizeof h.name);
      h.port = r->port;
      h.host[0] = 0;
      if (r->addr && r->addr->addr.type == ESP_IPADDR_TYPE_V4) {
        esp_ip4_addr_t ip = r->addr->addr.u_addr.ip4;
        snprintf(h.host, sizeof h.host, "%u.%u.%u.%u",
                 esp_ip4_addr1_16(&ip), esp_ip4_addr2_16(&ip), esp_ip4_addr3_16(&ip), esp_ip4_addr4_16(&ip));
      } else if (r->hostname) {
        strlcpy(h.host, r->hostname, sizeof h.host);
      }
    }
    mdns_query_results_free(res);
    mdns_query_async_delete(search);
    search = nullptr;
    svcIdx++;
    if (svcIdx >= NUM_SVC) { svcIdx = 0; sweeps++; }
  }

  if (!search && millis() - lastQuery > 250) {   // a breath between queries
    lastQuery = millis();
    if (svcIdx == 0) sweepStart = millis();
    search = mdns_query_async_new(NULL, SERVICES[svcIdx].type, SERVICES[svcIdx].proto,
                                  MDNS_TYPE_PTR, 1200, 8, NULL);
    if (!search) svcIdx = (svcIdx + 1) % NUM_SVC;
  }
}

void render() {
  spr.fillSprite(C_BG);
  header("mDNS", String(numHits) + (sweeps ? "" : " ~"), C_ACC, C_TEXT);

  if (!numHits) {
    if (sweeps >= 1) {                 // one full pass with nothing: multicast is probably blocked
      centered("Nothing answered", 140, 2, C_WARN);
      centered("multicast may be blocked", 160, 1, C_DIM);
      centered("(AP isolation / no IGMP)", 174, 1, C_DIM);
    } else {
      centered("Listening...", 150, 2);
    }
    spr.drawFastHLine(0, 305, 170, C_HDR);
    spr.setTextFont(1); spr.setTextColor(C_DIM, C_BG); spr.setTextDatum(BL_DATUM);
    spr.drawString(wifiUp() ? WiFi.localIP().toString() : String("Wi-Fi down"), 3, 318);
    show();
    return;
  }

  int pages = (numHits + ROWS - 1) / ROWS;
  if (pageNo >= pages) pageNo = 0;
  for (int r = 0; r < ROWS; r++) {
    int i = pageNo * ROWS + r;
    if (i >= numHits) break;
    const Hit& h = hits[i];
    int y = TOP + r * ROW_H;
    spr.setTextFont(1);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(C_ACC, C_BG);
    spr.drawString(fit(h.label, 72, 1), 3, y + 1);          // service type, top line
    spr.setTextColor(C_DIM, C_BG);
    spr.setTextDatum(TR_DATUM);
    if (h.host[0]) spr.drawString(fit(String(h.host) + (h.port ? ":" + String(h.port) : ""), 92, 1), 168, y + 1);
    spr.setTextFont(2);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(fit(h.name, 165, 2), 3, y + 12);          // device name, bottom line
    spr.drawFastHLine(0, y + ROW_H - 1, 170, FAINT(0x1082));
  }

  spr.drawFastHLine(0, 305, 170, C_HDR);
  spr.setTextFont(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(BL_DATUM);
  spr.drawString(sweeps ? "scan " + String(sweeps) : "scanning...", 3, 318);
  spr.setTextDatum(BR_DATUM);
  spr.drawString(String(min(pageNo, pages - 1) + 1) + "/" + String(pages), 167, 318);
  show();
}

void start() {
  setPortrait(true);
  pageNo = 0;
  if (!mdnsReady) { mdnsReady = (mdns_init() == ESP_OK); }
  started = true;
  render();
}

void run() {
  pump();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 400) { lastDraw = millis(); render(); }
}

void press(int pin) {
  if (pin == BTN_B) {
    pageNo++;
  } else {
    numHits = 0; sweeps = 0; svcIdx = 0;      // fresh scan
    if (search) { mdns_query_async_delete(search); search = nullptr; }
  }
  render();
}

}  // namespace mdnsbrowser
