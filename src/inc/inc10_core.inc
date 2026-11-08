// ---------------- page entry ----------------
static void enterPage(int page) {
  s_screen = page;
  if (page == SCR_CLOCK) {
    renderClock();
  } else if (page == SCR_PLANES) {
    drawPlanes(s_sweep);
  } else if (page == SCR_FORECAST) {
    renderForecast();
  } else if (page == SCR_RAIN) {
    rainRefresh();
  }
  // Drop any press edge that accumulated while the (possibly blocking) page built.
  buttonResync();
}

// ---------------- button ----------------
#define DEBOUNCE_MS 20
#define HOLD_MS 750
static bool s_rawPrev = true;        // last raw level (true = released)
static bool s_stable = true;         // debounced level
static unsigned long s_lastChange = 0;
static unsigned long s_pressTime = 0;
static bool s_longFired = false;
static bool s_suppressRelease = false; // swallow the release that ends a long-press

static void buttonResync() {
  bool raw = digitalRead(PR_KEY) == HIGH;
  s_rawPrev = raw;
  s_stable = raw;
  s_lastChange = millis();
  // Reset the hold timer so a key still held after a blocking build does not
  // instantly re-fire the long-press on the next loop.
  s_pressTime = millis();
  s_longFired = false;
}

static void handleButton() {
  bool raw = digitalRead(PR_KEY) == HIGH; // HIGH = released, LOW = pressed
  unsigned long now = millis();
  if (raw != s_rawPrev) { s_lastChange = now; s_rawPrev = raw; }
  if (now - s_lastChange >= DEBOUNCE_MS && raw != s_stable) {
    s_stable = raw;
    if (!raw) { // confirmed press
      s_pressTime = now;
      s_longFired = false;
      s_suppressRelease = false;
    } else {    // confirmed release
      if (s_suppressRelease) {
        // This release belongs to a long-press: swallow it, no page change.
        s_suppressRelease = false;
      } else if (!s_longFired) {
        int next = (s_screen + 1) % 4;
        Serial.printf("[key] short -> page %d\n", next);
        enterPage(next);
      }
    }
  }
  if (!s_stable && !s_longFired && now - s_pressTime >= HOLD_MS) {
    s_longFired = true;
    s_suppressRelease = true;
    s_rangeAuto = false; // manual range choice beats the boot auto-fit
    s_rangeIdx = (s_rangeIdx + 1) % RANGE_N;
    Serial.printf("[key] long range -> %.0fkm\n", rangeKm());
    enterPage(SCR_PLANES);
  }
}

// ---------------- ticks ----------------
static void tickClock() {
  if (!s_timeOk) return;
  // Refresh the system time every loop so the second hand/digits actually move.
  getLocalTime(&s_tm, 100);
  if (s_tm.tm_sec != s_lastSec) {
    s_lastSec = s_tm.tm_sec;
    updateClockHands();
  }
}
static void tickPlanes(unsigned long now) {
  if (now - s_sweepMs >= 100) {
    s_sweepMs = now;
    // Erase the old wedge, restore static content, then draw the new wedge.
    // No full-screen fill -> no flicker.
    drawWedge(s_sweep, true);
    drawRings();
    computeMapped(); // dead-reckon live positions so aircraft glide smoothly
    drawMarkers();
    drawLabels();
    s_sweep += 6;
    if (s_sweep >= 360) s_sweep -= 360;
    drawWedge(s_sweep, false);
    drawCardinals(); // repaint the compass letters the sweep just covered
  }
  if (now - s_planeMs >= 60000 || s_planeCount == 0) {
    if (fetchPlanes()) drawRadarBase();
  }
}
static void tickForecast(unsigned long now) {
  if ((!s_metOk && now - s_metTryMs >= 30000) ||
      (s_metOk && now - s_metMs >= 600000)) {
    s_metTryMs = now;
    fetchOpenMeteo();
  }
}
static void tickRain(unsigned long now) {
  if ((!s_rainOk && now - s_rainTryMs >= 30000) ||
      (s_rainOk && now - s_rainMs >= 600000)) {
    s_rainTryMs = now;
    rainRefresh();
  }
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("PlaneRadar %s\n", FW_VER);

  pinMode(PR_KEY, INPUT_PULLUP);

  s_bus = new Arduino_ESP32SPI(PR_DC, PR_CS, PR_SCLK, PR_MOSI);
  gfx = new Arduino_GC9A01(s_bus, PR_RST, 0, true);
  gfx->begin(PR_SPI_FREQ);
  gfx->setUTF8Print(true);
  gfx->fillScreen(C_BLACK);

  // PNG buffer is no longer held permanently; it is allocated on demand.
  s_prefs.begin("radar", false);
  int ver = s_prefs.getInt("ver", 0);
  s_ssid = s_prefs.getString("ssid", "");
  s_pass = s_prefs.getString("pass", "");
  s_manual = s_prefs.getBool("manual", true);
  s_lat = s_prefs.getFloat("lat", HOME_LAT);
  s_lon = s_prefs.getFloat("lon", HOME_LON);
  s_city = s_prefs.getString("city", HOME_CITY);

  // Migration from older config: keep WiFi credentials, but force the home
  // location to Nankang and switch to manual so the wrong IP result is dropped.
  if (ver != CFG_VER) {
    s_manual = true;
    s_lat = HOME_LAT;
    s_lon = HOME_LON;
    s_city = HOME_CITY;
    saveCfg();
    Serial.printf("[cfg] migrate %d->%d home=%s\n", ver, CFG_VER, s_city.c_str());
  }
  // Valid as long as WiFi credentials exist; version mismatch is migrated above.
  s_cfgValid = (s_ssid.length() > 0);

  if (!s_cfgValid) {
    startPortal();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(s_ssid.c_str(), s_pass.c_str());
  useFont(u8g2_font_helvB10_tr);
  txtC("WIFI...", 110, C_YELLOW);
  int wtry = 0;
  while (WiFi.status() != WL_CONNECTED && wtry < 40) {
    delay(500);
    wtry++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    txtC("WIFI FAIL", 130, C_RED);
    delay(2000);
    startPortal();
    return;
  }
  Serial.printf("[wifi] %s rssi=%d\n", WiFi.localIP().toString().c_str(),
                WiFi.RSSI());

  if (s_manual) {
    Serial.printf("[loc] manual home %s %.4f %.4f (skip IP)\n",
                  s_city.c_str(), s_lat, s_lon);
  } else {
    locateByIp();
  }
  syncTime();
  fetchOpenMeteo();
  fetchPlanes();

  s_screen = SCR_CLOCK;
  renderClock();
  buttonResync();
}

// ---------------- loop ----------------
void loop() {
  unsigned long now = millis();
  handleButton();
  switch (s_screen) {
    case SCR_CLOCK: tickClock(); break;
    case SCR_PLANES: tickPlanes(now); break;
    case SCR_FORECAST: tickForecast(now); break;
    case SCR_RAIN: tickRain(now); break;
  }
  delay(20);
}
