/**
 * @file ble-wasm.h
 * @brief Bluetooth Configuration. Services' UUID, MTU, etc.
 * @author O. Nakakaze
 */

#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#define GATTS_TAG "BLE-Module"

#define GATTS_SERVICE_UUID_WASM   0x00FF
#define GATTS_CHAR_UUID_WASM      0xFF01
#define GATTS_DESCR_UUID_WASM     0x3333
#define GATTS_NUM_HANDLE_WASM     4

#define GATTS_SERVICE_UUID_TEST_B   0x00EE
#define GATTS_CHAR_UUID_TEST_B      0xEE01
#define GATTS_DESCR_UUID_TEST_B     0x2222
#define GATTS_NUM_HANDLE_TEST_B     4

#define TEST_DEVICE_NAME            "ESP_GATTS_DEMO"
#define TEST_MANUFACTURER_DATA_LEN  17

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0x40

#define PREPARE_BUF_MAX_SIZE 1024

#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0

#define BLE_LOCAL_MTU 500

void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
