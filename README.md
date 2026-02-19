# UniFi Traffic Monitor – ESP32 + SH1106 OLED

Displays **real-time internet traffic** (IN ↓ / OUT ↑) from a **Ubiquiti UCG-MAX** on a 128×64 SH1106 OLED connected to an ESP32 over I²C.

---

## Display layout

```
┌────────────────────────────────┐
│ INTERNET TRAFFIC    │ Clients: │
├─────────────────────┤          │
│ ▼ IN                │    12    │
│   ~~~ 12.45 Mbps ~~ │          │
├─────────────────────┤          │
│ ▲ OUT               │          │
│   ~~~ 3.56 Mbps  ~~ │          │
└────────────────────────────────┘
```

The panel uses a split layout: **left 2/3** for internet traffic and **right 1/3** for clients.
The vertical divider spans the full panel height, with the client value slightly below `Clients:`.
Animated sine waves are drawn only inside the **IN** and **OUT** boxes.

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

// HTTPS port
#define UNIFI_PORT      443

// UniFi OS API key
#define UNIFI_API_KEY   "your_api_key"

// Site (usually "default")
#define UNIFI_SITE      "default"

// I²C pins (ESP32 defaults)
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22

// Poll interval (1000 ms recommended)
#define POLL_INTERVAL_MS  1000

// Rebuild TLS after this many consecutive fetch failures
#define MAX_FETCH_ERRORS  3
```

> **Tip:** Create a dedicated API key for this device in UniFi OS → Settings → API Keys.

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

The top line `Clients :#` is derived from health subsystem counts (`wlan`/`lan` user totals when available).

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
