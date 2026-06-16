/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * BT HID Bridge: connects to a BLE HID controller and exposes it as a
 * Classic Bluetooth HID device, forwarding HID reports transparently.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_hidh.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_hidd_api.h"
#include "esp_gap_bt_api.h"
#include "esp_sdp_api.h"
#include "driver/gpio.h"

static const char *TAG = "BT_HID_BRIDGE";

#define SCAN_DURATION_SECONDS 5
#define MAX_REPORT_LEN        64

#define NVS_NAMESPACE    "bthid_bridge"
#define NVS_KEY_BLE_DEV  "ble_dev"
#define NVS_KEY_BT_NAME  "bt_name"
#define NVS_KEY_BT_HOST  "bt_host"
#define BT_NAME_MAX_LEN  64
#define FORWARD_INTERVAL_MS CONFIG_BRIDGE_FORWARD_INTERVAL_MS

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
} __attribute__((packed)) nvs_ble_dev_t;

static bool page_bonded_hosts(void); /* forward declaration */

typedef struct {
    int16_t  report_id;  /* -1 = not found */
    uint16_t bit_offset;
    uint8_t  bit_size;
} axis_info_t;

typedef enum {
    BT_STATE_IDLE,        /* not connected */
    BT_STATE_CONNECTING,  /* ACL up or HID negotiation in flight */
    BT_STATE_CONNECTED,   /* HID profile active */
} bt_state_t;

static esp_hidd_dev_t * volatile s_bt_hid_dev    = NULL;
static esp_hidh_dev_t * volatile s_ble_hidh_dev  = NULL;
static char               s_bt_name[BT_NAME_MAX_LEN]; /* Classic BT device name, persists across reconnects */
static volatile bt_state_t s_bt_state           = BT_STATE_IDLE;
static portMUX_TYPE       s_bt_state_mux        = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_ble_connected        = false;
static volatile bool      s_ble_connecting       = false;
static esp_gatt_if_t      s_gattc_if             = ESP_GATT_IF_NONE;
static volatile uint16_t  s_ble_conn_id          = 0xFFFF;
static volatile uint16_t  s_battery_char_handle  = 0;
static volatile uint8_t   s_battery_level        = 0xFF; /* 0xFF = unknown */
static volatile bool      s_battery_read_pending  = false;
static uint8_t            s_battery_report_id    = 0;    /* 0 = not assigned */
static volatile bool      s_controller_connected_this_boot = false;
static volatile bool      s_host_connected_this_boot       = false;
static int                s_page_attempt_count             = 0; /* pairing mode: outgoing page attempts made */
static portMUX_TYPE       s_page_count_mux                 = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_bt_discoverable                = false;
static esp_hid_raw_report_map_t *s_report_maps   = NULL;
static size_t             s_report_maps_len       = 0;
#if CONFIG_BRIDGE_LOG_AXES
static axis_info_t        s_axis_lx = { .report_id = -1 }; /* Left  stick X  (Usage 0x30) */
static axis_info_t        s_axis_ly = { .report_id = -1 }; /* Left  stick Y  (Usage 0x31) */
static axis_info_t        s_axis_rx = { .report_id = -1 }; /* Right stick Z  (Usage 0x32) */
static axis_info_t        s_axis_ry = { .report_id = -1 }; /* Right stick Rz (Usage 0x35) */
#endif

typedef struct {
    uint8_t  data[MAX_REPORT_LEN];
    uint16_t len;
    size_t   map_index;
    uint8_t  report_id;
#if CONFIG_BRIDGE_LATENCY_MEASURE
    uint64_t timestamp_us;
#endif
} hid_report_t;

static QueueHandle_t s_report_queue = NULL;
static QueueHandle_t s_rumble_queue = NULL;

static esp_err_t nvs_save_ble_device(const esp_bd_addr_t bda, esp_ble_addr_type_t addr_type)
{
    nvs_ble_dev_t dev;
    memcpy(dev.bda, bda, 6);
    dev.addr_type = (uint8_t)addr_type;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(h, NVS_KEY_BLE_DEV, &dev, sizeof(dev));
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    return ret;
}

static esp_err_t nvs_save_bt_name(const char *name)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        ret = nvs_set_str(h, NVS_KEY_BT_NAME, name);
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    return ret;
}

static esp_err_t nvs_load_bt_name(char *name, size_t max_len)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_str(h, NVS_KEY_BT_NAME, name, &max_len);
    nvs_close(h);
    return ret;
}

static esp_err_t nvs_save_bt_host(const esp_bd_addr_t bda)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(h, NVS_KEY_BT_HOST, bda, sizeof(esp_bd_addr_t));
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
    }
    return ret;
}

static esp_err_t nvs_load_bt_host(esp_bd_addr_t bda)
{
    size_t len = sizeof(esp_bd_addr_t);
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_blob(h, NVS_KEY_BT_HOST, bda, &len);
    nvs_close(h);
    return ret;
}

static esp_err_t nvs_load_ble_device(esp_bd_addr_t bda, esp_ble_addr_type_t *addr_type)
{
    nvs_ble_dev_t dev;
    size_t len = sizeof(dev);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_blob(h, NVS_KEY_BLE_DEV, &dev, &len);
    nvs_close(h);

    if (ret == ESP_OK) {
        memcpy(bda, dev.bda, 6);
        *addr_type = (esp_ble_addr_type_t)dev.addr_type;
    }
    return ret;
}

#if CONFIG_BRIDGE_LOG_AXES
/* Walk the HID descriptor and record bit-positions for X/Y/Z/Rz axes. */
static void find_axes_in_map(const uint8_t *map, size_t len)
{
    uint32_t usage_page = 0, report_size = 0, report_count = 0, report_id = 0;
    uint32_t usages[64], usage_min = 0, usage_max = 0, usage_count = 0;
    bool     use_range = false;
    uint32_t bit_offsets[32] = {0};

    size_t i = 0;
    while (i < len) {
        uint8_t b = map[i];
        if (b == 0xFE) { if (i + 1 >= len) break; i += 3 + map[i + 1]; continue; }
        uint8_t  btag  = (b >> 4) & 0x0F;
        uint8_t  btype = (b >> 2) & 0x03;
        uint32_t dlen  = (uint32_t)(b & 0x03); if (dlen == 3) dlen = 4;
        uint32_t uval  = 0;
        for (uint32_t k = 0; k < dlen && (i + 1 + k) < len; k++)
            uval |= (uint32_t)map[i + 1 + k] << (8 * k);

        switch (btype) {
        case 1:
            switch (btag) {
            case 0: usage_page   = uval; break;
            case 7: report_size  = uval; break;
            case 8: report_id    = uval; break;
            case 9: report_count = uval; break;
            }
            break;
        case 2:
            switch (btag) {
            case 0: if (usage_count < 64) usages[usage_count++] = uval; use_range = false; break;
            case 1: usage_min = uval; use_range = true;  break;
            case 2: usage_max = uval;                    break;
            }
            break;
        case 0:
            if (btag == 8 && report_id < 32) {
                uint32_t bo = bit_offsets[report_id];
                for (uint32_t f = 0; f < report_count; f++) {
                    uint32_t usage = use_range
                        ? (f <= usage_max - usage_min ? usage_min + f : 0)
                        : (f < usage_count ? usages[f] : 0);
                    if (usage_page == 0x01) {
                        axis_info_t info = { .report_id = (int16_t)report_id,
                                             .bit_offset = (uint16_t)bo,
                                             .bit_size   = (uint8_t)report_size };
                        if (usage == 0x30) { s_axis_lx = info; ESP_LOGI(TAG, "Axis LX: report %u bit %u size %u", report_id, bo, report_size); }
                        if (usage == 0x31) { s_axis_ly = info; ESP_LOGI(TAG, "Axis LY: report %u bit %u size %u", report_id, bo, report_size); }
                        if (usage == 0x32) { s_axis_rx = info; ESP_LOGI(TAG, "Axis RX: report %u bit %u size %u", report_id, bo, report_size); }
                        if (usage == 0x35) { s_axis_ry = info; ESP_LOGI(TAG, "Axis RY: report %u bit %u size %u", report_id, bo, report_size); }
                    }
                    bo += report_size;
                }
                bit_offsets[report_id] = bo;
            }
            usage_count = 0; use_range = false;
            memset(usages, 0, sizeof(usages));
            break;
        }
        i += 1 + dlen;
    }
}

static int32_t read_axis(const uint8_t *data, uint16_t data_len, const axis_info_t *ax)
{
    uint32_t val = 0;
    for (uint8_t b = 0; b < ax->bit_size; b++) {
        uint16_t byte_idx = (ax->bit_offset + b) / 8;
        uint8_t  bit_idx  = (ax->bit_offset + b) % 8;
        if (byte_idx < data_len && ((data[byte_idx] >> bit_idx) & 1))
            val |= (1u << b);
    }
    return (int32_t)val;
}

static void log_axes_if_changed(uint8_t report_id, const uint8_t *data, uint16_t len)
{
    static uint8_t  s_prev[MAX_REPORT_LEN];
    static uint16_t s_prev_len = 0;

    if (len == s_prev_len && memcmp(data, s_prev, len) == 0) return;
    memcpy(s_prev, data, len);
    s_prev_len = len;

    if (s_axis_lx.report_id != report_id && s_axis_rx.report_id != report_id) return;

    int32_t lx = (s_axis_lx.report_id == report_id) ? read_axis(data, len, &s_axis_lx) : -1;
    int32_t ly = (s_axis_ly.report_id == report_id) ? read_axis(data, len, &s_axis_ly) : -1;
    int32_t rx = (s_axis_rx.report_id == report_id) ? read_axis(data, len, &s_axis_rx) : -1;
    int32_t ry = (s_axis_ry.report_id == report_id) ? read_axis(data, len, &s_axis_ry) : -1;
    ESP_LOGI(TAG, "Axes id=%u LX=%d LY=%d RX=%d RY=%d", report_id, lx, ly, rx, ry);
}
#endif

static void free_report_maps(void)
{
    if (!s_report_maps) {
        return;
    }
    for (size_t i = 0; i < s_report_maps_len; i++) {
        free((void *)s_report_maps[i].data);
    }
    free(s_report_maps);
    s_report_maps = NULL;
    s_report_maps_len = 0;
}

/* Walk all report maps with proper short-item parsing, return first unused report ID. */
static uint8_t pick_battery_report_id(void)
{
    bool used[256] = {false};
    used[0] = true; /* 0 = reserved (no report ID) */

    for (size_t m = 0; m < s_report_maps_len; m++) {
        const uint8_t *d = s_report_maps[m].data;
        size_t len = s_report_maps[m].len;
        size_t i = 0;
        while (i < len) {
            uint8_t b = d[i];
            if (b == 0xFE) { /* long item */
                if (i + 1 >= len) break;
                i += 3 + d[i + 1];
                continue;
            }
            uint32_t dlen = (uint32_t)(b & 0x03);
            if (dlen == 3) dlen = 4;
            uint8_t btype = (b >> 2) & 0x03;
            uint8_t btag  = (b >> 4) & 0x0F;
            if (btype == 1 && btag == 8 && dlen >= 1 && i + 1 < len) {
                /* Global item: Report ID */
                used[d[i + 1]] = true;
            }
            i += 1 + dlen;
        }
    }

    for (int id = 1; id <= 255; id++) {
        if (!used[id]) return (uint8_t)id;
    }
    return 0;
}

/* Append a Battery Strength Feature Report descriptor fragment to the last report map. */
static void append_battery_feature_report(uint8_t report_id)
{
    if (s_report_maps_len == 0) return;

    uint8_t desc[] = {
        0x05, 0x06,              /* Usage Page (Generic Device Controls) */
        0x09, 0x20,              /* Usage (Battery Strength)             */
        0xa1, 0x01,              /* Collection (Application)             */
        0x85, report_id,         /*   Report ID                          */
        0x09, 0x20,              /*   Usage (Battery Strength)           */
        0x15, 0x00,              /*   Logical Minimum (0)                */
        0x26, 0x64, 0x00,        /*   Logical Maximum (100)              */
        0x75, 0x08,              /*   Report Size (8)                    */
        0x95, 0x01,              /*   Report Count (1)                   */
        0xb1, 0x02,              /*   Feature (Data,Var,Abs)             */
        0xc0,                    /* End Collection                       */
    };

    size_t idx = s_report_maps_len - 1;
    size_t old_len = s_report_maps[idx].len;
    size_t new_len = old_len + sizeof(desc);
    uint8_t *buf = realloc((void *)s_report_maps[idx].data, new_len);
    if (!buf) {
        ESP_LOGE(TAG, "OOM appending battery descriptor");
        return;
    }
    memcpy(buf + old_len, desc, sizeof(desc));
    s_report_maps[idx].data = buf;
    s_report_maps[idx].len  = (uint16_t)new_len;
    ESP_LOGI(TAG, "Battery feature report appended (id=0x%02x) to map[%u]", report_id, idx);
}

static void set_bt_discoverable(bool discoverable)
{
    s_bt_discoverable = discoverable;
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                             discoverable ? ESP_BT_GENERAL_DISCOVERABLE
                                          : ESP_BT_NON_DISCOVERABLE);
}

static void bt_hidd_callback(void *handler_args, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        if (param->start.status == ESP_OK) {
            if (!s_host_connected_this_boot) {
                /* Pairing mode: page the cached host.
                 * Do NOT go discoverable yet — Bluedroid HIDD rejects incoming
                 * L2CAP connections from a different device while an outgoing
                 * connect is in flight.  Go discoverable only after all attempts
                 * are exhausted or if there is no cached host at all. */
                taskENTER_CRITICAL(&s_page_count_mux);
                int attempt = ++s_page_attempt_count;
                taskEXIT_CRITICAL(&s_page_count_mux);
                ESP_LOGI(TAG, "Classic BT HID started — paging cached host (attempt %d/3)", attempt);
                if (!page_bonded_hosts()) {
                    ESP_LOGI(TAG, "No cached host — making discoverable");
                    set_bt_discoverable(true);
                }
            } else {
                ESP_LOGI(TAG, "Classic BT HID started — not discoverable (host already seen this boot)");
                page_bonded_hosts();
            }
        } else {
            ESP_LOGE(TAG, "Classic BT HID start failed: %d", param->start.status);
        }
        break;
    case ESP_HIDD_CONNECT_EVENT:
        if (param->connect.status == ESP_OK) {
            ESP_LOGI(TAG, "Classic BT host connected");
            set_bt_discoverable(false);
            taskENTER_CRITICAL(&s_bt_state_mux);
            s_bt_state = BT_STATE_CONNECTED;
            taskEXIT_CRITICAL(&s_bt_state_mux);
            s_host_connected_this_boot = true;
        } else {
            ESP_LOGE(TAG, "Classic BT connect failed: %d", param->connect.status);
            taskENTER_CRITICAL(&s_bt_state_mux);
            s_bt_state = BT_STATE_IDLE;
            taskEXIT_CRITICAL(&s_bt_state_mux);
            if (!s_host_connected_this_boot) {
                if (s_page_attempt_count >= 3) {
                    ESP_LOGI(TAG, "Cached host not responding after %d attempts — going discoverable",
                             s_page_attempt_count);
                    /* Each page attempt calls HID_DevPlugDevice() internally, setting
                     * hd_cb.device.in_use=TRUE and device.addr=old_host.  While in_use
                     * is set, hidd_l2cif_connect_ind rejects incoming L2CAP connections
                     * from any different BDA ("incoming connections from different device").
                     * esp_bt_hid_device_virtual_cable_unplug() when not connected calls
                     * HID_DevUnplugDevice(), clearing in_use so new hosts can pair. */
                    esp_bt_hid_device_virtual_cable_unplug();
                    set_bt_discoverable(true);
                }
            }
        }
        break;
    case ESP_HIDD_DISCONNECT_EVENT: {
        taskENTER_CRITICAL(&s_bt_state_mux);
        bool was_connecting = (s_bt_state == BT_STATE_CONNECTING);
        s_bt_state = BT_STATE_IDLE;
        taskEXIT_CRITICAL(&s_bt_state_mux);
        ESP_LOGI(TAG, "Classic BT host disconnected");
        if (!s_host_connected_this_boot) {
            if (was_connecting) {
                /* Attempt count was incremented when paging was initiated;
                 * if attempts are exhausted ensure discoverable and in_use is cleared (idempotent). */
                if (s_page_attempt_count >= 3) {
                    esp_bt_hid_device_virtual_cable_unplug();
                    set_bt_discoverable(true);
                }
            } else {
                /* Unexpected disconnect in pairing mode — go discoverable */
                set_bt_discoverable(true);
            }
        } else if (was_connecting) {
            /* HID negotiation failed — bt_reconnect_task will retry */
            ESP_LOGI(TAG, "HID negotiation failed — will retry");
        } else if (s_ble_connected) {
            /* Host dropped while BLE is still up — reconnect to host */
            ESP_LOGI(TAG, "Host dropped, BLE still up — paging cached host");
            page_bonded_hosts();
        } else {
            /* BLE dropped first and triggered this disconnect — wait for BLE to
             * reconnect; start_classic_bt_hid() will page the host then */
            ESP_LOGI(TAG, "Host dropped due to BLE loss — will reconnect when BLE returns");
        }
        break;
    }
    case ESP_HIDD_OUTPUT_EVENT: {
        esp_hidh_dev_t *ble_dev = s_ble_hidh_dev;
        if (ble_dev && esp_hidh_dev_exists(ble_dev)) {
            esp_hidh_dev_output_set(ble_dev,
                                    param->output.map_index,
                                    param->output.report_id,
                                    param->output.data,
                                    param->output.length);
        }
        break;
    }
    case ESP_HIDD_FEATURE_EVENT:
        /* bt_hidd.c never acknowledges SET_REPORT on the success path, so the host
         * waits 3-4 s for a handshake timeout before retrying. Send it here. */
        if (param->feature.trans_type == ESP_HID_TRANS_SET_REPORT) {
            esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS);
        }
        if (param->feature.trans_type == ESP_HID_TRANS_GET_REPORT
                && s_battery_report_id != 0
                && param->feature.report_id == s_battery_report_id) {
            uint8_t level = (s_battery_level == 0xFF) ? 100 : s_battery_level;
            esp_hidd_dev_feature_set(s_bt_hid_dev,
                                     param->feature.map_index,
                                     s_battery_report_id,
                                     &level, 1);
        }
        if (param->feature.report_type == ESP_HID_REPORT_TYPE_OUTPUT) {
            uint16_t len = param->feature.length < MAX_REPORT_LEN
                           ? param->feature.length : MAX_REPORT_LEN;
            hid_report_t rumble;
            rumble.report_id = param->feature.report_id;
            rumble.map_index = param->feature.map_index;
            rumble.len       = len;
            memcpy(rumble.data, param->feature.data, len);
            xQueueOverwrite(s_rumble_queue, &rumble);
        }
        break;
    case ESP_HIDD_STOP_EVENT:
        ESP_LOGI(TAG, "Classic BT HID stopped");
        break;
    default:
        break;
    }
}

static bool page_bonded_hosts(void)
{
    /* Atomically claim the CONNECTING slot; bail if not idle. */
    taskENTER_CRITICAL(&s_bt_state_mux);
    bool idle = (s_bt_state == BT_STATE_IDLE);
    if (idle) s_bt_state = BT_STATE_CONNECTING;
    taskEXIT_CRITICAL(&s_bt_state_mux);
    if (!idle) return false;

    esp_bd_addr_t host_bda;
    if (nvs_load_bt_host(host_bda) == ESP_OK) {
        ESP_LOGI(TAG, "Paging last host " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(host_bda));
        esp_bt_hid_device_connect(host_bda);
        return true;
    }
    /* No cached host yet — fall back to first bonded device */
    int bond_num = esp_bt_gap_get_bond_device_num();
    if (bond_num <= 0) {
        taskENTER_CRITICAL(&s_bt_state_mux);
        s_bt_state = BT_STATE_IDLE;
        taskEXIT_CRITICAL(&s_bt_state_mux);
        return false;
    }
    esp_bd_addr_t *bond_list = calloc(bond_num, sizeof(esp_bd_addr_t));
    if (!bond_list) {
        ESP_LOGE(TAG, "OOM listing bonded devices");
        taskENTER_CRITICAL(&s_bt_state_mux);
        s_bt_state = BT_STATE_IDLE;
        taskEXIT_CRITICAL(&s_bt_state_mux);
        return false;
    }
    bool paged = false;
    if (esp_bt_gap_get_bond_device_list(&bond_num, bond_list) == ESP_OK) {
        ESP_LOGI(TAG, "Paging first bonded host " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bond_list[0]));
        esp_bt_hid_device_connect(bond_list[0]);
        paged = true;
    } else {
        taskENTER_CRITICAL(&s_bt_state_mux);
        s_bt_state = BT_STATE_IDLE;
        taskEXIT_CRITICAL(&s_bt_state_mux);
    }
    free(bond_list);
    return paged;
}

static void on_bt_acl_connect(const esp_bd_addr_t bda)
{
    ESP_LOGI(TAG, "Classic BT ACL connected: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
    nvs_save_bt_host(bda);
    taskENTER_CRITICAL(&s_bt_state_mux);
    s_bt_state = BT_STATE_CONNECTING;
    taskEXIT_CRITICAL(&s_bt_state_mux);
}

static void start_classic_bt_hid(esp_hidh_dev_t *ble_dev)
{
    if (s_bt_hid_dev != NULL) {
        /* Classic BT HID already running — page the host to reconnect */
        ESP_LOGI(TAG, "Classic BT HID already initialized, paging host");
        bool should_page = s_host_connected_this_boot;
        if (!should_page) {
            taskENTER_CRITICAL(&s_page_count_mux);
            if (s_page_attempt_count < 3) {
                s_page_attempt_count++;
                should_page = true;
            }
            taskEXIT_CRITICAL(&s_page_count_mux);
        }
        if (should_page) {
            page_bonded_hosts();
        }
        return;
    }

    size_t num_maps = 0;
    esp_hid_raw_report_map_t *maps = NULL;

    if (esp_hidh_dev_report_maps_get(ble_dev, &num_maps, &maps) != ESP_OK || num_maps == 0) {
        ESP_LOGE(TAG, "Failed to read BLE HID report maps");
        return;
    }

    free_report_maps();

    s_report_maps = calloc(num_maps, sizeof(esp_hid_raw_report_map_t));
    if (!s_report_maps) {
        ESP_LOGE(TAG, "OOM allocating report map array");
        return;
    }
    for (size_t i = 0; i < num_maps; i++) {
        uint8_t *buf = malloc(maps[i].len);
        if (!buf) {
            ESP_LOGE(TAG, "OOM allocating report map data[%u]", i);
            free_report_maps();
            return;
        }
        memcpy(buf, maps[i].data, maps[i].len);
        s_report_maps[i].data = buf;
        s_report_maps[i].len  = maps[i].len;
        ESP_LOGI(TAG, "Copied report map[%u]: %u bytes", i, maps[i].len);
    }
    s_report_maps_len = num_maps;

#if CONFIG_BRIDGE_LOG_AXES
    s_axis_lx.report_id = s_axis_ly.report_id = -1;
    s_axis_rx.report_id = s_axis_ry.report_id = -1;
    for (size_t i = 0; i < s_report_maps_len; i++)
        find_axes_in_map(s_report_maps[i].data, s_report_maps[i].len);
#endif

    s_battery_report_id = pick_battery_report_id();
    if (s_battery_report_id != 0) {
        append_battery_feature_report(s_battery_report_id);
    } else {
        ESP_LOGW(TAG, "No free report ID for battery feature report");
    }

    uint16_t vid = esp_hidh_dev_vendor_id_get(ble_dev);
    uint16_t pid = esp_hidh_dev_product_id_get(ble_dev);
    uint16_t ver = esp_hidh_dev_version_get(ble_dev);
    ESP_LOGI(TAG, "BLE device: VID=0x%04x PID=0x%04x VER=0x%04x", vid, pid, ver);

    /* Register a DIP SDP record so Classic BT hosts can discover VID/PID.
     * bt_hidd.c stores these values but never creates the required DIP record. */
    if (vid != 0 || pid != 0) {
        esp_bluetooth_sdp_record_t dip = {0};
        dip.dip.hdr.type                  = ESP_SDP_TYPE_DIP_SERVER;
        dip.dip.hdr.rfcomm_channel_number = -1;
        dip.dip.hdr.l2cap_psm             = -1;
        dip.dip.vendor                    = vid;
        dip.dip.vendor_id_source          = ESP_SDP_VENDOR_ID_SRC_USB;
        dip.dip.product                   = pid;
        dip.dip.version                   = ver;
        dip.dip.primary_record            = true;
        esp_sdp_create_record(&dip);
    }

    const char *ble_name = esp_hidh_dev_name_get(ble_dev);
    if (ble_name && ble_name[0] != '\0') {
        snprintf(s_bt_name, sizeof(s_bt_name), "%s Classic", ble_name);
        nvs_save_bt_name(s_bt_name);
    } else if (s_bt_name[0] == '\0') {
        snprintf(s_bt_name, sizeof(s_bt_name), "%s", CONFIG_BRIDGE_BT_DEVICE_NAME);
    }
    ESP_LOGI(TAG, "Classic BT name: \"%s\"", s_bt_name);

    static esp_hid_device_config_t bt_hid_config;
    bt_hid_config.vendor_id         = vid;
    bt_hid_config.product_id        = pid;
    bt_hid_config.version           = ver;
    bt_hid_config.device_name       = s_bt_name;
    bt_hid_config.manufacturer_name = "ESP32";
    bt_hid_config.serial_number     = "0000001";
    bt_hid_config.report_maps       = s_report_maps;
    bt_hid_config.report_maps_len   = s_report_maps_len;

    esp_bt_gap_set_device_name(s_bt_name);

    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_GAMEPAD;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_hidd_dev_t *hid_dev = NULL;
    esp_err_t ret = esp_hidd_dev_init(&bt_hid_config, ESP_HID_TRANSPORT_BT,
                                       bt_hidd_callback, &hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %d", ret);
        free_report_maps();
    } else {
        s_bt_hid_dev = hid_dev;
    }
}

static void ble_hidh_callback(void *handler_args, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, "BLE HID opened: " ESP_BD_ADDR_STR " \"%s\"",
                     ESP_BD_ADDR_HEX(bda),
                     esp_hidh_dev_name_get(param->open.dev));
            esp_hidh_dev_dump(param->open.dev, stdout);

            /* Default Bluedroid GATTC interval can be 50-100ms; request a tighter range. */
            esp_ble_conn_update_params_t conn_params = {
                .min_int = 6,   /* 7.5ms */
                .max_int = CONFIG_BRIDGE_BLE_MAX_CONN_INTERVAL,
                .latency = 0,
                .timeout = 400, /* 4s supervision timeout */
            };
            memcpy(conn_params.bda, bda, sizeof(esp_bd_addr_t));
            esp_ble_gap_update_conn_params(&conn_params);

            s_ble_hidh_dev = param->open.dev;
            s_ble_connected = true;   /* set before clearing connecting — no race with scan_task */
            s_ble_connecting = false;
            s_controller_connected_this_boot = true;

            /* Read initial battery level from Battery Service (0x180F / 0x2A19) */
            if (s_gattc_if != ESP_GATT_IF_NONE && s_ble_conn_id != 0xFFFF) {
                esp_bt_uuid_t svc_uuid  = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = 0x180F };
                esp_bt_uuid_t char_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = 0x2A19 };
                esp_gattc_service_elem_t svc = {0};
                esp_gattc_char_elem_t    chr = {0};
                uint16_t count = 1;
                if (esp_ble_gattc_get_service(s_gattc_if, s_ble_conn_id, &svc_uuid,
                                              &svc, &count, 0) == ESP_GATT_OK && count > 0) {
                    count = 1;
                    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_ble_conn_id,
                                                       svc.start_handle, svc.end_handle,
                                                       char_uuid, &chr, &count) == ESP_GATT_OK
                            && count > 0) {
                        s_battery_char_handle = chr.char_handle;
                        s_battery_read_pending = true;
                        if (esp_ble_gattc_read_char(s_gattc_if, s_ble_conn_id,
                                                    chr.char_handle,
                                                    ESP_GATT_AUTH_REQ_NONE) != ESP_GATT_OK) {
                            s_battery_read_pending = false;
                        }
                    }
                }
            }

            start_classic_bt_hid(param->open.dev);
        } else {
            ESP_LOGE(TAG, "BLE HID open failed");
            s_ble_connecting = false;
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVENT:
        if (param->battery.status == ESP_OK) {
            if (param->battery.level != s_battery_level)
                ESP_LOGI(TAG, "Battery: %u%%", param->battery.level);
            s_battery_level = param->battery.level;
        }
        break;

    case ESP_HIDH_INPUT_EVENT: {
        if (s_bt_state != BT_STATE_CONNECTED || !s_bt_hid_dev) {
            break;
        }
        uint16_t len = param->input.length > MAX_REPORT_LEN
                       ? MAX_REPORT_LEN : param->input.length;
#if CONFIG_BRIDGE_LOG_AXES
        log_axes_if_changed(param->input.report_id, param->input.data, len);
#endif
        hid_report_t report;
        report.len       = len;
        report.map_index = param->input.map_index;
        report.report_id = param->input.report_id;
        memcpy(report.data, param->input.data, len);
#if CONFIG_BRIDGE_LATENCY_MEASURE
        report.timestamp_us = esp_timer_get_time();
#endif
        xQueueOverwrite(s_report_queue, &report);
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        if (!param->close.dev) {
            ESP_LOGW(TAG, "BLE HID close with NULL dev");
            s_ble_hidh_dev        = NULL;
            s_ble_connected       = false;
            s_ble_connecting      = false;
            s_ble_conn_id         = 0xFFFF;
            s_battery_char_handle = 0;
            s_battery_read_pending = false;
            s_battery_level       = 0xFF;
            break;
        }
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        ESP_LOGI(TAG, "BLE HID closed: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
        s_ble_hidh_dev        = NULL;
        s_ble_connected       = false;
        s_ble_connecting      = false;
        s_ble_conn_id         = 0xFFFF;
        s_battery_char_handle = 0;
        s_battery_read_pending = false;
        s_battery_level       = 0xFF;
        if (s_bt_state == BT_STATE_CONNECTED) {
            ESP_LOGI(TAG, "BLE lost — disconnecting Classic BT host");
            esp_bt_hid_device_disconnect();
        }
        break;
    }

    default:
        break;
    }
}


#if CONFIG_BRIDGE_LATENCY_MEASURE
#define LATENCY_LOG_WINDOW 100
static void latency_record(uint32_t us)
{
    static uint32_t s_lat_min   = UINT32_MAX;
    static uint32_t s_lat_max   = 0;
    static uint64_t s_lat_sum   = 0;
    static uint32_t s_lat_count = 0;

    if (us < s_lat_min) s_lat_min = us;
    if (us > s_lat_max) s_lat_max = us;
    s_lat_sum += us;
    s_lat_count++;

    if (s_lat_count >= LATENCY_LOG_WINDOW) {
        ESP_LOGI(TAG, "Fwd latency us/%u: min=%u max=%u avg=%u",
                 LATENCY_LOG_WINDOW, s_lat_min, s_lat_max,
                 (uint32_t)(s_lat_sum / s_lat_count));
        s_lat_min   = UINT32_MAX;
        s_lat_max   = 0;
        s_lat_sum   = 0;
        s_lat_count = 0;
    }
}
#endif

static void hid_forward_task(void *pvParameters)
{
    hid_report_t report;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(FORWARD_INTERVAL_MS));
        if (xQueueReceive(s_report_queue, &report, 0) != pdTRUE) continue;
        if (s_bt_state != BT_STATE_CONNECTED || !s_bt_hid_dev) continue;
#if CONFIG_BRIDGE_LATENCY_MEASURE
        latency_record((uint32_t)(esp_timer_get_time() - report.timestamp_us));
#endif
        esp_err_t err = esp_hidd_dev_input_set(s_bt_hid_dev, report.map_index,
                                               report.report_id, report.data, report.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "input_set failed: id=%u err=0x%x", report.report_id, err);
        }
    }
}

static void rumble_forward_task(void *pvParameters)
{
    hid_report_t rumble;
    while (1) {
        if (xQueueReceive(s_rumble_queue, &rumble, portMAX_DELAY) != pdTRUE) continue;
        esp_hidh_dev_t *ble_dev = s_ble_hidh_dev;
        if (!ble_dev || !esp_hidh_dev_exists(ble_dev)) continue;
        esp_err_t err = esp_hidh_dev_output_set(ble_dev, rumble.map_index,
                                                 rumble.report_id, rumble.data, rumble.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rumble forward failed: id=%u err=0x%x", rumble.report_id, err);
        }
    }
}

static void scan_task(void *pvParameters)
{
    size_t results_len = 0;
    esp_hid_scan_result_t *results = NULL;
    const char *peer_name = CONFIG_BRIDGE_PEER_DEVICE_NAME;

    while (1) {
        if (s_ble_connected || s_ble_connecting) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Try to reconnect to the last known BLE device before scanning */
        esp_bd_addr_t cached_bda;
        esp_ble_addr_type_t cached_addr_type = BLE_ADDR_TYPE_PUBLIC;
        if (nvs_load_ble_device(cached_bda, &cached_addr_type) == ESP_OK) {
            ESP_LOGI(TAG, "Trying cached BLE device " ESP_BD_ADDR_STR " (up to 30s)",
                     ESP_BD_ADDR_HEX(cached_bda));
            s_ble_connecting = true;
            esp_hidh_dev_open(cached_bda, ESP_HID_TRANSPORT_BLE, cached_addr_type);

            int timeout = 30;
            while (timeout-- > 0 && s_ble_connecting && !s_ble_connected) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            if (s_ble_connected) {
                continue;
            }
            s_ble_connecting = false;
            if (s_controller_connected_this_boot) {
                ESP_LOGI(TAG, "Cached device unavailable — not scanning (controller already seen this boot)");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            ESP_LOGI(TAG, "Cached device unavailable, scanning for new device");
        }

        /* Scan for a new BLE HID device */
        ESP_LOGI(TAG, "Scanning for BLE HID devices (%ds)...", SCAN_DURATION_SECONDS);
        if (esp_hid_ble_scan(SCAN_DURATION_SECONDS, &results_len, &results) != ESP_OK) {
            ESP_LOGE(TAG, "BLE scan failed");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "Scan found %u result(s)", results_len);

        if (results_len > 0) {
            esp_hid_scan_result_t *r = results;
            esp_hid_scan_result_t *target = NULL;

            while (r) {
                ESP_LOGI(TAG, "  " ESP_BD_ADDR_STR " \"%s\" usage:%s rssi:%d",
                         ESP_BD_ADDR_HEX(r->bda),
                         r->name ? r->name : "",
                         esp_hid_usage_str(r->usage),
                         r->rssi);
                if (strlen(peer_name) > 0) {
                    if (r->name && strcmp(r->name, peer_name) == 0) {
                        target = r;
                        break;
                    }
                } else {
                    target = r;
                }
                r = r->next;
            }

            if (target) {
                ESP_LOGI(TAG, "Connecting to " ESP_BD_ADDR_STR " \"%s\"",
                         ESP_BD_ADDR_HEX(target->bda),
                         target->name ? target->name : "");
                nvs_save_ble_device(target->bda, target->ble.addr_type);
                s_ble_connecting = true;
                esp_hidh_dev_open(target->bda, ESP_HID_TRANSPORT_BLE,
                                  target->ble.addr_type);
            } else if (strlen(peer_name) > 0) {
                ESP_LOGI(TAG, "Target \"%s\" not found in scan results", peer_name);
            }

            esp_hid_scan_results_free(results);
            results = NULL;
            results_len = 0;
        }

        if (!s_ble_connecting && !s_ble_connected) {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

static void bt_reconnect_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_bt_state != BT_STATE_IDLE || !s_ble_connected) {
            continue;
        }
        if (s_host_connected_this_boot) {
            /* Reconnect mode: host was seen this boot, retry unconditionally */
            ESP_LOGI(TAG, "Classic BT host not connected — retrying page");
            page_bonded_hosts();
        } else {
            taskENTER_CRITICAL(&s_page_count_mux);
            int attempt = s_page_attempt_count < 3 ? ++s_page_attempt_count : 0;
            taskEXIT_CRITICAL(&s_page_count_mux);
            if (attempt > 0) {
                ESP_LOGI(TAG, "Classic BT host not connected — paging cached host (attempt %d/3)", attempt);
                if (!page_bonded_hosts()) {
                    ESP_LOGI(TAG, "No cached host — making discoverable");
                    set_bt_discoverable(true);
                }
            }
        }
        /* else: 3 attempts exhausted, already discoverable — nothing to do */
    }
}

#if CONFIG_BRIDGE_LED_ENABLE
static void led_task(void *pvParameters)
{
    bool level = false;
    while (1) {
        if (s_ble_connected && s_bt_state == BT_STATE_CONNECTED) {
            uint8_t batt = s_battery_level;
            /* 0xFF = unknown; treat same as >75% until first read arrives */
            int blinks = (batt <= 25) ? 1 : (batt <= 50) ? 2 : (batt <= 75) ? 3 : 0;
            if (blinks == 0) {
                gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                for (int i = 0; i < blinks; i++) {
                    gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 0);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 1);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                /* Stay on for the rest of the 5s cycle, checking state every 100 ms */
                int remaining_ms = 5000 - blinks * 400;
                while (remaining_ms > 0
                       && s_ble_connected
                       && s_bt_state == BT_STATE_CONNECTED) {
                    int chunk = remaining_ms < 100 ? remaining_ms : 100;
                    vTaskDelay(pdMS_TO_TICKS(chunk));
                    remaining_ms -= chunk;
                }
            }
        } else if (s_ble_connected && s_bt_discoverable) {
            /* Short-long blink: discoverable, waiting for a new BT host to pair */
            gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
            gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else if (s_ble_connected) {
            level = !level;
            gpio_set_level(CONFIG_BRIDGE_LED_GPIO, level);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            level = !level;
            gpio_set_level(CONFIG_BRIDGE_LED_GPIO, level);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#endif

static void battery_poll_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        /* Snapshot volatile GATT parameters before any guard checks so a
         * concurrent CLOSE_EVENT cannot zero them between our checks and the
         * actual call, which would leave s_battery_read_pending stuck true. */
        esp_gatt_if_t gattc_if   = s_gattc_if;
        uint16_t      conn_id    = s_ble_conn_id;
        uint16_t      char_handle = s_battery_char_handle;
        if (!s_ble_connected || char_handle == 0 || s_battery_read_pending) continue;
        if (gattc_if == ESP_GATT_IF_NONE || conn_id == 0xFFFF) continue;
        s_battery_read_pending = true;
        if (esp_ble_gattc_read_char(gattc_if, conn_id,
                                    char_handle, ESP_GATT_AUTH_REQ_NONE) != ESP_GATT_OK) {
            s_battery_read_pending = false;
        }
    }
}

static void bridge_gattc_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
    /* Update our state before delegating — esp_hidh_gattc_event_handler may
     * post ESP_HIDH_OPEN_EVENT to the HIDH task queue synchronously, and the
     * HIDH task can run before we return, so s_gattc_if / s_ble_conn_id must
     * be current before the call. */
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK)
            s_gattc_if = gattc_if;
        break;
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status == ESP_GATT_OK)
            s_ble_conn_id = param->open.conn_id;
        break;
    default:
        break;
    }

    /* Intercept our own battery read response before HIDH sees it. */
    if (event == ESP_GATTC_READ_CHAR_EVT
            && s_battery_read_pending
            && s_battery_char_handle != 0
            && param->read.handle == s_battery_char_handle) {
        if (param->read.status == ESP_GATT_OK && param->read.value_len >= 1) {
            if (param->read.value[0] != s_battery_level)
                ESP_LOGI(TAG, "Battery: %u%%", param->read.value[0]);
            s_battery_level = param->read.value[0];
        }
        s_battery_read_pending = false;
        return;
    }

    esp_hidh_gattc_event_handler(event, gattc_if, param);
}

static void sdp_callback(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    if (event == ESP_SDP_CREATE_RECORD_COMP_EVT) {
        ESP_LOGI(TAG, "DIP SDP record created (status=%d)", param->create_record.status);
    }
}

void app_main(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    /* Suppress noisy BT power-management log tags.
     * BT_HCI / BT_APPL emit W/E when the peer rejects sniff-mode (HCI 0x20 = LMP PDU Not Allowed),
     * which is normal for hosts that don't negotiate sniff. */
    esp_log_level_set("BT_HCI",  ESP_LOG_ERROR);
    esp_log_level_set("BT_APPL", ESP_LOG_NONE);

    ESP_LOGI(TAG, "Initializing BT stack (BTDM)");
    ESP_ERROR_CHECK(esp_hid_gap_init(HID_BRIDGE_MODE));
    esp_hid_gap_set_bt_acl_conn_cb(on_bt_acl_connect);

    /* Restore the BT Classic device name immediately so it is correct even
     * before BLE connects (e.g. during the 30-second cached-device window). */
    if (nvs_load_bt_name(s_bt_name, sizeof(s_bt_name)) == ESP_OK) {
        esp_bt_gap_set_device_name(s_bt_name);
        ESP_LOGI(TAG, "Restored Classic BT name: \"%s\"", s_bt_name);
    }

    /* BLE security parameters for headless host role (no IO, Just Works) */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1));

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(bridge_gattc_event_handler));

    esp_hidh_config_t hidh_config = {
        .callback         = ble_hidh_callback,
        .event_stack_size = 4096,
        .callback_arg     = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_config));

    esp_sdp_register_callback(sdp_callback);
    ESP_ERROR_CHECK(esp_sdp_init());

    s_report_queue = xQueueCreate(1, sizeof(hid_report_t));
    s_rumble_queue = xQueueCreate(1, sizeof(hid_report_t));
    xTaskCreate(hid_forward_task,    "hid_fwd",   4096, NULL, configMAX_PRIORITIES - 2, NULL);
    xTaskCreate(rumble_forward_task, "rumble_fwd", 2048, NULL, configMAX_PRIORITIES - 2, NULL);
    xTaskCreate(scan_task,           "scan",       6144, NULL, configMAX_PRIORITIES - 3, NULL);
    xTaskCreate(bt_reconnect_task,   "bt_recon",   2048, NULL, configMAX_PRIORITIES - 3, NULL);
    xTaskCreate(battery_poll_task,   "batt_poll",  2048, NULL, configMAX_PRIORITIES - 3, NULL);

#if CONFIG_BRIDGE_LED_ENABLE
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(CONFIG_BRIDGE_LED_GPIO, 0);
    xTaskCreate(led_task, "led", 1024, NULL, configMAX_PRIORITIES - 4, NULL);
#endif

    ESP_LOGI(TAG, "Bridge ready. Peer filter: \"%s\"",
             strlen(CONFIG_BRIDGE_PEER_DEVICE_NAME) > 0
                 ? CONFIG_BRIDGE_PEER_DEVICE_NAME : "(first found)");
}
