/**
 * @file mesh_msg_codes.h
 * @brief code definition for handling messages
 */

//Requests to the root node 0x0--
#define GET_ROUTING_TABLE 0x00

//Requests to nodes 0x2--
#define GET_NODE_NAME_AND_MAC_ADD 0x20

//Information of nodes 0xE--
#define INFORM_NODE_NAME 0xE0
#define INFORM_NODE_TXT_MSG 0xE1 
#define INFORM_CPU_LOAD 0xE2
#define INFORM_ROUTING_TABLE 0xE3 //| MSG Code | table length | MAC address | name length | name string |

//Transmitting read/sensored data 0xA--
#define Machine_DATA 0xA0