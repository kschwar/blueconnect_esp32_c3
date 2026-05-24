#pragma once

// Optional: copy this file to secrets.h and adjust it for your setup.
#define WIFI_SSID_VALUE "YOUR_WIFI"
#define WIFI_PASS_VALUE "YOUR_WIFI_PASSWORD"
#define MQTT_HOST_VALUE "192.168.1.10"
#define MQTT_PORT_VALUE 1883
#define MQTT_USER_VALUE ""
#define MQTT_PASS_VALUE ""
#define BLUECONNECT_MAC_VALUE "" // 00:A0:50:XX:XX:XX

// Optional pH calibration. Defaults match the fsedarkalex BlueConnect decoder:
// pH = (PH_CENTER_VALUE - raw_pH) / PH_SCALE_VALUE + PH_OFFSET_VALUE
#define PH_CENTER_VALUE 2048.0f
#define PH_SCALE_VALUE 235.0f
#define PH_OFFSET_VALUE 6.92f
