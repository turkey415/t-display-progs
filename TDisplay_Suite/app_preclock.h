// Precision Clock (CYD only: it uses the whole 320x240 screen)
// A lab-style clock kept by its own NTP client, not the ESP32's once-an-hour SNTP:
//   - polls six servers (Cloudflare, Google, Apple, NIST, pool.ntp.org, the gateway) every 64 s,
//     after a quick start-up burst like ntpd's "iburst"
//   - per server keeps the last 8 samples and trusts the one with the shortest round trip
//     (the NTP clock filter); jitter = RMS spread of those samples
//   - Marzullo's algorithm throws out servers that disagree with the majority (falsetickers),
//     and the survivors are averaged, weighted by how tight each one's error bound is
//   - a line fitted through the last 30 minutes of offsets gives the crystal's drift (ppm), so
//     the time runs smoothly between samples instead of jumping
// Times are taken on the ESP32's microsecond timer (its 40 MHz crystal), and the NTP exchange
// runs in its own task so the reply is timestamped the moment it arrives.
//
// Tap the digits: red / green / dark blue (remembered).
// Pages (tap anywhere else): 1 the clock, 2 the time sources (like ntpq -p), 3 three clocks side by side:
// NTP time, the ESP32's own system clock, and the bare crystal left free-running since the
// first sync, with a graph of how far the crystal has wandered.
// BOOT: sync again now (a fresh burst to every server).
//
// Accuracy over Wi-Fi is a few milliseconds; the "±" figure is the worst-case error bound
// NTP itself computes (half the round trip plus the server's own error), so it is honest
// about how many of the shown decimals can be trusted.
#pragma once
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <math.h>

namespace preclock {

// ---------- NTP sources ----------
const int NSRV = 6, NSAMP = 8, NHIST = 120;
const uint32_t POLL_MS = 64000, BURST_MS = 2000, HIST_EVERY_MS = 30000;
const int64_t NTP_UNIX = 2208988800LL;          // seconds from 1900 to 1970

struct Sample { int64_t off, delay; uint32_t at; };

struct Server {
  const char* host;      // "GW" = this network's gateway
  const char* label;
  uint32_t ip = 0;       // network byte order
  uint8_t  stratum = 0, leap = 0, reach = 0;
  int8_t   precision = 0;
  char     refid[16] = "";
  int64_t  rootDelay = 0, rootDisp = 0;          // µs
  Sample   s[NSAMP];
  int      n = 0, head = 0;
  int      burst = 4;                            // start-up queries left
  uint32_t nextAt = 0, lastReply = 0, resolvedAt = 0;
  int      misses = 0;
  // from the clock filter
  bool     valid = false;
  int64_t  off = 0, delay = 0, jitter = 0, dist = 0;
  char     state = ' ';  // * selected, + truechimer, x falseticker, - unusable, ' ' no reply yet
};

Server srv[NSRV] = {
  {"time.cloudflare.com", "cloudflare"},
  {"time.google.com",     "google"},
  {"time.apple.com",      "apple"},
  {"time.nist.gov",       "nist"},
  {"pool.ntp.org",        "pool"},
  {"GW",                  "gateway"},
};

// ---------- The combined clock ----------
struct Model {
  bool     synced = false;
  int64_t  sysOff = 0, sysJitter = 0, uncert = 0;  // µs: UTC - crystal; error bound
  int64_t  sysOffL = 0;                            // crystal time of that measurement
  int      sel = -1, good = 0;
  uint32_t lastSync = 0, firstSync = 0;
  // fitted line: UTC = L + a + (L - xm) * ppm / 1e6
  int64_t  a = 0, xm = 0;
  double   ppm = 0, fitRms = 0;
  bool     haveDrift = false;
  int64_t  anchor = 0;                             // offset at the first sync: the free-running clock
  time_t   anchorAt = 0;
  // history (for the fit and the graph)
  int64_t  hx[NHIST]; int32_t hy[NHIST];           // crystal µs, offset - anchor µs
  int      hn = 0;
} m;

SemaphoreHandle_t lock = nullptr;                  // guards srv[] and m (poller task vs drawing)
void take() { xSemaphoreTake(lock, portMAX_DELAY); }
void give() { xSemaphoreGive(lock); }
TaskHandle_t task = nullptr;
volatile bool resync = false;

int64_t nowL() { return esp_timer_get_time(); }   // the crystal, µs since boot

int64_t utcAt(const Model& md, int64_t L) {
  return L + md.a + (int64_t)((double)(L - md.xm) * md.ppm * 1e-6);
}

// An NTP 64-bit timestamp (seconds since 1900 . 2^-32 fractions) as µs since 1970.
int64_t ntpToUs(const uint8_t* p) {
  uint32_t sec = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
  uint32_t frac = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
  int64_t s = sec;
  if (s < NTP_UNIX) s += 1LL << 32;               // era 1 (after 2036)
  return (s - NTP_UNIX) * 1000000LL + (int64_t)(((uint64_t)frac * 1000000ULL) >> 32);
}

int64_t short16(const uint8_t* p) {               // NTP 16.16 seconds -> µs
  uint32_t v = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
  return (int64_t)(((uint64_t)v * 1000000ULL) >> 16);
}

bool resolve(Server& sv) {
  if (!strcmp(sv.host, "GW")) { sv.ip = (uint32_t)WiFi.gatewayIP(); return sv.ip != 0; }
  addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(sv.host, "123", &hints, &res) != 0 || !res) return false;
  sv.ip = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(res);
  sv.resolvedAt = millis();
  return true;
}

// One NTP exchange. Fills t1..t4 and the packet; false on no (valid) reply.
bool exchange(Server& sv, uint8_t* r, int64_t& t1, int64_t& t4) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return false;
  timeval tv = {1, 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(123);
  to.sin_addr.s_addr = sv.ip;
  uint8_t q[48] = {0};
  q[0] = 0x23;                                    // leap 0, version 4, mode 3 (client)
  t1 = nowL();
  memcpy(q + 40, &t1, 8);                         // a cookie the server echoes back
  bool ok = false;
  if (sendto(sock, q, 48, 0, (sockaddr*)&to, sizeof to) == 48) {
    uint32_t until = millis() + 1000;
    while (millis() < until) {
      sockaddr_in from = {};
      socklen_t fl = sizeof from;
      int n = recvfrom(sock, r, 48, 0, (sockaddr*)&from, &fl);
      t4 = nowL();
      if (n < 0) break;                           // timed out
      if (n == 48 && from.sin_addr.s_addr == sv.ip && (r[0] & 7) == 4 && !memcmp(r + 24, q + 40, 8)) { ok = true; break; }
    }
  }
  close(sock);
  return ok;
}

// The NTP clock filter: the sample with the shortest round trip is the most trustworthy.
void filter(Server& sv) {
  int best = -1;
  for (int i = 0; i < sv.n; i++)
    if (millis() - sv.s[i].at < 3600000UL && (best < 0 || sv.s[i].delay < sv.s[best].delay)) best = i;
  sv.valid = best >= 0;
  if (!sv.valid) return;
  sv.off = sv.s[best].off;
  sv.delay = sv.s[best].delay;
  double sq = 0;
  for (int i = 0; i < sv.n; i++) { double d = sv.s[i].off - sv.off; sq += d * d; }
  sv.jitter = (int64_t)sqrt(sq / sv.n);
  // root distance: the most this server's time can be off by
  int64_t age = (millis() - sv.s[best].at) * 15 / 1000;   // 15 ppm dispersion growth, µs
  sv.dist = sv.rootDelay / 2 + sv.rootDisp + sv.delay / 2 + sv.jitter + age;
}

// Marzullo's algorithm: the offset range most servers agree on. Servers whose range misses it
// are falsetickers. The survivors are averaged, weighted by 1 / root distance.
void select() {
  int idx[NSRV], nc = 0;
  for (int i = 0; i < NSRV; i++) {
    Server& sv = srv[i];
    bool usable = sv.valid && sv.reach && sv.stratum >= 1 && sv.stratum <= 15 && sv.leap != 3 &&
                  millis() - sv.lastReply < 600000UL;
    sv.state = sv.n ? (usable ? 'x' : '-') : ' ';
    if (usable) idx[nc++] = i;
  }
  if (!nc) { m.good = 0; m.sel = -1; return; }

  struct Edge { int64_t v; int t; } e[2 * NSRV];
  int ne = 0;
  for (int k = 0; k < nc; k++) {
    Server& sv = srv[idx[k]];
    e[ne++] = {sv.off - sv.dist, -1};             // range opens
    e[ne++] = {sv.off + sv.dist, +1};             // range closes
  }
  std::sort(e, e + ne, [](const Edge& x, const Edge& y) { return x.v < y.v || (x.v == y.v && x.t < y.t); });
  int cnt = 0, best = 0;
  int64_t lo = 0, hi = 0;
  for (int i = 0; i < ne; i++) {
    cnt -= e[i].t;
    if (cnt > best) { best = cnt; lo = e[i].v; hi = e[i + 1 < ne ? i + 1 : i].v; }
  }
  bool majority = best * 2 > nc;

  double wsum = 0, osum = 0;
  int sel = -1;
  m.good = 0;
  for (int k = 0; k < nc; k++) {
    Server& sv = srv[idx[k]];
    bool truechimer = !majority ? true : (sv.off - sv.dist <= hi && sv.off + sv.dist >= lo);
    if (!truechimer) continue;
    sv.state = '+';
    m.good++;
    double w = 1.0 / max<int64_t>(sv.dist, 100);
    wsum += w; osum += w * sv.off;
    if (sel < 0 || sv.dist < srv[sel].dist) sel = idx[k];
  }
  if (sel < 0) { m.sel = -1; return; }
  srv[sel].state = '*';
  m.sel = sel;
  m.sysOff = (int64_t)(osum / wsum);
  double sq = 0;
  for (int k = 0; k < nc; k++) {
    Server& sv = srv[idx[k]];
    if (sv.state != '+' && sv.state != '*') continue;
    double d = sv.off - m.sysOff; sq += d * d;
  }
  m.sysJitter = (int64_t)sqrt(sq / m.good + (double)srv[sel].jitter * srv[sel].jitter);
  m.uncert = srv[sel].dist;
  m.sysOffL = nowL();
  m.lastSync = millis();
  if (!m.synced) {
    m.synced = true;
    m.firstSync = millis();
    m.anchor = m.sysOff;
    m.anchorAt = (nowL() + m.sysOff) / 1000000;
  }
}

// Keeps a history point every 30 s (faster at first) and refits the drift line through the
// last 30 minutes.
void addPoint() {
  static uint32_t lastAt = 0;
  uint32_t gap = m.hn < 8 ? 4000 : HIST_EVERY_MS;
  int64_t L = nowL();
  if (m.hn && millis() - lastAt < gap) {
    if (!m.haveDrift) { m.a = m.sysOff; m.xm = L; }        // until there is a line, follow the latest
    return;
  }
  lastAt = millis();
  if (m.hn == NHIST) { memmove(m.hx, m.hx + 1, (NHIST - 1) * 8); memmove(m.hy, m.hy + 1, (NHIST - 1) * 4); m.hn--; }
  m.hx[m.hn] = L;
  m.hy[m.hn] = (int32_t)(m.sysOff - m.anchor);
  m.hn++;

  // least squares over the last 30 minutes
  int first = m.hn - 1;
  while (first > 0 && L - m.hx[first - 1] < 1800LL * 1000000LL) first--;
  int n = m.hn - first;
  double sx = 0, sy = 0;
  for (int i = first; i < m.hn; i++) { sx += (m.hx[i] - L) / 1e6; sy += m.hy[i]; }
  double mx = sx / n, my = sy / n, sxx = 0, sxy = 0;
  for (int i = first; i < m.hn; i++) { double dx = (m.hx[i] - L) / 1e6 - mx; sxx += dx * dx; sxy += dx * (m.hy[i] - my); }
  double span = (m.hx[m.hn - 1] - m.hx[first]) / 1e6;
  if (n >= 5 && span >= 300 && sxx > 0) {
    m.ppm = sxy / sxx;                               // µs of offset per s = ppm
    m.xm = L + (int64_t)(mx * 1e6);
    m.a = m.anchor + (int64_t)my;
    double r = 0;
    for (int i = first; i < m.hn; i++) {
      double dx = (m.hx[i] - L) / 1e6 - mx, res = m.hy[i] - my - m.ppm * dx;
      r += res * res;
    }
    m.fitRms = sqrt(r / n);
    m.haveDrift = true;
  } else {
    m.a = m.sysOff; m.xm = L; m.ppm = 0;
  }
}

void handleReply(Server& sv, const uint8_t* r, int64_t t1, int64_t t4) {
  int64_t T2 = ntpToUs(r + 32), T3 = ntpToUs(r + 40);
  take();
  sv.leap = r[0] >> 6;
  sv.stratum = r[1];
  sv.precision = (int8_t)r[3];
  sv.rootDelay = short16(r + 4);
  sv.rootDisp = short16(r + 8);
  if (sv.stratum <= 1) {                          // a 4-letter source name (GPS, PPS, ...)
    memcpy(sv.refid, r + 12, 4); sv.refid[4] = 0;
    for (int i = 0; i < 4; i++) if (sv.refid[i] && (sv.refid[i] < 32 || sv.refid[i] > 126)) sv.refid[i] = '?';
  } else {
    snprintf(sv.refid, sizeof sv.refid, "%u.%u.%u.%u", r[12], r[13], r[14], r[15]);
  }
  Sample smp = {((T2 - t1) + (T3 - t4)) / 2, (t4 - t1) - (T3 - T2), millis()};
  sv.s[sv.head] = smp;
  sv.head = (sv.head + 1) % NSAMP;
  if (sv.n < NSAMP) sv.n++;
  sv.lastReply = millis();
  filter(sv);
  select();
  if (m.synced) addPoint();
  Serial.printf("ntp: %-10s st %u  vs combined %+7.3f ms  delay %6.2f ms | +/-%.2f ms  drift %+.2f ppm  %d good, using %s\n",
                sv.label, sv.stratum, (smp.off - m.sysOff) / 1000.0, smp.delay / 1000.0, m.uncert / 1000.0,
                -m.ppm, m.good, m.sel >= 0 ? srv[m.sel].label : "-");
  give();
}

void poller(void*) {
  uint8_t r[48];
  uint32_t lastSend = 0;
  for (;;) {
    if (resync) {
      resync = false;
      take();
      for (auto& sv : srv) { sv.burst = 4; sv.nextAt = millis(); }
      give();
    }
    if (!wifiUp() || millis() - lastSend < 500) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    int pick = -1;
    for (int i = 0; i < NSRV; i++)
      if ((int32_t)(millis() - srv[i].nextAt) >= 0 && (pick < 0 || (int32_t)(srv[pick].nextAt - srv[i].nextAt) > 0)) pick = i;
    if (pick < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    Server& sv = srv[pick];
    lastSend = millis();
    if ((!sv.ip || millis() - sv.resolvedAt > 3600000UL) && !resolve(sv)) { sv.nextAt = millis() + 30000; continue; }

    int64_t t1 = 0, t4 = 0;
    bool ok = exchange(sv, r, t1, t4);
    bool kod = ok && r[1] == 0;                   // stratum 0 = "kiss of death" (e.g. RATE)
    take();
    sv.reach = (sv.reach << 1) | (ok && !kod ? 1 : 0);
    if (sv.burst > 0) sv.burst--;
    sv.misses = ok ? 0 : sv.misses + 1;
    uint32_t wait = sv.burst > 0 ? BURST_MS : POLL_MS;
    if (kod) wait = 300000;                       // asked to back off
    else if (sv.misses >= 3) wait = 300000;       // not answering (the gateway may not run NTP)
    sv.nextAt = millis() + wait;
    give();
    if (ok && !kod) handleReply(sv, r, t1, t4);
    else if (ok) { take(); memcpy(sv.refid, r + 12, 4); sv.refid[4] = 0; give(); }
  }
}

// ---------- Drawing ----------
int page = 0;
const int NUM_PAGES = 3;
uint32_t digitsUs = 1500;                         // how long drawing the digits takes

// LED colours: lit, lit while not yet synced, and the unlit-segment glow (each survives the
// CYD's 8-bit buffer). Tap the digits to change colour; remembered.
struct LedColor { uint16_t on, dim, ghost; };
const LedColor LEDS[] = {
  {0xF800, 0x7800, 0x2000},   // red
  {0x07E0, 0x03E0, 0x0100},   // green
  {0x001F, 0x0010, 0x0008},   // dark blue
};
const int NUM_LEDS = sizeof(LEDS) / sizeof(LEDS[0]);
int ledIx = 0;
const int DIGITS_Y = 26, DIGITS_H = 58;      // where the digits are, for taps
const uint8_t SEG[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// A seven-segment digit with pointed segments, w x h with segment thickness t.
void digit(int x, int y, int w, int h, int t, int n, uint16_t on, uint16_t off) {
  int hh = t / 2, g = max(1, t / 4);
  int l = x + hh, r = x + w - hh, top = y + hh, mid = y + h / 2, bot = y + h - hh;
  auto hs = [&](int x0, int x1, int c, uint16_t col) {
    x0 += g; x1 -= g;
    spr.fillRect(x0 + hh, c - hh, x1 - x0 - 2 * hh + 1, t, col);
    spr.fillTriangle(x0, c, x0 + hh, c - hh, x0 + hh, c + hh, col);
    spr.fillTriangle(x1, c, x1 - hh, c - hh, x1 - hh, c + hh, col);
  };
  auto vs = [&](int y0, int y1, int c, uint16_t col) {
    y0 += g; y1 -= g;
    spr.fillRect(c - hh, y0 + hh, t, y1 - y0 - 2 * hh + 1, col);
    spr.fillTriangle(c, y0, c - hh, y0 + hh, c + hh, y0 + hh, col);
    spr.fillTriangle(c, y1, c - hh, y1 - hh, c + hh, y1 - hh, col);
  };
  uint8_t b = n >= 0 && n <= 9 ? SEG[n] : 0x40;   // anything else: a dash
  auto c = [&](int s) { return b & (1 << s) ? on : off; };
  hs(l, r, top, c(0)); vs(top, mid, r, c(1)); vs(mid, bot, r, c(2)); hs(l, r, bot, c(3));
  vs(mid, bot, l, c(4)); vs(top, mid, l, c(5)); hs(l, r, mid, c(6));
}

// HH:MM:SS.mmm in LED digits at y; sync = whether the time is real.
void bigTime(int64_t us, int y, bool sync) {
  time_t sec = us / 1000000;
  int ms = (us % 1000000) / 1000;
  tm t;
  localtime_r(&sec, &t);
  const LedColor& lc = LEDS[ledIx];
  const uint16_t on = sync ? lc.on : lc.dim, LED_GHOST = lc.ghost;
  const int W = 31, H = 58, T = 7;
  int v[6] = {t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10, t.tm_sec / 10, t.tm_sec % 10};
  int xs[6] = {3, 39, 86, 122, 169, 205};
  for (int i = 0; i < 6; i++) digit(xs[i], y, W, H, T, v[i], on, LED_GHOST);
  for (int cx : {78, 161}) {                      // colons blink with the second
    uint16_t cc = ms < 500 ? on : LED_GHOST;
    spr.fillRect(cx - 3, y + 15, 6, 6, cc);
    spr.fillRect(cx - 3, y + H - 21, 6, 6, cc);
  }
  spr.fillRect(242, y + H - 5, 5, 5, on);         // decimal point
  int sm[3] = {ms / 100, ms / 10 % 10, ms % 10};
  for (int i = 0; i < 3; i++) digit(250 + i * 23, y + H - 34, 19, 34, 4, sm[i], on, LED_GHOST);
}

String fmtMs(int64_t us, bool sign = true, int dec = 3) {   // µs -> "+1.234 ms" / "12.3 µs"
  char b[24];
  double ms = us / 1000.0;
  if (fabs(ms) < 1.0 && dec >= 3) snprintf(b, sizeof b, sign ? "%+.0f us" : "%.0f us", (double)us);
  else snprintf(b, sizeof b, sign ? "%+.*f ms" : "%.*f ms", dec, ms);
  return b;
}

String ago(uint32_t at) {
  if (!at) return "never";
  return fmtDur((millis() - at) / 1000) + " ago";
}

void label(const char* k, const String& v, int x, int y, uint16_t col = C_TEXT) {
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(k, x, y);
  spr.setTextColor(col, C_BG);
  int room = (y >= 195 ? 316 : x < 160 ? 160 : 316) - (x + 58);   // the bottom rows span the width
  spr.drawString(fit(v, room, 2), x + 58, y);
}

void syncHeader(const Model& md, const char* title) {
  String st = !md.synced ? String("NOT SYNCED") : "+/-" + fmtMs(md.uncert, false, 1);
  uint16_t c = !md.synced ? C_BAD : millis() - md.lastSync > 300000UL ? C_WARN : C_GOOD;
  header(title, st, C_ACC, c);
}

void pageClock(const Model& md, const Server* s) {
  syncHeader(md, "PRECISION CLOCK");
  int64_t sysUs = 0;
  { timeval tv; gettimeofday(&tv, nullptr); sysUs = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec; }

  // date line and the extra time scales
  int64_t L = nowL();
  int64_t utc = md.synced ? utcAt(md, L) : sysUs;
  time_t sec = utc / 1000000;
  tm lt, ut;
  localtime_r(&sec, &lt);
  gmtime_r(&sec, &ut);
  char b[64];
  strftime(b, sizeof b, "%a %d %b %Y  %Z", &lt);
  spr.setTextFont(2);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(b, 4, 90);
  snprintf(b, sizeof b, "UTC %02d:%02d:%02d", ut.tm_hour, ut.tm_min, ut.tm_sec);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(b, 316, 90);
  double mjd = utc / 86400e6 + 40587.0;
  char wk[16];
  strftime(wk, sizeof wk, "Day %j  Wk %V", &lt);
  snprintf(b, sizeof b, "Unix %lld.%03d   MJD %.5f   %s", (long long)sec, (int)(utc % 1000000 / 1000), mjd, wk);
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(b, 4, 108);
  spr.drawFastHLine(0, 119, 320, C_HDR);

  if (md.synced) {
    const Server& sel = s[md.sel >= 0 ? md.sel : 0];
    // how far the last measurement landed from the smooth clock shown (chrony's "last offset")
    label("Last off", fmtMs(md.sysOff - (utcAt(md, md.sysOffL) - md.sysOffL)), 4, 123);
    label("Jitter", fmtMs(md.sysJitter, false), 4, 141);
    label("Delay", md.sel >= 0 ? fmtMs(sel.delay, false, 1) : String("-"), 4, 159);
    label("Stratum", md.sel >= 0 ? String(sel.stratum + 1) + "  via " + String(sel.refid) : String("-"), 4, 177);
    char d1[24], d2[24];
    snprintf(d1, sizeof d1, "%+.2f ppm", -md.ppm);          // + = the crystal runs fast
    snprintf(d2, sizeof d2, "%+.3f s/day", -md.ppm * 0.0864);
    label("Drift", md.haveDrift ? String(d1) : String("learning..."), 164, 123, md.haveDrift ? C_TEXT : C_DIM);
    label("Per day", md.haveDrift ? String(d2) : String("-"), 164, 141);
    label("Source", md.sel >= 0 ? String(sel.label) : String("-"), 164, 159, C_ACC);
    label("Synced", ago(md.lastSync) + "  " + String(md.good) + "/" + String(NSRV), 164, 177);
    int64_t crystal = md.anchor - (md.a + (int64_t)((double)(L - md.xm) * md.ppm * 1e-6));
    label("Crystal", fmtMs(crystal) + "  free-running, uncorrected", 4, 199, C_WARN);
    label("System", fmtMs(sysUs - utc) + "  ESP32 clock (hourly SNTP)", 4, 217, C_DIM);
  } else {
    centered("Asking time servers...", 160, 2);
    centered("showing the ESP32 system time meanwhile", 182, 1);
  }

  // the digits last, for the time the frame actually reaches the screen
  int64_t d0 = nowL();
  int64_t lag = digitsUs + pushUs / 4;            // the digits sit in the top quarter
  int64_t tShow = md.synced ? utcAt(md, nowL() + lag) : sysUs + lag;
  bigTime(tShow, DIGITS_Y, md.synced);
  digitsUs = (digitsUs * 7 + (uint32_t)(nowL() - d0)) / 8;
  show(5);
}

void pageSources(const Model& md, const Server* s) {
  syncHeader(md, "TIME SOURCES");
  spr.setTextFont(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.drawString("  server      st reach    offset    delay  jitter", 2, 24);
  for (int i = 0; i < NSRV; i++) {
    const Server& sv = s[i];
    int y = 36 + i * 31;
    uint16_t c = sv.state == '*' ? C_GOOD : sv.state == '+' ? C_TEXT : sv.state == 'x' ? C_BAD : C_DIM;
    spr.setTextFont(2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(c, C_BG);
    spr.drawString(String(sv.state) + " " + sv.label, 2, y);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(sv.n ? String(sv.stratum) : String("-"), 108, y);
    char reach[5];
    snprintf(reach, sizeof reach, "%o", sv.reach);
    spr.drawString(reach, 142, y);
    spr.drawString(sv.valid ? fmtMs(sv.off) : String("-"), 222, y);
    spr.drawString(sv.valid ? String(sv.delay / 1000.0, 1) : String("-"), 270, y);
    spr.drawString(sv.valid ? String(sv.jitter / 1000.0, 2) : String("-"), 316, y);
    spr.setTextFont(1);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(C_DIM, C_BG);
    IPAddress ip(sv.ip);
    String sub = String(sv.host) + "  " + (sv.ip ? ip.toString() : String("?")) +
                 (sv.refid[0] ? "  ref " + String(sv.refid) : String("")) +
                 (sv.lastReply ? "  " + ago(sv.lastReply) : String(""));
    spr.drawString(fit(sub, 300, 1), 14, y + 17);
  }
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("* in use  + agrees  x falseticker  - unusable   ms", 2, 230);
  show(5);
}

void clockRow(const char* name, const String& note, int64_t us, int y, uint16_t col) {
  time_t sec = us / 1000000;
  tm t;
  localtime_r(&sec, &t);
  char b[20];
  snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.tm_hour, t.tm_min, t.tm_sec, (int)(us % 1000000 / 1000));
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(2);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(name, 4, y);
  spr.setTextFont(4);
  spr.setTextColor(col, C_BG);
  spr.drawString(b, 4, y + 15);
  spr.setTextFont(2);
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(note, 316, y + 20);
}

void pageClocks(const Model& md) {
  syncHeader(md, "THREE CLOCKS");
  int64_t L = nowL() + pushUs / 3;
  timeval tv; gettimeofday(&tv, nullptr);
  int64_t sysUs = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec + pushUs / 3;
  if (!md.synced) { centered("Waiting for the first sync", 120, 2); show(5); return; }
  int64_t utc = utcAt(md, L), crystal = L + md.anchor;
  clockRow("NTP (the reference)", "+/-" + fmtMs(md.uncert, false, 1), utc, 22, C_GOOD);
  clockRow("ESP32 system clock (SNTP)", fmtMs(sysUs - utc), sysUs, 63, C_TEXT);
  char since[24];
  tm at; localtime_r(&md.anchorAt, &at);
  strftime(since, sizeof since, "%H:%M", &at);
  clockRow((String("Crystal, free-running since ") + since).c_str(), fmtMs(crystal - utc), crystal, 104, C_WARN);

  // graph: how far the crystal has wandered (µs vs time), last hour
  const int GX = 30, GY = 150, GW = 286, GH = 76;
  spr.drawRect(GX, GY, GW, GH, C_HDR);
  if (md.hn >= 2) {
    int32_t lo = 0, hi = 0;
    for (int i = 0; i < md.hn; i++) { lo = min(lo, -md.hy[i]); hi = max(hi, -md.hy[i]); }
    if (hi - lo < 2000) { int32_t c = (hi + lo) / 2; lo = c - 1000; hi = c + 1000; }
    int64_t t0 = md.hx[0], t1 = md.hx[md.hn - 1];
    if (t1 - t0 < 60000000LL) t1 = t0 + 60000000LL;
    auto px = [&](int64_t x) { return GX + 1 + (int)((x - t0) * (GW - 3) / (t1 - t0)); };
    auto py = [&](int32_t v) { return GY + GH - 2 - (int)((int64_t)(v - lo) * (GH - 3) / (hi - lo)); };
    if (lo < 0 && hi > 0) spr.drawFastHLine(GX + 1, py(0), GW - 2, C_HDR);
    for (int i = 1; i < md.hn; i++)
      spr.drawLine(px(md.hx[i - 1]), py(-md.hy[i - 1]), px(md.hx[i]), py(-md.hy[i]), C_WARN);
    spr.setTextFont(1);
    spr.setTextColor(C_DIM, C_BG);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(String(hi / 1000.0, 1), GX - 2, GY);
    spr.setTextDatum(BR_DATUM);
    spr.drawString(String(lo / 1000.0, 1), GX - 2, GY + GH);
    spr.setTextDatum(BL_DATUM);
    spr.drawString("crystal error, ms  (" + fmtDur((t1 - t0) / 1000000) + ")", GX + 4, GY + GH - 2);
  } else {
    centered("graph fills in over the first minutes", GY + GH / 2, 1);
  }
  show(5);
}

// ---------- App ----------
void start() {
  Preferences p;
  p.begin("suite", true);
  ledIx = constrain(p.getInt("pcclr", 0), 0, NUM_LEDS - 1);
  p.end();
  setFullScreen();
  WiFi.setSleep(false);       // power save holds replies at the access point: uneven delays
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!task) xTaskCreatePinnedToCore(poller, "ntp", 6144, nullptr, 2, &task, 0);
  page = 0;
}

Model md;                       // snapshots: the poller changes the live ones under the lock
Server snap[NSRV];

void drawPage() {
  spr.fillSprite(C_BG);
  if (page == 0) pageClock(md, snap);
  else if (page == 1) pageSources(md, snap);
  else pageClocks(md);
}

void run() {
  static uint32_t lastSlow = 0;
  if (page == 1 && millis() - lastSlow < 500 && !redrawNow) return;
  lastSlow = millis();
  take();
  md = m;
  memcpy((void*)snap, (void*)srv, sizeof srv);
  give();
  frame(drawPage);
}

void press(int pin) {
#ifdef BOARD_CYD
  // a tap on the digits changes their colour instead of the page
  if (pin == BTN_B && page == 0 && touch::pressTap && touch::pressY >= DIGITS_Y - 6 && touch::pressY <= DIGITS_Y + DIGITS_H + 6) {
    ledIx = (ledIx + 1) % NUM_LEDS;
    Preferences p;
    p.begin("suite", false);
    p.putInt("pcclr", ledIx);
    p.end();
    redrawNow = true;
    return;
  }
#endif
  if (pin == BTN_B) page = (page + 1) % NUM_PAGES;
  else resync = true;
  redrawNow = true;
}

}  // namespace preclock
