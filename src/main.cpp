// =============================================================================
//  PlaneRadar 3in1 - ESP32-C3 + GC9A01 240x240 SPI round panel
//
//  Screens : 0 = clock | 1 = aircraft radar (adsb.fi) | 2 = rain radar (RainViewer)
//  Key     : GPIO9 (BOOT button)  short press = next screen, hold 0.9 s = next range
//
//  FIXED WIRING (must not be changed):
//    LCD VCC  = 3V3   LCD GND   = GND
//    TFT_RST  = GPIO0  TFT_CS  = GPIO1
//    TFT_DC   = GPIO10 TFT_MOSI = GPIO3   TFT_SCLK = GPIO4
//    KEY      = GPIO9 (on-board BOOT button, INPUT_PULLUP, active low)
//    (no backlight pin on this panel; BL is tied to 3V3)
//
//  Notes on this revision:
//   * ESP32-C3 has NO PSRAM, so there is no 240x240 framebuffer / canvas.
//     Everything is drawn straight on the panel with incremental refresh
//     (only the beam and plane icons are repainted per frame) => no flicker.
//   * Rain tiles are PNG-decoded line by line and clipped to the dial circle,
//     so no 115KB overlay buffer is needed.
//   * RainViewer is fetched over plain HTTP on purpose: it answers 200 on port
//     80, so the weather path needs no TLS at all (no handshake stalls, ~45 KB
//     heap saved). adsb.fi redirects to HTTPS, so only the plane query uses TLS
//     and its handshake is capped at 6 s (WiFiClientSecure takes seconds).
//   * Default centre is Guangzhou Baiyun airport for bench testing; the config
//     portal can move it anywhere.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include <Arduino_GFX_Library.h>
#include <PNGdec.h>

#include "ChinaMap.h"

// ---------------------------------------------------------------- hardware --
#ifndef PR_SCLK
#define PR_SCLK 4
#endif
#ifndef PR_MOSI
#define PR_MOSI 3
#endif
#ifndef PR_CS
#define PR_CS   1
#endif
#ifndef PR_DC
#define PR_DC   10
#endif
#ifndef PR_RST
#define PR_RST  0
#endif
#ifndef PR_KEY
#define PR_KEY  9
#endif

#define PIN_SCLK PR_SCLK
#define PIN_MOSI PR_MOSI
#define PIN_CS   PR_CS
#define PIN_DC   PR_DC
#define PIN_RST  PR_RST
#define PIN_BTN  PR_KEY
#define PIN_BL   (-1)          // no backlight pin on this panel

#define LCD_W 240
#define LCD_H 240
#define R_CX  (LCD_W / 2)
#define R_CY  (LCD_H / 2)
#define R_R   112

#define SPI_HZ 27000000     // 27 MHz: safe on dupont wires, still no visible lag
#define TLS_HANDSHAKE_S 6   // seconds; a stuck host must not freeze the UI

// ------------------------------------------------------------------ config --
#define FW_NAME "PlaneRadar 3in1"
#define FW_VER  "1.0.8-c3"
#define AP_NAME "PlaneRadar-Setup"
#define NTP1    "ntp.aliyun.com"
#define NTP2    "cn.pool.ntp.org"

// built-in WiFi, used when NVS has nothing saved yet (fresh chip)
#define DEFAULT_SSID "NKWRJ"
#define DEFAULT_PASS "15107077996h"

// default position: Guangzhou Baiyun Intl (ZGGG)
#define DEFAULT_LAT 23.3924f
#define DEFAULT_LON 113.2988f
#define CFG_VER 105        // bumping this re-seeds the stored lat/lon once

static const float RANGES_KM[] = {25.0f, 50.0f, 100.0f, 200.0f};
static const int   RANGE_N = sizeof(RANGES_KM) / sizeof(RANGES_KM[0]);

#define MAX_PLANES 24

#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_GREEN   0x07E0
#define C_DKGREEN 0x0320
#define C_DKRED   0x3000
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_GRAY    0x8410
#define C_ORANGE  0xFD20
#define C_BLUE    0x041F

enum { SCR_CLOCK = 0, SCR_PLANES = 1, SCR_WEATHER = 2 };

// ----------------------------------------------------------------- globals --
static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX     *gfx   = nullptr;

static int      s_screen = SCR_PLANES;
static int      s_range  = 3;              // index into RANGES_KM (200 km by default)
static float    s_lat    = DEFAULT_LAT;
static float    s_lon    = DEFAULT_LON;
static String   s_ssid, s_pass;

static Preferences prefs;
static WebServer   server(80);
static bool        s_portal = false;

// ------------------------------------------------------------ plane storage --
struct Plane {
  float lat, lon, dist, brg, track;
  int   altFt, gsKt;
  bool  hasTrack;
  char  cs[9];
};
static Plane s_pl[MAX_PLANES];
static int   s_plN = 0;
static unsigned long s_nextFetch = 0;
static bool  s_adsbOk = false;
static String s_adsbMsg = "wait";

// ---------------------------------------------------------------- weather ----
static uint8_t *s_pngData = nullptr;
static int      s_pngCap  = 0;
static bool     s_wxOk    = false;
static String   s_wxMsg   = "wait";
static unsigned long s_wxStamp = 0;
static int      s_tileDx = 0, s_tileDy = 0;
static bool     s_wxDry  = false;      // true while self-testing, no pixels drawn
static bool     s_wxNeed = false;      // page asks for a tile, loop() does the work
static bool     s_wxProbeDone = false; // one-shot boot rain check (runs from loop)

// radar beam
static float s_sweep  = 0.0f;
static bool  s_beamOn = false;

// clock hands cache
static int  s_prevHand[6] = {0, 0, 0, 0, 0, 0};
static bool s_handValid = false;

static PNG s_png;

// ------------------------------------------------------------------- utils ---
static void textC(const char *t, int y, uint16_t col, uint8_t sz) {
  int w = (int)strlen(t) * 6 * sz;
  gfx->setTextSize(sz);
  gfx->setTextColor(col);
  gfx->setCursor(R_CX - w / 2, y);
  gfx->print(t);
}

static void textAt(const char *t, int x, int y, uint16_t col, uint8_t sz) {
  gfx->setTextSize(sz);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(t);
}

// local tangent-plane conversion, good enough inside a 100 km radius
static void geoToPolar(float lat, float lon, float *distKm, float *brgDeg) {
  float dLat = (lat - s_lat) * 0.0174532925f;
  float dLon = (lon - s_lon) * 0.0174532925f;
  float mlat = (lat + s_lat) * 0.5f * 0.0174532925f;
  float dx = dLon * 111.320f * cosf(mlat);
  float dy = dLat * 110.574f;
  *distKm = sqrtf(dx * dx + dy * dy);
  float b = atan2f(dx, dy) * 57.2957795f;     // 0 = north, clockwise
  if (b < 0) b += 360.0f;
  *brgDeg = b;
}

// ============================================================ drawing parts ==
static void drawRings() {
  gfx->drawCircle(R_CX, R_CY, R_R, C_GREEN);
  gfx->drawCircle(R_CX, R_CY, R_R * 2 / 3, C_DKGREEN);
  gfx->drawCircle(R_CX, R_CY, R_R / 3, C_DKGREEN);

  gfx->drawLine(R_CX, R_CY - R_R, R_CX, R_CY - R_R + 8, C_GREEN);
  gfx->drawLine(R_CX, R_CY + R_R - 8, R_CX, R_CY + R_R, C_GREEN);
  gfx->drawLine(R_CX - R_R, R_CY, R_CX - R_R + 8, R_CY, C_GREEN);
  gfx->drawLine(R_CX + R_R - 8, R_CY, R_CX + R_R, R_CY, C_GREEN);

  char b[16];
  snprintf(b, sizeof(b), "%dKM", (int)RANGES_KM[s_range]);
  textC(b, 24, C_CYAN, 1);
}

static void drawBeam(float deg, uint16_t col) {
  float a = deg * 0.0174532925f;
  int x2 = R_CX + (int)(R_R * sinf(a));
  int y2 = R_CY - (int)(R_R * cosf(a));
  gfx->drawLine(R_CX, R_CY, x2, y2, col);
}

static uint16_t altColor(int altFt) {
  if (altFt <= 0) return C_GRAY;
  float km = altFt * 0.0003048f;
  if (km < 2.0f) return C_RED;
  if (km < 6.0f) return C_ORANGE;
  if (km < 10.0f) return C_YELLOW;
  return C_BLUE;
}

static void drawPlaneIcon(int x, int y, float trackDeg, bool hasTrack, uint16_t col) {
  if (!hasTrack) {
    gfx->fillCircle(x, y, 2, col);
    return;
  }
  float a = trackDeg * 0.0174532925f;
  const float pts[6] = {0.0f, -6.0f, -4.0f, 5.0f, 4.0f, 5.0f};
  int px[3], py[3];
  for (int i = 0; i < 3; i++) {
    float x0 = pts[i * 2], y0 = pts[i * 2 + 1];
    px[i] = x + (int)(x0 * cosf(a) - y0 * sinf(a));
    py[i] = y + (int)(x0 * sinf(a) + y0 * cosf(a));
  }
  gfx->fillTriangle(px[0], py[0], px[1], py[1], px[2], py[2], col);
}

// icons only - also used every animation frame to repair what the beam erase hit
static void drawPlaneIcons() {
  float scale = (float)R_R / RANGES_KM[s_range];
  for (int i = 0; i < s_plN; i++) {
    float brg = s_pl[i].brg * 0.0174532925f;
    float d = s_pl[i].dist * scale;
    int x = R_CX + (int)(d * sinf(brg));
    int y = R_CY - (int)(d * cosf(brg));
    if ((x - R_CX) * (x - R_CX) + (y - R_CY) * (y - R_CY) > R_R * R_R) continue;
    drawPlaneIcon(x, y, s_pl[i].track, s_pl[i].hasTrack, altColor(s_pl[i].altFt));
  }
}

static void drawPlaneLabels() {
  float scale = (float)R_R / RANGES_KM[s_range];
  int labelled = 0;
  for (int i = 0; i < s_plN && labelled < 5; i++) {
    if (!s_pl[i].cs[0]) continue;
    float brg = s_pl[i].brg * 0.0174532925f;
    float d = s_pl[i].dist * scale;
    int x = R_CX + (int)(d * sinf(brg));
    int y = R_CY - (int)(d * cosf(brg));
    if ((x - R_CX) * (x - R_CX) + (y - R_CY) * (y - R_CY) > R_R * R_R) continue;
    char lb[24];
    int altM = (int)(s_pl[i].altFt * 0.3048f);
    if (altM > 0) snprintf(lb, sizeof(lb), "%s %dm", s_pl[i].cs, altM);
    else          snprintf(lb, sizeof(lb), "%s", s_pl[i].cs);
    int tx = x + 5, ty = y - 3;
    if (tx > LCD_W - 42) tx = x - 5 - (int)strlen(lb) * 6;
    if (ty < 20) ty = 20;
    if (ty > LCD_H - 14) ty = LCD_H - 14;
    textAt(lb, tx, ty, C_WHITE, 1);
    labelled++;
  }
}

static void drawPlaneFooter() {
  gfx->fillRect(0, 172, LCD_W, 68, C_BLACK);   // clear the band first

  if (s_plN > 0) {
    int best = 0;
    for (int i = 1; i < s_plN; i++) if (s_pl[i].dist < s_pl[best].dist) best = i;
    char d[40];
    snprintf(d, sizeof(d), "%s %dm %dkmh", s_pl[best].cs,
             (int)(s_pl[best].altFt * 0.3048f),
             (int)(s_pl[best].gsKt * 1.852f));
    textC(d, 172, C_WHITE, 1);
  } else {
    char m[40];
    if (!s_adsbOk) snprintf(m, sizeof(m), "ADSB:%s", s_adsbMsg.c_str());
    else           snprintf(m, sizeof(m), "NO AIRCRAFT");
    textC(m, 172, C_GRAY, 1);
  }

  char st[28];
  snprintf(st, sizeof(st), "%dKM  %d ACFT", (int)RANGES_KM[s_range], s_plN);
  textC(st, 188, C_GRAY, 1);
  textC("KEY=NEXT  HOLD=RANGE", 204, 0x4208, 1);
}

static void renderPlanes() {
  gfx->fillScreen(C_BLACK);
  drawRings();
  drawPlaneIcons();
  drawPlaneLabels();
  drawPlaneFooter();
  gfx->fillCircle(R_CX, R_CY, 2, C_GREEN);
  drawBeam(s_sweep, C_GREEN);
  s_beamOn = true;
}

// ------------------------------------------------------------------- clock ---
static void drawDial() {
  gfx->drawCircle(R_CX, R_CY, 78, C_DKGREEN);
  for (int i = 0; i < 12; i++) {
    float a = i * 30.0f * 0.0174532925f;
    int x1 = R_CX + (int)(70 * sinf(a));
    int y1 = R_CY - (int)(70 * cosf(a));
    int x2 = R_CX + (int)(76 * sinf(a));
    int y2 = R_CY - (int)(76 * cosf(a));
    gfx->drawLine(x1, y1, x2, y2, (i % 3 == 0) ? C_WHITE : C_GRAY);
  }
}

static void drawChinaFaint() {
  unsigned long off = 0;
  for (int r = 0; r < CN_RING_COUNT; r++) {
    int n = CN_RING_LEN[r];
    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
      uint16_t qx = pgm_read_word(&CN_PTS[off + i * 2]);
      uint16_t qy = pgm_read_word(&CN_PTS[off + i * 2 + 1]);
      int x = qx / 10, y = qy / 10;
      if (i > 0) gfx->drawLine(px, py, x, y, 0x0240);
      px = x; py = y;
    }
    off += n * 2;
  }
  int lx = (int)(CN_OX + (s_lon - CN_LON_MIN) * CN_KX);
  int ly = (int)(CN_OY + (CN_LAT_MAX - s_lat) * CN_KY);
  if (lx > 0 && lx < LCD_W && ly > 0 && ly < LCD_H) {
    gfx->fillCircle(lx, ly, 3, C_RED);
    gfx->drawCircle(lx, ly, 6, C_DKRED);
  }
}

static void drawClockText(struct tm *lt) {
  char b[28];
  gfx->fillRect(50, 92, 140, 22, C_BLACK);
  snprintf(b, sizeof(b), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
  textC(b, 94, C_CYAN, 2);

  gfx->fillRect(30, 118, 180, 12, C_BLACK);
  static const char *WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  snprintf(b, sizeof(b), "%04d-%02d-%02d  %s", lt->tm_year + 1900, lt->tm_mon + 1,
           lt->tm_mday, WD[lt->tm_wday]);
  textC(b, 119, C_GRAY, 1);
}

static void drawHands(struct tm *lt) {
  float ha = (lt->tm_hour % 12) * 30.0f + lt->tm_min * 0.5f;
  float ma = lt->tm_min * 6.0f;
  float sa = lt->tm_sec * 6.0f;

  int hx = R_CX + (int)(40 * sinf(ha * 0.0174532925f));
  int hy = R_CY - (int)(40 * cosf(ha * 0.0174532925f));
  int mx = R_CX + (int)(62 * sinf(ma * 0.0174532925f));
  int my = R_CY - (int)(62 * cosf(ma * 0.0174532925f));
  int sx = R_CX + (int)(66 * sinf(sa * 0.0174532925f));
  int sy = R_CY - (int)(66 * cosf(sa * 0.0174532925f));

  gfx->drawLine(R_CX, R_CY, hx, hy, C_WHITE);
  gfx->drawLine(R_CX, R_CY, mx, my, C_WHITE);
  gfx->drawLine(R_CX, R_CY, sx, sy, C_RED);

  s_prevHand[0] = hx; s_prevHand[1] = hy;
  s_prevHand[2] = mx; s_prevHand[3] = my;
  s_prevHand[4] = sx; s_prevHand[5] = sy;
  s_handValid = true;
}

static void renderClock() {
  gfx->fillScreen(C_BLACK);
  drawChinaFaint();
  drawDial();
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  drawClockText(&lt);
  s_handValid = false;
  drawHands(&lt);
  gfx->fillCircle(R_CX, R_CY, 3, C_GREEN);
}

static void clockTick() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);

  if (s_handValid) {          // erase old hands, then restore the ticks they crossed
    gfx->drawLine(R_CX, R_CY, s_prevHand[0], s_prevHand[1], C_BLACK);
    gfx->drawLine(R_CX, R_CY, s_prevHand[2], s_prevHand[3], C_BLACK);
    gfx->drawLine(R_CX, R_CY, s_prevHand[4], s_prevHand[5], C_BLACK);
    drawDial();
  }
  drawClockText(&lt);
  drawHands(&lt);
  gfx->fillCircle(R_CX, R_CY, 3, C_GREEN);
}

// ================================================================= weather ==
#define WX_ZOOM 7
static const char *WX_META_URL  = "http://api.rainviewer.com/public/weather-maps.json";
static const char *WX_TILE_HOST = "http://tilecache.rainviewer.com";

// one decoded PNG row -> panel, clipped to the round dial
static void pngRow(PNGDRAW *pDraw) {
  static int s_rowLog = 0;
  bool doLog = (pDraw->y == 0 || pDraw->y == 1 || pDraw->y == 63 || pDraw->y == 127 ||
                p Draw->y == 191 || pDraw->y == 255);
  if (doLog) {
    Serial.printf("[wx] row y=%d w=%d type=%d enter\n", pDraw->y, pDraw->iWidth,
                  pDraw->iPixelType);
    Serial.flush();
  }
  int ty = pDraw->y + s_tileDy;
  if (ty < 0 || ty >= LCD_H) return;
  int iPixelType = pDraw->iPixelType;
  uint8_t *px = pDraw->pPixels;
  int dy = ty - R_CY;
  int dy2 = dy * dy;

  gfx->startWrite();
  for (int i = 0; i < pDraw->iWidth; i++) {
    int tx = i + s_tileDx;
    if (tx < 0 || tx >= LCD_W) continue;
    int dx = tx - R_CX;
    if (dx * dx + dy2 > R_R * R_R) continue;

    uint16_t col = 0;
    bool transparent = false;

    if (iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA) {
      if (px[i * 4 + 3] < 30) transparent = true;
      else col = (uint16_t)(((px[i * 4] & 0xF8) << 8) | ((px[i * 4 + 1] & 0xFC) << 3) |
                            (px[i * 4 + 2] >> 3));
    } else if (iPixelType == PNG_PIXEL_TRUECOLOR) {
      col = (uint16_t)(((px[i * 3] & 0xF8) << 8) | ((px[i * 3 + 1] & 0xFC) << 3) |
                       (px[i * 3 + 2] >> 3));
    } else if (iPixelType == PNG_PIXEL_INDEXED) {
      uint8_t idx = px[i];
      if (idx == 0) transparent = true;
      else {
        uint8_t r = pDraw->pPalette[idx * 3 + 0];
        uint8_t g = pDraw->pPalette[idx * 3 + 1];
        uint8_t b = pDraw->pPalette[idx * 3 + 2];
        col = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    } else {
      uint8_t g8 = px[i];
      col = (uint16_t)(((g8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (g8 >> 3));
    }

    if (!transparent && !s_wxDry) gfx->drawPixel(tx, ty, col);
  }
  gfx->endWrite();
  if (doLog) {
    Serial.printf("[wx] row y=%d exit\n", pDraw->y);
    Serial.flush();
    s_rowLog++;
  }
}

static void allocPngBuffer() {
  static const int caps[] = {73728, 65536, 49152, 32768};
  for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
    s_pngData = (uint8_t *)malloc(caps[i]);
    if (s_pngData) {
      s_pngCap = caps[i];
      Serial.printf("[wx] png buffer %d bytes, free heap %u\n", s_pngCap,
                    (unsigned)ESP.getFreeHeap());
      return;
    }
  }
  s_pngCap = 0;
  Serial.printf("[wx] png buffer alloc FAILED, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

// plain HTTP on purpose: no TLS state, ~45 KB heap saved
static bool httpGetToBuffer(const char *url, uint8_t *buf, int cap, int *got) {
  *got = 0;
  WiFiClient c;
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(15000);
  if (!http.begin(c, String(url))) return false;
  http.addHeader("User-Agent", "PlaneRadar3in1/1.0");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[wx] tile http %d\n", code);
    http.end();
    return false;
  }
  WiFiClient *s = http.getStreamPtr();
  int total = http.getSize();
  int n = 0;
  unsigned long last = millis();
  while (http.connected()) {
    int avail = s->available();
    if (avail > 0) {
      int want = cap - n;
      if (want <= 0) break;
      if (avail > want) avail = want;
      int rd = s->readBytes(buf + n, avail);
      if (rd <= 0) break;
      n += rd;
      last = millis();
      if (total > 0 && n >= total) break;
    } else {
      if (millis() - last > 10000) break;
      delay(1);
    }
  }
  http.end();
  *got = n;
  return n > 0;
}

static bool fetchWeather() {
  s_wxOk = false;

  IPAddress ip;
  Serial.print("[wx] dns api.rainviewer.com ... "); Serial.flush();
  if (!WiFi.hostByName("api.rainviewer.com", ip)) {
    s_wxMsg = "DNS";
    Serial.println("FAIL"); Serial.flush();
    return false;
  }
  Serial.println(ip.toString().c_str()); Serial.flush();

  WiFiClient c;
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(12000);
  if (!http.begin(c, String(WX_META_URL))) { s_wxMsg = "BEGIN"; return false; }
  Serial.println("[wx] meta GET"); Serial.flush();
  int code = http.GET();
  Serial.printf("[wx] meta http %d\n", code); Serial.flush();
  if (code != HTTP_CODE_OK) {
    http.end();
    s_wxMsg = "META" + String(code);
    return false;
  }
  String body = http.getString();
  http.end();
  Serial.printf("[wx] meta body %d bytes\n", (int)body.length()); Serial.flush();

  JsonDocument doc;
  if (deserializeJson(doc, body)) { s_wxMsg = "JSON"; return false; }

  JsonArray past = doc["radar"]["past"].as<JsonArray>();
  if (past.size() == 0) { s_wxMsg = "NOFRAME"; return false; }
  JsonObject fr = past[past.size() - 1];
  String path = fr["path"] | "";
  String ts = fr["time"] | "";
  if (path.length() == 0) { s_wxMsg = "NOPATH"; return false; }

  int n = 1 << WX_ZOOM;
  double latr = s_lat * 0.0174532925;
  double xf = (s_lon + 180.0) / 360.0 * n * 256.0;
  double yf = (1.0 - log(tan(latr) + 1.0 / cos(latr)) / PI) / 2.0 * n * 256.0;
  int tx = (int)floor(xf / 256.0);
  int ty = (int)floor(yf / 256.0);
  s_tileDx = R_CX - (int)(xf - tx * 256.0);
  s_tileDy = R_CY - (int)(yf - ty * 256.0);

  char url[192];
  snprintf(url, sizeof(url), "%s%s/256/%d/%d/%d/2/1_1.png", WX_TILE_HOST, path.c_str(),
           WX_ZOOM, tx, ty);
  Serial.println(url); Serial.flush();

  if (!s_pngData) { s_wxMsg = "NOMEM"; return false; }
  int got = 0;
  if (!httpGetToBuffer(url, s_pngData, s_pngCap, &got)) {
    s_wxMsg = "TILE";
    Serial.printf("[wx] tile download failed, cap %d\n", s_pngCap);
    return false;
  }
  if (got >= s_pngCap) {
    s_wxMsg = "TRUNC";
    Serial.printf("[wx] tile %d bytes >= cap %d\n", got, s_pngCap);
    return false;
  }
  Serial.printf("[wx] tile %d bytes z%d %d/%d\n", got, WX_ZOOM, tx, ty);

  Serial.printf("[wx] before openRAM got=%d cap=%d heap=%u\n", got, s_pngCap,
                (unsigned)ESP.getFreeHeap());
  Serial.flush();
  int rc = s_png.openRAM(s_pngData, got, (PNG_DRAW_CALLBACK *)pngRow);
  Serial.printf("[wx] openRAM rc=%d w=%d h=%d type=%d bpp=%d pitch=%d\n", rc, s_png.getWidth(),
                s_png.getHeight(), s_png.getPixelType(), s_png.getBpp(), s_png.getBufferSize());
  Serial.flush();
  if (rc != PNG_SUCCESS) {
    s_wxMsg = "OPEN" + String(rc);
    Serial.printf("[wx] png open rc=%d\n", rc);
    return false;
  }
  Serial.println("[wx] before decode"); Serial.flush();
  Serial.printf("[wx] loopTask stack min before decode=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
  Serial.flush();
  rc = s_png.decode(NULL, 0);
  Serial.printf("[wx] after decode rc=%d\n", rc); Serial.flush();
  s_png.close();
  Serial.println("[wx] png closed"); Serial.flush();
  if (rc != PNG_SUCCESS) {
    s_wxMsg = "DEC" + String(rc);
    Serial.printf("[wx] png decode rc=%d\n", rc);
    return false;
  }

  s_wxOk = true;
  s_wxMsg = ts;
  return true;
}

static void drawRainSkeleton() {
  gfx->fillScreen(C_BLACK);
  gfx->drawCircle(R_CX, R_CY, R_R, 0x0320);
  gfx->drawCircle(R_CX, R_CY, R_R / 2, 0x0200);
  gfx->fillCircle(R_CX, R_CY, 3, C_WHITE);
  textC("RAIN RADAR", 8, C_CYAN, 1);
}

static void drawRainFrame() {
  gfx->drawCircle(R_CX, R_CY, R_R, 0x0320);
  gfx->drawCircle(R_CX, R_CY, R_R / 2, 0x0200);
  gfx->fillCircle(R_CX, R_CY, 3, C_WHITE);
  gfx->fillRect(0, 0, LCD_W, 34, C_BLACK);
  textC("RAIN RADAR", 8, C_CYAN, 1);
  char b[26];
  snprintf(b, sizeof(b), "Z%d %s", WX_ZOOM, s_wxMsg.c_str());
  textC(b, 22, C_GRAY, 1);
}

static void drawRainFail() {
  drawRainSkeleton();
  char b[26];
  snprintf(b, sizeof(b), "FAIL %s", s_wxMsg.c_str());
  textC(b, R_CY + 8, C_RED, 1);
  textC("KEY=NEXT", R_CY + 30, C_GRAY, 1);
}

static void enterWeather() {
  s_wxNeed = true;              // loop() performs the download, UI is never blocked
  drawRainSkeleton();
  textC("LOADING...", R_CY + 8, C_GRAY, 1);
}

// ------------------------------------------------------------------ network --
static void saveCfg() {
  if (!prefs.begin("pr3", false)) {
    Serial.println("[cfg] nvs open for write failed");
    return;
  }
  prefs.putString("ssid", s_ssid);
  prefs.putString("pass", s_pass);
  prefs.putFloat("lat", s_lat);
  prefs.putFloat("lon", s_lon);
  prefs.putInt("ver", CFG_VER);
  prefs.end();
}

static void loadCfg() {
  bool ok = prefs.begin("pr3", true);
  if (!ok) {                       // NVS empty on a fresh chip: seed it with defaults
    prefs.end();
    s_ssid = DEFAULT_SSID;
    s_pass = DEFAULT_PASS;
    s_lat = DEFAULT_LAT;
    s_lon = DEFAULT_LON;
    saveCfg();                     // write keys now, later reads never log NOT_FOUND
    Serial.printf("[cfg] nvs empty, seeded %s at %.4f/%.4f\n", DEFAULT_SSID, DEFAULT_LAT,
                  DEFAULT_LON);
    return;
  }
  s_ssid = prefs.getString("ssid", DEFAULT_SSID);
  s_pass = prefs.getString("pass", DEFAULT_PASS);
  int ver = prefs.getInt("ver", 0);
  if (ver < CFG_VER) {             // older layout: force the new bench-test centre
    s_lat = DEFAULT_LAT;
    s_lon = DEFAULT_LON;
    Serial.println("[cfg] cfg version bumped, centre reset to Baiyun");
  } else {
    s_lat = prefs.getFloat("lat", DEFAULT_LAT);
    s_lon = prefs.getFloat("lon", DEFAULT_LON);
  }
  prefs.end();
  saveCfg();
  Serial.printf("[cfg] nvs ssid='%s' lat=%.4f lon=%.4f ver=%d\n", s_ssid.c_str(), s_lat, s_lon,
                CFG_VER);
}

static void handleRoot() {
  String html =
      "<html><head><meta charset='utf-8'><meta name='viewport' "
      "content='width=device-width,initial-scale=1'></head><body "
      "style='font-family:sans-serif;background:#111;color:#eee;padding:18px'>"
      "<h3>PlaneRadar 3in1 SETUP</h3><form method='POST' action='/save'>"
      "WIFI SSID (2.4G only)<br><input name='ssid' style='width:100%;padding:8px'><br><br>"
      "WIFI PASSWORD<br><input name='pass' type='password' style='width:100%;padding:8px'><br><br>"
      "LATITUDE<br><input name='lat' style='width:100%;padding:8px'><br><br>"
      "LONGITUDE<br><input name='lon' style='width:100%;padding:8px'><br><br>"
      "<button style='padding:12px 28px;font-size:16px'>SAVE + REBOOT</button></form>"
      "<p style='color:#888'>default 23.3924 / 113.2988 (Baiyun)</p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
  if (server.hasArg("ssid")) s_ssid = server.arg("ssid");
  if (server.hasArg("pass")) s_pass = server.arg("pass");
  if (server.hasArg("lat"))  s_lat = server.arg("lat").toFloat();
  if (server.hasArg("lon")) s_lon = server.arg("lon").toFloat();
  saveCfg();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'><body style='background:#111;color:#0f0;"
              "font-family:sans-serif;padding:24px'>SAVED, REBOOTING...</body>");
  delay(1500);
  ESP.restart();
}

static void startPortal() {
  s_portal = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.printf("[wifi] portal AP=%s ip=%s\n", AP_NAME, WiFi.softAPIP().toString().c_str());

  gfx->fillScreen(C_BLACK);
  textC("WIFI SETUP", 60, C_CYAN, 2);
  textC(AP_NAME, 96, C_WHITE, 1);
  textC("192.168.4.1", 116, C_GREEN, 1);
  textC("open browser to config", 140, C_GRAY, 1);
}

static bool connectWifi() {
  if (s_ssid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  int found = WiFi.scanNetworks();
  Serial.printf("[wifi] scan found %d networks\n", found);
  int hits = 0;
  for (int i = 0; i < found; i++) {
    Serial.printf("[wifi]   %-20s rssi=%d ch=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.channel(i));
    if (WiFi.SSID(i) == s_ssid) hits++;
  }
  WiFi.scanDelete();
  if (hits == 0) Serial.printf("[wifi] target '%s' NOT in scan (2.4G band off or hidden)\n",
                               s_ssid.c_str());

  for (int attempt = 1; attempt <= 2; attempt++) {
    Serial.printf("[wifi] connect attempt %d to '%s'\n", attempt, s_ssid.c_str());
    Serial.flush();
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      return true;
    }
    Serial.printf("[wifi] attempt %d failed, status=%d\n", attempt, (int)WiFi.status());
    Serial.flush();
    WiFi.disconnect(true);
    delay(1000);
  }
  return false;
}

// --------------------------------------------------------------- adsb fetch --
#define FETCH_NM 250.0f      // one wide query (~460 km); the dial crops it per range

static void addPlane(const Plane &q) {
  if (s_plN < MAX_PLANES) {
    s_pl[s_plN++] = q;
  } else {                            // keep the nearest MAX_PLANES only
    int worst = 0;
    for (int i = 1; i < s_plN; i++) if (s_pl[i].dist > s_pl[worst].dist) worst = i;
    if (q.dist < s_pl[worst].dist) s_pl[worst] = q;
  }
}

static void sortPlanes() {
  for (int i = 1; i < s_plN; i++) {
    Plane t = s_pl[i];
    int j = i - 1;
    while (j >= 0 && s_pl[j].dist > t.dist) { s_pl[j + 1] = s_pl[j]; j--; }
    s_pl[j + 1] = t;
  }
}

static void fetchPlanes() {
  char url[128];
  snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v3/lat/%.5f/lon/%.5f/dist/%.1f",
           s_lat, s_lon, FETCH_NM);

  WiFiClientSecure c;
  c.setInsecure();
  c.setHandshakeTimeout(TLS_HANDSHAKE_S);
  HTTPClient http;
  http.setConnectTimeout(TLS_HANDSHAKE_S * 1000);
  http.setTimeout(12000);
  if (!http.begin(c, String(url))) { s_adsbMsg = "BEGIN"; return; }
  http.addHeader("User-Agent", "PlaneRadar3in1/1.0");
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  Serial.printf("[adsb] http %d\n", code); Serial.flush();
  if (code != HTTP_CODE_OK) {
    http.end();
    s_adsbMsg = "HTTP" + String(code);
    return;
  }

  JsonDocument filter;
  filter["ac"][0]["lat"] = true;
  filter["ac"][0]["lon"] = true;
  filter["ac"][0]["flight"] = true;
  filter["ac"][0]["alt_baro"] = true;
  filter["ac"][0]["gs"] = true;
  filter["ac"][0]["track"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    s_adsbMsg = String(err.c_str());
    Serial.printf("[adsb] json err %s\n", err.c_str());
    return;
  }

  s_plN = 0;
  JsonArray ac = doc["ac"].as<JsonArray>();
  Serial.printf("[adsb] api returned %d entries\n", (int)ac.size());
  for (JsonObject p : ac) {
    if (p["alt_baro"].is<const char *>()) continue;          // "ground"
    float la = p["lat"] | 0.0f;
    float lo = p["lon"] | 0.0f;
    if (la == 0.0f && lo == 0.0f) continue;                  // no position

    Plane q;
    memset(&q, 0, sizeof(q));
    q.lat = la;
    q.lon = lo;
    q.altFt = p["alt_baro"] | 0;
    q.gsKt = p["gs"] | 0;
    if (p["track"].is<float>() || p["track"].is<int>()) {
      q.track = p["track"] | 0.0f;
      q.hasTrack = true;
    } else {
      q.track = 0;
      q.hasTrack = false;
    }
    const char *cs = p["flight"] | "";
    strncpy(q.cs, cs, sizeof(q.cs) - 1);
    q.cs[sizeof(q.cs) - 1] = 0;
    for (int i = (int)strlen(q.cs) - 1; i >= 0 && q.cs[i] == ' '; i--) q.cs[i] = 0;

    geoToPolar(q.lat, q.lon, &q.dist, &q.brg);
    addPlane(q);
  }
  sortPlanes();
  s_adsbOk = true;
  s_adsbMsg = "ok";
  Serial.printf("[adsb] kept %d aircraft (nearest %.1f km), heap %u\n", s_plN,
                s_plN ? s_pl[0].dist : -1.0f, (unsigned)ESP.getFreeHeap());
}

static unsigned long planePeriod() {
  float r = RANGES_KM[s_range];
  if (r <= 25) return 8000;
  if (r <= 50) return 10000;
  if (r <= 100) return 12000;
  return 15000;
}

// ==================================================================== boot ===
static bool initDisplay() {
  Serial.println("[lcd] step1: create SPI bus"); Serial.flush();
  s_bus = new Arduino_ESP32SPI(PIN_DC, PIN_CS, PIN_SCLK, PIN_MOSI);
  Serial.println("[lcd] step2: create GC9A01 panel"); Serial.flush();
  Arduino_GC9A01 *panel = new Arduino_GC9A01(s_bus, PIN_RST, 0, false);
  gfx = panel;
  Serial.println("[lcd] step3: gfx->begin()"); Serial.flush();
  bool ok = gfx->begin(SPI_HZ);
  Serial.println("[lcd] step4: begin returned"); Serial.flush();
  gfx->fillScreen(C_BLACK);
  Serial.println("[lcd] step5: screen cleared"); Serial.flush();
  return ok;
}

static void enterPlanes() {
  renderPlanes();
  s_nextFetch = 0;
}

// High-priority diagnostic task: even if loopTask is stuck in a tight spin,
// this preempts it and prints, proving spin vs fault/deadlock.
static void diagTask(void *) {
  extern volatile uint32_t g_pngInner, g_pngOuter, g_pngInflateCalls;
  unsigned cnt = 0;
  for (;;) {
    Serial.printf("[diag] n=%u heap=%u diagStackMin=%u pngIn=%lu pngOut=%lu infCalls=%lu\n", cnt++,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(NULL),
                  (unsigned long)g_pngInner, (unsigned long)g_pngOuter,
                  (unsigned long)g_pngInflateCalls);
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== " FW_NAME " " FW_VER " ===");
  Serial.printf("[boot] pins MOSI=%d SCLK=%d CS=%d DC=%d RST=%d KEY=%d\n",
                PIN_MOSI, PIN_SCLK, PIN_CS, PIN_DC, PIN_RST, PIN_BTN);
  Serial.flush();

  pinMode(PIN_BTN, INPUT_PULLUP);

  xTaskCreate(diagTask, "diag", 2560, nullptr, 3, nullptr);

  Serial.println("[boot] load config"); Serial.flush();
  loadCfg();
  Serial.println("[boot] config loaded, display next"); Serial.flush();
  bool dispOk = initDisplay();
  Serial.printf("[lcd] GC9A01 init %s, heap %u\n", dispOk ? "OK" : "FAIL",
                (unsigned)ESP.getFreeHeap());
  Serial.flush();

  textC(FW_NAME, 100, C_CYAN, 1);
  textC(FW_VER, 116, C_GRAY, 1);
  textC("BOOTING", 134, C_GREEN, 1);

  allocPngBuffer();
  Serial.flush();

  Serial.println("[wifi] trying to join saved network"); Serial.flush();
  if (!connectWifi()) {
    Serial.println("[wifi] join failed, starting setup AP"); Serial.flush();
    startPortal();
  } else {
    Serial.printf("[wifi] connected %s ip=%s rssi=%d\n", s_ssid.c_str(),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTzTime("CST-8", NTP1, NTP2);
  }

  gfx->fillScreen(C_BLACK);
  s_screen = SCR_PLANES;
  enterPlanes();
#ifdef PR_SELFTEST
  unsigned long t;
  t = millis(); renderClock();
  Serial.printf("[self] clock page %lu ms\n", millis() - t); Serial.flush();
  delay(2000);
  drawRainSkeleton();
  textC("LOADING...", R_CY + 8, C_GRAY, 1);
  t = millis(); bool wok = fetchWeather();
  Serial.printf("[self] rain page %lu ms ok=%d msg=%s\n", millis() - t, (int)wok,
                s_wxMsg.c_str()); Serial.flush();
  if (wok) drawRainFrame(); else drawRainFail();
  delay(2500);
  t = millis(); s_screen = SCR_PLANES; enterPlanes();
  Serial.printf("[self] plane page %lu ms\n", millis() - t); Serial.flush();
  delay(2000);
#endif
  Serial.println("[boot] setup done"); Serial.flush();
}

static void handleButton() {
  static bool last = HIGH;
  static unsigned long tDown = 0;
  static bool longFired = false;
  bool now = digitalRead(PIN_BTN);
  unsigned long ms = millis();

  if (last == HIGH && now == LOW) {          // press
    tDown = ms;
    longFired = false;
  } else if (last == LOW && now == HIGH) {   // release before the hold threshold
    if (!longFired && ms - tDown >= 30) {    // => next screen (no upper limit!)
      s_screen = (s_screen + 1) % 3;
      Serial.printf("[ui] press %lu ms -> screen=%d\n", ms - tDown, s_screen);
      if (s_screen == SCR_PLANES) enterPlanes();
      else if (s_screen == SCR_WEATHER) enterWeather();
      else renderClock();
    }
  }
  if (now == LOW && !longFired && ms - tDown >= 900) {   // hold: next range, once
    longFired = true;
    s_range = (s_range + 1) % RANGE_N;
    Serial.printf("[ui] hold -> range=%dkm\n", (int)RANGES_KM[s_range]);
    s_screen = SCR_PLANES;
    enterPlanes();
  }
  last = now;
}

void loop() {
  static unsigned long lastHb = 0;
  if (millis() - lastHb > 10000UL) {
    lastHb = millis();
    Serial.printf("[hb] up=%lus screen=%d range=%dkm planes=%d wx=%s heap=%u wifi=%d\n",
                  millis() / 1000UL, s_screen, (int)RANGES_KM[s_range], s_plN,
                  s_wxMsg.c_str(), (unsigned)ESP.getFreeHeap(), (int)WiFi.status());
    Serial.flush();
  }

  if (s_portal) {
    server.handleClient();
    delay(2);
    return;
  }

  // one-shot rain check, 20 s after boot, never in the setup path
  if (!s_wxProbeDone && millis() > 20000UL) {
    s_wxProbeDone = true;
    Serial.println("[wxprobe] checking rainviewer tile"); Serial.flush();
    s_wxDry = true;
    bool wxok = fetchWeather();
    s_wxDry = false;
    Serial.printf("[wxprobe] %s msg=%s\n", wxok ? "OK" : "FAIL", s_wxMsg.c_str());
    Serial.flush();
  }

  handleButton();

  unsigned long now = millis();

  if (s_screen == SCR_PLANES) {
    if (now >= s_nextFetch) {
      fetchPlanes();
      s_nextFetch = now + planePeriod();
      renderPlanes();                       // one full repaint per data update
    } else {
      if (s_beamOn) drawBeam(s_sweep, C_BLACK);
      s_sweep += 4.0f;
      if (s_sweep >= 360.0f) s_sweep -= 360.0f;
      drawPlaneIcons();
      gfx->fillCircle(R_CX, R_CY, 2, C_GREEN);
      drawBeam(s_sweep, C_GREEN);
      s_beamOn = true;
    }
    delay(70);
  } else if (s_screen == SCR_WEATHER) {
    if (s_wxNeed) {
      s_wxNeed = false;
      bool ok = fetchWeather();            // skeleton was drawn by enterWeather()
      if (ok) drawRainFrame(); else drawRainFail();
      s_wxStamp = millis();
    } else if (millis() - s_wxStamp > 300000UL) {
      enterWeather();                      // refresh every 5 min
    }
    delay(200);
  } else {
    clockTick();
    delay(500);
  }
}
