/*
 * UniFi_Traffic_Monitor.ino
 *
 * Displays real-time internet traffic (IN / OUT Mbps) from a Ubiquiti UCG-MAX
 * on a 128x64 SH1106 OLED over I2C, with a built-in web dashboard.
 *
 * Target hardware : ESP32 (any variant)
 * Display         : SH1106 128x64 I2C (0x3C)
 * Controller      : Ubiquiti UCG-MAX (UniFi OS 5.x / Network 10.x)
 *
 * Required libraries (install via Arduino Library Manager):
 *   - U8g2            by oliver       (display driver)
 *   - ArduinoJson     by bblanchon    (JSON parsing)
 *   - WiFiClientSecure & HTTPClient   (bundled with ESP32 board package)
 *
 * Configuration: edit config.h
 *
 * Auth strategy:
 *   Uses the UniFi OS API key (X-API-KEY header) - no login, no cookies,
 *   no CSRF tokens, no session expiry to manage.
 *   Generate a key in UniFi OS -> Settings -> API Keys.
 *
 * Connection strategy:
 *   A single WiFiClientSecure + HTTPClient pair is kept alive across polls.
 *   HTTPClient::setReuse(true) suppresses a new TLS handshake on every GET,
 *   saving ~200-300 ms per poll and allowing a stable 1 s refresh rate.
 *   The connection is re-established automatically after any failure or
 *   after a WiFi drop.
 *
 * Dual-core operation:
 *   Core 0 (networkTask) - all HTTPS polling; never blocks Core 1.
 *   Core 1 (loop)        - OLED display, web server, LED, BOOT button.
 *   Shared state is protected by g_dataMutex (FreeRTOS mutex).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ---------------------------------------------------------------------------
// Display - SH1106 128x64, full-buffer, hardware I2C
// ---------------------------------------------------------------------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset */ U8X8_PIN_NONE);

// ---------------------------------------------------------------------------
// Persistent HTTP connection (reused across every poll)
// ---------------------------------------------------------------------------
static WiFiClientSecure g_secureClient;
static HTTPClient       g_http;
static bool             g_httpInitialised = false;
static WebServer        g_web(80);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int    g_fetchErrors    = 0;
static float  g_inMbps         = 0.0f;
static float  g_outMbps        = 0.0f;
static int    g_clients        = -1;
static float  g_wanUptimePct   = -1.0f;
static float  g_wanLatencyMs   = -1.0f;
static bool   g_wanDown        = false;
static bool   g_updateAvailable = false;
static String g_statusMsg      = "Connecting...";
static float  g_inGraphScale   = 0.1f;
static float  g_outGraphScale  = 0.1f;
static float  g_espCpuUtilPct  = -1.0f;
static float  g_espTempC       = -1.0f;
static float  g_ucgCpuUtilPct  = -1.0f;
static float  g_ucgMemUtilPct  = -1.0f;
static float  g_ucgTempC       = -1.0f;
static float  g_monthlyUsageGB = -1.0f;
static unsigned long g_lastEspStatsUpdateMs = 0;
static unsigned long g_lastUcgStatsUpdateMs = 0;

// ---------------------------------------------------------------------------
// FreeRTOS synchronisation (dual-core operation)
// ---------------------------------------------------------------------------
// g_dataMutex    - protects shared state written by Core 0 networkTask and
//                  read by Core 1 loop / drawDisplay / handleWebStats.
// g_lastFetchOk  - volatile; Core 1 reads the latest value without needing
//                  the mutex just to decide LED mode.
// g_networkReady - set by networkTask after the first poll attempt completes
//                  so Core 1 keeps LED_MODE_WORKING until there is real data.
// g_dataVersion  - incremented by networkTask each time new traffic data is
//                  committed; Core 1 uses it to skip redundant redraws.
static SemaphoreHandle_t     g_dataMutex   = NULL;
static volatile bool         g_lastFetchOk  = true;
static volatile bool         g_networkReady = false;
static volatile uint32_t     g_dataVersion  = 0;

enum LedMode {
  LED_MODE_WORKING,
  LED_MODE_WAN_DOWN,
  LED_MODE_OK,
  LED_MODE_ERROR
};

static LedMode g_ledMode = LED_MODE_WORKING;
static bool g_ledOn = false;
static bool g_ledPulseActive = false;
static unsigned long g_ledLastToggle = 0;
static unsigned long g_ledLastPulseStart = 0;

static bool g_bootLastRawPressed = false;
static bool g_bootStablePressed = false;
static unsigned long g_bootLastRawChangeMs = 0;
static unsigned long g_bootIpOverlayUntilMs = 0;
static String g_bootIpOverlayMsg = "";
static volatile unsigned long g_wanDownSinceMs = 0;  // millis() when WAN went down, 0 if up

static const int kBootButtonPin = 0;
static const bool kBootButtonActiveLow = true;
static const unsigned long kBootButtonDebounceMs = 30UL;
static const unsigned long kBootButtonIpShowMs = 5000UL;
static const unsigned long kControllerStatsPollMs  = 10000UL;
static const unsigned long kMonthlyUsagePollMs     = 300000UL; // 5 min
static const int kStatusLedPin = LED_BUILTIN;
static const bool kStatusLedActiveLow = false;
static const unsigned long kLedWorkingBlinkMs = 250UL;
static const unsigned long kLedWanDownBlinkMs = 70UL;
static const unsigned long kLedOkHeartbeatMs = 30000UL;
static const unsigned long kLedOkPulseMs = 120UL;
static constexpr float kRateBpsThresholdMbps = 0.001f;
static int g_ucgEndpointIdx = 0;  // rotates across resource-stats endpoints, one per cycle

// ---------------------------------------------------------------------------
// Waveform history  (one sample per horizontal pixel = 128 columns)
// ---------------------------------------------------------------------------
#define GRAPH_SAMPLES 128
#define WEB_HISTORY_POINTS 96

static float g_inHistory[GRAPH_SAMPLES]  = {0};
static float g_outHistory[GRAPH_SAMPLES] = {0};
static int   g_histIdx = 0;  // index where the NEXT sample will be written

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
bool  wifiConnect();
void  initHttpClient();
void  closeConnection();
bool  fetchTrafficStats();
bool  fetchUpdateAvailability(bool& updateAvailable);
bool  fetchJsonPayload(const String& url, String& payload, int& httpCode, int timeoutMs = 8000);
bool  keyLooksLikeUpdate(const char* key);
bool  keyLooksLikeAvailability(const char* key);
bool  keyLooksLikeInstalledVersion(const char* key);
bool  keyLooksLikeAvailableVersion(const char* key);
bool  valueLooksAvailable(JsonVariantConst value);
bool  extractVersionString(JsonVariantConst value, String& version);
int   compareVersionStrings(const String& lhs, const String& rhs);
bool  objectIndicatesUpdateAvailable(JsonObjectConst obj, bool allowLooseAvailableKeys);
bool  jsonContainsUpdateAvailable(JsonVariantConst node, bool parentIsUpdateKey = false, bool allowLooseAvailableKeys = false);
bool  parsePercentValue(JsonVariantConst value, float& pct);
bool  extractWanUptime24h(JsonObjectConst wanObj, float& pct);
bool  parseLatencyValue(JsonVariantConst value, float& latencyMs);
bool  extractWanLatencyMs(JsonObjectConst wanObj, float& latencyMs);
bool  isWanSubsysDown(JsonObjectConst wanObj);
bool  parseNumericValue(JsonVariantConst value, float& parsed);
bool  keyContainsAny(const String& key, const char* const* tokens, size_t tokenCount);
void  scanSystemMetrics(JsonVariantConst node, bool& gotCpu, bool& gotMem, bool& gotTemp, float& cpuPct, float& memPct, float& tempC);
bool  fetchControllerResourceStats(float& cpuPct, float& memPct, float& tempC);
bool  fetchMonthlyUsage();
float cToF(float c);
void  initWebServer();
void  handleWebRoot();
void  handleWebStats();
void  appendHistory(JsonArray arr, float* history, int points);
float historyPeakRecent(float* history, int samplesBack);
void  drawTrafficGraphInBox(float* history, int leftX, int rightX, int topY, int bottomY, float scalePeakMbps);
void  ledSet(bool on);
void  ledSetMode(LedMode mode);
void  ledUpdate();
void  initBootButton();
void  updateBootButton();
void  drawDisplay();
void  drawError(const String& line1, const String& line2 = "");
void  networkTask(void* pvParameters);

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[UniFi Traffic Monitor] starting...");

  initBootButton();

  if (STATUS_LED_ENABLED) {
    pinMode(kStatusLedPin, OUTPUT);
  }
  ledSetMode(LED_MODE_WORKING);

  // --- Display init --------------------------------------------------------
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  u8g2.begin();
  u8g2.setContrast(200);

  drawError("Connecting", "to WiFi...");

  // --- WiFi ----------------------------------------------------------------
  if (!wifiConnect()) {
    drawError("WiFi failed", "Check config.h");
    while (true) delay(1000); // halt
  }

  // --- Web server (runs on Core 1 alongside the display) ------------------
  initWebServer();

  // --- FreeRTOS: shared data mutex -----------------------------------------
  g_dataMutex = xSemaphoreCreateMutex();

  // --- Spawn network task pinned to Core 0 ---------------------------------
  // loop() runs on Core 1; all HTTPS polling runs on Core 0 where the WiFi
  // stack lives.  Blocking HTTP calls no longer stall the display or web
  // server, and the TLS client is touched by only one core.
  xTaskCreatePinnedToCore(
    networkTask,    // task function
    "networkTask",  // debug name
    16384,          // stack bytes (TLS + ArduinoJson need headroom)
    NULL,           // parameter (unused)
    1,              // priority (same as loop)
    NULL,           // task handle (not needed)
    0               // Core 0
  );
  Serial.println("[setup] network task started on Core 0");
  Serial.println("[setup] ready.");
}

// ===========================================================================
// loop()  -  Core 1: display, web server, LED, BOOT button
// ===========================================================================
// All HTTPS polling has moved to networkTask() on Core 0.  This function now
// only handles time-sensitive UI work that must never be blocked by network I/O.
void loop() {
  // Guard until setup() has finished initialising the mutex and spawning the
  // network task - prevents races during the very first few milliseconds.
  if (g_dataMutex == NULL) { delay(10); return; }

  ledUpdate();
  updateBootButton();
  g_web.handleClient();

  // Reflect the latest network task result in the status LED.
  // g_networkReady and g_lastFetchOk are volatile so no mutex needed here.
  if (g_networkReady) {
    bool wanDown = false;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    wanDown = g_wanDown;
    xSemaphoreGive(g_dataMutex);

    if (!g_lastFetchOk) {
      ledSetMode(LED_MODE_ERROR);
    } else if (wanDown) {
      ledSetMode(LED_MODE_WAN_DOWN);
    } else {
      ledSetMode(LED_MODE_OK);
    }
  }

  // Only redraw when networkTask has committed new data (version advanced) or
  // the boot IP overlay popup is active (it uses millis() expiry).
  // This prevents 4 of every 5 frames being a redundant clear+redraw+I2C blast
  // at 200 ms poll rate vs 1000 ms data rate, which caused visible choppiness.
  static uint32_t lastDrawnVersion = 0;
  bool overlayActive = ((long)(g_bootIpOverlayUntilMs - millis()) > 0) || g_wanDown;
  if (g_dataVersion != lastDrawnVersion || overlayActive) {
    lastDrawnVersion = g_dataVersion;
    drawDisplay();
  }

  delay(20);  // 50Hz button/LED polling; display only redraws on new data or overlay
}

// ===========================================================================
// networkTask()  -  Core 0: all HTTPS polling
// ===========================================================================
// Pinned to Core 0 where the WiFi/BT stack lives.  Blocking TLS calls here
// never stall the Core 1 display or web server.
void networkTask(void* pvParameters) {
  // Initialise the persistent TLS client on this core.
  initHttpClient();

  // Task-local timing (not shared - no mutex needed).
  unsigned long lastPoll                = 0;
  unsigned long lastUpdateCheck         = 0;
  unsigned long lastControllerStatsPoll = 0;
  unsigned long lastMonthlyPoll         = 0;

  for (;;) {
    // ---- WiFi watchdog --------------------------------------------------
    if (WiFi.status() != WL_CONNECTED) {
      xSemaphoreTake(g_dataMutex, portMAX_DELAY);
      g_statusMsg = "WiFi lost...";
      xSemaphoreGive(g_dataMutex);

      g_lastFetchOk  = false;
      g_networkReady = true;  // let Core 1 show the error state
      closeConnection();

      ledSetMode(LED_MODE_WORKING);
      if (wifiConnect()) {
        initHttpClient();
      } else {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_statusMsg = "WiFi failed";
        xSemaphoreGive(g_dataMutex);
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
      continue;
    }

    unsigned long now = millis();

    // ---- Traffic stats poll (every POLL_INTERVAL_MS) --------------------
    if (now - lastPoll >= POLL_INTERVAL_MS) {
      unsigned long pollStart = now;
      lastPoll = now;

      bool ok = fetchTrafficStats();  // writes shared state under mutex
      g_lastFetchOk = ok;             // volatile write

      if (!ok) {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_fetchErrors++;
        int errs = g_fetchErrors;
        xSemaphoreGive(g_dataMutex);
        if (errs >= MAX_FETCH_ERRORS) {
          closeConnection();
          initHttpClient();
          xSemaphoreTake(g_dataMutex, portMAX_DELAY);
          g_fetchErrors = 0;
          xSemaphoreGive(g_dataMutex);
        }
      } else {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_fetchErrors = 0;
        xSemaphoreGive(g_dataMutex);
      }

      // ---- Firmware update check (piggybacked on the poll cycle) --------
      if (lastUpdateCheck == 0 || (millis() - lastUpdateCheck >= UPDATE_CHECK_INTERVAL_MS)) {
        bool updateAvailable = false;
        bool checkOk = fetchUpdateAvailability(updateAvailable);
        if (checkOk) {
          xSemaphoreTake(g_dataMutex, portMAX_DELAY);
          bool changed = (g_updateAvailable != updateAvailable);
          g_updateAvailable = updateAvailable;
          if (changed) g_dataVersion++;
          xSemaphoreGive(g_dataMutex);
          Serial.printf("[Update] available=%s\n", updateAvailable ? "yes" : "no");
        } else {
          Serial.println("[Update] check failed; keeping previous state");
        }
        lastUpdateCheck = millis();
      }

      // ---- Controller resource stats (every kControllerStatsPollMs) -----
      if (lastControllerStatsPoll == 0 || (millis() - lastControllerStatsPoll >= kControllerStatsPollMs)) {
        float cpuPct = -1.0f, memPct = -1.0f, tempC = -1.0f;
        if (fetchControllerResourceStats(cpuPct, memPct, tempC)) {
          xSemaphoreTake(g_dataMutex, portMAX_DELAY);
          bool updatedAny = false;
          if (cpuPct >= 0.0f && cpuPct <= 100.0f) { g_ucgCpuUtilPct = cpuPct; updatedAny = true; }
          if (memPct >= 0.0f && memPct <= 100.0f) { g_ucgMemUtilPct = memPct; updatedAny = true; }
          if (tempC  >= 0.0f && tempC  <= 150.0f) { g_ucgTempC  = tempC;  updatedAny = true; }
          if (updatedAny) g_lastUcgStatsUpdateMs = millis();
          xSemaphoreGive(g_dataMutex);
        }
        lastControllerStatsPoll = millis();
      }

      // ---- Monthly bandwidth usage (every kMonthlyUsagePollMs) -----------
      if (lastMonthlyPoll == 0 || (millis() - lastMonthlyPoll >= kMonthlyUsagePollMs)) {
        fetchMonthlyUsage();
        lastMonthlyPoll = millis();
      }

      // ---- ESP telemetry ------------------------------------------------
      float espTemp = temperatureRead();
      unsigned long pollElapsed = millis() - pollStart;
      float workPct = min(100.0f, (100.0f * (float)pollElapsed) / (float)POLL_INTERVAL_MS);
      xSemaphoreTake(g_dataMutex, portMAX_DELAY);
      g_espTempC             = espTemp;
      g_espCpuUtilPct        = workPct;
      g_lastEspStatsUpdateMs = millis();
      xSemaphoreGive(g_dataMutex);

      g_networkReady = true;  // at least one poll attempt has completed
    }

    vTaskDelay(pdMS_TO_TICKS(10));  // yield; prevents WDT trigger
  }
}

// ===========================================================================
// HTTP client lifecycle
// ===========================================================================

/*
 * Initialise (or re-initialise) the persistent TLS client.
 * Safe to call multiple times - just resets internal state.
 */
void initHttpClient() {
  g_secureClient.setInsecure();   // LAN-trust: accept self-signed UCG-MAX cert
  g_http.setReuse(true);          // keep TCP/TLS connection alive between requests
  g_httpInitialised = true;
  Serial.println("[HTTP] persistent client ready");
}

/*
 * Tear down the current TLS connection (e.g. after WiFi drop or fatal error).
 * The next request will perform a full handshake.
 */
void closeConnection() {
  g_http.end();
  g_secureClient.stop();
  g_httpInitialised = false;
  Serial.println("[HTTP] connection closed");
}

// ===========================================================================
// WiFi helpers
// ===========================================================================
void ledSet(bool on) {
  if (!STATUS_LED_ENABLED) {
    g_ledOn = false;
    return;
  }

  g_ledOn = on;
  int level;
  if (on) {
    level = kStatusLedActiveLow ? LOW : HIGH;
  } else {
    level = kStatusLedActiveLow ? HIGH : LOW;
  }
  digitalWrite(kStatusLedPin, level);
}

void ledSetMode(LedMode mode) {
  if (!STATUS_LED_ENABLED) {
    g_ledMode = mode;
    g_ledOn = false;
    g_ledPulseActive = false;
    return;
  }

  if (g_ledMode == mode) return;

  g_ledMode = mode;
  g_ledLastToggle = millis();
  g_ledLastPulseStart = millis();
  g_ledPulseActive = false;

  if (mode == LED_MODE_ERROR) {
    ledSet(true);
  } else if (mode == LED_MODE_WORKING) {
    ledSet(true);
  } else {
    ledSet(false);
  }
}

void ledUpdate() {
  if (!STATUS_LED_ENABLED) return;

  unsigned long now = millis();

  if (g_ledMode == LED_MODE_ERROR) {
    if (!g_ledOn) ledSet(true);
    return;
  }

  if (g_ledMode == LED_MODE_WORKING) {
    if (now - g_ledLastToggle >= kLedWorkingBlinkMs) {
      ledSet(!g_ledOn);
      g_ledLastToggle = now;
    }
    return;
  }

  if (g_ledMode == LED_MODE_WAN_DOWN) {
    if (now - g_ledLastToggle >= kLedWanDownBlinkMs) {
      ledSet(!g_ledOn);
      g_ledLastToggle = now;
    }
    return;
  }

  // LED_MODE_OK: pulse every kLedOkHeartbeatMs
  if (g_ledPulseActive) {
    if (now - g_ledLastPulseStart >= kLedOkPulseMs) {
      ledSet(false);
      g_ledPulseActive = false;
    }
    return;
  }

  if (now - g_ledLastPulseStart >= kLedOkHeartbeatMs) {
    ledSet(true);
    g_ledPulseActive = true;
    g_ledLastPulseStart = now;
  }
}

bool wifiConnect() {
  ledSetMode(LED_MODE_WORKING);

  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    ledUpdate();
    if (millis() - t0 > 20000) {
      Serial.println("[WiFi] timeout");
      ledSetMode(LED_MODE_ERROR);
      return false;
    }
    delay(100);
    Serial.print('.');
  }

  Serial.printf("\n[WiFi] connected - IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void initWebServer() {
  g_web.on("/", HTTP_GET, handleWebRoot);
  g_web.on("/api/stats", HTTP_GET, handleWebStats);
  g_web.begin();
  Serial.printf("[Web] dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
}

void initBootButton() {
  if (!BOOT_BUTTON_ENABLED) return;

  if (kBootButtonActiveLow) {
    pinMode(kBootButtonPin, INPUT_PULLUP);
  } else {
    pinMode(kBootButtonPin, INPUT_PULLDOWN);
  }

  int level = digitalRead(kBootButtonPin);
  bool pressed = kBootButtonActiveLow ? (level == LOW) : (level == HIGH);
  g_bootLastRawPressed = pressed;
  g_bootStablePressed = pressed;
  g_bootLastRawChangeMs = millis();
}

void updateBootButton() {
  if (!BOOT_BUTTON_ENABLED) return;

  unsigned long now = millis();
  int level = digitalRead(kBootButtonPin);
  bool rawPressed = kBootButtonActiveLow ? (level == LOW) : (level == HIGH);

  if (rawPressed != g_bootLastRawPressed) {
    g_bootLastRawPressed = rawPressed;
    g_bootLastRawChangeMs = now;
  }

  if (now - g_bootLastRawChangeMs < kBootButtonDebounceMs) {
    return;
  }

  if (rawPressed != g_bootStablePressed) {
    g_bootStablePressed = rawPressed;

    if (g_bootStablePressed) {
      return;
    }

    g_bootIpOverlayMsg = "IP: " + WiFi.localIP().toString();
    g_bootIpOverlayUntilMs = now + kBootButtonIpShowMs;
    Serial.printf("[BOOT] short press -> show IP: %s\n", g_bootIpOverlayMsg.c_str());
    // loop() will pick up overlayActive on the very next 20ms tick and redraw.
    return;
  }
}

static bool webCheckAuth() {
  if (strlen(WEB_AUTH_USER) > 0 && strlen(WEB_AUTH_PASS) > 0) {
    if (!g_web.authenticate(WEB_AUTH_USER, WEB_AUTH_PASS)) {
      g_web.requestAuthentication(DIGEST_AUTH, "UniFi Monitor");
      return false;
    }
  }
  return true;
}

static void webSendSecurityHeaders() {
  g_web.sendHeader("X-Frame-Options", "SAMEORIGIN");
  g_web.sendHeader("X-Content-Type-Options", "nosniff");
  g_web.sendHeader("Cache-Control", "no-store");
}

void handleWebRoot() {
  if (!webCheckAuth()) return;
  static const char page[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>UniFi Traffic Monitor</title>
  <style>
    body { font-family: Arial, sans-serif; background:#0f172a; color:#e2e8f0; margin:0; padding:16px; }
    .grid { display:grid; gap:12px; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); }
    .card { background:#111827; border:1px solid #334155; border-radius:10px; padding:12px; }
    .k { color:#94a3b8; font-size:12px; }
    .v { font-size:24px; font-weight:bold; margin-top:6px; }
    .row { display:flex; justify-content:space-between; margin:4px 0; }
    canvas { width:100%; height:120px; background:#020617; border:1px solid #334155; border-radius:8px; }
    .ok { color:#22c55e; }
    .warn { color:#f59e0b; }
    .bad { color:#ef4444; }
  </style>
</head>
<body>
  <h2 style="margin:0 0 12px 0;">UniFi Traffic Monitor</h2>
  <div class="grid">
    <div class="card"><div class="k">IN</div><div id="in" class="v">--</div></div>
    <div class="card"><div class="k">OUT</div><div id="out" class="v">--</div></div>
    <div class="card"><div class="k">Clients</div><div id="clients" class="v">--</div></div>
    <div class="card"><div class="k">WAN Uptime (24h)</div><div id="uptime" class="v">--</div></div>
    <div class="card"><div class="k">WAN Latency</div><div id="latency" class="v">--</div></div>
    <div class="card"><div class="k">Monthly Usage</div><div id="usage" class="v">--</div></div>
    <div class="card"><div class="k">Controller Update</div><div id="update" class="v">--</div></div>
    <div class="card"><div class="k">UCG CPU</div><div id="ucgCpu" class="v">--</div></div>
    <div class="card"><div class="k">UCG Memory</div><div id="ucgMem" class="v">--</div></div>
    <div class="card"><div class="k">UCG Temp (degF)</div><div id="ucgTemp" class="v">--</div></div>
  </div>

  <div class="grid" style="margin-top:12px;">
    <div class="card"><div class="k">IN Traffic History</div><canvas id="inChart" width="500" height="120"></canvas></div>
    <div class="card"><div class="k">OUT Traffic History</div><canvas id="outChart" width="500" height="120"></canvas></div>
  </div>

  <div class="card" style="margin-top:12px;">
    <div class="k">Health</div>
    <div class="row"><span>WiFi RSSI</span><span id="rssi">--</span></div>
    <div class="row"><span>IP</span><span id="ip">--</span></div>
    <div class="row"><span>Status</span><span id="status">--</span></div>
    <div class="row"><span>WAN Down</span><span id="wanDown">--</span></div>
    <div class="row"><span>Fetch Errors</span><span id="fetchErr">--</span></div>
    <div class="row"><span>Free Heap</span><span id="heap">--</span></div>
    <div class="row"><span>Uptime</span><span id="up">--</span></div>
    <div class="row"><span>ESP CPU</span><span id="espCpu">--</span></div>
    <div class="row"><span>ESP Internal Temp</span><span id="espTemp">--</span></div>
    <div class="row"><span>ESP Stats Age</span><span id="espAge">--</span></div>
    <div class="row"><span>UCG Stats Age</span><span id="ucgAge">--</span></div>
  </div>

  <script>
    function drawLine(canvasId, data, color) {
      const c = document.getElementById(canvasId);
      const ctx = c.getContext('2d');
      const w = c.width, h = c.height;
      ctx.clearRect(0,0,w,h);
      if (!data || data.length < 2) return;
      const peak = Math.max(0.1, ...data);
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let i=0;i<data.length;i++) {
        const x = (i/(data.length-1))*(w-1);
        const y = (h-1) - (Math.min(data[i], peak)/peak)*(h-6) - 3;
        if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
      }
      ctx.stroke();
    }

    function fmtMbps(v) {
      if (v == null || isNaN(v)) return '--';
      if (v < 0.001) return Math.round(v * 1000000) + ' bps';
      if (v < 1) return (v * 1000).toFixed(0) + ' Kbps';
      return v.toFixed(2) + ' Mbps';
    }

    function fmtPct(v) {
      if (v == null || isNaN(v) || v < 0) return '--';
      return v.toFixed(1) + '%';
    }

    function fmtTemp(v) {
      if (v == null || isNaN(v) || v < -40) return '--';
      return v.toFixed(1) + ' degF';
    }

    function fmtAge(v) {
      if (v == null || isNaN(v) || v < 0) return '--';
      if (v < 60) return Math.round(v) + 's ago';
      const m = Math.floor(v / 60);
      const s = Math.round(v % 60);
      return m + 'm ' + s + 's ago';
    }

    async function tick() {
      try {
        const res = await fetch('/api/stats', {cache:'no-store'});
        const d = await res.json();
        document.getElementById('in').textContent = fmtMbps(d.in_mbps);
        document.getElementById('out').textContent = fmtMbps(d.out_mbps);
        document.getElementById('clients').textContent = d.clients >= 0 ? d.clients : '--';
        document.getElementById('uptime').textContent = d.wan_uptime_pct >= 0 ? d.wan_uptime_pct.toFixed(2)+'%' : '--.--% ';
        document.getElementById('latency').textContent = d.wan_latency_ms >= 0 ? d.wan_latency_ms.toFixed(1)+' ms' : '--.- ms';
        document.getElementById('update').textContent = d.update_available ? 'UPDATE AVAILABLE' : 'Up to date';
        document.getElementById('usage').textContent = d.monthly_usage_gb >= 0 ? d.monthly_usage_gb.toFixed(1)+' GB' : '--';
        document.getElementById('rssi').textContent = d.wifi_rssi + ' dBm';
        document.getElementById('ip').textContent = d.ip;
        document.getElementById('status').textContent = d.status;
        document.getElementById('wanDown').textContent = d.wan_down ? 'YES' : 'NO';
        document.getElementById('fetchErr').textContent = d.fetch_errors;
        document.getElementById('heap').textContent = d.heap_free + ' bytes';
        document.getElementById('up').textContent = d.uptime_s + ' s';
        document.getElementById('espAge').textContent = fmtAge(d.esp_stats_age_s);
        document.getElementById('ucgAge').textContent = fmtAge(d.ucg_stats_age_s);
        document.getElementById('espCpu').textContent = fmtPct(d.esp_cpu_util_pct);
        document.getElementById('espTemp').textContent = fmtTemp(d.esp_temp_f);
        document.getElementById('ucgCpu').textContent = fmtPct(d.ucg_cpu_util_pct);
        document.getElementById('ucgMem').textContent = fmtPct(d.ucg_mem_util_pct);
        document.getElementById('ucgTemp').textContent = fmtTemp(d.ucg_temp_f);
        drawLine('inChart', d.in_history || [], '#22c55e');
        drawLine('outChart', d.out_history || [], '#38bdf8');
      } catch (e) {
        document.getElementById('status').textContent = 'dashboard fetch error';
      }
    }

    tick();
    setInterval(tick, 2000);
  </script>
</body>
</html>
  )HTML";

  webSendSecurityHeaders();
  g_web.send(200, "text/html", page);
}

void appendHistory(JsonArray arr, float* history, int points) {
  if (points < 1) return;
  if (points > GRAPH_SAMPLES) points = GRAPH_SAMPLES;

  int oldestIdx = g_histIdx - points;
  while (oldestIdx < 0) oldestIdx += GRAPH_SAMPLES;

  for (int i = 0; i < points; i++) {
    int idx = (oldestIdx + i) % GRAPH_SAMPLES;
    arr.add(history[idx]);
  }
}

void handleWebStats() {
  if (!webCheckAuth()) return;
  DynamicJsonDocument doc(16384);

  // Snapshot all shared state under mutex; serialization happens after release.
  xSemaphoreTake(g_dataMutex, portMAX_DELAY);
  doc["in_mbps"]          = g_inMbps;
  doc["out_mbps"]         = g_outMbps;
  doc["clients"]          = g_clients;
  doc["wan_uptime_pct"]   = g_wanUptimePct;
  doc["wan_latency_ms"]   = g_wanLatencyMs;
  doc["wan_down"]         = g_wanDown;
  doc["update_available"] = g_updateAvailable;
  doc["status"]           = g_statusMsg;
  doc["fetch_errors"]     = g_fetchErrors;
  doc["wifi_rssi"]        = WiFi.RSSI();
  doc["ip"]               = WiFi.localIP().toString();
  doc["heap_free"]        = ESP.getFreeHeap();
  doc["uptime_s"]         = millis() / 1000UL;
  doc["esp_cpu_util_pct"] = g_espCpuUtilPct;
  doc["esp_temp_f"]       = (g_espTempC < 0.0f) ? -1.0f : cToF(g_espTempC);
  doc["ucg_cpu_util_pct"] = g_ucgCpuUtilPct;
  doc["ucg_mem_util_pct"] = g_ucgMemUtilPct;
  doc["ucg_temp_f"]         = (g_ucgTempC < 0.0f) ? -1.0f : cToF(g_ucgTempC);
  doc["monthly_usage_gb"]   = g_monthlyUsageGB;

  long espAgeMs = (g_lastEspStatsUpdateMs == 0) ? -1L : (long)(millis() - g_lastEspStatsUpdateMs);
  long ucgAgeMs = (g_lastUcgStatsUpdateMs == 0) ? -1L : (long)(millis() - g_lastUcgStatsUpdateMs);
  doc["esp_stats_age_s"]  = (espAgeMs < 0) ? -1.0f : (float)espAgeMs / 1000.0f;
  doc["ucg_stats_age_s"]  = (ucgAgeMs < 0) ? -1.0f : (float)ucgAgeMs / 1000.0f;

  JsonArray inHist  = doc.createNestedArray("in_history");
  JsonArray outHist = doc.createNestedArray("out_history");
  appendHistory(inHist,  g_inHistory,  WEB_HISTORY_POINTS);
  appendHistory(outHist, g_outHistory, WEB_HISTORY_POINTS);
  xSemaphoreGive(g_dataMutex);

  // Serialize and send outside the mutex - no shared state access here.
  String payload;
  serializeJson(doc, payload);
  webSendSecurityHeaders();
  g_web.send(200, "application/json", payload);
}

// ===========================================================================
// UniFi API helpers
// ===========================================================================

/*
 * GET /proxy/network/api/s/{site}/stat/health
 *
 * Authenticated with X-API-KEY header (no login / session management needed).
 * Reuses the persistent TLS connection - no handshake overhead on the hot path.
 *
 * The "wan" subsystem entry contains:
 *   "rx_bytes-r"  - bytes/sec received  (inbound  / download)
 *   "tx_bytes-r"  - bytes/sec sent      (outbound / upload)
 */
// Helper used only by fetchTrafficStats to write a status string under mutex.
static void _setStatus(const char* msg) {
  xSemaphoreTake(g_dataMutex, portMAX_DELAY);
  g_statusMsg = msg;
  xSemaphoreGive(g_dataMutex);
}

bool fetchTrafficStats() {
  // NOTE: must only be called from networkTask (Core 0); not re-entrant.
  if (!g_httpInitialised) initHttpClient();

  String url = String("https://") + UNIFI_HOST + ":" + UNIFI_PORT
             + "/proxy/network/api/s/" + UNIFI_SITE + "/stat/health";

  // begin() on the same host reuses the live TLS socket (no new handshake)
  if (!g_http.begin(g_secureClient, url)) {
    Serial.println("[UniFi] http.begin failed - reconnecting");
    closeConnection();
    initHttpClient();
    _setStatus("Conn error");
    return false;
  }

  g_http.addHeader("X-API-KEY", UNIFI_API_KEY);
  g_http.addHeader("Accept",    "application/json");
  g_http.setTimeout(8000);

  int httpCode = g_http.GET();
  Serial.printf("[UniFi] health HTTP %d\n", httpCode);

  if (httpCode == 401 || httpCode == 403) {
    Serial.println("[UniFi] API key rejected - check UNIFI_API_KEY in config.h");
    _setStatus("Bad API key");
    return false;
  }

  if (httpCode <= 0) {
    Serial.printf("[UniFi] connection error %d, rebuilding TLS\n", httpCode);
    closeConnection();
    initHttpClient();
    _setStatus("Conn error");
    return false;
  }

  if (httpCode != 200) {
    _setStatus(("API err " + String(httpCode)).c_str());
    return false;
  }

  String payload = g_http.getString();
  // Do NOT call g_http.end() - keep the socket alive for the next poll

  // ---- Parse JSON ---------------------------------------------------------
  // Response: { "meta": {...}, "data": [ { "subsystem": "wan", "rx_bytes-r": N, ... }, ... ] }
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[JSON] parse error: %s\n", err.c_str());
    _setStatus("JSON error");
    return false;
  }

  JsonArray dataArr = doc["data"].as<JsonArray>();
  if (dataArr.isNull()) {
    Serial.println("[JSON] 'data' array missing");
    _setStatus("No data");
    return false;
  }

  // ---- Parse into local variables (no shared state touched yet) -----------
  bool foundWan = false;
  bool wanDown = false;
  int wlanClients = -1;
  int lanClients  = -1;
  int fallbackClients = -1;
  float discoveredWanUptime  = -1.0f;
  float discoveredWanLatency = -1.0f;
  float localInMbps  = 0.0f;
  float localOutMbps = 0.0f;

  for (JsonObject subsys : dataArr) {
    const char* name = subsys["subsystem"];

    if (discoveredWanUptime < 0.0f) {
      float pct = -1.0f;
      if (extractWanUptime24h(subsys, pct)) {
        discoveredWanUptime = pct;
      }
    }

    if (discoveredWanLatency < 0.0f) {
      float ms = -1.0f;
      if (extractWanLatencyMs(subsys, ms)) {
        discoveredWanLatency = ms;
      }
    }

    int users = -1;
    if (!subsys["num_user"].isNull()) {
      users = subsys["num_user"] | -1;
    } else if (!subsys["num_sta"].isNull()) {
      users = subsys["num_sta"] | -1;
    }

    if (users >= 0) {
      if (name && strcmp(name, "wlan") == 0) {
        wlanClients = users;
      } else if (name && strcmp(name, "lan") == 0) {
        lanClients = users;
      }
      if (fallbackClients < 0) fallbackClients = users;
    }

    if (name && strcmp(name, "wan") == 0) {
      wanDown = isWanSubsysDown(subsys);

      // bytes per second -> Mbps  (x8 / 1 000 000)
      float rxRate = subsys["rx_bytes-r"] | 0.0f;
      float txRate = subsys["tx_bytes-r"] | 0.0f;
      localInMbps  = rxRate * 8.0f / 1000000.0f;
      localOutMbps = txRate * 8.0f / 1000000.0f;

      Serial.printf("[Stats] IN=%.2f Mbps  OUT=%.2f Mbps\n", localInMbps, localOutMbps);
      foundWan = true;
    }
  }

  if (!foundWan) {
    Serial.println("[Stats] WAN subsystem not found");
    _setStatus("No WAN data");
    return false;
  }

  // ---- Compute client count (still local) ---------------------------------
  int newClients = -1;
  if (wlanClients >= 0 || lanClients >= 0) {
    newClients = 0;
    if (wlanClients >= 0) newClients += wlanClients;
    if (lanClients  >= 0) newClients += lanClients;
  } else if (fallbackClients >= 0) {
    newClients = fallbackClients;
  }

  // ---- Commit all parsed values to shared state under mutex ---------------
  // The mutex is held only for this brief write block, NOT during the HTTP call.
  xSemaphoreTake(g_dataMutex, portMAX_DELAY);
  g_inMbps                  = localInMbps;
  g_outMbps                 = localOutMbps;
  g_inHistory[g_histIdx]    = localInMbps;
  g_outHistory[g_histIdx]   = localOutMbps;
  g_histIdx                 = (g_histIdx + 1) % GRAPH_SAMPLES;
  g_wanUptimePct            = discoveredWanUptime;
  g_wanLatencyMs            = discoveredWanLatency;
  g_wanDown                 = wanDown;
  // Track WAN down onset for the OLED counter overlay
  static bool prevCommitWanDown = false;
  if (wanDown && !prevCommitWanDown)  g_wanDownSinceMs = millis(); // up->down: latch start time
  else if (!wanDown)                  g_wanDownSinceMs = 0;        // up: clear
  prevCommitWanDown = wanDown;
  if (newClients >= 0) g_clients = newClients;
  g_statusMsg               = wanDown ? "WAN DOWN" : "OK";
  g_dataVersion++;  // signals Core 1 that a new frame is ready to draw
  xSemaphoreGive(g_dataMutex);

  return true;
}

bool fetchMonthlyUsage() {
  // POST /proxy/network/api/s/{site}/stat/report/monthly.gw
  // The controller returns one bucket per calendar month, sorted oldest-first.
  // The LAST entry is always the current (incomplete) month - exactly what the
  // UCG-MAX GUI displays. Reading only that entry means no NTP, no date math,
  // and the counter resets at the month boundary just as the GUI does.
  if (!g_httpInitialised) initHttpClient();

  String url = String("https://") + UNIFI_HOST + ":" + UNIFI_PORT
             + "/proxy/network/api/s/" + UNIFI_SITE + "/stat/report/monthly.gw";

  if (!g_http.begin(g_secureClient, url)) {
    closeConnection();
    initHttpClient();
    return false;
  }

  g_http.addHeader("X-API-KEY",    UNIFI_API_KEY);
  g_http.addHeader("Accept",       "application/json");
  g_http.addHeader("Content-Type", "application/json");
  g_http.setTimeout(8000);

  const char* body = "{\"attrs\":[\"wan-rx_bytes\",\"wan-tx_bytes\"]}";
  int httpCode = g_http.POST(body);
  if (httpCode != 200) {
    Serial.printf("[Monthly] HTTP %d\n", httpCode);
    return false;
  }

  String payload = g_http.getString();

  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Monthly] JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray dataArr = doc["data"].as<JsonArray>();
  if (dataArr.isNull() || dataArr.size() == 0) return false;

  // Take only the last (most recent) entry - that is the current month.
  JsonObject last = dataArr[dataArr.size() - 1];
  double rxBytes = last["wan-rx_bytes"].as<double>();
  double txBytes = last["wan-tx_bytes"].as<double>();

  float totalGB = (float)((rxBytes + txBytes) / 1.0e9);
  Serial.printf("[Monthly] usage=%.2f GB\n", totalGB);

  xSemaphoreTake(g_dataMutex, portMAX_DELAY);
  g_monthlyUsageGB = totalGB;
  xSemaphoreGive(g_dataMutex);

  return true;
}

bool fetchJsonPayload(const String& url, String& payload, int& httpCode, int timeoutMs) {
  if (!g_httpInitialised) initHttpClient();

  if (!g_http.begin(g_secureClient, url)) {
    Serial.println("[UniFi] http.begin failed during update check");
    closeConnection();
    initHttpClient();
    httpCode = -1;
    return false;
  }

  g_http.addHeader("X-API-KEY", UNIFI_API_KEY);
  g_http.addHeader("Accept", "application/json");
  g_http.setTimeout(timeoutMs);

  httpCode = g_http.GET();
  if (httpCode == 200) {
    payload = g_http.getString();
    return true;
  }

  if (httpCode <= 0) {
    Serial.printf("[Update] connection error %d; rebuilding TLS\n", httpCode);
    closeConnection();
    initHttpClient();
  }

  return false;
}

bool keyLooksLikeUpdate(const char* key) {
  if (!key) return false;
  String k = key;
  k.toLowerCase();
  return (k.indexOf("update") >= 0) ||
         (k.indexOf("upgrade") >= 0) ||
         (k.indexOf("upgradable") >= 0) ||
         (k.indexOf("firmware") >= 0) ||
         (k.indexOf("release") >= 0) ||
         (k.indexOf("install") >= 0);
}

bool keyLooksLikeAvailability(const char* key) {
  if (!key) return false;
  String k = key;
  k.toLowerCase();

  return (k == "available") ||
         (k.indexOf("available_version") >= 0) ||
         (k.indexOf("latest_version") >= 0) ||
         (k.indexOf("target_version") >= 0) ||
         (k.indexOf("candidate") >= 0) ||
         (k.indexOf("pending") >= 0) ||
         (k.indexOf("required") >= 0) ||
         (k.indexOf("status") >= 0);
}

bool keyLooksLikeInstalledVersion(const char* key) {
  if (!key) return false;
  String k = key;
  k.toLowerCase();

  return (k == "version") ||
         (k.indexOf("current_version") >= 0) ||
         (k.indexOf("installed_version") >= 0) ||
         (k.indexOf("running_version") >= 0) ||
         (k.indexOf("version_current") >= 0);
}

bool keyLooksLikeAvailableVersion(const char* key) {
  if (!key) return false;
  String k = key;
  k.toLowerCase();

  return (k.indexOf("available_version") >= 0) ||
         (k.indexOf("latest_version") >= 0) ||
         (k.indexOf("target_version") >= 0) ||
         (k.indexOf("upgrade_to") >= 0) ||
         (k.indexOf("new_version") >= 0) ||
         (k == "latest") ||
         (k == "target");
}

bool valueLooksAvailable(JsonVariantConst value) {
  if (value.is<bool>()) {
    return value.as<bool>();
  }

  if (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>()) {
    return value.as<double>() > 0.0;
  }

  if (value.is<const char*>()) {
    String s = value.as<const char*>();
    s.toLowerCase();
    return (s == "true") ||
           (s == "yes") ||
           (s == "available") ||
           (s == "pending") ||
           (s == "required") ||
           (s == "ready") ||
           (s == "installable") ||
           (s == "downloaded");
  }

  if (value.is<JsonArrayConst>()) {
    return value.as<JsonArrayConst>().size() > 0;
  }

  return false;
}

bool extractVersionString(JsonVariantConst value, String& version) {
  version = "";

  if (value.is<const char*>()) {
    String s = value.as<const char*>();
    s.trim();
    bool hasDigit = false;
    for (size_t i = 0; i < s.length(); i++) {
      if (isDigit(static_cast<unsigned char>(s[i]))) {
        hasDigit = true;
        break;
      }
    }
    if (hasDigit) {
      version = s;
      return true;
    }
    return false;
  }

  if (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>()) {
    double numeric = value.as<double>();
    if (numeric >= 0.0) {
      version = String(numeric, 3);
      return true;
    }
  }

  return false;
}

int compareVersionStrings(const String& lhs, const String& rhs) {
  size_t i = 0;
  size_t j = 0;

  while (i < lhs.length() || j < rhs.length()) {
    while (i < lhs.length() && !isDigit(static_cast<unsigned char>(lhs[i]))) i++;
    while (j < rhs.length() && !isDigit(static_cast<unsigned char>(rhs[j]))) j++;

    if (i >= lhs.length() && j >= rhs.length()) return 0;
    if (i >= lhs.length()) return -1;
    if (j >= rhs.length()) return 1;

    unsigned long leftPart = 0;
    while (i < lhs.length() && isDigit(static_cast<unsigned char>(lhs[i]))) {
      leftPart = (leftPart * 10UL) + (lhs[i] - '0');
      i++;
    }

    unsigned long rightPart = 0;
    while (j < rhs.length() && isDigit(static_cast<unsigned char>(rhs[j]))) {
      rightPart = (rightPart * 10UL) + (rhs[j] - '0');
      j++;
    }

    if (leftPart < rightPart) return -1;
    if (leftPart > rightPart) return 1;
  }

  return 0;
}

bool objectIndicatesUpdateAvailable(JsonObjectConst obj, bool allowLooseAvailableKeys) {
  String installedVersion;
  String availableVersion;

  for (JsonPairConst kv : obj) {
    const char* key = kv.key().c_str();
    JsonVariantConst child = kv.value();

    if (keyLooksLikeInstalledVersion(key)) {
      String parsed;
      if (extractVersionString(child, parsed)) {
        installedVersion = parsed;
      }
    }

    if (keyLooksLikeAvailableVersion(key)) {
      String parsed;
      if (extractVersionString(child, parsed)) {
        availableVersion = parsed;
      }
    }

    bool thisKeyIsUpdate = keyLooksLikeUpdate(key);
    bool thisKeyIsAvailability = allowLooseAvailableKeys && keyLooksLikeAvailability(key);
    if ((thisKeyIsUpdate || thisKeyIsAvailability) && valueLooksAvailable(child)) {
      return true;
    }
  }

  if (availableVersion.length() > 0) {
    if (installedVersion.length() == 0) return true;
    return compareVersionStrings(installedVersion, availableVersion) < 0;
  }

  return false;
}

bool jsonContainsUpdateAvailable(JsonVariantConst node, bool parentIsUpdateKey, bool allowLooseAvailableKeys) {
  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();
    if (objectIndicatesUpdateAvailable(obj, allowLooseAvailableKeys)) {
      return true;
    }

    for (JsonPairConst kv : obj) {
      const char* key = kv.key().c_str();
      JsonVariantConst child = kv.value();
      bool thisKeyIsUpdate = keyLooksLikeUpdate(key);
      bool thisKeyIsAvailability = allowLooseAvailableKeys && keyLooksLikeAvailability(key);

      if ((thisKeyIsUpdate || thisKeyIsAvailability) && valueLooksAvailable(child)) {
        return true;
      }

      if (jsonContainsUpdateAvailable(child,
                                      parentIsUpdateKey || thisKeyIsUpdate,
                                      allowLooseAvailableKeys)) {
        return true;
      }
    }
    return false;
  }

  if (node.is<JsonArrayConst>()) {
    JsonArrayConst arr = node.as<JsonArrayConst>();
    for (JsonVariantConst item : arr) {
      if (jsonContainsUpdateAvailable(item, parentIsUpdateKey, allowLooseAvailableKeys)) {
        return true;
      }
    }
    return false;
  }

  if (parentIsUpdateKey) {
    return valueLooksAvailable(node);
  }

  return false;
}

bool fetchUpdateAvailability(bool& updateAvailable) {
  updateAvailable = false;

  String base = String("https://") + UNIFI_HOST + ":" + UNIFI_PORT;
  String endpoint1 = base + "/api/system/updates";
  String endpoint2 = base + "/api/system";
  String endpoint3 = base + "/proxy/network/api/s/" + UNIFI_SITE + "/stat/sysinfo";

  const String endpoints[3] = {endpoint1, endpoint2, endpoint3};
  bool anySuccess = false;

  for (int i = 0; i < 3; i++) {
    String payload;
    int httpCode = 0;
    bool ok = fetchJsonPayload(endpoints[i], payload, httpCode);

    if (httpCode == 401 || httpCode == 403) {
      Serial.println("[Update] API key rejected during update check");
      continue;
    }

    if (!ok) {
      if (httpCode > 0) {
        Serial.printf("[Update] endpoint %d returned HTTP %d\n", i + 1, httpCode);
      }
      continue;
    }

    DynamicJsonDocument doc(24576);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.printf("[Update] JSON parse error on endpoint %d: %s\n", i + 1, err.c_str());
      continue;
    }

    anySuccess = true;
    bool allowLooseAvailableKeys = endpoints[i].endsWith("/api/system/updates") ||
                                   endpoints[i].endsWith("/api/system");
    if (jsonContainsUpdateAvailable(doc.as<JsonVariantConst>(), false, allowLooseAvailableKeys)) {
      updateAvailable = true;
      return true;
    }
  }

  return anySuccess;
}

bool parsePercentValue(JsonVariantConst value, float& pct) {
  if (value.isNull()) return false;

  if (value.is<JsonObjectConst>()) {
    JsonObjectConst obj = value.as<JsonObjectConst>();

    const char* percentKeys[] = {"pct", "percent", "percentage", "value"};
    for (size_t i = 0; i < sizeof(percentKeys) / sizeof(percentKeys[0]); i++) {
      JsonVariantConst v = obj[percentKeys[i]];
      if (parsePercentValue(v, pct)) return true;
    }

    for (JsonPairConst kv : obj) {
      if (parsePercentValue(kv.value(), pct)) return true;
    }

    return false;
  }

  if (value.is<JsonArrayConst>()) {
    JsonArrayConst arr = value.as<JsonArrayConst>();
    for (JsonVariantConst item : arr) {
      if (parsePercentValue(item, pct)) return true;
    }
    return false;
  }

  if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>()) {
    double v = value.as<double>();
    if (v >= 0.0 && v <= 1.0) v *= 100.0;  // ratio -> percent
    if (v < 0.0 || v > 100.0) return false;
    pct = (float)v;
    return true;
  }

  if (value.is<const char*>()) {
    String s = value.as<const char*>();
    s.trim();
    s.replace("%", "");
    if (s.length() == 0) return false;

    char* endPtr = nullptr;
    double v = strtod(s.c_str(), &endPtr);
    if (endPtr == s.c_str()) return false;
    if (v >= 0.0 && v <= 1.0) v *= 100.0;
    if (v < 0.0 || v > 100.0) return false;
    pct = (float)v;
    return true;
  }

  return false;
}

bool extractWanUptime24h(JsonObjectConst wanObj, float& pct) {
  // UCG-MAX fw 5.x: uptime_stats.WAN.uptime / uptime_stats.WAN.time_period gives
  // true float precision (e.g. 1088/1090 = 99.82%) matching the UCG display.
  // The integer "availability" field is already rounded and should be skipped.
  JsonVariantConst uptimeStats = wanObj["uptime_stats"];
  if (!uptimeStats.isNull() && uptimeStats.is<JsonObjectConst>()) {
    JsonVariantConst wanBlock = uptimeStats["WAN"];
    if (!wanBlock.isNull() && wanBlock.is<JsonObjectConst>()) {
      int uptimeSecs    = wanBlock["uptime"]     | -1;
      int timePeriodSecs = wanBlock["time_period"] | -1;
      if (uptimeSecs >= 0 && timePeriodSecs > 0) {
        pct = min(100.0f, 100.0f * (float)uptimeSecs / (float)timePeriodSecs);
        return true;
      }
      // Fallback: integer availability if time fields absent
      int avail = wanBlock["availability"] | -1;
      if (avail >= 0 && avail <= 100) { pct = (float)avail; return true; }
    }
  }

  // Fallback: direct keys for other UniFi hardware / firmware variants
  const char* directKeys[] = {
    "uptime_24h", "wan_uptime_24h", "uptime24h",
    "availability_24h", "wan_availability_24h"
  };
  for (size_t i = 0; i < sizeof(directKeys) / sizeof(directKeys[0]); i++) {
    JsonVariantConst v = wanObj[directKeys[i]];
    if (parsePercentValue(v, pct)) return true;
  }

  return false;
}

bool parseLatencyValue(JsonVariantConst value, float& latencyMs) {
  if (value.isNull()) return false;

  if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>()) {
    double v = value.as<double>();
    if (v < 0.0 || v > 10000.0) return false;
    latencyMs = (float)v;
    return true;
  }

  if (value.is<const char*>()) {
    String s = value.as<const char*>();
    s.trim();
    s.toLowerCase();
    s.replace("ms", "");
    if (s.length() == 0) return false;

    char* endPtr = nullptr;
    double v = strtod(s.c_str(), &endPtr);
    if (endPtr == s.c_str()) return false;
    if (v < 0.0 || v > 10000.0) return false;
    latencyMs = (float)v;
    return true;
  }

  if (value.is<JsonObjectConst>()) {
    JsonObjectConst obj = value.as<JsonObjectConst>();
    const char* keys[] = {"ms", "latency", "value", "avg", "average", "mean", "rtt"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
      JsonVariantConst v = obj[keys[i]];
      if (parseLatencyValue(v, latencyMs)) return true;
    }

    for (JsonPairConst kv : obj) {
      String key = kv.key().c_str();
      key.toLowerCase();
      if (key.indexOf("latency") >= 0 || key.indexOf("ping") >= 0 || key.indexOf("rtt") >= 0) {
        if (parseLatencyValue(kv.value(), latencyMs)) return true;
      }
    }
  }

  if (value.is<JsonArrayConst>()) {
    JsonArrayConst arr = value.as<JsonArrayConst>();
    for (JsonVariantConst item : arr) {
      if (parseLatencyValue(item, latencyMs)) return true;
    }
  }

  return false;
}

bool extractWanLatencyMs(JsonObjectConst wanObj, float& latencyMs) {
  const char* directKeys[] = {
    "latency",
    "wan_latency",
    "latency_ms",
    "avg_latency",
    "average_latency",
    "wan_latency_ms",
    "ping",
    "ping_ms",
    "rtt",
    "rtt_ms"
  };

  for (size_t i = 0; i < sizeof(directKeys) / sizeof(directKeys[0]); i++) {
    JsonVariantConst v = wanObj[directKeys[i]];
    if (parseLatencyValue(v, latencyMs)) return true;
  }

  for (JsonPairConst kv : wanObj) {
    String key = kv.key().c_str();
    key.toLowerCase();
    if (key.indexOf("latency") >= 0 || key.indexOf("ping") >= 0 || key.indexOf("rtt") >= 0) {
      if (parseLatencyValue(kv.value(), latencyMs)) return true;
    }
  }

  return false;
}

bool isWanSubsysDown(JsonObjectConst wanObj) {
  // UCG-MAX fw 5.x - two confirmed failure modes (status field is "ok" in all cases):
  //
  //  1. Port disabled (software):  uptime_stats={} WAN key absent, isp_name absent
  //  2. ISP/modem outage:          uptime_stats.WAN.downtime present,
  //                                all alerting_monitors[*].availability==0,
  //                                isp_name still present (physical link to router intact)

  JsonVariantConst uptimeStats = wanObj["uptime_stats"];
  if (!uptimeStats.isNull() && uptimeStats.is<JsonObjectConst>()) {
    JsonVariantConst wanBlock = uptimeStats["WAN"];

    if (wanBlock.isNull()) {
      // uptime_stats present but WAN key missing → port disabled / physical link down
      return true;
    }

    // downtime field appears in uptime_stats.WAN only during an active outage
    if (!wanBlock["downtime"].isNull()) return true;

    // All alerting monitors failing → ISP unreachable even with carrier present
    JsonVariantConst alerting = wanBlock["alerting_monitors"];
    if (!alerting.isNull() && alerting.is<JsonArrayConst>()) {
      JsonArrayConst arr = alerting.as<JsonArrayConst>();
      if (arr.size() > 0) {
        bool allDown = true;
        for (JsonVariantConst mon : arr) {
          if ((mon["availability"] | 1) != 0) { allDown = false; break; }
        }
        if (allDown) return true;
      }
    }

    // WAN block present, no downtime, probes passing → link is up
    return false;
  }

  // uptime_stats absent entirely — isp_name present only when link is genuinely up
  if (!wanObj["isp_name"].isNull()) return false;

  // Fallback boolean flags for other UniFi hardware / firmware variants
  const char* boolDownKeys[] = {"up", "connected", "is_up", "link_up", "wan_up"};
  for (size_t i = 0; i < sizeof(boolDownKeys) / sizeof(boolDownKeys[0]); i++) {
    JsonVariantConst v = wanObj[boolDownKeys[i]];
    if (v.is<bool>()) return !v.as<bool>();
  }

  // Fallback text state keys
  const char* stateKeys[] = {"status", "state", "wan_status", "link_state"};
  for (size_t i = 0; i < sizeof(stateKeys) / sizeof(stateKeys[0]); i++) {
    JsonVariantConst v = wanObj[stateKeys[i]];
    if (v.is<const char*>()) {
      String s = v.as<const char*>();
      s.toLowerCase();
      s.trim();
      if (s == "down" || s == "disconnected" || s == "offline" || s == "inactive") return true;
      if (s == "up" || s == "connected" || s == "online" || s == "active" || s == "ok") return false;
    }
  }

  return false;
}

bool parseNumericValue(JsonVariantConst value, float& parsed) {
  if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>()) {
    parsed = (float)value.as<double>();
    return true;
  }

  if (value.is<const char*>()) {
    String s = value.as<const char*>();
    s.trim();
    s.replace("%", "");
    s.replace("C", "");
    s.replace("c", "");
    s.replace("deg", "");
    if (s.length() == 0) return false;

    char* endPtr = nullptr;
    double v = strtod(s.c_str(), &endPtr);
    if (endPtr == s.c_str()) return false;
    parsed = (float)v;
    return true;
  }

  return false;
}

bool keyContainsAny(const String& key, const char* const* tokens, size_t tokenCount) {
  String lower = key;
  lower.toLowerCase();
  for (size_t i = 0; i < tokenCount; i++) {
    if (lower.indexOf(tokens[i]) >= 0) return true;
  }
  return false;
}

void scanSystemMetrics(JsonVariantConst node, bool& gotCpu, bool& gotMem, bool& gotTemp, float& cpuPct, float& memPct, float& tempC) {
  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();

    if (!gotMem) {
      float used = -1.0f;
      float total = -1.0f;
      float freeVal = -1.0f;
      float avail = -1.0f;

      if (parseNumericValue(obj["used"], used) && parseNumericValue(obj["total"], total) && total > 0.0f) {
        float pct = (used * 100.0f) / total;
        if (pct >= 0.0f && pct <= 100.0f) {
          memPct = pct;
          gotMem = true;
        }
      }

      if (!gotMem && parseNumericValue(obj["free"], freeVal) && parseNumericValue(obj["total"], total) && total > 0.0f) {
        float pct = ((total - freeVal) * 100.0f) / total;
        if (pct >= 0.0f && pct <= 100.0f) {
          memPct = pct;
          gotMem = true;
        }
      }

      if (!gotMem && parseNumericValue(obj["available"], avail) && parseNumericValue(obj["total"], total) && total > 0.0f) {
        float pct = ((total - avail) * 100.0f) / total;
        if (pct >= 0.0f && pct <= 100.0f) {
          memPct = pct;
          gotMem = true;
        }
      }
    }

    static const char* cpuTokens[] = {"cpu", "load"};
    static const char* memTokens[] = {"mem", "memory", "ram"};
    static const char* tempTokens[] = {"temp", "thermal"};
    static const char* memBadTokens[] = {"total", "free", "avail", "available", "cached", "buffer"};

    for (JsonPairConst kv : obj) {
      String key = kv.key().c_str();
      String keyLower = key;
      keyLower.toLowerCase();
      JsonVariantConst value = kv.value();

      float parsed = -1.0f;
      bool scalar = parseNumericValue(value, parsed);

      if (scalar) {
        if (!gotCpu && keyContainsAny(keyLower, cpuTokens, sizeof(cpuTokens) / sizeof(cpuTokens[0])) && keyLower.indexOf("temp") < 0) {
          float pct = parsed;
          if (pct >= 0.0f && pct <= 1.0f) pct *= 100.0f;
          if (pct >= 0.0f && pct <= 100.0f) {
            cpuPct = pct;
            gotCpu = true;
          }
        }

        if (!gotMem && keyContainsAny(keyLower, memTokens, sizeof(memTokens) / sizeof(memTokens[0]))
            && !keyContainsAny(keyLower, memBadTokens, sizeof(memBadTokens) / sizeof(memBadTokens[0]))) {
          float pct = parsed;
          if (pct >= 0.0f && pct <= 1.0f) pct *= 100.0f;
          if (pct >= 0.0f && pct <= 100.0f) {
            memPct = pct;
            gotMem = true;
          }
        }

        if (!gotTemp && keyContainsAny(keyLower, tempTokens, sizeof(tempTokens) / sizeof(tempTokens[0]))) {
          float c = parsed;
          bool keySaysF = (keyLower.indexOf("_f") >= 0 || keyLower.indexOf("fahren") >= 0);
          bool keySaysC = (keyLower.indexOf("_c") >= 0 || keyLower.indexOf("celsius") >= 0);

          if (keySaysF) {
            c = (c - 32.0f) * (5.0f / 9.0f);
          }
          if (c > 1000.0f && c < 200000.0f) {
            c = c / 1000.0f;
          }
          // Some payloads provide Fahrenheit without explicit unit hints.
          // Values above realistic router Celsius range are treated as Fahrenheit.
          if (!keySaysC && !keySaysF && c > 90.0f && c <= 150.0f) {
            c = (c - 32.0f) * (5.0f / 9.0f);
          }
          if (c > 150.0f && c <= 212.0f) {
            c = (c - 32.0f) * (5.0f / 9.0f);
          }
          if (c >= 0.0f && c <= 150.0f) {
            tempC = c;
            gotTemp = true;
          }
        }
      }

      if (!gotCpu || !gotMem || !gotTemp) {
        scanSystemMetrics(value, gotCpu, gotMem, gotTemp, cpuPct, memPct, tempC);
      }
    }
    return;
  }

  if (node.is<JsonArrayConst>()) {
    JsonArrayConst arr = node.as<JsonArrayConst>();
    for (JsonVariantConst item : arr) {
      if (gotCpu && gotMem && gotTemp) break;
      scanSystemMetrics(item, gotCpu, gotMem, gotTemp, cpuPct, memPct, tempC);
    }
  }
}

bool fetchControllerResourceStats(float& cpuPct, float& memPct, float& tempC) {
  cpuPct = -1.0f;
  memPct = -1.0f;
  tempC  = -1.0f;

  // Only ONE endpoint is tried per call - g_ucgEndpointIdx rotates on each
  // invocation so all three are covered over successive 10-second windows
  // without ever blocking more than one HTTPS request per poll cycle.
  String base = String("https://") + UNIFI_HOST + ":" + UNIFI_PORT;
  const String endpoints[3] = {
    base + "/proxy/network/api/s/" + UNIFI_SITE + "/stat/sysinfo",
    base + "/api/system",
    base + "/proxy/network/api/s/" + UNIFI_SITE + "/stat/device"
  };

  String url = endpoints[g_ucgEndpointIdx];
  g_ucgEndpointIdx = (g_ucgEndpointIdx + 1) % 3;

  String payload;
  int httpCode = 0;
  // 3 s timeout - keeps one blocking request well under one poll cycle
  bool ok = fetchJsonPayload(url, payload, httpCode, 3000);
  if (!ok) return false;

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  bool gotCpu  = false;
  bool gotMem  = false;
  bool gotTemp = false;
  scanSystemMetrics(doc.as<JsonVariantConst>(), gotCpu, gotMem, gotTemp, cpuPct, memPct, tempC);

  return gotCpu || gotMem || gotTemp;
}

float cToF(float c) {
  return (c * 9.0f / 5.0f) + 32.0f;
}

// ===========================================================================
// Display
// ===========================================================================

/*
 * Format a Mbps value into a compact human-readable string.
 */
static void formatRate(float mbps, char* buf, size_t bufLen) {
  if (mbps < kRateBpsThresholdMbps) {
    snprintf(buf, bufLen, "%.0f bps", mbps * 1000000.0f);
  } else if (mbps < 1.0f) {
    snprintf(buf, bufLen, "%.0f Kbps", mbps * 1000.0f);
  } else {
    snprintf(buf, bufLen, "%.2f Mbps", mbps);
  }
}

/*
 * Draw real traffic history as a line graph confined to one box.
 */
float historyPeakRecent(float* history, int samplesBack) {
  if (samplesBack < 1) samplesBack = 1;
  if (samplesBack > GRAPH_SAMPLES) samplesBack = GRAPH_SAMPLES;

  float peak = 0.0001f;
  int startIdx = g_histIdx - samplesBack;
  while (startIdx < 0) startIdx += GRAPH_SAMPLES;

  for (int i = 0; i < samplesBack; i++) {
    int idx = (startIdx + i) % GRAPH_SAMPLES;
    if (history[idx] > peak) peak = history[idx];
  }

  return peak;
}

void drawTrafficGraphInBox(float* history, int leftX, int rightX, int topY, int bottomY, float scalePeakMbps) {
  int w = rightX - leftX;
  int h = bottomY - topY;
  if (w < 10 || h < 8) return;

  float peak = scalePeakMbps;
  if (peak < 0.1f) peak = 0.1f;

  int graphPixels = w - 2;
  int windowSamples = graphPixels;
  if (windowSamples > GRAPH_SAMPLES) windowSamples = GRAPH_SAMPLES;
  if (windowSamples < 2) return;

  int oldestIdx = g_histIdx - windowSamples;
  while (oldestIdx < 0) oldestIdx += GRAPH_SAMPLES;

  int prevX = 0;
  int prevY = 0;
  bool hasPrev = false;

  for (int i = 0; i < windowSamples; i++) {
    int x = leftX + 1 + ((i * (graphPixels - 1)) / (windowSamples - 1));
    int sampleIdx = (oldestIdx + i) % GRAPH_SAMPLES;
    float v = history[sampleIdx];

    int y = bottomY - 1 - (int)((v / peak) * (h - 2));
    if (y <= topY) y = topY + 1;
    if (y >= bottomY) y = bottomY - 1;

    if (hasPrev) {
      u8g2.drawLine(prevX, prevY, x, y);
    }

    prevX = x;
    prevY = y;
    hasPrev = true;
  }
}

void drawDisplay() {
  char bufIn[16], bufOut[16], clientBuf[20], uptimeBuf[12], latencyBuf[12], usageBuf[12];
  char inLine[24], outLine[24];

  // Hold the mutex while reading shared globals and filling the u8g2 frame
  // buffer.  The mutex is released BEFORE sendBuffer() so the slow I2C
  // transfer does not block the network task from writing new data.
  xSemaphoreTake(g_dataMutex, portMAX_DELAY);

  formatRate(g_inMbps,  bufIn,  sizeof(bufIn));
  formatRate(g_outMbps, bufOut, sizeof(bufOut));

  u8g2.clearBuffer();

  // Two-column layout: left 2/3 traffic, right 1/3 clients
  const int splitX = 85;
  const int leftInnerL = 1;
  const int leftInnerR = splitX - 1;

  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawVLine(splitX, 0, 64);
  u8g2.drawHLine(1, 11, splitX - 1);
  u8g2.drawHLine(1, 37, splitX - 1);

  // Independent scale for each graph: peak over the entire ring buffer,
  // floored at 0.1 Mbps.  Scanning all GRAPH_SAMPLES (>= pixels drawn)
  // guarantees no visible sample overflows the scale.  Mirrors the web
  // graph which runs Math.max(0.1, ...all_data).
  float inPeakRaw  = historyPeakRecent(g_inHistory, GRAPH_SAMPLES);
  float outPeakRaw = historyPeakRecent(g_outHistory, GRAPH_SAMPLES);
  g_inGraphScale  = max(0.1f, inPeakRaw);
  g_outGraphScale = max(0.1f, outPeakRaw);

  drawTrafficGraphInBox(g_inHistory,  leftInnerL, leftInnerR, 12, 36, g_inGraphScale);
  drawTrafficGraphInBox(g_outHistory, leftInnerL, leftInnerR, 38, 62, g_outGraphScale);

  // Left column title + traffic values
  u8g2.setFont(u8g2_font_4x6_tf);
  const char* title = "INTERNET TRAFFIC";
  int titleW = u8g2.getStrWidth(title);
  int titleX = (splitX - titleW) / 2;
  u8g2.drawStr(titleX, 7, title);
  // Asterisk in top-left corner when a firmware update is available
  if (g_updateAvailable) {
    u8g2.drawStr(1, 7, "*");
  }

  u8g2.setFont(u8g2_font_5x8_tf);
  snprintf(inLine, sizeof(inLine), "IN: %s", bufIn);
  u8g2.drawStr(4, 20, inLine);

  u8g2.setFont(u8g2_font_5x8_tf);
  snprintf(outLine, sizeof(outLine), "OUT: %s", bufOut);
  u8g2.drawStr(4, 46, outLine);

  // Right column clients panel
  u8g2.setFont(u8g2_font_4x6_tf);
  const char* clientsTitle = "Clients:";
  int clientsTitleW = u8g2.getStrWidth(clientsTitle);
  int rightCenterX = splitX + ((128 - splitX) / 2);
  u8g2.drawStr(rightCenterX - (clientsTitleW / 2), 8, clientsTitle);

  if (g_clients >= 0) {
    snprintf(clientBuf, sizeof(clientBuf), "%d", g_clients);
  } else {
    snprintf(clientBuf, sizeof(clientBuf), "-");
  }
  u8g2.setFont(u8g2_font_5x8_tf);  // smaller than 6x10
  int clientW = u8g2.getStrWidth(clientBuf);
  u8g2.drawStr(rightCenterX - (clientW / 2), 15, clientBuf);

  u8g2.setFont(u8g2_font_4x6_tf);
  const char* uptimeTitle = "WAN UPTIME";
  int uptimeTitleW = u8g2.getStrWidth(uptimeTitle);
  u8g2.drawStr(rightCenterX - (uptimeTitleW / 2), 23, uptimeTitle);

  if (g_wanUptimePct >= 0.0f) {
    snprintf(uptimeBuf, sizeof(uptimeBuf), "%.2f%%", g_wanUptimePct);
  } else {
    snprintf(uptimeBuf, sizeof(uptimeBuf), "--.--%%");
  }
  int uptimeW = u8g2.getStrWidth(uptimeBuf);
  u8g2.drawStr(rightCenterX - (uptimeW / 2), 29, uptimeBuf);

  const char* latencyTitle = "WAN LATENCY";
  int latencyTitleW = u8g2.getStrWidth(latencyTitle);
  u8g2.drawStr(rightCenterX - (latencyTitleW / 2), 37, latencyTitle);

  if (g_wanLatencyMs >= 0.0f) {
    snprintf(latencyBuf, sizeof(latencyBuf), "%.1fms", g_wanLatencyMs);
  } else {
    snprintf(latencyBuf, sizeof(latencyBuf), "--.-ms");
  }
  int latencyW = u8g2.getStrWidth(latencyBuf);
  u8g2.drawStr(rightCenterX - (latencyW / 2), 43, latencyBuf);

  // Bottom of right column: USAGE
  u8g2.setFont(u8g2_font_4x6_tf);
  const char* usageTitle = "USAGE";
  int usageTitleW = u8g2.getStrWidth(usageTitle);
  u8g2.drawStr(rightCenterX - (usageTitleW / 2), 51, usageTitle);
  if (g_monthlyUsageGB >= 0.0f) {
    if (g_monthlyUsageGB >= 1000.0f)
      snprintf(usageBuf, sizeof(usageBuf), "%.1fTB", g_monthlyUsageGB / 1024.0f);
    else
      snprintf(usageBuf, sizeof(usageBuf), "%.1fGB", g_monthlyUsageGB);
  } else {
    snprintf(usageBuf, sizeof(usageBuf), "--GB");
  }
  int usageW = u8g2.getStrWidth(usageBuf);
  u8g2.drawStr(rightCenterX - (usageW / 2), 57, usageBuf);

  bool showBootIpOverlay = ((long)(g_bootIpOverlayUntilMs - millis()) > 0);
  bool showWanDownOverlay = (g_wanDownSinceMs != 0);  // volatile read, no mutex needed
  if (showWanDownOverlay) showBootIpOverlay = false;   // WAN DOWN takes priority

  // -- Status (bottom-left, only on error) ----------------------------------
  if (!showBootIpOverlay && !showWanDownOverlay && g_statusMsg != "OK") {
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(0, 63, g_statusMsg.c_str());
  }

  if (showBootIpOverlay) {
    const int boxW = 112;
    const int boxH = 24;
    const int boxX = (128 - boxW) / 2;
    const int boxY = (64 - boxH) / 2;

    u8g2.setDrawColor(0);
    u8g2.drawBox(boxX, boxY, boxW, boxH);
    u8g2.setDrawColor(1);
    u8g2.drawFrame(boxX, boxY, boxW, boxH);

    const char* popupTitle = "LOCAL IP";
    u8g2.setFont(u8g2_font_4x6_tf);
    int popupTitleW = u8g2.getStrWidth(popupTitle);
    u8g2.drawStr(boxX + (boxW - popupTitleW) / 2, boxY + 8, popupTitle);

    u8g2.setFont(u8g2_font_5x8_tf);
    int ipW = u8g2.getStrWidth(g_bootIpOverlayMsg.c_str());
    u8g2.drawStr(boxX + (boxW - ipW) / 2, boxY + 18, g_bootIpOverlayMsg.c_str());
  }

  if (showWanDownOverlay) {
    unsigned long downSecs = (millis() - g_wanDownSinceMs) / 1000UL;
    unsigned long m = downSecs / 60;
    unsigned long s = downSecs % 60;
    char countBuf[16];
    if (m > 0) snprintf(countBuf, sizeof(countBuf), "%lum %02lus", m, s);
    else       snprintf(countBuf, sizeof(countBuf), "%lus", s);

    const int boxW = 112;
    const int boxH = 24;
    const int boxX = (128 - boxW) / 2;
    const int boxY = (64 - boxH) / 2;

    u8g2.setDrawColor(0);
    u8g2.drawBox(boxX, boxY, boxW, boxH);
    u8g2.setDrawColor(1);
    u8g2.drawFrame(boxX, boxY, boxW, boxH);

    const char* wanTitle = "WAN DOWN";
    u8g2.setFont(u8g2_font_4x6_tf);
    int wanTitleW = u8g2.getStrWidth(wanTitle);
    u8g2.drawStr(boxX + (boxW - wanTitleW) / 2, boxY + 8, wanTitle);

    u8g2.setFont(u8g2_font_5x8_tf);
    int countW = u8g2.getStrWidth(countBuf);
    u8g2.drawStr(boxX + (boxW - countW) / 2, boxY + 18, countBuf);
  }

  // Release mutex before the I2C transfer so the network task is not blocked
  // by the ~5-10 ms it takes to clock out a full SH1106 frame buffer.
  xSemaphoreGive(g_dataMutex);
  u8g2.sendBuffer();
}

/*
 * Full-screen error display used during startup / fatal errors.
 */
void drawError(const String& line1, const String& line2) {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x10_tf);

  // Line 1 - centered vertically, slightly above middle
  int l1W = u8g2.getStrWidth(line1.c_str());
  u8g2.drawStr((128 - l1W) / 2, line2.isEmpty() ? 35 : 27, line1.c_str());

  if (!line2.isEmpty()) {
    int l2W = u8g2.getStrWidth(line2.c_str());
    u8g2.drawStr((128 - l2W) / 2, 43, line2.c_str());
  }

  u8g2.sendBuffer();
}
