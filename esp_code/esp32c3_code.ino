#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include <Preferences.h>

Preferences prefs;

// ================= SERVER =================
WebServer server(80);

bool relayState = false;

// ================= CONFIG =================
int relayPin = 2;
int sensorPin = 4;

float voltage = 240.0;
float sensitivity = 0.100;

// ================= GLOBALS =================
volatile float Irms = 0;
volatile float power = 0;
volatile float energy = 0;
float currentLimit = 20.0;
volatile bool faultTrip = false;

String wifiStatus = "Not connected";

// For RMS calculation
#define SAMPLE_BUFFER 200
float sampleBuffer[SAMPLE_BUFFER];
volatile int sampleIndex = 0;

// Timing
unsigned long lastEnergyTime = 0;

WiFiClientSecure espClient;
PubSubClient client(espClient);

// 🔐 Replace with your HiveMQ details
const char* mqtt_server = "0e66730e789d474d957d8738cc74ac60.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "Wirelessplug_iiti1";
const char* mqtt_pass = "Wirelessplug_iiti1";

// ================= HTML =================
String webpage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Smart Plug</title>
</head>
<body>

<h2>Smart Plug</h2>
<button id="toggleBtn" onclick="togglePlug()">OFF</button>
<h3 id="faultText" style="color:red;"></h3>
<button onclick="resetFault()">Reset Fault</button>

<h3>Current: <span id="current">0</span></h3>
<h3>Power: <span id="power">0</span></h3>
<h3>Energy: <span id="energy">0</span></h3>

<h3>WiFi</h3>
<input id="ssid" placeholder="SSID"><br>
<input id="pass" placeholder="Password"><br>
<button onclick="connectWiFi()">Connect</button>

<h4 id="status">Not connected</h4>

<button onclick="forgetWiFi()">Forget WiFi</button>

<script>
function forgetWiFi(){
  fetch('/forget');
}
</script>

<script>
function connectWiFi(){
  let ssid = document.getElementById("ssid").value;
  let pass = document.getElementById("pass").value;

  fetch(`/connect?ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`)
}

setInterval(()=>{
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById("current").innerText = d.current.toFixed(2);
    document.getElementById("power").innerText = d.power.toFixed(2);
    document.getElementById("energy").innerText = d.energy.toFixed(3);
  });

  fetch('/status').then(r=>r.text()).then(t=>{
    document.getElementById("status").innerText = t;
  });

},1000);
</script>

<script>
let isOn = false;
let fault = false;

function togglePlug() {
  if (fault) {
    alert("Fault active! Reset first.");
    return;
  }

  if (isOn) {
    fetch('/off');
  } else {
    fetch('/on');
  }
}

function resetFault() {
  fetch('/reset');
}

function updateButton() {
  document.getElementById("toggleBtn").innerText = isOn ? "ON" : "OFF";

  if (fault) {
    document.getElementById("faultText").innerText = "Overcurrent Trip!";
  } else {
    document.getElementById("faultText").innerText = "";
  }
}

setInterval(()=>{
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById("current").innerText = d.current.toFixed(2);
    document.getElementById("power").innerText = d.power.toFixed(2);
    document.getElementById("energy").innerText = d.energy.toFixed(3);

    isOn = d.relay == 1;
    fault = d.fault == 1;

    updateButton();
  });
},1000);
</script>

</body>
</html>
)rawliteral";

// ================= HANDLERS =================
void handleRoot() {
  server.send(200, "text/html", webpage);
}

void handleOn() {

  if (faultTrip) {
    server.send(200, "text/plain", "FAULT - Reset required");
    return;
  }

  digitalWrite(relayPin, HIGH);
  relayState = true;
  lastEnergyTime = millis();

  publishRelayState(); 

  server.send(200, "text/plain", "ON");
}

void handleOff() {
  digitalWrite(relayPin, LOW);
  relayState = false;

  publishRelayState(); 

  server.send(200, "text/plain", "OFF");
}

void handleResetFault() {
  faultTrip = false;

  if (client.connected()) {
    client.publish("smartplug/fault", "cleared");
    client.publish("smartplug/status", "online");
  }

  server.send(200, "text/plain", "Fault Cleared");
}

void handleData() {
  String json = "{";
  json += "\"current\":" + String(Irms) + ",";
  json += "\"power\":" + String(power) + ",";
  json += "\"energy\":" + String(energy);
  json += ",\"relay\":" + String(relayState ? 1 : 0);
  json += ",\"fault\":" + String(faultTrip ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

void handleStatus() {
  server.send(200, "text/plain", wifiStatus);
}

void handleConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  wifiStatus = "Connecting...";
  WiFi.disconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  server.send(200, "text/plain", "Trying...");
}

void handleForget() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  WiFi.disconnect(true);

  wifiStatus = "WiFi cleared";

  server.send(200, "text/plain", "Cleared");
}

// ================= TASKS =================

// 🌐 HIGH PRIORITY: Web server
void serverTask(void *pv) {
  while (1) {
    server.handleClient();
    vTaskDelay(1);
  }
}

// 📡 WiFi monitoring task
void wifiTask(void *pv) {
  while (1) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiStatus = "Connected: " + WiFi.localIP().toString();
    }
    else if (WiFi.status() == WL_CONNECT_FAILED) {
      wifiStatus = "Connection Failed";
    }
    else if (WiFi.status() != WL_CONNECTED) {
      wifiStatus = "Disconnected";
}
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ⚡ Measurement task (1 kHz)
void measurementTask(void *pv) {
  while (1) {

    if (relayState) {

      int adc = analogRead(sensorPin);
      float v = (adc / 4095.0) * 3.3;
      float current = (v - 1.65) / sensitivity;

      static int ocCount = 0;

      // ===== FAST OVERCURRENT PROTECTION =====
      if (fabs(current) > currentLimit) {
        ocCount++;

        if (ocCount >= 2) {   // 2 ms confirmation
          digitalWrite(relayPin, LOW);
          relayState = false;
          faultTrip = true;
          publishRelayState();

          if (client.connected()) {
            client.publish("smartplug/fault", "overcurrent");
            client.publish("smartplug/status", "tripped");
          }

          ocCount = 0;

          continue;   // 🔥 VERY IMPORTANT
        }
      } else {
        ocCount = 0;
      }

      // Store sample ONLY if safe
      sampleBuffer[sampleIndex++] = current;
      if (sampleIndex >= SAMPLE_BUFFER)
        sampleIndex = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// 🔋 Energy calculation task
void energyTask(void *pv) {
  while (1) {

    if (relayState) {

      float sum = 0;
      for (int i = 0; i < SAMPLE_BUFFER; i++)
        sum += sampleBuffer[i] * sampleBuffer[i];

      Irms = sqrt(sum / SAMPLE_BUFFER);
      power = voltage * Irms;

      unsigned long now = millis();
      float dt = (now - lastEnergyTime) / 1000.0;
      lastEnergyTime = now;

      energy += power * dt / 3600.0;
    }
    else {
      Irms = 0;
      power = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void mqttTask(void *pv) {

  while (1) {

    if (WiFi.status() == WL_CONNECTED) {

      static unsigned long lastAttempt = 0;

    if (!client.connected() && millis() - lastAttempt > 3000) {
    connectMQTT();
    lastAttempt = millis();
  }

      client.loop();

      // 📤 Publish data every 2 sec
      static unsigned long lastPub = 0;

      if (millis() - lastPub > 2000) {

        String payload = "{";
        payload += "\"current\":" + String(Irms) + ",";
        payload += "\"power\":" + String(power) + ",";
        payload += "\"energy\":" + String(energy);
        payload += "}";

        if (client.connected()) {
          client.publish("smartplug/data", payload.c_str());
        }

        lastPub = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  String msg;

  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  if (String(topic) == "smartplug/control") {

  if (msg == "1") {

    if (faultTrip) {
      Serial.println("⚠️ Cannot turn ON - Fault active");
      return;
    }

    digitalWrite(relayPin, HIGH);
    relayState = true;
    lastEnergyTime = millis();
    publishRelayState(); 
  }

  else if (msg == "0") {
    digitalWrite(relayPin, LOW);
    relayState = false;
    publishRelayState(); 
  }
}

  if (String(topic) == "smartplug/reset") {

  faultTrip = false;

  Serial.println("✅ Fault Reset from MQTT");

  // Update UI via MQTT
  if (client.connected()) {
    client.publish("smartplug/fault", "cleared");
    client.publish("smartplug/status", "online");
  }
}

}

void connectMQTT() {

  while (!client.connected()) {

    if (client.connect(
        "ESP32Client",
        mqtt_user,
        mqtt_pass,
        "smartplug/status",
        0,
        true,
        "offline"
    )) {

      client.subscribe("smartplug/control");
      client.subscribe("smartplug/reset");

      // ✅ Tell system we are online
      client.publish("smartplug/status", "online", true);
      publishRelayState(); 

    } else {
      delay(2000);
    }
  }
}

void publishRelayState() {
  if (client.connected()) {
    client.publish("smartplug/relay", relayState ? "1" : "0", true);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  // ================= WIFI INIT =================
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_Plug", "12345678");

  espClient.setInsecure();   // allow TLS without certificate

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  // 🔥 Load saved credentials BEFORE tasks start
  prefs.begin("wifi", true);

  String savedSSID = prefs.getString("ssid", "");
  String savedPASS = prefs.getString("pass", "");

  prefs.end();

  if (savedSSID != "") {
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    wifiStatus = "Auto-connecting...";
  }

  // ================= SERVER =================
  server.on("/", handleRoot);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/data", handleData);
  server.on("/connect", handleConnect);
  server.on("/status", handleStatus);
  server.on("/forget", handleForget);
  server.on("/reset", handleResetFault);

  server.begin();

  // ================= TASKS =================
  xTaskCreate(serverTask, "Server Task", 4096, NULL, 3, NULL);
  xTaskCreate(wifiTask, "WiFi Task", 4096, NULL, 3, NULL);
  xTaskCreate(measurementTask, "Measurement Task", 4096, NULL, 2, NULL);
  xTaskCreate(energyTask, "Energy Task", 4096, NULL, 1, NULL);
  xTaskCreate(mqttTask, "MQTT Task", 4096, NULL, 2, NULL);
}

// ================= LOOP =================
void loop() {
  // empty (RTOS handles everything)
}