// ---------------- forecast ----------------
static void renderForecast() {
  gfx->fillScreen(C_BLACK);
  gfx->drawCircle(R_CX, R_CY, R_R, C_DKGREEN);

  if (s_metOk) {
    // current weather icon + description (center)
    drawWxIcon(120, 34, s_code, 11);
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(wmoZh(s_code), 62, C_WHITE);
    // current temp (green) and today's high (yellow) on the right
    useFont(u8g2_font_helvB10_tr);
    char t[24];
    snprintf(t, sizeof(t), "%.0fC", s_temp);
    txtAt(t, 168, 30, C_GREEN);
    char hi[24];
    snprintf(hi, sizeof(hi), "/%.0fC", s_days[0].tmax);
    txtAt(hi, 168, 48, C_YELLOW);
  } else {
    useFont(u8g2_font_helvB10_tr);
    txtC("LOADING...", 50, C_GRAY);
  }

  gfx->drawLine(30, 78, 210, 78, C_DKGREEN);

  // 3 columns
  static const char *DL[3] = {"明天", "后天", "大后天"};
  int colX[3] = {48, 120, 192};
  useFont(u8g2_font_unifont_t_chinese4);
  for (int i = 0; i < 3; i++) {
    int w = textW(DL[i]);
    txtAt(DL[i], colX[i] - w / 2, 90, C_CYAN);
  }
  if (s_metOk) {
    for (int i = 0; i < 3; i++) {
      drawWxIcon(colX[i], 110, s_days[i].code, 8);
      useFont(u8g2_font_helvB08_tr);
      char tt[24];
      snprintf(tt, sizeof(tt), "%.0f/%.0f", s_days[i].tmin, s_days[i].tmax);
      int w = textW(tt);
      txtAt(tt, colX[i] - w / 2, 132, C_WHITE);
      useFont(u8g2_font_unifont_t_chinese4);
      int zw = textW(wmoZh(s_days[i].code));
      txtAt(wmoZh(s_days[i].code), colX[i] - zw / 2, 150, C_GREEN);
    }
  }

  gfx->drawLine(30, 164, 210, 164, C_DKGREEN);

  if (s_metOk) {
    // Chinese city name, bottom center
    useFont(u8g2_font_unifont_t_chinese4);
    txtC(s_city.c_str(), 176, C_YELLOW);

    // three bottom items: current temp (left), rain now (center), humidity (right)
    const char *rain = (s_prec >= 0.1f)
                           ? "有雨"
                           : ((s_code >= 51 && s_code <= 99) ? wmoZh(s_code)
                                                             : "无雨");
    char tb[16], hb[16];
    snprintf(tb, sizeof(tb), "%.0fC", s_temp);
    snprintf(hb, sizeof(hb), "%.0f%%", s_hum);
    useFont(u8g2_font_helvB08_tr);
    txtAt(tb, 52 - textW(tb) / 2, 196, C_WHITE);
    useFont(u8g2_font_unifont_t_chinese4);
    txtAt(rain, 120 - textW(rain) / 2, 196, C_WHITE);
    useFont(u8g2_font_helvB08_tr);
    txtAt(hb, 188 - textW(hb) / 2, 196, C_YELLOW);

    if (s_timeOk) {
      char u[24];
      snprintf(u, sizeof(u), "UPD %02d:%02d", s_tm.tm_hour, s_tm.tm_min);
      useFont(u8g2_font_helvB08_tr);
      txtAt(u, 120 - textW(u) / 2, 216, C_GRAY);
    }
  }
}
