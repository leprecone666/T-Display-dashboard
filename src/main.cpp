// T-Display dashboard: PC monitor / Claude Code status / network graphs.
// Data arrives over USB serial from pc/agent.py (one text line per message):
//   PC,cpu%,ram%,gpu%(-1=n/a),disk%,down_KBps,up_KBps,uptime_s[,gpu_temp_C]
//   CL,state,elapsed_s,tool        state: idle|working|waiting|done
//   TM,local_epoch_s               local wall-clock time as seconds
//   PG,name,ms,name,ms,...         ping results (ms<0 = timeout), up to 4 targets
//   US,5h%,5h_reset_min,wk%,wk_reset_min,ctx%   Claude plan limits (-1 = n/a)
//   TP,name,cpu%,ram_MB,...        top processes, up to 4
//   MD,state,title,artist,pos_s,dur_s   now playing (state: playing|paused|none)
//   GP,name,util%,temp,vram_used_MB,vram_total_MB,power_W,power_limit_W,fan%,clock_MHz
//   WX,temp,feels,wmo_code,wind_kmh,humidity%,tmax,tmin   weather
//   LK,0|1                         PC lock screen state
// Buttons: IO35 = next page (hold = auto-rotate), IO0 = brightness.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>

constexpr uint8_t ROTATION = 3;  // use 1 to flip the screen 180 degrees
constexpr int W = 240, H = 135;
constexpr uint8_t PIN_NEXT = 35, PIN_BRIGHT = 0, PIN_BL = 4;
constexpr uint32_t LINK_TIMEOUT_MS = 5000;
constexpr int HIST = 60;

TFT_eSPI tft;
TFT_eSprite spr(&tft);

uint16_t C_BG, C_TRACK, C_TEXT, C_MUTED, C_CYAN, C_VIOLET, C_GREEN, C_AMBER, C_RED, C_CLAUDE;

struct {
  float cpu = 0, ram = 0, gpu = -1, disk = 0, down = 0, up = 0;
  uint32_t uptime = 0;
  float gpuTemp = -1;
} pc;

time_t tmBase = 0;
uint32_t tmRx = 0;
char pgName[4][12];
int pgMs[4];
int pgN = 0;

int usFive = -1, usFiveMin = -1, usWeek = -1, usWeekMin = -1, usCtx = -1;
char tpName[4][12];
int tpCpu[4], tpMb[4];
int tpN = 0;
bool autoRotate = false;
uint32_t autoHoldUntil = 0;

TFT_eSprite mq(&tft);  // scratch strip for scrolling text
char mdState[10] = "none", mdTitle[64] = "", mdArtist[64] = "";
uint32_t mdPos = 0, mdDur = 0, mdRx = 0;

char gpName[16] = "";
int gpUtil = -1, gpTemp = -1, gpMemUsed = 0, gpMemTotal = 0, gpPower = -1, gpPowerLim = 0, gpFan = -1, gpClock = -1;
uint32_t gpRx = 0;

int wxTemp = 0, wxFeels = 0, wxCode = -1, wxWind = 0, wxHum = 0, wxMax = 0, wxMin = 0;
uint32_t wxRx = 0;

bool locked = false;
int prevPage = 0;
float curBr = 255;
const int NIGHT_FROM = 22, NIGHT_TO = 7;     // dim between 22:00 and 07:00
const int NIGHT_CAP = 25, AWAY_CAP = 6;      // brightness caps (0-255)

struct {
  char state[12] = "idle";
  char tool[28] = "";
  uint32_t elapsed = 0;
} cl;

uint32_t lastRx = 0;
float dCpu = 0, dRam = 0, dGpu = 0, dDisk = 0;
float histDown[HIST] = {0}, histUp[HIST] = {0};
int page = 0;
constexpr int PAGES = 9;  // 0 PC, 1 Claude, 2 Usage, 3 Top, 4 GPU, 5 Net, 6 Ping, 7 Clock, 8 Music
const uint8_t LEVELS[] = {255, 150, 70, 20};
int level = 0;

// ---------- helpers ----------
uint16_t mix(uint16_t a, uint16_t b, float t) {
  t = constrain(t, 0.f, 1.f);
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}

uint16_t loadColor(float p) {  // cyan -> amber -> red
  if (p < 0.6f) return C_CYAN;
  if (p < 0.85f) return mix(C_CYAN, C_AMBER, (p - 0.6f) / 0.25f);
  return mix(C_AMBER, C_RED, (p - 0.85f) / 0.15f);
}

// angles: degrees, clockwise, 0 = 6 o'clock (TFT_eSPI convention)
void arc(int x, int y, int r, int ir, float a0, float a1, uint16_t fg, uint16_t bg, bool round = true) {
  a0 = fmodf(a0 + 720.f, 360.f);
  a1 = fmodf(a1 + 720.f, 360.f);
  if (a0 <= a1) {
    spr.drawSmoothArc(x, y, r, ir, (uint32_t)a0, (uint32_t)a1, fg, bg, round);
  } else {
    spr.drawSmoothArc(x, y, r, ir, (uint32_t)a0, 360, fg, bg, false);
    spr.drawSmoothArc(x, y, r, ir, 0, (uint32_t)a1, fg, bg, false);
  }
}

void fmtRate(char* b, size_t n, float kbps) {
  if (kbps >= 1024) snprintf(b, n, "%.1f MB/s", kbps / 1024.f);
  else snprintf(b, n, "%.0f KB/s", kbps);
}

void fmtTime(char* b, size_t n, uint32_t s) {
  if (s >= 3600) snprintf(b, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
  else snprintf(b, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

bool linkUp() { return lastRx && millis() - lastRx < LINK_TIMEOUT_MS; }

void text(const char* s, int x, int y, int font, uint16_t col, uint8_t datum = ML_DATUM) {
  spr.setTextDatum(datum);
  spr.setTextColor(col, C_BG);
  spr.drawString(s, x, y, font);
}

void header(const char* title) {
  spr.fillSmoothCircle(8, 10, 3, linkUp() ? C_GREEN : C_RED, C_BG);
  text(title, 18, 10, 2, C_MUTED);
  if (autoRotate) text("AUTO", 128, 10, 1, C_CLAUDE);
  for (int i = 0; i < PAGES; i++) {
    int x = W - 8 - (PAGES - 1 - i) * 8;
    spr.fillSmoothCircle(x, 10, i == page ? 3 : 2, i == page ? C_TEXT : C_TRACK, C_BG);
  }
}

void bar(int x, int y, int w, int h, float p, uint16_t col) {
  spr.fillSmoothRoundRect(x, y, w, h, h / 2, C_TRACK, C_BG);
  if (p > 0.01f) spr.fillSmoothRoundRect(x, y, max((int)(w * p), h), h, h / 2, col, C_TRACK);
}

void statRow(int y, const char* label, float v, uint16_t col) {
  char b[12];
  if (v < 0) snprintf(b, sizeof b, "--");
  else snprintf(b, sizeof b, "%d%%", (int)(v + 0.5f));
  text(label, 124, y, 2, C_MUTED);
  text(b, 232, y, 2, v < 0 ? C_MUTED : C_TEXT, MR_DATUM);
  bar(124, y + 10, 108, 6, v < 0 ? 0 : v / 100.f, col);
}

void arrow(int x, int y, bool down, uint16_t col) {
  if (down) spr.fillTriangle(x - 4, y - 3, x + 4, y - 3, x, y + 4, col);
  else spr.fillTriangle(x - 4, y + 3, x + 4, y + 3, x, y - 4, col);
}

bool localTime(struct tm* out, uint32_t* msFrac = nullptr) {
  if (!tmRx) return false;
  uint32_t ms = millis() - tmRx;
  time_t t = tmBase + ms / 1000;
  gmtime_r(&t, out);
  if (msFrac) *msFrac = ms % 1000;
  return true;
}

// ---------- pages ----------
void pagePc() {
  header("PC MONITOR");
  const int cx = 62, cy = 70, R = 44, IR = 35;
  float p = constrain(dCpu / 100.f, 0.f, 1.f);
  arc(cx, cy, R, IR, 30, 330, C_TRACK, C_BG);
  if (p > 0.01f) arc(cx, cy, R, IR, 30, 30 + 300 * p, loadColor(p), C_BG);
  char b[16];
  snprintf(b, sizeof b, "%d", (int)(dCpu + 0.5f));
  text(b, cx, cy - 3, 4, C_TEXT, MC_DATUM);
  text("CPU %", cx, cy + 16, 2, C_MUTED, MC_DATUM);

  statRow(32, "RAM", dRam, loadColor(dRam / 100.f));
  char gl[16] = "GPU";
  if (pc.gpuTemp >= 0) snprintf(gl, sizeof gl, "GPU %dC", (int)(pc.gpuTemp + 0.5f));
  statRow(60, gl, pc.gpu < 0 ? -1 : dGpu, loadColor(dGpu / 100.f));
  statRow(88, "DISK C:", dDisk, loadColor(dDisk / 100.f));

  char r[16];
  arrow(10, 122, true, C_CYAN);
  fmtRate(r, sizeof r, pc.down);
  text(r, 20, 122, 2, C_TEXT);
  arrow(128, 122, false, C_VIOLET);
  fmtRate(r, sizeof r, pc.up);
  text(r, 138, 122, 2, C_TEXT);
}

void pageClaude() {
  header("CLAUDE CODE");
  const int cx = 58, cy = 76;
  float t = millis() / 1000.f;
  bool link = linkUp();
  const char* st = link ? cl.state : "offline";
  uint16_t col = C_MUTED;
  const char* label = "OFFLINE";

  if (!strcmp(st, "working")) {
    col = C_CLAUDE; label = "WORKING";
    float a = fmodf(t * 260.f, 360.f);
    arc(cx, cy, 40, 33, 0, 360, C_TRACK, C_BG, false);
    arc(cx, cy, 40, 33, a, a + 110, col, C_BG);
    arc(cx, cy, 40, 33, a + 180, a + 215, mix(C_BG, col, 0.45f), C_BG);
    int pr = 7 + (int)(3 * sinf(t * 5));
    spr.fillSmoothCircle(cx, cy, pr, col, C_BG);
  } else if (!strcmp(st, "waiting")) {
    col = C_AMBER; label = "NEEDS YOU";
    float k = 0.5f + 0.5f * sinf(t * 4.5f);
    arc(cx, cy, 40, 33, 0, 360, mix(C_TRACK, col, 0.3f + 0.7f * k), C_BG, false);
    spr.fillSmoothRoundRect(cx - 3, cy - 18, 7, 22, 3, col, C_BG);
    spr.fillSmoothCircle(cx, cy + 14, 4, col, C_BG);
  } else if (!strcmp(st, "done")) {
    col = C_GREEN; label = "DONE";
    arc(cx, cy, 40, 33, 0, 360, col, C_BG, false);
    spr.drawWideLine(cx - 16, cy + 1, cx - 5, cy + 12, 6, col, C_BG);
    spr.drawWideLine(cx - 5, cy + 12, cx + 17, cy - 11, 6, col, C_BG);
  } else {
    label = link ? "IDLE" : "OFFLINE";
    float k = 0.5f + 0.5f * sinf(t * 1.5f);
    arc(cx, cy, 40, 35, 0, 360, mix(C_TRACK, C_MUTED, 0.3f + 0.4f * k), C_BG, false);
    spr.fillSmoothCircle(cx, cy, 5, C_MUTED, C_BG);
  }

  text("STATUS", 116, 36, 2, C_MUTED);
  text(label, 116, 58, 4, col);

  char tool[14];
  snprintf(tool, sizeof tool, "%s", cl.tool);
  if (link && (!strcmp(st, "working") || !strcmp(st, "waiting")) && tool[0]) {
    text("TOOL", 116, 84, 2, C_MUTED);
    text(tool, 150, 84, 2, C_TEXT);
  }
  if (link && strcmp(st, "idle")) {
    char tm[12];
    fmtTime(tm, sizeof tm, cl.elapsed);
    text(!strcmp(st, "done") ? "TOOK" : "TIME", 116, 108, 2, C_MUTED);
    text(tm, 160, 108, 4, C_TEXT);
  }
}

void sparkline(int x, int y, int w, int h, const float* v, uint16_t col, float maxv) {
  float step = (float)w / (HIST - 1);
  uint16_t area = mix(C_BG, col, 0.22f);
  int px = x, py = y + h;
  for (int i = 0; i < HIST; i++) {
    float val = v[i] < 0 ? 0 : v[i];
    int cx = x + (int)(i * step);
    int cy = y + h - (int)(constrain(val / maxv, 0.f, 1.f) * h);
    spr.drawFastVLine(cx, cy, y + h - cy + 1, area);
    if (i) spr.drawLine(px, py, cx, cy, col);
    px = cx; py = cy;
  }
}

void pageNet() {
  header("NETWORK");
  float peakD = 100, peakU = 100;
  for (int i = 0; i < HIST; i++) {
    peakD = max(peakD, histDown[i]);
    peakU = max(peakU, histUp[i]);
  }
  char b[16];
  arrow(12, 30, true, C_CYAN);
  text("DOWN", 22, 30, 2, C_MUTED);
  fmtRate(b, sizeof b, pc.down);
  text(b, 230, 30, 2, C_TEXT, MR_DATUM);
  sparkline(10, 40, 220, 26, histDown, C_CYAN, peakD);

  arrow(12, 80, false, C_VIOLET);
  text("UP", 22, 80, 2, C_MUTED);
  fmtRate(b, sizeof b, pc.up);
  text(b, 230, 80, 2, C_TEXT, MR_DATUM);
  sparkline(10, 90, 220, 26, histUp, C_VIOLET, peakU);

  uint32_t u = pc.uptime;
  char up[24];
  snprintf(up, sizeof up, "UPTIME %lud %luh %lum", (unsigned long)(u / 86400), (unsigned long)((u / 3600) % 24), (unsigned long)((u / 60) % 60));
  text(up, 10, 126, 2, C_MUTED);
}


void fmtReset(char* b, size_t n, int min) {
  if (min < 0) snprintf(b, n, "--");
  else if (min >= 1440) snprintf(b, n, "resets %dd %dh", min / 1440, (min / 60) % 24);
  else snprintf(b, n, "resets %dh %02dm", min / 60, min % 60);
}

void usageRing(int cx, int cy, int pct, const char* label, int resetMin) {
  arc(cx, cy, 34, 27, 30, 330, C_TRACK, C_BG);
  char b[16];
  if (pct >= 0) {
    float q = constrain(pct / 100.f, 0.f, 1.f);
    if (q > 0.01f) arc(cx, cy, 34, 27, 30, 30 + 300 * q, loadColor(q), C_BG);
    snprintf(b, sizeof b, "%d", pct);
  } else {
    snprintf(b, sizeof b, "--");
  }
  text(b, cx, cy - 3, 4, C_TEXT, MC_DATUM);
  text(label, cx, cy + 17, 2, C_MUTED, MC_DATUM);
  fmtReset(b, sizeof b, resetMin);
  text(b, cx, 104, 2, C_MUTED, MC_DATUM);
}

void pageUsage() {
  header("CLAUDE USAGE");
  usageRing(62, 60, usFive, "5H", usFiveMin);
  usageRing(178, 60, usWeek, "WEEK", usWeekMin);
  text("CTX", 10, 125, 2, C_MUTED);
  bar(44, 122, 150, 6, usCtx < 0 ? 0 : usCtx / 100.f, usCtx < 0 ? C_MUTED : loadColor(usCtx / 100.f));
  char b[8];
  if (usCtx < 0) snprintf(b, sizeof b, "--");
  else snprintf(b, sizeof b, "%d%%", usCtx);
  text(b, 232, 125, 2, C_TEXT, MR_DATUM);
}

void pageTop() {
  header("TOP PROCESSES");
  if (!tpN) {
    text("collecting...", W / 2, 70, 2, C_MUTED, MC_DATUM);
    return;
  }
  for (int i = 0; i < tpN; i++) {
    int y = 34 + i * 24;
    float q = constrain(sqrtf(tpCpu[i] / 100.f), 0.f, 1.f);
    text(tpName[i], 8, y, 2, C_TEXT);
    bar(100, y - 3, 40, 6, q, loadColor(q));
    char b[12];
    snprintf(b, sizeof b, "%d%%", tpCpu[i]);
    text(b, 184, y, 2, C_TEXT, MR_DATUM);  // right edge 184, widest "100%" starts ~148
    if (tpMb[i] >= 1024) snprintf(b, sizeof b, "%.1fG", tpMb[i] / 1024.f);
    else snprintf(b, sizeof b, "%dM", tpMb[i]);
    text(b, 232, y, 2, C_MUTED, MR_DATUM);
  }
}

// Draws s into a w x 26 strip at (x,y); scrolls it when wider than the strip.
void marquee(int x, int y, int w, const char* s, int font, uint16_t col) {
  mq.fillSprite(C_BG);
  mq.setTextDatum(TL_DATUM);
  mq.setTextColor(col, C_BG);
  int tw = mq.textWidth(s, font);
  if (tw <= w) {
    mq.drawString(s, 0, 0, font);
  } else {
    const int gap = 36;
    float off = fmodf(millis() * 0.03f, (float)(tw + gap));
    mq.drawString(s, (int)-off, 0, font);
    mq.drawString(s, (int)(tw + gap - off), 0, font);
  }
  mq.pushToSprite(&spr, x, y);
}

void pageMusic() {
  header("NOW PLAYING");
  bool fresh = mdRx && millis() - mdRx < LINK_TIMEOUT_MS;
  bool playing = fresh && !strcmp(mdState, "playing");
  bool paused = fresh && !strcmp(mdState, "paused");
  float t = millis() / 1000.f;

  uint16_t col = playing ? C_GREEN : paused ? C_AMBER : C_MUTED;
  for (int i = 0; i < 7; i++) {  // equaliser
    int h = playing ? 6 + (int)(34 * (0.5f + 0.5f * sinf(t * (3.f + i * 0.9f) + i * 1.7f))) : 5;
    spr.fillSmoothRoundRect(10 + i * 10, 84 - h, 6, h, 3, col, C_BG);
  }

  if (!playing && !paused) {
    text("NOTHING PLAYING", 92, 56, 2, C_MUTED);
    text("start a track in Spotify", 92, 76, 2, C_TRACK);
    return;
  }
  marquee(92, 32, 140, mdTitle, 4, C_TEXT);
  marquee(92, 62, 140, mdArtist[0] ? mdArtist : "-", 2, C_MUTED);

  if (paused) text("PAUSED", 92, 100, 4, C_AMBER);
}

uint16_t tempColor(float t) {  // GPU temperature: cyan -> amber -> red
  if (t < 55) return C_CYAN;
  if (t < 75) return mix(C_CYAN, C_AMBER, (t - 55) / 20.f);
  return mix(C_AMBER, C_RED, (t - 75) / 15.f);
}

void statRowTxt(int y, const char* label, const char* val, float p, uint16_t col) {
  text(label, 124, y, 2, C_MUTED);
  text(val, 232, y, 2, C_TEXT, MR_DATUM);
  bar(124, y + 10, 108, 6, constrain(p, 0.f, 1.f), col);
}

void pageGpu() {
  header(gpName[0] ? gpName : "GPU");
  bool ok = gpRx && millis() - gpRx < LINK_TIMEOUT_MS && gpTemp >= 0;
  if (!ok) {
    text("no GPU data", W / 2, 70, 2, C_MUTED, MC_DATUM);
    return;
  }
  const int cx = 62, cy = 70, R = 44, IR = 35;
  float p = constrain(gpTemp / 100.f, 0.f, 1.f);
  arc(cx, cy, R, IR, 30, 330, C_TRACK, C_BG);
  if (p > 0.01f) arc(cx, cy, R, IR, 30, 30 + 300 * p, tempColor(gpTemp), C_BG);
  char b[20];
  snprintf(b, sizeof b, "%d", gpTemp);
  text(b, cx, cy - 3, 4, C_TEXT, MC_DATUM);
  text("TEMP C", cx, cy + 16, 2, C_MUTED, MC_DATUM);

  snprintf(b, sizeof b, "%d%%", gpUtil);
  text("LOAD", 124, 32, 2, C_MUTED);
  text(b, 232, 32, 2, C_TEXT, MR_DATUM);
  bar(124, 42, 108, 6, gpUtil / 100.f, loadColor(gpUtil / 100.f));

  float memP = gpMemTotal ? (float)gpMemUsed / gpMemTotal : 0.f;
  snprintf(b, sizeof b, "%.1f/%.0fG", gpMemUsed / 1024.f, gpMemTotal / 1024.f);
  statRowTxt(60, "VRAM", b, memP, loadColor(memP));

  float pwP = gpPowerLim > 0 ? (float)gpPower / gpPowerLim : 0.f;
  snprintf(b, sizeof b, "%dW", gpPower);
  statRowTxt(88, "POWER", b, pwP, loadColor(pwP));

  snprintf(b, sizeof b, "FAN %d%%", gpFan < 0 ? 0 : gpFan);
  text(b, 10, 122, 2, C_MUTED);
  snprintf(b, sizeof b, "%d MHz", gpClock);
  text(b, 230, 122, 2, C_MUTED, MR_DATUM);
}

// Weather glyph centred on (cx,cy), roughly 44 px wide. Codes are WMO weather codes.
void weatherIcon(int cx, int cy, int code) {
  float t = millis() / 1000.f;
  uint16_t cloudCol = mix(C_MUTED, C_TEXT, 0.55f);
  auto sun = [&](int x, int y, int r) {
    for (int i = 0; i < 8; i++) {
      float a = i * 0.7854f + t * 0.4f;
      spr.drawWideLine(x + cosf(a) * (r + 3), y + sinf(a) * (r + 3), x + cosf(a) * (r + 8), y + sinf(a) * (r + 8), 2, C_AMBER, C_BG);
    }
    spr.fillSmoothCircle(x, y, r, C_AMBER, C_BG);
  };
  auto cloud = [&](int x, int y, uint16_t c) {
    spr.fillSmoothCircle(x - 9, y + 4, 8, c, C_BG);
    spr.fillSmoothCircle(x + 3, y - 2, 11, c, C_BG);
    spr.fillSmoothCircle(x + 14, y + 4, 8, c, C_BG);
    spr.fillRect(x - 9, y + 4, 24, 8, c);
  };
  if (code == 0) {
    sun(cx, cy, 11);
  } else if (code <= 2) {
    sun(cx + 8, cy - 8, 8);
    cloud(cx - 2, cy + 4, cloudCol);
  } else if (code == 3) {
    cloud(cx, cy, cloudCol);
  } else if (code == 45 || code == 48) {
    cloud(cx, cy - 4, cloudCol);
    for (int i = 0; i < 3; i++) spr.drawFastHLine(cx - 14 + (i % 2) * 5, cy + 12 + i * 5, 26, C_MUTED);
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    cloud(cx, cy - 6, cloudCol);
    int ph = (int)(t * 12) % 6;
    for (int i = 0; i < 3; i++) {
      int x = cx - 10 + i * 9, y = cy + 8 + ((ph + i * 2) % 6) * 2;
      spr.drawWideLine(x, y, x - 3, y + 6, 2, C_CYAN, C_BG);
    }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    cloud(cx, cy - 6, cloudCol);
    int ph = (int)(t * 4) % 4;
    for (int i = 0; i < 3; i++) spr.fillSmoothCircle(cx - 10 + i * 10, cy + 10 + ((ph + i) % 4) * 3, 2, C_TEXT, C_BG);
  } else {
    cloud(cx, cy - 6, mix(C_MUTED, C_BG, 0.3f));
    spr.fillTriangle(cx + 2, cy + 4, cx - 6, cy + 16, cx + 1, cy + 14, C_AMBER);
    spr.fillTriangle(cx + 1, cy + 14, cx + 9, cy + 6, cx - 1, cy + 22, C_AMBER);
  }
}

void pageClock() {
  header("CLOCK");
  struct tm tv = {};
  uint32_t frac = 0;
  bool ok = localTime(&tv, &frac);
  char b[24] = "--:--";
  float sec = 0;
  if (ok) {
    snprintf(b, sizeof b, "%02d:%02d", tv.tm_hour, tv.tm_min);
    sec = tv.tm_sec + frac / 1000.f;
  }
  text(b, 8, 52, 6, C_TEXT);
  if (ok) {
    static const char* D[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char* M[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    snprintf(b, sizeof b, "%s  %d %s", D[tv.tm_wday], tv.tm_mday, M[tv.tm_mon]);
    text(b, 10, 92, 2, C_MUTED);
  }

  if (wxRx && wxCode >= 0) {  // weather block, right side
    weatherIcon(198, 44, wxCode);
    snprintf(b, sizeof b, "%dC", wxTemp);
    text(b, 198, 84, 4, C_TEXT, MC_DATUM);
    snprintf(b, sizeof b, "%d / %d", wxMax, wxMin);
    text(b, 198, 106, 2, C_MUTED, MC_DATUM);
    snprintf(b, sizeof b, "feels %d  %d%%", wxFeels, wxHum);
    text(b, 10, 108, 2, C_MUTED);
  } else {
    text("KYIV", 198, 50, 2, C_MUTED, MC_DATUM);
    text("no weather yet", 198, 70, 1, C_TRACK, MC_DATUM);
  }
  bar(10, 124, 220, 4, sec / 60.f, C_CLAUDE);
}

void pagePing() {
  header("PING");
  if (!pgN) {
    text("no targets", W / 2, 70, 2, C_MUTED, MC_DATUM);
    return;
  }
  for (int i = 0; i < pgN; i++) {
    int y = 34 + i * 24, ms = pgMs[i];
    uint16_t col = ms < 0 ? C_RED : ms < 60 ? C_GREEN : ms < 150 ? C_AMBER : C_RED;
    spr.fillSmoothCircle(10, y, 4, col, C_BG);
    text(pgName[i], 22, y, 2, C_TEXT);
    float p = ms < 0 ? 0.f : constrain(log10f(ms + 1.f) / log10f(301.f), 0.f, 1.f);
    bar(108, y - 3, 70, 6, p, col);
    char b[12];
    if (ms < 0) snprintf(b, sizeof b, "timeout");
    else snprintf(b, sizeof b, "%d ms", ms);
    text(b, 230, y, 2, col, MR_DATUM);
  }
}

// ---------- serial protocol ----------
int split(char* s, char** out, int maxn) {
  int n = 0;
  out[n++] = s;
  for (char* p = s; *p && n < maxn; p++)
    if (*p == ',') { *p = 0; out[n++] = p + 1; }
  return n;
}

void pushHist() {
  memmove(histDown, histDown + 1, (HIST - 1) * sizeof(float));
  memmove(histUp, histUp + 1, (HIST - 1) * sizeof(float));
  histDown[HIST - 1] = pc.down;
  histUp[HIST - 1] = pc.up;
}

void parseLine(char* line) {
  char* f[8];
  if (!strncmp(line, "PC,", 3)) {
    int n = split(line + 3, f, 8);
    if (n < 7) return;
    pc.cpu = atof(f[0]); pc.ram = atof(f[1]); pc.gpu = atof(f[2]); pc.disk = atof(f[3]);
    pc.down = atof(f[4]); pc.up = atof(f[5]); pc.uptime = strtoul(f[6], nullptr, 10);
    pc.gpuTemp = n > 7 ? atof(f[7]) : -1;
    pushHist();
    lastRx = millis();
  } else if (!strncmp(line, "TM,", 3)) {
    tmBase = (time_t)strtoul(line + 3, nullptr, 10);
    tmRx = millis();
    lastRx = tmRx;
  } else if (!strncmp(line, "PG,", 3)) {
    char* g[8];
    int n = split(line + 3, g, 8);
    pgN = 0;
    for (int i = 0; i + 1 < n && pgN < 4; i += 2) {
      snprintf(pgName[pgN], sizeof pgName[0], "%s", g[i]);
      pgMs[pgN++] = atoi(g[i + 1]);
    }
    lastRx = millis();
  } else if (!strncmp(line, "MD,", 3)) {
    char* g[6];
    if (split(line + 3, g, 6) < 5) return;
    snprintf(mdState, sizeof mdState, "%s", g[0]);
    snprintf(mdTitle, sizeof mdTitle, "%s", g[1]);
    snprintf(mdArtist, sizeof mdArtist, "%s", g[2]);
    mdPos = strtoul(g[3], nullptr, 10);
    mdDur = strtoul(g[4], nullptr, 10);
    mdRx = millis();
    lastRx = mdRx;
  } else if (!strncmp(line, "GP,", 3)) {
    char* g[10];
    if (split(line + 3, g, 10) < 9) return;
    snprintf(gpName, sizeof gpName, "%s", g[0]);
    gpUtil = atoi(g[1]); gpTemp = atoi(g[2]); gpMemUsed = atoi(g[3]); gpMemTotal = atoi(g[4]);
    gpPower = atoi(g[5]); gpPowerLim = atoi(g[6]); gpFan = atoi(g[7]); gpClock = atoi(g[8]);
    gpRx = millis();
    lastRx = gpRx;
  } else if (!strncmp(line, "WX,", 3)) {
    char* g[8];
    if (split(line + 3, g, 8) < 7) return;
    wxTemp = atoi(g[0]); wxFeels = atoi(g[1]); wxCode = atoi(g[2]); wxWind = atoi(g[3]);
    wxHum = atoi(g[4]); wxMax = atoi(g[5]); wxMin = atoi(g[6]);
    wxRx = millis();
  } else if (!strncmp(line, "LK,", 3)) {
    bool now = atoi(line + 3) != 0;
    if (now && !locked) { prevPage = page; page = 7; }   // lock screen: show the clock
    if (!now && locked) page = prevPage;
    locked = now;
    lastRx = millis();
  } else if (!strncmp(line, "US,", 3)) {
    char* g[6];
    if (split(line + 3, g, 6) < 5) return;
    usFive = atoi(g[0]); usFiveMin = atoi(g[1]); usWeek = atoi(g[2]); usWeekMin = atoi(g[3]); usCtx = atoi(g[4]);
    lastRx = millis();
  } else if (!strncmp(line, "TP,", 3)) {
    char* g[12];
    int n = split(line + 3, g, 12);
    tpN = 0;
    for (int i = 0; i + 2 < n && tpN < 4; i += 3) {
      snprintf(tpName[tpN], sizeof tpName[0], "%s", g[i]);
      tpCpu[tpN] = atoi(g[i + 1]);
      tpMb[tpN++] = atoi(g[i + 2]);
    }
    lastRx = millis();
  } else if (!strncmp(line, "CL,", 3)) {
    int n = split(line + 3, f, 3);  // tool may itself contain commas: keep the rest
    if (n < 2) return;
    char prev[12];
    strcpy(prev, cl.state);
    snprintf(cl.state, sizeof cl.state, "%s", f[0]);
    cl.elapsed = strtoul(f[1], nullptr, 10);
    snprintf(cl.tool, sizeof cl.tool, "%s", n > 2 ? f[2] : "");
    bool attention = (!strcmp(cl.state, "waiting") && strcmp(prev, "waiting")) ||
                     (!strcmp(cl.state, "done") && strcmp(prev, "done"));
    if (attention) {  // pull attention to the Claude page and pause auto-rotate
      page = 1;
      autoHoldUntil = millis() + 15000;
    }
    lastRx = millis();
  }
}

void readSerial() {
  static char buf[96];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      buf[len] = 0;
      if (len) parseLine(buf);
      len = 0;
    } else if (c != '\r' && len < sizeof buf - 1) {
      buf[len++] = c;
    }
  }
}

// ---------- buttons ----------
void buttons() {
  static bool lastNext = true, lastBr = true;
  static uint32_t tDown = 0, tBr = 0, tAuto = 0;
  uint32_t now = millis();
  bool n = digitalRead(PIN_NEXT), b = digitalRead(PIN_BRIGHT);

  if (!n && lastNext) tDown = now;  // pressed
  if (n && !lastNext && now - tDown > 40) {  // released
    if (now - tDown >= 800) autoRotate = !autoRotate;
    else page = (page + 1) % PAGES;
    tAuto = now;
  }
  if (!b && lastBr && now - tBr > 200) {
    level = (level + 1) % (sizeof LEVELS);
    tBr = now;
  }
  lastNext = n; lastBr = b;

  if (autoRotate && now > autoHoldUntil && now - tAuto > 6000) {
    page = (page + 1) % PAGES;
    tAuto = now;
  }
}

// The visible 135x240 window is a slice of the ST7789's 240x320 RAM; wipe the rest
// so stale data around the edges does not shimmer through.
void clearPanelRam() {
  tft.startWrite();
  tft.writecommand(0x2A);
  tft.writedata(0); tft.writedata(0); tft.writedata(0); tft.writedata(239);
  tft.writecommand(0x2B);
  tft.writedata(0); tft.writedata(0); tft.writedata(1); tft.writedata(0x3F);
  tft.writecommand(0x2C);
  for (uint32_t i = 0; i < 240UL * 320UL * 2; i++) tft.writedata(0);
  tft.endWrite();
}

void splash() {
  for (int i = 0; i <= 40; i++) {
    spr.fillSprite(C_BG);
    float p = i / 40.f;
    arc(W / 2, 58, 30, 24, 0, 360, C_TRACK, C_BG, false);
    arc(W / 2, 58, 30, 24, i * 9, i * 9 + 40 + 200 * p, C_CLAUDE, C_BG);
    text("T-DISPLAY", W / 2, 104, 4, C_TEXT, MC_DATUM);
    text("waiting for PC...", W / 2, 124, 2, C_MUTED, MC_DATUM);
    spr.pushSprite(0, 0);
    delay(15);
  }
}

void setup() {
  Serial.setRxBufferSize(512);
  Serial.begin(115200);
  pinMode(PIN_NEXT, INPUT);  // GPIO35 is input-only, external pull-up on the board
  pinMode(PIN_BRIGHT, INPUT_PULLUP);

  tft.init();
  clearPanelRam();
  tft.setRotation(ROTATION);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
  ledcWrite(0, LEVELS[level]);

  C_BG = tft.color565(10, 14, 24);
  C_TRACK = tft.color565(30, 38, 58);
  C_TEXT = tft.color565(235, 240, 250);
  C_MUTED = tft.color565(120, 132, 158);
  C_CYAN = tft.color565(34, 211, 238);
  C_VIOLET = tft.color565(167, 139, 250);
  C_GREEN = tft.color565(52, 211, 153);
  C_AMBER = tft.color565(251, 191, 36);
  C_RED = tft.color565(248, 113, 113);
  C_CLAUDE = tft.color565(217, 119, 87);

  spr.setColorDepth(16);
  spr.createSprite(W, H);
  mq.setColorDepth(16);
  mq.createSprite(140, 26);
  splash();
}

void loop() {
  static uint32_t lastFrame = 0;
  readSerial();
  buttons();

  uint32_t now = millis();
  if (now - lastFrame < 33) return;  // ~30 fps
  lastFrame = now;

  // ease displayed values toward targets
  dCpu += (pc.cpu - dCpu) * 0.25f;
  dRam += (pc.ram - dRam) * 0.25f;
  dGpu += ((pc.gpu < 0 ? 0 : pc.gpu) - dGpu) * 0.25f;
  dDisk += (pc.disk - dDisk) * 0.25f;

  // auto-dimmer: night hours, locked PC or lost link cap the brightness; fades smoothly
  int target = LEVELS[level];
  struct tm lt;
  if (localTime(&lt) && (lt.tm_hour >= NIGHT_FROM || lt.tm_hour < NIGHT_TO)) target = min(target, NIGHT_CAP);
  if (locked || (lastRx && now - lastRx > 15000)) target = min(target, AWAY_CAP);
  curBr += (target - curBr) * 0.08f;
  ledcWrite(0, (int)(curBr + 0.5f));

  spr.fillSprite(C_BG);
  if (page == 0) pagePc();
  else if (page == 1) pageClaude();
  else if (page == 2) pageUsage();
  else if (page == 3) pageTop();
  else if (page == 4) pageGpu();
  else if (page == 5) pageNet();
  else if (page == 6) pagePing();
  else if (page == 7) pageClock();
  else pageMusic();
  spr.pushSprite(0, 0);
}
