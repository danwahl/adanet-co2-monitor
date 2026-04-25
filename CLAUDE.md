# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Battery-powered CO₂ monitor firmware for an Adafruit Feather ESP32-S3 with a 2.13" tri-color e-ink FeatherWing, Sensirion SCD4x CO₂ sensor, and DPS310 pressure sensor. The hardware is enclosed in 3D-printed case parts (`adanet-case-*.stl`) and assembled from the BOM in `README.md`. Design goals: several-week battery life via aggressive deep-sleep; a readable e-ink display that refreshes every 180 s (the screen's minimum refresh interval).

## Sketches

Two independent Arduino sketches, each in its own folder/file at the repo root:

- `adanet-co2-monitor.ino` — the main firmware that runs on deployed devices.
- `adanet-init/adanet-init.ino` — one-time helper run over serial to pick °C/°F display units and perform SCD4x factory reset + forced recalibration against `CO2_AMBIENT` (defaults to 430 ppm; edit before calibrating if local ambient differs). Run this before flashing the main firmware for the first time.

There is no build system beyond the Arduino toolchain — these are `.ino` sketches, not a PlatformIO or CMake project.

## Build / flash

Requires Arduino IDE (1.8.19 / 2.x) with these libraries installed via Library Manager: Adafruit DPS310, Adafruit EPD, Adafruit MAX1704X (or Adafruit LC709203F — see compile-time flags below), Adafruit ThinkInk, Sensirion I2C SCD4x. The ESP32 Arduino core (v3.3.5-tested) must be installed per Espressif's arduino-esp32 instructions.

Board settings (Tools menu):
- Board: **Adafruit Feather ESP32-S3 2MB PSRAM**
- USB Mode: **Hardware CDC and JTAG**
- Upload Mode: **UART0/Hardware CDC**

To upload: hold **BOOT/DFU**, tap **Reset** to enter ROM bootloader, then Upload from the IDE. After upload, tap Reset again to start the firmware. The case must be open during flashing — the buttons are not externally accessible once the self-tapping screws are in.

## Compile-time configuration

Both sketches are gated by `#define` flags near the top of the `.ino` files; these must match the actual hardware revision:

- `USE_MAX17048` — selects Adafruit MAX17048 fuel gauge. Remove to fall back to LC709203F (older hardware).
- `USE_TRICOLOR_MFGNR` — selects the `ThinkInk_213_Tricolor_MFGNR` panel driver. Remove for the older `ThinkInk_213_Tricolor_RW` panel.
- `DEBUG` (main sketch only) — enables 115200-baud serial logging and **skips deep sleep**, so the device stays awake after a single cycle. Leave off for normal operation.

Keep these flags in sync between `adanet-co2-monitor.ino` and `adanet-init/adanet-init.ino` when working on a board — the init sketch must talk to the same fuel gauge and panel.

## Runtime architecture

The main firmware runs entirely in Arduino's `setup()`; `loop()` is unreachable in normal operation because `setup()` ends in `esp_deep_sleep_start()`. Every wake executes one full measurement+display cycle:

1. Power up I²C rail (`I2C_POWER` high), disable NeoPixel.
2. Read temperature-units preference (`'C'`/`'F'`) from NVS via `Preferences` namespace `adanet-co2`.
3. Read battery % from the fuel gauge, then hibernate it.
4. `begin_I2C()` the DPS310 and start SCD4x periodic measurement.
5. Loop `NUM_MEASUREMENTS` (=2) times: light-sleep 5 s, read pressure, push it into the SCD4x via `setAmbientPressureRaw` (hPa, uint16_t), read CO₂/temp/humidity. The first reading is intentionally discarded per Sensirion's low-power-operation app note. **Use the `Raw` variant**: in the `SensirionI2cScd4x` library (≥ v1.0.0) the non-Raw `setAmbientPressure` takes Pa as `uint32_t`, while the pre-1.0.0 `SensirionI2CScd4x` library's `setAmbientPressure` took hPa as `uint16_t` — passing hPa to the new non-Raw call silently under-compensates by 100×, distorting CO₂ readings and any FRC performed against them.
6. Stop SCD4x measurement, cut I²C power.
7. Update the e-ink only if there is no error **or** the error differs from `errorPrev` — this is a battery-life optimization, so don't remove it without understanding the tradeoff.
8. `esp_deep_sleep_start()` for `DISPLAY_WAIT − measurements × MEASUREMENT_WAIT` seconds (≈170 s).

### State that survives deep sleep

Two distinct persistence mechanisms — don't confuse them:

- **RTC memory** (`RTC_DATA_ATTR`): survives deep sleep but is lost on power cycle / reset. Used for the CO₂ history ring buffer (`co2HistoryFifo`, sized `UPDATES_PER_WEEK = 7 × 86400 / DISPLAY_WAIT`), its head index, and `errorPrev`. `co2HistoryAdd` always runs, even on error, so the time axis of the buffer stays aligned with wall-clock.
- **NVS flash** (`Preferences`): survives power cycles. Only holds the temperature-units preference, written by `adanet-init`.

The day/week CO₂ maxima shown on the display are computed on every wake by walking the ring buffer in `computeCo2Max`; there is no incremental aggregate.

### Error encoding

`error` packs an `Error` enum (`ERROR_CO2_SENSOR`, `ERROR_PRESSURE_SENSOR`, `ERROR_BATT_SENSOR`, `ERROR_LOW_BATT`) into the high 16 bits and, for SCD4x failures, the raw Sensirion error code into the low 16 bits. The screen shows the packed value as `%08X` plus a decoded message. Preserve this layout when adding new error paths.

### Display layout

Rotation 2 (USB on the left, landscape). `printfAligned` is the only drawing helper for text and handles `ALIGN_LEFT` / `ALIGN_CENTER` / `ALIGN_RIGHT` based on string length × text size × `CHAR_WIDTH` (6). CO₂ values ≥ `CO2_LIMIT` (800 ppm, CDC threshold) render in `EPD_RED`; battery < `BATT_WARN_LIMIT` (15 %) likewise. `BATT_ERROR_LIMIT` (5 %) triggers an error-screen takeover rather than a color change. `formatCo2` switches to a `%.0fK` abbreviation when either the current or max value would otherwise overflow the allotted width.
