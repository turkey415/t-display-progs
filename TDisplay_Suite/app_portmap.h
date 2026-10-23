// Switch Port Map
// A live front panel of a managed switch (made for the LAN party's Catalyst 3560s), read over
// SNMP v2c: every port as a jack with its link LED, in the switch's own layout (odd ports on
// top, blocks of 12, uplinks set apart).
//   LED colour: green = link, amber = 10M, orange = half duplex, dark = no link. It flickers with traffic,
//   blinks red when the port's input errors are climbing. A blue jack is a trunk, a struck-out
//   one is shut down, a yellow dot is PoE being delivered.
// The selected port's panel shows its description, VLAN or trunk, speed and duplex, PoE class,
// the CDP neighbour, errors, in/out LED meters (log scale, up to the port's speed) and a three
// minute traffic graph. A second view ranks the busiest ports.
//
// Setup page: "Switches for Port Map" (Name=IP,IP ...) and the SNMP community. The switch has
// to allow SNMP from the display's Wi-Fi address (check any snmp-server ACL).
//
// Keys: tap a port to select it; tap the bottom panel for the next view (port / top talkers);
// tap the header for the next switch. BOOT (BTN_A) = the next port with a link.
// T-Display: top key = next port with a link, bottom key = next view, then next switch.
#pragma once
#include <WiFiUdp.h>
#include <algorithm>

namespace portmap {

// ---------- Colours: the Perf-Deck palette ----------
const uint16_t P_BG     = 0x1082;   // #111110
const uint16_t P_PANEL  = 0x18C3;   // #1b1b1a
const uint16_t P_RAISED = 0x2124;   // chassis top
const uint16_t P_WELL   = 0x0861;   // #0d0d0c
const uint16_t P_LEDOFF = 0x2945;   // #2a2a28
const uint16_t P_LINE   = 0x39C6;   // #3a3a37
const uint16_t P_GRID   = 0x2965;   // #2c2c2a
const uint16_t P_TEXT   = 0xF77D;   // #f0efec
const uint16_t P_MUTED  = 0x8C30;   // #898781
const uint16_t P_AMBER  = 0xFD80;   // #ffb000
const uint16_t P_GREEN  = 0x3EEB;   // #3ddc5a
const uint16_t P_WARN   = 0xFD83;   // #fab219
const uint16_t P_ORANGE = 0xFC47;   // #ff8a3d
const uint16_t P_RED    = 0xFA69;   // #ff4d4d
const uint16_t P_BLUE   = 0x3C3C;   // #3987e5

uint16_t mix(uint16_t a, uint16_t b, int pct) {      // pct% of a over b
  int r = ((a >> 11) * pct + (b >> 11) * (100 - pct)) / 100;
  int g = (((a >> 5) & 63) * pct + ((b >> 5) & 63) * (100 - pct)) / 100;
  int l = ((a & 31) * pct + (b & 31) * (100 - pct)) / 100;
  return r << 11 | g << 5 | l;
}

// The 2.8" CYD's 8-bit buffer turns very dark greys black: lift them there.
uint16_t dk(uint16_t c) {
#if defined(BOARD_CYD) && !defined(BOARD_CYD35)
  return c < 0x2104 ? 0x2108 : c;
#else
  return c;
#endif
}

// ---------- Ports ----------
const int MAX_PORTS = 56;            // 48 + 4 uplinks, and a few spare
const int HIST      = 60;            // traffic samples per port (one per fast poll)
const uint32_t FAST_MS = 3000;       // link, counters, errors
const uint32_t SLOW_MS = 60000;      // names, VLANs, CDP, PoE

struct Port {
  uint32_t ifIndex;
  char  name[12];                    // Gi0/14
  char  alias[33];                   // the port description
  char  cdp[28], cdpPort[14];        // CDP neighbour and its port
  uint8_t oper, admin;               // 1 up, 2 down
  uint8_t duplex;                    // 2 half, 3 full
  uint8_t poe;                       // 0 none, 1 searching/off, 3 delivering, 4 fault
  uint8_t poeClass;                  // 1..5 = class 0..4
  bool  trunk, uplink;
  uint16_t vlan;
  uint32_t speed;                    // Mbps
  uint64_t inOct, outOct;            // last counters
  uint32_t inErr, errAt;             // input errors; when they last went up (millis)
  float inBps, outBps;
  uint8_t hIn[HIST], hOut[HIST];     // log10(bps) * 25
  int8_t col, row;                   // place on the panel
};

Port ports[MAX_PORTS];
int  numPorts = 0, numAccess = 0, numUplink = 0;
int  histN = 0;                      // samples recorded so far (all ports together)
char sysName[40] = "", model[28] = "";
uint32_t upSecs = 0;
int  poeUsed = -1, poeTotal = -1;    // watts

SemaphoreHandle_t lock = nullptr;
TaskHandle_t task = nullptr;
volatile uint32_t lastRun = 0;       // the app drew recently: the poller keeps going

// Poll state, for the header lamp and messages.
enum PollState { P_IDLE, P_WAIT, P_OK, P_NOREPLY, P_NOCONFIG };
volatile PollState state = P_NOCONFIG;
volatile uint32_t lastOk = 0, pollAt = 0;
char lastErr[48] = "";

// Switches from the setup page: "Name=IP,IP".
const int MAX_SW = 6;
String swName[MAX_SW], swIp[MAX_SW];
int numSw = 0, curSw = 0;
volatile bool swChanged = false;

int sel = -1;                        // selected port
int view = 0;                        // 0 port detail, 1 top talkers

// ---------- SNMP v2c (just enough: GET and GETBULK walks) ----------
namespace snmp {

struct Oid { uint32_t a[20]; uint8_t n; };

Oid parse(const char* s) {
  Oid o; o.n = 0;
  while (*s && o.n < 20) { o.a[o.n++] = strtoul(s, (char**)&s, 10); if (*s == '.') s++; }
  return o;
}

bool under(const Oid& o, const Oid& pre) {
  if (o.n <= pre.n) return false;
  for (int i = 0; i < pre.n; i++) if (o.a[i] != pre.a[i]) return false;
  return true;
}

int cmp(const Oid& x, const Oid& y) {
  for (int i = 0; i < x.n && i < y.n; i++) if (x.a[i] != y.a[i]) return x.a[i] < y.a[i] ? -1 : 1;
  return x.n - y.n;
}

struct Val {
  uint8_t type;                      // 0x02 int, 0x04 string, 0x41 counter, 0x42 gauge, 0x43 ticks, 0x46 counter64, 0x80-0x82 none
  uint64_t num;
  const uint8_t* str;
  int len;
  void copy(char* dst, int cap) const {
    int n = min(len, cap - 1), j = 0;
    for (int i = 0; i < n; i++) if (str[i] >= 32 && str[i] < 127) dst[j++] = str[i];
    dst[j] = 0;
  }
};

// ---- encoding (into a buffer, back to front would be cleverer; requests are tiny) ----
int putLen(uint8_t* p, int len) {
  if (len < 128) { p[0] = len; return 1; }
  if (len < 256) { p[0] = 0x81; p[1] = len; return 2; }
  p[0] = 0x82; p[1] = len >> 8; p[2] = len & 0xFF; return 3;
}

int putTlv(uint8_t* p, uint8_t tag, const uint8_t* body, int len) {
  p[0] = tag;
  int h = 1 + putLen(p + 1, len);
  memmove(p + h, body, len);
  return h + len;
}

int putInt(uint8_t* p, int32_t v) {
  uint8_t b[5]; int n = 0;
  uint32_t u = (uint32_t)v;
  // minimal two's complement bytes
  for (int i = 3; i >= 0; i--) {
    uint8_t byte = u >> (i * 8);
    if (n == 0 && i > 0) {
      uint8_t next = u >> ((i - 1) * 8);
      if ((byte == 0x00 && !(next & 0x80)) || (byte == 0xFF && (next & 0x80))) continue;
    }
    b[n++] = byte;
  }
  return putTlv(p, 0x02, b, n);
}

int putOid(uint8_t* p, const Oid& o) {
  uint8_t b[96]; int n = 0;
  b[n++] = o.a[0] * 40 + o.a[1];
  for (int i = 2; i < o.n; i++) {
    uint32_t v = o.a[i];
    uint8_t tmp[5]; int k = 0;
    do { tmp[k++] = v & 0x7F; v >>= 7; } while (v);
    while (k--) b[n++] = tmp[k] | (k ? 0x80 : 0);
  }
  return putTlv(p, 0x06, b, n);
}

// A request: pdu 0xA0 GET (oids as is) or 0xA5 GETBULK (max-repetitions rep).
int build(uint8_t* out, const char* comm, uint8_t pduType, uint32_t id, int rep, const Oid* oids, int n) {
  static uint8_t vbl[700], pdu[760], msg[820];
  int vl = 0;
  for (int i = 0; i < n; i++) {
    uint8_t vb[128]; int k = putOid(vb, oids[i]);
    vb[k++] = 0x05; vb[k++] = 0x00;                          // NULL value
    vl += putTlv(vbl + vl, 0x30, vb, k);
  }
  int pl = 0;
  pl += putInt(pdu + pl, (int32_t)id);
  pl += putInt(pdu + pl, 0);                               // non-repeaters / error-status
  pl += putInt(pdu + pl, pduType == 0xA5 ? rep : 0);       // max-repetitions / error-index
  pl += putTlv(pdu + pl, 0x30, vbl, vl);
  int ml = 0;
  ml += putInt(msg + ml, 1);                               // version: v2c
  ml += putTlv(msg + ml, 0x04, (const uint8_t*)comm, strlen(comm));
  ml += putTlv(msg + ml, pduType, pdu, pl);
  return putTlv(out, 0x30, msg, ml);
}

// ---- decoding ----
struct Rd {
  const uint8_t* p; int n, i;
  bool tl(uint8_t& tag, int& len) {
    if (i + 2 > n) return false;
    tag = p[i++];
    int l = p[i++];
    if (l & 0x80) {
      int k = l & 0x7F; l = 0;
      if (k > 3 || i + k > n) return false;
      while (k--) l = l << 8 | p[i++];
    }
    len = l;
    return i + len <= n;
  }
  bool num(uint8_t want, int64_t& v) {
    uint8_t t; int l;
    if (!tl(t, l) || t != want) return false;
    v = (l && (p[i] & 0x80)) ? -1 : 0;
    for (int k = 0; k < l; k++) v = (v << 8) | p[i++];
    return true;
  }
};

bool decOid(const uint8_t* p, int len, Oid& o) {
  o.n = 0;
  if (len < 1) return false;
  o.a[o.n++] = p[0] / 40; o.a[o.n++] = p[0] % 40;
  uint32_t v = 0;
  for (int i = 1; i < len; i++) {
    v = v << 7 | (p[i] & 0x7F);
    if (!(p[i] & 0x80)) { if (o.n >= 20) return false; o.a[o.n++] = v; v = 0; }
  }
  return true;
}

// Parses a response. Calls fn for each varbind. Returns the error-status, or -1 if malformed.
template <typename F>
int parse(const uint8_t* buf, int len, uint32_t wantId, F fn) {
  Rd r{buf, len, 0};
  uint8_t t; int l; int64_t v;
  if (!r.tl(t, l) || t != 0x30) return -1;
  if (!r.num(0x02, v)) return -1;                    // version
  if (!r.tl(t, l) || t != 0x04) return -1;           // community
  r.i += l;
  if (!r.tl(t, l) || t != 0xA2) return -1;           // response PDU
  if (!r.num(0x02, v) || (uint32_t)v != wantId) return -2;
  int64_t es, ei;
  if (!r.num(0x02, es) || !r.num(0x02, ei)) return -1;
  if (!r.tl(t, l) || t != 0x30) return -1;
  int end = r.i + l, idx = 0;
  while (r.i < end) {
    if (!r.tl(t, l) || t != 0x30) return -1;
    int vbEnd = r.i + l;
    if (!r.tl(t, l) || t != 0x06) return -1;
    Oid o;
    if (!decOid(r.p + r.i, l, o)) return -1;
    r.i += l;
    Val val{};
    if (!r.tl(t, l)) return -1;
    val.type = t; val.str = r.p + r.i; val.len = l;
    if (t == 0x02 || t == 0x41 || t == 0x42 || t == 0x43 || t == 0x46) {
      uint64_t u = (t == 0x02 && l && (r.p[r.i] & 0x80)) ? ~0ULL : 0;
      for (int k = 0; k < l; k++) u = (u << 8) | r.p[r.i + k];
      val.num = u;
    }
    r.i = vbEnd;
    fn(idx++, o, val);
  }
  return (int)es;
}

WiFiUDP udp;
bool udpOpen = false;
uint32_t nextId = 1;
uint16_t port = 161;                 // the agent's port ("IP:port" in the switch list)
uint8_t rx[1600];
uint8_t tx[900];

// Sends a request and waits for its reply. Returns the reply length, 0 on no reply.
int exchange(IPAddress ip, int len, uint32_t id) {
  if (!udpOpen) { udp.begin(16161); udpOpen = true; }
  for (int attempt = 0; attempt < 2; attempt++) {
    while (udp.parsePacket()) udp.flush();             // old replies
    udp.beginPacket(ip, port);
    udp.write(tx, len);
    udp.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 800) {
      int n = udp.parsePacket();
      if (n > 0) {
        n = udp.read(rx, sizeof rx);
        // a quick check of the request id happens in parse(); hand it over
        if (n > 0) return n;
      }
      delay(4);
    }
  }
  return 0;
}

// GET some scalars. fn(i, oid, val).
template <typename F>
bool get(IPAddress ip, const char* comm, const Oid* oids, int n, F fn) {
  uint32_t id = nextId++;
  int len = build(tx, comm, 0xA0, id, 0, oids, n);
  int got = exchange(ip, len, id);
  if (!got) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  int es = parse(rx, got, id, fn);
  xSemaphoreGive(lock);
  return es >= 0;
}

// Walks table columns together with GETBULK. fn(col, idx, idxLen, val) for each cell under
// its column. Stops when every column has left its subtree (or after maxRounds).
template <typename F>
bool walk(IPAddress ip, const char* comm, const Oid* cols, int ncols, int rep, F fn, int maxRounds = 40) {
  Oid last[8]; bool done[8];
  for (int c = 0; c < ncols; c++) { last[c] = cols[c]; done[c] = false; }
  for (int round = 0; round < maxRounds; round++) {
    int act[8], na = 0;
    Oid req[8];
    for (int c = 0; c < ncols; c++) if (!done[c]) { act[na] = c; req[na] = last[c]; na++; }
    if (!na) return true;
    uint32_t id = nextId++;
    int len = build(tx, comm, 0xA5, id, rep, req, na);
    int got = exchange(ip, len, id);
    if (!got) return false;
    bool progressed[8] = {false};
    xSemaphoreTake(lock, portMAX_DELAY);
    int es = parse(rx, got, id, [&](int i, const Oid& o, const Val& v) {
      int c = act[i % na];
      if (done[c]) return;
      if (v.type >= 0x80 || !under(o, cols[c]) || cmp(o, last[c]) <= 0) { done[c] = true; return; }
      last[c] = o;
      progressed[c] = true;
      fn(c, o.a + cols[c].n, o.n - cols[c].n, v);
    });
    xSemaphoreGive(lock);
    if (es == 1 && rep > 2) { rep /= 2; continue; }     // tooBig: ask for fewer rows
    if (es != 0) return false;
    for (int k = 0; k < na; k++) if (!progressed[act[k]]) done[act[k]] = true;
  }
  return true;
}

}  // namespace snmp

// ---------- Polling (its own task, so a slow switch never stalls the screen) ----------
using snmp::Oid;
Oid O_IFNAME, O_IFTYPE, O_ADMIN, O_HSPEED, O_ALIAS, O_DUPLEX, O_VMVLAN, O_TRUNK, O_CDPDEV, O_CDPPORT,
    O_PETHST, O_PETHCLS, O_OPER, O_HCIN, O_HCOUT, O_INERR, O_SYSNAME, O_SYSUP, O_ENTMODEL, O_POEPWR, O_POEUSED;

void initOids() {
  using snmp::parse;
  O_IFNAME  = parse("1.3.6.1.2.1.31.1.1.1.1");
  O_IFTYPE  = parse("1.3.6.1.2.1.2.2.1.3");
  O_ADMIN   = parse("1.3.6.1.2.1.2.2.1.7");
  O_HSPEED  = parse("1.3.6.1.2.1.31.1.1.1.15");
  O_ALIAS   = parse("1.3.6.1.2.1.31.1.1.1.18");
  O_DUPLEX  = parse("1.3.6.1.2.1.10.7.2.1.19");
  O_VMVLAN  = parse("1.3.6.1.4.1.9.9.68.1.2.2.1.2");
  O_TRUNK   = parse("1.3.6.1.4.1.9.9.46.1.6.1.1.14");
  O_CDPDEV  = parse("1.3.6.1.4.1.9.9.23.1.2.1.1.6");
  O_CDPPORT = parse("1.3.6.1.4.1.9.9.23.1.2.1.1.7");
  O_PETHST  = parse("1.3.6.1.2.1.105.1.1.1.6");
  O_PETHCLS = parse("1.3.6.1.2.1.105.1.1.1.10");
  O_OPER    = parse("1.3.6.1.2.1.2.2.1.8");
  O_HCIN    = parse("1.3.6.1.2.1.31.1.1.1.6");
  O_HCOUT   = parse("1.3.6.1.2.1.31.1.1.1.10");
  O_INERR   = parse("1.3.6.1.2.1.2.2.1.14");
  O_SYSNAME = parse("1.3.6.1.2.1.1.5.0");
  O_SYSUP   = parse("1.3.6.1.2.1.1.3.0");
  O_ENTMODEL= parse("1.3.6.1.2.1.47.1.1.1.1.13");
  O_POEPWR  = parse("1.3.6.1.2.1.105.1.3.1.1.2.1");
  O_POEUSED = parse("1.3.6.1.2.1.105.1.3.1.1.4.1");
}

Port* byIndex(uint32_t ifIndex) {
  for (int i = 0; i < numPorts; i++) if (ports[i].ifIndex == ifIndex) return &ports[i];
  return nullptr;
}

// "GigabitEthernet0/1" -> "Gi0/1"
void shortName(const char* in, char* out, int cap) {
  const char* d = in;
  while (*d && !isdigit((unsigned char)*d)) d++;
  if (d - in > 2) snprintf(out, cap, "%c%c%s", in[0], in[1], d);
  else snprintf(out, cap, "%s", in);
}

bool physical(const char* name, int type) {
  if (type != 6 && type != 117) return false;
  return strncmp(name, "Vl", 2) && strncmp(name, "Po", 2) && strncmp(name, "Nu", 2) && strncmp(name, "St", 2);
}

// The part of a port name before the last number ("Gi0/" of "Gi0/14") and that number.
int portNum(const char* name, char* group, int cap) {
  const char* s = name + strlen(name);
  while (s > name && isdigit((unsigned char)s[-1])) s--;
  int g = min((int)(s - name), cap - 1);
  memcpy(group, name, g); group[g] = 0;
  return atoi(s);
}

// Puts the ports on the panel: the main group in pairs (odd on top), uplinks to the right.
void layoutPanel() {
  char g0[12] = "", g[12];
  int cnt0 = 0, cntOther = 0;
  portNum(ports[0].name, g0, sizeof g0);
  for (int i = 0; i < numPorts; i++) { portNum(ports[i].name, g, sizeof g); if (!strcmp(g, g0)) cnt0++; else cntOther++; }
  // Uplinks: the smaller name group (Fa0/1-48 + Gi0/1-4), or the last 4 of 28/52 (3560G).
  for (int i = 0; i < numPorts; i++) {
    portNum(ports[i].name, g, sizeof g);
    bool first = !strcmp(g, g0);
    ports[i].uplink = cntOther ? (cntOther < cnt0 ? !first : first) : false;
  }
  if (!cntOther && (numPorts == 28 || numPorts == 52 || numPorts == 12 || numPorts == 10))
    for (int i = numPorts - (numPorts == 10 ? 2 : 4); i < numPorts; i++) ports[i].uplink = true;
  numAccess = numUplink = 0;
  for (int i = 0; i < numPorts; i++) {
    Port& p = ports[i];
    int k = p.uplink ? numUplink++ : numAccess++;
    p.col = k / 2; p.row = k % 2;
  }
}

void clearSwitch() {
  numPorts = numAccess = numUplink = 0;
  histN = 0;
  sysName[0] = model[0] = 0;
  upSecs = 0; poeUsed = poeTotal = -1;
  sel = -1;
}

uint8_t logv(float bps) { return bps < 1 ? 0 : (uint8_t)constrain(log10f(bps) * 25.0f, 0.0f, 255.0f); }

bool pollSlow(IPAddress ip, const char* comm) {
  // Port list: names and types (rebuilt each time, keeping counters for ports that stay).
  static uint32_t idxs[MAX_PORTS * 2]; static char names[MAX_PORTS * 2][12]; static uint8_t types[MAX_PORTS * 2];
  int n = 0;
  Oid c1[] = {O_IFNAME, O_IFTYPE};
  bool ok = snmp::walk(ip, comm, c1, 2, 12, [&](int c, const uint32_t* idx, int il, const snmp::Val& v) {
    if (il != 1) return;
    int k = 0;
    while (k < n && idxs[k] != idx[0]) k++;
    if (k == n) { if (n >= MAX_PORTS * 2) return; idxs[n] = idx[0]; names[n][0] = 0; types[n] = 0; n++; }
    if (c == 0) { char full[40]; v.copy(full, sizeof full); shortName(full, names[k], 12); }
    else types[k] = v.num;
  }, 30);
  if (!ok) return false;
  // the physical ports, in ifIndex order (the panel order on Catalysts)
  int keep[MAX_PORTS], m = 0;
  for (int k = 0; k < n && m < MAX_PORTS; k++) if (physical(names[k], types[k])) keep[m++] = k;
  std::sort(keep, keep + m, [&](int a, int b) { return idxs[a] < idxs[b]; });
  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = m != numPorts;
  for (int i = 0; i < m && !changed; i++) changed = ports[i].ifIndex != idxs[keep[i]];
  if (changed) {                                    // a new switch, or its ports changed: start over
    memset(ports, 0, sizeof ports);
    histN = 0; sel = -1;
  }
  for (int i = 0; i < m; i++) {
    Port& p = ports[i];
    p.ifIndex = idxs[keep[i]];
    strlcpy(p.name, names[keep[i]], sizeof p.name);
    p.cdp[0] = p.cdpPort[0] = 0;
    p.poe = p.poeClass = 0;
    p.trunk = false; p.vlan = 0;
  }
  numPorts = m;
  if (numPorts) layoutPanel();
  xSemaphoreGive(lock);
  if (!numPorts) { strlcpy(lastErr, "No Ethernet ports in its ifTable", sizeof lastErr); return false; }

  Oid c2[] = {O_ADMIN, O_HSPEED};
  snmp::walk(ip, comm, c2, 2, 12, [&](int c, const uint32_t* idx, int il, const snmp::Val& v) {
    Port* p = byIndex(idx[0]);
    if (!p) return;
    if (c == 0) p->admin = v.num; else p->speed = v.num;
  });
  Oid c3[] = {O_ALIAS};
  snmp::walk(ip, comm, c3, 1, 14, [&](int, const uint32_t* idx, int, const snmp::Val& v) {
    Port* p = byIndex(idx[0]); if (p) v.copy(p->alias, sizeof p->alias);
  });
  Oid c4[] = {O_DUPLEX, O_VMVLAN, O_TRUNK};
  snmp::walk(ip, comm, c4, 3, 12, [&](int c, const uint32_t* idx, int, const snmp::Val& v) {
    Port* p = byIndex(idx[0]);
    if (!p) return;
    if (c == 0) p->duplex = v.num;
    else if (c == 1) p->vlan = v.num;
    else p->trunk = v.num == 1;
  });
  Oid c5[] = {O_CDPDEV, O_CDPPORT};
  snmp::walk(ip, comm, c5, 2, 8, [&](int c, const uint32_t* idx, int il, const snmp::Val& v) {
    Port* p = byIndex(idx[0]);
    if (!p) return;
    char s[64]; v.copy(s, sizeof s);
    if (c == 0) {
      char* paren = strchr(s, '(');                    // "FOC123(SW2)" -> "SW2"
      if (paren && strchr(paren, ')')) { *strchr(paren, ')') = 0; memmove(s, paren + 1, strlen(paren)); }
      char* dot = strchr(s, '.'); if (dot) *dot = 0;   // drop the domain
      strlcpy(p->cdp, s, sizeof p->cdp);
    } else shortName(s, p->cdpPort, sizeof p->cdpPort);
  });
  // PoE: group.port, the nth port of the main group.
  Oid c6[] = {O_PETHST, O_PETHCLS};
  snmp::walk(ip, comm, c6, 2, 16, [&](int c, const uint32_t* idx, int il, const snmp::Val& v) {
    if (il != 2 || idx[0] != 1) return;
    int k = idx[1] - 1, seen = 0;
    for (int i = 0; i < numPorts; i++) {
      if (ports[i].uplink) continue;
      if (seen++ == k) { if (c == 0) ports[i].poe = v.num; else ports[i].poeClass = v.num; break; }
    }
  });
  Oid s1[] = {O_SYSNAME, O_POEPWR, O_POEUSED};
  snmp::get(ip, comm, s1, 3, [&](int i, const Oid&, const snmp::Val& v) {
    if (i == 0 && v.type == 0x04) v.copy(sysName, sizeof sysName);
    if (i == 1) poeTotal = v.type < 0x80 ? (int)v.num : -1;
    if (i == 2) poeUsed = v.type < 0x80 ? (int)v.num : -1;
  });
  if (!model[0]) {
    Oid c7[] = {O_ENTMODEL};
    snmp::walk(ip, comm, c7, 1, 4, [&](int, const uint32_t*, int, const snmp::Val& v) {
      if (!model[0] && v.len > 2) v.copy(model, sizeof model);
    }, 2);
  }
  return true;
}

bool pollFast(IPAddress ip, const char* comm) {
  static uint32_t lastAt = 0;
  uint32_t now = millis();
  float dt = lastAt ? (now - lastAt) / 1000.0f : 0;
  lastAt = now;
  Oid c[] = {O_OPER, O_HCIN, O_HCOUT, O_INERR};
  bool ok = snmp::walk(ip, comm, c, 4, 10, [&](int col, const uint32_t* idx, int, const snmp::Val& v) {
    Port* p = byIndex(idx[0]);
    if (!p) return;
    switch (col) {
      case 0: p->oper = v.num; break;
      case 1: p->inBps  = (dt > 0.5f && p->inOct  && v.num >= p->inOct)  ? (v.num - p->inOct)  * 8 / dt : 0; p->inOct  = v.num; break;
      case 2: p->outBps = (dt > 0.5f && p->outOct && v.num >= p->outOct) ? (v.num - p->outOct) * 8 / dt : 0; p->outOct = v.num; break;
      case 3: if (p->inErr && v.num > p->inErr) p->errAt = millis(); p->inErr = v.num; break;
    }
  });
  if (!ok) return false;
  Oid s[] = {O_SYSUP};
  snmp::get(ip, comm, s, 1, [&](int, const Oid&, const snmp::Val& v) { upSecs = v.num / 100; });
  xSemaphoreTake(lock, portMAX_DELAY);
  // one history column for every port
  for (int i = 0; i < numPorts; i++) {
    Port& p = ports[i];
    memmove(p.hIn, p.hIn + 1, HIST - 1); memmove(p.hOut, p.hOut + 1, HIST - 1);
    p.hIn[HIST - 1] = logv(p.inBps); p.hOut[HIST - 1] = logv(p.outBps);
  }
  if (dt > 0.5f && histN < HIST) histN++;
  if (sel < 0) for (int i = 0; i < numPorts; i++) if (ports[i].oper == 1) { sel = i; break; }
  xSemaphoreGive(lock);
  return true;
}

void poller(void*) {
  uint32_t lastSlow = 0, lastFast = 0;
  bool haveList = false;
  for (;;) {
    if (millis() - lastRun > 3000 || !numSw || !wifiUp()) { delay(250); continue; }
    if (swChanged) {
      xSemaphoreTake(lock, portMAX_DELAY); clearSwitch(); swChanged = false; xSemaphoreGive(lock);
      haveList = false; lastSlow = lastFast = 0;
    }
    IPAddress ip;
    String host = swIp[curSw];
    int colon = host.indexOf(':');
    snmp::port = colon > 0 ? host.substring(colon + 1).toInt() : 161;
    if (colon > 0) host = host.substring(0, colon);
    if (!ip.fromString(host)) { state = P_NOREPLY; strlcpy(lastErr, "Bad IP address", sizeof lastErr); delay(1000); continue; }
    String comm = cfg.community.length() ? cfg.community : String("public");
    uint32_t now = millis();
    bool ok = true;
    if (!haveList || now - lastSlow > SLOW_MS) {
      if (!haveList) state = P_WAIT;
      pollAt = millis();
      ok = pollSlow(ip, comm.c_str());
      if (ok) { haveList = true; lastSlow = now; }
    }
    if (ok && haveList && now - lastFast >= FAST_MS) {
      lastFast = now;
      pollAt = millis();
      ok = pollFast(ip, comm.c_str());
    }
    if (ok) { state = P_OK; lastOk = millis(); lastErr[0] = 0; }
    else {
      if (!lastErr[0] || haveList) snprintf(lastErr, sizeof lastErr, "No SNMP reply from %s", swIp[curSw].c_str());
      state = P_NOREPLY;
      delay(2000);
    }
    delay(50);
  }
}

void loadSwitches() {
  numSw = 0;
  String s = cfg.switches;
  while (s.length() && numSw < MAX_SW) {
    int comma = s.indexOf(',');
    String item = comma < 0 ? s : s.substring(0, comma);
    s = comma < 0 ? String("") : s.substring(comma + 1);
    item.trim();
    if (!item.length()) continue;
    int eq = item.indexOf('=');
    swName[numSw] = eq < 0 ? String("") : item.substring(0, eq);
    swIp[numSw]   = eq < 0 ? item : item.substring(eq + 1);
    swName[numSw].trim(); swIp[numSw].trim();
    numSw++;
  }
  if (curSw >= numSw) curSw = 0;
}

// ---------- Drawing ----------
int W, H;
bool big;                              // the 480x320 layout

// Panel geometry, worked out per frame from the screen size and port count.
int chX, chY, chW, chH;                // chassis
int cellW, cellH, pitch, rowGap, numRowH;
int cellX[MAX_PORTS], cellY[MAX_PORTS];
int hdrH, sumY, detY;

// Small uppercase labels with letter spacing, like Perf-Deck's .k
void klabel(const char* s, int x, int y, uint16_t c, int datum = TL_DATUM) {
  spr.setTextFont(1);
  spr.setTextColor(c);
  int adv = 7, w = strlen(s) * adv - 1;
  if (datum == TR_DATUM) x -= w;
  else if (datum == TC_DATUM) x -= w / 2;
  spr.setTextDatum(TL_DATUM);
  char ch[2] = {0, 0};
  for (const char* p = s; *p; p++) { ch[0] = toupper(*p); spr.drawString(ch, x, y); x += adv; }
}

// A glowing LED rectangle: dim halo, then the lit body.
void led(int x, int y, int w, int h, uint16_t c, bool on) {
  if (!on) { spr.fillRect(x, y, w, h, dk(P_LEDOFF)); return; }
  spr.drawRect(x - 1, y - 1, w + 2, h + 2, mix(c, P_BG, 35));
  spr.fillRect(x, y, w, h, c);
  if (w > 3 && h > 2) spr.drawFastHLine(x + 1, y, w - 2, mix(P_TEXT, c, 35));   // a highlight
}

// Seven-segment digits with ghost segments, Perf-Deck's .seg7. Returns the width drawn.
int seg7(const char* s, int x, int y, int h, uint16_t c) {
  static const uint8_t SEG[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  int w = h * 11 / 20, t = max(2, h / 9), x0 = x;
  uint16_t ghost = dk(mix(c, P_WELL, 12));
  for (const char* p = s; *p; p++) {
    if (*p == '.') { spr.fillRect(x - t / 2, y + h - t, t, t, c); x += t / 2 + 1; continue; }
    uint8_t m = *p >= '0' && *p <= '9' ? SEG[*p - '0'] : *p == '-' ? 0x40 : 0;
    int hh = h / 2;
    struct { int x, y, w, h; } r[7] = {
      {x + t, y, w - 2 * t, t}, {x + w - t, y + t, t, hh - t}, {x + w - t, y + hh, t, hh - t},
      {x + t, y + h - t, w - 2 * t, t}, {x, y + hh, t, hh - t}, {x, y + t, t, hh - t},
      {x + t, y + hh - t / 2, w - 2 * t, t}};
    for (int k = 0; k < 7; k++) spr.fillRect(r[k].x, r[k].y, r[k].w, r[k].h, (m >> k) & 1 ? c : ghost);
    x += w + t + 1;
  }
  return x - x0;
}

// Cisco's way: green is a healthy link at any normal speed; 10M or half duplex stand out.
uint16_t speedColor(const Port& p) {
  if (p.duplex == 2) return P_ORANGE;
  if (p.speed && p.speed < 100) return P_WARN;
  return P_GREEN;
}

String fmtBps(float b) {
  char s[16];
  if (b >= 1e9)      snprintf(s, sizeof s, "%.2f G", b / 1e9);
  else if (b >= 1e6) snprintf(s, sizeof s, b >= 1e8 ? "%.0f M" : "%.1f M", b / 1e6);
  else if (b >= 1e3) snprintf(s, sizeof s, "%.0f k", b / 1e3);
  else               snprintf(s, sizeof s, "%.0f ", b);
  return s;
}

void layout() {
  W = canvasWidth(); H = canvasHeight();
  big = W >= 480;
  hdrH = big ? 26 : 20;
  int cols = (numAccess + 1) / 2, ucols = (numUplink + 1) / 2;
  int blocks = max(0, (cols - 1) / 6);
  int bg = big ? 6 : 4, ug = numUplink ? (big ? 16 : 8) : 0, m = big ? 12 : 6;
  pitch = cols + ucols ? (W - 2 * m - blocks * bg - ug) / (cols + ucols) : 16;
  pitch = constrain(pitch, 6, big ? 26 : 20);
  cellW = pitch - 2;
  cellH = big ? 20 : (H >= 240 ? 14 : 11);
  rowGap = big ? 5 : 3;
  numRowH = (big || H >= 240) && pitch >= 12 ? 10 : 0;
  int used = (cols + ucols) * pitch + blocks * bg + ug;
  chW = used + 2 * m; chX = (W - chW) / 2;
  chY = hdrH + 3;
  chH = 2 * numRowH + 2 * cellH + rowGap + (big ? 14 : 8);
  int y0 = chY + (big ? 7 : 4) + numRowH;
  for (int i = 0; i < numPorts; i++) {
    Port& p = ports[i];
    int x = chX + m;
    if (p.uplink) x += cols * pitch + blocks * bg + ug + p.col * pitch;
    else          x += p.col * pitch + (p.col / 6) * bg;
    cellX[i] = x; cellY[i] = y0 + p.row * (cellH + rowGap);
  }
  sumY = chY + chH + (big ? 6 : 3);
  detY = sumY + (big ? 18 : 13);
}

void drawHeader() {
  spr.fillRect(0, 0, W, hdrH, dk(P_PANEL));
  spr.drawFastHLine(0, hdrH - 1, W, dk(P_LINE));
  // poll lamp: green fresh, amber waiting/stale, red no reply; it flashes while asking
  bool asking = millis() - pollAt < 150;
  uint16_t lc = state == P_OK ? (millis() - lastOk < 10000 ? P_GREEN : P_WARN) : state == P_NOREPLY ? P_RED : P_WARN;
  led(big ? 10 : 6, hdrH / 2 - 3, big ? 14 : 10, 6, asking ? P_TEXT : lc, true);
  int x = big ? 32 : 22;
  klabel("Port map", x, hdrH / 2 - 4, P_AMBER);
  x += 8 * 7 + (big ? 10 : 6);
  String nm = numSw ? (swName[curSw].length() ? swName[curSw] : String(sysName[0] ? sysName : swIp[curSw].c_str())) : String("no switch set");
  spr.setTextFont(2);
  spr.setTextDatum(ML_DATUM);
  spr.setTextColor(P_TEXT);
  spr.drawString(fit(nm, big ? 170 : 110, 2), x, hdrH / 2);
  if (numSw > 1) {
    char b[8]; snprintf(b, sizeof b, "%d/%d", curSw + 1, numSw);
    klabel(b, x + min((int)spr.textWidth(nm, 2), big ? 170 : 110) + 8, hdrH / 2 - 4, P_MUTED);
  }
  // right: model and uptime
  char r[48] = "";
  if (upSecs) {
    if (big && model[0]) snprintf(r, sizeof r, "%s  up %s", model, fmtDur(upSecs).c_str());
    else snprintf(r, sizeof r, "up %s", fmtDur(upSecs).c_str());
  }
  klabel(r, hdrRight(), hdrH / 2 - 4, P_MUTED, TR_DATUM);
}

void drawChassis() {
  // a brushed-metal-ish panel: three bands, a bevel, mounting holes
  int r = big ? 6 : 4;
  spr.fillRoundRect(chX, chY, chW, chH, r, dk(P_RAISED));
  spr.fillRect(chX + 2, chY + chH / 2, chW - 4, chH / 2 - r, dk(P_PANEL));
  spr.drawRoundRect(chX, chY, chW, chH, r, dk(P_LINE));
  spr.drawFastHLine(chX + r, chY + 1, chW - 2 * r, dk(0x4208));
  if (big) for (int sx : {chX + 5, chX + chW - 8}) for (int sy : {chY + 6, chY + chH - 9}) spr.fillCircle(sx + 1, sy + 1, 2, P_BG);

  uint32_t now = millis();
  for (int i = 0; i < numPorts; i++) {
    Port& p = ports[i];
    int x = cellX[i], y = cellY[i];
    bool up = p.oper == 1, shut = p.admin == 2;
    uint16_t sc = speedColor(p);
    // the jack: a dark well, tinted when there's a link
    uint16_t well = up ? (p.trunk ? mix(P_BLUE, P_WELL, 35) : mix(sc, P_WELL, 16)) : dk(P_WELL);
    if (p.uplink) {                                   // an SFP cage
      spr.fillRect(x, y, cellW, cellH, well);
      spr.drawRect(x, y, cellW, cellH, up ? mix(sc, P_LINE, 60) : dk(P_LINE));
      spr.drawRect(x + 2, y + 2, cellW - 4, cellH - 4, dk(mix(P_LINE, P_WELL, 50)));
    } else {
      spr.fillRect(x, y, cellW, cellH, well);
      spr.drawRect(x, y, cellW, cellH, up ? mix(p.trunk ? P_BLUE : sc, P_LINE, 55) : dk(P_LINE));
      int tw = max(2, cellW / 2), tx = x + (cellW - tw) / 2;          // the latch notch
      if (p.row == 0) spr.fillRect(tx, y + cellH - 3, tw, 3, dk(P_RAISED));
      else            spr.fillRect(tx, y, tw, 3, dk(P_PANEL));
    }
    if (shut) { spr.drawLine(x + 2, y + 2, x + cellW - 3, y + cellH - 3, mix(P_RED, P_WELL, 60)); }
    if (p.poe == 3) spr.fillRect(x + cellW / 2 - 1, y + cellH / 2 - 1, 3, 3, P_AMBER);
    if (p.poe == 4) spr.fillRect(x + cellW / 2 - 1, y + cellH / 2 - 1, 3, 3, P_RED);
    // the link LED, outside the jack (above the top row, below the bottom row)
    int lh = big ? 3 : 2, ly = p.row == 0 ? y - lh - 2 : y + cellH + 2;
    bool errs = p.errAt && now - p.errAt < 15000;
    float lvl = max(p.inBps, p.outBps);
    bool flick = lvl > 8000 && random(100) < 6 + min(24, (int)(log10f(lvl) * 3));   // short blinks
    if (errs)    led(x + 1, ly, cellW - 2, lh, P_RED, (now / 250) % 2);
    else if (up) led(x + 1, ly, cellW - 2, lh, sc, !flick);
    else         led(x + 1, ly, cellW - 2, lh, 0, false);
    if (i == sel) {                                   // the selected port: amber brackets
      spr.drawRect(x - 2, y - 2, cellW + 4, cellH + 4, P_AMBER);
      spr.drawRect(x - 3, y - 3, cellW + 6, cellH + 6, mix(P_AMBER, P_BG, 30));
    }
    // port numbers above the top row and below the bottom one
    if (numRowH) {
      char g[12]; int n = portNum(p.name, g, sizeof g);
      if (pitch >= 14 || p.col % 3 == 0 || p.uplink) {
        spr.setTextFont(1);
        spr.setTextDatum(p.row == 0 ? BC_DATUM : TC_DATUM);
        spr.setTextColor(i == sel ? P_AMBER : P_MUTED);
        spr.drawString(String(n), x + cellW / 2, p.row == 0 ? y - lh - 4 : y + cellH + lh + 5);
      }
    }
  }
}

void lamp(int& x, int y, uint16_t c, bool on, const char* label) {
  led(x, y + 1, big ? 12 : 8, big ? 5 : 4, c, on);
  x += big ? 18 : 12;
  klabel(label, x, y, on ? P_TEXT : P_MUTED);
  x += strlen(label) * 7 + (big ? 14 : 8);
}

void drawSummary() {
  int up = 0, down = 0, poe = 0, err = 0, trunk = 0, shut = 0;
  float in = 0, out = 0;
  uint32_t now = millis();
  for (int i = 0; i < numPorts; i++) {
    Port& p = ports[i];
    if (p.admin == 2) shut++; else if (p.oper == 1) up++; else down++;
    if (p.poe == 3) poe++;
    if (p.trunk) trunk++;
    if (p.errAt && now - p.errAt < 15000) err++;
    if (!p.trunk && !p.uplink) { in += p.inBps; out += p.outBps; }
  }
  int x = big ? 10 : 6, y = sumY;
  char b[24];
  snprintf(b, sizeof b, "%d up", up);     lamp(x, y, P_GREEN, up > 0, b);
  snprintf(b, sizeof b, "%d idle", down); lamp(x, y, P_MUTED, false, b);
  if (shut) { snprintf(b, sizeof b, "%d shut", shut); lamp(x, y, P_RED, false, b); }
  if (poe || poeUsed >= 0) {
    if (big && poeUsed >= 0 && poeTotal > 0) snprintf(b, sizeof b, "PoE %d  %d/%dW", poe, poeUsed, poeTotal);
    else snprintf(b, sizeof b, "PoE %d", poe);
    lamp(x, y, P_AMBER, poe > 0, b);
  }
  if (big && trunk) { snprintf(b, sizeof b, "%d trunk", trunk); lamp(x, y, P_BLUE, true, b); }
  if (err) { snprintf(b, sizeof b, "%d err", err); lamp(x, y, P_RED, (now / 250) % 2, b); }
  // edge-port traffic in and out of the room's seats, if it fits after the lamps
  String s = "Seats " + fmtBps(in) + "/" + fmtBps(out);
  int sw = s.length() * 7;
  if (x + sw < W - (big ? 10 : 6)) klabel(s.c_str(), W - (big ? 10 : 6), y, P_MUTED, TR_DATUM);
}

// A horizontal LED bar meter on a log scale from 1 kb/s to the port's speed.
void meter(int x, int y, int w, int h, float bps, float maxBps, uint16_t c) {
  int n = big ? 28 : 18, gap = big ? 2 : 1;
  int sw = (w - (n - 1) * gap) / n;
  float lo = 3, hi = max(4.0f, log10f(max(maxBps, 1e4f)));
  float f = bps < 1000 ? 0 : constrain((log10f(bps) - lo) / (hi - lo), 0.0f, 1.0f);
  int lit = round(f * n);
  for (int i = 0; i < n; i++) {
    uint16_t z = i < n * 0.6 ? c : i < n * 0.85 ? P_WARN : P_RED;
    led(x + i * (sw + gap), y, sw, h, z, i < lit);
  }
}

void drawGraph(int x, int y, int w, int h, const Port& p) {
  spr.fillRect(x, y, w, h, dk(P_WELL));
  spr.drawRect(x, y, w, h, dk(P_LINE));
  float lo = 3, hi = max(6.0f, log10f(max(p.speed, (uint32_t)1) * 1e6f));
  auto py = [&](uint8_t v) { float l = v / 25.0f; return y + h - 2 - (int)(constrain((l - lo) / (hi - lo), 0.0f, 1.0f) * (h - 4)); };
  // decade grid with labels
  spr.setTextFont(1);
  spr.setTextDatum(TR_DATUM);
  const char* lbl[] = {"1k", "10k", "100k", "1M", "10M", "100M", "1G", "10G"};
  for (int d = 4; d <= (int)hi; d++) {
    int gy = y + h - 2 - (int)((d - lo) / (hi - lo) * (h - 4));
    for (int gx = x + 2; gx < x + w - 2; gx += 4) spr.drawPixel(gx, gy, dk(P_GRID));
    if (big || d % 3 == 0) { spr.setTextColor(dk(0x5AA9)); spr.drawString(lbl[d - 3], x + w - 3, gy + 2); }
  }
  int n = min(histN, HIST);
  if (n < 2) { klabel("collecting", x + w / 2, y + h / 2 - 4, P_MUTED, TC_DATUM); return; }
  float step = (float)(w - 4) / (HIST - 1);
  for (int i = HIST - n; i < HIST; i++) {          // IN: a filled area and its line
    int gx = x + 2 + (int)(i * step), gx2 = x + 2 + (int)((i + 1) * step);
    int yy = py(p.hIn[i]);
    spr.fillRect(gx, yy, max(1, gx2 - gx), y + h - 2 - yy, mix(P_GREEN, P_WELL, 22));
  }
  for (int i = HIST - n + 1; i < HIST; i++) {
    int ax = x + 2 + (int)((i - 1) * step), bx = x + 2 + (int)(i * step);
    spr.drawLine(ax, py(p.hIn[i - 1]), bx, py(p.hIn[i]), P_GREEN);
    spr.drawLine(ax, py(p.hOut[i - 1]), bx, py(p.hOut[i]), P_BLUE);
  }
}

void drawDetail() {
  int x = big ? 8 : 4, y = detY, w = W - 2 * x, h = H - y - (big ? 6 : 2);
  if (h < 30) return;
  spr.fillRoundRect(x, y, w, h, big ? 8 : 4, dk(P_WELL));
  spr.drawRoundRect(x, y, w, h, big ? 8 : 4, dk(0x2104));
  if (sel < 0 || sel >= numPorts) { klabel("tap a port", x + w / 2, y + h / 2 - 4, P_MUTED, TC_DATUM); return; }
  Port& p = ports[sel];
  bool up = p.oper == 1;
  uint16_t sc = up ? speedColor(p) : P_MUTED;
  int pad = big ? 12 : 6, lx = x + pad, ty = y + (big ? 10 : 5);
  int leftW = big ? 200 : 128;

  // left column: port, state, description, VLAN, PoE, CDP, errors
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(big ? 4 : 2);
  spr.setTextColor(P_TEXT);
  spr.drawString(p.name, lx, ty);
  int nx = lx + spr.textWidth(p.name) + 8;
  const char* st = p.admin == 2 ? "SHUT" : up ? "UP" : "NO LINK";
  led(nx, ty + (big ? 9 : 5), big ? 10 : 7, big ? 5 : 4, p.admin == 2 ? P_RED : sc, up || p.admin == 2);
  klabel(st, nx + (big ? 16 : 11), ty + (big ? 7 : 3), up ? sc : p.admin == 2 ? P_RED : P_MUTED);
  ty += big ? 30 : 17;
  spr.setTextFont(2);
  spr.setTextColor(p.alias[0] ? P_TEXT : P_MUTED);
  spr.drawString(fit(p.alias[0] ? p.alias : "(no description)", leftW, 2), lx, ty);
  ty += big ? 20 : 15;
  char b[48];
  auto row = [&](const char* k, const String& v, uint16_t c) {
    klabel(k, lx, ty + 4, P_MUTED);
    spr.setTextFont(2); spr.setTextDatum(TL_DATUM); spr.setTextColor(c);
    spr.drawString(fit(v, leftW - 44, 2), lx + 44, ty);
    ty += big ? 18 : 14;
  };
  if (up) {
    snprintf(b, sizeof b, "%s %s", p.speed >= 1000 ? (String(p.speed / 1000) + "G").c_str() : (String(p.speed) + "M").c_str(),
             p.duplex == 3 ? "full" : p.duplex == 2 ? "HALF" : "");
    row("link", b, p.duplex == 2 ? P_RED : sc);
  }
  row(p.trunk ? "mode" : "vlan", p.trunk ? String("trunk") : p.vlan ? String(p.vlan) : String("-"), p.trunk ? P_BLUE : P_TEXT);
  if (p.poe == 3) { snprintf(b, sizeof b, "on, class %d", max(0, p.poeClass - 1)); row("poe", b, P_AMBER); }
  else if (p.poe == 4) row("poe", "FAULT", P_RED);
  if (p.cdp[0] && (big || ty < y + h - 14)) row("cdp", String(p.cdp) + (p.cdpPort[0] ? " " + String(p.cdpPort) : ""), P_TEXT);
  if (big || ty < y + h - 14) {
    bool climbing = p.errAt && millis() - p.errAt < 15000;
    row("errs", String(p.inErr) + (climbing ? "  rising" : ""), climbing ? P_RED : p.inErr ? P_WARN : P_MUTED);
  }

  // right column: IN / OUT meters with seven-segment readouts, then the graph
  int rx = x + pad + leftW + (big ? 10 : 6), rw = x + w - pad - rx, ry = y + (big ? 10 : 5);
  float maxBps = max(p.speed, (uint32_t)1) * 1e6f;
  auto readout = [&](const char* k, float bps, uint16_t c) {
    klabel(k, rx, ry + 2, P_MUTED);
    char num[12]; const char* unit;
    if (bps >= 1e9)      { snprintf(num, sizeof num, "%.2f", bps / 1e9); unit = "Gb/s"; }
    else if (bps >= 1e6) { snprintf(num, sizeof num, bps >= 1e8 ? "%.0f" : "%.1f", bps / 1e6); unit = "Mb/s"; }
    else                 { snprintf(num, sizeof num, "%.0f", bps / 1e3); unit = "kb/s"; }
    int sh = big ? 18 : 11;
    int sx = rx + rw - (big ? 36 : 26);
    int sw = seg7(num, -1000, 0, sh, c);           // measure (drawn off-canvas)
    seg7(num, sx - sw, ry, sh, c);
    klabel(unit, rx + rw, ry + sh - 8, P_MUTED, TR_DATUM);
    ry += sh + (big ? 4 : 3);
    meter(rx, ry, rw, big ? 8 : 5, bps, maxBps, c);
    ry += big ? 14 : 8;
  };
  readout("in", p.inBps, P_GREEN);
  readout("out", p.outBps, P_BLUE);
  int gh = y + h - pad / 2 - ry - 2;
  if (gh > 24) drawGraph(rx, ry + 2, rw, gh, p);
}

void drawTalkers() {
  int x = big ? 8 : 4, y = detY, w = W - 2 * x, h = H - y - (big ? 6 : 2);
  if (h < 30) return;
  spr.fillRoundRect(x, y, w, h, big ? 8 : 4, dk(P_WELL));
  spr.drawRoundRect(x, y, w, h, big ? 8 : 4, dk(0x2104));
  klabel("Top talkers", x + 10, y + 6, P_AMBER);
  int order[MAX_PORTS], n = 0;
  for (int i = 0; i < numPorts; i++) if (ports[i].oper == 1) order[n++] = i;
  std::sort(order, order + n, [](int a, int b) { return ports[a].inBps + ports[a].outBps > ports[b].inBps + ports[b].outBps; });
  int rh = big ? 19 : 13, ty = y + (big ? 20 : 16);
  int rows = min(n, (y + h - ty - 2) / rh);
  for (int k = 0; k < rows; k++) {
    Port& p = ports[order[k]];
    int cx = x + 10;
    spr.setTextFont(2); spr.setTextDatum(TL_DATUM);
    spr.setTextColor(order[k] == sel ? P_AMBER : P_TEXT);
    spr.drawString(p.name, cx, ty);
    cx += big ? 56 : 44;
    if (big) { spr.setTextColor(P_MUTED); spr.drawString(fit(p.alias[0] ? p.alias : p.cdp, 110, 2), cx, ty); cx += 118; }
    int mw = (x + w - 10 - cx - (big ? 150 : 80)) / 2;
    meter(cx, ty + 3, mw, big ? 7 : 5, p.inBps, p.speed * 1e6f, P_GREEN);
    meter(cx + mw + 6, ty + 3, mw, big ? 7 : 5, p.outBps, p.speed * 1e6f, P_BLUE);
    spr.setTextDatum(TR_DATUM);
    int ex = x + w - 10;
    spr.setTextColor(P_GREEN);
    spr.drawString(fmtBps(p.inBps), ex - (big ? 72 : 40), ty);
    spr.setTextColor(P_BLUE);
    spr.drawString(fmtBps(p.outBps), ex, ty);
    ty += rh;
  }
  if (!rows) klabel("no ports with a link", x + w / 2, y + h / 2, P_MUTED, TC_DATUM);
}

void message(const char* a, const char* b) {
  int y = hdrH + (H - hdrH) / 2 - 20;
  spr.setTextDatum(MC_DATUM);
  spr.setTextFont(big ? 4 : 2);
  spr.setTextColor(P_TEXT);
  spr.drawString(a, W / 2, y);
  spr.setTextFont(2);
  spr.setTextColor(P_MUTED);
  spr.drawString(b, W / 2, y + (big ? 30 : 20));
}

void render() {
  spr.fillSprite(P_BG);
  drawHeader();
  if (!numSw) message("Add a switch in setup", "Hold the screen: \"Switches for Port Map\"");
  else if (!wifiUp()) message("Wi-Fi down", "");
  else if (!numPorts) {
    if (state == P_NOREPLY) message(lastErr, "Check the community and the switch's SNMP ACL");
    else message("Asking the switch...", swIp[curSw].c_str());
  } else {
    drawChassis();
    drawSummary();
    if (view == 0) drawDetail(); else drawTalkers();
  }
  show();
}

void draw() {
  xSemaphoreTake(lock, portMAX_DELAY);
  layout();
  frame(render);
  xSemaphoreGive(lock);
}

// ---------- App ----------
void start() {
  if (!lock) { lock = xSemaphoreCreateMutex(); initOids(); }
  setNative();
  String before = numSw ? swIp[curSw] : String("");
  loadSwitches();
  if (!numSw) state = P_NOCONFIG;
  if (!numSw || swIp[curSw] != before) swChanged = true;
  lastRun = millis();
  if (!task) xTaskCreatePinnedToCore(poller, "snmp", 8192, nullptr, 1, &task, 0);
  draw();
}

void run() {
  lastRun = millis();
  static uint32_t lastDraw = 0;
  if (redrawNow || millis() - lastDraw > 250) { lastDraw = millis(); draw(); }
}

void nextUpPort() {
  for (int k = 1; k <= numPorts; k++) {
    int i = (max(sel, 0) + k) % numPorts;
    if (ports[i].oper == 1) { sel = i; return; }
  }
}

void nextSwitch() {
  if (numSw < 2) return;
  curSw = (curSw + 1) % numSw;
  swChanged = true;
}

void press(int pin) {
  xSemaphoreTake(lock, portMAX_DELAY);
#ifdef BOARD_CYD
  if (pin == BTN_B && touch::pressTap) {
    int tx = touch::pressX, ty = touch::pressY;
    if (ty < hdrH) nextSwitch();
    else if (ty >= detY) view = (view + 1) % 2;
    else {
      int best = -1, bd = 1 << 30;                     // the nearest jack, within a little slack
      for (int i = 0; i < numPorts; i++) {
        int dx = tx - (cellX[i] + cellW / 2), dy = ty - (cellY[i] + cellH / 2);
        int d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
      }
      if (best >= 0 && bd < (pitch + 6) * (pitch + 6) * 2) { sel = best; view = 0; }
    }
  } else if (pin == BTN_A) nextUpPort();
#else
  if (pin == BTN_B) { nextUpPort(); view = 0; }
  else if (pin == BTN_A) { if (view == 0) view = 1; else { view = 0; nextSwitch(); } }
#endif
  xSemaphoreGive(lock);
  draw();
}

}  // namespace portmap
