// Ping Graph
// A scrolling latency graph to one target, with jitter, loss and a spike count: the screen for
// "is the game server lagging, and when?". Pings about twice a second and plots each result.
// Targets come from the LAN Sentinel host list (so the game / GXL server is one tap away);
// the gateway is always first.
// Keys: BTN_B next target, BTN_A reset the stats and graph.
#pragma once
#include <ESP32Ping.h>

namespace pinggraph {

const int W = 320, GTOP = 24, GBOT = 150, GH = GBOT - GTOP;   // graph box
const int MAXPT = W;                                          // one point per column
const uint32_t GAP_MS = 500;

struct Tgt { String label; String target; };
Tgt      tgts[8];
int      numTgts = 0, cur = 0;
String   builtFrom;

float    pts[MAXPT];        // ms, or -1 for a lost ping
int      nPts = 0;
IPAddress ip;
bool     resolved = false;
uint32_t lastPing = 0;

// running stats since the last reset
int      sent = 0, lost = 0;
float    sumMs = 0, minMs = 1e9, maxMs = 0;
float    prevMs = -1, sumJit = 0; int jitN = 0;
int      spikes = 0;       // pings over the spike threshold
float    spikeAt = 100;    // ms

void resetStats() {
  nPts = 0; sent = lost = 0; sumMs = 0; minMs = 1e9; maxMs = 0;
  prevMs = -1; sumJit = 0; jitN = 0; spikes = 0;
}

// Pull the targets from the Sentinel list; gateway first, skip bare "GW" duplicates.
void buildTargets() {
  String list = cfg.hosts.length() ? cfg.hosts : String(sentinel::DEFAULT_HOSTS);   // same list as LAN Sentinel
  if (list == builtFrom && numTgts) return;
  builtFrom = list;
  numTgts = 0;
  tgts[numTgts++] = {"Gateway", "GW"};
  int pos = 0;
  while (pos < (int)list.length() && numTgts < 8) {
    int comma = list.indexOf(',', pos);
    if (comma < 0) comma = list.length();
    String item = list.substring(pos, comma); pos = comma + 1;
    item.trim();
    int eq = item.indexOf('=');
    String label = eq < 0 ? item : item.substring(0, eq);
    String tg = eq < 0 ? item : item.substring(eq + 1);
    label.trim(); tg.trim();
    if (!tg.length() || tg.equalsIgnoreCase("GW")) continue;
    if (numTgts >= (int)(sizeof tgts / sizeof tgts[0])) break;
    tgts[numTgts++] = {label, tg};
  }
}

void resolve() {
  resolved = false;
  Tgt& t = tgts[cur];
  if (t.target.equalsIgnoreCase("GW")) { ip = WiFi.gatewayIP(); resolved = ip != IPAddress(0,0,0,0); return; }
  if (ip.fromString(t.target)) { resolved = true; return; }
  if (wifiUp()) resolved = WiFi.hostByName(t.target.c_str(), ip) == 1;
}

void addPoint(float ms) {
  if (nPts < MAXPT) pts[nPts++] = ms;
  else { memmove(pts, pts + 1, (MAXPT - 1) * sizeof(float)); pts[MAXPT - 1] = ms; }
  sent++;
  if (ms < 0) { lost++; prevMs = -1; return; }
  sumMs += ms; minMs = min(minMs, ms); maxMs = max(maxMs, ms);
  if (ms > spikeAt) spikes++;
  if (prevMs >= 0) { sumJit += fabs(ms - prevMs); jitN++; }
  prevMs = ms;
}

void doPing() {
  if (!resolved) { resolve(); if (!resolved) { addPoint(-1); return; } }
  bool ok = wifiUp() && Ping.ping(ip, 1);
  addPoint(ok ? Ping.averageTime() : -1);
}

void render() {
  spr.fillSprite(C_BG);
  Tgt& t = tgts[cur];
  float avg = (sent - lost) ? sumMs / (sent - lost) : 0;
  float jit = jitN ? sumJit / jitN : 0;
  int lossPct = sent ? (lost * 100 + sent / 2) / sent : 0;

  header("PING  " + t.label, resolved ? ip.toString() : String("resolving..."), C_ACC, C_DIM);

  // y-scale: round the max up to something tidy, floor of 50 ms so a quiet LAN still reads well.
  float top = max(50.0f, maxMs * 1.2f);
  // gridlines + labels
  spr.setTextFont(1); spr.setTextDatum(ML_DATUM); spr.setTextColor(C_DIM, C_BG);
  for (int g = 0; g <= 2; g++) {
    int y = GBOT - g * GH / 2;
    spr.drawFastHLine(0, y, W, FAINT(0x1082));
    spr.drawString(String((int)(top * g / 2)) + "ms", 2, y - 5);
  }
  int spikeY = GBOT - (int)(constrain(spikeAt / top, 0.f, 1.f) * GH);
  for (int x = 40; x < W; x += 8) spr.drawPixel(x, spikeY, C_SLOW);   // dashed spike line

  // the trace: newest on the right
  for (int i = 0; i < nPts; i++) {
    int x = W - nPts + i;
    if (x < 0) continue;
    float ms = pts[i];
    if (ms < 0) { spr.drawFastVLine(x, GTOP, GH, 0x3000); continue; }   // lost = faint red column
    int y = GBOT - (int)(constrain(ms / top, 0.f, 1.f) * GH);
    uint16_t c = ms > spikeAt ? C_SLOW : ms > spikeAt / 2 ? C_WARN : C_GOOD;
    spr.drawFastVLine(x, y, GBOT - y, c);
  }

  // stats strip along the bottom: four key/value columns
  spr.drawFastHLine(0, GBOT, W, C_HDR);
  float last = nPts ? pts[nPts - 1] : -1;
  struct { const char* k; String v; uint16_t c; } cols[4] = {
    {"now",  last < 0 ? String("LOST") : String(last, 0) + "ms", last < 0 ? C_BAD : (last > spikeAt ? C_SLOW : C_GOOD)},
    {"avg",  String(avg, 0) + "ms", C_TEXT},
    {"jit",  String(jit, 1) + "ms", jit > 10 ? C_WARN : C_TEXT},
    {"loss", String(lossPct) + "%", lossPct ? C_BAD : C_GOOD},
  };
  int x = 4;
  for (auto& c : cols) {
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(C_DIM, C_BG); spr.setTextFont(1);  spr.drawString(c.k, x, 152);
    spr.setTextColor(c.c, C_BG);   spr.setTextFont(2);  spr.drawString(c.v, x, 160);
    x += 80;
  }
  // spike count, just under the header on the right so it clears the battery badge
  spr.setTextFont(1); spr.setTextDatum(TR_DATUM); spr.setTextColor(C_SLOW, C_BG);
  spr.drawString(String(spikes) + " spikes >" + String((int)spikeAt) + "ms", W - 2, 22);
  show();
}

void start() {
  buildTargets();
  resetStats();
  resolve();
  render();
}

void run() {
  if (millis() - lastPing > GAP_MS) {
    lastPing = millis();
    doPing();
    render();
  }
}

void press(int pin) {
  if (pin == BTN_B) {
    buildTargets();
    cur = (cur + 1) % numTgts;
    resetStats();
    resolve();
  } else {
    resetStats();
  }
  render();
}

}  // namespace pinggraph
