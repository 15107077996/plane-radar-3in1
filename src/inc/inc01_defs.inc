#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <SPIFFS.h>
#include <time.h>
#include <PNGdec.h>

// ---------------- pins (locked, do not change) ----------------
#ifndef PR_RST
#define PR_RST 0
#endif
#ifndef PR_CS
#define PR_CS 1
#endif
#ifndef PR_DC
#define PR_DC 10
#endif
#ifndef PR_MOSI
#define PR_MOSI 3
#endif
#ifndef PR_SCLK
#define PR_SCLK 4
#endif
#ifndef PR_KEY
#define PR_KEY 9
#endif
#ifndef PR_SPI_FREQ
#define PR_SPI_FREQ 40000000
#endif

// ---------------- geometry --------------
#define SCR_W 240
#define SCR_H 240
#define R_CX 120
#define R_CY 120
#define R_R 112

// ---------------- colors --------------
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GREEN 0x07E0
#define C_DKGREEN 0x0320
#define C_DKRED 0x3000
#define C_RED 0xF800
#define C_YELLOW 0xFFE0
#define C_CYAN 0x07FF
#define C_GRAY 0x8410
#define C_ORANGE 0xFD20
#define C_BLUE 0x041F
#define C_MAGENTA 0xF81F
#define C_DKBLUE 0x0210

// ---------------- version ----------------
#define FW_VER "1.3.1-c3"
#define CFG_VER 107
// Manual home location: Ganzhou Nankang District (district govt 25.6614N 114.7650E).
// IP geolocation on this network wrongly resolves to Beijing, so manual mode is default.
#define HOME_LAT 25.6614f
#define HOME_LON 114.7650f
#define HOME_CITY "南康区"
#define WX_ZOOM 7
#define FETCH_NM 250.0
#define MAX_PLANES 40
#define MAX_LABELS 3
// Upper bound only: the buffer is allocated to the exact tile Content-Length,
// so the real footprint is ~17KB instead of this figure.
#define PNG_BUF_CAP 65536
#define PNG_BUF_MIN 16384
#define RAIN_TILES_MAX 4
#define RAIN_FRAMES 3

// ---------------- screens ----------------
// Clock, planes, forecast, rain, self-check/about. Short press cycles all five.
enum { SCR_CLOCK = 0, SCR_PLANES = 1, SCR_FORECAST = 2, SCR_RAIN = 3, SCR_ABOUT = 4 };
#define SCR_N 5

// ---------------- ranges (7 steps) ----------------
// ADS-B coverage around Nankang is sparse: the nearest aircraft is often 200km+
// away, so the top step reaches 400km. The default follows the traffic.
#define RANGE_N 7
static const float RANGES_KM[RANGE_N] = {10, 25, 50, 100, 200, 300, 400};
static int s_rangeIdx = 6;       // boot default 400km; auto-fit narrows it later
static bool s_rangeAuto = true;  // cleared once the user long-presses for a range
static float rangeKm() { return RANGES_KM[s_rangeIdx]; }
static int s_screen = SCR_CLOCK;

// ---------------- display ----------------
static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX *gfx = nullptr;
static PNG s_png;

// ---------------- config ----------------
static Preferences s_prefs;
static String s_ssid;
static String s_pass;
static String s_city = HOME_CITY;
static float s_lat = HOME_LAT;
static float s_lon = HOME_LON;
static bool s_manual = true;   // true = fixed home location, skip IP locate
static bool s_cfgValid = false;
static WebServer *s_srv = nullptr;
static int s_rainSrc = 0;       // 0 = RainViewer radar mosaic, 1 = satellite IR cloud
static bool s_fsOk = false;     // SPIFFS mounted, rain page can cache frames

// ---------------- time ----------------
static struct tm s_tm;
static bool s_timeOk = false;
static int s_lastSec = -1;

// ---------------- planes ----------------
struct Plane {
  char call[12];
  char typ[8];
  double lat;          // last network-reported position (dead-reckoning base)
  double lon;
  int alt;
  int head;            // track, degrees
  float gs;            // ground speed, knots
  unsigned long baseMs; // time of the network fix
  float distNm;
  float distM;
  float bear;
};
static Plane s_planes[MAX_PLANES];
static int s_planeCount = 0;
static unsigned long s_planeMs = 0;
static float s_sweep = 0;
static unsigned long s_sweepMs = 0;
static unsigned long s_pageEnterMs = 0;

// ---------------- weather ----------------
struct DayWx {
  int code;
  float tmax;
  float tmin;
  int pop;
};
static float s_temp = 0, s_hum = 0, s_wind = 0, s_prec = 0;
static int s_code = 0;
static DayWx s_days[3];
static unsigned long s_metMs = 0;
static unsigned long s_metTryMs = 0;
static bool s_metOk = false;
static unsigned long s_metCostMs = 0;   // duration of the last Open-Meteo fetch

// ---------------- rain / png ----------------
static uint8_t *s_pngData = nullptr;
static size_t s_pngLen = 0;
static size_t s_pngCap = 0; // bytes actually allocated (exact tile size)
static int s_pngMode = 0; // 0 = opaque base, 1 = rain overlay
static int s_wxRows = 0;
static int s_rainPx = 0;
static int s_rainPxMax = 0;
static unsigned long s_rainMs = 0;
static int s_tileX = 0, s_tileY = 0;
static int s_tileDx = 0, s_tileDy = 0;
static String s_rainPath = "";
static bool s_rainOk = false;
static unsigned long s_rainTryMs = 0;
static unsigned long s_rainCostMs = 0;  // duration of the last rain refresh
struct RainTile { int tx; int ty; int dx; int dy; };
static RainTile s_tiles[RAIN_TILES_MAX];
static int s_tileN = 0;
static String s_framePath[RAIN_FRAMES];
static int s_frameN = 0;
static bool s_animReady = false;
static int s_animFrame = 0;
static unsigned long s_animMs = 0;

// ---------------- forward --------------
static void renderClock();
static void drawPlanes(float sweep);
static void drawCardinals();
static void renderForecast();
static void drawAbout();
static bool rainRefresh();
static void rainAnimTick(unsigned long now);
static void enterPage(int page);
static void handleButton();
static void buttonResync();
static bool fetchOpenMeteo();
static void drawWxIcon(int cx, int cy, int code, int s);
static const char *wmoZh(int code);
static void startWeb();
static void webLoop();
