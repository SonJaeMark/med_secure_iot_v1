#include <Arduino.h>

// Pin Definitions
const int stepPin = 25;
const int dirPin = 26;
const int dispenseButtonPin = 2; // Test dispense button
const int calButtonPin = 5;      // Calibration button
const int irSensorPin = 18;      // IR sensor digital output
const int endSwitchPin = 19;     // End switch (empty dispenser)
const int startSwitchPin = 21;   // Start switch (fully stacked dispenser)

// Motor configuration & Inversion Helper Flag
const int stepsPerRevolution = 200; // 1.8 deg per step for 42BYGHW616
const bool INVERT_MOTOR_DIRECTION = false; // Set to true if physical wiring requires inverted movement
bool systemBusy = false;

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

void setup() {
  Serial.begin(115200);

  // Configure motor pins
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  
  // Configure inputs with internal pull-ups (Buttons/Switches pull LOW when pressed/triggered)
  pinMode(dispenseButtonPin, INPUT_PULLUP);
  pinMode(calButtonPin, INPUT_PULLUP);
  pinMode(irSensorPin, INPUT);
  pinMode(startSwitchPin, INPUT_PULLUP);
  pinMode(endSwitchPin, INPUT_PULLUP);

  Serial.println("Med Dispenser Initialized. Ready.");
}

void loop() {
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

void executeDispenseCycle() {
  // --- STEP 1: DISPENSE (Rotate Forward) ---
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

  // // --- STEP 2: REFILL / RETURN (Rotate Reverse) ---
  // setMotorDirection(false); // Set reverse direction
  // Serial.println("Returning to starting position...");

  // while (true) {
  //   digitalWrite(stepPin, HIGH);
  //   delayMicroseconds(1000);
  //   digitalWrite(stepPin, LOW);
  //   delayMicroseconds(1000);

  //   // Check if start end switch is reached (Pin 21 - fully stacked dispenser)
  //   if (digitalRead(startSwitchPin) == LOW) {
  //     Serial.println("Start position reached. Motor stopped.");
  //     break;
  //   }
  // }
}

void executeCalibrationCycle() {
  Serial.println("Starting calibration routine...");

  // --- STEP 1: Move forward until End Switch is reached (Pin 19) ---
  setMotorDirection(true); // Set forward direction
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

  // --- STEP 2: Move reverse until Start Switch is reached (Pin 21) ---
  setMotorDirection(false); // Set reverse direction
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