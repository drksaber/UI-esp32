# UniFi Traffic Monitor - ESP32 + SH1106 OLED

Displays **real-time internet traffic** (IN / OUT Mbps) from a **Ubiquiti UCG-MAX** on a 128x64 SH1106 OLED connected to an ESP32 over I2C. Also serves a built-in web dashboard with live graphs and full telemetry.

---

## Display layout

```
+---------------------------------+
|  INTERNET TRAFFIC  | Clients:   |
+--------------------+     12     |
| IN: 12.45 Mbps     | WAN UPTIME |
|   /\/\             |  99.82%    |
+--------------------+ WAN LATENCY|
| OUT: 3.56 Mbps     |   14.2ms   |
|   /\/\             |   USAGE    |
|                    |  432.5GB   |
+---------------------------------+
```

Split layout: **left 2/3** for traffic graphs, **right 1/3** for status panel.

- IN / OUT graphs scale independently based on peak traffic in the ring buffer
- Values switch automatically between **bps**, **Kbps**, and **Mbps**
- `WAN UPTIME` - rolling availability percentage from UCG-MAX monitoring probes (2 decimal places)
- `WAN LATENCY` - average latency from UCG-MAX alerting monitors
- `USAGE` - current month's total WAN bandwidth (IN + OUT) in GB or TB

### WAN DOWN overlay

When internet connectivity is lost, the normal display is replaced by a centered popup:

```
+---------------------------------+
|                                 |
|        +--------------+         |
|        |   WAN DOWN   |         |
|        |    2m 14s    |         |
|        +--------------+         |
|                                 |
+---------------------------------+
```

The counter increments in real time until connectivity is restored. Three failure modes are detected:

- **Port disabled in software** - `uptime_stats` becomes empty `{}`
- **Modem/ISP outage** - `uptime_stats.WAN.downtime` field appears
- **All alerting monitors failing** - all `alerting_monitors[*].availability == 0`

---

## Web dashboard

The ESP32 serves a built-in dashboard on port 80.

- Open: `http://<ESP32_IP>/`
- Live JSON API: `http://<ESP32_IP>/api/stats`

Dashboard cards:
- IN / OUT traffic values and live scrolling history graphs
- Client count
- WAN uptime percentage
- WAN latency
- Monthly bandwidth usage (GB)
- UCG-MAX CPU utilization, memory utilization, and temperature (°F)
- **ESP32 supply voltage** (requires optional voltage divider - see Hardware)
- Health panel: WiFi RSSI, IP, status, WAN down flag, fetch errors, free heap, uptime, ESP32 CPU utilization, ESP32 internal temperature, data age timestamps

---

## Hardware

| Component | Notes |
|-----------|-------|
| ESP32 (any variant) | Tested on ESP32-WROOM-32 |
| SH1106 128x64 OLED (I2C) | Address `0x3C` (or `0x3D`) |
| UCG-MAX | UniFi OS 5.x / Network 10.x |

### Wiring - display

```
SH1106 OLED          ESP32
------------------------------
VCC  ---------------  3.3 V
GND  ---------------  GND
SDA  ---------------  GPIO 21  (default; override in config.h)
SCL  ---------------  GPIO 22  (default; override in config.h)
```

### Wiring - power monitor (optional)

To monitor the ESP32 supply voltage, wire a resistor voltage divider from the supply rail to an ADC1 pin. Only ADC1 pins (GPIO 32–39) work alongside WiFi — ADC2 is blocked by the WiFi driver.

```
Supply rail (e.g. 5 V)
        |
       [R1]  e.g. 100 kΩ
        |
        +------- GPIO 35 (ADC1_CH7, default POWER_MONITOR_PIN)
        |
       [R2]  e.g. 100 kΩ
        |
       GND
```

Set `POWER_MONITOR_RATIO = (R1 + R2) / R2` in `config.h`. With equal resistors the ratio is `2.0`, giving a full-scale input of ~6.6 V (ESP32 ADC tops out at ~3.3 V). Set `POWER_MONITOR_PIN -1` to disable the feature entirely.

---

## Software requirements

Install via **Arduino IDE → Tools → Manage Libraries**:

| Library | Author | Purpose |
|---------|--------|---------|
| **U8g2** | oliver | SH1106 display driver |
| **ArduinoJson** | bblanchon | JSON parsing (v6 or v7) |

`WiFiClientSecure` and `HTTPClient` ship with the **ESP32 Arduino board package** — no separate install needed.

### Board package

1. In Arduino IDE open **File → Preferences** and add to *Additional board manager URLs*:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Open **Tools → Board → Boards Manager**, search for `esp32` and install **esp32 by Espressif Systems**.
3. Select **Tools → Board → ESP32 Arduino → ESP32 Dev Module** (or your specific variant).

---

## Configuration

Edit `UniFi_Traffic_Monitor/config.h` before flashing:

```cpp
// WiFi
#define WIFI_SSID       "your_network_name"
#define WIFI_PASSWORD   "your_wifi_password"

// UCG-MAX LAN IP or hostname
#define UNIFI_HOST      "192.168.0.1"

// HTTPS port
#define UNIFI_PORT      443

// UniFi OS API key  (UniFi OS -> Settings -> API Keys)
#define UNIFI_API_KEY   "your_api_key"

// Site (usually "default")
#define UNIFI_SITE      "default"

// I2C pins (ESP32 defaults)
#define OLED_SDA_PIN    21
#define OLED_SCL_PIN    22

// Poll interval (1000 ms recommended minimum)
#define POLL_INTERVAL_MS  1000

// Rebuild TLS after this many consecutive fetch failures
#define MAX_FETCH_ERRORS  3

// Power monitor - ADC1 pin + voltage divider ratio; -1 to disable
#define POWER_MONITOR_PIN    35
#define POWER_MONITOR_RATIO  2.0f

// Status LED (uses LED_BUILTIN by default)
#define STATUS_LED_ENABLED  1

// Optional web dashboard digest auth - leave blank for open access
#define WEB_AUTH_USER   ""
#define WEB_AUTH_PASS   ""

// BOOT button (GPIO0)
#define BOOT_BUTTON_ENABLED  1
```

---

## How it works

### Dual-core architecture

- **Core 0 - networkTask**: all HTTPS polling (traffic stats, monthly usage, controller telemetry, power monitoring). Blocking TLS calls here never stall the display.
- **Core 1 - loop**: OLED rendering, web server, LED state machine, BOOT button. Runs at 50 Hz; only redraws the OLED when new data arrives from Core 0.
- Shared state is protected by a FreeRTOS mutex (`g_dataMutex`). The mutex is released before the I2C transfer so the network task is never blocked by display I/O.

### Traffic data

`GET https://<UCG-MAX>/proxy/network/api/s/<site>/stat/health`

The `wan` subsystem entry contains `rx_bytes-r` (bytes/sec IN) and `tx_bytes-r` (bytes/sec OUT).
Conversion: `Mbps = bytes_per_sec × 8 / 1 000 000`

Client count is derived from `wlan` + `lan` subsystem user counts.

### WAN uptime

Parsed from `uptime_stats.WAN.uptime / uptime_stats.WAN.time_period × 100` — the same calculation the UCG-MAX display uses, expressed to 2 decimal places. Returns `--` when WAN is down (field absent).

### Monthly usage

`POST https://<UCG-MAX>/proxy/network/api/s/<site>/stat/report/monthly.gw`
Body: `{"attrs":["wan-rx_bytes","wan-tx_bytes"]}`

Returns the current month's gateway traffic totals. Polled every 5 minutes. Displayed in GB (auto-switches to TB above 1000 GB). Shows `--GB` until the first fetch completes (~5 min after boot).

### Power monitor

When `POWER_MONITOR_PIN` is set, the firmware averages 4 `analogReadMilliVolts()` samples every poll cycle and multiplies by `POWER_MONITOR_RATIO` to recover the actual supply voltage. The result is exposed on the web dashboard as **ESP Supply Voltage** (in volts). The feature compiles out entirely when `POWER_MONITOR_PIN` is `-1`.

### Controller resource stats

Polled every 10 seconds by cycling through three API endpoints:
- `/proxy/network/api/s/<site>/stat/sysinfo`
- `/api/system`
- `/proxy/network/api/s/<site>/stat/device`

Reports UCG-MAX CPU %, memory %, and temperature (°F) on the dashboard.

### Status LED

| State | Pattern |
|-------|---------|
| Connecting / working | Fast blink (250 ms) |
| WAN down | Rapid blink (70 ms) |
| Healthy | Heartbeat pulse every 30 s |
| API / connectivity error | Solid ON |

Set `STATUS_LED_ENABLED 0` to disable.

### BOOT button

Short press → shows local IP in a popup overlay on the OLED for ~5 seconds.
Set `BOOT_BUTTON_ENABLED 0` to disable.

### Security notes

- HTTPS uses `setInsecure()` (certificate validation disabled) to support self-signed UCG-MAX certs. Keep the ESP32 on a trusted LAN/VLAN.
- The API key is stored in plaintext in `config.h`; treat firmware source and flashed devices as sensitive.
- The web dashboard supports optional HTTP Digest Auth via `WEB_AUTH_USER` / `WEB_AUTH_PASS`.

---

## Flashing

1. Open the `UniFi_Traffic_Monitor/` folder as a sketch in Arduino IDE.
2. Edit `config.h` with your WiFi credentials, UCG-MAX IP, and API key.
3. Select **Tools → Port → \<your COM/tty port\>**.
4. Click **Sketch → Upload**.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| "WiFi failed" on screen | Wrong SSID/password, or ESP32 too far from AP |
| "Bad API key" on screen | Wrong or missing `UNIFI_API_KEY` in config.h |
| "No WAN data" | UCG-MAX WAN not active or site name is not `default` |
| Blank display | Check SDA/SCL wiring; verify I2C address (try `0x3D`) |
| "Conn error" looping | Network issue; TLS rebuilds automatically after `MAX_FETCH_ERRORS` failures |
| USAGE shows `--GB` | Monthly fetch not yet completed — appears within 5 minutes of boot |
| Supply voltage shows `--` | `POWER_MONITOR_PIN` is `-1`, or ADC pin not wired up |

### Wrong I2C address?

Run an I2C scanner sketch to confirm the address, then update the constructor in the `.ino`:

```cpp
u8g2.setI2CAddress(0x3D * 2);
```
