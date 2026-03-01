#pragma once

// =============================================================
//  WiFi Settings
// =============================================================
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// =============================================================
//  Ubiquiti UCG-MAX Settings
// =============================================================
// IP address (or hostname) of your UCG-MAX on the local network
#define UNIFI_HOST      "192.168.0.1"
#define UNIFI_PORT      443

// API key - generate in UniFi OS -> Settings -> API Keys
// (no username/password needed)
#define UNIFI_API_KEY   "YOUR_API_KEY_HERE"

// Site name - almost always "default" unless you renamed it
#define UNIFI_SITE      "default"

// =============================================================
//  Display - SH1106 128x64 I2C
// =============================================================
// Standard I2C address for SH1106 (0x3C or 0x3D)
#define OLED_I2C_ADDR   0x3C

// SDA / SCL pins - change if you are NOT using the ESP32 defaults
// Default ESP32: SDA=21, SCL=22
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22

// =============================================================
//  Polling Interval
// =============================================================
// How often (milliseconds) to query the UniFi API.
// 1000 ms is the practical minimum - the UCG-MAX bytes-r stat updates once/sec.
// The persistent TLS connection eliminates handshake overhead so 1 s is stable.
#define POLL_INTERVAL_MS  1000

// Consecutive fetch failures before the TLS connection is rebuilt
#define MAX_FETCH_ERRORS  3

// How often to check if UniFi OS / Network updates are available
// 30 minutes = 1,800,000 ms
#define UPDATE_CHECK_INTERVAL_MS  1800000UL

// =============================================================
//  Status LED
// =============================================================
// Set to 0 to disable all status LED behavior
#define STATUS_LED_ENABLED       1

// =============================================================
//  Web Dashboard Authentication
// =============================================================
// Set both to non-empty strings to require HTTP Digest Auth on / and /api/stats.
// Leave blank to allow unauthenticated access (default - LAN-trust assumption).
#define WEB_AUTH_USER   ""
#define WEB_AUTH_PASS   ""

// =============================================================
//  BOOT Button (GPIO0 on most ESP32 dev boards)
// =============================================================
// Set to 0 to ignore BOOT button input
#define BOOT_BUTTON_ENABLED         1


