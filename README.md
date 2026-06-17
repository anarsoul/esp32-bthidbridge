# ESP32 BT HID Bridge

Bridges a BLE HID gamepad to a Classic Bluetooth HID host. The ESP32 connects to the BLE controller as a GATT client and re-exposes it to a Classic BT host (PC, console, car head unit), transparently forwarding HID reports.

> **Note:** only gamepads have been tested. Keyboards and mice are unlikely to work correctly — the bridge uses a depth-1 latest-report-wins queue, which means reports can be overwritten before forwarding. This is fine for analog stick state but will silently drop key press/release events and mouse movement deltas.

```
[ BLE HID controller ] <--BLE--> [ ESP32 ] <--Classic BT--> [ Host ]
```

## Features

- Automatic BLE device discovery and connection
- Caches the last connected BLE device in NVS — reconnects on boot without requiring pairing mode
- Classic BT device name mirrors the BLE controller name (e.g. `StadiaMXPB-d2ea Classic`)
- Name and BLE device address persist across reboots; updated automatically when a new controller is paired
- Forces BLE connection interval to 10 ms (min = max = 8 × 1.25 ms) — short enough for responsive input, long enough to reduce radio slot contention with Classic BT
- HID reports forwarded by a 3 ms periodic poll — decouples BT Classic TX timing from BLE RX events to prevent Bluedroid BTC task queue overflow
- Latest-report-wins depth-1 queue — if the BLE controller sends faster than the Classic BT host can consume, only the most recent report is forwarded, avoiding stale stick positions
- Output reports (rumble/haptics) forwarded from the Classic BT host back to the BLE controller
- Battery level read from the BLE controller and exposed to the Classic BT host as a HID feature report (Battery Strength, usage 0x06/0x20)
- Supports any BLE HID gamepad (identified by HID service UUID or HID appearance value)
- Optional BLE device name filter to lock onto a specific controller
- Status LED (connect between GPIO13 and GND): fast blink = waiting for BLE controller; slow blink = BLE connected, paging cached host; short-long blink = BLE connected, discoverable (waiting for new host to pair); steady = both connected; when fully connected, blinks every 5 s to show controller battery level: 1 blink = 0–25%, 2 blinks = 26–50%, 3 blinks = 51–75%, steady = above 75%

## Requirements

- **Hardware**: ESP32 with dual-mode Bluetooth (WROOM, WROVER, or equivalent). Classic Bluetooth is only available on the original ESP32. Note: ESP32-S3, C3, C6, H2 are not supported.
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
| `BRIDGE_AUTO_RECONNECT` | enabled | Automatically restart scanning and reconnect after the BLE controller disconnects. |
| `BRIDGE_LOG_AXES` | disabled | Log analog axis values (LX/LY/RX/RY) on every changed report. Useful for debugging stuck controls. |
| `BRIDGE_LATENCY_MEASURE` | disabled | Log HID forwarding latency (min/max/avg over 100 reports) between BLE report receipt and Classic BT forwarding. |
| `BRIDGE_LED_ENABLE` | enabled | Enable the status LED. |
| `BRIDGE_LED_GPIO` | `13` | GPIO pin for the status LED. Connect an LED with a series resistor between this pin and GND. |
| `BRIDGE_SSP_ENABLED` | enabled | Use Secure Simple Pairing for Classic BT. Disable to fall back to legacy PIN pairing. |

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

### Web flasher

The easiest way to flash the ESP32 is via the [web installer](https://anarsoul.github.io/esp32-bthidbridge/) in Chrome — no toolchain required.

Before flashing, put the ESP32 into boot mode: press and hold the **BOOT** button, press and release **RESET**, then release **BOOT**.

### First pairing

1. Flash the firmware and power on the ESP32.
2. Put your BLE HID controller into pairing mode.
3. The ESP32 scans every 5 seconds and connects to the first BLE HID device found (or the one matching `BRIDGE_PEER_DEVICE_NAME`).
4. Once the BLE controller connects, the ESP32 becomes discoverable over Classic BT under the name `<controller name> Classic`.
5. On your host (PC / console / phone), scan for Bluetooth devices and pair with the ESP32 as you would with any HID peripheral.

### Subsequent use

After the first successful pairing, both connections are cached:

- On boot the ESP32 attempts to reconnect to the cached BLE controller for up to 30 seconds before falling back to scanning for a new one. Once a controller has connected during a boot session, the ESP32 will not scan for a different device — it retries the cached address only.
- The Classic BT device name is restored from NVS immediately on boot, so the host can reconnect without waiting for the BLE side to come up first.
- Once a Classic BT host has connected during a boot session, the ESP32 becomes non-discoverable and will only page the known host on subsequent disconnects — it will not accept connections from new hosts.

### Status LED

Connect an LED with a series resistor between GPIO13 and GND. The LED indicates the current connection state:

| Pattern | Meaning |
|---------|---------|
| Fast blink (100 ms) | Waiting for BLE controller |
| Slow blink (500 ms) | BLE controller connected, paging cached Classic BT host |
| Short-long blink (100 ms / 500 ms) | BLE controller connected, discoverable — waiting for Classic BT host to pair |
| Steady on | Both connected, controller battery above 75% |
| 3 blinks every 5 s | Both connected, controller battery 51–75% |
| 2 blinks every 5 s | Both connected, controller battery 26–50% |
| 1 blink every 5 s | Both connected, controller battery 0–25% |

The GPIO pin and enable/disable can be changed via `BRIDGE_LED_GPIO` and `BRIDGE_LED_ENABLE` in `idf.py menuconfig`.

### Replacing the controller

The old controller must be offline (powered off or out of range) before pairing a new one. Reset the ESP32 — on boot it will attempt the cached address for up to 30 seconds, fail, then scan for and connect to the new controller. The cached address and Classic BT name are updated automatically.

### Pairing a new Classic BT host

The ESP32 is discoverable only during a boot session in which no Classic BT host has yet connected. To pair a new host:

1. Ensure the old host is off or out of range, **or** make it forget the bridge (remove from its Bluetooth device list).
2. Reset the ESP32. It pages the cached host up to 3 times; if the host does not respond it automatically becomes discoverable. Making the host forget the bridge triggers an immediate refusal, so discoverable mode is reached faster.
3. On the new host, scan for Bluetooth devices and pair with the ESP32 as you would with any HID peripheral.

## Latency

Measured with a Stadia controller using [controllertest.io](https://controllertest.io/latency-test):

| Setup | Average latency |
|-------|----------------|
| Stadia controller → Mac (direct BLE) | ~10 ms |
| Stadia controller → Bridge → Mac (Classic BT) | ~25 ms |


The Classic BT host may negotiate sniff mode on the connection, which adds latency in multiples of the sniff interval. This is outside the bridge's control and depends on the host.

Note: the controller may negotiate a higher BLE interval than requested if its firmware requires it.

### Measuring forwarding latency

Enable `BRIDGE_LATENCY_MEASURE` in `idf.py menuconfig` to log the time between receiving a BLE report and the `esp_hidd_dev_input_set` call. Every 100 forwarded reports the serial monitor prints:

```
Fwd latency us/100: min=NNN max=NNN avg=NNN
```

Useful to verify there are no Bluedroid BTC task stalls, which show up as elevated `max` values.

## Known Issues

### Bogus analog stick position on host connect

Occasionally, right after the Classic BT host connects, the host reports the analog sticks as not centered. The state corrects itself as soon as any control is used.

**Workaround:** move the analog sticks briefly after connecting to the host.

## Architecture

```
BLE side (GATTC / HID host)            Classic BT side (HIDD / HID device)
──────────────────────────────          ────────────────────────────────────
scan_task                               bt_hidd_callback
  nvs_load_ble_device()                   CONNECT → make non-discoverable
  esp_hidh_dev_open()  ──────────────►    DISCONNECT → restore discoverability

ble_hidh_callback
  OPEN  → request 10 ms BLE interval
         start_classic_bt_hid()
  INPUT → xQueueOverwrite(report) ──►  hid_forward_task
                                         sleep(3 ms)
  CLOSE → esp_bt_hid_device_disconnect() xQueueReceive(0) → esp_hidd_dev_input_set()

bt_hidd_callback
  OUTPUT → esp_hidh_dev_output_set()  ◄── Classic BT host (direct output reports)
  FEATURE(output) → xQueueOverwrite(rumble) ◄── rumble_forward_task
                                                   → esp_hidh_dev_output_set()
```

The HIDH event callback never blocks — it writes the latest report into a depth-1 queue via `xQueueOverwrite` and returns immediately. A dedicated `hid_forward_task` drains the queue and calls `esp_hidd_dev_input_set`. This prevents the Bluedroid BTC task queue from backing up when the Classic BT host polls infrequently, which would otherwise cause BLE GATT notification queue overflow and dropped reports (resulting in analog sticks freezing at their last reported position).

**Why poll-based, not event-driven.** An event-driven design (`xQueueReceive` with `portMAX_DELAY`) forwards each report the instant it arrives, phase-locking BT Classic TX to BLE RX. Because both share Bluedroid's BTC task queue, back-to-back RX+TX operations saturate it and drop reports — the same freeze symptom. A fixed poll interval ensures the TX phase drifts relative to the BLE connection events so they are never consistently co-scheduled.

**Why 3 ms poll interval.** The BLE connection interval is 10 ms. With a 3 ms poll, LCM(3, 10) = 30 ms, so the TX phase rotates through all ten possible 1 ms offsets within 30 ms — no two consecutive polls land at the same phase relative to a BLE connection event. 3 ms also bounds worst-case forwarding latency to 3 ms (average 1.5 ms), which is imperceptible.

**Why 10 ms BLE interval.** A 7.5 ms interval (6 × 1.25 ms) increases BLE radio slot frequency, raising the probability that a BLE receive and a Classic BT transmit compete for the same radio window, causing BTC queue stalls and elevated `max` forwarding latency. 10 ms reduces that contention while keeping input latency acceptable.

NVS namespace `bthid_bridge` stores:

| Key | Content |
|-----|---------|
| `ble_dev` | Last BLE device address (6 bytes) and address type (1 byte) |
| `bt_name` | Last Classic BT device name (`<BLE name> Classic`) |
| `bt_host` | Last connected Classic BT host BDA (6 bytes) |
