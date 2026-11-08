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
  oy = y + (int)(-lx * cs + ly * sn + 0.5f);
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
      // Keep the MAX_PLANES nearest aircraft in distance order. The JSON array
      // order is arbitrary, so taking the first 40 entries used to drop the
      // nearby ones and leave the screen empty.
      int n = 0;
      int seen = 0;
      for (JsonObject p : ac) {
        seen++;
        if (p["lat"].isNull() || p["lon"].isNull()) continue;
        Plane tmp;
        const char *fl = p["flight"];
        const char *tp = p["t"];
        String cs = fl ? String(fl) : String("????");
        cs.trim();
        cs.toCharArray(tmp.call, sizeof(tmp.call));
        String tt = tp ? String(tp) : String("");
        tt.toCharArray(tmp.typ, sizeof(tmp.typ));
        tmp.lat = p["lat"].as<double>();
        tmp.lon = p["lon"].as<double>();
        tmp.alt = p["alt_baro"] | 0;
        tmp.head = p["track"] | 0;
        tmp.gs = p["gs"] | 0;
        tmp.baseMs = millis();
        float dkm = haversineKm(s_lat, s_lon, tmp.lat, tmp.lon);
        tmp.distNm = dkm / 1.852f;
        tmp.distM = dkm * 1000.0f;
        tmp.bear = bearingTo(s_lat, s_lon, tmp.lat, tmp.lon);
        if (n >= MAX_PLANES && dkm >= s_planes[MAX_PLANES - 1].distM / 1000.0f)
          continue;
        int pos = (n < MAX_PLANES) ? n : (MAX_PLANES - 1);
        while (pos > 0 && s_planes[pos - 1].distM > tmp.distM) {
          s_planes[pos] = s_planes[pos - 1];
          pos--;
        }
        s_planes[pos] = tmp;
        if (n < MAX_PLANES) n++;
      }
      s_planeCount = n;
      s_planeMs = millis();
      ok = true;
      // Once, on boot, pick the smallest range that actually holds traffic.
      if (s_rangeAuto && s_planeCount > 0) {
        int want = RANGE_N - 1;
        for (int ri = 0; ri < RANGE_N; ri++) {
          int fit = 0;
          for (int i = 0; i < s_planeCount; i++)
            if (s_planes[i].distM <= RANGES_KM[ri] * 1000.0f) fit++;
          if (fit >= 3) { want = ri; break; }
        }
        s_rangeIdx = want;
      }
      int nt = nearest();
      Serial.printf("[planes] total=%d seen=%d nearest=%d %s %.0fkm range=%.0fkm\n",
                    s_planeCount, seen, nt, nt >= 0 ? s_planes[nt].call : "-",
                    nt >= 0 ? planeDistKm(nt) : 0, rangeKm());
    }
  } else {
    Serial.printf("[planes] http %d\n", code);
  }
  http.end();
  return ok;
}
