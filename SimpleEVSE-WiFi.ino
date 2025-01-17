#include <ESP8266WiFi.h>              // Whole thing is about using Wi-Fi networks
#include <SPI.h>                      // RFID MFRC522 Module uses SPI protocol
#include <ESP8266mDNS.h>              // Zero-config Library (Bonjour, Avahi)
#include <ArduinoJson.h>              // JSON Library for Encoding and Parsing Json object to send browser
#include <FS.h>                       // SPIFFS Library for storing web files to serve to web browsers
#include <ESPAsyncTCP.h>              // Async TCP Library is mandatory for Async Web Server
#include <ESPAsyncWebServer.h>        // Async Web Server with built-in WebSocket Plug-in
#include <SPIFFSEditor.h>             // This creates a web page on server which can be used to edit text based files
#include <TimeLib.h>                  // Library for converting epochtime to a date
#include <WiFiUdp.h>                  // Library for manipulating UDP packets which is used by NTP Client to get Timestamps
#include <SoftwareSerial.h>           // Using GPIOs for Serial Modbus communication
#include <ModbusMaster.h>

#include <string.h>
#include "src/proto.h"
#include "src/ntp.h"
#include "src/websrc.h"

#ifdef ESP8266
extern "C" {
#include "user_interface.h"  // Used to get Wifi status information
}
#endif

//////////////////////////////////////////////////////////////////////////////////////////
///////       Variables For Whole Scope
//////////////////////////////////////////////////////////////////////////////////////////
//EVSE Variables
unsigned long millisStartCharging = 0;
unsigned long millisStopCharging = 0;
int16_t iPrice = 0;
uint8_t maxinstall = 0;
uint8_t iFactor = 0;
float consumption = 0.0;
int currentToSet = 6;
int8_t evseStatus = 0;
bool evseSessionTimeOut = false;
bool vehicleCharging = false;
bool vehicleChargingFlag = false;
AsyncWebParameter* awp;
const char * initLog = "{\"type\":\"latestlog\",\"list\":[]}";

//Debug
bool debug = true;

//Metering
float meterReading = 0.0;
float meteredKWh = 0.0;
float currentKW = 0.0;
uint8_t intLength = 0;

//Metering S0
uint8_t meterPin;
uint16_t meterTimeout = 10; //sec
uint16_t kwhimp;
uint8_t meterphase;
volatile unsigned long numberOfMeterImps = 0;
unsigned long meterImpMillis = 0;
unsigned long previousMeterMillis = 0;
volatile uint8_t meterInterrupt = 0;

//objects and instances
SoftwareSerial sSerial(D1, D2); //SoftwareSerial object (RX, TX)
ModbusMaster evseNode;
ModbusMaster meterNode;
AsyncWebServer server(80);    // Create AsyncWebServer instance on port "80"
AsyncWebSocket ws("/ws");     // Create WebSocket instance on URL "/ws"
NtpClient NTP;

uint8_t queryTimer = 5; // seconds
unsigned long lastModbusAnswer = 0;
unsigned long evseQueryTimeOut = 0;

//Loop
unsigned long previousMillis = 0;
unsigned long previousLoopMillis = 0;
unsigned long cooldown = 0;
bool toSetEVSEcurrent = false;
bool toQueryEVSE = false;
bool toSendStatus = false;
bool toReboot = false;

//EVSE Modbus Registers
uint16_t evseAmpsConfig;     //Register 1000
uint16_t evseAmpsOutput;     //Register 1001
uint16_t evseVehicleStatus;  //Register 1002
uint16_t evseAmpsPP;         //Register 1003
uint16_t evseTurnOff;        //Register 1004
uint16_t evseFirmware;       //Register 1005
uint16_t evseState;          //Register 1006

uint16_t evseAmpsAfterboot;  //Register 2000
uint16_t evseModbusEnabled;  //Register 2001
uint16_t evseAmpsMin;        //Register 2002
uint16_t evseAnIn;           //Register 2003
uint16_t evseAmpsPowerOn;    //Register 2004
uint16_t evseReg2005;        //Register 2005
uint16_t evseShareMode;      //Register 2006
uint16_t evsePpDetection;    //Register 2007
uint16_t evseBootFirmware;   //Register 2009

//Settings
bool useSMeter = false;
bool inAPMode = false;
bool inFallbackMode = false;
bool isWifiConnected = false;
String lastUsername = "";
String lastUID = "";
char * deviceHostname = NULL;
uint8_t buttonPin;
char * adminpass = NULL;
int timeZone;
const char * ntpIP = "pool.ntp.org";

//Others
String msg = ""; //WS communication

//////////////////////////////////////////////////////////////////////////////////////////
///////       Auxiliary Functions
//////////////////////////////////////////////////////////////////////////////////////////

String ICACHE_FLASH_ATTR printIP(IPAddress adress) {
  return (String)adress[0] + "." + (String)adress[1] + "." + (String)adress[2] + "." + (String)adress[3];
}

void ICACHE_FLASH_ATTR parseBytes(const char* str, char sep, byte* bytes, int maxBytes, int base) {
  for (int i = 0; i < maxBytes; i++) {
    bytes[i] = strtoul(str, NULL, base);
    str = strchr(str, sep);
    if (str == NULL || *str == '\0') {
      break;
    }
    str++;
  }
}

void ICACHE_RAM_ATTR handleMeterInt() {  //interrupt routine for metering
  if(meterImpMillis < millis()){
    meterImpMillis = millis();
    meterInterrupt += 1;
    numberOfMeterImps ++;
  }
}

void ICACHE_RAM_ATTR updateS0MeterData() {
  if (vehicleCharging){
    currentKW = 3600.0 / float(meterImpMillis - previousMeterMillis) / float(kwhimp / 1000.0) * (float)iFactor ;  //Calculating kW
    previousMeterMillis = meterImpMillis;
    meterImpMillis = meterImpMillis + intLength;
    meterInterrupt = 0;
    meteredKWh = float(numberOfMeterImps) / float(kwhimp / 1000.0) / 1000.0 * float(iFactor);
  }
}

int ICACHE_FLASH_ATTR getChargingTime(){
  uint32_t iTime;
  if(vehicleCharging&&vehicleChargingFlag){
    iTime = millis() - millisStartCharging;
  }
  else {
    iTime = millisStopCharging - millisStartCharging;
  }
  return iTime;
}

void ICACHE_FLASH_ATTR sendStatus() {
  // Getting additional Modbus data
  uint8_t result;
  struct ip_info info;
  FSInfo fsinfo;
  if (!SPIFFS.info(fsinfo)) {
    Serial.print(F("[ WARN ] Error getting info on SPIFFS"));
  }
  DynamicJsonBuffer jsonBuffer12;
  JsonObject& root = jsonBuffer12.createObject();
  root["command"] = "status";
  root["heap"] = ESP.getFreeHeap();
  root["chipid"] = String(ESP.getChipId(), HEX);
  root["cpu"] = ESP.getCpuFreqMHz();
  root["availsize"] = ESP.getFreeSketchSpace();
  root["availspiffs"] = fsinfo.totalBytes - fsinfo.usedBytes;
  root["spiffssize"] = fsinfo.totalBytes;
  root["uptime"] = NTP.getDeviceUptimeString();

  if (inAPMode) {
    wifi_get_ip_info(SOFTAP_IF, &info);
    struct softap_config conf;
    wifi_softap_get_config(&conf);
    root["ssid"] = String(reinterpret_cast<char*>(conf.ssid));
    root["dns"] = printIP(WiFi.softAPIP());
    root["mac"] = WiFi.softAPmacAddress();
  }
  else {
    wifi_get_ip_info(STATION_IF, &info);
    struct station_config conf;
    wifi_station_get_config(&conf);
    root["ssid"] = String(reinterpret_cast<char*>(conf.ssid));
    root["dns"] = printIP(WiFi.dnsIP());
    root["mac"] = WiFi.macAddress();
  }

  IPAddress ipaddr = IPAddress(info.ip.addr);
  IPAddress gwaddr = IPAddress(info.gw.addr);
  IPAddress nmaddr = IPAddress(info.netmask.addr);
  root["ip"] = printIP(ipaddr);
  root["gateway"] = printIP(gwaddr);
  root["netmask"] = printIP(nmaddr);

  // Getting actual Modbus data
  queryEVSE();
  evseNode.clearTransmitBuffer();
  evseNode.clearResponseBuffer();
  result = evseNode.readHoldingRegisters(0x07D0, 10);  // read 10 registers starting at 0x07D0 (2000)
  
  if (result != 0){
    // error occured
    evseVehicleStatus = 0;
    Serial.print("[ ModBus ] Error ");
    Serial.print(result, HEX);
    Serial.println(" occured while getting additional EVSE data");
    //delay(100);
    return;
  }
  else{
    // register successufully read
    if(debug) Serial.println("[ ModBus ] got additional EVSE data successfully ");
    lastModbusAnswer = millis();

    //process answer
    for(int i = 0; i < 10; i++){
      switch(i){
      case 0:
        evseAmpsAfterboot  = evseNode.getResponseBuffer(i);    //Register 2000
        break;
      case 1:
        evseModbusEnabled = evseNode.getResponseBuffer(i);     //Register 2001
        break;
      case 2:
        evseAmpsMin = evseNode.getResponseBuffer(i);           //Register 2002
        break;
      case 3: 
        evseAnIn = evseNode.getResponseBuffer(i);             //Reg 2003
        break;
      case 4:
        evseAmpsPowerOn = evseNode.getResponseBuffer(i);      //Reg 2004
        break;
      case 5:
        evseReg2005 = evseNode.getResponseBuffer(i);          //Reg 2005
        break;
      case 6:
        evseShareMode = evseNode.getResponseBuffer(i);        //Reg 2006
        break;
      case 7:
        evsePpDetection = evseNode.getResponseBuffer(i);       //Register 2007
        break;
      case 9:
        evseBootFirmware = evseNode.getResponseBuffer(i);       //Register 2009
        break;    
      }
    }
  }

  root["evse_amps_conf"] = evseAmpsConfig;          //Reg 1000
  root["evse_amps_out"] = evseAmpsOutput;           //Reg 1001
  root["evse_vehicle_state"] = evseVehicleStatus;   //Reg 1002
  root["evse_pp_limit"] = evseAmpsPP;               //Reg 1003
  root["evse_turn_off"] = evseTurnOff;              //Reg 1004
  root["evse_firmware"] = evseFirmware;             //Reg 1005
  root["evse_state"] = evseState;                   //Reg 1006
  root["evse_amps_afterboot"] = evseAmpsAfterboot;  //Reg 2000
  root["evse_modbus_enabled"] = evseModbusEnabled;  //Reg 2001
  root["evse_amps_min"] = evseAmpsMin;              //Reg 2002
  root["evse_analog_input"] = evseAnIn;             //Reg 2003
  root["evse_amps_poweron"] = evseAmpsPowerOn;      //Reg 2004
  root["evse_2005"] = evseReg2005;                  //Reg 2005
  root["evse_sharing_mode"] = evseShareMode;        //Reg 2006
  root["evse_pp_detection"] = evsePpDetection;      //Reg 2007
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

// Send Scanned SSIDs to websocket clients as JSON object
void ICACHE_FLASH_ATTR printScanResult(int networksFound) {
  DynamicJsonBuffer jsonBuffer13;
  JsonObject& root = jsonBuffer13.createObject();
  root["command"] = "ssidlist";
  JsonArray& scan = root.createNestedArray("list");
  for (int i = 0; i < networksFound; ++i) {
    JsonObject& item = scan.createNestedObject();
    // Print SSID for each network found
    item["ssid"] = WiFi.SSID(i);
    item["bssid"] = WiFi.BSSIDstr(i);
    item["rssi"] = WiFi.RSSI(i);
    item["channel"] = WiFi.channel(i);
    item["enctype"] = WiFi.encryptionType(i);
    item["hidden"] = WiFi.isHidden(i) ? true : false;
  }
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
  WiFi.scanDelete();
}

//////////////////////////////////////////////////////////////////////////////////////////
///////       EVSE Modbus functions
//////////////////////////////////////////////////////////////////////////////////////////
bool ICACHE_FLASH_ATTR queryEVSE(){
  uint8_t result;
  
  evseNode.clearTransmitBuffer();
  evseNode.clearResponseBuffer();
  result = evseNode.readHoldingRegisters(0x03E8, 7);  // read 7 registers starting at 0x03E8 (1000)
  
  if (result != 0){

    evseVehicleStatus = 0;
    Serial.print("[ ModBus ] Error ");
    Serial.print(result, HEX);
    Serial.println(" occured while getting EVSE data - trying again...");
    evseNode.clearTransmitBuffer();
    evseNode.clearResponseBuffer();
    delay(500);
    return false;
  }
  else{
    // register successufully read
    if(debug) Serial.println("[ ModBus ] got EVSE data successfully ");
    lastModbusAnswer = millis();

    //process answer
    for(int i = 0; i < 7; i++){
      switch(i){
      case 0:
        evseAmpsConfig = evseNode.getResponseBuffer(i);     //Register 1000
        break;
      case 1:
        evseAmpsOutput = evseNode.getResponseBuffer(i);     //Register 1001
        break;
      case 2:
        evseVehicleStatus = evseNode.getResponseBuffer(i);   //Register 1002
        break;
      case 3:
        evseAmpsPP = evseNode.getResponseBuffer(i);          //Register 1003
        break;
      case 4:
        evseTurnOff = evseNode.getResponseBuffer(i);          //Register 1004
        break;
      case 5:
        evseFirmware = evseNode.getResponseBuffer(i);        //Register 1005
        break;
      case 6:
        evseState = evseNode.getResponseBuffer(i);      //Register 1006
        break;
      }
    }
    if (evseVehicleStatus == 0){
      evseStatus = 0; //modbus communication failed
    }
    if (evseState == 3){     //EVSE not Ready
        if (evseVehicleStatus == 2 ||
            evseVehicleStatus == 3 ){
          evseStatus = 2; //vehicle detected
        }
        else{
          evseStatus = 1; // EVSE deactivated
        }
        if (vehicleCharging == true){   //vehicle interrupted charging
          vehicleCharging = false;
          millisStopCharging = millis();
          if(debug) Serial.println("Vehicle interrupted charging");
        }
        return true;
    }
    if (evseVehicleStatus == 1){
      evseStatus = 1;  // ready
    }
    else if (evseVehicleStatus == 2){
      evseStatus = 2; //vehicle detected
    }
    else if (evseVehicleStatus == 3){
      evseStatus = 3; //charging
      if (vehicleCharging == false) {
        millisStartCharging = millis();
        meteredKWh = 0;
        numberOfMeterImps = 0;
        vehicleCharging = true;
      }
    }
    return true;
  }
}

bool ICACHE_FLASH_ATTR activateEVSE() {
  static uint16_t iTransmit;
  if(debug) Serial.println("[ ModBus ] Query Modbus before activating EVSE");
  queryEVSE();
  
  if (evseState == 3 &&
      evseVehicleStatus != 0){    //no modbus error occured
      iTransmit = 0;         
      
    uint8_t result;
    evseNode.clearTransmitBuffer();
    evseNode.setTransmitBuffer(0, iTransmit); // set word 0 of TX buffer (bits 15..0)
    result = evseNode.writeMultipleRegisters(0x07D5, 1);  // write register 0x07D5 (2005)
  
    if (result != 0){
      // error occured
      Serial.print("[ ModBus ] Error ");
      Serial.print(result, HEX);
      Serial.println(" occured while activating EVSE - trying again...");
      return false;
    }
    else{
      // register successufully written
      if(debug) Serial.println("[ ModBus ] EVSE successfully activated");
      numberOfMeterImps = 0;
      sendEVSEdata();
      return true;
    }
  }
  else if (evseVehicleStatus != 0){
    return true;
  }
  return false;
}

bool ICACHE_FLASH_ATTR setEVSEcurrent(){  // telegram 1: write EVSE current
  //New ModBus Master Library
  uint8_t result;
  
  evseNode.clearTransmitBuffer();
  evseNode.setTransmitBuffer(0, currentToSet); // set word 0 of TX buffer (bits 15..0)
  result = evseNode.writeMultipleRegisters(0x03E8, 1);  // write register 0x03E8 (1000 - Actual configured amps value)
  
  if (result != 0){
    // error occured
    Serial.print("[ ModBus ] Error ");
    Serial.print(result, HEX);
    Serial.println(" occured while setting current in EVSE - trying again...");
    return false;
  }
  else{   
    // register successufully written
    if(debug) Serial.println("[ ModBus ] Current successfully set");
    evseAmpsConfig = currentToSet;  //foce update in WebUI
    sendEVSEdata();               //foce update in WebUI
    toSetEVSEcurrent = false;
    return true;
  }
}

bool ICACHE_FLASH_ATTR setEVSERegister(uint16_t reg, uint16_t val){
  uint8_t result;
  evseNode.clearTransmitBuffer();
  evseNode.setTransmitBuffer(0, val); // set word 0 of TX buffer (bits 15..0)
  result = evseNode.writeMultipleRegisters(reg, 1);  // write given register
  
  if (result != 0){
    // error occured
    Serial.print("[ ModBus ] Error ");
    Serial.print(result, HEX);
    Serial.println(" occured while setting EVSE Register " + (String)reg + " to " + (String)val);
    return false;
  }
  else{   
    // register successufully written
    if(debug) Serial.println("[ ModBus ] Register " + (String)reg + " successfully set to " + (String)val);
    return true;
  }
}

//////////////////////////////////////////////////////////////////////////////////////////
///////       Websocket Functions
//////////////////////////////////////////////////////////////////////////////////////////
void ICACHE_FLASH_ATTR pushSessionTimeOut(){
  // push "TimeOut" to evse.htm!
  // Encode a JSON Object and send it to All WebSocket Clients
  DynamicJsonBuffer jsonBuffer15;
  JsonObject& root = jsonBuffer15.createObject();
  root["command"] = "sessiontimeout";
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
  if(debug) Serial.println("[ WebSocket ] TimeOut sent to browser!");
}

void ICACHE_FLASH_ATTR sendEVSEdata(){
  if (evseSessionTimeOut == false){
    DynamicJsonBuffer jsonBuffer9;
    JsonObject& root = jsonBuffer9.createObject();
    root["command"] = "getevsedata";
    root["evse_vehicle_state"] = evseStatus;
    root["evse_current_limit"] = evseAmpsConfig;
    root["evse_current"] = String(currentKW, 2);
    root["evse_charging_time"] = getChargingTime();
    root["evse_charged_kwh"] = String(meteredKWh, 2);
    root["evse_charged_amount"] = String((meteredKWh * float(iPrice) / 100.0), 2);
    root["evse_maximum_current"] = maxinstall;
    if(meteredKWh == 0.0){
      root["evse_charged_mileage"] = "0.0";  
    }
    else{
      root["evse_charged_mileage"] = String((meteredKWh * 100.0 / consumption), 1);
    }
    root["ap_mode"] = inAPMode;
    size_t len = root.measureLength();
    AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len);
    if (buffer) {
      root.printTo((char *)buffer->get(), len + 1);
     ws.textAll(buffer);
    }
  }
}

void ICACHE_FLASH_ATTR sendTime() {
  DynamicJsonBuffer jsonBuffer10;
  JsonObject& root = jsonBuffer10.createObject();
  root["command"] = "gettime";
  root["epoch"] = now();
  root["timezone"] = timeZone;
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len);
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

void ICACHE_FLASH_ATTR onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_ERROR) {
    if(debug) Serial.printf("[ WARN ] WebSocket[%s][%u] error(%u): %s\r\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  }
  else if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len) {
      //the whole message is in a single frame and we got all of it's data
      if(debug)Serial.println("[ Websocket ] single Frame - all data is here!");
      for (size_t i = 0; i < info->len; i++) {
        msg += (char) data[i];
      }
      DynamicJsonBuffer jsonBuffer8;
      JsonObject& root = jsonBuffer8.parseObject(msg);
      if (!root.success()) {
        if(debug) Serial.println(F("[ WARN ] Couldn't parse WebSocket message"));
        msg = "";
        return;
      }
      processWsEvent(root, client);
    }
    else{
      //message is comprised of multiple frames or the frame is split into multiple packets
      if(debug)Serial.println("[ Websocket ] more than one Frame!");
      for (size_t i = 0; i < len; i++) {
        msg += (char) data[i];
      }
      if(info->final && (info->index + len) == info->len){
        DynamicJsonBuffer jsonBuffer8;
        JsonObject& root = jsonBuffer8.parseObject(msg);
        if (!root.success()) {
          if(debug) Serial.println(F("[ WARN ] Couldn't parse WebSocket message"));
          msg = "";
          return;
        }
        root.prettyPrintTo(Serial);
        processWsEvent(root, client);
      }
    }
  }
}

void ICACHE_FLASH_ATTR processWsEvent(JsonObject& root, AsyncWebSocketClient * client){
  const char * command = root["command"];
  if (strcmp(command, "remove")  == 0) {
    const char* uid = root["uid"];
    String filename = "/P/";
    filename += uid;
    SPIFFS.remove(filename);
  }
  else if (strcmp(command, "configfile")  == 0) {
    if(debug) Serial.println("[ SYSTEM ] Try to update config.json...");
    File f = SPIFFS.open("/config.json", "w+");
    if (f) {
      root.prettyPrintTo(f);
      //f.print(msg);
      f.close();
      if(debug) Serial.println("[ SYSTEM ] Success - going to reboot now");
      ESP.reset();
    }
    else{
      if(debug) Serial.println("[ SYSTEM ] Could not save config.json");
    }
  }
  else if (strcmp(command, "status")  == 0) {
    toSendStatus = true;
  }
  else if (strcmp(command, "scan")  == 0) {
    WiFi.scanNetworksAsync(printScanResult, true);
  }
  else if (strcmp(command, "gettime")  == 0) {
    sendTime();
  }
  else if (strcmp(command, "settime")  == 0) {
    unsigned long t = root["epoch"];
    setTime(t);
    sendTime();
  }
  else if (strcmp(command, "getconf")  == 0) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      size_t len = configFile.size();
      AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
      if (buffer) {
        configFile.readBytes((char *)buffer->get(), len + 1);
        ws.textAll(buffer);
      }
      configFile.close();
    }
  }
  else if (strcmp(command, "getevsedata") == 0){
    sendEVSEdata();
    evseQueryTimeOut = millis() + 10000; //Timeout for pushing data in loop
    evseSessionTimeOut = false;
    if(debug) Serial.println("[ WebSocket ] Data sent to UI");
    toQueryEVSE = true;
  }
  else if (strcmp(command, "setcurrent") == 0){
    currentToSet = root["current"];
    if(debug) Serial.print("[ WebSocket ] Call setEVSECurrent() ");
    if(debug) Serial.println(currentToSet);
    toSetEVSEcurrent = true;
  }
  else if (strcmp(command, "setevsereg") == 0){
    uint16_t reg = atoi(root["register"]);
    uint16_t val = atoi(root["value"]);
    setEVSERegister(reg, val);
  }
  else if (strcmp(command, "factoryreset") == 0){
    SPIFFS.remove("/config.json");
    SPIFFS.remove("/latestlog.json");
  }
  msg = "";
}

//////////////////////////////////////////////////////////////////////////////////////////
///////       Setup Functions
//////////////////////////////////////////////////////////////////////////////////////////
bool ICACHE_FLASH_ATTR connectSTA(const char* ssid, const char* password, byte bssid[6]) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password, 0, bssid);
  Serial.print(F("[ INFO ] Trying to connect WiFi: "));
  Serial.print(ssid);

  unsigned long now = millis();
  uint8_t timeout = 20;
  do {
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
    delay(500);
    if(debug) Serial.print(F("."));
  }
  while (millis() - now < timeout * 1000);
  if (WiFi.status() == WL_CONNECTED) {
    isWifiConnected = true;
    return true;
  }
  else {
    if(debug) Serial.println();
    if(debug) Serial.println(F("[ WARN ] Couldn't connect in time"));
    return false;
  }
}

bool ICACHE_FLASH_ATTR startAP(const char * ssid, const char * password = NULL){
  //WiFi.disconnect(true);
  inAPMode = true;
  WiFi.mode(WIFI_AP);
  Serial.print(F("[ INFO ] Configuring access point... "));
  bool success = WiFi.softAP(ssid, password);
  Serial.println(success ? "Ready" : "Failed!");
  // Access Point IP
  IPAddress myIP = WiFi.softAPIP();
  Serial.print(F("[ INFO ] AP IP address: "));
  Serial.println(myIP);
  Serial.printf("[ INFO ] AP SSID: %s\n", ssid);
  isWifiConnected = success;
  return success;
}

bool ICACHE_FLASH_ATTR loadConfiguration() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    if(debug) Serial.println(F("[ WARN ] Failed to open config file"));
    return false;
  }
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer14;
  JsonObject& json = jsonBuffer14.parseObject(buf.get());
  if (!json.success()) {
    Serial.println(F("[ WARN ] Failed to parse config file"));
    return false;
  }
  Serial.println(F("[ INFO ] Config file found"));
  json.prettyPrintTo(Serial);
  Serial.println();

  if (json.containsKey("debug")){
    debug = json["debug"];
  }
  if(debug){
    Serial.println("[ DEBUGGER ] Debug Mode: ON!");
  }
  else{
    Serial.println("[ DEBUGGER ] Debug Mode: OFF!");
  }
  
  useSMeter = false;
  if(json.containsKey("meter") &&
      json.containsKey("metertype")){
    iPrice = json["price"];
    if(json["metertype"] == "S0"){
      if(json.containsKey("intpin") &&
          json.containsKey("kwhimp") &&
          json.containsKey("meterphase")){
        meterPin = json["intpin"];
        kwhimp = json["kwhimp"];
        meterphase = json["meterphase"];
        useSMeter = true;
        if(json.containsKey("implen")){
          intLength = json["implen"];
          intLength += 3;
        }
        else{
          intLength = 33;
        }
        if(debug) Serial.println(F("[ INFO ] S0 Meter is configured"));
      }
    }
  }

  if(json.containsKey("avgconsumption")){
    String sConsumption = json["avgconsumption"];
    consumption = strtof((sConsumption).c_str(),0);
  }
  
  if(json.containsKey("ntpIP")){
    ntpIP = json["ntpIP"];
  }
  
  const char * l_hostname = json["hostnm"];
  free(deviceHostname);
  deviceHostname = strdup(l_hostname);
  
  const char * bssidmac = json["bssid"];
  byte bssid[6];
  parseBytes(bssidmac, ':', bssid, 6, 16);
  WiFi.hostname(deviceHostname);

  if (!MDNS.begin(deviceHostname)) {
    Serial.println("Error setting up MDNS responder!");
  }
  MDNS.addService("http", "tcp", 80);

  if(json.containsKey("timezone")){
    timeZone = json["timezone"];
  }
  
  iFactor = json["factor"];
  maxinstall = json["maxinstall"];

  const char * ssid = json["ssid"];
  const char * password = json["pswd"];
  int wmode = json["wmode"];
  adminpass = strdup(json["adminpwd"]);

  ws.setAuthentication("admin", adminpass);
  server.addHandler(new SPIFFSEditor("admin", adminpass));

  queryEVSE();
  
  vehicleCharging = false;
  activateEVSE();

  if (wmode == 1) {
    if(debug) Serial.println(F("[ INFO ] SimpleEVSE Wifi is running in AP Mode "));
    WiFi.disconnect(true);
    return startAP(ssid, password);
  }
  if (!connectSTA(ssid, password, bssid)) {
    return false;
  }

  if(json.containsKey("staticip") &&
      json.containsKey("ip") &&
      json.containsKey("subnet") &&
      json.containsKey("gateway") &&
      json.containsKey("dns")){
    if (json["staticip"] == true){
      const char * clientipch = json["ip"];
      const char * subnetch = json["subnet"];
      const char * gatewaych = json["gateway"];
      const char * dnsch = json["dns"];

      IPAddress clientip;
      IPAddress subnet;
      IPAddress gateway;
      IPAddress dns;

      clientip.fromString(clientipch);
      subnet.fromString(subnetch);
      gateway.fromString(gatewaych);
      dns.fromString(dnsch);
      
      WiFi.config(clientip, gateway, subnet, dns);
    }
  }

  Serial.println();
  Serial.print(F("[ INFO ] Client IP address: "));
  Serial.println(WiFi.localIP());

//Check internet connection
  delay(100);
    if(debug) Serial.print("[ NTP ] NTP Server - set up NTP"); 
    const char * ntpserver = ntpIP;
    IPAddress timeserverip;
    WiFi.hostByName(ntpserver, timeserverip);
    String ip = printIP(timeserverip);
    if(debug) Serial.println(" IP: " + ip);
    NTP.Ntp(ntpIP, timeZone, 3600);   //use NTP Server, timeZone, update every x sec
//  }
}

void ICACHE_FLASH_ATTR setWebEvents(){
  server.on("/index.htm", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "text/html", WEBSRC_INDEX_HTM, WEBSRC_INDEX_HTM_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "text/javascript", WEBSRC_SCRIPT_JS, WEBSRC_SCRIPT_JS_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });
  
  server.on("/fonts/glyph.woff", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "font/woff", WEBSRC_GLYPH_WOFF, WEBSRC_GLYPH_WOFF_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/fonts/glyph.woff2", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "font/woff", WEBSRC_GLYPH_WOFF2, WEBSRC_GLYPH_WOFF2_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/required/required.css", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "text/css", WEBSRC_REQUIRED_CSS, WEBSRC_REQUIRED_CSS_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/required/required.js", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "text/javascript", WEBSRC_REQUIRED_JS, WEBSRC_REQUIRED_JS_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/status_charging.svg", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "image/svg+xml", WEBSRC_STATUS_CHARGING_SVG, WEBSRC_STATUS_CHARGING_SVG_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/status_detected.svg", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "image/svg+xml", WEBSRC_STATUS_DETECTED_SVG, WEBSRC_STATUS_DETECTED_SVG_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/status_ready.svg", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse * response = request->beginResponse_P(200, "image/svg+xml", WEBSRC_STATUS_READY_SVG, WEBSRC_STATUS_READY_SVG_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

    //
    //  HTTP API
    //
  
  //getParameters
  server.on("/getParameters", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonBuffer jsonBuffer17;
    JsonObject& item = jsonBuffer17.createObject();
    item["vehicleState"] = evseStatus;
    item["actualCurrent"] = evseAmpsConfig;
    item["actualPower"] =  float(int((currentKW + 0.005) * 100.0)) / 100.0;
    item["duration"] = getChargingTime();
    item["energy"] = float(int((meteredKWh + 0.005) * 100.0)) / 100.0;
    item["mileage"] = float(int(((meteredKWh * 100.0 / consumption) + 0.05) * 10.0)) / 10.0;
    item["meterReading"] = float(int((meteredKWh + 0.005) * 100.0)) / 100.0;;
    if(meterphase == 1){
      float fCurrent = float(int((currentKW / float(iFactor) / 0.227 + 0.005) * 100.0) / 100.0);
      if(iFactor == 1){
        item["currentP1"] = fCurrent;
        item["currentP2"] = 0.0;
        item["currentP3"] = 0.0;
      }
      else if(iFactor == 2){
        item["currentP1"] = fCurrent;
        item["currentP2"] = fCurrent;
        item["currentP3"] = 0.0;
      }
      else if(iFactor == 3){
        item["currentP1"] = fCurrent;
        item["currentP2"] = fCurrent;
        item["currentP3"] = fCurrent;
      }
    }
    else{
      float fCurrent = float(int((currentKW / 0.227 / float(iFactor) / 3.0 + 0.005) * 100.0) / 100.0);
      item["currentP1"] = fCurrent;
      item["currentP2"] = fCurrent;
      item["currentP3"] = fCurrent;
    }

    item.printTo(*response);
    request->send(response);
  });
  
  //setCurrent (0,233)
  server.on("/setCurrent", HTTP_GET, [](AsyncWebServerRequest * request) {
      awp = request->getParam(0);
      if(awp->name() == "current"){
        if(atoi(awp->value().c_str()) <= maxinstall && atoi(awp->value().c_str()) >= 6 ){
          currentToSet = atoi(awp->value().c_str());
          if(setEVSEcurrent()){
            request->send(200, "text/plain", "S0_set current to A");
          }          
          else{
            request->send(200, "text/plain", "E0_could not set current - internal error");
          }
        }
        else{
          request->send(200, "text/plain", ("E1_could not set current - give a value between 6 and " + (String)maxinstall));
        }
      }
      else{
        request->send(200, "text/plain", "E2_could not set current - wrong parameter");
      }
  });
  

    server.on("/evseHost", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonBuffer jsonBuffer19;
    JsonObject& root = jsonBuffer19.createObject();
    root["type"] = "evseHost";
    JsonArray& list = root.createNestedArray("list");
    DynamicJsonBuffer jsonBuffer20;
    JsonObject& item = jsonBuffer20.createObject();

    struct ip_info info;
    if (inAPMode) {
      wifi_get_ip_info(SOFTAP_IF, &info);
      struct softap_config conf;
      wifi_softap_get_config(&conf);
      item["ssid"] = String(reinterpret_cast<char*>(conf.ssid));
      item["dns"] = printIP(WiFi.softAPIP());
      item["mac"] = WiFi.softAPmacAddress();
    }
    else {
      wifi_get_ip_info(STATION_IF, &info);
      struct station_config conf;
      wifi_station_get_config(&conf);
      item["ssid"] = String(reinterpret_cast<char*>(conf.ssid));
      item["dns"] = printIP(WiFi.dnsIP());
      item["mac"] = WiFi.macAddress();
    }

    IPAddress ipaddr = IPAddress(info.ip.addr);
    IPAddress gwaddr = IPAddress(info.gw.addr);
    IPAddress nmaddr = IPAddress(info.netmask.addr);
    item["ip"] = printIP(ipaddr);
    item["gateway"] = printIP(gwaddr);
    item["netmask"] = printIP(nmaddr);

    list.add(item);
    root.printTo(*response);
    request->send(response);
  });
  
}

void ICACHE_FLASH_ATTR fallbacktoAPMode() {
  WiFi.disconnect(true);
  if(debug) Serial.println(F("[ INFO ] SimpleEVSE Wifi is running in Fallback AP Mode"));
  uint8_t macAddr[6];
  WiFi.softAPmacAddress(macAddr);
  char ssid[16];
  sprintf(ssid, "EVSE-WiFi-%02x%02x%02x", macAddr[3], macAddr[4], macAddr[5]);
  if(adminpass == NULL)adminpass = "admin";
  isWifiConnected = startAP(ssid);
  void setWebEvents();
  inFallbackMode = true;
}

void ICACHE_FLASH_ATTR startWebserver() {
  // Start WebSocket Plug-in and handle incoming message on "onWsEvent" function
  server.addHandler(&ws);
  ws.onEvent(onWsEvent);
  server.onNotFound([](AsyncWebServerRequest * request) {
    AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", "Not found");
    request->send(response);
  });

  // Simple Firmware Update Handler
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest * request) {
    toReboot = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", toReboot ? "OK" : "FAIL");
    response->addHeader("Connection", "close");
    request->send(response);
  }, [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      if(debug) Serial.printf("[ UPDT ] Firmware update started: %s\n", filename.c_str());
      Update.runAsync(true);
      if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
        Update.printError(Serial);
      }
    }
    if (!Update.hasError()) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
    }
    if (final) {
      if (Update.end(true)) {
        if(debug) Serial.printf("[ UPDT ] Firmware update finished: %uB\n", index + len);
      } else {
        Update.printError(Serial);
      }
    }
  });

  setWebEvents();

  // HTTP basic authentication
  server.on("/login", HTTP_GET, [](AsyncWebServerRequest * request) {
      if (!request->authenticate("admin", adminpass)) {
          return request->requestAuthentication();
      }
      request->send(200, "text/plain", "Success");
  });
  
  server.rewrite("/", "/index.htm");
  server.begin();
}

//////////////////////////////////////////////////////////////////////////////////////////
///////       Setup
//////////////////////////////////////////////////////////////////////////////////////////
void ICACHE_FLASH_ATTR setup() {
  Serial.begin(9600);
  delay(1000);
  pinMode(D0, INPUT_PULLDOWN_16);
  
  SPIFFS.begin();
  sSerial.begin(9600);
  evseNode.begin(1, sSerial);
  
  if (!loadConfiguration()) {
    fallbacktoAPMode();
  }
  if (useSMeter){
    pinMode(meterPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(meterPin), handleMeterInt, FALLING);
    if(debug) Serial.println("[ Meter ] Use GPIO 0-15 with Pull-Up");
  }
  now();
  startWebserver();
  if(debug) Serial.println("End of setup routine");
}

//////////////////////////////////////////////////////////////////////////////////////////
///////       Loop
//////////////////////////////////////////////////////////////////////////////////////////
void ICACHE_RAM_ATTR loop() {
  unsigned long currentMillis = millis();
  unsigned long deltaTime = currentMillis - previousLoopMillis;
  unsigned long uptime = NTP.getUptimeSec();
  previousLoopMillis = currentMillis;
  
  if (inFallbackMode && uptime > 600){
    toReboot = true;
  }
  if (toReboot) {
    if(debug) Serial.println(F("[ UPDT ] Rebooting..."));
    delay(100);
    ESP.restart();
  }
  if ((currentMillis > ( lastModbusAnswer + 3000)) && //Update Modbus data every 3000ms and send data to WebUI
        toQueryEVSE == true &&
        evseSessionTimeOut == false) {
    queryEVSE();
    sendEVSEdata();
  }
  else if (currentMillis > evseQueryTimeOut &&    //Setting timeout for Evse poll / push to ws
          evseSessionTimeOut == false){
    evseSessionTimeOut = true;
    pushSessionTimeOut();
  }
  if(currentMillis > lastModbusAnswer + ( queryTimer * 1000 ) && evseSessionTimeOut == true){ //Query modbus every x seconds, when webinterface is not shown
    queryEVSE();
  }
  if (meterInterrupt != 0){
    updateS0MeterData();
  }
  if (useSMeter && previousMeterMillis < millis() - (meterTimeout * 1000)) {  //Timeout when there is less than ~300 watt power consuption -> 6 sec of no interrupt from meter
    if(previousMeterMillis != 0){
      currentKW = 0.0;
    }
  }
  if (toSetEVSEcurrent){
    setEVSEcurrent();
  }
  if (toSendStatus == true){
    sendStatus();
    toSendStatus = false;
  }
}
