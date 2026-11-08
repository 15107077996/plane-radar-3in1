// ---------------- clock ----------------
static void drawHands(int hour, int minute, int second) {
  float hh = (hour % 12) + minute / 60.0f;
  float aH = hh * 30 * PI / 180;
  float aM = minute * 6 * PI / 180;
  float aS = second * 6 * PI / 180;
  int hx = R_CX + (int)(sin(aH) * 46), hy = R_CY - (int)(cos(aH) * 46);
  int mx = R_CX + (int)(sin(aM) * 70), my = R_CY - (int)(cos(aM) * 70);
  gfx->drawLine(R_CX, R_CY, hx, hy, C_WHITE);
  gfx->drawLine(R_CX, R_CY, mx, my, C_WHITE);
  int sx = R_CX + (int)(sin(aS) * 84), sy = R_CY - (int)(cos(aS) * 84);
  gfx->drawLine(R_CX, R_CY, sx, sy, C_RED);
}

// ---------------- sunrise / sunset / moon ----------------
// NOAA solar position (spreadsheet form). Local clock hours, no network needed.
// Verified against Nankang 2026-11-08: sunrise 06:33, sunset 17:36.
// NOTE: never name a variable "local" here - zlib (pulled in by PNGdec) defines
// `local` as a macro, which rewrites the declaration into nonsense.
static float s_sunRise = -1.0f, s_sunSet = -1.0f;
static char s_moonName[8] = "";
static int s_moonAge = 0;
static int s_astroDay = -1;

static bool sunTimes(float lat, float lon, int yday, float &riseH, float &setH) {
  float lngHour = lon / 15.0f;
  for (int pass = 0; pass < 2; pass++) {
    float T = (float)yday + ((pass == 0 ? 6.0f : 18.0f) - lngHour) / 24.0f;
    float M = (0.9856f * T) - 3.289f;
    float L = M + (1.916f * sin((float)deg2rad(M))) +
              (0.020f * sin((float)deg2rad(2 * M))) + 282.634f;
    L = fmodf(L, 360.0f);
    if (L < 0) L += 360.0f;
    float RA = (float)rad2deg(atan(0.91764 * tan(deg2rad(L))));
    RA = fmodf(RA, 360.0f);
    if (RA < 0) RA += 360.0f;
    float Lq = floor(L / 90.0f) * 90.0f;
    float RAq = floor(RA / 90.0f) * 90.0f;
    RA = (RA + (Lq - RAq)) / 15.0f;
    float decl = (float)rad2deg(asin(0.39771 * sin(deg2rad(L))));
    float cosH = ((float)cos(deg2rad(90.833)) -
                  sin(deg2rad(lat)) * sin(deg2rad(decl))) /
                 (cos(deg2rad(lat)) * cos(deg2rad(decl)));
    if (cosH > 1.0f || cosH < -1.0f) return false; // polar day / night
    float H = (float)rad2deg(acos(cosH)) / 15.0f;
    float T2 = RA + (pass == 0 ? -H : H) - (0.06571f * T) - 6.622f;
    float clk = fmodf(T2 - lngHour + 8.0f, 24.0f); // CST-8 clock hours
    if (clk < 0) clk += 24.0f;
    if (pass == 0) riseH = clk;
    else setH = clk;
  }
  return true;
}

static void ensureAstro() {
  if (!s_timeOk) return;
  int key = (s_tm.tm_year + 1900) * 400 + s_tm.tm_yday;
  if (key == s_astroDay) return;
  s_astroDay = key;
  s_sunRise = -1.0f;
  s_sunSet = -1.0f;
  sunTimes(s_lat, s_lon, s_tm.tm_yday + 1, s_sunRise, s_sunSet);
  // Moon phase: synodic month since the 2000-01-06 18:14 UTC new moon.
  time_t nowUtc = time(nullptr);
  double days = ((double)nowUtc - 947182440.0) / 86400.0;
  double syn = 29.530588853;
  double ph = fmod(days, syn);
  if (ph < 0) ph += syn;
  ph /= syn;
  s_moonAge = (int)(ph * syn + 0.5);
  const char *nm;
  if (ph < 0.03 || ph >= 0.97) nm = "新月";
  else if (ph < 0.22) nm = "娥眉月";
  else if (ph < 0.28) nm = "上弦月";
  else if (ph < 0.47) nm = "盈凸月";
  else if (ph < 0.53) nm = "满月";
  else if (ph < 0.72) nm = "亏凸月";
  else if (ph < 0.78) nm = "下弦月";
  else nm = "残月";
  snprintf(s_moonName, sizeof(s_moonName), "%s", nm);
  Serial.printf("[astro] rise %.2f set %.2f moonAge %d\n", s_sunRise, s_sunSet,
                s_moonAge);
}

static void drawClockCenter() {
  // big digital time
  useFont(u8g2_font_logisoso24_tn);
  char dig[8];
  snprintf(dig, sizeof(dig), "%02d:%02d", s_tm.tm_hour, s_tm.tm_min);
  txtC(dig, 62, C_YELLOW);

  // date + weekday
  static const char *WD[7] = {"周日", "周一", "周二", "周三",
                              "周四", "周五", "周六"};
  useFont(u8g2_font_unifont_t_chinese4);
  char date[40];
  snprintf(date, sizeof(date), "%02d-%02d %s", s_tm.tm_mon + 1, s_tm.tm_mday,
           WD[s_tm.tm_wday]);
  txtC(date, 94, C_WHITE);

  // city (Chinese)
  txtC(s_city, 114, C_ORANGE);

  // current weather in Chinese
  if (s_metOk) txtC(wmoZh(s_code), 132, C_CYAN);

  // temperature row
  useFont(u8g2_font_helvB08_tr);
  if (s_metOk) {
    char b[40];
    snprintf(b, sizeof(b), "%.0fC hum%.0f%%", s_temp, s_hum);
    txtC(b, 148, C_GREEN);
  }

  // sunrise / sunset, one row each: a single combined line overflows the circle
  useFont(u8g2_font_unifont_t_chinese4);
  if (s_sunRise >= 0) {
    int rh = (int)s_sunRise, rm = (int)((s_sunRise - rh) * 60);
    int sh = (int)s_sunSet, sm = (int)((s_sunSet - sh) * 60);
    char sr[24], ss[24];
    snprintf(sr, sizeof(sr), "日出 %02d:%02d", rh, rm);
    snprintf(ss, sizeof(ss), "日落 %02d:%02d", sh, sm);
    txtC(sr, 166, C_CYAN);
    txtC(ss, 182, C_CYAN);
  }

  // moon phase
  if (s_moonName[0]) {
    char mb[24];
    snprintf(mb, sizeof(mb), "%s %d天", s_moonName, s_moonAge);
    txtC(mb, 198, C_GRAY);
  }
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
    ensureAstro();
    drawClockCenter();
    drawHands(s_tm.tm_hour, s_tm.tm_min, s_tm.tm_sec);
  } else {
    useFont(u8g2_font_helvB10_tr);
    txtC("TIME SYNC...", 120, C_YELLOW);
  }
}

// Flicker-free per-second update: only the inner disc changes; the outer
// dial (ring, ticks, numbers) stays untouched.
static void updateClockHands() {
  gfx->fillCircle(R_CX, R_CY, 85, C_BLACK);
  drawClockCenter();
  drawHands(s_tm.tm_hour, s_tm.tm_min, s_tm.tm_sec);
}
