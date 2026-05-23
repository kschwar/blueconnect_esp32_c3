#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "DEIN_WLAN"
#endif
#ifndef WIFI_PASS_VALUE
#define WIFI_PASS_VALUE "DEIN_PASSWORT"
#endif
#ifndef MQTT_HOST_VALUE
#define MQTT_HOST_VALUE "192.168.1.10"
#endif
#ifndef MQTT_PORT_VALUE
#define MQTT_PORT_VALUE 1883
#endif
#ifndef MQTT_USER_VALUE
#define MQTT_USER_VALUE ""
#endif
#ifndef MQTT_PASS_VALUE
#define MQTT_PASS_VALUE ""
#endif
#ifndef BLUECONNECT_MAC_VALUE
#define BLUECONNECT_MAC_VALUE ""
#endif

// ================================================================
// Benutzer-Konfiguration
// ================================================================
static const char* WIFI_SSID = WIFI_SSID_VALUE;
static const char* WIFI_PASS = WIFI_PASS_VALUE;

static const char* MQTT_HOST = MQTT_HOST_VALUE;
static const int   MQTT_PORT = MQTT_PORT_VALUE;
static const char* MQTT_USER = MQTT_USER_VALUE;
static const char* MQTT_PASS = MQTT_PASS_VALUE;

// Optional: MAC-Adresse deines BlueConnect Go eintragen, z.B. "aa:bb:cc:dd:ee:ff".
// Leer lassen, wenn per Service UUID gesucht werden soll.
static const char* BLUECONNECT_MAC = BLUECONNECT_MAC_VALUE;

static const char* DEVICE_ID   = "blueconnect_go_esp32c3";
static const char* DEVICE_NAME = "BlueConnect Go ESP32-C3";
static const char* HOSTNAME    = "blueconnect-c3";

static const uint32_t MEASURE_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 Minuten
static const uint32_t MEASURE_RETRY_INTERVAL_MS = 60UL * 1000UL;  // 1 Minute nach Fehler
static const uint32_t BLE_SCAN_SECONDS    = 12;
static const uint32_t BLE_NOTIFY_TIMEOUT_MS = 35000;
static const bool ENABLE_DIAGNOSTICS = true;

// ================================================================
// BlueConnect BLE UUIDs
// ================================================================
static NimBLEUUID BLUE_SERVICE_UUID("F3300001-F0A2-9B06-0C59-1BC4763B5C00");
static NimBLEUUID BLUE_WRITE_UUID  ("F3300002-F0A2-9B06-0C59-1BC4763B5C00");
static NimBLEUUID BLUE_NOTIFY_UUID ("F3300003-F0A2-9B06-0C59-1BC4763B5C00");

// ================================================================
// MQTT Topics
// ================================================================
static const char* TOPIC_STATE = "blueconnect/go/state";
static const char* TOPIC_AVAIL = "blueconnect/go/availability";
static const char* TOPIC_DIAG  = "blueconnect/go/diagnostics";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer server(80);

struct Measurement {
  bool valid = false;
  float temperatureC = NAN;
  float ph = NAN;
  float orpMv = NAN;
  float chlorinePpm = NAN;
  float ecUsCm = NAN;
  float saltPpm = NAN;
  float batteryVoltage = NAN;
  float batteryPercent = NAN;
  uint16_t batteryRaw = 0;
  uint16_t conductivityRaw = 0;
  int statusRaw = 0;
  int rssi = 0;
  String mac;
  String rawHex;
  uint32_t lastSuccessMs = 0;
  String lastError = "not started";
};

Measurement last;
volatile bool notificationReceived = false;
std::string notificationPayload;
NimBLEAdvertisedDevice* foundDevice = nullptr;
String lastAdvertisement = "";
uint32_t lastScanMs = 0;
uint32_t lastAttemptMs = 0;
bool lastReadOk = false;

String bytesToHex(const uint8_t* data, size_t len) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

bool macMatches(const std::string& addr) {
  if (strlen(BLUECONNECT_MAC) == 0) return true;
  String a = addr.c_str();
  String b = BLUECONNECT_MAC;
  a.toLowerCase();
  b.toLowerCase();
  return a == b;
}

uint16_t readLe16(const uint8_t* data, size_t offset) {
  return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

float clampFloat(float value, float minValue, float maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

// Decoder nach adamantivm/BlueConnect: Button-Char mit 0x01 triggern,
// Notify-Char lesen und Messwerte ab Byte-Offset 1 little-endian dekodieren.
bool parseBluePayload(const uint8_t* data, size_t len, Measurement& m) {
  m.rawHex = bytesToHex(data, len);
  if (len < 11) {
    m.lastError = "payload too short: " + String(len);
    return false;
  }

  const uint16_t rawTemp = readLe16(data, 1);
  const uint16_t rawPh = readLe16(data, 3);
  const uint16_t rawOrp = readLe16(data, 5);
  const uint16_t rawCond = readLe16(data, 7);
  const uint16_t rawBatt = readLe16(data, 9);

  const float tempC = rawTemp / 100.0f + 0.1f;
  const float ph = (2048.0f - rawPh) / 235.0f + 6.92f;
  const float orpMv = rawOrp / 4.0f - 5.0f;
  const float baseChlorine = max(0.0f, (orpMv - 650.0f) / 150.0f);
  const float phFactor = powf(10.0f, 7.5f - ph);
  const float chlorinePpm = clampFloat(baseChlorine * phFactor * 1.2f, 0.0f, 5.0f);
  const float batteryVoltage = rawBatt / 1000.0f;
  const float batteryPercent = clampFloat((rawBatt - 2800.0f) / (3640.0f - 2800.0f) * 100.0f, 0.0f, 100.0f);

  m.temperatureC = tempC;
  m.ph = ph;
  m.orpMv = orpMv;
  m.chlorinePpm = chlorinePpm;
  m.conductivityRaw = rawCond;
  if (rawCond != 0) {
    const float ec25 = rawCond / (1.0f + 0.02f * (tempC - 25.0f));
    m.saltPpm = ec25 * 10.1f;
    m.ecUsCm = m.saltPpm / 0.65f;
  } else {
    m.ecUsCm = NAN;
    m.saltPpm = NAN;
  }
  m.batteryRaw = rawBatt;
  m.batteryVoltage = batteryVoltage;
  m.batteryPercent = batteryPercent;
  m.statusRaw = data[0];
  m.valid = isfinite(tempC) && isfinite(ph) && isfinite(orpMv) && tempC > -20 && tempC < 80 && ph >= 0 && ph <= 14 && rawBatt >= 2500 && rawBatt <= 4500;
  if (!m.valid) m.lastError = "parsed values implausible";
  return m.valid;
}

void notifyCallback(NimBLERemoteCharacteristic* c, uint8_t* data, size_t length, bool isNotify) {
  notificationPayload.assign((char*)data, length);
  notificationReceived = true;
  Serial.printf("[BLE] Notify %u bytes: %s\n", (unsigned)length, bytesToHex(data, length).c_str());
}

class AdvertisedCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
    const bool hasService = advertisedDevice->isAdvertisingService(BLUE_SERVICE_UUID);
    const bool hasTargetMac = macMatches(advertisedDevice->getAddress().toString());

    if (ENABLE_DIAGNOSTICS) {
      String line = String(advertisedDevice->getAddress().toString().c_str()) +
                    " RSSI=" + String(advertisedDevice->getRSSI()) +
                    " name=" + String(advertisedDevice->getName().c_str()) +
                    " service=" + String(hasService ? "yes" : "no");
      lastAdvertisement = line;
      Serial.println("[SCAN] " + line);
    }

    if ((strlen(BLUECONNECT_MAC) > 0 && hasTargetMac) || (strlen(BLUECONNECT_MAC) == 0 && hasService)) {
      foundDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
      NimBLEDevice::getScan()->stop();
    }
  }
};

void ensureMqtt();
void publishDiscovery();
void publishState();
void publishDiagnostics(const char* reason);

bool scanAndReadBlueConnect() {
  lastAttemptMs = millis();
  last.lastError = "scanning";
  notificationReceived = false;
  notificationPayload.clear();
  if (foundDevice) { delete foundDevice; foundDevice = nullptr; }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvertisedCallbacks(), true);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(40);

  Serial.println("[BLE] Scan start");
  lastScanMs = millis();
  scan->start(BLE_SCAN_SECONDS, false);
  scan->clearResults();

  if (!foundDevice) {
    last.lastError = "blueconnect not found";
    publishDiagnostics("not_found");
    return false;
  }

  last.mac = foundDevice->getAddress().toString().c_str();
  last.rssi = foundDevice->getRSSI();
  Serial.printf("[BLE] Found %s RSSI=%d\n", last.mac.c_str(), last.rssi);

  NimBLEClient* client = NimBLEDevice::createClient();
  client->setConnectTimeout(8);

  if (!client->connect(foundDevice)) {
    last.lastError = "connect failed";
    NimBLEDevice::deleteClient(client);
    publishDiagnostics("connect_failed");
    return false;
  }

  NimBLERemoteService* service = client->getService(BLUE_SERVICE_UUID);
  if (!service) {
    last.lastError = "service not found";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    publishDiagnostics("service_not_found");
    return false;
  }

  NimBLERemoteCharacteristic* notifyChr = service->getCharacteristic(BLUE_NOTIFY_UUID);
  NimBLERemoteCharacteristic* writeChr  = service->getCharacteristic(BLUE_WRITE_UUID);

  if (!notifyChr || !writeChr) {
    last.lastError = "characteristic not found";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    publishDiagnostics("characteristic_not_found");
    return false;
  }

  if (notifyChr->canNotify()) {
    if (!notifyChr->subscribe(true, notifyCallback)) {
      last.lastError = "notify subscribe failed";
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      publishDiagnostics("notify_subscribe_failed");
      return false;
    }
  } else {
    last.lastError = "notify characteristic cannot notify";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    publishDiagnostics("notify_not_supported");
    return false;
  }

  uint8_t trigger = 0x01;
  Serial.println("[BLE] Write trigger 0x01");
  if (!writeChr->writeValue(&trigger, 1, true)) {
    last.lastError = "write trigger failed";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    publishDiagnostics("write_failed");
    return false;
  }

  uint32_t start = millis();
  while (!notificationReceived && millis() - start < BLE_NOTIFY_TIMEOUT_MS) {
    delay(20);
  }

  bool ok = false;
  if (notificationReceived && notificationPayload.size() > 0) {
    Measurement m = last;
    ok = parseBluePayload((const uint8_t*)notificationPayload.data(), notificationPayload.size(), m);
    m.mac = last.mac;
    m.rssi = last.rssi;
    m.lastSuccessMs = millis();
    if (ok) m.lastError = "ok";
    last = m;
  } else {
    last.lastError = "notify timeout";
  }

  client->disconnect();
  NimBLEDevice::deleteClient(client);

  if (ok) {
    publishState();
    publishDiagnostics("success");
  } else {
    publishDiagnostics("parse_or_timeout_failed");
  }
  return ok;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
  }
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  while (!mqtt.connected()) {
    Serial.printf("[MQTT] Connecting to %s:%d\n", MQTT_HOST, MQTT_PORT);
    bool ok;
    if (strlen(MQTT_USER) > 0) ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, TOPIC_AVAIL, 1, true, "offline");
    else ok = mqtt.connect(DEVICE_ID, TOPIC_AVAIL, 1, true, "offline");

    if (ok) {
      Serial.println("[MQTT] Connected");
      mqtt.publish(TOPIC_AVAIL, "online", true);
      publishDiscovery();
    } else {
      Serial.printf("[MQTT] failed rc=%d\n", mqtt.state());
      delay(3000);
    }
  }
}

void publishJson(const char* topic, JsonDocument& doc, bool retained=false) {
  char buf[1024];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topic, (const uint8_t*)buf, n, retained);
}

void publishState() {
  ensureMqtt();
  JsonDocument doc;
  doc["temperature"] = serialized(String(last.temperatureC, 2));
  doc["ph"] = serialized(String(last.ph, 2));
  doc["orp"] = serialized(String(last.orpMv, 0));
  doc["chlorine"] = serialized(String(last.chlorinePpm, 2));
  if (isfinite(last.ecUsCm)) doc["ec"] = serialized(String(last.ecUsCm, 0));
  else doc["ec"] = nullptr;
  if (isfinite(last.saltPpm)) doc["salt"] = serialized(String(last.saltPpm, 0));
  else doc["salt"] = nullptr;
  doc["battery"] = serialized(String(last.batteryPercent, 0));
  doc["battery_voltage"] = serialized(String(last.batteryVoltage, 2));
  doc["battery_raw"] = last.batteryRaw;
  doc["conductivity_raw"] = last.conductivityRaw;
  doc["status_raw"] = last.statusRaw;
  doc["rssi"] = last.rssi;
  doc["mac"] = last.mac;
  doc["raw_hex"] = last.rawHex;
  doc["last_error"] = last.lastError;
  doc["uptime_s"] = millis() / 1000;
  publishJson(TOPIC_STATE, doc, true);
}

void publishDiagnostics(const char* reason) {
  ensureMqtt();
  JsonDocument doc;
  doc["reason"] = reason;
  doc["last_error"] = last.lastError;
  doc["last_advertisement"] = lastAdvertisement;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["blue_rssi"] = last.rssi;
  doc["mac"] = last.mac;
  publishJson(TOPIC_DIAG, doc, true);
}

void discoverySensor(const char* objectId, const char* name, const char* deviceClass, const char* unit, const char* valueTemplate, const char* stateClass = "measurement") {
  JsonDocument doc;
  String uniqueId = String(DEVICE_ID) + "_" + objectId;
  doc["name"] = name;
  doc["unique_id"] = uniqueId;
  doc["state_topic"] = TOPIC_STATE;
  doc["availability_topic"] = TOPIC_AVAIL;
  doc["value_template"] = valueTemplate;
  if (strlen(deviceClass) > 0) doc["device_class"] = deviceClass;
  if (strlen(unit) > 0) doc["unit_of_measurement"] = unit;
  if (strlen(stateClass) > 0) doc["state_class"] = stateClass;
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0] = DEVICE_ID;
  dev["name"] = DEVICE_NAME;
  dev["manufacturer"] = "DIY ESP32-C3";
  dev["model"] = "BlueConnect BLE MQTT Bridge";

  String topic = "homeassistant/sensor/" + String(DEVICE_ID) + "/" + objectId + "/config";
  publishJson(topic.c_str(), doc, true);
}

void publishDiscovery() {
  discoverySensor("temperature", "Pool Temperatur", "temperature", "°C", "{{ value_json.temperature }}");
  discoverySensor("ph", "Pool pH", "ph", "pH", "{{ value_json.ph }}");
  discoverySensor("orp", "Pool ORP", "voltage", "mV", "{{ value_json.orp }}");
  discoverySensor("chlorine", "Pool Freies Chlor", "", "ppm", "{{ value_json.chlorine }}");
  discoverySensor("ec", "Pool Leitfähigkeit", "", "µS/cm", "{{ value_json.ec }}");
  discoverySensor("salt", "Pool Salz", "", "ppm", "{{ value_json.salt }}");
  discoverySensor("battery", "BlueConnect Batterie", "battery", "%", "{{ value_json.battery }}");
  discoverySensor("battery_voltage", "BlueConnect Batteriespannung", "voltage", "V", "{{ value_json.battery_voltage }}");
  discoverySensor("rssi", "BlueConnect RSSI", "signal_strength", "dBm", "{{ value_json.rssi }}");
  discoverySensor("battery_raw", "BlueConnect Battery Raw", "", "mV", "{{ value_json.battery_raw }}", "");
  discoverySensor("conductivity_raw", "BlueConnect Conductivity Raw", "", "", "{{ value_json.conductivity_raw }}", "");
}

String htmlPage() {
  String s;
  s += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>BlueConnect ESP32-C3</title><style>body{font-family:system-ui;margin:24px;max-width:900px} .card{border:1px solid #ddd;border-radius:12px;padding:16px;margin:12px 0} code{background:#f5f5f5;padding:2px 4px;border-radius:4px} button{padding:10px 14px;border-radius:8px;border:1px solid #999;background:white}</style></head><body>";
  s += "<h1>BlueConnect ESP32-C3</h1>";
  s += "<div class='card'><h2>Werte</h2>";
  s += "Temperatur: <b>" + String(last.temperatureC, 2) + " °C</b><br>";
  s += "pH: <b>" + String(last.ph, 2) + "</b><br>";
  s += "ORP: <b>" + String(last.orpMv, 0) + " mV</b><br>";
  s += "Freies Chlor: <b>" + String(last.chlorinePpm, 2) + " ppm</b><br>";
  s += "Leitfähigkeit: <b>" + String(last.ecUsCm, 0) + " µS/cm</b><br>";
  s += "Salz: <b>" + String(last.saltPpm, 0) + " ppm</b><br>";
  s += "Batterie: <b>" + String(last.batteryPercent, 0) + " %</b><br>";
  s += "Batteriespannung: <b>" + String(last.batteryVoltage, 2) + " V</b><br>";
  s += "Battery raw: <b>" + String(last.batteryRaw) + " mV</b><br>";
  s += "Conductivity raw: <b>" + String(last.conductivityRaw) + "</b><br>";
  s += "Status raw: <b>" + String(last.statusRaw) + "</b><br>";
  s += "RSSI: <b>" + String(last.rssi) + " dBm</b><br>";
  s += "Raw: <code>" + last.rawHex + "</code></div>";
  s += "<div class='card'><h2>Status</h2>";
  s += "MAC: <code>" + last.mac + "</code><br>";
  s += "Fehler: <code>" + last.lastError + "</code><br>";
  s += "Letzte Advertisement: <code>" + lastAdvertisement + "</code><br>";
  s += "Heap: " + String(ESP.getFreeHeap()) + " Bytes<br>";
  s += "WLAN RSSI: " + String(WiFi.RSSI()) + " dBm<br>";
  s += "Uptime: " + String(millis()/1000) + " s</div>";
  s += "<div class='card'><form action='/measure' method='post'><button>Jetzt messen</button></form> ";
  s += "<p>JSON: <a href='/api/state'>/api/state</a> · Diagnose: <a href='/api/diagnostics'>/api/diagnostics</a></p></div>";
  s += "</body></html>";
  return s;
}

void setupWeb() {
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", htmlPage()); });
  server.on("/measure", HTTP_POST, [](){
    bool ok = scanAndReadBlueConnect();
    lastReadOk = ok;
    server.sendHeader("Location", "/");
    server.send(ok ? 303 : 503, "text/plain", ok ? "ok" : last.lastError);
  });
  server.on("/api/state", HTTP_GET, [](){
    JsonDocument doc;
    doc["temperature"] = last.temperatureC;
    doc["ph"] = last.ph;
    doc["orp"] = last.orpMv;
    doc["chlorine"] = last.chlorinePpm;
    if (isfinite(last.ecUsCm)) doc["ec"] = last.ecUsCm;
    else doc["ec"] = nullptr;
    if (isfinite(last.saltPpm)) doc["salt"] = last.saltPpm;
    else doc["salt"] = nullptr;
    doc["battery"] = last.batteryPercent;
    doc["battery_voltage"] = last.batteryVoltage;
    doc["battery_raw"] = last.batteryRaw;
    doc["conductivity_raw"] = last.conductivityRaw;
    doc["status_raw"] = last.statusRaw;
    doc["rssi"] = last.rssi;
    doc["mac"] = last.mac;
    doc["raw_hex"] = last.rawHex;
    doc["last_error"] = last.lastError;
    String out; serializeJsonPretty(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/diagnostics", HTTP_GET, [](){
    JsonDocument doc;
    doc["last_advertisement"] = lastAdvertisement;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["uptime_s"] = millis()/1000;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["last_error"] = last.lastError;
    String out; serializeJsonPretty(doc, out);
    server.send(200, "application/json", out);
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBlueConnect Go ESP32-C3 MQTT Bridge");

  connectWifi();
  mqtt.setBufferSize(1024);
  ensureMqtt();
  setupWeb();

  NimBLEDevice::init("BlueConnect-C3");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  publishDiagnostics("boot");
  lastReadOk = scanAndReadBlueConnect();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }
  ensureMqtt();
  mqtt.loop();
  server.handleClient();

  const uint32_t intervalMs = lastReadOk ? MEASURE_INTERVAL_MS : MEASURE_RETRY_INTERVAL_MS;
  if (lastAttemptMs == 0 || millis() - lastAttemptMs >= intervalMs) {
    lastReadOk = scanAndReadBlueConnect();
  }

  delay(5);
}
