#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// Pin Definitions
const int stepPin = 25;
const int dirPin = 26;
const int dispenseButtonPin = 2; // Test dispense button
const int calButtonPin = 5;      // Calibration button
const int irSensorPin = 18;      // IR sensor digital output
const int endSwitchPin = 19;     // End switch (empty dispenser)
const int startSwitchPin = 21;   // Start switch (fully stacked dispenser)

// Machine ID
const String machineId = "MSv1M-001";

// Motor configuration & Inversion Helper Flag
const int stepsPerRevolution = 200; 
const bool INVERT_MOTOR_DIRECTION = false; 
bool systemBusy = false;

// Web Server on port 80
WebServer server(80);
Preferences preferences;

// Helper method for easy motor direction switching
void setMotorDirection(bool forward) {
  if (INVERT_MOTOR_DIRECTION) {
    digitalWrite(dirPin, forward ? LOW : HIGH);
  } else {
    digitalWrite(dirPin, forward ? HIGH : LOW);
  }
}

// Forward declarations
void executeDispenseCycle();
void executeCalibrationCycle();
void handleApiDispense();
void handleRoot();
void handleSaveCreds();

void setup() {
  Serial.begin(115200);

  // Configure motor pins
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  
  // Configure inputs with internal pull-ups
  pinMode(dispenseButtonPin, INPUT_PULLUP);
  pinMode(calButtonPin, INPUT_PULLUP);
  pinMode(irSensorPin, INPUT);
  pinMode(startSwitchPin, INPUT_PULLUP);
  pinMode(endSwitchPin, INPUT_PULLUP);

  // Load saved Wi-Fi credentials
  preferences.begin("wifi-config", false);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");

  if (ssid.length() > 0) {
    Serial.printf("Connecting to saved WiFi: %s\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(500);
      Serial.print(".");
    }
  }

  // If connection fails, spin up AP Hotspot for configuration
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nStarting AP Hotspot for Setup...");
    WiFi.softAP("MedDispenser-Setup-" + machineId);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSaveCreds);
  } else {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Setup API endpoint once connected
    server.on("/api/dispense", HTTP_POST, handleApiDispense);
    server.on("/api/status", HTTP_GET, []() {
      server.send(200, "application/json", "{\"machineId\":\"" + machineId + "\", \"status\":\"online\"}");
    });
  }

  server.begin();
  Serial.println("Med Dispenser Initialized. Ready.");
}

void loop() {
  server.handleClient();

  if (systemBusy) {
    return;
  }

  // Check if test dispense button is pressed
  if (digitalRead(dispenseButtonPin) == LOW) {
    Serial.println("Dispense command received.");
    systemBusy = true;
    executeDispenseCycle();
    systemBusy = false;
  }
  
  // Check if calibration button is pressed
  if (digitalRead(calButtonPin) == LOW) {
    Serial.println("Calibration command received.");
    systemBusy = true;
    executeCalibrationCycle();
    systemBusy = false;
  }
}

void handleRoot() {
  String html = "<html><body>";
  html += "<h2>Med Dispenser Setup (" + machineId + ")</h2>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "SSID:<br><input type=\"text\" name=\"ssid\"><br>";
  html += "Password:<br><input type=\"password\" name=\"password\"><br><br>";
  html += "<input type=\"submit\" value=\"Save & Connect\">";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveCreds() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");

  preferences.putString("ssid", ssid);
  preferences.putString("password", password);

  server.send(200, "text/html", "<h3>Credentials Saved! Restarting device...</h3>");
  delay(2000);
  ESP.restart();
}

void handleApiDispense() {
  if (systemBusy) {
    server.send(503, "application/json", "{\"error\":\"System busy\"}");
    return;
  }

  Serial.println("API Dispense command received.");
  systemBusy = true;
  executeDispenseCycle();
  systemBusy = false;

  server.send(200, "application/json", "{\"success\":true,\"message\":\"Dispense cycle completed\",\"machineId\":\"" + machineId + "\"}");
}

void executeDispenseCycle() {
  setMotorDirection(true); // Set forward direction
  Serial.println("Dispensing...");

  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000); 
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    // Check if IR sensor detects medicine falling (LOW when triggered)
    if (digitalRead(irSensorPin) == LOW) {
      Serial.println("Medicine drop detected by IR sensor! Stopping dispense.");
      delay(200); 
      break;
    }

    // Check if the end switch is reached (Pin 19 - empty dispenser)
    if (digitalRead(endSwitchPin) == LOW) {
      Serial.println("End switch reached!");
      break;
    }
  }

  delay(500);

  // --- STEP 2: REFILL / RETURN (Rotate Reverse) ---
  setMotorDirection(false); // Set reverse direction
  Serial.println("Returning to starting position...");

  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    // Check if start end switch is reached (Pin 21 - fully stacked dispenser)
    if (digitalRead(startSwitchPin) == LOW) {
      Serial.println("Start position reached. Motor stopped.");
      break;
    }
  }
}

void executeCalibrationCycle() {
  Serial.println("Starting calibration routine...");

  setMotorDirection(true); 
  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    if (digitalRead(endSwitchPin) == LOW) {
      Serial.println("Calibration: End switch hit.");
      break;
    }
  }

  delay(500);

  setMotorDirection(false); 
  while (true) {
    digitalWrite(stepPin, HIGH);
    delayMicroseconds(1000);
    digitalWrite(stepPin, LOW);
    delayMicroseconds(1000);

    if (digitalRead(startSwitchPin) == LOW) {
      Serial.println("Calibration: Start switch hit. Homing complete.");
      break;
    }
  }
}