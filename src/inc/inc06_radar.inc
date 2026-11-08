// ---------------- radar (static base + animated sweep wedge) ----------------
#define WEDGE_N 10
#define WEDGE_STEP 4
static const uint16_t WEDGE_COL[WEDGE_N] = {
    0x07E0, 0x0640, 0x0500, 0x0400, 0x0320,
    0x0280, 0x0200, 0x0180, 0x0120, 0x00C0};

struct Mapped { int i; int x; int y; float d; };
static Mapped s_mapped[MAX_PLANES];
static int s_mappedCnt = 0;

static void computeMapped() {
  float R = rangeKm();
  unsigned long now = millis();
  s_mappedCnt = 0;
  for (int i = 0; i < s_planeCount; i++) {
    double la, lo;
    planeLive(i, now, la, lo);
    float d = haversineKm(s_lat, s_lon, la, lo);
    if (d > R) continue;
    float b = bearingTo(s_lat, s_lon, la, lo) * PI / 180;
    int rr = (int)(R_R * d / R);
    int x = R_CX + (int)(sin(b) * rr);
    int y = R_CY - (int)(cos(b) * rr);
    if (s_mappedCnt < MAX_PLANES) {
      s_mapped[s_mappedCnt].i = i;
      s_mapped[s_mappedCnt].x = x;
      s_mapped[s_mappedCnt].y = y;
      s_mapped[s_mappedCnt].d = d;
      s_mappedCnt++;
    }
  }
  // nearest first
  for (int a = 0; a < s_mappedCnt - 1; a++)
    for (int b = a + 1; b < s_mappedCnt; b++) {
      if (s_mapped[b].d < s_mapped[a].d) {
        Mapped t = s_mapped[a]; s_mapped[a] = s_mapped[b]; s_mapped[b] = t;
      }
    }
}

static void drawRings() {
  float R = rangeKm();
  for (int i = 0; i < 4; i++) gfx->drawCircle(R_CX, R_CY, R_R - i, C_RED);
  for (int f = 1; f <= 3; f++) {
    int rr = (int)(R_R * f / 3.0f);
    gfx->drawCircle(R_CX, R_CY, rr, C_DKGREEN);
  }
  gfx->drawLine(R_CX, R_CY - R_R, R_CX, R_CY + R_R, C_DKGREEN);
  gfx->drawLine(R_CX - R_R, R_CY, R_CX + R_R, R_CY, C_DKGREEN);
  (void)R;
}

// Compass letters inside the sweep circle: 北 above, 南 below, 东 right, 西 left.
// Placed 66px off centre so they clear both the header text and the 75px ring.
static void drawCardinals() {
  useFont(u8g2_font_unifont_t_chinese4);
  txtC("北", 60, C_WHITE);
  txtC("南", 192, C_WHITE);
  txtAt("东", 186, 126, C_WHITE);
  txtAt("西", 54, 126, C_WHITE);
}

// Is the compass point inside the fan the sweep just repainted? Redrawing all
// four letters every 100ms cost three pointless unifont blits per tick and made
// the animation uneven, so only the covered ones are repainted.
static bool arcCovers(float sweep, float ang) {
  float d = sweep - ang;
  while (d < 0) d += 360;
  while (d >= 360) d -= 360;
  return d <= 42.0f;
}

static void drawCardinalsNear(float sweep) {
  bool n = arcCovers(sweep, 0), s = arcCovers(sweep, 180);
  bool e = arcCovers(sweep, 90), w = arcCovers(sweep, 270);
  if (!(n || s || e || w)) return;
  useFont(u8g2_font_unifont_t_chinese4);
  if (n) txtC("北", 60, C_WHITE);
  if (s) txtC("南", 192, C_WHITE);
  if (e) txtAt("东", 186, 126, C_WHITE);
  if (w) txtAt("西", 54, 126, C_WHITE);
}

static void drawMarkers() {
  for (int i = 0; i < s_mappedCnt; i++) {
    int pi = s_mapped[i].i;
    drawPlaneMarker(s_mapped[i].x, s_mapped[i].y, s_planes[pi].head,
                    altColor(s_planes[pi].alt));
  }
}

// range numbers + header + per-plane text labels (all static text)
static void drawLabels() {
  float R = rangeKm();
  useFont(u8g2_font_helvB08_tr);
  char lb[16];
  for (int f = 1; f <= 3; f++) {
    int rr = (int)(R_R * f / 3.0f);
    float val = R * f / 3.0f;
    if (val >= 10) snprintf(lb, sizeof(lb), "%.0f", val);
    else snprintf(lb, sizeof(lb), "%.1f", val);
    txtAt(lb, R_CX + 2, R_CY - rr + 3, C_CYAN);
  }

  int lc = s_mappedCnt < MAX_LABELS ? s_mappedCnt : MAX_LABELS;
  useFont(u8g2_font_helvB10_tr);
  for (int k = 0; k < lc; k++) {
    int pi = s_mapped[k].i;
    int mx = s_mapped[k].x, my = s_mapped[k].y;
    bool onRight = mx >= R_CX;
    int lx = onRight ? mx + 8 : mx - 62;
    int ly = my - 6;
    if (ly < 30) ly = 30;
    if (ly > 196) ly = 196;
    char dm[16];
    snprintf(dm, sizeof(dm), "%.0fm", planeLiveDistKm(pi) * 1000.0f);
    txtAt(s_planes[pi].call, lx, ly, C_WHITE);
    if (s_planes[pi].typ[0]) txtAt(s_planes[pi].typ, lx, ly + 12, C_YELLOW);
    txtAt(dm, lx, ly + 24, C_CYAN);
  }

  // header: Chinese location name + range/count
  useFont(u8g2_font_unifont_t_chinese4);
  txtC(s_city, 12, C_YELLOW);
  useFont(u8g2_font_helvB08_tr);
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "%.0fkm N%d", R, s_planeCount);
  txtC(hdr, 27, C_GREEN);
}

static void drawWedge(float sweep, bool erase) {
  // Solid gradient fan: one filled pie slice per gradient step.
  for (int k = 0; k < WEDGE_N; k++) {
    float a1 = (sweep - k * WEDGE_STEP) * PI / 180;
    float a2 = (sweep - (k + 1) * WEDGE_STEP) * PI / 180;
    int x1 = R_CX + (int)(sin(a1) * (R_R - 6));
    int y1 = R_CY - (int)(cos(a1) * (R_R - 6));
    int x2 = R_CX + (int)(sin(a2) * (R_R - 6));
    int y2 = R_CY - (int)(cos(a2) * (R_R - 6));
    gfx->fillTriangle(R_CX, R_CY, x1, y1, x2, y2,
                      erase ? C_BLACK : WEDGE_COL[k]);
  }
  gfx->fillCircle(R_CX, R_CY, 3, erase ? C_BLACK : C_WHITE);
}

static void drawRadarBase() {
  gfx->fillScreen(C_BLACK);
  drawRings();
  computeMapped();
  drawMarkers();
  drawLabels();
  drawWedge(s_sweep, false);
  drawCardinals();
}

// Full redraw (page entry / after plane fetch).
static void drawPlanes(float sweep) {
  (void)sweep;
  drawRadarBase();
}
