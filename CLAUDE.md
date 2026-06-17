# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Toolchain

ESP-IDF v6.0.1. Activate before any `idf.py` command:

```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
```

Target is **ESP32 only** (original, dual-mode). ESP32-S3/C3/C6/H2 lack Classic Bluetooth and are unsupported.

## Common commands

```bash
idf.py build                        # build
idf.py flash                        # flash (auto-detects port)
idf.py -p /dev/ttyUSB0 flash monitor  # flash + open serial monitor (Ctrl-] to exit)
idf.py menuconfig                   # interactive config (BT HID Bridge Configuration section)
idf.py reconfigure                  # re-run CMake — needed after Kconfig changes
idf.py erase-flash && idf.py flash  # wipe all flash (including NVS) and reflash
```

After changing `Kconfig.projbuild`, run `idf.py reconfigure` before `idf.py build` to pick up new `CONFIG_*` symbols.

After changing `sdkconfig.defaults`, also update `sdkconfig` manually or via `idf.py reconfigure` — the two must stay in sync.

## Architecture

All application logic lives in `main/bridge_main.c`. `main/esp_hid_gap.c` / `.h` are lightly modified ESP-IDF example helpers for GAP/scan.

### Data flow

Input (controller → host):
```
BLE controller ──GATT notify──► ble_hidh_callback (ESP_HIDH_INPUT_EVENT)
                                     │ xQueueOverwrite  (non-blocking, depth-1)
                                     ▼
                                s_report_queue
                                     │ xQueueReceive every FORWARD_INTERVAL_MS
                                     ▼
                                hid_forward_task
                                     │ esp_hidd_dev_input_set()
                                     ▼
                                Classic BT HID host
```

Output (host → controller — rumble/haptics):
```
Classic BT host ──SET_REPORT──► bt_hidd_callback (ESP_HIDD_FEATURE_EVENT / OUTPUT_EVENT)
                                     │ xQueueOverwrite (depth-1) or direct call
                                     ▼
                                s_rumble_queue  →  rumble_forward_task
                                                        │ esp_hidh_dev_output_set()
                                                        ▼
                                                   BLE controller
```

Battery:
```
battery_poll_task (every 10 s) ──GATT read──► BLE Battery Service (0x180F / 0x2A19)
                                                   │ bridge_gattc_event_handler intercepts result
                                                   ▼
                                              s_battery_level  ──► bt_hidd_callback (GET_REPORT)
                                                                        │ esp_hidd_dev_feature_set()
                                                                        ▼
                                                                   Classic BT host
```

### Why the queue exists

`esp_hidd_dev_input_set` calls `btc_task_post(portMAX_DELAY)` internally — it blocks until the Bluedroid BTC task queue has space. Calling it directly from the HIDH event task blocks BLE GATT notification processing, causing notification queue overflow and dropped reports (analog sticks freeze). The depth-1 `xQueueOverwrite` queue keeps the HIDH event task non-blocking; `hid_forward_task` absorbs the blocking call at a capped rate.

### Key Kconfig options (`main/Kconfig.projbuild`)

| Option | Default | Notes |
|--------|---------|-------|
| `BRIDGE_SSP_ENABLED` | y | Secure Simple Pairing for Classic BT. Disable to fall back to legacy PIN. |
| `BRIDGE_PEER_DEVICE_NAME` | *(empty)* | Lock onto a specific BLE device by name. |
| `BRIDGE_BT_DEVICE_NAME` | `ESP32 HID Bridge` | Fallback Classic BT name when BLE device has no name. |
| `BRIDGE_AUTO_RECONNECT` | y | Re-scan after BLE disconnect. |
| `BRIDGE_LOG_AXES` | n | Enable to log LX/LY/RX/RY on every changed report — useful for debugging stuck sticks. |
| `BRIDGE_LATENCY_MEASURE` | n | Log HID forwarding latency (min/max/avg over 100 reports). |
| `BRIDGE_LED_ENABLE` | y | Enable the status LED. |
| `BRIDGE_LED_GPIO` | 13 | GPIO pin for the status LED. |

### NVS persistence (`bthid_bridge` namespace)

| Key | Content |
|-----|---------|
| `ble_dev` | BLE device address + address type (7 bytes packed) |
| `bt_name` | Classic BT name (`<BLE name> Classic`) |
| `bt_host` | Last connected Classic BT host BDA |

### HID descriptor parsing

`find_axes_in_map()` walks the raw HID descriptor at connection time to locate X/Y/Z/Rz axis fields (usages 0x30/0x31/0x32/0x35, usage page 0x01). Results are stored in `s_axis_lx/ly/rx/ry` (`axis_info_t`: report_id, bit_offset, bit_size). Used only when `BRIDGE_LOG_AXES` is enabled.
