# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.0.8] - 2026-06-18

### Added

- ESP32 D1 mini build variant (`bthidbridge-d1mini.bin`, LED on GPIO 2 — built-in
  blue LED). CI now builds and publishes both the default (GPIO 13) and D1 mini
  variants as release assets; the web installer exposes a board selector to switch
  between the two manifests.

### Changed

- Web installer title and heading updated to the full project name "ESP32 BLE HID to
  Bluetooth Classic Bridge"; firmware version is now displayed dynamically in the
  heading, fetched at page-load time from `manifest.json`.
- "Pairing a new host" note promoted to a proper `<h2>` section in the web installer.
- `docs/manifest.json` added for local development (points at local build directory;
  overwritten by CI at release time with the correct version and GitHub Pages paths).

## [0.0.7] - 2026-06-18

### Changed

- BLE connection interval relaxed from 10 ms (8 × 1.25 ms) to 15 ms (12 × 1.25 ms);
  measured latency on Linux host is unchanged.
- HID forward poll interval relaxed from 3 ms to 5 ms.
- FreeRTOS tick rate lowered from 1 kHz to 200 Hz to reduce CPU overhead.

### Fixed

- SET_REPORT handshake ACK is now retried on a 50 ms poll from `rumble_forward_task`
  when `esp_bt_hid_device_report_error()` fails (e.g. L2CAP congested), instead of
  leaving the host waiting for a 3–4 s timeout. The pending flag is cleared on BT
  disconnect to prevent a stale retry.

## [0.0.6] - 2026-06-16

### Added

- ESP-IDF patch (`patches/0001-bta-dm-disable-hid-device-sniff.patch`) that
  zeros the HD profile's allowed-modes mask in `bta_dm_cfg.c`, preventing
  Bluedroid from requesting sniff mode on every HID report sent to the host.
- `scripts/apply_patches.sh`: idempotent patch apply script, run automatically
  at CMake configure time via `CMakeLists.txt`.

### Changed

- BLE connection interval hardcoded to 10 ms (min = max = 8 × 1.25 ms);
  `BRIDGE_BLE_MAX_CONN_INTERVAL` Kconfig option removed.
- HID forward poll hardcoded to 3 ms; `BRIDGE_FORWARD_INTERVAL_MS` Kconfig
  option removed. LCM(3, 10) = 30 ms ensures the TX phase rotates through all
  BLE connection-event offsets, preventing consistent co-scheduling.
- All bridge tasks pinned to core 1; HIDH callback and `hid_forward_task`
  marked `IRAM_ATTR` to reduce ISR/scheduler jitter.
- CPU frequency raised from 160 MHz to 240 MHz.
- FreeRTOS tick rate raised from 200 Hz to 1 kHz.
- `CONFIG_FREERTOS_IN_IRAM` enabled to keep the scheduler out of flash.
- Release workflow no longer builds a separate 10 ms BLE interval variant;
  the `bthidbridge-10ms-ble.bin` artifact is removed.

### Fixed

- Disabled Bluedroid sniff-mode requests for the HID device profile, reducing
  Classic BT latency on Linux hosts from ~40–50 ms to ~20–25 ms. Mac and
  Android were unaffected (they reject the sniff request); Linux accepted it,
  incurring a ~40–50 ms Sniff→Active round-trip on every report.

## [0.0.5] - 2026-06-15

### Added

- Battery level is now reported to the Classic BT host via a HID battery
  feature report, with periodic polling of the BLE peripheral's Battery
  Service.
- Rumble/output reports from the Classic BT host are forwarded to the BLE
  peripheral (Stadia controller and any BLE HID device with output reports).
- DIP SDP record registered on the Classic BT side to expose the controller's
  VID/PID to the host.
- New LED blink pattern (short-long) while the bridge is discoverable and
  waiting for a new Classic BT host to pair.

### Changed

- Battery level is now logged only when it changes, reducing log noise during
  periodic BLE polling.
- In pairing mode the bridge now pages the cached Classic BT host up to 3
  times before falling back to discoverable mode. After exhausting attempts,
  `esp_bt_hid_device_virtual_cable_unplug()` is called to clear Bluedroid's
  internal `in_use` flag so a new host can pair successfully.
- `EXAMPLE_SSP_ENABLED` Kconfig symbol renamed to `BRIDGE_SSP_ENABLED`.

### Fixed

- `page_bonded_hosts()` now returns a bool so callers can distinguish "a page
  was initiated" from "no cached host found", preventing a race where the
  bridge could go discoverable while an outgoing page was still in flight.

## [0.0.4] - 2026-06-15

### Added

- `BRIDGE_BLE_MAX_CONN_INTERVAL` Kconfig option: BLE connection max interval in
  units of 1.25 ms (default 12 = 15 ms). Common values documented in help text.
- Release workflow builds a second firmware variant with a 10 ms BLE interval
  (`bthidbridge-10ms-ble.bin`) alongside the default build.
- Web installer now offers a variant selector between the two builds and includes
  project description, features, requirements, status LED reference, and
  first-pairing instructions.

## [0.0.3] - 2026-06-13

### Added

- Status LED blinks to indicate battery level when fully connected.

### Fixed

- Replaced `s_bt_connected` bool with `bt_state_t` enum, eliminating a HID
  negotiation race that could cause the bridge to stall after reconnect.
- Fixed ghost device-open and double-free crash on BLE reconnect.

## [0.0.2] - 2026-06-12

### Added

- Status LED support on GPIO 13 (configurable via `BRIDGE_LED_GPIO`).
- `BRIDGE_LATENCY_MEASURE` Kconfig option: logs the time between receiving a
  BLE HID report and forwarding it over Classic BT.

### Changed

- FreeRTOS tick rate raised to 200 Hz; default `BRIDGE_FORWARD_INTERVAL_MS`
  lowered to 5 ms (200 Hz) for reduced latency.
- New BLE device discovery is now performed only right after reset and before any
  successful connection

### Fixed

- `bt_reconnect_task` no longer skips the first retry gap on initial boot.
- Fixed missing `CONFIG_BRIDGE_LOG_AXES` guard around `read_axis()`, which
  caused an unused-function compiler warning.
- Fixed memory allocation in `page_bonded_hosts` (`calloc` instead of `malloc`).
- Classic BT host page is now retried every 5 s until reconnection succeeds
  instead of giving up after one attempt.

## [0.0.1] - 2026-06-10

### Added

- Initial release: bridges a BLE HID device (Stadia controller) to a Classic
  Bluetooth HID host (e.g. Tesla center console).
- Depth-1 `xQueueOverwrite` queue decouples the BLE GATT callback from the
  Classic BT forwarding path, fixing analog-stick freezes caused by blocking
  `btc_task_post` calls on the GATT thread.
- Analog axis debug traces (`BRIDGE_LOG_AXES`) with HID descriptor parsing to
  locate X/Y/Z/Rz axis fields at connection time.
- Classic BT host reconnection: reconnects to the last paired host on boot.
- NVS persistence of paired BLE device address and last Classic BT host BDA.
- Web flasher via [esp-web-tools](https://esphome.github.io/esp-web-tools/)
  hosted on GitHub Pages, with a GitHub Actions release workflow.
- MIT license.

### Changed

- Default `BRIDGE_FORWARD_INTERVAL_MS` set to 10 ms (100 Hz).
- CPU frequency set to 160 MHz.

[Unreleased]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.8...HEAD
[0.0.8]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.7...v0.0.8
[0.0.7]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.6...v0.0.7
[0.0.6]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.5...v0.0.6
[0.0.5]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.4...v0.0.5
[0.0.4]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.3...v0.0.4
[0.0.3]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.2...v0.0.3
[0.0.2]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.1...v0.0.2
[0.0.1]: https://github.com/anarsoul/esp32-bthidbridge/releases/tag/v0.0.1
