/*
 * UniFi_Traffic_Monitor.ino
 *
 * Displays real-time internet traffic (IN / OUT Mbps) from a Ubiquiti UCG-MAX
 * on a 128×64 SH1106 OLED over I²C.
 *
 * Target hardware : ESP32 (any variant)
 * Display         : SH1106 128×64 I²C (0x3C)
 * Controller      : Ubiquiti UCG-MAX (UniFi OS 5.x / Network 10.x)
 *
 * Required libraries (install via Arduino Library Manager):
 *   - U8g2            by oliver  (display driver)
 *   - ArduinoJson     by bblanchon (JSON parsing)
 *   - WiFiClientSecure & HTTPClient – bundled with ESP32 board package
 *
 * Configuration: edit config.h
 *
 * Auth strategy:
 *   Uses the UniFi OS API key (X-API-KEY header) – no login, no cookies,
 *   no CSRF tokens, no session expiry to manage.
 *   Generate a key in UniFi OS → Settings → API Keys.
 *
 * Connection strategy:
 *   A single WiFiClientSecure + HTTPClient pair is kept alive across polls.
 *   HTTPClient::setReuse(true) suppresses a new TLS handshake on every GET,
 *   saving ~200-300 ms per poll and allowing a stable 1 s refresh rate.
 *   The connection is re-established automatically after any failure or
 *   after a WiFi drop.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "config.h"

// ---------------------------------------------------------------------------
// Display – SH1106 128×64, full-buffer, hardware I²C
// ---------------------------------------------------------------------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset */ U8X8_PIN_NONE);

// ---------------------------------------------------------------------------
// Persistent HTTP connection (reused across every poll)
// ---------------------------------------------------------------------------
static WiFiClientSecure g_secureClient;
static HTTPClient       g_http;
static bool             g_httpInitialised = false;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int    g_fetchErrors    = 0;
static float  g_inMbps         = 0.0f;
static float  g_outMbps        = 0.0f;
static int    g_clients        = -1;
static String g_statusMsg      = "Connecting...";
static unsigned long g_lastPoll = 0;

// ---------------------------------------------------------------------------
// Waveform history  (one sample per horizontal pixel = 128 columns)
// ---------------------------------------------------------------------------
#define GRAPH_SAMPLES 128

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
void  drawSineInBox(int leftX, int rightX, int topY, int bottomY, float phase);
void  drawDisplay();
void  drawError(const String& line1, const String& line2 = "");

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[UniFi Traffic Monitor] starting...");

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

  // --- Persistent TLS client -----------------------------------------------
  initHttpClient();

  Serial.println("[setup] ready.");
}

// ===========================================================================
// loop()
// ===========================================================================
void loop() {
  // Reconnect WiFi if lost
  if (WiFi.status() != WL_CONNECTED) {
    closeConnection();
    g_statusMsg = "WiFi lost...";
    drawDisplay();
    wifiConnect();
    initHttpClient();
    return;
  }

  // Poll on interval
  if (millis() - g_lastPoll >= POLL_INTERVAL_MS) {
    g_lastPoll = millis();

    if (!fetchTrafficStats()) {
      g_fetchErrors++;
      if (g_fetchErrors >= MAX_FETCH_ERRORS) {
        // Persistent failure – tear down TLS and rebuild on next poll
        closeConnection();
        initHttpClient();
        g_fetchErrors = 0;
      }
    } else {
      g_fetchErrors = 0;
    }

    drawDisplay();
  }
}

// ===========================================================================
// HTTP client lifecycle
// ===========================================================================

/*
 * Initialise (or re-initialise) the persistent TLS client.
 * Safe to call multiple times – just resets internal state.
 */
void initHttpClient() {
  g_secureClient.setInsecure();   // accept self-signed UCG-MAX cert
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
bool wifiConnect() {
  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      Serial.println("[WiFi] timeout");
      return false;
    }
    delay(500);
    Serial.print('.');
  }

  Serial.printf("\n[WiFi] connected – IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// ===========================================================================
// UniFi API helpers
// ===========================================================================

/*
 * GET /proxy/network/api/s/{site}/stat/health
 *
 * Authenticated with X-API-KEY header (no login / session management needed).
 * Reuses the persistent TLS connection – no handshake overhead on the hot path.
 *
 * The "wan" subsystem entry contains:
 *   "rx_bytes-r"  – bytes/sec received  (inbound  / download)
 *   "tx_bytes-r"  – bytes/sec sent      (outbound / upload)
 */
bool fetchTrafficStats() {
  if (!g_httpInitialised) initHttpClient();

  String url = String("https://") + UNIFI_HOST + ":" + UNIFI_PORT
             + "/proxy/network/api/s/" + UNIFI_SITE + "/stat/health";

  // begin() on the same host reuses the live TLS socket (no new handshake)
  if (!g_http.begin(g_secureClient, url)) {
    Serial.println("[UniFi] http.begin failed – reconnecting");
    closeConnection();
    initHttpClient();
    return false;
  }

  g_http.addHeader("X-API-KEY", UNIFI_API_KEY);
  g_http.addHeader("Accept",    "application/json");
  g_http.setTimeout(8000);

  int httpCode = g_http.GET();
  Serial.printf("[UniFi] health HTTP %d\n", httpCode);

  if (httpCode == 401 || httpCode == 403) {
    Serial.println("[UniFi] API key rejected – check UNIFI_API_KEY in config.h");
    g_statusMsg = "Bad API key";
    return false;
  }

  if (httpCode <= 0) {
    Serial.printf("[UniFi] connection error %d, rebuilding TLS\n", httpCode);
    g_statusMsg = "Conn error";
    closeConnection();
    initHttpClient();
    return false;
  }

  if (httpCode != 200) {
    g_statusMsg = "API err " + String(httpCode);
    return false;
  }

  String payload = g_http.getString();
  // Do NOT call g_http.end() – keep the socket alive for the next poll

  // ---- Parse JSON ---------------------------------------------------------
  // Response: { "meta": {...}, "data": [ { "subsystem": "wan", "rx_bytes-r": N, ... }, ... ] }
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[JSON] parse error: %s\n", err.c_str());
    g_statusMsg = "JSON error";
    return false;
  }

  JsonArray dataArr = doc["data"].as<JsonArray>();
  if (dataArr.isNull()) {
    Serial.println("[JSON] 'data' array missing");
    g_statusMsg = "No data";
    return false;
  }

  bool foundWan = false;
  int wlanClients = -1;
  int lanClients  = -1;
  int fallbackClients = -1;

  for (JsonObject subsys : dataArr) {
    const char* name = subsys["subsystem"];

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
      // bytes per second → Mbps  (×8 / 1 000 000)
      float rxRate = subsys["rx_bytes-r"] | 0.0f;
      float txRate = subsys["tx_bytes-r"] | 0.0f;

      g_inMbps  = rxRate * 8.0f / 1000000.0f;
      g_outMbps = txRate * 8.0f / 1000000.0f;

      // Push into waveform ring buffers
      g_inHistory[g_histIdx]  = g_inMbps;
      g_outHistory[g_histIdx] = g_outMbps;
      g_histIdx = (g_histIdx + 1) % GRAPH_SAMPLES;

      Serial.printf("[Stats] IN=%.2f Mbps  OUT=%.2f Mbps\n", g_inMbps, g_outMbps);
      g_statusMsg = "OK";
      foundWan = true;
    }
  }

  if (wlanClients >= 0 || lanClients >= 0) {
    int total = 0;
    if (wlanClients >= 0) total += wlanClients;
    if (lanClients >= 0)  total += lanClients;
    g_clients = total;
  } else if (fallbackClients >= 0) {
    g_clients = fallbackClients;
  }

  if (!foundWan) {
    Serial.println("[Stats] WAN subsystem not found");
    g_statusMsg = "No WAN data";
    return false;
  }

  return true;
}

// ===========================================================================
// Display
// ===========================================================================

/*
 * Format a Mbps value into a compact human-readable string.
 */
static void formatRate(float mbps, char* buf, size_t bufLen) {
  if (mbps < 1.0f) {
    snprintf(buf, bufLen, "%.0f Kbps", mbps * 1000.0f);
  } else {
    snprintf(buf, bufLen, "%.2f Mbps", mbps);
  }
}

/*
 * Draw a subtle animated sine-wave background confined to one box.
 */
void drawSineInBox(int leftX, int rightX, int topY, int bottomY, float phase) {
  int w = rightX - leftX;
  int h = bottomY - topY;
  if (w < 10 || h < 8) return;

  float t = millis() * 0.0035f;
  float amp = h / 4.0f;
  if (amp < 2.0f) amp = 2.0f;

  for (int x = leftX + 1; x < rightX; x++) {
    float localX = (float)(x - leftX);
    float yMain = topY + (h * 0.42f) + sinf((localX * 0.22f) + t + phase) * amp;
    float ySub  = topY + (h * 0.68f) + sinf((localX * 0.12f) - (t * 1.5f) + phase) * (amp * 0.45f);

    int y1 = (int)yMain;
    int y2 = (int)ySub;

    if (y1 > topY && y1 < bottomY && (x % 2 == 0)) u8g2.drawPixel(x, y1);
    if (y2 > topY && y2 < bottomY && (x % 3 == 0)) u8g2.drawPixel(x, y2);
  }
}

void drawDisplay() {
  char bufIn[16], bufOut[16], clientBuf[20];
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

  // Sine-wave background per IN/OUT box only
  drawSineInBox(leftInnerL, leftInnerR, 12, 36, 0.0f);
  drawSineInBox(leftInnerL, leftInnerR, 38, 62, 1.5f);

  // Left column title + traffic values
  u8g2.setFont(u8g2_font_4x6_tf);
  const char* title = "INTERNET TRAFFIC";
  int titleW = u8g2.getStrWidth(title);
  u8g2.drawStr((splitX - titleW) / 2, 6, title);

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(4, 20, "\x19 IN");
  int inW = u8g2.getStrWidth(bufIn);
  u8g2.drawStr((splitX - inW) / 2, 33, bufIn);

  u8g2.drawStr(4, 46, "\x1a OUT");
  int outW = u8g2.getStrWidth(bufOut);
  u8g2.drawStr((splitX - outW) / 2, 59, bufOut);

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
  u8g2.setFont(u8g2_font_6x10_tf);
  int clientW = u8g2.getStrWidth(clientBuf);
  u8g2.drawStr(rightCenterX - (clientW / 2), 15, clientBuf);

  // ── Status (bottom-left, only on error) ──────────────────────────────────
  if (g_statusMsg != "OK") {
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(0, 63, g_statusMsg.c_str());
  }

  u8g2.sendBuffer();
}

/*
 * Full-screen error display used during startup / fatal errors.
 */
void drawError(const String& line1, const String& line2) {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_6x10_tf);

  // Line 1 – centered vertically, slightly above middle
  int l1W = u8g2.getStrWidth(line1.c_str());
  u8g2.drawStr((128 - l1W) / 2, line2.isEmpty() ? 35 : 27, line1.c_str());

  if (!line2.isEmpty()) {
    int l2W = u8g2.getStrWidth(line2.c_str());
    u8g2.drawStr((128 - l2W) / 2, 43, line2.c_str());
  }

  u8g2.sendBuffer();
}
