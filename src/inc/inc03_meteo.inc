// ---------------- Open-Meteo ----------------
static bool fetchOpenMeteo() {
  unsigned long t0 = millis();
  char url[512];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,"
           "wind_speed_10m,precipitation,wind_gusts_10m,visibility,cloud_cover"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,sunrise,sunset"
           "&timezone=auto&forecast_days=4",
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
      }
      JsonObject daily = doc["daily"];
      JsonArray dcode = daily["weather_code"];
      JsonArray dmax = daily["temperature_2m_max"];
      JsonArray dmin = daily["temperature_2m_min"];
      JsonArray dpop = daily["precipitation_probability_max"];
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
      s_metMs = millis();
      s_metOk = true;
      ok = true;
      Serial.printf("[om] %.1fC hum %.0f wind %.1f code %d prec %.1f\n", s_temp,
                    s_hum, s_wind, s_code, s_prec);
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
