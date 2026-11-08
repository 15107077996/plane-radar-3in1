// ---------------- Open-Meteo ----------------
static bool fetchOpenMeteo() {
  char url[420];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max&timezone=auto&forecast_days=4",
           s_lat, s_lon);
  HTTPClient http;
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
      }
      JsonArray dcode = doc["daily"]["weather_code"];
      JsonArray dmax = doc["daily"]["temperature_2m_max"];
      JsonArray dmin = doc["daily"]["temperature_2m_min"];
      JsonArray dpop = doc["daily"]["precipitation_probability_max"];
      for (int i = 0; i < 3; i++) {
        int di = i + 1;
        if (dcode.size() > di) {
          s_days[i].code = dcode[di] | 0;
          s_days[i].tmax = dmax[di] | 0;
          s_days[i].tmin = dmin[di] | 0;
          s_days[i].pop = dpop[di] | 0;
        }
      }
      s_metMs = millis();
      s_metOk = true;
      ok = true;
      Serial.printf("[om] %.1fC hum %.0f wind %.1f code %d\n",
                    s_temp, s_hum, s_wind, s_code);
    }
  } else {
    Serial.printf("[om] http %d\n", code);
  }
  http.end();
  return ok;
}
