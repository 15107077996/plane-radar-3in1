// ---------------- weather labels ----------------
static const char *wmoZh(int code) {
  if (code == 0) return "晴";
  if (code == 1 || code == 2) return "多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code >= 51 && code <= 55) return "毛毛雨";
  if (code == 56 || code == 57 || code == 66 || code == 67) return "冻雨";
  if (code == 61) return "小雨";
  if (code == 63) return "中雨";
  if (code == 65) return "大雨";
  if (code >= 71 && code <= 77) return "雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code >= 95) return "雷阵雨";
  return "多云";
}

enum { IC_SUN, IC_PART, IC_CLOUD, IC_FOG, IC_DRIZZLE, IC_RAIN, IC_SNOW, IC_THUNDER };
static int iconKind(int code) {
  if (code == 0) return IC_SUN;
  if (code == 1 || code == 2) return IC_PART;
  if (code == 3) return IC_CLOUD;
  if (code == 45 || code == 48) return IC_FOG;
  if (code >= 51 && code <= 57) return IC_DRIZZLE;
  if (code >= 95) return IC_THUNDER;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return IC_SNOW;
  if (code >= 61) return IC_RAIN;
  return IC_PART;
}

static void fillCloud(int cx, int cy, int s, uint16_t col) {
  gfx->fillCircle(cx - s, cy + s / 2, s, col);
  gfx->fillCircle(cx, cy, s + 1, col);
  gfx->fillCircle(cx + s, cy + s / 3, s - 1, col);
  gfx->fillRect(cx - s, cy + s / 2, 2 * s, s, col);
}

static void drawWxIcon(int cx, int cy, int code, int s) {
  int kind = iconKind(code);
  if (kind == IC_SUN || kind == IC_PART) {
    gfx->fillCircle(cx, cy, s, C_YELLOW);
    for (int a = 0; a < 360; a += 45) {
      float r = a * PI / 180;
      int x1 = cx + (int)(cos(r) * (s + 2));
      int y1 = cy + (int)(sin(r) * (s + 2));
      int x2 = cx + (int)(cos(r) * (s + 6));
      int y2 = cy + (int)(sin(r) * (s + 6));
      gfx->drawLine(x1, y1, x2, y2, C_YELLOW);
    }
  }
  if (kind == IC_PART) {
    gfx->fillCircle(cx + s, cy + s / 2, s, C_BLACK);
    fillCloud(cx + s / 2, cy + s, s - 1, C_WHITE);
  } else if (kind == IC_CLOUD) {
    fillCloud(cx, cy, s, C_WHITE);
  } else if (kind == IC_FOG) {
    fillCloud(cx, cy - s / 2, s - 1, C_GRAY);
    for (int i = 0; i < 3; i++)
      gfx->drawLine(cx - s, cy + s + i * 4, cx + s, cy + s + i * 4, C_GRAY);
  } else if (kind == IC_DRIZZLE || kind == IC_RAIN) {
    fillCloud(cx, cy - s / 2, s, C_WHITE);
    int n = (kind == IC_RAIN) ? 3 : 2;
    for (int i = 0; i < n; i++) {
      int x = cx - s + i * (2 * s / (n > 1 ? n - 1 : 1));
      gfx->drawLine(x, cy + s, x - 2, cy + s + 6, C_BLUE);
    }
  } else if (kind == IC_SNOW) {
    fillCloud(cx, cy - s / 2, s, C_WHITE);
    for (int i = 0; i < 3; i++) {
      int x = cx - s + i * s;
      gfx->fillCircle(x, cy + s + 4, 2, C_WHITE);
    }
  } else if (kind == IC_THUNDER) {
    fillCloud(cx, cy - s / 2, s, C_GRAY);
    gfx->fillTriangle(cx, cy + s, cx + 6, cy + s, cx - 2, cy + s + 10, C_YELLOW);
    gfx->fillTriangle(cx - 2, cy + s + 10, cx + 4, cy + s + 10, cx, cy + s + 16, C_YELLOW);
  }
}

