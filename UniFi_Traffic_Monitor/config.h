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

// API key – generate in UniFi OS → Settings → API Keys
// (no username/password needed)
#define UNIFI_API_KEY   "YOUR_API_KEY_HERE"

// Site name – almost always "default" unless you renamed it
#define UNIFI_SITE      "default"

// =============================================================
//  Display – SH1106 128×64 I²C
// =============================================================
// Standard I²C address for SH1106 (0x3C or 0x3D)
#define OLED_I2C_ADDR   0x3C

// SDA / SCL pins – change if you are NOT using the ESP32 defaults
// Default ESP32: SDA=21, SCL=22
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22

// =============================================================
//  Polling Interval
// =============================================================
// How often (milliseconds) to query the UniFi API.
// 1000 ms is the practical minimum – the UCG-MAX bytes-r stat updates once/sec.
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

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// GPIO used for the status LED
#define STATUS_LED_PIN           LED_BUILTIN

// Set to 1 if your LED is active-low (ON = GPIO LOW)
#define STATUS_LED_ACTIVE_LOW    0

// While loading / connecting, LED blinks quickly
#define LED_WORKING_BLINK_MS     250UL

// If WAN is reported down, blink super fast
#define LED_WAN_DOWN_BLINK_MS    70UL

// Healthy state heartbeat blink interval
#define LED_OK_HEARTBEAT_MS      30000UL

// Width of the heartbeat pulse
#define LED_OK_PULSE_MS          120UL

// =============================================================
//  BOOT Button (GPIO0 on most ESP32 dev boards)
// =============================================================
// Set to 0 to ignore BOOT button input
#define BOOT_BUTTON_ENABLED         1
