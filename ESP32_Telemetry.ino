/*
  RC Balloon Telemetry (ESP32) — AP-Only Website + JSON + Static OLED Status Page + Ready LED + Flight Timer Control
  - DHT11 on GPIO19 (DATA) with a 10k pull-up resistor from DATA to 3.3V
  - I2C: SSD1306 (0x3C), MPL3115A2 (0x60), optional MLX90614 (0x5A/0x5B)
  - Wi-Fi: ESP32 hosts its own AP (no STA attempts)

  Endpoints:
    GET /           -> Full telemetry website (includes Flight Time + Reset Initial Altitude)
    GET /data.json  -> JSON

  JSON shape:
    {
      "envelopeTempF": <float|null>,  // MLX90614 object temp (°F)
      "ambientTempF":  <float|null>,  // DHT temp (°F) or MPL temp fallback
      "humidityPct":   <float|null>,  // DHT RH %
      "pressurehPa":   <float|null>,  // MPL pressure (hPa)
      "altitudeM":     <float|null>,  // MPL altitude (m)
      "timestamp":     <millis>
    }

  Board: DOIT ESP32 DEVKIT V1 (or ESP32 Dev Module)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_MPL3115A2.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// =================== USER CONFIG ===================
// Access Point (ESP32-hosted Wi-Fi). Set password "" for an OPEN AP.
#define AP_SSID      "P.R. NM: RC Balloon"
#define AP_PASSWORD  ""        // "" -> open AP; set to "StrongPass123" for WPA2

// If GPIO21/22 may be bad, set to 1 to use alternate pins 33/32:
#define USE_ALT_I2C_PINS 0

// ---- DHT: CNT5 in the kit is a DHT-family sensor. ----
#define DHT_TYPE DHT11
#define DHT_PIN  19               // DATA -> GPIO19 (D19) with 10k to 3V3
// ===================================================

// -------- Pins (I2C) --------
#if USE_ALT_I2C_PINS
  #define I2C_SDA 33
  #define I2C_SCL 32
#else
  #define I2C_SDA 21
  #define I2C_SCL 22
#endif

#define READY_LED_PIN 23

// -------- OLED setup --------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C   // change to 0x3D if your OLED scans there

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool g_oledOK = false;

// -------- Web --------
WebServer server(80);

// -------- Sensors --------
Adafruit_MPL3115A2 mpl;
bool mplOk = false;

DHT dht(DHT_PIN, DHT_TYPE);

// MLX90614 (SMBus manual reads) — optional
static uint8_t g_mlxAddr = 0;
static bool    g_mlxOK   = false;

static bool mlxReadWord(uint8_t addr, uint8_t reg, uint16_t &word){
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 3) != 3) return false; // LSB, MSB, PEC
  uint8_t lsb = Wire.read();
  uint8_t msb = Wire.read();
  (void)Wire.read(); // discard PEC
  word = (uint16_t)msb<<8 | lsb;
  return true;
}
static bool mlxReadTempC(uint8_t addr, float &tC, bool ambient){
  uint16_t raw = 0;
  uint8_t reg = ambient ? 0x06 : 0x07;
  if (!mlxReadWord(addr, reg, raw)) return false;
  tC = (raw * 0.02f) - 273.15f;
  return (tC > -100.0f && tC < 300.0f);
}

// -------- Telemetry state --------
volatile float s_alt_m     = NAN; // MPL altitude (m)
volatile float s_mplTempC  = NAN; // MPL temp (°C)
volatile float s_pres_hPa  = NAN; // MPL pressure (hPa)
volatile float s_dhtTempC  = NAN; // DHT ambient (°C)
volatile float s_dhtHum    = NAN; // DHT %RH
volatile float s_envObjC   = NAN; // MLX object temp (°C)

uint32_t g_lastSampleMs = 0;
const uint32_t TELEMETRY_PERIOD_MS = 1000;  // main loop update

// DHT pacing (DHT11 needs ~1s; we’ll give it margin + retries)
const uint32_t DHT_PERIOD_MS = 1200;
uint32_t g_lastDhtMs = 0;

bool readDhtOnce(float &tC, float &rh) {
  tC = dht.readTemperature();   // °C
  rh = dht.readHumidity();      // %
  return !isnan(tC) && !isnan(rh) && rh >= 0.0f && rh <= 100.0f;
}

// ================= Overheat thresholds for MLX object temperature =================
const float OVERHEAT_ON_F  = 200.0f;  // blink fast when at/above this
const float OVERHEAT_OFF_F = 195.0f;  // return to solid when at/below this
static bool g_overheat = false;


// -------- Utils --------
static inline float c2f(float c){ return c * 9.0f/5.0f + 32.0f; }
static float scalePressureToHectoPascal(float raw){
  if (raw > 2000.0f) return raw / 100.0f; // Pa -> hPa
  if (raw < 50.0f)   return raw * 100.0f; // kPa -> hPa
  return raw;
}

// ================= READY LED =================
enum LedMode : uint8_t { LED_OFF=0, LED_SOLID, LED_BLINK_SLOW, LED_BLINK_FAST, LED_DOUBLE };
volatile uint8_t g_ledMode = LED_DOUBLE; // AP heartbeat at boot
static inline void setLedMode(uint8_t m){ g_ledMode = m; }
void serviceReadyLed(){
  static bool level = false;
  uint32_t now = millis();
  switch(g_ledMode){
    case LED_OFF:         level = false; break;
    case LED_SOLID:       level = true;  break;
    case LED_BLINK_SLOW: {  // ~1 Hz
      const uint32_t period = 1000;
      level = ((now / (period/2)) & 1);
    } break;
    case LED_BLINK_FAST: {  // ~4 Hz
      const uint32_t period = 100;
      level = ((now / (period/2)) & 1);
    } break;
    case LED_DOUBLE: {      // two quick pulses every 1.2 s
      const uint32_t cycle = 1200;
      uint32_t t = now % cycle;
      level = (t < 120) || (t >= 240 && t < 360);
    } break;
    default: level = false; break;
  }
  digitalWrite(READY_LED_PIN, level ? HIGH : LOW);
}

// ================= I2C helpers =================
static void i2cBusRecover(int sda=I2C_SDA, int scl=I2C_SCL){
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delay(2);
  if (digitalRead(sda)==LOW){
    pinMode(scl, OUTPUT);
    for (int i=0;i<20 && digitalRead(sda)==LOW;i++){
      digitalWrite(scl, HIGH); delayMicroseconds(6);
      digitalWrite(scl, LOW);  delayMicroseconds(6);
    }
    pinMode(sda, OUTPUT); digitalWrite(sda, LOW);
    digitalWrite(scl, HIGH); delayMicroseconds(6);
    pinMode(sda, INPUT_PULLUP);
  }
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
}
static void i2cScan(){
  Serial.println(F("[SCAN] I2C scan..."));
  uint8_t count=0;
  for (uint8_t addr=0x08; addr<=0x77; addr++){
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err==0){
      Serial.print(F("[SCAN] 0x"));
      if (addr<16) Serial.print('0');
      Serial.println(addr, HEX);
      count++;
    }
  }
  if (!count) Serial.println(F("[SCAN] No I2C devices found"));
  else        Serial.printf("[SCAN] Found %u device(s)\n", count);
}
static bool i2cAck(uint8_t addr){
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ================= OLED helpers =================
uint32_t g_lastOledMs = 0;
const uint32_t OLED_PERIOD_MS = 1000; // refresh the static status page ~1 Hz

void oledLines(const char* l1, const char* l2=nullptr, const char* l3=nullptr, const char* l4=nullptr, const char* l5=nullptr) {
  if (!g_oledOK) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  if (l1) display.println(l1);
  if (l2) display.println(l2);
  if (l3) display.println(l3);
  if (l4) display.println(l4);
  if (l5) display.println(l5);
  display.display();
}
void oledBoot() {
  char b2[32]; snprintf(b2, sizeof(b2), "Build: %s", __TIME__);
  char b3[32]; snprintf(b3, sizeof(b3), "Date : %s", __DATE__);
  oledLines("ESP32 Telemetry (AP)", b2, b3, " (If stuck here check wires)");
}
void oledWifiAP(IPAddress ip){
  char ln[32]; snprintf(ln, sizeof(ln), "IP: %s", ip.toString().c_str());
  oledLines("WiFi AP ready", ln, "SSID: " AP_SSID);
}
void oledSensorSummary(bool mplOk_, bool mlxOk_, bool dhtOk_){
  char a[22], b[22], c[22];
  snprintf(a, sizeof(a), "MPL3115A2: %s", mplOk_?"OK":"--");
  snprintf(b, sizeof(b), "MLX90614 : %s", mlxOk_?"OK":"--");
  snprintf(c, sizeof(c), "DHT11    : %s", dhtOk_?"OK":"--");
  oledLines("Sensors:", a, b, c);
}

// Centered big text (fixed max/std::max type issue)
void oledBigCentered(const char* msg) {
  if (!g_oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  int16_t cx = (int16_t)((SCREEN_WIDTH  - (int)w) / 2);
  int16_t cy = (int16_t)((SCREEN_HEIGHT - (int)h) / 2);
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  display.setCursor(cx, cy);
  display.println(msg);
  display.display();
}

// ================= Static OLED Status Page =================
static IPAddress g_apIP(0,0,0,0);

// Convert bool -> "[OK]" or "[  ]"
static const char* okBox(bool ok){ return ok ? "[OK]" : "[X]"; }

// Draw: 
// STATUS: Live
// SSID: <...>
// Website: <ip>
// ---------------------
// MPL: [OK]    MLX: [  ]
// DHT11: [OK]
void oledStatusPage(){
  if (!g_oledOK) return;

  // Determine "online" from current data (not just ACK)
  bool mplOnline = mplOk && isfinite(s_alt_m) && isfinite(s_pres_hPa);
  bool dhtOnline = isfinite(s_dhtTempC) && isfinite(s_dhtHum);
  bool mlxOnline = g_mlxOK && isfinite(s_envObjC);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);

  display.println("STATUS: Live");
  display.print("SSID: "); display.println(AP_SSID);
  display.print("Website: "); display.println(g_apIP.toString());
  display.println("---------------------");

  // Row 1: MPL + MLX
  display.print("MPL: "); display.println(okBox(mplOnline));

  display.print("MLX: "); display.println(okBox(mlxOnline));

  // Row 2: DHT11
  display.print("DHT11: "); display.println(okBox(dhtOnline));

  display.display();
}

// ================= OLED DIAGNOSTIC HELPERS =================
void flashReady(uint8_t times = 3, uint16_t onMs = 300, uint16_t offMs = 220) {
  for (uint8_t i = 0; i < times; ++i) {
    oledBigCentered("READY");
    digitalWrite(READY_LED_PIN, HIGH);
    delay(onMs);
    display.clearDisplay(); display.display();
    digitalWrite(READY_LED_PIN, LOW);
    delay(offMs);
  }
}

// Show "Website live" briefly (we'll end on static status page after)
void oledWebsiteLive(IPAddress ip) {
  if (!g_oledOK) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("WEBSITE LIVE");
  display.print("SSID: "); display.println(AP_SSID);
  if (AP_PASSWORD[0] == '\0') display.println("PW  : (OPEN)");
  else { display.print("PW  : "); display.println(AP_PASSWORD); }
  display.print("URL : http://"); display.println(ip.toString());
  display.println("JSON: /data.json");
  display.display();
}

// ================= SENSOR SELF-TEST =================
struct SelfTest { bool mpl=false; bool dht=false; bool mlx=false; };

SelfTest runSensorSelfTest(uint32_t timeoutMs = 1800) {
  SelfTest st;
  uint32_t t0 = millis();

  // MPL3115A2
  if (mplOk) {
    while (millis() - t0 < timeoutMs) {
      float a = mpl.getAltitude();
      float p = mpl.getPressure();
      float t = mpl.getTemperature();
      if (isfinite(a) && isfinite(p) && isfinite(t)) { st.mpl = true; break; }
      delay(80);
    }
  }

  // DHT11
  {
    float tC, rh;
    for (int i=0; i<5 && !st.dht; ++i) {
      if (readDhtOnce(tC, rh) && isfinite(tC) && isfinite(rh)) st.dht = true;
      else delay(150);
    }
  }

  // MLX90614 (object temp)
  if (g_mlxOK) {
    float tC; st.mlx = mlxReadTempC(g_mlxAddr, tC, /*ambient=*/false) && isfinite(tC);
  }

  return st;
}

// ================= Web: HTML + JSON =================

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>RC Balloon Telemetry (ESP32)</title>
  <style>
    :root{
      /* Okabe–Ito palette for colorblind accessibility */
      --panel: rgba(255,255,255,.9);
      --text: #0f172a;
      --muted: #475569;

      /* Accessible status hues (not red/green) */
      --ok:   #0072B2;  /* blue */
      --warn: #D55E00;  /* orange */

      --sky-top: #cfeaff; --sky-mid: #8bd1ff; --sky-bot: #6bb8f5;

      --mt1: #4b6ea9; --mt2: #3f5f94; --mt3: #355383;

      --ground1: #2e7d32; --ground2: #1b5e20;

      --btn: #0072B2; 
      --btnHover: #005b8a;
    }
    *{box-sizing:border-box}
    html,body{height:100%;margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Cantarell,Noto Sans,sans-serif;color:var(--text)}
    body{
      background:#6bb8f5;
      overflow:auto;
      overflow-x:hidden;
    }

    /* ===== Background layers (fixed) ===== */
    /* Background sky can stay fixed (optional), but it’s fine as-is */
    .sky{
      position:fixed; inset:0; z-index:-3; pointer-events:none;
      background:linear-gradient(to bottom,var(--sky-top),var(--sky-mid),var(--sky-bot));
    }

    /* Clouds: in normal flow, full width bar above the balloon */
    .clouds{
      position:relative; width:100%; height:34vh; min-height:180px; 
      z-index:0; pointer-events:none; /* behind the balloon */
      margin-bottom:-6vh;             /* gentle overlap with scene */
    }
    .clouds svg{ width:100%; height:100%; display:block; }
    .cloud{ fill:#fff; opacity:.92; filter:drop-shadow(0 6px 10px rgba(255,255,255,.35)); }

    /* Mountains: NORMAL FLOW (like grass), full width, sits right above grass */
    .mountains{
      position:relative; width:100%; height:28vh; min-height:160px; max-height:360px;
      z-index:0; pointer-events:none; /* behind the UI */
      margin-top:6vh;                 /* gives a little breathing room under balloon */
      margin-bottom:-2px;             /* ensures it "touches" the grass with no gap */
    }
    .mountains svg{ width:100%; height:100%; display:block; }

    /* Keep your original grass as-is */
    .ground{
      background:linear-gradient(to bottom,var(--ground1),var(--ground2));
      clip-path:polygon(0 0,100% 12%,100% 100%,0 100%);
      box-shadow: 0 -10px 30px rgba(0,0,0,.25) inset;
    }



    /* ===== Layout ===== */
    .wrap{position:relative;min-height:100%;width:100%;display:grid;grid-template-rows:auto 120px}
    .scene{position:relative;display:grid;grid-template-columns:1fr 420px;gap:20px;align-items:center;padding:16px 20px;max-width:1200px;margin:0 auto;width:100%}
    @media (max-width: 1000px){.scene{grid-template-columns:1fr}}
    .right-col{display:flex;flex-direction:column;gap:12px}

    .ground{
      background:linear-gradient(to bottom,var(--ground1),var(--ground2));
      clip-path:polygon(0 0,100% 12%,100% 100%,0 100%);
      box-shadow: 0 -10px 30px rgba(0,0,0,.25) inset;
    }

    .badges-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
    .badge{display:flex;align-items:center;gap:8px;background:var(--panel);border:1px solid rgba(0,0,0,.06);border-radius:999px;padding:10px 14px;font-size:16px}
    .label{font-size:14px;color:var(--muted)}
    .source-note{font-size:12px;color:var(--muted);margin-left:6px}
    .value{font-weight:800;font-size:20px}
    .card{background:var(--panel);border:1px solid rgba(0,0,0,.06);border-radius:18px;box-shadow:0 8px 20px rgba(0,0,0,.12);padding:16px}
    .metrics h2{margin:0 0 8px 0;font-size:22px}
    .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
    .metric{background:#fff;border:1px solid rgba(0,0,0,.06);border-radius:12px;padding:12px}
    .metric .name{font-size:14px;color:var(--muted)}
    .metric .num{font-size:22px;font-weight:800}
    .note{font-size:12px;color:var(--muted);margin-top:4px}
    .speed-row{display:flex;align-items:center;justify-content:space-between;margin-top:6px}
    .speed-val{font-size:22px;font-weight:800}
    .speed-val.up{color:var(--ok)} 
    .speed-val.down{color:var(--warn)}
    .bar{height:12px;background:#e5e7eb;border-radius:999px;overflow:hidden;margin-top:8px}
    .bar > div{height:100%;transition:width .25s ease}
    .stage{background:transparent;border:0;padding:0;position:relative}

    /* Balloon area */
    .balloon-wrap{position:relative;display:flex;align-items:center;justify-content:center;min-height:560px;height:62vh}
    /* Scale the SVG 1.5x (envelope + basket), without touching the overlay pill */
    .balloon-wrap svg{
      transform: scale(1.2);
      transform-box: view-box;
      transform-origin: 50% 45%;
    }

    svg{max-width:460px;width:100%;height:auto;filter: drop-shadow(0 18px 28px rgba(0,0,0,.22))}
    .overlay-pill{position:absolute;top:8px;left:50%;transform:translateX(-50%);background:var(--panel);border:1px solid rgba(0,0,0,.06);border-radius:999px;padding:8px 14px;font-size:24px;font-weight:800}

    @media (max-width: 600px){
      .badge{font-size:15px;padding:10px 12px}
      .value{font-size:18px}
      .metric .num{font-size:20px}
      .overlay-pill{font-size:20px; top:10px}
      svg{max-width:360px}
      .balloon-wrap{min-height:420px;height:60vh}
      .balloon-wrap svg{ transform: scale(1); } /* a bit smaller on small screens */
    }

    .controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
    .btn{appearance:none;border:0;border-radius:999px;background:var(--btn);color:#fff;font-weight:800;padding:12px 16px;cursor:pointer;box-shadow:0 6px 16px rgba(0,0,0,.25)}
    .btn:hover{background:var(--btnHover)}
    .time-pill{display:flex;align-items:center;gap:8px;background:var(--panel);border:1px solid rgba(0,0,0,.06);border-radius:999px;padding:10px 14px;font-weight:800}

    .lift-line{display:flex;align-items:center;gap:8px}
    .lift-line .arrow{font-size:22px;line-height:1}
    @media (max-width: 600px){
      .lift-line .arrow{font-size:20px}
    }

    /* Temperature mini-chart (smaller) */
    .chart-line{
      width:100%;
      height:100px;
      display:block;
      margin-top:6px;
    }
    .chart-controls{
      display:flex; align-items:center; gap:8px; margin-top:6px;
      font-size:12px; color:var(--muted);
    }
    .chart-controls select, .chart-controls input[type="number"]{
      appearance:none; border:1px solid rgba(0,0,0,.12); background:#fff; border-radius:8px; padding:6px 8px; font-weight:600; width:auto;
    }

    /* Tiny link status under metrics */
    .link-mini{
      display:inline-flex; align-items:center; gap:6px;
      font-size:11px; color:#334155; background:var(--panel);
      border:1px solid rgba(0,0,0,.08); border-radius:999px; padding:4px 8px; margin-top:8px;
    }
    .link-mini .led{width:7px;height:7px;border-radius:50%;display:inline-block}
    .link-mini.live .led{background:var(--ok)}
    .link-mini.degraded .led{background:var(--warn)}
    .link-mini.offline .led{background:#94a3b8}

    /* Hot envelope blink when > 200°F */
    @keyframes hotBlink {
      0% { fill:#fde047; }   /* warm yellow */
      50% { fill:#dc2626; }  /* red */
      100% { fill:#fde047; }
    }
    .hot-blink { animation: hotBlink 0.6s steps(2, start) infinite; }
  </style>
</head>
<body>
  <!-- Sky -->
  <div class="sky"></div>

  <div class="wrap">
    <div class="scene">
      <div class="stage">
        <div class="balloon-wrap">
          <svg viewBox="0 0 240 340" aria-label="Hot Air Balloon">
            <defs>
              <linearGradient id="shine" x1="0" x2="1">
                <stop offset="0%" stop-color="white" stop-opacity="0.25" />
                <stop offset="100%" stop-color="white" stop-opacity="0" />
              </linearGradient>
            </defs>
            <g id="envelope-group">
              <ellipse cx="120" cy="110" rx="90" ry="100" id="envelope" fill="#3b82f6" />
              <ellipse cx="85" cy="80" rx="35" ry="50" fill="url(#shine)" />
            </g>
            <rect x="95" y="210" width="50" height="12" rx="6" fill="#111827" opacity="0.7" />
            <path d="M102 222 L100 260 M138 222 L140 260" stroke="#4b5563" stroke-width="3" />
            <ellipse cx="120" cy="222" rx="12" ry="6" fill="#f59e0b" opacity="0.6" />
            <g>
              <rect x="90" y="260" width="60" height="40" rx="8" fill="#8b5a2b" />
              <rect x="95" y="265" width="50" height="8" rx="4" fill="#a9713c" />
              <rect x="95" y="278" width="50" height="18" rx="4" fill="#6d4221" />
            </g>
          </svg>

          <div class="overlay-pill"><span class="label">Envelope: </span><strong id="envTemp">-- °F</strong></div>
        </div>
      </div>

      <div class="right-col">
        <!-- Ambient row -->
        <div class="badges-row">
          <div class="badge">
            <span class="label">Ambient</span>
            <span class="value" id="ambient">-- °F</span>
            <span class="source-note" id="ambientSrcNote"></span>
          </div>
        </div>

        <!-- Humidity + Pressure row (Pressure in atm) -->
        <div class="badges-row">
          <div class="badge"><span class="label">Humidity</span> <span class="value" id="humidity">-- %</span></div>
          <div class="badge"><span class="label">Pressure</span> <span class="value" id="pressure">-- atm</span></div>
        </div>

        <!-- Controls / Flight Time -->
        <div class="controls">
          <button class="btn" id="btnReset">Start Flight / Reset Altitude</button>
          <div class="time-pill"><span class="label">Flight Time: </span> <span id="flightTime">00:00:00</span></div>
        </div>

        <!-- Unified Flight Metrics card (includes temp chart + inputs) -->
        <div class="card metrics">
          <h2>Flight Metrics</h2>

          <!-- Small input controls -->
          <div class="chart-controls" style="margin-bottom:6px">
            <span>Envelope Ø (ft):</span>
            <input type="number" id="envelopeInput" min="1" max="200" step="0.5" value="20" />
            <span>· Temp Window:</span>
            <select id="tempWindow">
              <option value="2">2 min</option>
              <option value="5" selected>5 min</option>
              <option value="10">10 min</option>
              <option value="15">15 min</option>
            </select>
          </div>

          <div class="grid">
            <div class="metric"><div class="name">Altitude (current)</div><div class="num" id="altNow">-- ft</div></div>
            <div class="metric"><div class="name">Initial Altitude</div><div class="num" id="altInit">-- ft</div></div>
            <div class="metric"><div class="name">Δ vs. Initial</div><div class="num" id="altDelta">-- ft</div></div>
            <div class="metric">
              <div class="name">Vertical Speed</div>
              <div class="speed-row">
                <div class="speed-val" id="vSpeed">-- ft/s</div>
              </div>
              <div class="bar"><div id="vBar" style="width:50%;"></div></div>
            </div>
          </div>

          <h2 style="margin-top:12px">Estimated Lift (<span id="envLabel">—</span> Envelope)</h2>
          <div class="grid">
            <div class="metric">
              <div class="name">Envelope Volume</div>
              <div class="num" id="volFt3">-- ft³</div>
              <div class="note" id="volEq">V = (4/3)πr³, r = D/2</div>
            </div>
            <div class="metric">
              <div class="name">Lift (pounds-force)</div>
              <div class="num lift-line"><span class="arrow">↑</span><span id="liftLbf">-- lbf</span></div>
              <div class="note">≈ <span id="liftN_note">-- N</span></div>
              <div class="note" id="liftEq">L = (ρ_out − ρ_in)·V·g</div>
            </div>
          </div>

          <!-- Temperature history (inside same card, smaller) -->
          <h2 style="margin-top:12px"><div class="note" id="tempHeader">Ambient vs. Envelope (last 5 min, °F)</div></h2>
          <div style="margin-top:12px">
            <canvas id="tempChart" class="chart-line" aria-label="Temperature history"></canvas>
          </div>

          <!-- Tiny link status under the card -->
          <div class="link-mini offline" id="linkStatus" title="Polling /data.json for liveness">
            <span class="led" aria-hidden="true"></span>
            <span id="linkLabel">Offline</span>
            <span id="linkDetail">RTT — · age —</span>
          </div>
        </div>
      </div>
    </div>

    <div class="ground"></div>
  </div>

  <script>
    // ======= EASY KNOBS (mutable) =======
    let ENVELOPE_DIAMETER_FT = 20;
    let TEMP_WINDOW_MINUTES = 5;  // Default; user can change via dropdown

    // ----- constants -----
    const MIN_F = 70, MAX_F = 220;
    const R = 287.058, G = 9.80665;
    const FT_TO_M = 0.3048, M_TO_FT = 3.280839895013123;
    const M3_TO_FT3 = 35.314667;
    const HPA_PER_ATM = 1013.25;

    // Derived geometry (SI for physics) — recomputed when diameter changes
    let DIAMETER_M, RADIUS_M, VOLUME_M3, VOLUME_FT3, RADIUS_FT;
    function recomputeFromDiameter(){
      DIAMETER_M = ENVELOPE_DIAMETER_FT * FT_TO_M;
      RADIUS_M   = DIAMETER_M / 2;
      VOLUME_M3  = (4/3) * Math.PI * Math.pow(RADIUS_M, 3);
      VOLUME_FT3 = VOLUME_M3 * M3_TO_FT3;
      RADIUS_FT  = ENVELOPE_DIAMETER_FT / 2;
    }
    recomputeFromDiameter();

    // Telemetry state
    const state = { envelopeTempF: NaN, ambientTempF: NaN, humidityPct: NaN, pressurehPa: NaN, altitudeM: NaN, timestamp: Date.now(), ambientSource: "" };
    let initialAlt = null, verticalSpeed = 0;

    // Flight timer
    let flightStartMs = null;
    const fmt2 = (n)=>String(n).padStart(2,'0');
    function fmtDuration(ms){
      if (!Number.isFinite(ms) || ms < 0) return "00:00:00";
      const s = Math.floor(ms/1000);
      const hh = Math.floor(s/3600);
      const mm = Math.floor((s%3600)/60);
      const ss = s%60;
      return `${fmt2(hh)}:${fmt2(mm)}:${fmt2(ss)}`;
    }

    const $ = (id)=>document.getElementById(id);
    const envEl=$("envelope"), envTemp=$("envTemp"), ambient=$("ambient"), humidity=$("humidity"), pressure=$("pressure");
    const altNow=$("altNow"), altInit=$("altInit"), altDelta=$("altDelta"), vSpeed=$("vSpeed"), vBar=$("vBar");
    const envLabel=$("envLabel"), volFt3=$("volFt3"), liftLbf=$("liftLbf"), liftN_note=$("liftN_note");
    const ambientSrcNote = $("ambientSrcNote");
    const volEq = $("volEq"), liftEq = $("liftEq");
    const flightTimeEl = $("flightTime");
    const btnReset = $("btnReset");
    const envelopeInput = $("envelopeInput");

    // Link status (tiny)
    const linkStatus = $("linkStatus");
    const linkLabel  = $("linkLabel");
    const linkDetail = $("linkDetail");

    // Chart bits
    const tempCanvas = document.getElementById("tempChart");
    const tempHeader = document.getElementById("tempHeader");
    const tempWindowSel = document.getElementById("tempWindow");

    const clamp=(x,min,max)=>Math.min(max,Math.max(min,x));
    const f2c=(f)=> (f-32)*5/9, c2k=(c)=> c+273.15, lerp=(a,b,t)=>a+(b-a)*t;
    function hexToRgb(hex){ const m=/^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex); return m?{r:parseInt(m[1],16),g:parseInt(m[2],16),b:parseInt(m[3],16)}:{r:0,g:0,b:0}; }
    function lerpColor(c1,c2,t){const a=hexToRgb(c1),b=hexToRgb(c2);return `rgb(${Math.round(lerp(a.r,b.r,t))}, ${Math.round(lerp(a.g,b.g,t))}, ${Math.round(lerp(a.b,b.b,t))})`;}
    function fmtTempF(x){return Number.isFinite(x)? x.toFixed(1)+" °F" : "-- °F"}
    function fmtPct(x){return Number.isFinite(x)? x.toFixed(0)+" %" : "-- %"}
    function fmtAtm(x){return Number.isFinite(x)? (x/HPA_PER_ATM).toFixed(3)+" atm" : "-- atm"}
    function fmtFeet(x){return Number.isFinite(x)? (x*M_TO_FT).toFixed(1)+" ft" : "-- ft"}
    function fmtFeetDelta(x){return Number.isFinite(x)? (x*M_TO_FT).toFixed(1)+" ft" : "-- ft"}
    function fmtFpsFromMs(x){return Number.isFinite(x)? (x*M_TO_FT).toFixed(2)+" ft/s" : "-- ft/s";}
    function densityAir(pressureHpa, tempF){ const P=(Number(pressureHpa)||1013.25)*100; const T=c2k(f2c(Number(tempF)||20)); return P/(R*T); }

    // Lift formatter
    function fmtLift(val, unit, decimals){
      if (!Number.isFinite(val)) return `-- ${unit}`;
      const s = val.toFixed(decimals)+" "+unit;
      return (val < 0) ? `${s} (0)` : s;
    }

    function computeLift(ambF, envF, pressureHpa){
      const rhoOutVal = densityAir(pressureHpa, ambF);
      const rhoInVal  = densityAir(pressureHpa, envF);
      const L = (rhoOutVal - rhoInVal) * VOLUME_M3 * G; // N
      return { newtons: L, lbf: L/4.4482216152605 };
    }

    function skyGradientForAmbient(f){
      const t = clamp((Number(f||0) - 10)/(100-10), 0, 1);
      const cTop='#4b77b3', cMid='#2f5e98', cBot='#1f3f6f';
      const wTop='#cfeaff', wMid='#8bd1ff', wBot='#6bb8f5';
      const skyEl = document.querySelector('.sky');
      if (skyEl) {
        skyEl.style.background = `linear-gradient(to bottom, ${lerpColor(cTop,wTop,t)}, ${lerpColor(cMid,wMid,t)}, ${lerpColor(cBot,wBot,t)})`;
      }
    }

    function render(){
      // Envelope diameter label + volume equation
      envLabel.textContent = ENVELOPE_DIAMETER_FT.toFixed(1) + "-ft";
      if (volEq) volEq.textContent = `V = (4/3)πr³, r = D/2 = ${RADIUS_FT.toFixed(1)} ft`;
      if (liftEq) liftEq.textContent = "L = (ρ_out − ρ_in)·V·g";

      // Envelope color or blink when >200F
      const isHot = Number.isFinite(state.envelopeTempF) && state.envelopeTempF > 200;
      envEl.classList.toggle("hot-blink", !!isHot);
      if (!isHot){
        const t = clamp((state.envelopeTempF - MIN_F)/(MAX_F - MIN_F), 0, 1);
        envEl.removeAttribute("style");
        envEl.setAttribute("fill", lerpColor("#3b82f6","#ef4444", t));
      } else {
        envEl.removeAttribute("fill");
      }

      envTemp.textContent=fmtTempF(state.envelopeTempF);
      ambient.textContent=fmtTempF(state.ambientTempF);
      humidity.textContent=fmtPct(state.humidityPct);
      pressure.textContent=fmtAtm(state.pressurehPa);
      ambientSrcNote.textContent = state.ambientSource ? `(from ${state.ambientSource})` : "";
      skyGradientForAmbient(state.ambientTempF);

      const altM = Number(state.altitudeM);
      const initM = Number(initialAlt);
      const dAlt = (Number.isFinite(altM) && Number.isFinite(initM)) ? (altM - initM) : NaN;
      altNow.textContent  = fmtFeet(altM);
      altInit.textContent = fmtFeet(initM);
      altDelta.textContent= fmtFeetDelta(dAlt);

      const rangeFtPerS = 16.4; // ~5 m/s
      const vFps = Number.isFinite(verticalSpeed) ? verticalSpeed*M_TO_FT : NaN;
      const tt=Math.min(1,Math.max(0,((Number.isFinite(vFps)?vFps:0)+rangeFtPerS)/(2*rangeFtPerS)));
      vBar.style.width=(tt*100).toFixed(1)+"%";
      vBar.style.background=(vFps>=0)
        ? "linear-gradient(90deg,#0072B2,#56B4E9)"
        : "linear-gradient(90deg,#D55E00,#E69F00)";
      vSpeed.textContent=fmtFpsFromMs(verticalSpeed);
      vSpeed.className="speed-val "+(vFps>=0?"up":"down");

      const L=computeLift(state.ambientTempF,state.envelopeTempF,state.pressurehPa);
      volFt3.textContent = Number.isFinite(VOLUME_FT3) ? VOLUME_FT3.toFixed(0)+" ft³" : "-- ft³";
      liftLbf.textContent = fmtLift(L.lbf, "lbf", 2);
      liftN_note.textContent = fmtLift(L.newtons, "N", 1);

      // Temp header text
      const mins = Math.round(tempWindowMs/60000);
      if (tempHeader) tempHeader.textContent = `Ambient vs. Envelope (last ${mins} min, °F)`;

      // Flight Time display
      if (flightStartMs!==null){
        flightTimeEl.textContent = fmtDuration(Date.now() - flightStartMs);
      } else {
        flightTimeEl.textContent = "00:00:00";
      }
    }

    // ======= LINK / LIVENESS (tiny) =======
    let lastOkTs = 0;
    const recentRtts = []; // ms, last 20
    function setLinkStatus(nowMs){
      const age = nowMs - lastOkTs;
      let cls = "offline", label = "Offline";
      if (age < 3000){ cls = "live"; label = "Live"; }
      else if (age < 8000){ cls = "degraded"; label = "Degraded"; }

      linkStatus.classList.remove("live","degraded","offline");
      linkStatus.classList.add(cls);
      linkLabel.textContent = label;

      const avg = recentRtts.length ? Math.round(recentRtts.reduce((a,b)=>a+b,0)/recentRtts.length) : NaN;
      const rttText = Number.isFinite(avg) ? `${avg} ms` : "—";
      linkDetail.textContent = `RTT ${rttText} · age ${(age/1000).toFixed(1)}s`;
    }

    // ======= Temperature history (window selectable) =======
    let tempWindowMs = TEMP_WINDOW_MINUTES * 60 * 1000;
    const tempHist = []; // { t, ambF, envF }

    function pushTempSample(ts, ambF, envF){
      tempHist.push({ t: ts, ambF, envF });
      const cutoff = ts - tempWindowMs;
      while (tempHist.length && tempHist[0].t < cutoff) tempHist.shift();
    }

    function sizeCanvasForDPR(canvas){
      if (!canvas) return 1;
      const dpr = window.devicePixelRatio || 1;
      const rect = canvas.getBoundingClientRect();
      const needW = Math.max(1, Math.floor(rect.width  * dpr));
      const needH = Math.max(1, Math.floor(rect.height * dpr));
      if (canvas.width !== needW || canvas.height !== needH){
        canvas.width = needW; canvas.height = needH;
      }
      return dpr;
    }

    function drawTempChart(){
      const canvas = tempCanvas;
      if (!canvas) return;
      const ctx = canvas.getContext("2d");
      if (!ctx) return;

      const dpr = sizeCanvasForDPR(canvas);
      const W = canvas.width, H = canvas.height;

      ctx.clearRect(0,0,W,H);

      if (tempHist.length < 2){
        ctx.fillStyle = "rgba(0,0,0,0.35)";
        ctx.font = `${12*dpr}px system-ui, sans-serif`;
        ctx.fillText("No data yet…", 8*dpr, 18*dpr);
        return;
      }

      const tNow = tempHist[tempHist.length-1].t;
      const tMin = tNow - tempWindowMs;
      const tMax = tNow;

      const yMin = MIN_F, yMax = MAX_F;

      const padL = 36*dpr, padR = 10*dpr, padT = 6*dpr, padB = 16*dpr;
      const plotW = W - padL - padR;
      const plotH = H - padT - padB;

      // Axes
      ctx.strokeStyle = "rgba(0,0,0,0.25)";
      ctx.lineWidth = 1*dpr;
      ctx.beginPath();
      ctx.moveTo(padL, padT); ctx.lineTo(padL, H - padB); ctx.lineTo(W - padR, H - padB);
      ctx.stroke();

      const xOf = (t)=> padL + ((t - tMin) / (tMax - tMin)) * plotW;
      const yOf = (f)=> padT + (1 - (f - yMin)/(yMax - yMin)) * plotH;

      // Grid + labels
      ctx.fillStyle = "rgba(0,0,0,0.45)";
      ctx.font = `${10*dpr}px system-ui, sans-serif`;
      const yTicks = [yMin, (yMin+yMax)/2, yMax];
      ctx.strokeStyle = "rgba(0,0,0,0.10)";
      ctx.lineWidth = 1*dpr;
      yTicks.forEach(val=>{
        const y = yOf(val);
        ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(W - padR, y); ctx.stroke();
        ctx.fillText(val.toFixed(0)+"°", 4*dpr, y + 3*dpr);
      });

      // Time labels
      const mins = Math.round(tempWindowMs/60000);
      ctx.fillText(`-${mins} min`, padL, H - 3*dpr);
      const nowLabel = "now";
      const tw = ctx.measureText(nowLabel).width;
      ctx.fillText(nowLabel, W - padR - tw, H - 3*dpr);

      function drawSeries(selector, stroke){
        let started = false;
        ctx.beginPath();
        for (let i=0;i<tempHist.length;i++){
          const p = tempHist[i];
          if (p.t < tMin) continue;
          const f = selector(p);
          if (Number.isFinite(f)){
            const x = xOf(p.t), y = yOf(f);
            if (!started){ ctx.moveTo(x,y); started = true; }
            else { ctx.lineTo(x,y); }
          } else {
            started = false;
          }
        }
        ctx.strokeStyle = stroke;
        ctx.lineWidth = 2*dpr;
        ctx.stroke();
      }

      drawSeries(p=>p.ambF, "#0072B2"); // Ambient
      drawSeries(p=>p.envF, "#D55E00"); // Envelope

      // Legend
      const legY = padT + 10*dpr;
      ctx.lineWidth = 3*dpr;
      ctx.strokeStyle = "#0072B2"; ctx.beginPath(); ctx.moveTo(padL, legY); ctx.lineTo(padL+18*dpr, legY); ctx.stroke();
      ctx.strokeStyle = "#D55E00"; ctx.beginPath(); ctx.moveTo(padL+60*dpr, legY); ctx.lineTo(padL+78*dpr, legY); ctx.stroke();
      ctx.fillStyle = "rgba(0,0,0,0.55)";
      ctx.font = `${10*dpr}px system-ui, sans-serif`;
      ctx.fillText("Ambient", padL+22*dpr, legY+3*dpr);
      ctx.fillText("Envelope", padL+82*dpr, legY+3*dpr);
    }
    window.addEventListener('resize', ()=>{ drawTempChart(); setLinkStatus(Date.now()); });

    // Controls
    if (tempWindowSel){
      tempWindowSel.addEventListener('change', ()=>{
        const mins = parseInt(tempWindowSel.value,10) || 5;
        TEMP_WINDOW_MINUTES = mins;
        tempWindowMs = mins * 60 * 1000;
        const now = Date.now(), cutoff = now - tempWindowMs;
        while (tempHist.length && tempHist[0].t < cutoff) tempHist.shift();
        render();
        drawTempChart();
      });
    }
    if (envelopeInput){
      envelopeInput.addEventListener('change', ()=>{
        const val = parseFloat(envelopeInput.value);
        if (Number.isFinite(val) && val > 0){
          ENVELOPE_DIAMETER_FT = val;
          recomputeFromDiameter();
          render();
          drawTempChart();
        }
      });
    }

    function updateTelemetry(packet){
      const ts=(packet&&Number(packet.timestamp))||Date.now();
      const prevAlt=state.altitudeM, prevTs=state.timestamp;
      Object.assign(state, packet||{}, { timestamp: ts });

      if (initialAlt===null && Number.isFinite(state.altitudeM)) {
        initialAlt = state.altitudeM;
      }

      if (Number.isFinite(prevAlt)&&Number.isFinite(state.altitudeM)){
        const dt=(ts-prevTs)/1000;
        if (dt>0){
          const v=(state.altitudeM-prevAlt)/dt; // m/s
          verticalSpeed=0.7*verticalSpeed+0.3*v;
        }
      }

      // Capture temps for chart
      pushTempSample(Date.now(),
        Number.isFinite(state.ambientTempF) ? state.ambientTempF : NaN,
        Number.isFinite(state.envelopeTempF)? state.envelopeTempF: NaN
      );

      render();
      drawTempChart();
    }
    window.updateTelemetry=updateTelemetry;

    async function poll(){
      const t0 = performance.now();
      try{
        const url = '/data.json?t=' + Date.now();       // cache-buster for Safari
        const r   = await fetch(url, { cache: 'no-cache' }); // simpler cache hint
        if (r.ok){
          const data = await r.json();
          updateTelemetry(data);
          lastOkTs = Date.now();
          const rtt = Math.round(performance.now() - t0);
          recentRtts.push(rtt);
          if (recentRtts.length > 20) recentRtts.shift();
        }
      }catch(e){}
      setLinkStatus(Date.now());
    }
    // Use a slightly calmer cadence for captive stacks
    setInterval(poll, 1000);
    setInterval(()=> setLinkStatus(Date.now()), 500);
    setInterval(()=>{ if (flightStartMs!==null) render(); }, 250);

    function resetFlight(){
      if (Number.isFinite(state.altitudeM)) {
        initialAlt = state.altitudeM;
      } else {
        initialAlt = null;
      }
      verticalSpeed = 0;
      flightStartMs = Date.now();
      render();
      drawTempChart();
    }
    btnReset.addEventListener('click', resetFlight);

    // Initial
    render();
    drawTempChart();
    setLinkStatus(Date.now());


  </script>
</body>
</html>)HTML";




// ---- JSON builder/handlers ----
String buildTelemetryJson(){
  float envC = s_envObjC;

  // Decide ambient from DHT (preferred) else MPL
  bool haveDht = !isnan(s_dhtTempC);
  float ambC   = haveDht ? s_dhtTempC : s_mplTempC;
  const char* ambSrc = haveDht ? "DHT" : "MPL";

  String json = "{";
  json += "\"envelopeTempF\":"; json += (isnan(envC) ? "null" : String(c2f(envC),1)); json += ",";
  json += "\"ambientTempF\":";  json += (isnan(ambC) ? "null" : String(c2f(ambC),1)); json += ",";
  json += "\"humidityPct\":";   json += (isnan(s_dhtHum) ? "null" : String(s_dhtHum,0)); json += ",";
  json += "\"pressurehPa\":";   json += (isnan(s_pres_hPa) ? "null" : String(s_pres_hPa,1)); json += ",";
  json += "\"altitudeM\":";     json += (isnan(s_alt_m) ? "null" : String(s_alt_m,1)); json += ",";
  json += "\"ambientSource\":\""; json += ambSrc; json += "\",";
  json += "\"timestamp\":";     json += String((uint32_t)millis());
  json += "}";
  return json;
}


void handleRoot(){ server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }
void handleDataJson(){ server.send(200, "application/json; charset=utf-8", buildTelemetryJson()); }
// ---- Captive-portal helpers (for iOS/Android/Windows CNA) ----
void handleCNA(){
  server.send(200, "text/html",
    "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<p>Connected to ESP32.</p>"
    "<p><a href=\"http://192.168.4.1/\">Open telemetry site</a></p>");
}
void handle204(){ server.send(204); }          // Android/Chrome probe
void handleRootNotFound(){ handleRoot(); }     // send main page for unknown paths


// ================= Setup / Loop =================
void setup(){
  Serial.begin(115200);
  delay(50);

  pinMode(READY_LED_PIN, OUTPUT);
  digitalWrite(READY_LED_PIN, LOW);
  setLedMode(LED_DOUBLE);   // AP heartbeat during boot

  // I2C bring-up
  i2cBusRecover(I2C_SDA, I2C_SCL);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(50000);     // 50 kHz for robust mixed-bus wiring
  Wire.setTimeout(150);     // generous timeout for slow edges
  i2cScan();

  // OLED init (optional)
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    g_oledOK = true;
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0x20); // dim
    oledBoot();
  } else {
    Serial.println("[OLED] init failed");
    g_oledOK = false;
  }

  // Sensors
  mplOk = mpl.begin();
  Serial.println(mplOk ? "[OK ] MPL3115A2 ready" : "[WARN] MPL3115A2 not found");

  dht.begin();
  delay(1500);   // DHT warm-up

  // Probe MLX only if it ACKs (prevents IDF error spam)
  g_mlxOK = false;
  for (uint8_t a : { (uint8_t)0x5A, (uint8_t)0x5B }) {
    if (i2cAck(a)) {
      uint16_t dummy;
      if (mlxReadWord(a, 0x06, dummy)) { g_mlxAddr = a; g_mlxOK = true; break; }
    }
  }
  if (g_mlxOK) Serial.printf("[OK ] MLX90614 @0x%02X\n", g_mlxAddr);
  else         Serial.println("[INFO] MLX90614 not detected (skipping MLX reads)");

  // ---- Self-test pass 1: quick checklist while we finalize Wi-Fi ----
  SelfTest st1 = runSensorSelfTest();
  oledSensorSummary(st1.mpl, st1.mlx, st1.dht);
  delay(700);

  // --- AP-ONLY WIFI ---
  WiFi.mode(WIFI_AP);
  if (AP_PASSWORD[0] == '\0') WiFi.softAP(AP_SSID);
  else                        WiFi.softAP(AP_SSID, AP_PASSWORD);
  g_apIP = WiFi.softAPIP();
  Serial.print("[WIFI] AP SSID: "); Serial.println(AP_SSID);
  Serial.print("[WIFI] AP IP:   "); Serial.println(g_apIP);

  // Web
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data.json", HTTP_GET, handleDataJson);

  // Captive portal probes / helpers
  server.on("/hotspot-detect.html", HTTP_GET, handleCNA); // iOS
  server.on("/generate_204",       HTTP_GET, handle204);  // Android/Chrome
  server.on("/ncsi.txt",           HTTP_GET, handleCNA);  // Windows
  server.on("/fwlink",             HTTP_GET, handleCNA);  // Legacy
  server.onNotFound(handleRootNotFound);

  server.begin();
  Serial.println("[WEB ] Server started");


  // ---- READY flashes with LED synced ----
  flashReady(3, 320, 220);

  // ---- Brief "website live" splash (optional) ----
  oledWebsiteLive(g_apIP);
  delay(1400);

  // ---- Self-test pass 2: then land on the requested STATIC STATUS PAGE ----
  SelfTest st2 = runSensorSelfTest();
  (void)st2; // status page will compute live OK boxes from data anyway
  oledStatusPage();  // <- final page we stay on

  // LED solid
  setLedMode(LED_SOLID);

  g_lastSampleMs = millis();
}

void loop(){
  server.handleClient();
  serviceReadyLed();

  uint32_t now = millis();
  if (now - g_lastSampleMs >= TELEMETRY_PERIOD_MS){
    g_lastSampleMs = now;

    // MPL
    if (mplOk){
      float alt  = mpl.getAltitude();     // m
      float tC   = mpl.getTemperature();  // °C
      float pres = mpl.getPressure();     // Pa or kPa
      if (!isnan(alt))  s_alt_m    = alt;
      if (!isnan(tC))   s_mplTempC = tC;
      if (!isnan(pres)) s_pres_hPa = scalePressureToHectoPascal(pres);
    }

    // MLX (optional)
    if (g_mlxOK){
      float tCobj;
      if (mlxReadTempC(g_mlxAddr, tCobj, /*ambient=*/false)) {
        s_envObjC = tCobj;

        // ---- Overheat LED logic (with hysteresis) ----
        if (isfinite(s_envObjC)) {
          float envF = c2f(s_envObjC);
          if (!g_overheat && envF >= OVERHEAT_ON_F) {
            g_overheat = true;
            setLedMode(LED_BLINK_FAST);
          } else if (g_overheat && envF <= OVERHEAT_OFF_F) {
            g_overheat = false;
            setLedMode(LED_SOLID);
          }
        }
      }
      delay(2); // tiny spacing between I2C ops
    }


    // DHT11 (paced + retries, keeps last good values)
    if (now - g_lastDhtMs >= DHT_PERIOD_MS) {
      g_lastDhtMs = now;
      float t, h; bool ok = false;
      for (int i = 0; i < 3 && !ok; ++i) {
        ok = readDhtOnce(t, h);
        if (!ok) delay(60);
      }
      if (ok) {
        s_dhtTempC = t;
        s_dhtHum   = h;
      } // else keep prior values
    }
  }

  // --- Keep showing the SINGLE status page and refresh it about once/second ---
  if (g_oledOK && millis() - g_lastOledMs >= OLED_PERIOD_MS){
    g_lastOledMs = millis();
    oledStatusPage();
  }
}
