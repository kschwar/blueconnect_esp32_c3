#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "YOUR_WIFI"
#endif
#ifndef WIFI_PASS_VALUE
#define WIFI_PASS_VALUE "YOUR_WIFI_PASSWORD"
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
#ifndef PH_CENTER_VALUE
#define PH_CENTER_VALUE 2048.0f
#endif
#ifndef PH_SCALE_VALUE
#define PH_SCALE_VALUE 235.0f
#endif
#ifndef PH_OFFSET_VALUE
#define PH_OFFSET_VALUE 6.92f
#endif

// ================================================================
// User configuration
// ================================================================
static const char* WIFI_SSID = WIFI_SSID_VALUE;
static const char* WIFI_PASS = WIFI_PASS_VALUE;

static const char* MQTT_HOST = MQTT_HOST_VALUE;
static const int   MQTT_PORT = MQTT_PORT_VALUE;
static const char* MQTT_USER = MQTT_USER_VALUE;
static const char* MQTT_PASS = MQTT_PASS_VALUE;

// Optional: set the MAC address of your Blue Connect Go, e.g. "aa:bb:cc:dd:ee:ff".
// Leave empty to search by service UUID.
static const char* BLUECONNECT_MAC = BLUECONNECT_MAC_VALUE;

static const char* DEVICE_ID   = "blueconnect_go_esp32c3";
static const char* DEVICE_NAME = "BlueConnect Go ESP32-C3";
static const char* HOSTNAME    = "blueconnect-c3";

static const uint32_t MEASURE_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes
static const uint32_t MEASURE_RETRY_INTERVAL_MS = 60UL * 1000UL;  // 1 minute after failure
static const uint32_t BLE_SCAN_SECONDS    = 20;
static const uint16_t BLE_SCAN_INTERVAL_MS = 80;
static const uint16_t BLE_SCAN_WINDOW_MS   = 80;
static const uint32_t BLE_NOTIFY_TIMEOUT_MS = 35000;
static const bool ENABLE_DIAGNOSTICS = true;
static const esp_power_level_t BLE_TX_POWER = ESP_PWR_LVL_P9;
static const float PH_CENTER = PH_CENTER_VALUE;
static const float PH_SCALE = PH_SCALE_VALUE;
static const float PH_OFFSET = PH_OFFSET_VALUE;

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
static const char* TOPIC_CMD_MEASURE = "blueconnect/go/measure/set";
static const char* TOPIC_CMD_SCAN = "blueconnect/go/scan/set";
static const char* TOPIC_CMD_REBOOT = "blueconnect/go/reboot/set";

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
  uint16_t phRaw = 0;
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
static const uint8_t MAX_SCAN_RESULTS = 12;
String lastAdvertisement = "";
String scanResultAddresses[MAX_SCAN_RESULTS];
String scanResultLines[MAX_SCAN_RESULTS];
uint8_t scanResultCount = 0;
uint32_t scanAdvertisementCount = 0;
uint32_t lastScanMs = 0;
uint32_t lastAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
bool lastReadOk = false;
bool retainedStateCleared = false;
bool measurementRequested = false;
bool measurementInProgress = false;
bool scanRequested = false;
bool scanOnlyInProgress = false;
bool rebootRequested = false;
bool targetSeen = false;

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

void resetScanResults() {
  lastAdvertisement = "";
  scanResultCount = 0;
  scanAdvertisementCount = 0;
  for (uint8_t i = 0; i < MAX_SCAN_RESULTS; i++) {
    scanResultAddresses[i] = "";
    scanResultLines[i] = "";
  }
}

void rememberAdvertisement(const String& address, const String& line) {
  scanAdvertisementCount++;
  lastAdvertisement = line;

  for (uint8_t i = 0; i < scanResultCount; i++) {
    if (scanResultAddresses[i] == address) {
      scanResultLines[i] = line;
      return;
    }
  }

  if (scanResultCount < MAX_SCAN_RESULTS) {
    scanResultAddresses[scanResultCount] = address;
    scanResultLines[scanResultCount] = line;
    scanResultCount++;
    return;
  }

  for (uint8_t i = 1; i < MAX_SCAN_RESULTS; i++) {
    scanResultAddresses[i - 1] = scanResultAddresses[i];
    scanResultLines[i - 1] = scanResultLines[i];
  }
  scanResultAddresses[MAX_SCAN_RESULTS - 1] = address;
  scanResultLines[MAX_SCAN_RESULTS - 1] = line;
}

String htmlEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

String scanResultsText() {
  if (scanResultCount == 0) return "No advertisements seen yet";
  String out;
  for (uint8_t i = 0; i < scanResultCount; i++) {
    if (i > 0) out += "\n";
    out += scanResultLines[i];
  }
  return out;
}

String bleTxPowerText() {
  return "+9 dBm";
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
  const float ph = (PH_CENTER - rawPh) / PH_SCALE + PH_OFFSET;
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
  m.phRaw = rawPh;
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
    if (foundDevice) return;

    const String address = advertisedDevice->getAddress().toString().c_str();
    const bool hasService = advertisedDevice->isAdvertisingService(BLUE_SERVICE_UUID);
    const bool hasTargetMac = macMatches(advertisedDevice->getAddress().toString());
    const bool isTarget = (strlen(BLUECONNECT_MAC) > 0 && hasTargetMac) ||
                          (strlen(BLUECONNECT_MAC) == 0 && hasService);

    if (isTarget) {
      targetSeen = true;
    }

    if (ENABLE_DIAGNOSTICS) {
      String line = address +
                    " RSSI=" + String(advertisedDevice->getRSSI()) +
                    " name=" + String(advertisedDevice->getName().c_str()) +
                    " service=" + String(hasService ? "yes" : "no");
      rememberAdvertisement(address, line);
      Serial.println("[SCAN] " + line);
    }

    if (isTarget) {
      foundDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
      NimBLEDevice::getScan()->stop();
    }
  }

};

AdvertisedCallbacks advertisedCallbacks;

void ensureMqtt();
void handleBackground();
void publishDiscovery();
void publishState();
void publishDiagnostics(const char* reason);
void mqttCallback(char* topic, uint8_t* payload, unsigned int length);
void clearRetainedState();

String currentOperationState() {
  if (measurementInProgress) return "measuring";
  if (scanOnlyInProgress) return "scanning";
  if (measurementRequested) return "measurement queued";
  if (scanRequested) return "scan queued";
  return "idle";
}

bool scanBlueConnectOnly() {
  scanOnlyInProgress = true;
  last.lastError = "scanning";
  targetSeen = false;
  resetScanResults();
  if (foundDevice) { delete foundDevice; foundDevice = nullptr; }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&advertisedCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);

  Serial.println("[BLE] Manual scan start");
  lastScanMs = millis();
  scan->start(BLE_SCAN_SECONDS, false);
  scan->clearResults();

  bool ok = false;
  if (foundDevice) {
    last.mac = foundDevice->getAddress().toString().c_str();
    last.rssi = foundDevice->getRSSI();
    last.lastError = "scan found blueconnect";
    Serial.printf("[BLE] Scan found %s RSSI=%d\n", last.mac.c_str(), last.rssi);
    delete foundDevice;
    foundDevice = nullptr;
    publishDiagnostics("scan_found");
    ok = true;
  } else {
    last.lastError = "blueconnect not found";
    publishDiagnostics("scan_not_found");
  }

  scanOnlyInProgress = false;
  return ok;
}

bool scanAndReadBlueConnect() {
  measurementInProgress = true;
  lastAttemptMs = millis();
  last.lastError = "scanning";
  targetSeen = false;
  resetScanResults();
  notificationReceived = false;
  notificationPayload.clear();
  if (foundDevice) { delete foundDevice; foundDevice = nullptr; }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&advertisedCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);

  Serial.println("[BLE] Scan start");
  lastScanMs = millis();
  scan->start(BLE_SCAN_SECONDS, false);
  scan->clearResults();

  if (!foundDevice) {
    last.lastError = "blueconnect not found";
    measurementInProgress = false;
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
    measurementInProgress = false;
    publishDiagnostics("connect_failed");
    return false;
  }

  NimBLERemoteService* service = client->getService(BLUE_SERVICE_UUID);
  if (!service) {
    last.lastError = "service not found";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    measurementInProgress = false;
    publishDiagnostics("service_not_found");
    return false;
  }

  NimBLERemoteCharacteristic* notifyChr = service->getCharacteristic(BLUE_NOTIFY_UUID);
  NimBLERemoteCharacteristic* writeChr  = service->getCharacteristic(BLUE_WRITE_UUID);

  if (!notifyChr || !writeChr) {
    last.lastError = "characteristic not found";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    measurementInProgress = false;
    publishDiagnostics("characteristic_not_found");
    return false;
  }

  if (notifyChr->canNotify()) {
    if (!notifyChr->subscribe(true, notifyCallback)) {
      last.lastError = "notify subscribe failed";
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      measurementInProgress = false;
      publishDiagnostics("notify_subscribe_failed");
      return false;
    }
  } else {
    last.lastError = "notify characteristic cannot notify";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    measurementInProgress = false;
    publishDiagnostics("notify_not_supported");
    return false;
  }

  uint8_t trigger = 0x01;
  Serial.println("[BLE] Write trigger 0x01");
  if (!writeChr->writeValue(&trigger, 1, true)) {
    last.lastError = "write trigger failed";
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    measurementInProgress = false;
    publishDiagnostics("write_failed");
    return false;
  }

  uint32_t start = millis();
  while (!notificationReceived && millis() - start < BLE_NOTIFY_TIMEOUT_MS) {
    handleBackground();
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
  measurementInProgress = false;
  return ok;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    ArduinoOTA.handle();
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
  }
}

void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA
    .onStart([]() {
      Serial.println("[OTA] Start");
    })
    .onEnd([]() {
      Serial.println("\n[OTA] End");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
    })
    .onError([](ota_error_t error) {
      Serial.printf("[OTA] Error[%u]\n", error);
    });
  ArduinoOTA.begin();
  Serial.printf("[OTA] Ready: %s.local\n", HOSTNAME);
}

void handleBackground() {
  ArduinoOTA.handle();
  server.handleClient();
  mqtt.loop();
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttemptMs < 3000) return;
  lastMqttAttemptMs = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  Serial.printf("[MQTT] Connecting to %s:%d\n", MQTT_HOST, MQTT_PORT);
  bool ok;
  if (strlen(MQTT_USER) > 0) ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS, TOPIC_AVAIL, 1, true, "offline");
  else ok = mqtt.connect(DEVICE_ID, TOPIC_AVAIL, 1, true, "offline");

  if (ok) {
    Serial.println("[MQTT] Connected");
    mqtt.publish(TOPIC_AVAIL, "online", true);
    mqtt.subscribe(TOPIC_CMD_MEASURE);
    mqtt.subscribe(TOPIC_CMD_SCAN);
    mqtt.subscribe(TOPIC_CMD_REBOOT);
    publishDiscovery();
    if (last.valid) {
      publishState();
    } else {
      clearRetainedState();
    }
  } else {
    Serial.printf("[MQTT] failed rc=%d\n", mqtt.state());
  }
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  const bool isPress = length == 0 || message.equalsIgnoreCase("PRESS") ||
                       message.equalsIgnoreCase("ON") || message == "1" ||
                       message.equalsIgnoreCase("true");
  if (!isPress) {
    Serial.printf("[MQTT] Ignoring command topic=%s payload=%s\n", topic, message.c_str());
    return;
  }

  if (strcmp(topic, TOPIC_CMD_MEASURE) == 0) {
    if (!measurementInProgress && !scanOnlyInProgress) {
      measurementRequested = true;
      last.lastError = "measure queued by mqtt";
      Serial.println("[MQTT] Measure queued");
    }
  } else if (strcmp(topic, TOPIC_CMD_SCAN) == 0) {
    if (!measurementInProgress && !scanOnlyInProgress) {
      scanRequested = true;
      last.lastError = "scan queued by mqtt";
      Serial.println("[MQTT] Scan queued");
    }
  } else if (strcmp(topic, TOPIC_CMD_REBOOT) == 0) {
    rebootRequested = true;
    last.lastError = "reboot queued by mqtt";
    Serial.println("[MQTT] Reboot queued");
  }
}

void publishJson(const char* topic, JsonDocument& doc, bool retained=false) {
  char buf[1024];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topic, (const uint8_t*)buf, n, retained);
}

void clearRetainedState() {
  if (retainedStateCleared) return;
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_STATE, "", true);
  retainedStateCleared = true;
  Serial.println("[MQTT] Cleared retained state until first valid measurement");
}

void publishState() {
  ensureMqtt();
  if (!last.valid) {
    clearRetainedState();
    Serial.println("[MQTT] Skip state publish without valid measurement");
    return;
  }

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
  doc["ph_raw"] = last.phRaw;
  doc["battery_raw"] = last.batteryRaw;
  doc["conductivity_raw"] = last.conductivityRaw;
  doc["status_raw"] = last.statusRaw;
  doc["rssi"] = last.rssi;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["mac"] = last.mac;
  doc["raw_hex"] = last.rawHex;
  doc["last_error"] = last.lastError;
  doc["uptime_s"] = millis() / 1000;
  publishJson(TOPIC_STATE, doc, true);
  retainedStateCleared = true;
}

void publishDiagnostics(const char* reason) {
  ensureMqtt();
  JsonDocument doc;
  doc["reason"] = reason;
  doc["last_error"] = last.lastError;
  doc["last_advertisement"] = lastAdvertisement;
  doc["scan_seen_count"] = scanAdvertisementCount;
  JsonArray scanResults = doc["scan_results"].to<JsonArray>();
  for (uint8_t i = 0; i < scanResultCount; i++) {
    scanResults.add(scanResultLines[i]);
  }
  doc["free_heap"] = ESP.getFreeHeap();
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ble_tx_power"] = bleTxPowerText();
  doc["ble_scan_seconds"] = BLE_SCAN_SECONDS;
  doc["ble_scan_interval_ms"] = BLE_SCAN_INTERVAL_MS;
  doc["ble_scan_window_ms"] = BLE_SCAN_WINDOW_MS;
  doc["ph_center"] = PH_CENTER;
  doc["ph_scale"] = PH_SCALE;
  doc["ph_offset"] = PH_OFFSET;
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

void discoveryButton(const char* objectId, const char* name, const char* commandTopic) {
  JsonDocument doc;
  String uniqueId = String(DEVICE_ID) + "_" + objectId;
  doc["name"] = name;
  doc["unique_id"] = uniqueId;
  doc["command_topic"] = commandTopic;
  doc["payload_press"] = "PRESS";
  doc["availability_topic"] = TOPIC_AVAIL;
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0] = DEVICE_ID;
  dev["name"] = DEVICE_NAME;
  dev["manufacturer"] = "DIY ESP32-C3";
  dev["model"] = "BlueConnect BLE MQTT Bridge";

  String topic = "homeassistant/button/" + String(DEVICE_ID) + "/" + objectId + "/config";
  publishJson(topic.c_str(), doc, true);
}

void publishDiscovery() {
  discoverySensor("temperature", "Pool Temperature", "temperature", "°C", "{{ value_json.temperature }}");
  discoverySensor("ph", "Pool pH", "ph", "", "{{ value_json.ph }}");
  discoverySensor("orp", "Pool ORP", "voltage", "mV", "{{ value_json.orp }}");
  discoverySensor("chlorine", "Pool Free Chlorine", "", "ppm", "{{ value_json.chlorine }}");
  discoverySensor("ec", "Pool Conductivity", "", "µS/cm", "{{ value_json.ec }}");
  discoverySensor("salt", "Pool Salt", "", "ppm", "{{ value_json.salt }}");
  discoverySensor("battery", "BlueConnect Battery", "battery", "%", "{{ value_json.battery }}");
  discoverySensor("battery_voltage", "BlueConnect Battery Voltage", "voltage", "V", "{{ value_json.battery_voltage }}");
  discoverySensor("rssi", "BlueConnect BLE RSSI", "signal_strength", "dBm", "{{ value_json.rssi }}");
  discoverySensor("wifi_rssi", "BlueConnect WiFi RSSI", "signal_strength", "dBm", "{{ value_json.wifi_rssi }}");
  discoverySensor("ph_raw", "BlueConnect pH Raw", "", "", "{{ value_json.ph_raw }}", "");
  discoverySensor("battery_raw", "BlueConnect Battery Raw", "", "mV", "{{ value_json.battery_raw }}", "");
  discoverySensor("conductivity_raw", "BlueConnect Conductivity Raw", "", "", "{{ value_json.conductivity_raw }}", "");
  discoveryButton("measure_now", "BlueConnect Measure Now", TOPIC_CMD_MEASURE);
  discoveryButton("scan_ble", "BlueConnect BLE Scan", TOPIC_CMD_SCAN);
  discoveryButton("reboot", "BlueConnect Reboot", TOPIC_CMD_REBOOT);
}

String htmlPage() {
  String s;
  String operationState = currentOperationState();
  s += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>BlueConnect ESP32-C3</title><style>body{font-family:system-ui;margin:24px;max-width:900px} .card{border:1px solid #ddd;border-radius:12px;padding:16px;margin:12px 0} code{background:#f5f5f5;padding:2px 4px;border-radius:4px} button{padding:10px 14px;border-radius:8px;border:1px solid #999;background:white} .muted{color:#666;font-size:.9em}</style></head><body>";
  s += "<h1>BlueConnect ESP32-C3</h1>";
  s += "<div class='card'><h2>Measurements</h2>";
  s += "Temperature: <b><span id='temperature'>" + String(last.temperatureC, 2) + "</span> &deg;C</b><br>";
  s += "pH: <b><span id='ph'>" + String(last.ph, 2) + "</span></b><br>";
  s += "ORP: <b><span id='orp'>" + String(last.orpMv, 0) + "</span> mV</b><br>";
  s += "Free chlorine: <b><span id='chlorine'>" + String(last.chlorinePpm, 2) + "</span> ppm</b><br>";
  s += "Conductivity: <b><span id='ec'>" + String(last.ecUsCm, 0) + "</span> &micro;S/cm</b><br>";
  s += "Salt: <b><span id='salt'>" + String(last.saltPpm, 0) + "</span> ppm</b><br>";
  s += "Battery: <b><span id='battery'>" + String(last.batteryPercent, 0) + "</span> %</b><br>";
  s += "Battery voltage: <b><span id='battery_voltage'>" + String(last.batteryVoltage, 2) + "</span> V</b><br>";
  s += "pH raw: <b><span id='ph_raw'>" + String(last.phRaw) + "</span></b><br>";
  s += "Battery raw: <b><span id='battery_raw'>" + String(last.batteryRaw) + "</span> mV</b><br>";
  s += "Conductivity raw: <b><span id='conductivity_raw'>" + String(last.conductivityRaw) + "</span></b><br>";
  s += "Status raw: <b><span id='status_raw'>" + String(last.statusRaw) + "</span></b><br>";
  s += "RSSI: <b><span id='rssi'>" + String(last.rssi) + "</span> dBm</b><br>";
  s += "Raw: <code id='raw_hex'>" + last.rawHex + "</code></div>";
  s += "<div class='card'><h2>Status</h2>";
  s += "Operation: <b id='operation'>" + operationState + "</b><br>";
  s += "Target MAC: <code id='target_mac'>" + String(BLUECONNECT_MAC) + "</code><br>";
  s += "MAC: <code id='mac'>" + last.mac + "</code><br>";
  s += "Last error: <code id='last_error'>" + last.lastError + "</code><br>";
  s += "Last advertisement: <code id='last_advertisement'>" + lastAdvertisement + "</code><br>";
  s += "Advertisements seen: <span id='scan_seen_count'>" + String(scanAdvertisementCount) + "</span><br>";
  s += "Heap: <span id='free_heap'>" + String(ESP.getFreeHeap()) + "</span> Bytes<br>";
  s += "Wi-Fi RSSI: <span id='wifi_rssi'>" + String(WiFi.RSSI()) + "</span> dBm<br>";
  s += "BLE TX power: <span id='ble_tx_power'>" + bleTxPowerText() + "</span><br>";
  s += "BLE scan: <span id='ble_scan_config'>" + String(BLE_SCAN_SECONDS) + " s, " + String(BLE_SCAN_WINDOW_MS) + "/" + String(BLE_SCAN_INTERVAL_MS) + " ms</span><br>";
  s += "pH calibration: <span id='ph_calibration'>" + String(PH_CENTER, 2) + " / " + String(PH_SCALE, 2) + " / " + String(PH_OFFSET, 2) + "</span><br>";
  s += "Uptime: <span id='uptime_s'>" + String(millis()/1000) + "</span> s<br>";
  s += "<span class='muted'>Live refresh: <span id='live_status'>starting</span></span></div>";
  s += "<div class='card'><h2>Scan Results</h2><pre id='scan_results' style='white-space:pre-wrap;margin:0'>" + htmlEscape(scanResultsText()) + "</pre></div>";
  s += "<div class='card'><form id='measure_form' action='/measure' method='post' style='display:inline-block;margin-right:8px'><button>Measure now</button></form>";
  s += "<form id='scan_form' action='/scan' method='post' style='display:inline-block'><button>Scan</button></form> ";
  s += "<p>JSON: <a href='/api/state'>/api/state</a> &middot; Diagnostics: <a href='/api/diagnostics'>/api/diagnostics</a></p></div>";
  s += "<script>";
  s += "const $=id=>document.getElementById(id);";
  s += "function set(id,v){const e=$(id);if(e)e.textContent=v;}";
  s += "function fmt(v,d){return typeof v==='number'&&isFinite(v)?v.toFixed(d):'nan';}";
  s += "function val(v,f){return v==null?f:v;}";
  s += "async function postAction(url){set('live_status','sending');try{await fetch(url,{method:'POST'});await refresh();}catch(e){set('live_status','offline');}}";
  s += "async function refresh(){try{const st=await fetch('/api/state',{cache:'no-store'}).then(r=>r.json());const dg=await fetch('/api/diagnostics',{cache:'no-store'}).then(r=>r.json());";
  s += "set('temperature',fmt(st.temperature,2));set('ph',fmt(st.ph,2));set('orp',fmt(st.orp,0));set('chlorine',fmt(st.chlorine,2));set('ec',fmt(st.ec,0));set('salt',fmt(st.salt,0));";
  s += "set('battery',fmt(st.battery,0));set('battery_voltage',fmt(st.battery_voltage,2));set('ph_raw',val(st.ph_raw,0));set('battery_raw',val(st.battery_raw,0));set('conductivity_raw',val(st.conductivity_raw,0));set('status_raw',val(st.status_raw,0));";
  s += "set('rssi',val(st.rssi,0));set('raw_hex',st.raw_hex||'');set('operation',st.operation||dg.operation||'idle');set('mac',st.mac||'');set('last_error',st.last_error||dg.last_error||'');";
  s += "set('target_mac',dg.target_mac||'');set('last_advertisement',dg.last_advertisement||'');set('scan_seen_count',val(dg.scan_seen_count,0));set('scan_results',(dg.scan_results&&dg.scan_results.length)?dg.scan_results.join('\\n'):'No advertisements seen yet');";
  s += "set('free_heap',val(dg.free_heap,''));set('wifi_rssi',val(dg.wifi_rssi,''));";
  s += "set('ble_tx_power',dg.ble_tx_power||'');set('ble_scan_config',val(dg.ble_scan_seconds,'')+' s, '+val(dg.ble_scan_window_ms,'')+'/'+val(dg.ble_scan_interval_ms,'')+' ms');";
  s += "set('ph_calibration',fmt(dg.ph_center,2)+' / '+fmt(dg.ph_scale,2)+' / '+fmt(dg.ph_offset,2));";
  s += "set('uptime_s',val(dg.uptime_s,''));set('live_status','ok');";
  s += "}catch(e){set('live_status','offline');}}";
  s += "$('measure_form').addEventListener('submit',e=>{e.preventDefault();postAction('/measure');});";
  s += "$('scan_form').addEventListener('submit',e=>{e.preventDefault();postAction('/scan');});";
  s += "refresh();setInterval(refresh,2000);";
  s += "</script>";
  s += "</body></html>";
  return s;
}

void setupWeb() {
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", htmlPage()); });
  server.on("/measure", HTTP_POST, [](){
    if (!measurementInProgress) {
      measurementRequested = true;
    }
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", measurementInProgress ? "measurement already running" : "measurement queued");
  });
  server.on("/scan", HTTP_POST, [](){
    if (!measurementInProgress && !scanOnlyInProgress) {
      scanRequested = true;
      last.lastError = "scan queued";
      Serial.println("[WEB] Scan queued");
    }
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", (measurementInProgress || scanOnlyInProgress) ? "operation already running" : "scan queued");
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
    doc["ph_raw"] = last.phRaw;
    doc["battery_raw"] = last.batteryRaw;
    doc["conductivity_raw"] = last.conductivityRaw;
    doc["status_raw"] = last.statusRaw;
    doc["rssi"] = last.rssi;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["mac"] = last.mac;
    doc["raw_hex"] = last.rawHex;
    doc["last_error"] = last.lastError;
    doc["operation"] = currentOperationState();
    String out; serializeJsonPretty(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/diagnostics", HTTP_GET, [](){
    JsonDocument doc;
    doc["last_advertisement"] = lastAdvertisement;
    doc["target_mac"] = BLUECONNECT_MAC;
    doc["scan_seen_count"] = scanAdvertisementCount;
    JsonArray scanResults = doc["scan_results"].to<JsonArray>();
    for (uint8_t i = 0; i < scanResultCount; i++) {
      scanResults.add(scanResultLines[i]);
    }
    doc["free_heap"] = ESP.getFreeHeap();
    doc["uptime_s"] = millis()/1000;
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ble_tx_power"] = bleTxPowerText();
    doc["ble_scan_seconds"] = BLE_SCAN_SECONDS;
    doc["ble_scan_interval_ms"] = BLE_SCAN_INTERVAL_MS;
    doc["ble_scan_window_ms"] = BLE_SCAN_WINDOW_MS;
    doc["ph_center"] = PH_CENTER;
    doc["ph_scale"] = PH_SCALE;
    doc["ph_offset"] = PH_OFFSET;
    doc["last_error"] = last.lastError;
    doc["operation"] = currentOperationState();
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
  setupOta();
  mqtt.setBufferSize(1024);
  ensureMqtt();
  setupWeb();

  NimBLEDevice::init("BlueConnect-C3");
  NimBLEDevice::setPower(BLE_TX_POWER);

  publishDiagnostics("boot");
  lastReadOk = scanAndReadBlueConnect();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }
  ensureMqtt();
  handleBackground();

  if (rebootRequested) {
    rebootRequested = false;
    last.lastError = "rebooting";
    publishDiagnostics("reboot_requested");
    mqtt.publish(TOPIC_AVAIL, "offline", true);
    mqtt.loop();
    delay(250);
    ESP.restart();
  }

  const uint32_t intervalMs = lastReadOk ? MEASURE_INTERVAL_MS : MEASURE_RETRY_INTERVAL_MS;
  const bool scheduledMeasurementDue = lastAttemptMs == 0 || millis() - lastAttemptMs >= intervalMs;
  if (!measurementInProgress && !scanOnlyInProgress && measurementRequested) {
    measurementRequested = false;
    lastReadOk = scanAndReadBlueConnect();
  } else if (!measurementInProgress && !scanOnlyInProgress && scanRequested) {
    scanRequested = false;
    scanBlueConnectOnly();
  } else if (!measurementInProgress && !scanOnlyInProgress && scheduledMeasurementDue) {
    lastReadOk = scanAndReadBlueConnect();
  }

  delay(5);
}
