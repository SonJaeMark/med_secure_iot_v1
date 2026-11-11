#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Pin Definitions for Multiple Motors
struct MotorPins {
  int stepPin;
  int dirPin;
};

const MotorPins motors[3] = {
  {25, 26}, // Motor 1
  {27, 14}, // Motor 2
  {12, 13}  // Motor 3 (GPIO 12 is a strapping pin - move it if you get boot issues)
};

const int irSensorPins[3] = {18, 2, 5}; // IR sensors for Motors 1-3
const int endSwitchPins[3] = {19, 22, 23};   // End switches for Motors 1-3
const int startSwitchPins[3] = {21, 16, 17}; // Start switches for Motors 1-3

// Machine ID & API Security Key
const String machineId = "MSv1M-001";
const String apiKey = "qr-medsecure-super-secret-key-123";

// Motor configuration & Inversion Helper Flag
const int stepsPerRevolution = 200;
const bool INVERT_MOTOR_DIRECTION = false;
bool systemBusy = false;

// Polling Interval (every 5 seconds)
unsigned long lastPollTime = 0;
const unsigned long pollInterval = 5000;

// Web Server on port 80
WebServer server(80);
Preferences preferences;

// Built from the IP and Port entered in the setup portal on every boot
// (kept in RAM only, never saved). Result looks like: http://192.168.1.50:8000/api/v1
const String apiBasePath = "/api/v1";
String backendServerUrl = "";
bool onlineRoutesRegistered = false;

// Helper method for motor direction switching with motor channel selection
void setMotorDirection(int motorChannel, bool forward) {
  int dirPin = motors[motorChannel - 1].dirPin;
  if (INVERT_MOTOR_DIRECTION) {
    digitalWrite(dirPin, forward ? LOW : HIGH);
  } else {
    digitalWrite(dirPin, forward ? HIGH : LOW);
  }
}

// Forward declarations
void executeDispenseCycle(int motorChannel);
void executeCalibrationCycle(int motorChannel);
void pollBackend();
void sendMachineLog(String logType, String message);
void handleApiDispense();
void handleRoot();
void handleSaveCreds();
void registerOnlineRoutes();
String cleanHost(String host);
void testBackendConnection();

// Cleans up the IP/host the user typed: trims spaces, strips http(s):// , any path and any :port
String cleanHost(String host) {
  host.trim();
  if (host.startsWith("http://")) host = host.substring(7);
  else if (host.startsWith("https://")) host = host.substring(8);

  int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);

  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);

  host.trim();
  return host;
}

void setup() {
  Serial.begin(115200);

  // Never let the SDK store Wi-Fi credentials in flash
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP_STA);

  // Erase anything saved by older firmware (ssid, password, old server_url)
  preferences.begin("med-config", false);
  preferences.clear();
  preferences.end();

  // Configure all motor pins
  for (int i = 0; i < 3; i++) {
    pinMode(motors[i].stepPin, OUTPUT);
    pinMode(motors[i].dirPin, OUTPUT);
  }

  // Configure inputs
  for (int i = 0; i < 3; i++) {
    pinMode(irSensorPins[i], INPUT);
  }
  for (int i = 0; i < 3; i++) {
    pinMode(startSwitchPins[i], INPUT_PULLUP);
    pinMode(endSwitchPins[i], INPUT_PULLUP);
  }

  // Nothing is stored, so every boot starts the setup hotspot
  Serial.println("Starting AP Hotspot for Setup...");
  WiFi.softAP(("MedDispenser-Setup-" + machineId).c_str());
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSaveCreds);
  server.begin();
  Serial.println("Med Dispenser Initialized. Waiting for setup.");
}

void loop() {
  server.handleClient();

  if (systemBusy || WiFi.status() != WL_CONNECTED || backendServerUrl.length() == 0) {
    return;
  }

  // Periodic polling to Django backend server
  if (millis() - lastPollTime >= pollInterval) {
    lastPollTime = millis();
    pollBackend();
  }
}

// Local endpoints, available once Wi-Fi is connected
void registerOnlineRoutes() {
  if (onlineRoutesRegistered) return;
  onlineRoutesRegistered = true;

  server.on("/api/dispense", HTTP_POST, handleApiDispense);
  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json", "{\"machineId\":\"" + machineId + "\", \"status\":\"online\"}");
  });
}

// One-off check right after Wi-Fi connects, so URL problems show up in the Serial Monitor
void testBackendConnection() {
  String testUrl = backendServerUrl + "/iot/poll/?machine_id=" + machineId;
  Serial.printf("Testing backend: %s\n", testUrl.c_str());

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(testUrl);
  http.addHeader("X-API-Key", apiKey);
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.printf("Backend reachable. HTTP status: %d\n", httpCode);
  } else {
    Serial.printf("Backend NOT reachable: %s\n", http.errorToString(httpCode).c_str());
    Serial.println("Check: correct PC IP (not localhost/127.0.0.1), same Wi-Fi network,");
    Serial.println("Django running with 0.0.0.0:8000, firewall allows port 8000, ALLOWED_HOSTS includes the IP.");
  }
  http.end();
}

// Poll pending instruction from Django backend
void pollBackend() {
  HTTPClient http;
  String pollUrl = backendServerUrl + "/iot/poll/?machine_id=" + machineId;

  http.setTimeout(5000);
  http.begin(pollUrl);
  http.addHeader("X-API-Key", apiKey);

  int httpCode = http.GET();
  if (httpCode > 0) {
    String payload = http.getString();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      String status = doc["status"].as<String>();
      if (status == "success") {
        JsonObject cmd = doc["command"];
        int cmdId = cmd["id"];
        String cmdType = cmd["command_type"].as<String>();
        int motorChannel = cmd["motor_channel"] | 1; // Default to channel 1 if missing
        if (motorChannel < 1 || motorChannel > 3) motorChannel = 1;

        Serial.printf("Fetched command ID %d: %s for Motor %d\n", cmdId, cmdType.c_str(), motorChannel);

        systemBusy = true;
        if (cmdType == "DISPENSE") {
          executeDispenseCycle(motorChannel);
          sendMachineLog("INFO", "Dispense cycle completed successfully on Motor " + String(motorChannel));
        } else if (cmdType == "CALIBRATE") {
          executeCalibrationCycle(motorChannel);
          sendMachineLog("INFO", "Calibration routine completed successfully on Motor " + String(motorChannel));
        } else if (cmdType == "TEST_IR") {
          String sensorStates;
          for (int i = 0; i < 3; i++) {
            String state = digitalRead(irSensorPins[i]) == LOW ? "Triggered" : "Clear";
            Serial.printf("Motor %d IR Sensor state: %s\n", i + 1, state.c_str());
            if (i > 0) sensorStates += ", ";
            sensorStates += "Motor " + String(i + 1) + ": " + state;
          }
          sendMachineLog("INFO", "IR sensor test: " + sensorStates);
        }
        systemBusy = false;
      }
    } else {
      Serial.printf("Failed to parse poll JSON response (HTTP %d).\n", httpCode);
    }
  } else {
    Serial.printf("HTTP Poll failed (%s), error: %s\n", pollUrl.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
}

// Send machine logs back to the backend
void sendMachineLog(String logType, String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(backendServerUrl + "/logs/machine/");
  http.addHeader("X-API-Key", apiKey);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["log_type"] = logType;
  doc["message"] = message + " (Machine: " + machineId + ")";

  String requestBody;
  serializeJson(doc, requestBody);

  int httpCode = http.POST(requestBody);
  if (httpCode > 0) {
    Serial.printf("Machine log sent to backend. HTTP status: %d\n", httpCode);
  } else {
    Serial.printf("Machine log failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void handleRoot() {
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body>";
  html += "<h2>Med Dispenser Setup (" + machineId + ")</h2>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "Wi-Fi SSID:<br><input type=\"text\" name=\"ssid\" required><br><br>";
  html += "Wi-Fi Password:<br><input type=\"password\" name=\"password\"><br><br>";
  html += "Backend Server IP:<br><input type=\"text\" name=\"server_ip\" placeholder=\"192.168.1.100\" required><br>";
  html += "<small>Your computer's IP on the same Wi-Fi network (not localhost).</small><br><br>";
  html += "Backend Server Port:<br><input type=\"number\" name=\"server_port\" value=\"8000\" min=\"1\" max=\"65535\" required><br><br>";
  html += "<input type=\"submit\" value=\"Save & Connect\">";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveCreds() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String serverIp = cleanHost(server.arg("server_ip"));
  int serverPort = server.arg("server_port").toInt();

  if (ssid.length() == 0 || serverIp.length() == 0 || serverPort < 1 || serverPort > 65535) {
    server.send(400, "text/html", "<h3>SSID, Backend IP and a valid Port (1-65535) are required.</h3><a href=\"/\">Go back</a>");
    return;
  }

  // Kept in RAM only. Nothing here is written to flash.
  backendServerUrl = "http://" + serverIp + ":" + String(serverPort) + apiBasePath;
  Serial.printf("Backend URL set to: %s\n", backendServerUrl.c_str());

  // Send the reply first, because the hotspot shuts down once Wi-Fi connects
  server.send(200, "text/html",
              "<h3>Connecting to Wi-Fi...</h3>"
              "<p>If this hotspot disappears, the device connected successfully.<br>"
              "If it stays up, the Wi-Fi details were wrong: reload this page and try again.</p>");
  delay(500);

  Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    WiFi.softAPdisconnect(true); // Close the setup hotspot
    WiFi.mode(WIFI_STA);
    registerOnlineRoutes();
    testBackendConnection();
  } else {
    Serial.println("\nWiFi connection failed. Setup hotspot remains active.");
    WiFi.disconnect();
    backendServerUrl = "";
  }
}

void handleApiDispense() {
  if (systemBusy) {
    server.send(503, "application/json", "{\"error\":\"System busy\"}");
    return;
  }

  int motorChannel = 1;
  if (server.hasArg("motor_channel")) {
    motorChannel = server.arg("motor_channel").toInt();
    if (motorChannel < 1 || motorChannel > 3) motorChannel = 1;
  }

  Serial.printf("Local API Dispense command received for Motor %d.\n", motorChannel);
  systemBusy = true;
  executeDispenseCycle(motorChannel);
  systemBusy = false;

  server.send(200, "application/json", "{\"success\":true,\"message\":\"Dispense cycle completed\",\"motor_channel\":" + String(motorChannel) + ",\"machineId\":\"" + machineId + "\"}");
}

void executeDispenseCycle(int motorChannel) {
  int motorIndex = motorChannel - 1;
  int stepPin = motors[motorIndex].stepPin;

  setMotorDirection(motorChannel, true); // Set forward direction
  Serial.printf("Dispensing using Motor %d...\n", motorChannel);

  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    // Check if IR sensor detects medicine falling (LOW when triggered)
    if (digitalRead(irSensorPins[motorIndex]) == LOW) {
      Serial.printf("Medicine drop detected by Motor %d IR sensor! Stopping dispense.\n", motorChannel);
      delay(200);
      break;
    }

    // Check if end switch is reached (empty dispenser)
    if (digitalRead(endSwitchPins[motorIndex]) == LOW) {
      Serial.printf("End switch reached for Motor %d!\n", motorChannel);
      break;
    }
  }

  delay(500);

  // Return to starting position (Reverse direction)
  setMotorDirection(motorChannel, false);
  Serial.println("Returning to starting position...");

  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    // Check if start switch is reached (fully stacked dispenser)
    if (digitalRead(startSwitchPins[motorIndex]) == LOW) {
      Serial.printf("Start position reached for Motor %d. Motor stopped.\n", motorChannel);
      break;
    }
  }
}

void executeCalibrationCycle(int motorChannel) {
  int motorIndex = motorChannel - 1;
  int stepPin = motors[motorIndex].stepPin;
  Serial.printf("Starting calibration routine for Motor %d...\n", motorChannel);

  setMotorDirection(motorChannel, true);
  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    if (digitalRead(endSwitchPins[motorIndex]) == LOW) {
      Serial.printf("Calibration: Motor %d end switch hit.\n", motorChannel);
      break;
    }
  }

  delay(500);

  setMotorDirection(motorChannel, false);
  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    if (digitalRead(startSwitchPins[motorIndex]) == LOW) {
      Serial.printf("Calibration: Motor %d start switch hit. Homing complete.\n", motorChannel);
      break;
    }
  }
}