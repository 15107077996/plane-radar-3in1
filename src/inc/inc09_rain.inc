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
    // contiguous opaque runs, now overwriting base on transparent pixels
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
  // Allocate exactly what this tile needs: a 72KB block no longer fits after
  // WiFi/TLS, while the tiles themselves are 5-17KB.
  size_t need = (total > 0) ? (size_t)total + 8 : (size_t)PNG_BUF_CAP;
  if (need > PNG_BUF_CAP) {
    Serial.printf("[mem] tile too big total=%d cap=%u\n", total,
                  (unsigned)PNG_BUF_CAP);
    http.end();
    return false;
  }
  if (!ensurePngBuf(need)) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  s_pngLen = 0;
  uint32_t start = millis();
  while (http.connected() && (total < 0 || (int)s_pngLen < total)) {
    size_t avail = stream->available();
    if (avail) {
      if (s_pngLen + avail > s_pngCap) avail = s_pngCap - s_pngLen;
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
  // Sanity check before touching the network: the smallest tile needs this much.
  if (ESP.getMaxAllocHeap() < PNG_BUF_MIN) {
    Serial.printf("[mem] low heap free=%u maxblk=%u\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    gfx->fillScreen(C_BLACK);
    useFont(u8g2_font_unifont_t_chinese4);
    txtC("内存不足", 120, C_RED);
    return false;
  }

  // Show an immediate loading screen so the page is never mistaken for missing.
  gfx->fillScreen(C_BLACK);
  useFont(u8g2_font_unifont_t_chinese4);
  txtC("降雨云图", 104, C_YELLOW);
  txtC("加载中...", 126, C_WHITE);

  HTTPClient http;
  http.begin("http://api.rainviewer.com/public/weather-maps.json");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[rain] meta http=%d\n", code); http.end();
    txtC("云图网络错误", 150, C_RED); releasePngBuf(); return false;
  }
  String meta = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, meta)) {
    Serial.println("[rain] meta json fail");
    txtC("云图数据错误", 150, C_RED); releasePngBuf(); return false;
  }
  JsonArray now = doc["radar"]["nowcast"];
  JsonArray past = doc["radar"]["past"];
  if ((now.isNull() || now.size() == 0) && (past.isNull() || past.size() == 0)) {
    Serial.println("[rain] no frames");
    txtC("暂无云图", 150, C_RED); releasePngBuf(); return false;
  }
  // Prefer the first nowcast frame; if nowcast is empty, use the latest past frame.
  if (!now.isNull() && now.size() > 0) {
    s_rainPath = (const char *)now[0]["path"];
  } else {
    s_rainPath = (const char *)past[past.size() - 1]["path"];
    Serial.println("[rain] nowcast empty, use latest past");
  }
  if (s_rainPath.length() == 0) {
    Serial.println("[rain] no path");
    txtC("暂无云图", 150, C_RED); releasePngBuf(); return false;
  }

  double fx, fy;
  lonLatToTile((double)s_lat, (double)s_lon, WX_ZOOM, s_tileX, s_tileY, fx, fy);
  // One 256x256 tile cannot fill a 224px dial: the home pixel sits off-centre
  // inside its tile, so the dial spans 2..4 tiles. Draw every tile the dial
  // touches, one at a time, reusing the same small buffer.
  double gx = (double)s_tileX * 256.0 + fx;
  double gy = (double)s_tileY * 256.0 + fy;
  int txA = (int)floor((gx - R_R) / 256.0);
  int txB = (int)floor((gx + R_R) / 256.0);
  int tyA = (int)floor((gy - R_R) / 256.0);
  int tyB = (int)floor((gy + R_R) / 256.0);
  if (txA < 0) txA = 0;
  if (tyA < 0) tyA = 0;
  if (txB > 127) txB = 127;
  if (tyB > 127) tyB = 127;

  gfx->fillScreen(C_BLACK);
  bool ok = false;
  bool baseBad = false, rainBad = false;
  char baseUrl[200];
  char rainUrl[240];
  int tcount = 0;
  for (int ty = tyA; ty <= tyB; ty++) {
    for (int tx = txA; tx <= txB; tx++) {
      s_tileDx = (int)round(R_CX - (gx - (double)tx * 256.0));
      s_tileDy = (int)round(R_CY - (gy - (double)ty * 256.0));
      // base map (Gaode, opaque)
      snprintf(baseUrl, sizeof(baseUrl),
               "http://webrd01.is.autonavi.com/appmaptile?lang=zh_cn&size=1"
               "&scale=1&style=8&x=%d&y=%d&z=%d", tx, ty, WX_ZOOM);
      if (!downloadToBuf(baseUrl)) { baseBad = true; continue; }
      if (!decodePng(0)) { baseBad = true; continue; }
      tcount++;
      // rain overlay for the same tile
      snprintf(rainUrl, sizeof(rainUrl),
               "http://tilecache.rainviewer.com%s/256/%d/%d/%d/2/1_1.png",
               s_rainPath.c_str(), WX_ZOOM, tx, ty);
      if (!downloadToBuf(rainUrl)) { rainBad = true; continue; }
      if (!decodePng(1)) { rainBad = true; continue; }
      ok = true;
    }
  }
  if (!ok) {
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(baseBad ? "底图加载失败" : "雨层加载失败", 120, C_RED);
  } else {
    gfx->drawCircle(R_CX, R_CY, R_R, C_WHITE);
    int cover = s_rainPx * 100 / 39408;
    useFont(u8g2_font_helvB10_tr);
    char st[32];
    snprintf(st, sizeof(st), "OK R%d%%", cover);
    txtC(st, 224, C_GREEN);
    s_rainMs = millis();
    s_rainOk = true;
    Serial.printf("[rain] ok tiles=%d cover=%d%% free=%u\n", tcount, cover,
                  ESP.getFreeHeap());
  }

  releasePngBuf();
  return ok;
}
