#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WebServer.h>
#include <time.h>
#include <PNGdec.h>

// ---------------- pins (locked, do not change) ----------------
#ifndef PR_RST
#define PR_RST 0
#endif
#ifndef PR_CS
#define PR_CS 1
#endif
#ifndef PR_DC
#define PR_DC 10
#endif
#ifndef PR_MOSI
#define PR_MOSI 3
#endif
#ifndef PR_SCLK
#define PR_SCLK 4
#endif
#ifndef PR_KEY
#define PR_KEY 9
#endif
#ifndef PR_SPI_FREQ
#define PR_SPI_FREQ 40000000
#endif

// ---------------- geometry ----------------
#define SCR_W 240
#define SCR_H 240
#define R_CX 120
#define R_CY 120
#define R_R 112

// ---------------- colors ----------------
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GREEN 0x07E0
#define C_DKGREEN 0x0320
#define C_DKRED 0x3000
#define C_RED 0xF800
#define C_YELLOW 0xFFE0
#define C_CYAN 0x07FF
#define C_GRAY 0x8410
#define C_ORANGE 0xFD20
#define C_BLUE 0x041F
#define C_MAGENTA 0xF81F
#define C_DKBLUE 0x0210

// ---------------- version ----------------
#define FW_VER "1.2.0-c3"
#define CFG_VER 105
#define WX_ZOOM 7
#define FETCH_NM 250.0
#define MAX_PLANES 40
#define MAX_LABELS 6
#define PNG_BUF_CAP 73728

// ---------------- screens ----------------
enum { SCR_CLOCK = 0, SCR_PLANES = 1, SCR_FORECAST = 2, SCR_RAIN = 3 };

// ---------------- ranges (6 steps) ----------------
static const float RANGES_KM[6] = {5, 10, 25, 50, 100, 200};
static int s_rangeIdx = 1; // default 10km
static float rangeKm() { return RANGES_KM[s_rangeIdx]; }
static int s_screen = SCR_CLOCK;

// ---------------- display ----------------
static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX *gfx = nullptr;
static PNG s_png;

// ---------------- config ----------------
static Preferences s_prefs;
static String s_ssid;
static String s_pass;
static String s_city = "Guangzhou";
static float s_lat = 23.3924f;
static float s_lon = 113.2988f;
static bool s_cfgValid = false;
static WebServer *s_srv = nullptr;

// ---------------- time ----------------
static struct tm s_tm;
static bool s_timeOk = false;
static int s_lastSec = -1;

// ---------------- planes ----------------
struct Plane {
  char call[12];
  char typ[8];
  double lat;
  double lon;
  int alt;
  int head;
  float distNm;
  float distM;
  float bear;
};
static Plane s_planes[MAX_PLANES];
static int s_planeCount = 0;
static unsigned long s_planeMs = 0;
static float s_sweep = 0;
static unsigned long s_sweepMs = 0;

// ---------------- weather ----------------
struct DayWx {
  int code;
  float tmax;
  float tmin;
  int pop;
};
static float s_temp = 0, s_hum = 0, s_wind = 0;
static int s_code = 0;
static DayWx s_days[3];
static unsigned long s_metMs = 0;
static unsigned long s_metTryMs = 0;
static bool s_metOk = false;

// ---------------- rain / png ----------------
static uint8_t *s_pngData = nullptr;
static size_t s_pngLen = 0;
static int s_pngMode = 0; // 0 = opaque base, 1 = rain overlay
static int s_wxRows = 0;
static int s_rainPx = 0;
static unsigned long s_rainMs = 0;
static int s_tileX = 0, s_tileY = 0;
static int s_tileDx = 0, s_tileDy = 0;
static String s_rainPath = "";
static bool s_rainOk = false;
static unsigned long s_rainTryMs = 0;

// ---------------- forward ----------------
static void renderClock();
static void drawPlanes(float sweep);
static void renderForecast();
static bool rainRefresh();
static void enterPage(int page);
static void handleButton();
static bool fetchOpenMeteo();
static void drawWxIcon(int cx, int cy, int code, int s);
static const char *wmoZh(int code);

// ---------------- text helpers ----------------
static void useFont(const uint8_t *f) {
  gfx->setFont(f);
  gfx->setTextSize(1);
}
static int textW(const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}
static void txtAt(const char *s, int x, int y, uint16_t c) {
  gfx->setTextColor(c);
  gfx->setCursor(x, y);
  gfx->print(s);
}
static void txtC(const char *s, int y, uint16_t c) {
  int w = textW(s);
  gfx->setTextColor(c);
  gfx->setCursor(R_CX - w / 2, y);
  gfx->print(s);
}
static void txtC(const String &s, int y, uint16_t c) { txtC(s.c_str(), y, c); }

// ---------------- geometry ----------------
static double deg2rad(double d) { return d * PI / 180.0; }
static double rad2deg(double r) { return r * 180.0 / PI; }

static float haversineKm(double lat1, double lon1, double lat2, double lon2) {
  double R = 6371.0;
  double dLat = deg2rad(lat2 - lat1);
  double dLon = deg2rad(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(deg2rad(lat1)) * cos(deg2rad(lat2)) *
             sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return (float)(R * c);
}

static float bearingTo(double lat1, double lon1, double lat2, double lon2) {
  double dLon = deg2rad(lon2 - lon1);
  double y = sin(dLon) * cos(deg2rad(lat2));
  double x = cos(deg2rad(lat1)) * sin(deg2rad(lat2)) -
             sin(deg2rad(lat1)) * cos(deg2rad(lat2)) * cos(dLon);
  float b = (float)rad2deg(atan2(y, x));
  if (b < 0) b += 360;
  return b;
}

// Spherical-mercator tile math
static void lonLatToTile(double lat, double lon, int z, int &tx, int &ty,
                         double &px, double &py) {
  double n = pow(2.0, z);
  double sf = n * 256.0;
  double mx = (lon + 180.0) / 360.0 * sf;
  double siny = sin(deg2rad(lat));
  if (siny > 0.9999) siny = 0.9999;
  if (siny < -0.9999) siny = -0.9999;
  double my = (0.5 - log((1 + siny) / (1 - siny)) / (4 * PI)) * sf;
  tx = (int)floor(mx / 256.0);
  ty = (int)floor(my / 256.0);
  px = mx - tx * 256.0;
  py = my - ty * 256.0;
}

// ---------------- config portal ----------------
static void saveCfg() {
  s_prefs.putInt("ver", CFG_VER);
  s_prefs.putString("ssid", s_ssid);
  s_prefs.putString("pass", s_pass);
  s_prefs.putFloat("lat", s_lat);
  s_prefs.putFloat("lon", s_lon);
  s_prefs.putString("city", s_city);
}

static void handleRoot() {
  String html = "<!doctype html><html><head><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>PlaneRadar Setup</title></head><body>"
                "<h2>PlaneRadar WiFi</h2><form method=POST action=/save>"
                "SSID:<br><input name=ssid length=32><br>"
                "Password:<br><input name=pass length=32><br><br>"
                "<button type=submit>Save</button></form></body></html>";
  s_srv->send(200, "text/html", html);
}
static void handleSave() {
  if (s_srv->hasArg("ssid")) s_ssid = s_srv->arg("ssid");
  if (s_srv->hasArg("pass")) s_pass = s_srv->arg("pass");
  saveCfg();
  s_srv->send(200, "text/plain", "Saved, restarting...");
  delay(800);
  ESP.restart();
}

static void startPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PlaneRadar-Setup");
  s_srv = new WebServer(80);
  s_srv->on("/", handleRoot);
  s_srv->on("/save", handleSave);
  s_srv->begin();
  Serial.println("AP PlaneRadar-Setup up, http://192.168.4.1");
  useFont(u8g2_font_helvB08_tr);
  gfx->fillScreen(C_BLACK);
  txtC("SETUP WIFI", 100, C_YELLOW);
  txtC("PlaneRadar-Setup", 120, C_WHITE);
  txtC("192.168.4.1", 140, C_CYAN);
  while (true) {
    s_srv->handleClient();
    delay(10);
  }
}

// ---------------- IP locate ----------------
static void locateByIp() {
  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=status,message,city,lat,lon");
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, body) == 0) {
      const char *st = doc["status"];
      if (st && strcmp(st, "success") == 0) {
        float la = doc["lat"] | s_lat;
        float lo = doc["lon"] | s_lon;
        const char *ci = doc["city"];
        s_lat = la;
        s_lon = lo;
        if (ci) s_city = String(ci);
        saveCfg();
        Serial.printf("[loc] %s %.4f %.4f\n", s_city.c_str(), s_lat, s_lon);
      } else {
        Serial.println("[loc] status fail, keep center");
      }
    }
  } else {
    Serial.printf("[loc] http %d, keep center\n", code);
  }
  http.end();
}

// ---------------- NTP ----------------
static void syncTime() {
  configTzTime("CST-8", "ntp.aliyun.com", "cn.pool.ntp.org");
  int tries = 0;
  while (!s_timeOk && tries < 40) {
    if (getLocalTime(&s_tm, 500)) s_timeOk = true;
    tries++;
  }
  if (s_timeOk) Serial.printf("[ntp] ok %04d-%02d-%02d\n",
                              s_tm.tm_year + 1900, s_tm.tm_mon + 1, s_tm.tm_mday);
}

// ---------------- Open-Meteo ----------------
static bool fetchOpenMeteo() {
  char url[420];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max&timezone=auto&forecast_days=4",
           s_lat, s_lon);
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (deserializeJson(doc, body) == 0) {
      JsonObject cur = doc["current"];
      if (!cur.isNull()) {
        s_temp = cur["temperature_2m"] | s_temp;
        s_hum = cur["relative_humidity_2m"] | s_hum;
        s_wind = cur["wind_speed_10m"] | s_wind;
        s_code = cur["weather_code"] | s_code;
      }
      JsonArray dcode = doc["daily"]["weather_code"];
      JsonArray dmax = doc["daily"]["temperature_2m_max"];
      JsonArray dmin = doc["daily"]["temperature_2m_min"];
      JsonArray dpop = doc["daily"]["precipitation_probability_max"];
      for (int i = 0; i < 3; i++) {
        int di = i + 1;
        if (dcode.size() > di) {
          s_days[i].code = dcode[di] | 0;
          s_days[i].tmax = dmax[di] | 0;
          s_days[i].tmin = dmin[di] | 0;
          s_days[i].pop = dpop[di] | 0;
        }
      }
      s_metMs = millis();
      s_metOk = true;
      ok = true;
      Serial.printf("[om] %.1fC hum %.0f wind %.1f code %d\n",
                    s_temp, s_hum, s_wind, s_code);
    }
  } else {
    Serial.printf("[om] http %d\n", code);
  }
  http.end();
  return ok;
}

// ---------------- weather labels ----------------
static const char *wmoZh(int code) {
  if (code == 0) return "晴";
  if (code == 1 || code == 2) return "多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code >= 51 && code <= 55) return "毛毛雨";
  if (code == 56 || code == 57 || code == 66 || code == 67) return "冻雨";
  if (code == 61) return "小雨";
  if (code == 63) return "中雨";
  if (code == 65) return "大雨";
  if (code >= 71 && code <= 77) return "雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code >= 95) return "雷阵雨";
  return "多云";
}

enum { IC_SUN, IC_PART, IC_CLOUD, IC_FOG, IC_DRIZZLE, IC_RAIN, IC_SNOW, IC_THUNDER };
static int iconKind(int code) {
  if (code == 0) return IC_SUN;
  if (code == 1 || code == 2) return IC_PART;
  if (code == 3) return IC_CLOUD;
  if (code == 45 || code == 48) return IC_FOG;
  if (code >= 51 && code <= 57) return IC_DRIZZLE;
  if (code >= 95) return IC_THUNDER;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return IC_SNOW;
  if (code >= 61) return IC_RAIN;
  return IC_PART;
}

static void fillCloud(int cx, int cy, int s, uint16_t col) {
  gfx->fillCircle(cx - s, cy + s / 2, s, col);
  gfx->fillCircle(cx, cy, s + 1, col);
  gfx->fillCircle(cx + s, cy + s / 3, s - 1, col);
  gfx->fillRect(cx - s, cy + s / 2, 2 * s, s, col);
}

static void drawWxIcon(int cx, int cy, int code, int s) {
  int kind = iconKind(code);
  if (kind == IC_SUN || kind == IC_PART) {
    gfx->fillCircle(cx, cy, s, C_YELLOW);
    for (int a = 0; a < 360; a += 45) {
      float r = a * PI / 180;
      int x1 = cx + (int)(cos(r) * (s + 2));
      int y1 = cy + (int)(sin(r) * (s + 2));
      int x2 = cx + (int)(cos(r) * (s + 6));
      int y2 = cy + (int)(sin(r) * (s + 6));
      gfx->drawLine(x1, y1, x2, y2, C_YELLOW);
    }
  }
  if (kind == IC_PART) {
    gfx->fillCircle(cx + s, cy + s / 2, s, C_BLACK);
    fillCloud(cx + s / 2, cy + s, s - 1, C_WHITE);
  } else if (kind == IC_CLOUD) {
    fillCloud(cx, cy, s, C_WHITE);
  } else if (kind == IC_FOG) {
    fillCloud(cx, cy - s / 2, s - 1, C_GRAY);
    for (int i = 0; i < 3; i++)
      gfx->drawLine(cx - s, cy + s + i * 4, cx + s, cy + s + i * 4, C_GRAY);
  } else if (kind == IC_DRIZZLE || kind == IC_RAIN) {
    fillCloud(cx, cy - s / 2, s, C_WHITE);
    int n = (kind == IC_RAIN) ? 3 : 2;
    for (int i = 0; i < n; i++) {
      int x = cx - s + i * (2 * s / (n > 1 ? n - 1 : 1));
      gfx->drawLine(x, cy + s, x - 2, cy + s + 6, C_BLUE);
    }
  } else if (kind == IC_SNOW) {
    fillCloud(cx, cy - s / 2, s, C_WHITE);
    for (int i = 0; i < 3; i++) {
      int x = cx - s + i * s;
      gfx->fillCircle(x, cy + s + 4, 2, C_WHITE);
    }
  } else if (kind == IC_THUNDER) {
    fillCloud(cx, cy - s / 2, s, C_GRAY);
    gfx->fillTriangle(cx, cy + s, cx + 6, cy + s, cx - 2, cy + s + 10, C_YELLOW);
    gfx->fillTriangle(cx - 2, cy + s + 10, cx + 4, cy + s + 10, cx, cy + s + 16, C_YELLOW);
  }
}

// ---------------- planes ----------------
static float planeDistKm(int i) {
  return haversineKm(s_lat, s_lon, s_planes[i].lat, s_planes[i].lon);
}
static int nearest() {
  int best = -1;
  float bd = 1e9;
  for (int i = 0; i < s_planeCount; i++) {
    float d = planeDistKm(i);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

static uint16_t altColor(int alt) {
  if (alt <= 0) return C_GRAY;
  if (alt < 3000) return C_CYAN;
  if (alt < 8000) return C_GREEN;
  if (alt < 11000) return C_YELLOW;
  return C_RED;
}

static void drawPlaneMarker(int x, int y, int head, uint16_t col) {
  float a = head * PI / 180;
  float fx = sin(a), fy = -cos(a);
  float rx = cos(a), ry = sin(a);
  int px[4], py[4];
  px[0] = x + (int)(fx * 7); py[0] = y + (int)(fy * 7);
  px[1] = x + (int)(-fx * 5 + rx * 4); py[1] = y + (int)(-fy * 5 + ry * 4);
  px[2] = x + (int)(-fx * 2); py[2] = y + (int)(-fy * 2);
  px[3] = x + (int)(-fx * 5 - rx * 4); py[3] = y + (int)(-fy * 5 - ry * 4);
  gfx->fillTriangle(px[0], py[0], px[1], py[1], px[2], py[2], col);
  gfx->fillTriangle(px[2], py[2], px[3], py[3], px[0], py[0], col);
}

static bool fetchPlanes() {
  char url[160];
  snprintf(url, sizeof(url),
           "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%.1f",
           s_lat, s_lon, FETCH_NM);
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.begin(tls);
  http.setURL(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (deserializeJson(doc, body) == 0) {
      JsonArray ac = doc["ac"];
      int n = 0;
      for (JsonObject p : ac) {
        if (n >= MAX_PLANES) break;
        if (p["lat"].isNull() || p["lon"].isNull()) continue;
        Plane &pl = s_planes[n];
        const char *fl = p["flight"];
        const char *tp = p["t"];
        String cs = fl ? String(fl) : String("????");
        cs.trim();
        cs.toCharArray(pl.call, sizeof(pl.call));
        String tt = tp ? String(tp) : String("");
        tt.toCharArray(pl.typ, sizeof(pl.typ));
        pl.lat = p["lat"].as<double>();
        pl.lon = p["lon"].as<double>();
        pl.alt = p["alt_baro"] | 0;
        pl.head = p["track"] | 0;
        pl.distNm = haversineKm(s_lat, s_lon, pl.lat, pl.lon) / 1.852f;
        pl.distM = haversineKm(s_lat, s_lon, pl.lat, pl.lon) * 1000.0f;
        pl.bear = bearingTo(s_lat, s_lon, pl.lat, pl.lon);
        n++;
      }
      s_planeCount = n;
      s_planeMs = millis();
      ok = true;
      int nt = nearest();
      Serial.printf("[planes] total=%d nearest=%d %s %.1fkm\n",
                    s_planeCount, nt, nt >= 0 ? s_planes[nt].call : "-",
                    nt >= 0 ? planeDistKm(nt));
    }
  } else {
    Serial.printf("[planes] http %d\n", code);
  }
  http.end();
  return ok;
}

static void drawPlanes(float sweep) {
  gfx->fillScreen(C_BLACK);

  // thick red outer ring
  for (int i = 0; i < 4; i++)
    gfx->drawCircle(R_CX, R_CY, R_R - i, C_RED);

  // range circles
  float R = rangeKm();
  for (int f = 1; f <= 3; f++) {
    int rr = (int)(R_R * f / 3.0f);
    gfx->drawCircle(R_CX, R_CY, rr, C_DKGREEN);
  }
  // green crosshair
  gfx->drawLine(R_CX, R_CY - R_R, R_CX, R_CY + R_R, C_DKGREEN);
  gfx->drawLine(R_CX - R_R, R_CY, R_CX + R_R, C_DKGREEN);

  // range labels on right
  useFont(u8g2_font_helvB08_tr);
  char lb[16];
  for (int f = 1; f <= 3; f++) {
    int rr = (int)(R_R * f / 3.0f);
    float val = R * f / 3.0f;
    if (val >= 10) snprintf(lb, sizeof(lb), "%.0f", val);
    else snprintf(lb, sizeof(lb), "%.1f", val);
    txtAt(lb, R_CX + 2, R_CY - rr + 3, C_CYAN);
  }

  // sweep with fading trails
  static const int BACK[3] = {4, 8, 12};
  static const uint16_t TRAIL[3] = {0x0320, 0x0200, 0x0100};
  for (int i = 0; i < 3; i++) {
    float a = (sweep - BACK[i]) * PI / 180;
    int ex = R_CX + (int)(sin(a) * (R_R - 6));
    int ey = R_CY - (int)(cos(a) * (R_R - 6));
    gfx->drawLine(R_CX, R_CY, ex, ey, TRAIL[i]);
  }
  float sa = sweep * PI / 180;
  int sx = R_CX + (int)(sin(sa) * (R_R - 6));
  int sy = R_CY - (int)(cos(sa) * (R_R - 6));
  gfx->drawLine(R_CX, R_CY, sx, sy, C_GREEN);
  gfx->fillCircle(R_CX, R_CY, 3, C_WHITE);

  // map planes, pick nearest MAX_LABELS for labels
  int labelIdx[MAX_LABELS];
  int lc = 0;
  struct MP { int i; int x; int y; float d; } ;
  MP mapped[MAX_PLANES];
  int mc = 0;
  for (int i = 0; i < s_planeCount; i++) {
    float d = planeDistKm(i);
    if (d > R) continue;
    float b = s_planes[i].bear * PI / 180;
    int rr = (int)(R_R * d / R);
    int x = R_CX + (int)(sin(b) * rr);
    int y = R_CY - (int)(cos(b) * rr);
    if (mc < MAX_PLANES) { mapped[mc].i = i; mapped[mc].x = x; mapped[mc].y = y; mapped[mc].d = d; mc++; }
  }
  // sort mapped by distance for label priority
  for (int a = 0; a < mc - 1; a++)
    for (int b = a + 1; b < mc; b++)
      if (mapped[b].d < mapped[a]) { MP t = mapped[a]; mapped[a] = mapped[b]; mapped[b] = t; }
  for (int i = 0; i < mc && lc < MAX_LABELS; i++) labelIdx[lc++] = mapped[i].i;

  // markers
  for (int i = 0; i < mc; i++) {
    int pi = mapped[i].i;
    drawPlaneMarker(mapped[i].x, mapped[i].y, s_planes[pi].head,
                    altColor(s_planes[pi].alt));
  }

  // labels
  useFont(u8g2_font_helvB08_tr);
  for (int k = 0; k < lc; k++) {
    int pi = labelIdx[k];
    int x = 0, y = 0;
    for (int i = 0; i < mc; i++) if (mapped[i].i == pi) { x = mapped[i].x; y = mapped[i].y; }
    int wCall = textW(s_planes[pi].call);
    int wTyp = s_planes[pi].typ[0] ? textW(s_planes[pi].typ) : 0;
    char dm[16];
    snprintf(dm, sizeof(dm), "%.0fm", s_planes[pi].distM);
    int wDm = textW(dm);
    int lw = wCall;
    if (wTyp > lw) lw = wTyp;
    if (wDm > lw) lw = wDm;
    int lx;
    if (x > 145) lx = x - 8 - lw;
    else lx = x + 8;
    if (lx < 4) lx = 4;
    if (lx + lw > 236) lx = 236 - lw;
    int top = y - 14;
    if (top < 10) top = 10;
    if (top > 196) top = 196;
    gfx->drawLine(x, y, lx, top + 12, C_GRAY);
    txtAt(s_planes[pi].call, lx, top, C_WHITE);
    if (s_planes[pi].typ[0]) txtAt(s_planes[pi].typ, lx, top + 10, C_YELLOW);
    txtAt(dm, lx, top + 20, C_CYAN);
  }

  // header
  useFont(u8g2_font_helvB08_tr);
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "RANGE %.0fkm  N%d", R, s_planeCount);
  txtC(hdr, 16, C_GREEN);
}

// ---------------- clock ----------------
static void drawHands(int hour, int minute, int second) {
  float hh = (hour % 12) + minute / 60.0f;
  float aH = hh * 30 * PI / 180;
  float aM = minute * 6 * PI / 180;
  float aS = second * 6 * PI / 180;
  int hx = R_CX + (int)(sin(aH) * 48), hy = R_CY - (int)(cos(aH) * 48);
  int mx = R_CX + (int)(sin(aM) * 78), my = R_CY - (int)(cos(aM) * 78);
  gfx->drawLine(R_CX, R_CY, hx, hy);
  gfx->drawLine(R_CX, R_CY, mx, my);
  int sx = R_CX + (int)(sin(aS) * 86), sy = R_CY - (int)(cos(aS) * 86);
  gfx->drawLine(R_CX, R_CY, sx, sy, C_RED);
}

static void renderClock() {
  gfx->fillScreen(C_BLACK);
  gfx->drawCircle(R_CX, R_CY, R_R, C_DKGREEN);

  // ticks + 12 numbers
  useFont(u8g2_font_helvB08_tr);
  for (int i = 0; i < 60; i++) {
    float a = i * 6 * PI / 180;
    bool major = (i % 5 == 0);
    int r1 = major ? R_R - 9 : R_R - 5;
    int r2 = R_R - 2;
    int x1 = R_CX + (int)(sin(a) * r1), y1 = R_CY - (int)(cos(a) * r1);
    int x2 = R_CX + (int)(sin(a) * r2), y2 = R_CY - (int)(cos(a) * r2);
    gfx->drawLine(x1, y1, x2, y2, major ? C_WHITE : C_GRAY);
  }
  for (int n = 1; n <= 12; n++) {
    float a = n * 30 * PI / 180;
    int x = R_CX + (int)(sin(a) * 92);
    int y = R_CY - (int)(cos(a) * 92);
    char s[4];
    snprintf(s, sizeof(s), "%d", n);
    int w = textW(s);
    txtAt(s, x - w / 2, y + 3, C_WHITE);
  }

  if (s_timeOk) {
    // big digital time
    useFont(u8g2_font_logisoso24_tn);
    char dig[8];
    snprintf(dig, sizeof(dig), "%02d:%02d", s_tm.tm_hour, s_tm.tm_min);
    txtC(dig, 58, C_YELLOW);

    // date + weekday
    static const char *WD[7] = {"周日", "周一", "周二", "周三",
                                "周四", "周五", "周六"};
    useFont(u8g2_font_unifont_t_chinese4);
    char date[40];
    snprintf(date, sizeof(date), "%04d-%02d-%02d %s",
             s_tm.tm_year + 1900, s_tm.tm_mon + 1, s_tm.tm_mday, WD[s_tm.tm_wday]);
    txtC(date, 92, C_WHITE);

    // city
    useFont(u8g2_font_helvB10_tr);
    txtC(s_city, 116, C_ORANGE);

    // weather chinese
    useFont(u8g2_font_unifont_t_chinese4);
    if (s_metOk) txtC(wmoZh(s_code), 138, C_CYAN);

    // bottom row
    useFont(u8g2_font_helvB08_tr);
    if (s_metOk) {
      char b[48];
      snprintf(b, sizeof(b), "%.0fC  %s  hum%.0f%%",
               s_temp, wmoZh(s_code), s_hum);
      txtC(b, 172, C_GREEN);
    }
    drawHands(s_tm.tm_hour, s_tm.tm_min, s_tm.tm_sec);
  } else {
    useFont(u8g2_font_helvB08_tr);
    txtC("TIME SYNC...", 120, C_YELLOW);
  }
}

// ---------------- forecast ----------------
static void renderForecast() {
  gfx->fillScreen(C_BLACK);
  gfx->drawCircle(R_CX, R_CY, R_R, C_DKGREEN);

  // top: city left, icon center, temps right
  useFont(u8g2_font_helvB10_tr);
  txtAt(s_city.c_str(), 28, 34, C_YELLOW);
  if (s_metOk) {
    drawWxIcon(120, 40, s_code, 10);
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(wmoZh(s_code), 69, C_WHITE);
    useFont(u8g2_font_helvB10_tr);
    char t[24];
    snprintf(t, sizeof(t), "%.0fC", s_temp);
    txtAt(t, 178, 34, C_GREEN);
    char hi[24];
    snprintf(hi, sizeof(hi), "/%.0fC", s_days[0].tmax);
    txtAt(hi, 178, 50, C_YELLOW);
  } else {
    useFont(u8g2_font_helvB08_tr);
    txtC("LOADING...", 50, C_GRAY);
  }

  gfx->drawLine(30, 76, 210, 76, C_DKGREEN);

  // 3 columns
  static const char *DL[3] = {"明天", "后天", "大后天"};
  int colX[3] = {48, 120, 192};
  useFont(u8g2_font_unifont_t_chinese4);
  for (int i = 0; i < 3; i++) {
    int w = textW(DL[i]);
    txtAt(DL[i], colX[i] - w / 2, 88, C_CYAN);
  }
  if (s_metOk) {
    for (int i = 0; i < 3; i++) {
      drawWxIcon(colX[i], 108, s_days[i].code, 8);
      useFont(u8g2_font_helvB08_tr);
      char tt[24];
      snprintf(tt, sizeof(tt), "%.0f/%.0f", s_days[i].tmin, s_days[i].tmax);
      int w = textW(tt);
      txtAt(tt, colX[i] - w / 2, 130, C_WHITE);
      useFont(u8g2_font_unifont_t_chinese4);
      int zw = textW(wmoZh(s_days[i].code));
      txtAt(wmoZh(s_days[i].code), colX[i] - zw / 2, 148, C_GREEN);
    }
  }

  gfx->drawLine(30, 162, 210, 162, C_DKGREEN);

  // bottom row
  useFont(u8g2_font_helvB08_tr);
  if (s_metOk) {
    char b[64];
    const char *rain = (s_code >= 51 && s_code <= 99) ? wmoZh(s_code) : "无雨";
    snprintf(b, sizeof(b), "%.0fC  %s  %.0f%%", s_temp, rain, s_hum);
    txtC(b, 178, C_WHITE);
    if (s_timeOk) {
      char u[24];
      snprintf(u, sizeof(u), "UPD %02d:%02d", s_tm.tm_hour, s_tm.tm_min);
      txtC(u, 200, C_GRAY);
    }
  }
}

// ---------------- rain png ----------------
static void pngRow(PNGDRAW *pDraw) {
  s_wxRows++;
  int ty = pDraw->y + s_tileDy;
  if (ty < 0 || ty >= SCR_H) return;
  int iPixelType = pDraw->iPixelType;
  uint8_t *px = pDraw->pPixels;
  int w = pDraw->iWidth;
  if (w > 256) w = 256;
  int dy = ty - R_CY;
  int dy2 = dy * dy;
  uint16_t linebuf[256];
  bool opaque[256];
  for (int i = 0; i < 256; i++) { linebuf[i] = 0; opaque[i] = false; }

  for (int i = 0; i < w; i++) {
    int tx = i + s_tileDx;
    bool inCircle = false;
    if (tx >= 0 && tx < SCR_W) {
      int dx = tx - R_CX;
      if (dx * dx + dy2 <= R_R * R_R) inCircle = true;
    }
    uint16_t col = 0;
    bool trans = false;
    if (iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA) {
      uint8_t a = px[i * 4 + 3];
      if (a < 30) trans = true;
      else col = (uint16_t)(((px[i * 4] & 0xF8) << 8) |
                           ((px[i * 4 + 1] & 0xFC) << 3) | (px[i * 4 + 2] >> 3));
      if (s_pngMode == 1 && a >= 30 && inCircle) s_rainPx++;
    } else if (iPixelType == PNG_PIXEL_TRUECOLOR) {
      col = (uint16_t)(((px[i * 3] & 0xF8) << 8) |
                       ((px[i * 3 + 1] & 0xFC) << 3) | (px[i * 3 + 2] >> 3));
    } else if (iPixelType == PNG_PIXEL_INDEXED) {
      uint8_t idx = px[i];
      uint8_t a = pDraw->pPalette[768 + idx]; // tRNS alpha, default 255
      if (idx == 0 || a < 30) trans = true;
      else {
        uint8_t r = pDraw->pPalette[idx * 3];
        uint8_t g = pDraw->pPalette[idx * 3 + 1];
        uint8_t b = pDraw->pPalette[idx * 3 + 2];
        col = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
      if (s_pngMode == 1 && !trans && inCircle) s_rainPx++;
    } else {
      uint8_t g8 = px[i];
      col = (uint16_t)(((g8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (g8 >> 3));
    }
    linebuf[i] = col;
    if (s_pngMode == 0) opaque[i] = inCircle;            // base: ignore alpha
    else opaque[i] = inCircle && !trans;                // rain: skip transparent
  }

  if (s_pngMode == 0) {
    // full-width single burst, out-of-circle black
    gfx->draw16bitRGBBitmap(s_tileDx, ty, linebuf, 256, 1);
  } else {
    // contiguous opaque runs, never overwriting base on transparent pixels
    int i = 0;
    while (i < w) {
      if (!opaque[i]) { i++; continue; }
      int st = i;
      while (i < w && opaque[i]) i++;
      gfx->draw16bitRGBBitmap(st + s_tileDx, ty, &linebuf[st], i - st, 1);
    }
  }
}

static bool decodePng(int mode) {
  s_pngMode = mode;
  s_wxRows = 0;
  int rc = s_png.openRAM(s_pngData, (int)s_pngLen, pngRow);
  if (rc != PNG_SUCCESS) {
    Serial.printf("[png] open rc=%d\n", rc);
    return false;
  }
  int pngW = s_png.getWidth();
  int pngH = s_png.getHeight();
  rc = s_png.decode(0, 0);
  s_png.close();
  bool benign = (rc != PNG_SUCCESS && s_wxRows >= pngH);
  if (rc != PNG_SUCCESS && !benign) {
    Serial.printf("[png] decode rc=%d rows=%d/%d\n", rc, s_wxRows, pngH);
    return false;
  }
  Serial.printf("[png] mode=%d rc=%d rows=%d/%d\n", mode, rc, s_wxRows, pngH);
  return true;
}

static bool downloadToBuf(const char *url) {
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[dl] %s http=%d\n", url, code);
    http.end();
    return false;
  }
  int total = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  s_pngLen = 0;
  uint32_t start = millis();
  while (http.connected() && (total < 0 || (int)s_pngLen < total)) {
    size_t avail = stream->available();
    if (avail) {
      if (s_pngLen + avail > PNG_BUF_CAP) avail = PNG_BUF_CAP - s_pngLen;
      if (avail == 0) break;
      int got = stream->readBytes(s_pngData + s_pngLen, avail);
      s_pngLen += got;
    } else {
      delay(2);
      if (millis() - start > 12000) break;
    }
  }
  http.end();
  if ((int)s_pngLen < 64) {
    Serial.printf("[dl] too short %u\n", s_pngLen);
    return false;
  }
  return true;
}

static bool rainRefresh() {
  s_rainOk = false;
  s_rainPx = 0;
  HTTPClient http;
  http.begin("http://api.rainviewer.com/public/weather-maps.json");
  int code = http.GET();
  if (code != 200) { Serial.printf("[rain] meta http=%d\n", code); http.end(); return false; }
  String meta = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, meta) != 0) { Serial.println("[rain] meta json fail"); return false; }
  JsonArray now = doc["radar"]["nowcast"];
  if (now.isNull() || now.size() == 0) { Serial.println("[rain] no nowcast"); return false; }
  s_rainPath = (const char *)now[0]["path"];
  if (s_rainPath.length() == 0) { Serial.println("[rain] no path"); return false; }

  double fx, fy;
  lonLatToTile((double)s_lat, (double)s_lon, WX_ZOOM, s_tileX, s_tileY, fx, fy);
  s_tileDx = (int)round(R_CX - fx);
  s_tileDy = (int)round(R_CY - fy);

  // base map (Gaode, icon center, opaque)
  char baseUrl[200];
  snprintf(baseUrl, sizeof(baseUrl),
           "http://webrd01.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1"
           "&style=8&x=%d&y=%d&z=%d", s_tileX, s_tileY, WX_ZOOM);
  gfx->fillScreen(C_BLACK);
  if (!downloadToBuf(baseUrl)) { txtC("BASE ERR", 120, C_RED); return false; }
  if (!decodePng(0)) { txtC("BASE DEC ERR", 120, C_RED); return false; }

  // rain overlay
  char rainUrl[240];
  snprintf(rainUrl, sizeof(rainUrl),
           "http://tilecache.rainviewer.com%s/256/%d/%d/%d/2/1_1.png",
           s_rainPath.c_str(), WX_ZOOM, s_tileX, s_tileY);
  if (!downloadToBuf(rainUrl)) {
    useFont(u8g2_font_helvB08_tr);
    txtC("RAIN ERR", 120, C_RED);
    return false;
  }
  if (!decodePng(1)) {
    useFont(u8g2_font_helvB08_tr);
    txtC("RAIN DEC ERR", 120, C_RED);
    return false;
  }

  gfx->drawCircle(R_CX, R_CY, R_R, C_WHITE);
  int cover = s_rainPx * 100 / 39408;
  useFont(u8g2_font_helvB08_tr);
  char st[32];
  snprintf(st, sizeof(st), "OK R%d%%", cover);
  txtC(st, 224, C_GREEN);
  s_rainMs = millis();
  s_rainOk = true;
  Serial.printf("[rain] ok cover=%d%%\n", cover);
  return true;
}

// ---------------- page entry ----------------
static void enterPage(int page) {
  s_screen = page;
  if (page == SCR_CLOCK) {
    renderClock();
  } else if (page == SCR_PLANES) {
    drawPlanes(s_sweep);
  } else if (page == SCR_FORECAST) {
    renderForecast();
  } else if (page == SCR_RAIN) {
    rainRefresh();
  }
}

// ---------------- button ----------------
#define DEBOUNCE_MS 20
#define HOLD_MS 600
static bool s_prev = true;
static unsigned long s_tDown = 0;
static bool s_longFired = false;

static void handleButton() {
  bool level = digitalRead(PIN_KEY) == HIGH;
  unsigned long now = millis();
  if (level != s_prev) {
    delay(DEBOUNCE_MS);
    level = digitalRead(PIN_KEY) == HIGH;
    if (level != s_prev) {
      if (!level) {
        s_tDown = now;
        s_longFired = false;
      }
      s_prev = level;
    }
  }
  if (level == LOW && !s_longFired && now - s_tDown >= HOLD_MS) {
    s_longFired = true;
    s_rangeIdx = (s_rangeIdx + 1) % 6;
    Serial.printf("[key] long range -> %.0fkm\n", rangeKm());
    enterPage(SCR_PLANES);
  }
  if (level == HIGH && s_prev == LOW) {
    if (!s_longFired) {
      int next = (s_screen + 1) % 4;
      Serial.printf("[key] short -> page %d\n", next);
      enterPage(next);
    }
  }
}

// ---------------- ticks ----------------
static void tickClock() {
  if (s_timeOk && s_tm.tm_sec != s_lastSec) {
    s_lastSec = s_tm.tm_sec;
    renderClock();
  }
}
static void tickPlanes(unsigned long now) {
  if (now - s_sweepMs >= 80) {
    s_sweepMs = now;
    s_sweep += 7;
    if (s_sweep >= 360) s_sweep -= 360;
    drawPlanes(s_sweep);
  }
  if (now - s_planeMs >= 60000 || s_planeCount == 0) {
    if (fetchPlanes()) drawPlanes(s_sweep);
  }
}
static void tickForecast(unsigned long now) {
  if ((!s_metOk && now - s_metTryMs >= 30000) ||
      (s_metOk && now - s_metMs >= 600000)) {
    s_metTryMs = now;
    fetchOpenMeteo();
  }
}
static void tickRain(unsigned long now) {
  if ((!s_rainOk && now - s_rainTryMs >= 30000) ||
      (s_rainOk && now - s_rainMs >= 600000)) {
    s_rainTryMs = now;
    rainRefresh();
  }
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("PlaneRadar %s\n", FW_VER);

  pinMode(PIN_KEY, INPUT_PULLUP);
  s_prev = digitalRead(PIN_KEY) == HIGH;

  s_bus = new Arduino_ESP32SPI(PR_DC, PR_CS, PR_SCLK, PR_MOSI, GFX_NOT_DEFINED,
                               PR_SPI_FREQ, SPI_MODE0, 1, 1, 1, 1);
  gfx = new Arduino_GC9A01(s_bus, PR_RST, 0, true);
  gfx->begin();
  gfx->setUTF8Print(true);
  gfx->fillScreen(C_BLACK);

  s_pngData = (uint8_t *)malloc(PNG_BUF_CAP);
  if (!s_pngData) Serial.println("[mem] png alloc fail");

  s_prefs.begin("radar", false);
  int ver = s_prefs.getInt("ver", 0);
  s_ssid = s_prefs.getString("ssid", "");
  s_pass = s_prefs.getString("pass", "");
  s_lat = s_prefs.getFloat("lat", 23.3924f);
  s_lon = s_prefs.getFloat("lon", 113.2988f);
  s_city = s_prefs.getString("city", "Guangzhou");
  s_cfgValid = (ver == CFG_VER && s_ssid.length() > 0);

  if (!s_cfgValid) {
    startPortal();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(s_ssid.c_str(), s_pass.c_str());
  useFont(u8g2_font_helvB08_tr);
  txtC("WIFI...", 110, C_YELLOW);
  int wtry = 0;
  while (WiFi.status() != WL_CONNECTED && wtry < 40) {
    delay(500);
    wtry++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    txtC("WIFI FAIL", 130, C_RED);
    delay(2000);
    startPortal();
    return;
  }
  Serial.printf("[wifi] %s rssi=%d\n", WiFi.localIP().toString().c_str(),
                WiFi.RSSI());

  locateByIp();
  syncTime();
  fetchOpenMeteo();
  fetchPlanes();

  s_screen = SCR_CLOCK;
  renderClock();
}

// ---------------- loop ----------------
void loop() {
  unsigned long now = millis();
  handleButton();
  switch (s_screen) {
    case SCR_CLOCK: tickClock(); break;
    case SCR_PLANES: tickPlanes(now); break;
    case SCR_FORECAST: tickForecast(now); break;
    case SCR_RAIN: tickRain(now); break;
  }
  delay(20);
}
