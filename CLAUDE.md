# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

PlatformIO firmware for an ESP32-S3 hydroponics controller. It reads a DS18B20 temperature probe, a TDS sensor, and a pH sensor, drives a water-fill pump relay based on float-switch levels, and syncs state with a Firebase Realtime Database so a remote dashboard can view sensor data and override pump control.

## Commands

This is a PlatformIO project (`platformio.ini`), built via the PlatformIO CLI or the VS Code/PlatformIO IDE extension.

- Build: `pio run`
- Upload to board: `pio run -t upload`
- Serial monitor (115200 baud): `pio device monitor`
- Build + upload + monitor: `pio run -t upload -t monitor`
- Clean: `pio run -t clean`

There is no test suite configured (`test/` only contains PlatformIO's placeholder README).

Target board: `esp32-s3-devkitm-1` (Arduino framework). USB CDC-on-boot is enabled, so `Serial` becomes available over native USB after boot — the `delay(2000)` at the top of `setup()` exists to give the host time to attach before the first log lines print.

## Architecture

Everything lives in `src/main.cpp` as a single Arduino sketch (`setup()` / `loop()`), no custom classes or additional source files.

**Control flow is cooperative multitasking via `millis()`-gated intervals inside one `loop()`**, not RTOS tasks or interrupts:
- Firebase stream polling (`pollFirebaseStream()`) runs every iteration, unthrottled — this keeps the RTDB stream connection alive.
- Sensor read + pump automation runs every `SENSOR_INTERVAL` (1s).
- Firebase upload (`uploadToFirebase()`) runs every `FB_UPLOAD_INTERVAL` (3s), deliberately staggered slower than the sensor loop to reduce payload/timeout pressure on the Firebase client.

**Pump control has three layers, evaluated in this priority order inside the sensor block:**
1. Float-switch safety fault (`lowSwitchOpen && topSwitchClosed`, i.e. both switches disagree) forces the pump off and reports `SENSOR ERROR`, overriding everything else including cloud override.
2. `pumpOverride` (an `int`, mirrored from Firebase path `/control/pumpOverride` via the RTDB stream): `1` = force fill, `0` = force off, `-1` = AUTO.
3. AUTO mode runs a simple fill/idle state machine driven by the low and high float switches (`isFilling` persists across loop iterations to know whether a fill is in progress).

`RELAY_1` is active-low (`LOW` = pump on, `HIGH` = pump off, default `HIGH` at boot).

**Firebase wiring** (`initFirebase()`):
- Anonymous auth via `Firebase.signUp()`.
- Two separate `FirebaseData` handles are used deliberately: `fbdo` is dedicated to the long-lived `/control/pumpOverride` stream, `fbdoUpload` is dedicated to periodic `/sensor` writes — mixing stream and request/response traffic on one handle is a common source of Firebase client bugs, hence the split.
- SSL buffer sizes and `fbdoUpload`'s response size are both tuned down/up (see inline `// Fix:` comments in the code) to work around payload timeout issues on unstable/extended Wi-Fi; if you touch Firebase timing or buffers, keep those constraints in mind.
- If Wi-Fi fails to connect within 30 attempts (~15s), the device falls back to running sensors/pump AUTO logic fully offline — Firebase is simply never initialized.

**Sensor conversions to be aware of when editing:**
- ADC attenuation is set to `ADC_2_5db` (~1.5V max), and `ANALOG_MAX_VOLTAGE` must match that hardware configuration — the code has an inline warning about this; if the TDS/pH sensor hardware outputs up to 3.3V, both the attenuation call and `ANALOG_MAX_VOLTAGE` need to change together.
- TDS uses a temperature-compensated voltage before applying the cubic calibration polynomial.
- pH uses a linear approximation around a 1.50V neutral point.

## Configuration / secrets

Wi-Fi credentials and the Firebase API key/database URL live in `include/secrets.h` (gitignored, `#include`d from `src/main.cpp`), not in source. A committed `include/secrets.h.example` documents the expected macros (`WIFI_SSID`, `WIFI_PASSWORD`, `FIREBASE_API_KEY`, `FIREBASE_DATABASE_URL`) — copy it to `include/secrets.h` and fill in real values on a fresh checkout.

Note: the real credentials were committed in plaintext in earlier history (before this file existed) and pushed to the `origin` remote. Moving them out of `src/main.cpp` does not remove them from git history — treat those specific values as compromised and rotate them (new Wi-Fi password, new/regenerated Firebase API key) if this repo's history is or becomes visible to anyone untrusted.
