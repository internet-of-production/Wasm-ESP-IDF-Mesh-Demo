//ESP-WIFI-MESH-DOCUMENTATION https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp-wifi-mesh.html

// Include FreeRTOS for delay
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "mesh_msg_codes.h"

//BLE
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "ble-wasm.h"

//wasm3
/*#include "wasm3.h"
#include "m3_env.h"
#include "wasm3_defs.h"*/


#define RX_SIZE          (1500) //MTU of incoming packets. MESH_MTU is 1500 bytes
#define TX_SIZE          (1460) //MTU of outgoing packets. 
#define CONFIG_MESH_ROUTE_TABLE_SIZE 10 //There is a limit??
#define CONFIG_MESH_AP_CONNECTIONS 5 //MAX 10
#define CONFIG_MESH_CHANNEL 0 /* channel (must match the router's channel) */
#define CONFIG_MESH_AP_PASSWD "wasiwasm"
#define MESH_NODE_NAME "data_processor"
#define MESH_DATA_STREAM_TABLE_LEN 2 // there is two receiver

//Node with the same MESH_ID can communicates each other.
static const uint8_t MESH_ID[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
//Broadcast group
static const mesh_addr_t broadcast_group_id = {.addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

//base MAC address must be unicast MAC (least significant bit of first byte must be zero)
uint8_t new_mac[6] = {0x00,0x00,0x00,0x00,0xFF,0xFF};

static bool is_mesh_connected = false;
static bool is_running = true;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_netif_t *netif_sta = NULL;
static uint8_t tx_buf[TX_SIZE] = { 0, }; //Buffer for outgoing
static uint8_t rx_buf[RX_SIZE] = { 0, }; //Buffer for incoming
static mesh_addr_t data_stream_table[MESH_DATA_STREAM_TABLE_LEN]; //TODO: every node must have a routing table of data.
uint8_t num_of_destination = 0;
uint8_t current_total_nodes = 0;
uint8_t snapshot_total_nodes = 0;
uint32_t data_stream_table_trans_id;
uint8_t current_root_mac[6];
uint8_t default_dest_mac[6] = {0xEC,0x94,0xCB,0x6F,0xBD,0xB0};//root

uint8_t ble_array[BLE_ROUTE_TABLE_MTU]; //For table size, name size, and wasmflag. 
uint8_t ds_ble_array[BLE_LOCAL_MTU]; //For data stream diagram
uint8_t ds_ble_array_offset = 1;
uint8_t num_received_ds_table = 0;
bool this_node_is_Wasm_target = true;
mesh_addr_t wasm_target;
mesh_addr_t mesh_target;

static const char *MESH_TAG = "mesh_main";
static const char *TAG = "wasm";

#define HAS_WASM_MODULE
#define WASM_STACK_SLOTS    4000
#define CALC_INPUT  2
#define FATAL(func, msg) { ESP_LOGE(TAG, "Fatal: " func " "); ESP_LOGE(TAG, "%s", msg);}
#define BASE_PATH "/spiffs"


/************************************************************************
 * Management of data stream table in the mesh network
 ************************************************************************/

void set_default_destination(){
    num_of_destination = 1;
    for(int j=0; j<6;j++){
        data_stream_table[0].addr[j] = default_dest_mac[j];
    }    
}

/******************
 * BLE
 ******************/

///Declare the static function
static void gatts_profile_wasm_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static void gatts_profile_mesh_graph_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

static uint8_t char1_str[] = {0x11,0x22,0x33};
static esp_gatt_char_prop_t wasm_property = 0;
static esp_gatt_char_prop_t mesh_graph_property = 0;
int wasm_packet_number = 0;
int wasm_current_transmit_offset = 0;

static esp_attr_value_t gatts_demo_char1_val =
{
    .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX,
    .attr_len     = sizeof(char1_str),
    .attr_value   = char1_str,
};

static uint8_t adv_config_done = 0;
#define adv_config_flag      (1 << 0)
#define scan_rsp_config_flag (1 << 1)

#ifdef CONFIG_SET_RAW_ADV_DATA
static uint8_t raw_adv_data[] = {
        0x02, 0x01, 0x06,
        0x02, 0x0a, 0xeb, 0x03, 0x03, 0xab, 0xcd
};
static uint8_t raw_scan_rsp_data[] = {
        0x0f, 0x09, 0x45, 0x53, 0x50, 0x5f, 0x47, 0x41, 0x54, 0x54, 0x53, 0x5f, 0x44,
        0x45, 0x4d, 0x4f
};
#else

static uint8_t adv_service_uuid128[32] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xEE, 0x00, 0x00, 0x00,
    //second uuid, 32bit, [12], [13], [14], [15] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

// The length of adv data must be less than 31 bytes
//static uint8_t test_manufacturer[TEST_MANUFACTURER_DATA_LEN] =  {0x12, 0x23, 0x45, 0x56};
//adv data
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006, //slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval = 0x0010, //slave connection max interval, Time = max_interval * 1.25 msec
    .appearance = 0x00,
    .manufacturer_len = 0, //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data =  NULL, //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
// scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    //.min_interval = 0x0006,
    //.max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0, //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data =  NULL, //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

#endif /* CONFIG_SET_RAW_ADV_DATA */

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};


struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_WASM_APP_ID] = {
        .gatts_cb = gatts_profile_wasm_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
    [PROFILE_MESH_GRAPH_APP_ID] = {
        .gatts_cb = gatts_profile_mesh_graph_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    }
};

typedef struct {
    uint8_t                 *prepare_buf;
    int                     prepare_len;
} prepare_type_env_t;

static prepare_type_env_t a_prepare_write_env;
static prepare_type_env_t b_prepare_write_env;

void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);
void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);


void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
#ifdef CONFIG_SET_RAW_ADV_DATA
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done==0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done==0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#else
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0){
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#endif
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        //advertising start complete event to indicate advertising start successfully or failed
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TAG, "Advertising start failed\n");
        } else {
            ESP_LOGI(GATTS_TAG, "Start adv successfully\n");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTS_TAG, "Advertising stop failed\n");
        } else {
            ESP_LOGI(GATTS_TAG, "Stop adv successfully\n");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(GATTS_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}


//This function responses for a write requiest if a client requires a response (need_rsp).
void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    esp_gatt_status_t status = ESP_GATT_OK;
    if (param->write.need_rsp){
        if (param->write.is_prep){
            if (prepare_write_env->prepare_buf == NULL) {
                prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE*sizeof(uint8_t));
                prepare_write_env->prepare_len = 0;
                if (prepare_write_env->prepare_buf == NULL) {
                    ESP_LOGE(GATTS_TAG, "Gatt_server prep no mem\n");
                    status = ESP_GATT_NO_RESOURCES;
                }
            } else {
                if(param->write.offset > PREPARE_BUF_MAX_SIZE) {
                    status = ESP_GATT_INVALID_OFFSET;
                } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
                    status = ESP_GATT_INVALID_ATTR_LEN;
                }
            }

            esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
            esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);
            if (response_err != ESP_OK){
               ESP_LOGE(GATTS_TAG, "Send response error\n");
            }
            free(gatt_rsp);
            if (status != ESP_GATT_OK){
                return;
            }
            memcpy(prepare_write_env->prepare_buf + param->write.offset,
                   param->write.value,
                   param->write.len);
            prepare_write_env->prepare_len += param->write.len;

        }else{
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, NULL);
        }
    }
}

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC){
        esp_log_buffer_hex(GATTS_TAG, prepare_write_env->prepare_buf, prepare_write_env->prepare_len);
    }else{
        ESP_LOGI(GATTS_TAG,"ESP_GATT_PREP_WRITE_CANCEL");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

static void gatts_profile_wasm_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {

    esp_err_t err;
    mesh_data_t data;
    data.data = tx_buf;
    data.size = sizeof(tx_buf);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;

    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(GATTS_TAG, "REGISTER_APP_EVT, status %d, app_id %d\n", param->reg.status, param->reg.app_id);
        gl_profile_tab[PROFILE_WASM_APP_ID].service_id.is_primary = true;
        gl_profile_tab[PROFILE_WASM_APP_ID].service_id.id.inst_id = 0x00;
        gl_profile_tab[PROFILE_WASM_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_WASM_APP_ID].service_id.id.uuid.uuid.uuid16 = GATTS_SERVICE_UUID_WASM;

        esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(TEST_DEVICE_NAME);
        if (set_dev_name_ret){
            ESP_LOGE(GATTS_TAG, "set device name failed, error code = %x", set_dev_name_ret);
        }
#ifdef CONFIG_SET_RAW_ADV_DATA
        esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
        if (raw_adv_ret){
            ESP_LOGE(GATTS_TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
        }
        adv_config_done |= adv_config_flag;
        esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
        if (raw_scan_ret){
            ESP_LOGE(GATTS_TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
        }
        adv_config_done |= scan_rsp_config_flag;
#else
        //config adv data
        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret){
            ESP_LOGE(GATTS_TAG, "config adv data failed, error code = %x", ret);
        }
        adv_config_done |= adv_config_flag;
        //config scan response data
        ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
        if (ret){
            ESP_LOGE(GATTS_TAG, "config scan response data failed, error code = %x", ret);
        }
        adv_config_done |= scan_rsp_config_flag;

#endif
        esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_WASM_APP_ID].service_id, GATTS_NUM_HANDLE_WASM);
        break;
    case ESP_GATTS_READ_EVT: {
        ESP_LOGI(GATTS_TAG, "GATT_READ_EVT, conn_id %d, trans_id %d, handle %d\n", param->read.conn_id, param->read.trans_id, param->read.handle);
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = 2;
        rsp.attr_value.value[0] = BLE_LOCAL_MTU>>8;
        rsp.attr_value.value[1] = (uint8_t)BLE_LOCAL_MTU;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                    ESP_GATT_OK, &rsp);
        break;
    }
    case ESP_GATTS_WRITE_EVT: {
        ESP_LOGI(GATTS_TAG, "GATT_WRITE_EVT, conn_id %d, trans_id %d, handle %d", param->write.conn_id, param->write.trans_id, param->write.handle);
        if (!param->write.is_prep){
            ESP_LOGI(GATTS_TAG, "GATT_WRITE_EVT, value len %d, value :", param->write.len);
            esp_log_buffer_hex(GATTS_TAG, param->write.value, param->write.len);
            if (gl_profile_tab[PROFILE_WASM_APP_ID].descr_handle == param->write.handle && param->write.len == 2){
                uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
                if (descr_value == 0x0001){
                    if (wasm_property & ESP_GATT_CHAR_PROP_BIT_NOTIFY){
                        ESP_LOGI(GATTS_TAG, "notify enable");
                        uint8_t notify_data[15];
                        for (int i = 0; i < sizeof(notify_data); ++i)
                        {
                            notify_data[i] = i%0xff;
                        }
                        //the size of notify_data[] need less than MTU size
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, gl_profile_tab[PROFILE_WASM_APP_ID].char_handle,
                                                sizeof(notify_data), notify_data, false);
                    }
                }else if (descr_value == 0x0002){
                    if (wasm_property & ESP_GATT_CHAR_PROP_BIT_INDICATE){
                        ESP_LOGI(GATTS_TAG, "indicate enable");
                        uint8_t indicate_data[15];
                        for (int i = 0; i < sizeof(indicate_data); ++i)
                        {
                            indicate_data[i] = i%0xff;
                        }
                        //the size of indicate_data[] need less than MTU size
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, gl_profile_tab[PROFILE_WASM_APP_ID].char_handle,
                                                sizeof(indicate_data), indicate_data, true);
                    }
                }
                else if (descr_value == 0x0000){
                    ESP_LOGI(GATTS_TAG, "notify/indicate disable ");
                }else{
                    ESP_LOGE(GATTS_TAG, "unknown descr value");
                    esp_log_buffer_hex(GATTS_TAG, param->write.value, param->write.len);
                }

            }
            else {
                if(param->write.value[0] == 0x01){
                    wasm_packet_number = (param->write.value[1]) << 8 | param->write.value[2];

                    uint8_t thisMAC[6] = {0};
                    ESP_ERROR_CHECK(esp_read_mac(thisMAC, ESP_MAC_WIFI_STA));
                    for(int i=0;i<6;i++){
                        wasm_target.addr[i] = param->write.value[4+i];
                    }

                    //Check if the destination of new Wasm is this node
                    this_node_is_Wasm_target = true;
                    for(int j=0; j<6; j++){
                        if(wasm_target.addr[j] != thisMAC[j]){
                            this_node_is_Wasm_target = false;
                            //TODO: send initial message to clear Wasm file at the receiver!!
                            tx_buf[0] = SEND_WASM_INIT;
                            tx_buf[1] = param->write.value[1];
                            tx_buf[2] = param->write.value[2];
                            tx_buf[3] = param->write.value[3];
                            err = esp_mesh_send(&wasm_target, &data, MESH_DATA_P2P, NULL, 0);

                            if (err) {
                                ESP_LOGE(MESH_TAG, "Error occured at sending message: SEND_WASM_INIT: err code %x", err);
                            }

                            break;
                        }
                    }
                    if(this_node_is_Wasm_target){
                        int result = remove("/spiffs/main.wasm");
                        if(result){
                            ESP_LOGE(GATTS_TAG, "Failed to remove wasm file ");
                        }
                    }
                }
                else if(param->write.value[0] == 0x02){
                    if(!wasm_packet_number){
                        ESP_LOGE(GATTS_TAG, "Missing the initial packet for uploading wasm");
                        return;
                    }
                    if(this_node_is_Wasm_target){
                        wasm_current_transmit_offset = (param->write.value[1]) << 8 | param->write.value[2];
                        FILE* wasmFile = fopen("/spiffs/main.wasm", "ab");//Open with append mode
                        if (wasmFile == NULL) {
                            ESP_LOGE(GATTS_TAG, "Failed to open file for reading");
                            return;
                        }
                        size_t written_length = fwrite(param->write.value+3, 1, param->write.len-3, wasmFile);
                        fclose(wasmFile);
                        if(!written_length){
                            ESP_LOGE(GATTS_TAG, "Failed to write wasm binary");
                            return;
                        }
                        if(wasm_current_transmit_offset+1 == wasm_packet_number){
                            esp_restart();
                        }
                    }
                    else{
                        //SEND Wasm packets to the target node via MESH
                        tx_buf[0] = SEND_WASM;
                        tx_buf[1] = param->write.len;
                        for(int i=0; i<param->write.len; i++){
                            tx_buf[i+2] = param->write.value[i];
                        }
                        err = esp_mesh_send(&wasm_target, &data, MESH_DATA_P2P, NULL, 0);

                        if (err) {
                            ESP_LOGE(MESH_TAG, "Error occured at sending message: SEND_WASM: err code %x", err);
                        }
                    }
                }
                else {
                    ESP_LOGI(GATTS_TAG, "Unknown package flag for updating Wasm");
                }
            }
        }
        example_write_event_env(gatts_if, &a_prepare_write_env, param);
        break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGI(GATTS_TAG,"ESP_GATTS_EXEC_WRITE_EVT");
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        example_exec_write_event_env(&a_prepare_write_env, param);
        break;
    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
        break;
    case ESP_GATTS_UNREG_EVT:
        break;
    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(GATTS_TAG, "CREATE_SERVICE_EVT, status %d,  service_handle %d\n", param->create.status, param->create.service_handle);
        gl_profile_tab[PROFILE_WASM_APP_ID].service_handle = param->create.service_handle;
        gl_profile_tab[PROFILE_WASM_APP_ID].char_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_WASM_APP_ID].char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_WASM;

        esp_ble_gatts_start_service(gl_profile_tab[PROFILE_WASM_APP_ID].service_handle);
        wasm_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        esp_err_t add_char_ret = esp_ble_gatts_add_char(gl_profile_tab[PROFILE_WASM_APP_ID].service_handle, &gl_profile_tab[PROFILE_WASM_APP_ID].char_uuid,
                                                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                        wasm_property,
                                                        &gatts_demo_char1_val, NULL);
        if (add_char_ret){
            ESP_LOGE(GATTS_TAG, "add char failed, error code =%x",add_char_ret);
        }
        break;
    case ESP_GATTS_ADD_INCL_SRVC_EVT:
        break;
    case ESP_GATTS_ADD_CHAR_EVT: {
        uint16_t length = 0;
        const uint8_t *prf_char;

        ESP_LOGI(GATTS_TAG, "ADD_CHAR_EVT, status %d,  attr_handle %d, service_handle %d\n",
                param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);
        gl_profile_tab[PROFILE_WASM_APP_ID].char_handle = param->add_char.attr_handle;
        gl_profile_tab[PROFILE_WASM_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_WASM_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_err_t get_attr_ret = esp_ble_gatts_get_attr_value(param->add_char.attr_handle,  &length, &prf_char);
        if (get_attr_ret == ESP_FAIL){
            ESP_LOGE(GATTS_TAG, "ILLEGAL HANDLE");
        }

        ESP_LOGI(GATTS_TAG, "the gatts demo char length = %x\n", length);
        for(int i = 0; i < length; i++){
            ESP_LOGI(GATTS_TAG, "prf_char[%x] =%x\n",i,prf_char[i]);
        }
        esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(gl_profile_tab[PROFILE_WASM_APP_ID].service_handle, &gl_profile_tab[PROFILE_WASM_APP_ID].descr_uuid,
                                                                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        if (add_descr_ret){
            ESP_LOGE(GATTS_TAG, "add char descr failed, error code =%x", add_descr_ret);
        }
        break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        gl_profile_tab[PROFILE_WASM_APP_ID].descr_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(GATTS_TAG, "ADD_DESCR_EVT, status %d, attr_handle %d, service_handle %d\n",
                 param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        ESP_LOGI(GATTS_TAG, "SERVICE_START_EVT, status %d, service_handle %d\n",
                 param->start.status, param->start.service_handle);
        break;
    case ESP_GATTS_STOP_EVT:
        break;
    case ESP_GATTS_CONNECT_EVT: {
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        //For the IOS system, please reference the apple official documents about the ble connection parameters restrictions. 
        conn_params.latency = 0;
        conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
        conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
        conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:",
                 param->connect.conn_id,
                 param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                 param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);
        gl_profile_tab[PROFILE_WASM_APP_ID].conn_id = param->connect.conn_id;
        //start sent the update connection parameters to the peer device.
        esp_ble_gap_update_conn_params(&conn_params);
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_DISCONNECT_EVT, disconnect reason 0x%x", param->disconnect.reason);
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GATTS_CONF_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_CONF_EVT, status %d attr_handle %d", param->conf.status, param->conf.handle);
        if (param->conf.status != ESP_GATT_OK){
            esp_log_buffer_hex(GATTS_TAG, param->conf.value, param->conf.len);
        }
        break;
        //TODO: ADD case for ESP_GATTS_CREAT_ATTR_TAB_EVT
        
    case ESP_GATTS_OPEN_EVT:
    case ESP_GATTS_CANCEL_OPEN_EVT:
    case ESP_GATTS_CLOSE_EVT:
    case ESP_GATTS_LISTEN_EVT:
    case ESP_GATTS_CONGEST_EVT:
    default:
        break;
    }
}



static void gatts_profile_mesh_graph_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    esp_err_t err;
    mesh_data_t data;
    data.data = tx_buf;
    data.size = sizeof(tx_buf);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;

    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(GATTS_TAG, "REGISTER_APP_EVT, status %d, app_id %d\n", param->reg.status, param->reg.app_id);
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_id.is_primary = true;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_id.id.inst_id = 0x00;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_id.id.uuid.uuid.uuid16 = GATTS_SERVICE_UUID_MESH_GRAPH;

        esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_id, GATTS_NUM_HANDLE_MESH_GRAPH);
        break;
    case ESP_GATTS_READ_EVT: {
        ESP_LOGI(GATTS_TAG, "GATT_READ_EVT, conn_id %d, trans_id %d, handle %d\n", param->read.conn_id, param->read.trans_id, param->read.handle);
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;

        
        snapshot_total_nodes = current_total_nodes;
        //TODO: What happens if this event is called twice in very short time?

        ds_ble_array_offset = 1;//reset ble_array_offset
        num_received_ds_table = 0; //reset received data stream tables of nodes in the mesh network

        tx_buf[0] = GET_DATA_STREAM_TABLE_ROOT;//Request root node to broadcast the get_data_stream_table message
            //err = esp_mesh_send(&broadcast_group_id, &tx_data, MESH_DATA_GROUP, NULL, 0);
            err = esp_mesh_send(NULL, &data, MESH_DATA_P2P, NULL, 0);
            
            if (err) {
                ESP_LOGE(MESH_TAG, "Error occured at sending message: GET_DATA_STREAM_TABLE: %x", err);
            }
        
        data_stream_table_trans_id = param->read.trans_id;

        break;
    }
    case ESP_GATTS_WRITE_EVT: {
        ESP_LOGI(GATTS_TAG, "ENTER write event, MESH_GRAPH!!");
        if (!param->write.is_prep){
            ESP_LOGI(GATTS_TAG, "GATT_WRITE_EVT, value len %d, value :", param->write.len);
            esp_log_buffer_hex(GATTS_TAG, param->write.value, param->write.len);
            if (gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].descr_handle == param->write.handle && param->write.len == 2){
                uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
                if (descr_value == 0x0001){
                    if (mesh_graph_property & ESP_GATT_CHAR_PROP_BIT_NOTIFY){
                        ESP_LOGI(GATTS_TAG, "notify enable");
                        
                        ESP_LOGI(GATTS_TAG, "GATT_WRITE_EVT, conn_id %d, trans_id %d, handle %d\n", param->read.conn_id, param->read.trans_id, param->read.handle);

                        tx_buf[0] = GET_ROUTING_TABLE;
                    
                        uint8_t notify_data[15];
                        for (int i = 0; i < sizeof(notify_data); ++i)
                        {
                            notify_data[i] = i%0xff;
                        }

                        vTaskDelay(1 * 2000 / portTICK_PERIOD_MS);
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_handle,
                                                sizeof(notify_data), notify_data, true);
                        vTaskDelay(1 * 2000 / portTICK_PERIOD_MS);

                        //broadcasting in the mesh network
                        err = esp_mesh_send(&broadcast_group_id, &data, MESH_DATA_GROUP, NULL, 0);
                            if (err) {
                                ESP_LOGE(MESH_TAG, "Error occured at sending message");
                            
                            }
                    }
                }else if (descr_value == 0x0002){
                    if (wasm_property & ESP_GATT_CHAR_PROP_BIT_INDICATE){
                        ESP_LOGI(GATTS_TAG, "indicate enable");
                        uint8_t indicate_data[15];
                        for (int i = 0; i < sizeof(indicate_data); ++i)
                        {
                            indicate_data[i] = i%0xff;
                        }
                        //the size of indicate_data[] need less than MTU size
                        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_handle,
                                                sizeof(indicate_data), indicate_data, true);
                    }
                }
                else if (descr_value == 0x0000){
                    ESP_LOGI(GATTS_TAG, "notify/indicate disable ");
                }else{
                    ESP_LOGE(GATTS_TAG, "unknown descr value");
                    esp_log_buffer_hex(GATTS_TAG, param->write.value, param->write.len);
                }

            }
            else {
                switch (param->write.value[0])
                {
                case BLE_WASM_MOVING:
                    tx_buf[0] = WASM_MOVING;
                    for(int j=0; j<6; j++){
                        mesh_target.addr[j] = param->write.value[j+1];
                    }
                    for(int i=0; i<6; i++){
                            tx_buf[i+1] = param->write.value[i+7];
                    }
                    err = esp_mesh_send(&mesh_target, &data, MESH_DATA_P2P, NULL, 0);
                    if (err) {
                        ESP_LOGE(MESH_TAG, "Error occured at sending message: WASM_MOVING: err code %x", err);
                    }
                    break;
                case BLE_WASM_OFF_MSG:
                    tx_buf[0] = WASM_OFF_MSG;
                    for(int j=0; j<6; j++){
                        mesh_target.addr[j] = param->write.value[j+1];
                    }
                    err = esp_mesh_send(&mesh_target, &data, MESH_DATA_P2P, NULL, 0);
                    if (err) {
                        ESP_LOGE(MESH_TAG, "Error occured at sending message: WASM_OFF_MSG: err code %x", err);
                    }
                    break;
                case BLE_ADD_DATA_DEST:
                    //an edge target on the mesh graph is changed ; add the data destination to the DS table of corresponding node
                    tx_buf[0] = ADD_NEW_DATA_DEST;
                    for(int j=0; j<6; j++){
                        mesh_target.addr[j] = param->write.value[j+1];
                    }
                    for(int i=0; i<6; i++){
                            tx_buf[i+1] = param->write.value[i+7];
                    }
                    err = esp_mesh_send(&mesh_target, &data, MESH_DATA_P2P, NULL, 0);

                    if (err) {
                        ESP_LOGE(MESH_TAG, "Error occured at sending message: ADD_NEW_DATA_DEST: err code %x", err);
                    }
                    break;
                case BLE_REMOVE_DATA_DEST:
                    //an edge target on the mesh graph is changed ; delete the data destination from the DS table of corresponding node
                    tx_buf[0] = REMOVE_DATA_DEST;
                    for(int j=0; j<6; j++){
                        mesh_target.addr[j] = param->write.value[j+1];
                    }
                    for(int i=0; i<6; i++){
                            tx_buf[i+1] = param->write.value[i+7];
                    }
                    err = esp_mesh_send(&mesh_target, &data, MESH_DATA_P2P, NULL, 0);

                    if (err) {
                        ESP_LOGE(MESH_TAG, "Error occured at sending message: REMOVE_DATA_DEST: err code %x", err);
                    }
                    break;
                
                default:
                    ESP_LOGI(GATTS_TAG, "Unknown package flag for updating Wasm");
                    break;
                }
            }
        }
        example_write_event_env(gatts_if, &b_prepare_write_env, param);
        break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGI(GATTS_TAG,"ESP_GATTS_EXEC_WRITE_EVT");
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        example_exec_write_event_env(&b_prepare_write_env, param);
        break;
    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
        break;
    case ESP_GATTS_UNREG_EVT:
        break;
    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(GATTS_TAG, "CREATE_SERVICE_EVT, status %d,  service_handle %d\n", param->create.status, param->create.service_handle);
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_handle = param->create.service_handle;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_MESH_GRAPH;

        esp_ble_gatts_start_service(gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_handle);
        mesh_graph_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        esp_err_t add_char_ret =esp_ble_gatts_add_char( gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_handle, &gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_uuid,
                                                        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                        mesh_graph_property,
                                                        NULL, NULL);
        if (add_char_ret){
            ESP_LOGE(GATTS_TAG, "add char failed, error code =%x",add_char_ret);
        }
        break;
    case ESP_GATTS_ADD_INCL_SRVC_EVT:
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        ESP_LOGI(GATTS_TAG, "ADD_CHAR_EVT, status %d,  attr_handle %d, service_handle %d\n",
                 param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);

        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_handle = param->add_char.attr_handle;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_ble_gatts_add_char_descr(gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].service_handle, &gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].descr_uuid,
                                     ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                     NULL, NULL);
        break;
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].descr_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(GATTS_TAG, "ADD_DESCR_EVT, status %d, attr_handle %d, service_handle %d\n",
                 param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        ESP_LOGI(GATTS_TAG, "SERVICE_START_EVT, status %d, service_handle %d\n",
                 param->start.status, param->start.service_handle);
        break;
    case ESP_GATTS_STOP_EVT:
        break;
    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(GATTS_TAG, "CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:",
                 param->connect.conn_id,
                 param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                 param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]);
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].conn_id = param->connect.conn_id;
        gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].gatts_if = gatts_if;
        break;
    case ESP_GATTS_CONF_EVT:
        ESP_LOGI(GATTS_TAG, "ESP_GATTS_CONF_EVT status %d attr_handle %d", param->conf.status, param->conf.handle);
        if (param->conf.status != ESP_GATT_OK){
            esp_log_buffer_hex(GATTS_TAG, param->conf.value, param->conf.len);
        }
    break;
    case ESP_GATTS_LISTEN_EVT:
    case ESP_GATTS_DISCONNECT_EVT:
    case ESP_GATTS_OPEN_EVT:
    case ESP_GATTS_CANCEL_OPEN_EVT:
    case ESP_GATTS_CLOSE_EVT:
    case ESP_GATTS_CONGEST_EVT:
    default:
        break;
    }
}

void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    // If event is register event, store the gatts_if for each profile 
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGI(GATTS_TAG, "Reg app failed, app_id %04x, status %d\n",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    //If the gatts_if equal to profile A, call profile A cb handler,so here call each profile's callback
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gatts_if == ESP_GATT_IF_NONE || // ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function
                    gatts_if == gl_profile_tab[idx].gatts_if) {
                if (gl_profile_tab[idx].gatts_cb) {
                    gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

/******************
 * WIFI MESH
 ******************/
void esp_mesh_p2p_tx_main(void *arg)
{
    esp_err_t err;
    int send_count = 0;
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    mesh_data_t data;
    data.data = tx_buf;
    data.size = sizeof(tx_buf);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;
    is_running = true;

    while (is_running) {
        //esp_mesh_get_routing_table returns only descendant nodes, no ancestors!!
        esp_mesh_get_routing_table((mesh_addr_t *) &route_table,
                                   CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
        if (send_count && !(send_count % 100)) {
            ESP_LOGI(MESH_TAG, "size:%d/%d,send_count:%d", route_table_size,
                     esp_mesh_get_routing_table_size(), send_count);
        }

        char* message = "Hello, here is data monitor";
        int len = strlen(message);
        tx_buf[0] = INFORM_NODE_TXT_MSG;
        for(int j=0; j<len; j++){
            tx_buf[j+1] = (uint8_t)message[j];
        }

        for (int i = 0; i < num_of_destination; i++) {
            err = esp_mesh_send(&data_stream_table[i], &data, MESH_DATA_P2P, NULL, 0);
            if (err) {
                ESP_LOGE(MESH_TAG,
                         "[ROOT-2-UNICAST:%d][L:%d]parent:"MACSTR" to "MACSTR", heap:%d[err:0x%x, proto:%d, tos:%d]",
                         send_count, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                         MAC2STR(route_table[i].addr), esp_get_minimum_free_heap_size(),
                         err, data.proto, data.tos);
            }
        }

        /* if route_table_size is less than 10, add delay to avoid watchdog in this task. */
        if (route_table_size < 10) {
            vTaskDelay(1 * 3000 / portTICK_PERIOD_MS);
        }
    }
    vTaskDelete(NULL);
}

void esp_mesh_p2p_rx_main(void *arg)
{
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t rx_data;
    mesh_data_t tx_data;
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    mesh_addr_t inform_ds_table_dest;
    int route_table_size = 0;
    int send_count = 0;
    //reception
    int flag = 0;
    rx_data.data = rx_buf;
    rx_data.size = RX_SIZE;
    //transimission
    tx_data.data = tx_buf;
    tx_data.size = sizeof(tx_buf);
    tx_data.proto = MESH_PROTO_BIN;
    tx_data.tos = MESH_TOS_P2P;
    esp_gatt_rsp_t rsp;

    is_running = true;

    while (is_running) {
        rx_data.size = RX_SIZE;
        err = esp_mesh_recv(&from, &rx_data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK || !rx_data.size) {
            ESP_LOGE(MESH_TAG, "err:0x%x, size:%d", err, rx_data.size);
            continue;
        }

        switch (rx_data.data[0])
        {
        case INFORM_NODE_TXT_MSG:
            ESP_LOGI(MESH_TAG, "Received message: %s", (char*)rx_data.data+1);
            break;
        case GET_ROUTING_TABLE:
            //esp_mesh_get_routing_table returns only descendant nodes, no ancestors!!
            esp_mesh_get_routing_table((mesh_addr_t *) &route_table,
                                   CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
            //| MSG Code | table length | MAC addresses | name length | name string | has wasm (yes=0x01, no=0x00)|
            tx_buf[0] = (uint8_t)INFORM_ROUTING_TABLE;
            tx_buf[1] = (uint8_t)esp_mesh_get_routing_table_size(); 
            for(int j=0; j<route_table_size; j++){
                for(int i=0;i<6;i++){
                    tx_buf[j*6+i+2] = route_table[j].addr[i];
                }
            }
            int name_len = strlen(MESH_NODE_NAME);
            tx_buf[route_table_size*6+2] = (uint8_t)name_len;
            for(int k=0; k<name_len; k++){
                tx_buf[route_table_size*6 + 2 + k] = (uint8_t)MESH_NODE_NAME[k];
            }
            //has Wasm?
            #ifdef HAS_WASM_MODULE
            tx_buf[2 + route_table_size*6 + name_len] = 0x01;
            #else
            tx_buf[2 + route_table_size*6 + name_len] = 0x00;
            #endif
            //Response
            err = esp_mesh_send(&from, &tx_data, MESH_DATA_P2P, NULL, 0);
            if (err) {
                ESP_LOGE(MESH_TAG,
                         "[ROOT-2-UNICAST:%d][L:%d]parent:"MACSTR" to "MACSTR", heap:%d[err:0x%x, proto:%d, tos:%d]",
                         send_count, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                         MAC2STR(from.addr), esp_get_minimum_free_heap_size(),
                         err, tx_data.proto, tx_data.tos);
            }
            break;
        case INFORM_ROUTING_TABLE: //| MSG Code | table length | MAC addresses | name length | name string | has wasm (yes=0x01, no=0x00)|
        //TODO: forward to WebIDE via BLE
            ESP_LOGI(MESH_TAG, "Length (INFORM_ROUTING_TABLE): %d", rx_data.data[1]);
            for(int i=0; i<rx_data.data[1]*6; i++){
                ESP_LOGI(MESH_TAG, "MAC: %d", rx_data.data[i+2]);
            }
            
            for(int j=0; j<sizeof(ble_array); j++){
                ble_array[j] = rx_data.data[j+1];
            }
            
            //TODO: check rx_data.data size.
            err = esp_ble_gatts_send_indicate(gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].gatts_if, gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].conn_id, 
                gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].char_handle, sizeof(ble_array), ble_array, false);
                ESP_LOGI(MESH_TAG, "The table data size: %d", sizeof(ble_array));
                if(err){
                    ESP_LOGE(GATTS_TAG, "Notification failed!! CODE: %d", err);
                }
            
            break;
        case GET_DATA_STREAM_TABLE: //| MSG Code | MAC addresse of the client |
            ESP_LOGI(MESH_TAG, "A Mesh-Message arrived: GET_DATA_STREAM_TABLE");
            tx_buf[0] = INFORM_DATA_STREAM_TABLE;
            
            tx_buf[1] = 0;// This node has no wasm module.
           
            tx_buf[2] = num_of_destination;

            if(num_of_destination != 0){
                for(int j=0; j<num_of_destination; j++){
                    for(int i=0;i<6;i++){
                        tx_buf[j*6+i+3] = data_stream_table[j].addr[i];
                    }
                }
            }

            for(int i=0; i<6; i++){
                inform_ds_table_dest.addr[i] = rx_data.data[i+1];
            }

            err = esp_mesh_send(&inform_ds_table_dest, &tx_data, MESH_DATA_P2P, NULL, 0);
            if (err) {
                ESP_LOGE(MESH_TAG,
                         "[ROOT-2-UNICAST:%d][L:%d]parent:"MACSTR" to "MACSTR", heap:%d[err:0x%x, proto:%d, tos:%d]",
                         send_count, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                         MAC2STR(from.addr), esp_get_minimum_free_heap_size(),
                         err, tx_data.proto, tx_data.tos);
            }
            ESP_LOGI(MESH_TAG, "Send a Mesh-Message was successful: answer of GET_DATA_STREAM_TABLE");

            break;
        case INFORM_DATA_STREAM_TABLE: //| MSG Code | Wasm flag (Does the sender node has Wasm module?) | table length | MAC addresses |
        //TODO: forward to WebIDE via BLE
            ESP_LOGI(MESH_TAG, "Length (INFORM_DATA_STREAM_TABLE): %d", rx_data.data[2]);
            for(int i=0; i<rx_data.data[2]*6; i++){
                ESP_LOGI(MESH_TAG, "MAC: %d", rx_data.data[i+3]);
            }
            
            if(ds_ble_array_offset + rx_data.data[2]*6 + 1 > BLE_LOCAL_MTU){
                ESP_LOGE(MESH_TAG, "Data stream table size is too large!!");
                ds_ble_array_offset = 1;
                num_received_ds_table = 0;
                break;
            }

            num_received_ds_table++; 
            ds_ble_array[0] = num_received_ds_table;
            ds_ble_array[ds_ble_array_offset] = rx_data.data[1];//wasm flag
            ds_ble_array_offset += 1;
            ds_ble_array[ds_ble_array_offset] = rx_data.data[2] + 1;//including the owner of the table
            ds_ble_array_offset += 1;

            for(int i=0; i<6; i++){
                ds_ble_array[ds_ble_array_offset + i] = from.addr[i];
            }

            ds_ble_array_offset += 6;

            for(int j=0; j<rx_data.data[2]*6; j++){
                ds_ble_array[ds_ble_array_offset + j] = rx_data.data[j+3];
            }

            ds_ble_array_offset = ds_ble_array_offset + rx_data.data[2]*6; //TODO: check this offset
            
            
            ESP_LOGI(MESH_TAG, "num_received_ds_table: %d", num_received_ds_table);
            ESP_LOGI(MESH_TAG, "snapshot_total_nodes %d", snapshot_total_nodes);
            if(num_received_ds_table == snapshot_total_nodes){
                rsp.attr_value.len = ds_ble_array_offset;
                for(int j=0;j<ds_ble_array_offset;j++){
                    rsp.attr_value.value[j] = ds_ble_array[j];
                }

                ESP_LOGI(GATTS_TAG, "Send stream table via BLE");
                esp_ble_gatts_send_response(gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].gatts_if, gl_profile_tab[PROFILE_MESH_GRAPH_APP_ID].conn_id, 
                                        data_stream_table_trans_id, ESP_GATT_OK, &rsp);

                ds_ble_array_offset = 1;
                num_received_ds_table = 0;
            }
            
            break;
        case INFORM_TOTAL_NUMBER_OF_NODES:
            current_total_nodes = rx_data.data[1];
            ESP_LOGI(MESH_TAG, "%d nodes are participating in this mesh network", current_total_nodes);
            break;
        case SEND_WASM_INIT:
            //Receive wasm init msg. | MSG_CODE| #packet (upperbyte) | #packet (lower byte)|
            wasm_packet_number = (rx_data.data[1]) << 8 | rx_data.data[2];
            ESP_LOGI(MESH_TAG, "Init for Wasm update. #packet is %d", wasm_packet_number);
                    int result = remove("/spiffs/main.wasm"); //clear current wasm file
                    if(result){
                        ESP_LOGE(GATTS_TAG, "Failed to remove wasm file ");
                    }

            break;
        case SEND_WASM:
            //Receive wasm | MSG_CODE| payload_length from here to end | write_code (BLE) | current_offset (upperbyte) | current_offset (lower byte)| | Wasm binary |
            wasm_current_transmit_offset = (rx_data.data[3]) << 8 | rx_data.data[4];
            ESP_LOGI(MESH_TAG, "current transmit offset of WASM is %d", wasm_current_transmit_offset);
            FILE* wasmFile = fopen("/spiffs/main.wasm", "ab");//Open with append mode
            if (wasmFile == NULL) {
                ESP_LOGE(GATTS_TAG, "Failed to open file for reading");
                return;
            }
            ESP_LOGI(MESH_TAG, "Open file was successful");
            size_t written_length = fwrite(rx_data.data+5, 1, rx_data.data[2]-3, wasmFile); //TODO: here causes a guru meditation error!!
            ESP_LOGI(MESH_TAG, "Written length of WASM update is %d", written_length);
            fclose(wasmFile);
            if(!written_length){
                ESP_LOGE(GATTS_TAG, "Failed to write wasm binary");
                return;
            }
            if(wasm_current_transmit_offset+1 == wasm_packet_number){
                esp_restart(); //TODO: check if it works without restart.
            }

            break;
        case ADD_NEW_DATA_DEST: //|MSG_CODE|DEST_MAC|
            if(num_of_destination <= MESH_DATA_STREAM_TABLE_LEN){
                for(int i=0; i<6; i++){
                    data_stream_table[num_of_destination].addr[i] = rx_data.data[i+1];
                }
                num_of_destination++;
            }
            break;
        case REMOVE_DATA_DEST: //|MSG_CODE|DEST_MAC|
            if(num_of_destination > 0){
                int k=0;
                int count_same_value = 0;
                for(k=0; k<num_of_destination; k++){
                    for(int j=0; j<6; j++){
                        if(data_stream_table[k].addr[j]==rx_data.data[j+1]){ 
                            count_same_value++;
                        }
                    }
                    if(count_same_value==6){//found the MAC to be removed?
                        break;
                    }
                    else{
                        count_same_value = 0;
                    }
                }
                
                for(; k<=num_of_destination; k++){ //shift one step addresses to the top of list.
                    for(int m=0; m<6; m++){
                        data_stream_table[num_of_destination-1].addr[m] = data_stream_table[num_of_destination].addr[m];
                    }
                }
                num_of_destination--;
            }
            break;
        default:
            ESP_LOGI(MESH_TAG, "Received message: %s", (char*)rx_data.data);
            break;
        }
        
    }
    vTaskDelete(NULL);
}

esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_comm_p2p_started = false;
    if (!is_comm_p2p_started) {
        is_comm_p2p_started = true;
        //Transmission task
        xTaskCreate(esp_mesh_p2p_tx_main, "MPTX", 3072, NULL, 5, NULL);
        //Reception task
        xTaskCreate(esp_mesh_p2p_rx_main, "MPRX", 3072, NULL, 5, NULL);
    }
    return ESP_OK;
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));

}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint16_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR", duty:%d",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr), connected->duty);
        last_layer = mesh_layer;
        is_mesh_connected = true;
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
    
        esp_mesh_comm_p2p_start();
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));

        for(int i=0; i<6; i++){
            current_root_mac[i] = root_addr->addr[i];
        }
        
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    case MESH_EVENT_PS_PARENT_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d", ps_duty->duty);
    }
    break;
    case MESH_EVENT_PS_CHILD_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, "MACSTR", duty:%d", ps_duty->child_connected.aid-1,
                MAC2STR(ps_duty->child_connected.mac), ps_duty->duty);
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%d", event_id);
        break;
    }
}

void app_main() {
    //ESP_LOGI(TAG, "set MAC address");
    //esp_base_mac_addr_set(new_mac);

    //set_default_destination();

    //Initialize spiffs
    ESP_LOGI(TAG, "Initializing SPIFFS");
    esp_err_t ret;

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret_spiffs = esp_vfs_spiffs_register(&conf);

    if (ret_spiffs != ESP_OK) {
        if (ret_spiffs == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret_spiffs == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret_spiffs));
        }
        return;
    }
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /*  create network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));

    /*  Wi-Fi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    /*  register IP events handler */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());


    //  mesh initialization 
    ESP_ERROR_CHECK(esp_mesh_init());
    // register mesh events handler 
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

    // Enable the Mesh IE encryption by default 
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    // mesh ID 
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    // channel (must match the router's channel)
    cfg.channel = CONFIG_MESH_CHANNEL;

    // Is root node fix?
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));

    //set group id
    ESP_ERROR_CHECK(esp_mesh_set_group_id(&broadcast_group_id, 1));

    // mesh softAP 
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
        strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    //set sendi block time
    ESP_ERROR_CHECK(esp_mesh_send_block_time(1000));

    // mesh start 
    ESP_ERROR_CHECK(esp_mesh_start());
    
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%d, %s<%d>%s, ps:%d\n",  esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)":"(tree)", esp_mesh_is_ps_enabled()); 

    //******BLE*******
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gap register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gatts_app_register(PROFILE_WASM_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gatts_app_register(PROFILE_MESH_GRAPH_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
        return;
    }
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(BLE_LOCAL_MTU);
    if (local_mtu_ret){
        ESP_LOGE(GATTS_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }

}