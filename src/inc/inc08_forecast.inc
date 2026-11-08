// ---------------- forecast ----------------
// Temperature print helper. The Helvetica subsets carry no degree-sign glyph at
// all, which is why earlier builds could only show "24C". The degree mark is
// therefore drawn as a tiny filled circle instead of a font character.
static int txtTemp8(float v, int x, int y, uint16_t color) {
  char n[16];
  snprintf(n, sizeof(n), "%.0f", v);
  useFont(u8g2_font_helvB08_tr);
  int w = textW(n);
  txtAt(n, x, y, color);
  gfx->fillCircle(x + w + 2, y + 2, 1, color);
  txtAt("C", x + w + 5, y, color);
  return w + 5 + textW("C");
}

static void txtTemp10(float v, int x, int y, uint16_t color) {
  char n[16];
  snprintf(n, sizeof(n), "%.0f", v);
  useFont(u8g2_font_helvB10_tr);
  int w = textW(n);
  txtAt(n, x, y, color);
  gfx->fillCircle(x + w + 2, y + 2, 2, color);
  txtAt("C", x + w + 6, y, color);
}

// Rows: icon+desc 32/58, divider 72, day columns 84..144, divider 158, city 170,
// temp/rain/hum 184, wind level 200, alert (or UPD clock) 214.
static void renderForecast() {
  gfx->fillScreen(C_BLACK);
  gfx->drawCircle(R_CX, R_CY, R_R, C_DKGREEN);

  if (s_metOk) {
    // current weather icon + description (center)
    drawWxIcon(120, 32, s_code, 11);
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(wmoZh(s_code), 58, C_WHITE);
    // current temp (green) and today's high (yellow) on the right
    txtTemp10(s_temp, 168, 28, C_GREEN);
    useFont(u8g2_font_helvB10_tr);
    txtAt("/", 168, 46, C_YELLOW);
    txtTemp10(s_days[0].tmax, 168 + textW("/"), 46, C_YELLOW);
  } else {
    useFont(u8g2_font_helvB10_tr);
    txtC("LOADING...", 50, C_GRAY);
  }

  gfx->drawLine(30, 72, 210, 72, C_DKGREEN);

  // 3 columns
  static const char *DL[3] = {"明天", "后天", "大后天"};
  int colX[3] = {48, 120, 192};
  useFont(u8g2_font_unifont_t_chinese4);
  for (int i = 0; i < 3; i++) {
    int w = textW(DL[i]);
    txtAt(DL[i], colX[i] - w / 2, 84, C_CYAN);
  }
  if (s_metOk) {
    for (int i = 0; i < 3; i++) {
      drawWxIcon(colX[i], 104, s_days[i].code, 8);
      useFont(u8g2_font_helvB08_tr);
      char tt[24];
      snprintf(tt, sizeof(tt), "%.0f/%.0f", s_days[i].tmin, s_days[i].tmax);
      int w = textW(tt) + 8; // room for the degree mark
      int x0 = colX[i] - w / 2;
      txtAt(tt, x0, 126, C_WHITE);
      gfx->fillCircle(x0 + textW(tt) + 2, 128, 1, C_WHITE);
      txtAt("C", x0 + textW(tt) + 5, 126, C_WHITE);
      useFont(u8g2_font_unifont_t_chinese4);
      int zw = textW(wmoZh(s_days[i].code));
      txtAt(wmoZh(s_days[i].code), colX[i] - zw / 2, 144, C_GREEN);
    }
  }

  gfx->drawLine(30, 158, 210, 158, C_DKGREEN);

  if (s_metOk) {
    // Chinese city name, bottom center
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(s_city.c_str(), 170, C_YELLOW);

    // three items: current temp (left), rain now (center), humidity (right)
    const char *rain = (s_prec >= 0.1f)
                           ? "有雨"
                           : ((s_code >= 51 && s_code <= 99) ? wmoZh(s_code)
                                                             : "无雨");
    char hb[16];
    snprintf(hb, sizeof(hb), "%.0f%%", s_hum);
    txtTemp8(s_temp, 44, 184, C_WHITE);
    useFont(u8g2_font_unifont_t_chinese4);
    txtAt(rain, 120 - textW(rain) / 2, 184, C_WHITE);
    useFont(u8g2_font_helvB08_tr);
    txtAt(hb, 190 - textW(hb) / 2, 184, C_YELLOW);

    // Wind level (Beaufort), with the direction when the unit knows it.
    char wb[40];
    if (s_windFrom >= 0)
      snprintf(wb, sizeof(wb), "%s风 %d级", windDirZh(s_windFrom), s_windLvl);
    else
      snprintf(wb, sizeof(wb), "风 %d级", s_windLvl);
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(wb, 200, C_CYAN);

    // Threshold alert wins the last line; otherwise show the update clock.
    if (s_alert.length()) {
      txtC(s_alert.c_str(), 214, C_RED);
    } else if (s_timeOk) {
      char u[24];
      snprintf(u, sizeof(u), "UPD %02d:%02d", s_tm.tm_hour, s_tm.tm_min);
      useFont(u8g2_font_helvB08_tr);
      txtAt(u, 120 - textW(u) / 2, 214, C_GRAY);
    }
  }
}
