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

//wasm3
#include "wasm3.h"
#include "m3_env.h"
#include "wasm3_defs.h"


#define USE_WASM

#define RX_SIZE          (1500) //MTU of incoming packets. MESH_MTU is 1500 bytes
#define TX_SIZE          (1460) //MTU of outgoing packets. 
#define CONFIG_MESH_ROUTE_TABLE_SIZE 10 //There is a limit??
#define CONFIG_MESH_AP_CONNECTIONS 5 //MAX 10
#define CONFIG_MESH_CHANNEL 0 /* channel (must match the router's channel) */
#define CONFIG_MESH_AP_PASSWD "wasiwasm"
#define CONFIG_MESH_ROUTER_SSID "FRITZ!Box 7560 YQ"
#define CONFIG_MESH_ROUTER_PASSWD "19604581320192568195"
#define MESH_NODE_NAME "data_processor"
#define MESH_DATA_STREAM_TABLE_LEN 2*6 // there is two receiver

//Node with the same MESH_ID can communicates each other.
static const uint8_t MESH_ID[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
//Broadcast group
static const mesh_addr_t broadcast_group_id = {.addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
//base MAC address must be unicast MAC (least significant bit of first byte must be zero)
uint8_t new_mac[6] = {0x00,0x00,0x00,0x00,0xFF,0x01};

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


static const char *MESH_TAG = "mesh_main";
static const char *TAG = "wasm";

#define HAS_WASM_MODULE
#define WASM_STACK_SLOTS    4000
#define CALC_INPUT  2
#define FATAL(func, msg) { ESP_LOGE(TAG, "Fatal: " func " "); ESP_LOGE(TAG, "%s", msg);}
#define BASE_PATH "/spiffs"

IM3Environment env;
IM3Runtime runtime;
IM3Module module;
IM3Function calcWasm;
int wasmResult = 0;

/**
 * @fn 
 * WASM setup using wasm3
 */
static void run_wasm()
{
    // load wasm from SPIFFS
    /* If default CONFIG_ARDUINO_LOOP_STACK_SIZE 8192 < wasmFile,
    a new size must be given in  \Users\<user>\.platformio\packages\framework-arduinoespressif32\tools\sdk\include\config\sdkconfig.h
    https://community.platformio.org/t/esp32-stack-configuration-reloaded/20994/2
    */
    FILE* wasmFile = fopen("/spiffs/main.wasm", "rb");
    if (wasmFile == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }

    //get file status
    struct stat sb;
    if (stat("/spiffs/main.wasm", &sb) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }

    // read file
    unsigned int build_main_wasm_len = sb.st_size;
    unsigned char build_main_wasm[build_main_wasm_len];
    fread(build_main_wasm, 1, sb.st_size, wasmFile);
    fclose(wasmFile);

    ESP_LOGI(TAG, "wasm_length:");
    ESP_LOGI(TAG, "%d", build_main_wasm_len);

    ESP_LOGI(TAG, "Loading WebAssembly was successful");
    //ESP_LOGI(TAG, "Wasm Version ID: ");
    //ESP_LOGI(TAG, getWasmVersionId());

    M3Result result = m3Err_none;

    //it warks also without using variable
    //uint8_t* wasm = (uint8_t*)build_app_wasm;

    env = m3_NewEnvironment ();
    if (!env) {/*setWasmInvalidFlag();*/ FATAL("NewEnvironment", "failed"); esp_restart();}

    runtime = m3_NewRuntime (env, WASM_STACK_SLOTS, NULL);
    if (!runtime) {/*setWasmInvalidFlag();*/ FATAL("m3_NewRuntime", "failed"); esp_restart();}

    #ifdef WASM_MEMORY_LIMIT
    runtime->memoryLimit = WASM_MEMORY_LIMIT;
    #endif

    result = m3_ParseModule (env, &module, build_main_wasm, build_main_wasm_len);
    if (result) {/*setWasmInvalidFlag();*/ FATAL("m3_ParseModule", result); esp_restart();}

    result = m3_LoadModule (runtime, module);
    if (result) {/*setWasmInvalidFlag();*/ FATAL("m3_LoadModule", result); esp_restart();}

    // link
    //result = LinkArduino (runtime);
    //if (result) FATAL("LinkArduino", result);


    result = m3_FindFunction (&calcWasm, runtime, "calcWasm");
    if (result) {/*setWasmInvalidFlag();*/ FATAL("m3_FindFunction(calcWasm)", result); esp_restart();}

    ESP_LOGI(TAG, "Running WebAssembly...");

}

  /**
 * @fn 
 * Call WASM task
 */

  void wasm_task(){
    const void *i_argptrs[CALC_INPUT];
    char inputBytes[CALC_INPUT] = {0x01, 0x02};
    M3Result result = m3Err_none;
    
    for(int i=0; i<CALC_INPUT ;i++){
      i_argptrs[i] = &inputBytes[i];
    }

    /*
    m3_Call(function, number_of_arguments, arguments_array)
    To get return, one have to call a function with m3_Call first, then call m3_GetResultsV(function, adress).
    (Apparently) m3_Call stores the result in the liner memory, then m3_GetResultsV accesses the address.
    */
    result = m3_Call(calcWasm,CALC_INPUT,i_argptrs);                       
    if(result){
      //setWasmInvalidFlag();
      FATAL("m3_Call(calcWasm):", result);
      esp_restart();
    }

    result = m3_GetResultsV(calcWasm, &wasmResult);
    if(result){
      //setWasmInvalidFlag();
      FATAL("m3_GetResultsV(calcWasm):", result);
      esp_restart();
    }

  }

/************************************************************************
 * Management of data stream table in the mesh network
 ************************************************************************/

void set_default_destination(){
    num_of_destination = 1;
    for(int j=0; j<6;j++){
        data_stream_table[0].addr[j] = default_dest_mac[j];
    }    
}

int wasm_packet_number = 0;
int wasm_current_transmit_offset = 0;


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

        char* message = "Hello, here is the processor";
        int len = strlen(message);
        tx_buf[0] = INFORM_NODE_TXT_MSG;
        for(int j=1; j<len+1; j++){
            tx_buf[j] = (uint8_t)message[j];
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
            ESP_LOGI(MESH_TAG, "Received message: %s", (char*)rx_data.data);
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
        case GET_DATA_STREAM_TABLE: //| MSG Code | MAC addresse of the client |
            tx_buf[0] = INFORM_DATA_STREAM_TABLE;
            #ifdef USE_WASM
            tx_buf[1] = 1; //WASM flag. Information of Wasm availability
            #else
            tx_buf[1] = 0;
            #endif
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

            break;
        case INFORM_TOTAL_NUMBER_OF_NODES:
            current_total_nodes = rx_data.data[1];
            ESP_LOGI(MESH_TAG, "%d nodes are participating in this mesh network", current_total_nodes);
            //add_to_data_stream_table(current_root_mac);//TODO: this implementation is just for experimental purpose. Remove if you know which MAC should initially be used. 
            break;
        case SEND_WASM_INIT:
            //Receive wasm init msg. | MSG_CODE| #packet (upperbyte) | #packet (lower byte)|
            wasm_packet_number = (rx_data.data[1]) << 8 | rx_data.data[2];
            ESP_LOGI(MESH_TAG, "Init for Wasm update. #packet is %d", wasm_packet_number);
                    int result = remove("/spiffs/main.wasm"); //clear current wasm file
                    if(result){
                        ESP_LOGE(MESH_TAG, "Failed to remove wasm file ");
                    }
                    ESP_LOGI(MESH_TAG, "remove wasm file was successful");

            break;
        case SEND_WASM:
            //Receive wasm | MSG_CODE| payload_length from here to end | write_code (BLE) | current_offset (upperbyte) | current_offset (lower byte)| | Wasm binary |
            wasm_current_transmit_offset = (rx_data.data[3]) << 8 | rx_data.data[4];
                FILE* wasmFile = fopen("/spiffs/main.wasm", "ab");//Open with append mode
                if (wasmFile == NULL) {
                    ESP_LOGE(MESH_TAG, "Failed to open file for reading");
                    return;
                }
                ESP_LOGI(MESH_TAG, "Write new wasm binary chunk");
                size_t write_len = rx_data.data[1]-3;
                size_t written_length = fwrite(rx_data.data+5, 1, write_len, wasmFile);
                ESP_LOGI(MESH_TAG, "Write new wasm binary chunk end");
                fclose(wasmFile);
                if(!written_length){
                    ESP_LOGE(MESH_TAG, "Failed to write wasm binary");
                    return;
                }
                if(wasm_current_transmit_offset+1 == wasm_packet_number){
                    ESP_LOGI(MESH_TAG, "Wasm update succeeded");
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
    ESP_LOGI(TAG, "set MAC address");
    esp_base_mac_addr_set(new_mac);

    set_default_destination();

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
    // router 
    
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
    strlen(CONFIG_MESH_ROUTER_PASSWD));


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

    // mesh start 
    ESP_ERROR_CHECK(esp_mesh_start());
    
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%d, %s<%d>%s, ps:%d\n",  esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)":"(tree)", esp_mesh_is_ps_enabled()); 


    ESP_LOGI(TAG, "Loading wasm");
    run_wasm(NULL);

    wasm_task();
    ESP_LOGI(TAG, "Wasm result:");
    ESP_LOGI(TAG,"%d", wasmResult);
}