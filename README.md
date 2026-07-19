# Hydro — ESP32-S3 Hydroponics Controller

Firmware for an ESP32-S3 board that monitors a hydroponics reservoir and automates topping it up, with live telemetry and remote override via Firebase Realtime Database.

## Features

- **Water temperature** via a DS18B20 (OneWire) probe
- **TDS (nutrient concentration)** via an analog TDS sensor, temperature-compensated
- **pH** via an analog pH sensor
- **Automatic pump control** using two float switches (low/high level) to refill the reservoir
- **Cloud sync** to Firebase Realtime Database: sensor readings are pushed every 3s, and a `/control/pumpOverride` node is streamed live so the pump can be force-started, force-stopped, or left in AUTO from a remote dashboard
- **Offline fallback**: if Wi-Fi fails to connect, the pump still runs in AUTO mode using local sensors only

## Hardware

| Signal | Pin |
|---|---|
| DS18B20 (OneWire bus) | GPIO 4 |
| TDS sensor (analog) | GPIO 5 |
| pH sensor (analog) | GPIO 1 |
| Relay (pump) | GPIO 6 |
| Low water float switch | GPIO 2 |
| High water float switch | GPIO 3 |

The relay output is active-low (pump on = `LOW`). ADC attenuation is configured for a ~1.5V max input range — see `ANALOG_MAX_VOLTAGE` in `src/main.cpp` if your sensor hardware outputs a different range.

## Build & flash

Built with [PlatformIO](https://platformio.org/) for the `esp32-s3-devkitm-1` board.

```
pio run                    # build
pio run -t upload          # flash
pio device monitor         # serial monitor (115200 baud)
```

## Configuration

Wi-Fi credentials and Firebase project settings (API key, database URL) live in `include/secrets.h`, which is gitignored and not committed. To set up a fresh checkout:

```
cp include/secrets.h.example include/secrets.h
```

Then fill in real values in `include/secrets.h`:

```cpp
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."
#define FIREBASE_API_KEY      "..."
#define FIREBASE_DATABASE_URL "..."
```

## Firebase data model

- Writes to `/sensor`: `temperature`, `tds`, `ph`, `pump` (`RUNNING`/`IDLE`), `water_status`, `override`, `plantStatus`, `plantNote`, `ts`
- Reads (streamed) from `/control/pumpOverride`: `-1` = AUTO, `0` = force pump off, `1` = force pump on
- Reads (streamed) from `/camera/detection`: `{ status, note, ts }` — plant health analysis from a companion ESP32-CAM board (see the `esp32s3-test` project), run through OpenAI's vision API by a Cloud Function. Purely informational — folded into the `/sensor` upload for the dashboard, never wired into pump control.
- Written by the companion camera board (not this board) to `/camera/latest`: `{ url, ts }` — a daily snapshot photo, re-uploaded to the same Storage path each day.

## Dependencies

Managed via `platformio.ini`:
- [OneWire](https://github.com/PaulStoffregen/OneWire)
- [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library)
- [Firebase Arduino Client Library for ESP8266 and ESP32](https://github.com/mobizt/Firebase-ESP-Client)
