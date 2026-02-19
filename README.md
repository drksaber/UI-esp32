# UniFi Traffic Monitor – ESP32 + SH1106 OLED

Displays **real-time internet traffic** (IN ↓ / OUT ↑ Mbps) from a **Ubiquiti UCG-MAX** on a 128×64 SH1106 OLED connected to an ESP32 over I²C.

---

## Display layout

```
┌────────────────────────────────┐
│      INTERNET TRAFFIC          │
├────────────────────────────────┤
│ ▼ IN                           │
│         12.45 Mbps             │
├────────────────────────────────┤
│ ▲ OUT                          │
│          3.56 Mbps             │
└────────────────────────────────┘
```

Values automatically switch between **Kbps** and **Mbps** depending on the magnitude.

---

## Hardware

| Component | Notes |
|-----------|-------|
| ESP32 (any variant) | Tested on ESP32-WROOM-32 |
| SH1106 128×64 OLED (I²C) | Address `0x3C` (or `0x3D`) |
| UCG-MAX | UniFi OS 5.x / Network 10.x |

### Wiring

```
SH1106 OLED          ESP32
─────────────────────────────
VCC  ───────────────  3.3 V
GND  ───────────────  GND
SDA  ───────────────  GPIO 21  (default; override in config.h)
SCL  ───────────────  GPIO 22  (default; override in config.h)
```

---

## Software requirements

Install these libraries through **Arduino IDE → Tools → Manage Libraries**:

| Library | Author | Purpose |
|---------|--------|---------|
| **U8g2** | oliver | SH1106 display driver |
| **ArduinoJson** | bblanchon | JSON parsing (v6 or v7) |

The `WiFiClientSecure` and `HTTPClient` libraries ship with the **ESP32 Arduino board package** (no separate install needed).

### Board package

1. In Arduino IDE open **File → Preferences** and add to *Additional board manager URLs*:  
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Open **Tools → Board → Boards Manager**, search for `esp32` and install the **esp32 by Espressif Systems** package.
3. Select **Tools → Board → ESP32 Arduino → ESP32 Dev Module** (or your specific variant).

---

## Configuration

Edit `UniFi_Traffic_Monitor/config.h` before flashing:

```cpp
// WiFi
#define WIFI_SSID       "your_network_name"
#define WIFI_PASSWORD   "your_wifi_password"

// UCG-MAX LAN IP or hostname
#define UNIFI_HOST      "192.168.1.1"

// Local admin credentials
#define UNIFI_USER      "admin"
#define UNIFI_PASS      "your_password"

// Site (usually "default")
#define UNIFI_SITE      "default"

// I²C pins (ESP32 defaults)
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22

// Poll interval
#define POLL_INTERVAL_MS  2000
```

> **Tip:** Create a dedicated read-only local admin account in the UniFi console for this device instead of using your main account.

---

## How it works

### Authentication (API key)

Every request includes the header `X-API-KEY: <your key>`. No login step, no session cookies, no CSRF tokens, no expiry to manage.

### Traffic data

`GET https://<UCG-MAX>/proxy/network/api/s/<site>/stat/health`  
Headers: `X-API-KEY: …` and `Accept: application/json`  
→ Returns per-subsystem health. The `"wan"` entry contains:
- `rx_bytes-r` — bytes/sec received (download / IN)
- `tx_bytes-r` — bytes/sec transmitted (upload / OUT)

Conversion: `Mbps = bytes_per_sec × 8 ÷ 1 000 000`

---

## Flashing

```bash
# Open the sketch folder in Arduino IDE, configure config.h, then:
# Tools → Port → <your COM/tty port>
# Sketch → Upload
```

Monitor output via **Tools → Serial Monitor** at **115200 baud** to watch API responses and parsed values.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| "WiFi failed" on screen | Wrong SSID/password, or ESP32 too far from AP |
| "Bad API key" on screen | Wrong or missing `UNIFI_API_KEY` in config.h |
| "No WAN data" | UCG-MAX WAN interface not active or site name is not `default` |
| Blank display | Check SDA/SCL wiring; verify I²C address (try `0x3D` in U8g2 constructor) |
| "Conn error" looping | Network issue; sketch rebuilds TLS automatically after 3 failures |

### Wrong I²C address?

If the display stays blank, change the constructor in the `.ino` to use address `0x3D`:

```cpp
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, U8X8_PIN_NONE, U8X8_PIN_NONE);
// then call:
u8g2.setI2CAddress(0x3D * 2);
```

Or run an I²C scanner sketch to confirm the address before flashing.

Ubiquiti Internet traffic tracker on ESP32
