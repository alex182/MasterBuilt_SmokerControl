
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <max6675.h>
#include <Wire.h>
#include <AsyncMqttClient.h>
#include <Ticker.h>
#include <ArduinoOTA.h>

//mqtt configs
#define MQTT_HOST IPAddress(192,168,1,145)
#define MQTT_PORT 1883
#define mqtt_user ""
#define mqtt_password ""

#define MQTT_FOOD_PUB_TEMP "esp/food/current/temperature"
#define MQTT_SMOKER_PUB_TEMP "esp/smoker/current/temperature"
#define MQTT_FOOD_PUB_TARGET_TEMP "esp/food/target/temperature"
#define MQTT_SMOKER_PUB_TARGET_TEMP "esp/smoker/target/temperature"

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;

// Replace with your network credentials
const char* ssid     = "";
const char* password = "";
const char* hostName = "smokerControl";

IPAddress local_IP(192,168,4,22);
IPAddress gateway(192,168,4,9);
IPAddress subnet(255,255,255,0);

//thermocouple configs
const int ktcSO = 12;
const int ktcCLK = 14;
const int smokerCS = 13;
const int foodCS = 0;

float smokerTempProbeReading = 0;
float foodTempProbeReading = 0;

MAX6675 smoker(ktcCLK, smokerCS, ktcSO);
MAX6675 foodProbe(ktcCLK, foodCS, ktcSO);

//heating element configs
int heatingElement = 2;
bool heatingElementOn = false; 

int smokerTargetTemp = 225;
int foodTargetTemp = 203;

//input params to set targets
const char* targetFoodTempInputParam = "targetFoodTempTb";
const char* targetSmokerTempInputParam = "targetSmokerTempTb";

//website body
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
 <html>
    <head>
      <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
        <link rel=\"icon\" href=\"data:,\">
        <style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}</style>
    </head>
        <script type='text/JavaScript'>function AutoRefresh(t){setTimeout(location.reload(true, t))}</script>
        <body>
          <h1>Smoker Control
          </h1>
          
          %TEMPPLACEHOLDER%

          <form action='/get'
              <div>Set Target Food Temp: </div>
              <input type='text' name='targetFoodTempTb'>
              <input type='submit' value='Submit'>
          </form>
          <br><br>
          <form action='/get'
              <div>Set Target Smoker Temp: </div>
              <input type='text' name='targetSmokerTempTb'>
              <input type='submit' value='Submit'>
          </form>
          <br><br>
        </body>
  </html>)rawliteral";

// Set web server port number to 80
AsyncWebServer  server(80);

//mqtt stuff
void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
   mqttClient.setCredentials("quser","quser");

   int connectAttempts = 0;
   while (!mqttClient.connected() && connectAttempts <= 20) {
        connectAttempts++; 
    // Attempt to connect
    Serial.print("Attempting to connect to MQTT broker ");
    Serial.println(connectAttempts); 
    mqttClient.connect();

    // Wait some time to space out connection requests
    delay(3000);
   }

   Serial.println("Connected to MQTT broker");
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);
}

void onMqttPublish(uint16_t packetId) {
  Serial.print("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");

  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

String processor(const String& var){
  String replaceStrings = "";
  if(var == "TEMPPLACEHOLDER"){
    replaceStrings += "<p>Current Smoker Temp: "+String(smokerTempProbeReading)+"</p>";
    replaceStrings +=  "<p>Current Food Temp: "+String(foodTempProbeReading)+"</p>";
    replaceStrings +=  "<p>Current Food Target Temp: "+String(foodTargetTemp)+"</p>";
    replaceStrings +=  "<p>Current Smoker Target Temp: "+String(smokerTargetTemp)+"</p>";
    replaceStrings +=  "<p>Current Heating Element State: "+String(heatingElementOn)+"</p>";
  }
  
  return replaceStrings;
}

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(heatingElement, OUTPUT);

  digitalWrite(heatingElement, HIGH);

  // Connect to Wi-Fi network with SSID and password
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  int wifiConnectAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiConnectAttempts < 100) {
    wifiConnectAttempts+=1; 
    delay(1000);
    Serial.print("Attempt: ");
    Serial.println(wifiConnectAttempts);
  }

  if(WiFi.status() != WL_CONNECTED){
      Serial.println("");
      Serial.println("Could not connect to wifi, going into AP mode...");

      Serial.println(WiFi.softAPConfig(local_IP, gateway, subnet) ? "Ready" : "Failed!");

      Serial.println(WiFi.softAP("Smoker_AP") ? "Ready" : "Failed!");
    }
    else{
        // Print local IP address and start web server
        Serial.println("");
        Serial.println("WiFi connected.");
        Serial.println("IP address: ");
        Serial.println(WiFi.localIP());

        mqttClient.setServer(MQTT_HOST, MQTT_PORT);
        connectToMqtt();

        mqttClient.onConnect(onMqttConnect);
        mqttClient.onDisconnect(onMqttDisconnect);
        mqttClient.onPublish(onMqttPublish);
    }

    WiFi.hostname(hostName);
    Serial.println("HostName: ");
    Serial.println(hostName);

    // Send web page with input fields to client
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send_P(200, "text/html", index_html,processor);
    });

  //capture events from webpage
  server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
    String inputMessage;
    String inputParam;
    
    if (request->hasParam(targetFoodTempInputParam)) {
      inputMessage = request->getParam(targetFoodTempInputParam)->value();
      inputParam = targetFoodTempInputParam;
      foodTargetTemp = inputMessage.toInt();
      
      Serial.print("foodTargetTemp updated to: ");
      Serial.println(foodTargetTemp);
    }
    else if (request->hasParam(targetSmokerTempInputParam)) {
      inputMessage = request->getParam(targetSmokerTempInputParam)->value();
      inputParam = targetSmokerTempInputParam;

      smokerTargetTemp = inputMessage.toInt();
      
      Serial.print("smokerTargetTemp updated to: ");
      Serial.println(smokerTargetTemp);
    }
    else {
      inputMessage = "No message sent";
      inputParam = "none";
    }

    Serial.println(inputMessage);    
    request->send_P(200, "text/html", index_html,processor);
  });


  //OTA Setup
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  
  server.begin();
}

void loop(){

  delay(5000);
  smokerTempProbeReading = smoker.readFahrenheit();
  foodTempProbeReading = foodProbe.readFahrenheit();

  if(smokerTempProbeReading < smokerTargetTemp)
  {
    Serial.print("Heating element on");
    digitalWrite(LED_BUILTIN, HIGH);
    digitalWrite(heatingElement, HIGH);
    heatingElementOn = true;
  }
  else{
    Serial.print("Heating element off");
    digitalWrite(LED_BUILTIN, LOW);
    digitalWrite(heatingElement, LOW);
    heatingElementOn = false; 
  }

  Serial.print("Smoker Temp: ");
  Serial.println(smokerTempProbeReading);

  Serial.print("Food Temp: ");
  Serial.println(foodTempProbeReading);

  if(WiFi.status() == WL_CONNECTED && mqttClient.connected()){
    uint16_t smokerTargetTempPacket = mqttClient.publish(MQTT_SMOKER_PUB_TARGET_TEMP , 1, true, String(smokerTargetTemp).c_str());                            
    Serial.printf("Publishing on topic %s at QoS 1, packetId: %i ", MQTT_SMOKER_PUB_TARGET_TEMP , smokerTargetTempPacket);
    Serial.printf("Message: %.2f \n", smokerTargetTemp);
  
    uint16_t smokerTempPacket = mqttClient.publish(MQTT_SMOKER_PUB_TEMP, 1, true, String(smokerTempProbeReading).c_str());                            
    Serial.printf("Publishing on topic %s at QoS 1, packetId: %i ", MQTT_SMOKER_PUB_TEMP, smokerTempPacket);
    Serial.printf("Message: %.2f \n", smokerTempProbeReading);
    
    uint16_t foodTargetTempPacket = mqttClient.publish(MQTT_FOOD_PUB_TARGET_TEMP , 1, true, String(foodTargetTemp).c_str());                            
    Serial.printf("Publishing on topic %s at QoS 1, packetId: %i ", MQTT_FOOD_PUB_TARGET_TEMP , foodTargetTempPacket);
    Serial.printf("Message: %.2f \n", foodTargetTemp);
    
    uint16_t foodTempPacket = mqttClient.publish(MQTT_FOOD_PUB_TEMP, 1, true, String(foodTempProbeReading).c_str());                            
    Serial.printf("Publishing on topic %s at QoS 1, packetId: %i ", MQTT_FOOD_PUB_TEMP, foodTempPacket);
    Serial.printf("Message: %.2f \n", foodTempProbeReading);
  }
  else{
    
    int wifiConnectAttempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifiConnectAttempts < 50) {
    wifiConnectAttempts+=1; 
      delay(1000);
      Serial.print("Attempt: ");
      Serial.println(wifiConnectAttempts);
    }
  }

  ArduinoOTA.handle();
}
