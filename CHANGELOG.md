# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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

[Unreleased]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.2...HEAD
[0.0.2]: https://github.com/anarsoul/esp32-bthidbridge/compare/v0.0.1...v0.0.2
[0.0.1]: https://github.com/anarsoul/esp32-bthidbridge/releases/tag/v0.0.1
