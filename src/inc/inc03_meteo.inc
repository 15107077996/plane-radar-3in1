// ---------------- Open-Meteo ----------------
// Wind level and local alert state. Open-Meteo publishes no official warning
// feed, so the alert below is computed from its own numbers against fixed
// thresholds -- it is a heads-up, not a meteorological bureau warning.
static int s_windLvl = 0;   // Beaufort 0..12
static int s_windFrom = -1; // direction in degrees, -1 = unknown
static float s_gust = 0.0f; // gust km/h
static String s_alert = ""; // empty when nothing to flag

static int beaufort(float kmh) {
  static const float lim[12] = {1, 5, 11, 19, 28, 38, 49, 61, 74, 88, 102, 117};
  int l = 0;
  while (l < 12 && kmh > lim[l]) l++;
  return l;
}

static const char *windDirZh(int deg) {
  static const char *D[8] = {"北", "东北", "东", "东南",
                             "南", "西南", "西", "西北"};
  if (deg < 0) return "";
  return D[((deg + 22) / 45) % 8];
}

static void buildAlert(float gmax, float psum, int pop) {
  s_alert = "";
  if (s_code >= 95) {
    s_alert = "雷电预警";
  } else if (psum >= 25.0f || (s_code >= 63 && psum >= 10.0f)) {
    s_alert = "强降雨预警";
  } else if (gmax >= 39.0f || s_windLvl >= 6) {
    s_alert = "大风预警";
  } else if (s_code >= 45 && s_code <= 48) {
    s_alert = "大雾预警";
  } else if (s_temp >= 35.0f) {
    s_alert = "高温预警";
  } else if (s_temp <= 0.0f) {
    s_alert = "低温预警";
  } else if (pop >= 80 && s_code >= 51) {
    s_alert = "降雨提醒";
  }
  Serial.printf("[alert] '%s' lvl=%d gust=%.1f psum=%.1f pop=%d\n",
                s_alert.c_str(), s_windLvl, gmax, psum, pop);
}

static bool fetchOpenMeteo() {
  unsigned long t0 = millis();
  char url[512];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,"
           "wind_speed_10m,wind_direction_10m,wind_gusts_10m,precipitation"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,precipitation_sum,"
           "wind_gusts_10m_max&timezone=auto&forecast_days=4",
           s_lat, s_lon);
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      JsonObject cur = doc["current"];
      if (!cur.isNull()) {
        s_temp = cur["temperature_2m"] | s_temp;
        s_hum = cur["relative_humidity_2m"] | s_hum;
        s_wind = cur["wind_speed_10m"] | s_wind;
        s_code = cur["weather_code"] | s_code;
        s_prec = cur["precipitation"] | 0.0f;
        s_windFrom = cur["wind_direction_10m"] | -1;
        s_gust = cur["wind_gusts_10m"] | 0.0f;
        s_windLvl = beaufort(s_wind);
      }
      JsonObject daily = doc["daily"];
      JsonArray dcode = daily["weather_code"];
      JsonArray dmax = daily["temperature_2m_max"];
      JsonArray dmin = daily["temperature_2m_min"];
      JsonArray dpop = daily["precipitation_probability_max"];
      JsonArray dsum = daily["precipitation_sum"];
      JsonArray dgust = daily["wind_gusts_10m_max"];
      int dn = (int)dmax.size();
      // NOTE: temperatures are floats in the payload, so the default value must
      // be a float. Writing "| 0" makes ArduinoJson return the int default 0
      // for every fractional value, which is why the columns showed 0/0.
      for (int i = 0; i < 3; i++) {
        int di = i + 1;
        if (di < dn) {
          s_days[i].code = dcode[di] | 0;
          s_days[i].tmax = dmax[di] | -99.0f;
          s_days[i].tmin = dmin[di] | -99.0f;
          s_days[i].pop = dpop[di] | 0;
        }
      }
      float gmax = dgust[1] | 0.0f;
      float psum = dsum[1] | 0.0f;
      int pop0 = dpop[1] | 0;
      buildAlert(gmax, psum, pop0);
      s_metMs = millis();
      s_metOk = true;
      ok = true;
      Serial.printf("[om] %.1fC hum %.0f wind %.1f dir %d gust %.1f lvl %d code %d prec %.1f\n",
                    s_temp, s_hum, s_wind, s_windFrom, s_gust, s_windLvl, s_code,
                    s_prec);
      Serial.printf("[om] days n=%d %.0f/%.0f %.0f/%.0f %.0f/%.0f\n", dn,
                    s_days[0].tmin, s_days[0].tmax, s_days[1].tmin,
                    s_days[1].tmax, s_days[2].tmin, s_days[2].tmax);
    }
  } else {
    Serial.printf("[om] http %d\n", code);
  }
  http.end();
  s_metCostMs = millis() - t0;
  return ok;
}
