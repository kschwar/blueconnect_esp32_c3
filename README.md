# BlueConnect Go ESP32-C3 MQTT Bridge

ESP32-C3 firmware that reads a Blue Connect Go over BLE and publishes the values to MQTT with Home Assistant MQTT Discovery.

This project is intended as a small standalone bridge: the ESP32-C3 connects to Wi-Fi, scans for the Blue Connect Go, triggers a BLE measurement, decodes the notification payload, and publishes the result to Home Assistant.

All measurements are read locally over Bluetooth Low Energy. The Blue Connect cloud/API is not used.

## Features

- ESP32-C3 as BLE central
- Blue Connect Go measurement trigger over BLE
- Fully local Bluetooth Low Energy readout, no Blue Connect API required
- MQTT state and diagnostics topics
- Home Assistant MQTT Discovery
- Small local web interface
- Live BLE scan results in the web interface
- Optional fixed Blue Connect MAC address
- Retry after failed reads
- Raw payload and diagnostics for calibration/debugging

## Hardware

- ESP32-C3 DevKitM-1 or compatible ESP32-C3 board
- Blue Connect Go
- MQTT broker reachable from the ESP32 network
- Home Assistant with MQTT integration enabled

## BLE Protocol

The BLE UUIDs and decoder are based on community reverse engineering from:

- https://github.com/adamantivm/BlueConnect

The bridge writes `0x01` to characteristic `F3300002-F0A2-9B06-0C59-1BC4763B5C00` and waits for a notification on `F3300003-F0A2-9B06-0C59-1BC4763B5C00`.

The decoder currently derives:

- temperature
- pH
- ORP
- estimated free chlorine
- conductivity / salt estimate
- battery voltage and percentage
- RSSI and raw BLE payload

Important: chlorine, EC, and salt are estimates derived from the BLE payload and empirical formulas. Treat them as approximate values, not lab-grade measurements.

## Setup

1. Install Visual Studio Code.
2. Install the PlatformIO extension.
3. Open this project folder in PlatformIO.
4. Copy `include/secrets_example.h` to `include/secrets.h`.
5. Edit `include/secrets.h`.
6. Connect the ESP32-C3 by USB.
7. Build and upload with PlatformIO.
8. Open the serial monitor at `115200` baud.

`include/secrets.h` is ignored by Git and should not be committed.

## Dependencies

The project uses PlatformIO and pins the BLE stack to `h2zero/NimBLE-Arduino@1.4.3`.

NimBLE-Arduino 2.x changes parts of the scan and connection behavior. This firmware currently targets 1.4.3 because it has been more reliable with the tested Blue Connect Go device.

## Upload

The first upload must be done over USB so the OTA-enabled firmware is installed:

```bash
pio run -e esp32-c3-devkitm-1 -t upload
```

After the ESP32 is connected to Wi-Fi, later uploads can be done over the network:

```bash
pio run -e esp32-c3-devkitm-1-ota -t upload
```

The OTA environment uses `blueconnect-c3.local` as upload target. If mDNS does not work in your network, replace `upload_port` in `platformio.ini` with the ESP32 IP address.

## Monitoring

PlatformIO OTA only covers firmware upload. `pio device monitor` and the VS Code "Monitor" task still use a serial port, even when the OTA environment is selected. For live remote diagnostics over Wi-Fi, use the web UI, `/api/diagnostics`, or the MQTT diagnostics topic.

USB serial monitor:

```bash
pio device monitor -e esp32-c3-devkitm-1
```

Remote status without USB:

```text
http://blueconnect-c3.local/
http://blueconnect-c3.local/api/diagnostics
```

## Configuration

Create `include/secrets.h` from `include/secrets_example.h`:

```cpp
#pragma once

#define WIFI_SSID_VALUE "YOUR_WIFI"
#define WIFI_PASS_VALUE "YOUR_WIFI_PASSWORD"
#define MQTT_HOST_VALUE "192.168.1.10"
#define MQTT_PORT_VALUE 1883
#define MQTT_USER_VALUE ""
#define MQTT_PASS_VALUE ""
#define BLUECONNECT_MAC_VALUE ""
```

`BLUECONNECT_MAC_VALUE` can be left empty to search by BLE service UUID. For more reliable operation, set the fixed MAC address of your Blue Connect Go.

Main timing values are defined in `src/main.cpp`:

```cpp
MEASURE_INTERVAL_MS        // 15 minutes after a successful read
MEASURE_RETRY_INTERVAL_MS  // 1 minute after a failed read
BLE_SCAN_SECONDS           // scan window
BLE_NOTIFY_TIMEOUT_MS      // wait time for BLE notification
```

## MQTT Topics

State:

```text
blueconnect/go/state
```

Availability:

```text
blueconnect/go/availability
```

Diagnostics:

```text
blueconnect/go/diagnostics
```

Example state payload:

```json
{
  "temperature": 25.9,
  "ph": 7.75,
  "orp": 576,
  "chlorine": 0,
  "ec": null,
  "salt": null,
  "battery": 92,
  "battery_voltage": 3.62,
  "battery_raw": 3620,
  "conductivity_raw": 0,
  "rssi": -66,
  "mac": "AA:BB:CC:DD:EE:FF",
  "raw_hex": "33140A5607750800001E0E1D",
  "last_error": "ok",
  "uptime_s": 123
}
```

## Home Assistant

The firmware publishes MQTT Discovery config under:

```text
homeassistant/sensor/blueconnect_go_esp32c3/...
```

After the first successful MQTT connection, Home Assistant should create sensor entities for temperature, pH, ORP, chlorine, EC, salt, battery, battery voltage, RSSI, and raw diagnostic values.

## Web Interface

Open:

```text
http://blueconnect-c3.local/
```

If mDNS does not work in your network, use the IP address printed in the serial monitor.

Available endpoints:

- `/` web UI
- `/measure` queue a manual measurement trigger
- `/scan` queue a BLE scan without reading measurements
- `/api/state` current state as JSON
- `/api/diagnostics` diagnostic JSON

The web UI polls the JSON endpoints every 2 seconds, so measurements, scan state, RSSI, BLE scan results, and diagnostics update without reloading the page.

The `Scan Results` section lists recently seen BLE advertisements from the last scan. Entries include MAC address, RSSI, advertised name, and whether the expected Blue Connect service UUID was present in that advertisement. Some Blue Connect advertisements do not expose the service UUID, so a fixed `BLUECONNECT_MAC_VALUE` is recommended.

The firmware does not rely on a BLE `connectable` flag from scan results. On the tested Blue Connect Go this flag can be misleading even when a connection and measurement readout work correctly.

## Behavior

On boot, the ESP32 connects to Wi-Fi and MQTT, starts the web server, initializes BLE, and triggers one measurement.

After that:

- successful read: next automatic read after 15 minutes
- failed read or BLE notification timeout: retry after 1 minute
- manual `/measure` request: queues a read and redirects immediately

The web UI and OTA handler are serviced during BLE scan and notification waits, so the device should remain reachable while a measurement is running. BLE connect and GATT discovery can still block briefly.

The Blue Connect Go may not be continuously reachable over BLE. Make sure the official app is not actively connected while the ESP32 tries to read the sensor.

## Troubleshooting

Serial monitor messages are the best first diagnostic source when USB is connected. Without USB, use the live web UI, `/api/diagnostics`, or the retained MQTT diagnostics topic.

Common messages:

- `blueconnect not found`: sensor did not advertise during the scan window, MAC filter is wrong, or the official app is connected.
- `connect failed`: BLE connection failed; move the ESP closer or retry.
- `notify timeout`: the measurement trigger was sent, but no notification arrived before timeout.
- `parsed values implausible`: a notification arrived, but decoded values failed plausibility checks.
- MQTT reconnect loop: broker host, port, user, password, or network routing is wrong.

The firmware publishes `raw_hex` and diagnostic data so that decoder formulas can be adjusted if your device firmware differs.

For BLE discovery issues, press `Scan` in the web interface and compare `Target MAC` with the addresses listed under `Scan Results`.

## Security

Do not commit `include/secrets.h`. It contains Wi-Fi and MQTT credentials. Use `include/secrets_example.h` only as a template with placeholders.

## Credits

BLE UUIDs and decoding formulas are based on the community work in:

- [adamantivm/BlueConnect](https://github.com/adamantivm/BlueConnect)

## License

MIT. See [LICENSE](LICENSE).

## Status

This is experimental firmware based on community BLE reverse engineering. It is useful for local automation and monitoring, but it is not an official Blue Connect integration.
