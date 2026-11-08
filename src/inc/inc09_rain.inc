// ---------------- rain png ----------------
// RainViewer encodes echo intensity in the alpha channel (light rain ~30-60,
// heavy ~150+). Copying the raw RGB ignored that, so weak echoes landed as pale
// smudges on the map and the page looked empty. Map alpha to a real colour
// ladder instead, the way the reference screenshot looks.
static uint16_t rainColor(uint8_t a) {
  if (a < 10) return C_BLACK; // caller treats this as transparent
  if (a < 32) return 0x0320;  // very light: dark green
  if (a < 60) return C_GREEN; // light
  if (a < 90) return C_YELLOW;
  if (a < 125) return C_ORANGE;
  if (a < 170) return C_RED;
  return C_MAGENTA; // extreme core
}

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
      if (a < 10) trans = true;
      else if (s_pngMode == 1 && s_rainSrc == 0) col = rainColor(a);
      else col = (uint16_t)(((px[i * 4] & 0xF8) << 8) |
                            ((px[i * 4 + 1] & 0xFC) << 3) | (px[i * 4 + 2] >> 3));
      if (s_pngMode == 1 && !trans && inCircle) s_rainPx++;
    } else if (iPixelType == PNG_PIXEL_TRUECOLOR) {
      col = (uint16_t)(((px[i * 3] & 0xF8) << 8) |
                       ((px[i * 3 + 1] & 0xFC) << 3) | (px[i * 3 + 2] >> 3));
    } else if (iPixelType == PNG_PIXEL_INDEXED) {
      uint8_t idx = px[i];
      uint8_t a = pDraw->pPalette[768 + idx]; // tRNS alpha, default 255
      if (idx == 0 || a < 10) trans = true;
      else if (s_pngMode == 1 && s_rainSrc == 0) col = rainColor(a);
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
    // contiguous opaque runs
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
  return true;
}

static bool downloadToBuf(const char *url) {
  HTTPClient http;
  http.setConnectTimeout(4500);
  http.setTimeout(6500);
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
      if (millis() - start > 8000) break;
    }
  }
  http.end();
  if ((int)s_pngLen < 64) {
    Serial.printf("[dl] too short %u\n", s_pngLen);
    return false;
  }
  return true;
}

static void rainTileUrl(char *out, size_t n, const String &path, int tx, int ty) {
  if (s_rainSrc == 1)
    snprintf(out, n, "https://tilecache.rainviewer.com%s/256/%d/%d/%d/0/0_0.png",
             path.c_str(), WX_ZOOM, tx, ty);
  else
    snprintf(out, n, "http://tilecache.rainviewer.com%s/256/%d/%d/%d/2/1_1.png",
             path.c_str(), WX_ZOOM, tx, ty);
}

// Base tiles are cached under their own coordinates, so a different city simply
// misses the cache instead of painting the wrong map.
static void baseName(char *out, size_t n, int i) {
  snprintf(out, n, "/b%dx%d.png", s_tiles[i].tx, s_tiles[i].ty);
}

// Progress line kept on top of whatever has already been painted.
static void loadText(const char *s) {
  gfx->fillRect(40, 92, 160, 20, C_BLACK);
  useFont(u8g2_font_unifont_t_chinese4);
  txtC(s, 106, C_CYAN);
}

static void drawRainHud() {
  gfx->drawCircle(R_CX, R_CY, R_R, C_WHITE);
  int cover = s_rainPxMax * 100 / 39408;
  char st[32];
  snprintf(st, sizeof(st), "OK R%d%% %d/%d", cover, s_animReady ? s_animFrame + 1 : 1,
           s_animReady ? s_frameN : 1);
  useFont(u8g2_font_helvB08_tr);
  txtC(st, 224, C_GREEN);
}

static size_t cachedSize(const char *p);

// One animation frame = base tiles from flash, then that frame's rain tiles on
// top. Redrawing the base is what erases the previous frame, so the echoes
// actually travel instead of piling up.
static bool drawFrame(int f) {
  bool ok = false;
  s_rainPx = 0;
  for (int i = 0; i < s_tileN; i++) {
    char p[16];
    baseName(p, sizeof(p), i);
    s_tileDx = s_tiles[i].dx;
    s_tileDy = s_tiles[i].dy;
    if (fsLoad(p) && decodePng(0)) ok = true;
  }
  if (ok) {
    for (int i = 0; i < s_tileN; i++) {
      char p[16];
      snprintf(p, sizeof(p), "/r%d_%d.png", f, i);
      s_tileDx = s_tiles[i].dx;
      s_tileDy = s_tiles[i].dy;
      if (fsLoad(p)) decodePng(1);
    }
  }
  if (s_rainPx > s_rainPxMax) s_rainPxMax = s_rainPx;
  return ok;
}

static void rainAnimTick(unsigned long now) {
  if (!s_animReady || s_frameN < 2) return;
  if (now - s_animMs < 500) return;
  s_animMs = now;
  s_animFrame = (s_animFrame + 1) % s_frameN;
  drawFrame(s_animFrame);
  drawRainHud();
}

// Cached base tile? Map tiles for a z7 cell barely change, so reuse it from
// flash instead of burning a whole HTTP round-trip on every refresh.
static size_t cachedSize(const char *p) {
  if (!s_fsOk) return 0;
  File f = SPIFFS.open(p, "r");
  if (!f) return 0;
  size_t sz = (size_t)f.size();
  f.close();
  return sz;
}

static bool allFramesPresent() {
  if (!s_fsOk) return false;
  for (int fr = 0; fr < s_frameN; fr++) {
    for (int i = 0; i < s_tileN; i++) {
      char p[16];
      snprintf(p, sizeof(p), "/r%d_%d.png", fr, i);
      if (cachedSize(p) < 100) return false;
    }
  }
  return true;
}

// Cache signature = frame paths + the tile coordinates they were cut for. The
// tile part matters: after a city change the same slot files would otherwise be
// painted at the wrong place and the map would show another region's rain.
static String frameSig() {
  String s = "";
  for (int i = 0; i < s_frameN; i++) { s += s_framePath[i]; s += "|"; }
  s += "@";
  for (int i = 0; i < s_tileN; i++) {
    char t[16];
    snprintf(t, sizeof(t), "%d,%d;", s_tiles[i].tx, s_tiles[i].ty);
    s += t;
  }
  return s;
}

// RainViewer publishes a new frame every 10 min, so a 6-min refresh usually
// reuses exactly the same three images. Remembering the frame list in flash
// turns that refresh into a pure redraw (~1s instead of ~10s over HTTP).
static bool framesCached() {
  if (!s_fsOk || s_frameN <= 0) return false;
  File f = SPIFFS.open("/rpath.txt", "r");
  if (!f) return false;
  String old = f.readString();
  f.close();
  if (old != frameSig()) return false;
  return allFramesPresent();
}

static void saveFrameSig() {
  if (!s_fsOk) return;
  File f = SPIFFS.open("/rpath.txt", "w");
  if (!f) return;
  f.print(frameSig());
  f.close();
}

static bool s_reuseFr[RAIN_FRAMES];
static int s_reuseFrom[RAIN_FRAMES];

// New frame list normally slides by one: {t-20,t-10,t} becomes {t-10,t,t+10}.
// Two of the three images are already on flash, so rename their slots instead of
// downloading them again -- a refresh that lands on a brand new frame then costs
// one tile pair (~2s) rather than eight requests (~16s).
static bool shiftFrames() {
  if (!s_fsOk || s_frameN <= 0 || s_tileN <= 0) return false;
  for (int i = 0; i < RAIN_FRAMES; i++) { s_reuseFr[i] = false; s_reuseFrom[i] = -1; }
  File f = SPIFFS.open("/rpath.txt", "r");
  if (!f) return false;
  String old = f.readString();
  f.close();
  int sep = old.indexOf('@');
  if (sep < 0) return false;
  String oldTiles = old.substring(sep + 1);
  String curTiles = "";
  for (int i = 0; i < s_tileN; i++) {
    char t[16];
    snprintf(t, sizeof(t), "%d,%d;", s_tiles[i].tx, s_tiles[i].ty);
    curTiles += t;
  }
  if (oldTiles != curTiles) return false; // city/tile changed: nothing to reuse
  String oldPath[RAIN_FRAMES];
  int oldN = 0;
  int p = 0;
  while (oldN < RAIN_FRAMES) {
    int q = old.indexOf('|', p);
    if (q < 0 || q > sep) break;
    oldPath[oldN++] = old.substring(p, q);
    p = q + 1;
  }
  if (oldN <= 0) return false;

  int reuseN = 0;
  for (int j = 0; j < s_frameN; j++) {
    for (int k = 0; k < oldN; k++) {
      if (oldPath[k] != s_framePath[j]) continue;
      bool have = true;
      for (int i = 0; i < s_tileN; i++) {
        char pth[16];
        snprintf(pth, sizeof(pth), "/r%d_%d.png", k, i);
        if (cachedSize(pth) < 100) { have = false; break; }
      }
      if (!have) continue;
      s_reuseFr[j] = true;
      s_reuseFrom[j] = k;
      reuseN++;
      break;
    }
  }
  if (reuseN <= 0) return false;

  // Two passes through temp names: moving slot k down into slot j would
  // otherwise overwrite the very file the next frame still needs.
  for (int j = 0; j < s_frameN; j++) {
    if (!s_reuseFr[j]) continue;
    for (int i = 0; i < s_tileN; i++) {
      char a[16], b[16];
      snprintf(a, sizeof(a), "/r%d_%d.png", s_reuseFrom[j], i);
      snprintf(b, sizeof(b), "/tm%d_%d.png", s_reuseFrom[j], i);
      SPIFFS.remove(b);
      SPIFFS.rename(a, b);
    }
  }
  for (int j = 0; j < s_frameN; j++) {
    for (int i = 0; i < s_tileN; i++) {
      char pth[16];
      snprintf(pth, sizeof(pth), "/r%d_%d.png", j, i);
      if (s_reuseFr[j]) {
        char b[16];
        snprintf(b, sizeof(b), "/tm%d_%d.png", s_reuseFrom[j], i);
        SPIFFS.remove(pth);
        SPIFFS.rename(b, pth);
      } else if (s_reuseFrom[j] < 0) {
        // slot holds a frame that is no longer in the list: drop it, or the
        // "all frames present" check would vouch for a stale image
        SPIFFS.remove(pth);
      }
    }
  }
  Serial.printf("[rain] shift reuse=%d/%d\n", reuseN, s_frameN);
  return true;
}

static bool rainRefresh() {
  unsigned long t0 = millis();
  s_rainOk = false;
  s_animReady = false;
  s_frameN = 0;
  s_rainPx = 0;
  s_rainPxMax = 0;
  for (int i = 0; i < RAIN_FRAMES; i++) { s_reuseFr[i] = false; s_reuseFrom[i] = -1; }
  // Sanity check before touching the network: the smallest tile needs this much.
  if (ESP.getMaxAllocHeap() < PNG_BUF_MIN) {
    Serial.printf("[mem] low heap free=%u maxblk=%u\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    gfx->fillScreen(C_BLACK);
    useFont(u8g2_font_unifont_t_chinese4);
    txtC("内存不足", 120, C_RED);
    return false;
  }

  // Immediate loading screen so the page is never mistaken for missing.
  gfx->fillScreen(C_BLACK);
  useFont(u8g2_font_unifont_t_chinese4);
  txtC("降雨云图", 104, C_YELLOW);
  loadText("读取数据...");

  HTTPClient http;
  http.setConnectTimeout(4500);
  http.setTimeout(6500);
  http.begin("http://api.rainviewer.com/public/weather-maps.json");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[rain] meta http=%d\n", code); http.end();
    loadText("云图网络错误"); releasePngBuf(); return false;
  }
  String meta = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, meta)) {
    Serial.println("[rain] meta json fail");
    loadText("云图数据错误"); releasePngBuf(); return false;
  }

  // Frame list: radar past frames (newest 3) or satellite IR (newest 3).
  JsonArray arr = (s_rainSrc == 1) ? doc["satellite"]["infrared"].as<JsonArray>()
                                   : doc["radar"]["past"].as<JsonArray>();
  int total = (arr.isNull()) ? 0 : (int)arr.size();
  if (total <= 0) {
    Serial.println("[rain] no frames");
    loadText("暂无云图");
    releasePngBuf();
    return false;
  }
  int want = (total < RAIN_FRAMES) ? total : RAIN_FRAMES;
  for (int i = 0; i < want; i++) {
    int si = total - want + i;
    const char *pth = arr[si]["path"];
    s_framePath[i] = pth ? String(pth) : String("");
    if (s_framePath[i].length() == 0) want = i;
  }
  s_frameN = want;
  if (s_frameN <= 0) {
    loadText("暂无云图");
    releasePngBuf();
    return false;
  }
  s_rainPath = s_framePath[s_frameN - 1];
  Serial.printf("[rain] frames=%d src=%d last=%s\n", s_frameN, s_rainSrc,
                s_rainPath.c_str());

  double fx, fy;
  lonLatToTile((double)s_lat, (double)s_lon, WX_ZOOM, s_tileX, s_tileY, fx, fy);
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

  s_tileN = 0;
  char url[240];
  for (int ty = tyA; ty <= tyB && s_tileN < RAIN_TILES_MAX; ty++) {
    for (int tx = txA; tx <= txB && s_tileN < RAIN_TILES_MAX; tx++) {
      s_tiles[s_tileN].tx = tx;
      s_tiles[s_tileN].ty = ty;
      s_tiles[s_tileN].dx = (int)round(R_CX - (gx - (double)tx * 256.0));
      s_tiles[s_tileN].dy = (int)round(R_CY - (gy - (double)ty * 256.0));
      s_tileN++;
    }
  }

  // Without a mounted filesystem the frames cannot be cached, so fall back to
  // a single static overlay (old behaviour) instead of showing nothing.
  bool single = (!s_fsOk || s_frameN < 2);

  // Everything already on flash and the frame list unchanged: just redraw.
  // Otherwise try the slide-by-one reuse before touching the network.
  bool cached = (!single && framesCached());
  int shifted = 0;
  if (!single && !cached && shiftFrames()) {
    shifted = 1;
    cached = allFramesPresent();
  }
  if (cached) {
    s_animReady = true;
    s_animFrame = s_frameN - 1;
    gfx->fillScreen(C_BLACK);
    if (drawFrame(s_animFrame)) {
      drawRainHud();
      s_rainMs = millis();
      s_animMs = millis();
      s_rainOk = true;
      s_rainPxMax = s_rainPx;
      saveFrameSig();
      releasePngBuf();
      s_rainCostMs = millis() - t0;
      Serial.printf("[rain] reuse frames cover=%d%% cost=%lums shifted=%d\n",
                    s_rainPxMax * 100 / 39408, s_rainCostMs, shifted);
      return true;
    }
  }

  gfx->fillScreen(C_BLACK);
  bool baseOk = false, rainOk = false;
  for (int i = 0; i < s_tileN; i++) {
    char p[16];
    baseName(p, sizeof(p), i);
    s_tileDx = s_tiles[i].dx;
    s_tileDy = s_tiles[i].dy;
    bool got = false;
    if (cachedSize(p) > 2000) got = (fsLoad(p) && decodePng(0));
    if (!got) {
      snprintf(url, sizeof(url),
               "http://webrd01.is.autonavi.com/appmaptile?lang=zh_cn&size=1"
               "&scale=1&style=8&x=%d&y=%d&z=%d",
               s_tiles[i].tx, s_tiles[i].ty, WX_ZOOM);
      if (downloadToBuf(url)) {
        decodePng(0);
        fsSave(p, s_pngLen);
        got = true;
      }
    }
    if (got) baseOk = true;
    char prog[24];
    snprintf(prog, sizeof(prog), "底图 %d/%d", i + 1, s_tileN);
    loadText(prog);
  }
  if (!baseOk) {
    loadText("底图加载失败");
    releasePngBuf();
    return false;
  }

  if (single) {
    // draw the newest frame only, straight over the base
    for (int i = 0; i < s_tileN; i++) {
      s_tileDx = s_tiles[i].dx;
      s_tileDy = s_tiles[i].dy;
      rainTileUrl(url, sizeof(url), s_rainPath, s_tiles[i].tx, s_tiles[i].ty);
      if (!downloadToBuf(url)) continue;
      decodePng(1);
      rainOk = true;
    }
  } else {
    // Newest frame first and shown immediately, older frames filled in after:
    // the map appears in ~3s instead of after every frame has been fetched.
    for (int k = 0; k < s_frameN; k++) {
      int f = s_frameN - 1 - k;
      bool any = false;
      if (s_reuseFr[f]) {
        any = true; // shifted slot: the PNG is already on flash
      } else {
        for (int i = 0; i < s_tileN; i++) {
          s_tileDx = s_tiles[i].dx;
          s_tileDy = s_tiles[i].dy;
          rainTileUrl(url, sizeof(url), s_framePath[f], s_tiles[i].tx, s_tiles[i].ty);
          if (!downloadToBuf(url)) continue;
          char p[16];
          snprintf(p, sizeof(p), "/r%d_%d.png", f, i);
          fsSave(p, s_pngLen);
          any = true;
        }
      }
      if (any) rainOk = true;
      if (f == s_frameN - 1 && any) {
        s_animReady = false;
        s_animFrame = f;
        drawFrame(f);
        drawRainHud();
      }
      char prog[24];
      snprintf(prog, sizeof(prog), "云图 %d/%d", s_frameN - f, s_frameN);
      loadText(prog);
    }
    if (rainOk) {
      if (allFramesPresent()) {
        s_animReady = true;
        saveFrameSig();
      }
      s_animFrame = s_frameN - 1;
      drawFrame(s_animFrame);
    }
  }

  if (!rainOk) {
    loadText("雨层加载失败");
  } else {
    drawRainHud();
    s_rainMs = millis();
    s_rainOk = true;
    s_animMs = millis();
    Serial.printf("[rain] ok tiles=%d frames=%d cover=%d%% free=%u\n", s_tileN,
                  s_frameN, s_rainPxMax * 100 / 39408, ESP.getFreeHeap());
  }

  releasePngBuf();
  s_rainCostMs = millis() - t0;
  Serial.printf("[rain] cost=%lums\n", s_rainCostMs);
  return rainOk;
}
