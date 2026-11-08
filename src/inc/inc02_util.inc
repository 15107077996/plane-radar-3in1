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

// ---------------- PNG buffer lifecycle ----------------
// The PNG buffer is only held while downloading/decoding rain tiles, and it is
// sized to the exact tile Content-Length (~16KB base map, ~5KB rain layer)
// instead of a fixed 72KB block that no longer fits after WiFi/TLS.
// If a smaller allocation fails the size is halved down to PNG_BUF_MIN.
static bool ensurePngBuf(size_t need) {
  if (need < PNG_BUF_MIN) need = PNG_BUF_MIN;
  if (need > PNG_BUF_CAP) need = PNG_BUF_CAP;
  if (s_pngData && s_pngCap >= need) return true;
  if (s_pngData) {
    free(s_pngData);
    s_pngData = nullptr;
    s_pngCap = 0;
  }
  size_t cap = need;
  while (cap >= PNG_BUF_MIN) {
    s_pngData = (uint8_t *)malloc(cap);
    if (s_pngData) {
      s_pngCap = cap;
      Serial.printf("[mem] png buf %u free=%u maxblk=%u\n", (unsigned)cap,
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return true;
    }
    cap /= 2;
  }
  Serial.printf("[mem] png alloc fail need=%u free=%u maxblk=%u\n",
                (unsigned)need, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return false;
}
static void releasePngBuf() {
  if (s_pngData) {
    free(s_pngData);
    s_pngData = nullptr;
  }
  s_pngCap = 0;
  s_pngLen = 0;
}

// ---------------- config portal ----------------
static void saveCfg() {
  s_prefs.putInt("ver", CFG_VER);
  s_prefs.putString("ssid", s_ssid);
  s_prefs.putString("pass", s_pass);
  s_prefs.putBool("manual", s_manual);
  s_prefs.putFloat("lat", s_lat);
  s_prefs.putFloat("lon", s_lon);
  s_prefs.putString("city", s_city);
}

static void handleRoot() {
  String html = "<doctype html><html><head><meta charset=utf-8>"
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
  s_srv = new WebServer(8);
  s_srv->on("/", handleRoot);
  s_srv->on("/save", handleSave);
  s_srv->begin();
  Serial.println("AP PlaneRadar-Setup up, http://192.168.4.1");
  useFont(u8g2_font_helvB10_tr);
  gfx->fillScreen(C_BLACK);
  txtC("SETUP WIFI", 100, C_YELLOW);
  txtC("PlaneRadar-Setup", 122, C_WHITE);
  txtC("192.168.4.1", 144, C_CYAN);
  while (true) {
    s_srv->handleClient();
    delay(10);
  }
}

// Reverse-geocode a coordinate to a proper Chinese county/city name.
// BigDataCloud free client endpoint (no key): locality = county/district,
// city = prefecture-level city. Avoids street-level names such as "金融街".
static String reverseName(double la, double lo) {
  char url[170];
  snprintf(url, sizeof(url),
           "https://api.bigdatacloud.net/data/reverse-geocode-client?"
           "latitude=%.5f&longitude=%.5f&localityLanguage=zh",
           la, lo);
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.begin(tls, url);
  String name = "";
  if (http.GET() == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      const char *loc = doc["locality"];
      const char *city = doc["city"];
      if (loc && loc[0]) name = String(loc);
      else if (city && city[0]) name = String(city);
    }
  }
  http.end();
  return name;
}

// ---------------- IP locate ----------------
static void locateByIp() {
  HTTPClient http;
  // lang=zh-CN -> Chinese city/region names
  http.begin("http://ip-api.com/json/?fields=status,message,city,district,regionName,lat,lon&lang=zh-CN");
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      const char *st = doc["status"];
      if (st && strcmp(st, "success") == 0) {
        float la = doc["lat"] | s_lat;
        float lo = doc["lon"] | s_lon;
        const char *ci = doc["city"];
        const char *rg = doc["regionName"];
        s_lat = la;
        s_lon = lo;
        // Prefer the proper county/city name from reverse geocoding;
        // fall back to ip-api city, then region, then keep current.
        String rn = reverseName(s_lat, s_lon);
        if (rn.length()) s_city = rn;
        else if (ci && ci[0]) s_city = String(ci);
        else if (rg && rg[0]) s_city = String(rg);
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

