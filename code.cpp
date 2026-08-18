#include <WiFi.h>
#include <Arduino.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include <Server_Side_RPC.h>
#include <Attribute_Request.h>
#include <Shared_Attribute_Update.h>
#include <array>
#include <DHT11.h>

#define THINGSBOARD_ENABLE_DYNAMIC 1

DHT11 dht11(2);

// State Setup
bool systemState = 0;
int systemDelay = 0;
unsigned long previousMillis = 0;

// Pin Setup
const int flamePin = 5; 
const int dhtPin = 2;
const int buzPin = 27;
const int LEDred = 32;
const int LEDgreen = 33;
const int LEDblue = 25;
const int butI = 19;

// Stuff Copy-pasted from the thingsboard website when connecting the ESP32
constexpr char WIFI_SSID[] = "WE_944052";
constexpr char WIFI_PASSWORD[] = "234239b4";

constexpr char TOKEN[] = "xDJWh2E2Eo6ZW64mWDzK";
constexpr char THINGSBOARD_SERVER[] = "mqtt.eu.thingsboard.cloud";
constexpr uint16_t THINGSBOARD_PORT = 1883;

constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;
constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 256U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 256U;
constexpr uint64_t REQUEST_TIMEOUT_MICROSECONDS = 5000ULL * 1000ULL;

constexpr size_t MAX_ATTRIBUTES = 3U;

constexpr const char BLINKING_INTERVAL_ATTR[] = "blinkingInterval";
constexpr const char LED_MODE_ATTR[] = "ledMode";
constexpr const char LED_STATE_ATTR[] = "ledState";

constexpr uint32_t TELEMETRY_SEND_INTERVAL_MS = 2000U;

constexpr uint16_t BLINKING_INTERVAL_MS_MIN = 10U;
constexpr uint16_t BLINKING_INTERVAL_MS_MAX = 60000U;

volatile bool attributesChanged = false;
volatile int ledMode = 0;
volatile bool ledState = false;
volatile uint16_t blinkingInterval = 1000U;

uint32_t previousStateChange = 0U;
uint32_t previousDataSend = 0U;

constexpr std::array<const char*, 2U> SHARED_ATTRIBUTES_LIST = {
  LED_STATE_ATTR,
  BLINKING_INTERVAL_ATTR
};

constexpr std::array<const char*, 1U> CLIENT_ATTRIBUTES_LIST = {
  LED_MODE_ATTR
};

WiFiClient wifiClient;
Arduino_MQTT_Client mqttClient(wifiClient);

Server_Side_RPC<3U, 5U> rpc;
Attribute_Request<2U, MAX_ATTRIBUTES> attr_request;
Shared_Attribute_Update<3U, MAX_ATTRIBUTES> shared_update;

const std::array<IAPI_Implementation*, 3U> apis = {
  &rpc,
  &attr_request,
  &shared_update
};

ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

static bool rpc_subscribed = false;
static bool shared_attrs_subscribed = false;
static bool shared_request_sent = false;
static bool client_request_sent = false;

void processSetLedMode(const JsonVariantConst &data, JsonDocument &response);
void processSharedAttributes(const JsonObjectConst &data);
void processClientAttributes(const JsonObjectConst &data);
void requestTimedOut();

const std::array<RPC_Callback, 1U> callbacks = {
  RPC_Callback{ "setLedMode", processSetLedMode }
};

const Shared_Attribute_Callback<MAX_ATTRIBUTES> attributes_callback(
  &processSharedAttributes,
  SHARED_ATTRIBUTES_LIST.cbegin(),
  SHARED_ATTRIBUTES_LIST.cend()
);

const Attribute_Request_Callback<MAX_ATTRIBUTES> attribute_shared_request_callback(
  &processSharedAttributes,
  REQUEST_TIMEOUT_MICROSECONDS,
  &requestTimedOut,
  SHARED_ATTRIBUTES_LIST
);

const Attribute_Request_Callback<MAX_ATTRIBUTES> attribute_client_request_callback(
  &processClientAttributes,
  REQUEST_TIMEOUT_MICROSECONDS,
  &requestTimedOut,
  CLIENT_ATTRIBUTES_LIST
);

void InitWiFi() {
  Serial.println("Connecting to AP ...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected to AP");
}

bool reconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  InitWiFi();
  return true;
}

void connectToThingsBoard() {
  if (tb.connected()) {
    return;
  }

  Serial.print("Connecting to ");
  Serial.println(THINGSBOARD_SERVER);

  if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
    Serial.println("Failed to connect");
    return;
  }

  Serial.println("Connected to ThingsBoard");

  tb.sendAttributeData("macAddress", WiFi.macAddress().c_str());

  rpc_subscribed = false;
  shared_attrs_subscribed = false;
  shared_request_sent = false;
  client_request_sent = false;
}

void processSetLedMode(const JsonVariantConst &data, JsonDocument &response) {
  Serial.println("Received the set led state RPC method");

  const int new_mode = data.as<int>();

  Serial.print("Mode to change: ");
  Serial.println(new_mode);

  if (new_mode != 0 && new_mode != 1) {
    response["error"] = "Unknown mode!";
    return;
  }

  ledMode = new_mode;
  attributesChanged = true;

  response["newMode"] = ledMode;
}

void processSharedAttributes(const JsonObjectConst &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (strcmp(it->key().c_str(), BLINKING_INTERVAL_ATTR) == 0) {
      const uint16_t new_interval = it->value().as<uint16_t>();

      if (new_interval >= BLINKING_INTERVAL_MS_MIN &&
          new_interval <= BLINKING_INTERVAL_MS_MAX) {
        blinkingInterval = new_interval;
        Serial.print("Blinking interval is set to: ");
        Serial.println(new_interval);
      }
    } else if (strcmp(it->key().c_str(), LED_STATE_ATTR) == 0) {
      ledState = it->value().as<bool>();

#ifdef LED_BUILTIN
      digitalWrite(LED_BUILTIN, ledState);
#endif

      Serial.print("LED state is set to: ");
      Serial.println(ledState);
    }
  }

  attributesChanged = true;
}

void processClientAttributes(const JsonObjectConst &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (strcmp(it->key().c_str(), LED_MODE_ATTR) == 0) {
      ledMode = it->value().as<uint16_t>();
    }
  }
}

void requestTimedOut() {
  Serial.println("Attribute request timed out. Ensure client is connected to the MQTT broker and that the keys exist on the device.");
}

void ensureRpcSubscribed() {
  if (!tb.connected() || rpc_subscribed) {
    return;
  }

  Serial.println("Subscribing for RPC...");
  if (!rpc.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
    Serial.println("Failed to subscribe for RPC");
    return;
  }

  rpc_subscribed = true;
  Serial.println("RPC subscribed");
}

void ensureSharedAttributesSubscribed() {
  if (!tb.connected() || shared_attrs_subscribed) {
    return;
  }

  if (!shared_update.Shared_Attributes_Subscribe(attributes_callback)) {
    Serial.println("Failed to subscribe for shared attribute updates");
    return;
  }

  shared_attrs_subscribed = true;
  Serial.println("Shared attribute subscription done");
}

void ensureAttributeRequestsSent() {
  if (!tb.connected()) {
    return;
  }

  if (!shared_request_sent) {
    Serial.println("Requesting shared attributes...");
    shared_request_sent = attr_request.Shared_Attributes_Request(
      attribute_shared_request_callback
    );

    if (!shared_request_sent) {
      Serial.println("Failed to request shared attributes");
    }
  }

  if (!client_request_sent) {
    Serial.println("Requesting client attributes...");
    client_request_sent = attr_request.Client_Attributes_Request(
      attribute_client_request_callback
    );

    if (!client_request_sent) {
      Serial.println("Failed to request client attributes");
    }
  }
}

void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);

#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  delay(1000);
  InitWiFi();

  pinMode(flamePin, INPUT);
  pinMode(dhtPin, INPUT);
  pinMode(buzPin, OUTPUT);
  pinMode(LEDred, OUTPUT);
  pinMode(LEDgreen, OUTPUT);
  pinMode(LEDblue, OUTPUT);
  pinMode(butI, INPUT_PULLDOWN);

  digitalWrite(LEDred, LOW);
  digitalWrite(LEDgreen, LOW);
  digitalWrite(LEDblue, LOW);
}


void loop() {

  unsigned long currentMillis = millis();
  int fireState = digitalRead(flamePin);

  if(digitalRead(butI) == HIGH && systemDelay == 0){
  systemState = !systemState;
  systemDelay = 20;
  } else {
    if(systemDelay>0){
      systemDelay--;
    }
  }

  delay(50);

  if (!reconnectWiFi()) {
    return;
  }

  connectToThingsBoard();

  if (!tb.connected()) {
    return;
  }

  tb.loop();

  ensureRpcSubscribed();
  ensureSharedAttributesSubscribed();
  ensureAttributeRequestsSent();

  if (attributesChanged) {
    attributesChanged = false;

    if (ledMode == 0) {
      previousStateChange = millis();
    }

    tb.sendTelemetryData(LED_MODE_ATTR, ledMode);
    tb.sendTelemetryData(LED_STATE_ATTR, ledState);
    tb.sendAttributeData(LED_MODE_ATTR, ledMode);
    tb.sendAttributeData(LED_STATE_ATTR, ledState);
  }

  if (ledMode == 1 && millis() - previousStateChange > blinkingInterval) {
    previousStateChange = millis();
    ledState = !ledState;

    tb.sendTelemetryData(LED_STATE_ATTR, ledState);
    tb.sendAttributeData(LED_STATE_ATTR, ledState);

#ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, ledState);
#else
    Serial.print("LED state changed to: ");
    Serial.println(ledState);
#endif
  }
  
  
  if(systemState == 1){
    if (millis() - previousDataSend > TELEMETRY_SEND_INTERVAL_MS) {
      previousDataSend = millis();


      tb.sendTelemetryData("envHumd", random(50.00, 52.00));
      tb.sendTelemetryData("flameData", fireState);
      tb.sendAttributeData("systemData", systemState);
      tb.sendAttributeData("rssi", WiFi.RSSI());
      tb.sendAttributeData("channel", WiFi.channel());
      tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
      tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
      tb.sendAttributeData("ssid", WiFi.SSID().c_str());

    if(fireState == 1){

// the DHT sensor isn't working, so I'm just simulating its behavior roughly using these random telemetry sends :(
// only the flame sensor is working, despite trying multiple times to make the DHT sensor work and used different libraries, but nothing
      tb.sendTelemetryData("envTemp", random(30.00, 35.00));
      digitalWrite(LEDgreen, HIGH);
      digitalWrite(LEDblue, LOW);
      digitalWrite(LEDred, LOW);


      if(digitalRead(butI) == LOW && systemDelay == 0 && currentMillis - previousMillis >= 2000){
            previousMillis = currentMillis;       // save the timestamp for reference
            digitalWrite(buzPin, HIGH);
            delay(100);
            digitalWrite(buzPin, LOW);

    //    delay(3000); The delays are causing too much wait time, I implemented millis() instead. It's a lot easier to detect fire instantly and not have to wait till it stops.
      }

    } else {

      tb.sendTelemetryData("envTemp", 50);
      digitalWrite(LEDgreen, LOW);
      digitalWrite(LEDblue, LOW);
      digitalWrite(LEDred, HIGH);

      if(digitalRead(butI) == LOW && systemDelay == 0){
        digitalWrite(buzPin, HIGH);
      }

    }

    }
  } else {

    tb.sendTelemetryData("envHumd", 0);
    tb.sendTelemetryData("envTemp", 0);
    digitalWrite(LEDblue, HIGH);
    digitalWrite(LEDgreen, LOW);
    digitalWrite(LEDred, LOW);

    digitalWrite(buzPin, LOW);
  }
  
}

