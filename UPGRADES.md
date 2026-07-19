# Upgrades / TODO

Tracked improvements for the hydroponics controller, not yet implemented. Roughly ordered by priority within each section.

## Safety

- [ ] **Watchdog timer.** Nothing recovers the board if `loop()` ever blocks (e.g. a hung Firebase call) while the pump is mid-fill. Add `esp_task_wdt` so a hang triggers a reset instead of leaving the relay on indefinitely.

## Correctness / robustness

- [ ] **Smooth ADC readings.** `analogRead(TDS_PIN)` / `analogRead(PH_PIN)` are single-sample reads taken once a second; ESP32 ADC is noisy. Add a small moving average (or median-of-N) before applying the calibration math to stabilize reported TDS/pH.
- [ ] **Wi-Fi retry backoff.** The reconnect loop (`WIFI_RETRY_INTERVAL`, `src/main.cpp`) retries every 30s indefinitely with no backoff. Low priority — fine for now, but consider exponential backoff (capped) if the AP is expected to be down for extended periods.

## Maintainability

- [ ] **Name the calibration magic numbers.** The TDS cubic polynomial coefficients (`133.42`, `-255.86`, `857.39`) and the pH linear approximation constants (`1.50`, `0.18`) are inlined in `loop()`. Pull them into named constants at the top of the file so recalibrating the sensors doesn't require hunting through the control loop.
- [ ] **Extract testable conversion functions.** TDS/pH math is inlined in `loop()`. Factor out `computeTDS(rawADC, tempC)` / `computePH(rawADC)` as free functions so they can be covered by PlatformIO unit tests independent of hardware.

## Notes

- This file is for tracking, not history — once an item ships, remove it (git log has the record) rather than checking it off and leaving it.
