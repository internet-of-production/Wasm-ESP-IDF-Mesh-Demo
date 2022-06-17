const serviceUUID = 0x00FF;
//Characteristics
const CharacteristicUUID_Notification = '000000000-0000-0000-0000-000000000000';
const CharacteristicUUID_ReadRead = 0xFF01;
const CharacteristicUUID_Write = '00000000-0000-0000-0000-000000000000';

let keyDevice;
let keyServer;
let keyService;
let keyNotificationCharacteristic;
let keyReadCharacteristic;
let keyWriteCharacteristic;

let esp32Characteristic;
let mtu = 20 //Default BLE MTU size is 20 Bytes

/**
 * web bluetooth api
 * scan ble peripheral devices
 */
function bleScan(){
    //TODO: get service_uuid and characteristic_uuid from html form
    //TODO: show a status message of connection with alert or something
    //TODO: Store requested MTU size!!
    navigator.bluetooth.requestDevice({
        filters: [{
            //name: 'NAME',
            //namePrefix:'PREFIX',
            services: [serviceUUID],
        }]
    })
        .then(device => {
            //startModel('connecting...');
            //connecting
            console.log("device.id    : " + device.id);
            console.log("device.name  : " + device.name);
            console.log("device.uuids : " + device.uuids);
            keyDevice = device;
            keyDevice.addEventListener('gattserverdisconnected', onDisconnected);
            return device.gatt.connect();
        })
        //get service
        .then(server => {
            keyServer = server;
            console.log('Getting service...');
            return server.getPrimaryService(serviceUUID);
        })
        //get characteristic
        .then(service => {
            keyService = service;
            console.log('Getting Notification Characteristic...');
            return service.getCharacteristic(CharacteristicUUID_ReadRead);
        })
        //Characteristic
        .then(characteristic => {
            //Write here Read/Write/Notifications process for characteristic
            esp32Characteristic = characteristic;
            console.log('Got Characteristic');
            return esp32Characteristic.readValue();
        })
        .then(value => {
            //Get local MTU from the peripheral device.
            let mtu1 = value.getUint8(0);
            let mtu2 = value.getUint8(1);
            mtu = (mtu1<<8 | mtu2);
            console.log('Negotiated MTU size is ' + mtu);
        })
        .catch(error => {
            console.log(error);
        })
};

/**
 * web bluetooth
 * upload wasm binary using writeValue method (ESP_GATTS_WRITE_EVT)
 * @param: Uint8Array, wasm binary compiled by AssemblyScript CLI (See ide.html)
 */
function onUpload(wasmArray){
    console.log('onUpload')
    console.log(wasmArray.length)
    let numberOfPackets = Math.ceil(wasmArray.length/(mtu-3))
    //Assume that the max number of packets is 16^4
    let bufferInit = new ArrayBuffer(3)//
    let byteArray = new Uint8Array(bufferInit)
    byteArray[0] = 0x01;
    byteArray[1] = numberOfPackets>>8;
    byteArray[2] = numberOfPackets % 256;

    let bufferNext = new ArrayBuffer(mtu)
    let byteNextArray = new Uint8Array(bufferNext)


    esp32Characteristic.writeValue(byteArray)
        .then(_ => {
                console.log('numberOfPackets')
                console.log(numberOfPackets)
                let len = 0
                for(let i=0; i<numberOfPackets; i++) {
                    //Fit size of the last packet
                    if(i+1 == numberOfPackets){
                        let bufferLast = new ArrayBuffer((wasmArray.length % mtu) + 3)
                        byteNextArray = new Uint8Array(bufferLast)
                    }
                    byteNextArray[0] = 0x02
                    byteNextArray[1] = i >> 8
                    byteNextArray[2] = i % 256

                    len = byteNextArray.length
                    for (let j = 3; j < len; j++) {
                        byteNextArray[j] = wasmArray[i * (len - 3) + j - 3] //TODO: One can make here more simple
                    }
                    console.log('byteNextArray')
                    console.log(byteNextArray.length)
                    esp32Characteristic.writeValue(byteNextArray).then()
                        .catch(err=> {
                        console.log(err)
                    })
                }
        })
        .catch(error => {
            console.log(error);
    })
}

function onDisconnected(){
    console.log('> Bluetooth Device disconnected')
};