/*
  Smart Plant Monitoring System
  Board: ESP32 DevKit
  Sensors: capacitive soil moisture sensor + DHT22
  Output: low-voltage DC pump through a suitable driver/relay

  Install libraries:
    - DHT sensor library by Adafruit
    - Adafruit Unified Sensor

  Safety:
    Never connect a pump directly to an ESP32 GPIO.
    Use a properly rated low-voltage driver and separate pump supply.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>

// ---------------- Wi-Fi settings ----------------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---------------- Pin configuration -------------
constexpr int SOIL_PIN = 34;   // ADC1 input-only pin
constexpr int DHT_PIN = 4;
constexpr int PUMP_PIN = 26;

// Most relay modules are active LOW. Change to false for active-HIGH drivers.
constexpr bool PUMP_ACTIVE_LOW = true;
#define DHT_TYPE DHT22

DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);

// ---------------- Calibration and control --------
// Replace these with readings from your own sensor.
const int SOIL_DRY_RAW = 3000;
const int SOIL_WET_RAW = 1400;

const int MOISTURE_START_PERCENT = 30;
const unsigned long SENSOR_INTERVAL_MS = 2500;
const unsigned long PUMP_MAX_RUNTIME_MS = 5000;       // Start short; test safely.
const unsigned long WATERING_COOLDOWN_MS = 30UL * 60UL * 1000UL;

int soilRaw = 0;
int soilMoisture = 0;
float temperatureC = NAN;
float humidity = NAN;

bool pumpOn = false;
bool manualCycle = false;
unsigned long pumpStartedAt = 0;
unsigned long lastAutomaticWateringAt = 0;
unsigned long lastSensorReadAt = 0;
bool hasAutomaticWatered = false;

// ---------------- Dashboard -----------------------
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Plant Monitor</title>
  <style>
    :root { font-family: Arial, sans-serif; color: #183126; background: #f2f7f1; }
    * { box-sizing: border-box; }
    body { margin: 0; padding: 24px; }
    main { max-width: 850px; margin: auto; }
    h1 { margin-bottom: 6px; }
    .sub { color: #557064; margin-top: 0; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit,minmax(190px,1fr)); gap: 14px; }
    .card { background: white; padding: 20px; border-radius: 16px; box-shadow: 0 3px 14px #163b1b12; }
    .label { color: #60786c; font-size: 14px; }
    .value { font-size: 32px; font-weight: 700; margin-top: 10px; }
    .unit { font-size: 16px; color: #60786c; }
    .controls { margin-top: 16px; }
    button { border: 0; padding: 12px 18px; border-radius: 10px; font-weight: 700; cursor: pointer; margin: 6px 8px 0 0; }
    .on { background: #247b48; color: white; }
    .off { background: #e5ece7; color: #183126; }
    #status { margin-top: 14px; color: #557064; }
    .bar { height: 10px; background: #e5ece7; border-radius: 10px; overflow: hidden; margin-top: 12px; }
    #moistureBar { height: 100%; width: 0%; background: #319b62; transition: width .3s; }
    footer { margin-top: 22px; font-size: 13px; color: #718278; }
  </style>
</head>
<body>
<main>
  <h1>🌱 Smart Plant Monitor</h1>
  <p class="sub">Live readings from your ESP32</p>
  <section class="grid">
    <article class="card">
      <div class="label">Soil moisture</div>
      <div class="value"><span id="moisture">--</span><span class="unit"> %</span></div>
      <div class="bar"><div id="moistureBar"></div></div>
    </article>
    <article class="card">
      <div class="label">Temperature</div>
      <div class="value"><span id="temperature">--</span><span class="unit"> °C</span></div>
    </article>
    <article class="card">
      <div class="label">Air humidity</div>
      <div class="value"><span id="humidity">--</span><span class="unit"> %</span></div>
    </article>
    <article class="card">
      <div class="label">Pump status</div>
      <div class="value" id="pump">--</div>
      <div class="label" id="mode">Mode: --</div>
    </article>
  </section>
  <section class="card controls">
    <strong>Manual watering</strong><br>
    <button class="on" onclick="setPump('on')">Run pump (time-limited)</button>
    <button class="off" onclick="setPump('off')">Stop pump</button>
    <div id="status">Connecting to ESP32...</div>
  </section>
  <footer>Dashboard refreshes every 3 seconds. Keep the ESP32 and this device on the same Wi-Fi network.</footer>
</main>
<script>
async function refresh() {
  try {
    const response = await fetch('/api/status', {cache: 'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const d = await response.json();
    document.getElementById('moisture').textContent = d.soilMoisture;
    document.getElementById('moistureBar').style.width = d.soilMoisture + '%';
    document.getElementById('temperature').textContent =
      d.temperatureC === null ? '--' : Number(d.temperatureC).toFixed(1);
    document.getElementById('humidity').textContent =
      d.humidity === null ? '--' : Number(d.humidity).toFixed(1);
    document.getElementById('pump').textContent = d.pumpOn ? 'ON' : 'OFF';
    document.getElementById('mode').textContent = 'Mode: ' + d.mode;
    document.getElementById('status').textContent = 'Connected · Soil raw: ' + d.soilRaw;
  } catch (e) {
    document.getElementById('status').textContent = 'Could not reach ESP32. Check Wi-Fi.';
  }
}
async function setPump(state) {
  try {
    const r = await fetch('/api/pump?state=' + state);
    const d = await r.json();
    document.getElementById('status').textContent = d.message || 'Command sent';
    refresh();
  } catch (e) {
    document.getElementById('status').textContent = 'Pump command failed.';
  }
}
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)rawliteral";

// ---------------- Helper functions ----------------
void setPumpOutput(bool on) {
  pumpOn = on;
  const int outputLevel = PUMP_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(PUMP_PIN, outputLevel);
}

int readSoilMoisturePercent(int rawValue) {
  // A typical capacitive sensor reads higher when dry and lower when wet.
  const float percent = 100.0f * (SOIL_DRY_RAW - rawValue) /
                        (float)(SOIL_DRY_RAW - SOIL_WET_RAW);
  return constrain((int)roundf(percent), 0, 100);
}

String nullableFloat(float value, int decimals = 1) {
  if (isnan(value) || isinf(value)) return "null";
  return String(value, decimals);
}

void readSensors() {
  soilRaw = analogRead(SOIL_PIN);
  soilMoisture = readSoilMoisturePercent(soilRaw);

  const float newHumidity = dht.readHumidity();
  const float newTemperature = dht.readTemperature();

  // Keep last valid reading if a DHT sample fails.
  if (!isnan(newHumidity)) humidity = newHumidity;
  if (!isnan(newTemperature)) temperatureC = newTemperature;

  Serial.printf("Soil raw: %d | Moisture: %d%% | Temp: %.1f C | Humidity: %.1f%%\n",
                soilRaw, soilMoisture, temperatureC, humidity);
}

void startPump(bool isManual) {
  if (pumpOn) return;
  manualCycle = isManual;
  pumpStartedAt = millis();
  setPumpOutput(true);
  Serial.println(isManual ? "Manual watering started" : "Automatic watering started");
}

void stopPump() {
  if (!pumpOn) return;
  setPumpOutput(false);
  if (!manualCycle) {
    lastAutomaticWateringAt = millis();
    hasAutomaticWatered = true;
  }
  manualCycle = false;
  Serial.println("Pump stopped");
}

void updateWateringControl() {
  const unsigned long now = millis();

  if (pumpOn && now - pumpStartedAt >= PUMP_MAX_RUNTIME_MS) {
    stopPump();
    return;
  }

  if (!pumpOn && soilMoisture < MOISTURE_START_PERCENT) {
    const bool cooldownComplete = !hasAutomaticWatered ||
        (now - lastAutomaticWateringAt >= WATERING_COOLDOWN_MS);

    if (cooldownComplete) {
      startPump(false);
    }
  }
}

// ---------------- HTTP handlers ------------------
void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleStatus() {
  String json = "{";
  json += "\"soilRaw\":" + String(soilRaw) + ",";
  json += "\"soilMoisture\":" + String(soilMoisture) + ",";
  json += "\"temperatureC\":" + nullableFloat(temperatureC) + ",";
  json += "\"humidity\":" + nullableFloat(humidity) + ",";
  json += "\"pumpOn\":" + String(pumpOn ? "true" : "false") + ",";
  json += "\"mode\":\"" + String(manualCycle ? "MANUAL" : "AUTO") + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handlePump() {
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"message\":\"Missing state=on or state=off\"}");
    return;
  }

  const String state = server.arg("state");
  if (state == "on") {
    if (!pumpOn) startPump(true);
    server.send(200, "application/json",
                "{\"message\":\"Manual watering requested; runtime is limited.\"}");
  } else if (state == "off") {
    stopPump();
    server.send(200, "application/json", "{\"message\":\"Pump stopped.\"}");
  } else {
    server.send(400, "application/json",
                "{\"message\":\"Invalid state. Use on or off.\"}");
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. Dashboard: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection timed out. Restart after checking credentials.");
  }
}

void setup() {
  Serial.begin(115200);

  // Set the output OFF before starting network or sensor work.
  pinMode(PUMP_PIN, OUTPUT);
  setPumpOutput(false);

  analogReadResolution(12);
  dht.begin();

  readSensors();
  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/pump", HTTP_GET, handlePump);
  server.begin();

  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  const unsigned long now = millis();
  if (now - lastSensorReadAt >= SENSOR_INTERVAL_MS) {
    lastSensorReadAt = now;
    readSensors();
  }

  updateWateringControl();
}
