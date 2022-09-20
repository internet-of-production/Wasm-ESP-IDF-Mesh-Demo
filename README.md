# Wasm-ESP-IDF-Mesh-Demo
## Installation Instructions
### Prerequisites
- Installation with [Platform IO IDE for VSCode](https://docs.platformio.org/en/stable/integration/ide/vscode.html) is recommended. 
- This prototype uses [ESP-WIFI-MESH](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/esp-wifi-mesh.html) and create a fixed-root mesh network, therefore, a router is necessary. 
- At least three ESP boards are required for the root, processor, and monitor node.
- Exactly one ESP MCU must be the root node.

### PIO setting
Check your ESP board and edit `platformio.ini`

### Configuration
Make the following change in `main.c` for each node type implementation.
- Set your router's SSID and password
- To deploy multiple processor/monitor nodes, set unique MAC addresses for `uint8_t new_mac[6]`

### Upload
Make sure that your board is connected and PIO destinates the corresponding port. After build and upload the program to the MCU, a sample WASM file `main.wasm` also has to be uploded. You find this operation button under `Project task -> <board name> -> platform -> upload file system image` in the PIO menu.

## Web IDE
- A prototype of web IDE is available here: https://wasm-ide.herokuapp.com/ide.html
- Source code and instruction for UI: https://github.com/Ayato77/Wasm-ble-web-ide
