// ---------------- planes ----------------
static float planeDistKm(int i) {
  return haversineKm(s_lat, s_lon, s_planes[i].lat, s_planes[i].lon);
}

// Destination point from start, bearing (deg) and distance (km).
static void destPoint(double lat, double lon, float bear, double km,
                      double &outLat, double &outLon) {
  double R = 6371.0;
  double br = deg2rad(bear);
  double lr = km / R;
  double phi1 = deg2rad(lat), lam1 = deg2rad(lon);
  double phi2 = asin(sin(phi1) * cos(lr) +
                     cos(phi1) * sin(lr) * cos(br));
  double lam2 = lam1 + atan2(sin(br) * sin(lr) * cos(phi1),
                             cos(lr) - sin(phi1) * sin(phi2));
  outLat = rad2deg(phi2);
  outLon = rad2deg(lam2);
}

// Live position via dead reckoning: extrapolate from the last network fix
// along the track at ground speed. A new fetch resets the base.
static void planeLive(int i, unsigned long now, double &la, double &lo) {
  Plane &p = s_planes[i];
  float dt = (now - p.baseMs) / 1000.0f; // seconds since fix
  if (dt < 0) dt = 0;
  if (dt > 60) dt = 60;                  // cap extrapolation window
  if (p.gs > 1.0f && dt > 0) {
    double km = (p.gs * 1.852) * (dt / 3600.0); // knots -> km/h * hours
    destPoint(p.lat, p.lon, (float)p.head, km, la, lo);
  } else {
    la = p.lat;
    lo = p.lon;
  }
}

// Live distance from the center to the extrapolated position.
static float planeLiveDistKm(int i) {
  double la, lo;
  planeLive(i, millis(), la, lo);
  return haversineKm(s_lat, s_lon, la, lo);
}

static int nearest() {
  int best = -1;
  float bd = 1e9;
  for (int i = 0; i < s_planeCount; i++) {
    float d = planeLiveDistKm(i);
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

// Rotate a body-coordinate point (lx=forward, ly=right) by heading.
static void planeRot(int x, int y, float sn, float cs,
                     float lx, float ly, int &ox, int &oy) {
  ox = x + (int)(lx * sn + ly * cs + 0.5f);
  oy = x + (int)(-lx * cs + ly * sn + 0.5f);
  oy = x + (int)(-lx * cs + ly * sn + 0.5f);
}
static void fillTriBody(int x, int y, float sn, float cs,
                        float x0, float y0, float x1, float y1,
                        float x2, float y2, uint16_t col) {
  int ax, ay, bx, by, cx, cy;
  planeRot(x, y, sn, cs, x0, y0, ax, ay);
  planeRot(x, y, sn, cs, x1, y1, bx, by);
  planeRot(x, y, sn, cs, x2, y2, cx, cy);
  gfx->fillTriangle(ax, ay, bx, by, cx, cy, col);
}

// Clear top-view airplane icon (fuselage + swept wings + tail), rotated by heading.
static void drawPlaneMarker(int x, int y, int head, uint16_t col) {
  float a = head * PI / 180.0f;
  float sn = sin(a), cs = cos(a);
  // fuselage (nose -> tail)
  fillTriBody(x, y, sn, cs, 8, 0, -6, 1, -6, -1, col);
  // right main wing
  fillTriBody(x, y, sn, cs, 0, 0, -3, 9, -3, 0, col);
  fillTriBody(x, y, sn, cs, -3, 9, -4, 9, -3, 0, col);
  // left main wing
  fillTriBody(x, y, sn, cs, 0, 0, -3, 0, -3, -9, col);
  fillTriBody(x, y, sn, cs, -3, 0, -4, -9, -3, -9, col);
  // tail planes
  fillTriBody(x, y, sn, cs, -5, 0, -7, 3, -6, 0, col);
  fillTriBody(x, y, sn, cs, -5, 0, -6, 0, -7, -3, col);
}

static bool fetchPlanes() {
  // Never start TLS while the big PNG buffer is held.
  releasePngBuf();

  char url[160];
  snprintf(url, sizeof(url),
           "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%.1f",
           s_lat, s_lon, FETCH_NM);
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.begin(tls, url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      JsonArray ac = doc["ac"];
      int n = 0;
      for (JsonObject p : ac) {
        if (n >= MAX_PLANES) break;
        if (p["lat"].isNull() || p["lon"].isNull()) continue;
        Plane &pl = s_planes[n];
        const char *fl = p["flight"];
        const char *tp = p["t"];
        String cs = fl ? String(fl) : "????";
        cs.trim();
        cs.toCharArray(pl.call, sizeof(pl.call));
        String tt = tp ? String(tt) : String("");
        tt.toCharArray(pl.typ, sizeof(pl.typ));
        pl.lat = p["lat"].as<double>();
        pl.lon = p["lon"].as<double>();
        pl.alt = p["alt_baro"] | 0;
        pl.head = p["track"] | 0;
        pl.gs = p["gs"] | 0;
        pl.baseMs = millis();
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
                    nt >= 0 ? planeDistKm(nt) : 0);
    }
  } else {
    Serial.printf("[planes] http %d\n", code);
  }
  http.end();
  return ok;
}
