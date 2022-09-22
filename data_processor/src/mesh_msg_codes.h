/**
 * @file mesh_msg_codes.h
 * @brief code definition for handling messages
 */

//Requests to the root node 0x0--
#define GET_ROUTING_TABLE 0x00
#define GET_DATA_STREAM_TABLE 0x01

//Requests to nodes 0x2--
#define GET_NODE_NAME_AND_MAC_ADD 0x20

//Information of nodes 0xE--
#define INFORM_NODE_NAME 0xE0
#define INFORM_NODE_TXT_MSG 0xE1 
#define INFORM_CPU_LOAD 0xE2
#define INFORM_ROUTING_TABLE 0xE3 //| MSG Code | table length | MAC address | name length | name string | has wasm (yes=0x01, no=0x00)|
#define INFORM_DATA_STREAM_TABLE 0xE4 //| MSG Code | total number of nodes | table length | MAC addresses |
#define INFORM_TOTAL_NUMBER_OF_NODES 0xE5 // | MSG Code | total number of nodes | Root node informs all other nodes

//Transmitting read/sensored data 0xA--
#define Machine_DATA 0xA0
#define MACHINE_DATA_INT 0xA1

//Wasm Update
#define SEND_WASM_INIT 0xB0 
#define SEND_WASM 0xB1
#define WASM_OFF_MSG 0xB2
#define WASM_MOVING 0xB4

//Event of modification on the mesh graph UI
#define ADD_NEW_DATA_DEST 0xC1
#define REMOVE_DATA_DEST 0xC2