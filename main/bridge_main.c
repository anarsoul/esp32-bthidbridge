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

static const char *TAG = "BT_HID_BRIDGE";

#define SCAN_DURATION_SECONDS 5
#define MAX_REPORT_LEN        64

#define NVS_NAMESPACE   "bthid_bridge"
#define NVS_KEY_BLE_DEV "ble_dev"
#define NVS_KEY_BT_NAME "bt_name"
#define BT_NAME_MAX_LEN 64

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
} __attribute__((packed)) nvs_ble_dev_t;

static esp_hidd_dev_t    *s_bt_hid_dev          = NULL;
static char               s_bt_name[BT_NAME_MAX_LEN]; /* Classic BT device name, persists across reconnects */
static volatile bool      s_bt_connected         = false;
static volatile bool      s_ble_connected        = false;
static volatile bool      s_ble_connecting       = false;
static esp_hid_raw_report_map_t *s_report_maps   = NULL;
static size_t             s_report_maps_len       = 0;

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

static void bt_hidd_callback(void *handler_args, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        if (param->start.status == ESP_OK) {
            ESP_LOGI(TAG, "Classic BT HID started — making discoverable");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        } else {
            ESP_LOGE(TAG, "Classic BT HID start failed: %d", param->start.status);
        }
        break;
    case ESP_HIDD_CONNECT_EVENT:
        if (param->connect.status == ESP_OK) {
            ESP_LOGI(TAG, "Classic BT host connected");
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            s_bt_connected = true;
        } else {
            ESP_LOGE(TAG, "Classic BT connect failed: %d", param->connect.status);
        }
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "Classic BT host disconnected");
        s_bt_connected = false;
        if (s_ble_connected) {
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    case ESP_HIDD_STOP_EVENT:
        ESP_LOGI(TAG, "Classic BT HID stopped");
        break;
    default:
        break;
    }
}

static void start_classic_bt_hid(esp_hidh_dev_t *ble_dev)
{
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

    uint16_t vid = esp_hidh_dev_vendor_id_get(ble_dev);
    uint16_t pid = esp_hidh_dev_product_id_get(ble_dev);
    uint16_t ver = esp_hidh_dev_version_get(ble_dev);
    ESP_LOGI(TAG, "BLE device: VID=0x%04x PID=0x%04x VER=0x%04x", vid, pid, ver);

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

    esp_err_t ret = esp_hidd_dev_init(&bt_hid_config, ESP_HID_TRANSPORT_BT,
                                       bt_hidd_callback, &s_bt_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %d", ret);
        free_report_maps();
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

            /* Request minimum connection interval for lowest latency.
             * Default Bluedroid GATTC interval can be 50-100ms; 7.5ms is the BLE minimum. */
            esp_ble_conn_update_params_t conn_params = {
                .min_int = 6,   /* 7.5ms */
                .max_int = 12,  /* 15ms  */
                .latency = 0,
                .timeout = 400, /* 4s supervision timeout */
            };
            memcpy(conn_params.bda, bda, sizeof(esp_bd_addr_t));
            esp_ble_gap_update_conn_params(&conn_params);

            s_ble_connected = true;   /* set before clearing connecting — no race with scan_task */
            s_ble_connecting = false;
            start_classic_bt_hid(param->open.dev);
        } else {
            ESP_LOGE(TAG, "BLE HID open failed");
            s_ble_connecting = false;
        }
        break;
    }

    case ESP_HIDH_INPUT_EVENT: {
        if (!s_bt_connected || !s_bt_hid_dev) {
            break;
        }
        uint16_t len = param->input.length > MAX_REPORT_LEN
                       ? MAX_REPORT_LEN : param->input.length;
        esp_hidd_dev_input_set(s_bt_hid_dev, param->input.map_index,
                               param->input.report_id, param->input.data, len);
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        ESP_LOGI(TAG, "BLE HID closed: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
        esp_hidh_dev_free(param->close.dev);

        s_ble_connected  = false;
        s_ble_connecting = false;
        s_bt_connected   = false;

        if (s_bt_hid_dev) {
            esp_hidd_dev_deinit(s_bt_hid_dev);
            s_bt_hid_dev = NULL;
        }
        free_report_maps();
        break;
    }

    default:
        break;
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

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_hidh_config_t hidh_config = {
        .callback         = ble_hidh_callback,
        .event_stack_size = 4096,
        .callback_arg     = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_config));

    xTaskCreate(scan_task, "scan", 6144, NULL, configMAX_PRIORITIES - 3, NULL);

    ESP_LOGI(TAG, "Bridge ready. Peer filter: \"%s\"",
             strlen(CONFIG_BRIDGE_PEER_DEVICE_NAME) > 0
                 ? CONFIG_BRIDGE_PEER_DEVICE_NAME : "(first found)");
}
