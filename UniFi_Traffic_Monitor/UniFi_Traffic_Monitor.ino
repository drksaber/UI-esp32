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
static String g_statusMsg      = "Connecting...";
static unsigned long g_lastPoll = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
bool  wifiConnect();
void  initHttpClient();
void  closeConnection();
bool  fetchTrafficStats();
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
  for (JsonObject subsys : dataArr) {
    const char* name = subsys["subsystem"];
    if (name && strcmp(name, "wan") == 0) {
      // bytes per second → Mbps  (×8 / 1 000 000)
      float rxRate = subsys["rx_bytes-r"] | 0.0f;
      float txRate = subsys["tx_bytes-r"] | 0.0f;

      g_inMbps  = rxRate * 8.0f / 1000000.0f;
      g_outMbps = txRate * 8.0f / 1000000.0f;

      Serial.printf("[Stats] IN=%.2f Mbps  OUT=%.2f Mbps\n", g_inMbps, g_outMbps);
      g_statusMsg = "OK";
      foundWan = true;
      break;
    }
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
 * Draw a downward-pointing filled triangle (download arrow).
 * (cx, ty) = center-x, top y
 */
static void drawArrowDown(int cx, int ty) {
  // Triangle: flat top, point at bottom
  // Top-left (cx-5, ty), Top-right (cx+5, ty), Tip (cx, ty+8)
  u8g2.drawTriangle(cx - 5, ty, cx + 5, ty, cx, ty + 8);
}

/*
 * Draw an upward-pointing filled triangle (upload arrow).
 * (cx, by) = center-x, bottom y
 */
static void drawArrowUp(int cx, int by) {
  // Tip at top, flat base at bottom
  u8g2.drawTriangle(cx, by - 8, cx - 5, by, cx + 5, by);
}

/*
 * Format a Mbps value into a human-readable string.
 *   < 1 Mbps   → show in Kbps
 *   >= 1 Mbps  → "xxx.xx Mbps"
 */
static void formatMbps(float mbps, char* buf, size_t bufLen) {
  if (mbps < 1.0f) {
    float kbps = mbps * 1000.0f;
    snprintf(buf, bufLen, "%.1f Kbps", kbps);
  } else if (mbps < 10.0f) {
    snprintf(buf, bufLen, "%.2f Mbps", mbps);
  } else if (mbps < 100.0f) {
    snprintf(buf, bufLen, "%.1f Mbps", mbps);
  } else {
    snprintf(buf, bufLen, "%.0f Mbps", mbps);
  }
}

/* ─────────────────────────────────────────────────────────────────────────
 *  Main display routine
 *
 *  Layout (128×64)
 *  ┌────────────────────────────────┐  y=0
 *  │      INTERNET TRAFFIC          │  y≈9   (6×10 font, centered)
 *  ├────────────────────────────────┤  y=11  (horizontal rule)
 *  │ ▼ IN                           │  y=21  (label, 5×8 font)
 *  │       xxx.xx Mbps              │  y=34  (value, 8×13B font, centered)
 *  ├────────────────────────────────┤  y=37  (horizontal rule)
 *  │ ▲ OUT                          │  y=47  (label)
 *  │       xxx.xx Mbps              │  y=60  (value)
 *  └────────────────────────────────┘  y=63
 * ────────────────────────────────────────────────────────────────────────*/
void drawDisplay() {
  char bufIn[20], bufOut[20];
  formatMbps(g_inMbps,  bufIn,  sizeof(bufIn));
  formatMbps(g_outMbps, bufOut, sizeof(bufOut));

  u8g2.clearBuffer();

  // ── Title ────────────────────────────────────────────────────────────────
  u8g2.setFont(u8g2_font_6x10_tf);
  const char* title = "INTERNET TRAFFIC";
  int titleW = u8g2.getStrWidth(title);
  u8g2.drawStr((128 - titleW) / 2, 9, title);

  // ── Horizontal rules ─────────────────────────────────────────────────────
  u8g2.drawHLine(0, 11, 128);
  u8g2.drawHLine(0, 37, 128);

  // ── IN section ───────────────────────────────────────────────────────────
  // Arrow
  drawArrowDown(8, 14);      // tiny down-arrow at left

  // Label "IN"
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(18, 21, "IN");

  // Value (centered, bold, medium)
  u8g2.setFont(u8g2_font_8x13B_tf);
  int inW = u8g2.getStrWidth(bufIn);
  u8g2.drawStr((128 - inW) / 2, 34, bufIn);

  // ── OUT section ──────────────────────────────────────────────────────────
  // Arrow
  drawArrowUp(8, 48);        // tiny up-arrow at left

  // Label "OUT"
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(18, 47, "OUT");

  // Value
  u8g2.setFont(u8g2_font_8x13B_tf);
  int outW = u8g2.getStrWidth(bufOut);
  u8g2.drawStr((128 - outW) / 2, 60, bufOut);

  // ── Status dot (top-right corner, only when there's an issue) ────────────
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
