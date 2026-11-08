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

// ---------------- flash cache (SPIFFS) ----------------
static bool fsLoad(const char *path) {
  if (!s_fsOk) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  size_t sz = (size_t)f.size();
  if (!ensurePngBuf(sz)) { f.close(); return false; }
  size_t rd = f.read(s_pngData, sz);
  f.close();
  s_pngLen = rd;
  return rd == sz && sz > 64;
}
static bool fsSave(const char *path, size_t len) {
  if (!s_fsOk || !s_pngData || len < 64) return false;
  File f = SPIFFS.open(path, "w");
  if (!f) return false;
  size_t wr = f.write(s_pngData, len);
  f.close();
  return wr == len;
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
  s_prefs.putInt("rsrc", s_rainSrc);
}

static void handleRoot() {
  char lat[16], lon[16];
  snprintf(lat, sizeof(lat), "%.5f", s_lat);
  snprintf(lon, sizeof(lon), "%.5f", s_lon);
  String html = "<!doctype html><html><head><meta charset=utf-8>";
  html += "<meta name=viewport content='width=device-width,initial-scale=1'>";
  html += "<title>PlaneRadar</title><style>";
  html += "body{font-family:sans-serif;margin:16px;background:#111;color:#ddd}";
  html += "input,select{width:100%;padding:8px;margin:4px 0 12px;background:#222;";
  html += "color:#ddd;border:1px solid #444}";
  html += "button{padding:12px 20px;background:#2a2;color:#fff;border:0;border-radius:4px}";
  html += "h2,h3{color:#ee0}</style></head><body>";
  html += "<h2>PlaneRadar " FW_VER "</h2><form method=POST action=/save>";
  html += "WiFi name<br><input name=ssid value='" + s_ssid + "'>";
  html += "WiFi password<br><input name=pass value='" + s_pass + "'>";
  html += "City name<br><input name=city value='" + s_city + "'>";
  html += "Latitude<br><input name=lat value='" + String(lat) + "'>";
  html += "Longitude<br><input name=lon value='" + String(lon) + "'>";
  html += "Fixed home (skip IP locate)<br><select name=manual>";
  html += (s_manual ? "<option value=1 selected>on</option><option value=0>off</option>"
                     : "<option value=1>on</option><option value=0 selected>off</option>");
  html += "</select>Rain layer source<br><select name=rsrc>";
  html += (s_rainSrc == 0
               ? "<option value=0 selected>radar echo</option><option value=1>satellite IR</option>"
               : "<option value=0>radar echo</option><option value=1 selected>satellite IR</option>");
  html += "</select><button type=submit>Save and reboot</button></form>";
  html += "<h3>OTA firmware update</h3>";
  html += "<form method=POST action=/update enctype=multipart/form-data>";
  html += "<input type=file name=update><button type=submit>Upload and flash</button></form>";
  html += "<p>Upload firmware.bin (the app image), not a full merged dump.</p>";
  html += "</body></html>";
  s_srv->send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
  if (s_srv->hasArg("ssid")) s_ssid = s_srv->arg("ssid");
  if (s_srv->hasArg("pass")) s_pass = s_srv->arg("pass");
  if (s_srv->hasArg("city")) {
    String c = s_srv->arg("city");
    c.trim();
    if (c.length()) s_city = c;
  }
  if (s_srv->hasArg("lat")) s_lat = (float)atof(s_srv->arg("lat").c_str());
  if (s_srv->hasArg("lon")) s_lon = (float)atof(s_srv->arg("lon").c_str());
  if (s_srv->hasArg("manual")) s_manual = s_srv->arg("manual").toInt() != 0;
  if (s_srv->hasArg("rsrc")) s_rainSrc = s_srv->arg("rsrc").toInt();
  saveCfg();
  Serial.printf("[cfg] saved ssid=%s city=%s lat=%.4f lon=%.4f manual=%d rsrc=%d\n",
                s_ssid.c_str(), s_city.c_str(), s_lat, s_lon, (int)s_manual,
                s_rainSrc);
  s_srv->send(200, "text/plain; charset=utf-8", "Saved, rebooting...");
  delay(800);
  ESP.restart();
}

// ---------------- OTA over HTTP ----------------
// Update writes straight into the inactive app slot, so the 1.3MB image never
// has to sit in RAM. Verified by the bootloader (elf-sha256) on next boot.
static void handleUpdateUpload() {
  HTTPUpload &up = s_srv->upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] start %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Serial.printf("[ota] begin fail err=%u\n", Update.getError());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    size_t wr = Update.write(up.buf, up.currentSize);
    if (wr != up.currentSize)
      Serial.printf("[ota] write short %u of %u\n", (unsigned)wr,
                    (unsigned)up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true))
      Serial.printf("[ota] end ok %u bytes\n", (unsigned)up.totalSize);
    else
      Serial.printf("[ota] end fail err=%u\n", Update.getError());
  }
}
static void handleUpdateDone() {
  bool ok = !Update.hasError();
  s_srv->send(200, "text/plain; charset=utf-8",
              ok ? "Update OK, rebooting" : "Update FAILED");
  if (ok) {
    delay(600);
    ESP.restart();
  }
}

static void startWeb() {
  if (s_srv) return;
  s_srv = new WebServer(8);
  s_srv->on("/", HTTP_GET, handleRoot);
  s_srv->on("/save", HTTP_POST, handleSave);
  s_srv->on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  s_srv->begin();
  Serial.println("[web] ready");
}
static void webLoop() {
  if (s_srv) s_srv->handleClient();
}

static void startPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PlaneRadar-Setup");
  startWeb();
  Serial.println("AP PlaneRadar-Setup up, http://192.168.4.1");
  useFont(u8g2_font_helvB10_tr);
  gfx->fillScreen(C_BLACK);
  txtC("SETUP WIFI", 88, C_YELLOW);
  txtC("PlaneRadar-Setup", 110, C_WHITE);
  txtC("192.168.4.1", 132, C_CYAN);
  useFont(u8g2_font_unifont_t_chinese4);
  txtC("手机连它再打开网页", 154, C_GRAY);
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
