# ESP-WIFI-MESH Demo
Mesh network based on the Arduino frame work provides a simple implementation. However, detailed configuration like menuconfig of esp-idf is not available. Our project intends that a MCU provides the communication network and a way to modify Wasm file over the air, for example, via WiFi or BLE simultaneously. Therefore, we investigate whether ESP-IDF fulfills our requirements.

## Setting
### Root node
- esp_mesh_set_type(MESH_ROOT);
- router configuration
### Intermediate node
- Adjust the root node's MAC adress
- No setting for the esp_mesh_set_type necessary
- No router setting necessary 
### Note
- Root node must connect to a router: Without router configuration, it can not send messgages due to implementation of the library

### Documentation 
You find the documentation under [./doxygen](./doxygen)
