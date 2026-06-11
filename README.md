# ESP32 BT HID Bridge

Bridges a BLE HID controller (gamepad, keyboard, mouse) to a Classic Bluetooth HID device. The ESP32 connects to the BLE controller as a GATT client and re-exposes it to a Classic BT host (PC, console, Android phone), transparently forwarding all HID reports.

```
[ BLE HID controller ] <--BLE--> [ ESP32 ] <--Classic BT--> [ Host ]
```

## Features

- Automatic BLE device discovery and connection
- Caches the last connected BLE device in NVS — reconnects on boot without requiring pairing mode
- Classic BT device name mirrors the BLE controller name (e.g. `StadiaMXPB-d2ea Classic`)
- Name and BLE device address persist across reboots; updated automatically when a new controller is paired
- Requests minimum BLE connection interval (7.5 ms) for lowest latency
- HID reports forwarded via a dedicated task with configurable rate cap (default 20 ms / 50 Hz) to prevent BTA/L2CAP overflow on hosts that poll Classic BT infrequently
- Latest-report-wins queue — if the BLE controller sends faster than the forward rate, only the most recent report is forwarded, avoiding stale stick positions
- Supports any BLE HID device (identified by HID service UUID or HID appearance value)
- Optional BLE device name filter to lock onto a specific controller

## Requirements

- **Hardware**: ESP32 with dual-mode Bluetooth (WROOM, WROVER, or equivalent). Classic Bluetooth is only available on the original ESP32 — ESP32-S3, C3, C6, H2 are not supported.
- **Toolchain**: ESP-IDF v6.0.1

## Setup

### Activate ESP-IDF

```bash
source ~/.espressif/v6.0.1/esp-idf/export.sh
```

Or use the activation helper if installed:

```bash
source ~/.espressif/tools/activate_idf_v6.0.1.sh
```

### Configure (optional)

```bash
idf.py menuconfig
```

Relevant options are under **BT HID Bridge Configuration**:

| Option | Default | Description |
|--------|---------|-------------|
| `BRIDGE_PEER_DEVICE_NAME` | *(empty)* | Connect only to a BLE device with this exact advertised name. Leave empty to connect to the first HID device found. |
| `BRIDGE_BT_DEVICE_NAME` | `ESP32 HID Bridge` | Fallback Classic BT name used when the BLE device has no advertised name. |
| `BRIDGE_FORWARD_INTERVAL_MS` | `20` | Minimum interval between HID report forwards to the Classic BT host (ms). Reports arriving faster are coalesced; only the latest is sent. 20 ms = 50 Hz. |
| `BRIDGE_AUTO_RECONNECT` | enabled | Automatically restart scanning and reconnect after the BLE controller disconnects. |
| `BRIDGE_LOG_AXES` | disabled | Log analog axis values (LX/LY/RX/RY) on every changed report. Useful for debugging stuck controls. |
| `EXAMPLE_SSP_ENABLED` | enabled | Use Secure Simple Pairing for Classic BT. Disable to fall back to legacy PIN pairing. |

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash
```

Replace `/dev/ttyUSB0` with your serial port (`/dev/ttyACM0` on some systems, `COM3` on Windows).

### Monitor logs

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Press `Ctrl-]` to exit the monitor.

## Usage

### First pairing

1. Flash the firmware and power on the ESP32.
2. Put your BLE HID controller into pairing mode.
3. The ESP32 scans every 5 seconds and connects to the first BLE HID device found (or the one matching `BRIDGE_PEER_DEVICE_NAME`).
4. Once the BLE controller connects, the ESP32 becomes discoverable over Classic BT under the name `<controller name> Classic`.
5. On your host (PC / console / phone), scan for Bluetooth devices and pair with the ESP32 as you would with any HID peripheral.

### Subsequent use

After the first successful pairing, both connections are cached:

- On boot the ESP32 attempts to reconnect to the cached BLE controller for up to 30 seconds before falling back to scanning for a new one.
- The Classic BT device name is restored from NVS immediately on boot, so the host can reconnect without waiting for the BLE side to come up first.

### Replacing the controller

Put the new BLE controller into pairing mode while the ESP32 has no active BLE connection (previous controller is off or out of range). The ESP32 will discover and connect to the new device, then update the cached address and Classic BT name automatically.

## Architecture

```
BLE side (GATTC / HID host)            Classic BT side (HIDD / HID device)
──────────────────────────────          ────────────────────────────────────
scan_task                               bt_hidd_callback
  nvs_load_ble_device()                   CONNECT → make non-discoverable
  esp_hidh_dev_open()  ──────────────►    DISCONNECT → restore discoverability

ble_hidh_callback
  OPEN  → request 7.5 ms BLE interval
         start_classic_bt_hid()
  INPUT → xQueueOverwrite(report) ──►  hid_forward_task
  CLOSE → esp_hidd_dev_deinit()          sleep(FORWARD_INTERVAL_MS)
                                         xQueueReceive() → esp_hidd_dev_input_set()
```

The HIDH event callback never blocks — it writes the latest report into a depth-1 queue via `xQueueOverwrite` and returns immediately. A dedicated `hid_forward_task` wakes every `FORWARD_INTERVAL_MS`, dequeues the most recent report, and calls `esp_hidd_dev_input_set`. This prevents the Bluedroid BTC task queue from backing up when the Classic BT host polls infrequently, which would otherwise cause BLE GATT notification queue overflow and dropped reports (resulting in analog sticks freezing at their last reported position).

NVS namespace `bthid_bridge` stores:

| Key | Content |
|-----|---------|
| `ble_dev` | Last BLE device address (6 bytes) and address type (1 byte) |
| `bt_name` | Last Classic BT device name (`<BLE name> Classic`) |
