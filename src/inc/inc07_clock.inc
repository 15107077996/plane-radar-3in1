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

static void drawClockCenter() {
  // big digital time
  useFont(u8g2_font_logisoso24_tn);
  char dig[8];
  snprintf(dig, sizeof(dig), "%02d:%02d", s_tm.tm_hour, s_tm.tm_min);
  txtC(dig, 66, C_YELLOW);

  // date + weekday
  static const char *WD[7] = {"周日", "周一", "周二", "周三",
                              "周四", "周五", "周六"};
  useFont(u8g2_font_unifont_t_chinese4);
  char date[40];
  snprintf(date, sizeof(date), "%04d-%02d-%02d %s",
           s_tm.tm_year + 1900, s_tm.tm_mon + 1, s_tm.tm_mday, WD[s_tm.tm_wday]);
  txtC(date, 98, C_WHITE);

  // city (Chinese)
  useFont(u8g2_font_unifont_t_chinese4);
  txtC(s_city, 120, C_ORANGE);

  // weather chinese
  useFont(u8g2_font_unifont_t_chinese4);
  if (s_metOk) txtC(wmoZh(s_code), 140, C_CYAN);

  // bottom row
  useFont(u8g2_font_helvB08_tr);
  if (s_metOk) {
    char b[48];
    snprintf(b, sizeof(b), "%.0fC hum%.0f%% %s",
             s_temp, s_hum, wmoZh(s_code));
    txtC(b, 176, C_GREEN);
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

