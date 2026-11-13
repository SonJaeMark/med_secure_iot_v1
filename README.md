# Med Secure IoT Dispenser

ESP32 firmware for a three-channel medication dispenser. The firmware drives
three stepper motor channels, reads end and start limit switches, detects a
dispense with an IR sensor, and communicates with a Django backend over Wi-Fi.

## Hardware

### Pin assignments

| Function | Motor 1 | Motor 2 | Motor 3 |
|---|---:|---:|---:|
| Step | GPIO 25 | GPIO 27 | GPIO 12 |
| Direction | GPIO 26 | GPIO 14 | GPIO 13 |
| IR sensor | GPIO 18 | GPIO 2 | GPIO 5 |
| End switch | GPIO 19 | GPIO 22 | GPIO 23 |
| Start switch | GPIO 21 | GPIO 16 | GPIO 17 |

Limit switches are configured with `INPUT_PULLUP` and are treated as
active-low: connect the input to ground when triggered. IR sensors are
configured as plain `INPUT`s and are also treated as active-low by the
firmware. Use sensor outputs compatible with the ESP32's 3.3 V logic.

Check the GPIO assignments against your exact ESP32 board and wiring before
powering the motors. In particular, GPIO 2, 5, and 12 have boot-strapping
functions on many ESP32 boards; ensure attached sensors do not force an
incompatible level during boot. GPIO 16 and 17 may be unavailable on some
module variants. Use suitable motor driver boards; do not connect stepper
motors directly to ESP32 GPIO pins.

## Build and upload

This is a PlatformIO project targeting the `esp32dev` board with the Arduino
framework. ArduinoJson is declared as a project dependency.

Install PlatformIO, connect the ESP32, then run from the project directory:

```sh
pio run
pio run --target upload
pio device monitor --baud 115200
```

## First-time Wi-Fi setup

1. Power on the ESP32 and open the serial monitor at **115200 baud**.
2. Connect a phone or computer to the setup Wi-Fi network named
   `MedDispenser-Setup-<machine ID>` (the current ID is `MSv1M-001`).
3. Open the access point address shown in the serial monitor in a browser.
   The ESP32 normally uses `192.168.4.1` for its setup access point.
4. Enter the Wi-Fi SSID and password, backend server IP address, and port
   (default `8000`), then submit the form. After a successful connection, these
   settings are saved in ESP32 preferences and reused on later boots. If no
   saved settings exist or the saved Wi-Fi network cannot be reached, the
   setup hotspot opens again. The form pre-fills the saved SSID and backend
   address; enter the Wi-Fi password again when reconnecting.
5. When connected, the setup access point is shut down. The device prints its
   Wi-Fi IP and tests the backend connection.

The backend URL is assembled as `http://<server-ip>:<port>/api/v1`. Use the
backend computer's LAN IP address, not `localhost` or `127.0.0.1`. The backend
must be reachable from the ESP32 on the same network and listen on an
externally reachable interface.

## Operation

- Backend commands can select Motor 1, 2, or 3. Invalid/missing motor channel
  values default to Motor 1.
- Each motor has its own IR sensor. A dispense runs the selected motor toward
  its end switch. It stops early if that motor's IR sensor detects a medicine
  drop. Use the refill operation to return the selected motor to its start
  switch.
- Calibration moves the selected motor to its end switch and then homes it at
  its start switch.
- The backend `TEST_IR` command reports the state of all three IR sensors.
- Backend polling occurs every five seconds while Wi-Fi and the configured
  backend URL are available.

Motor motion currently runs in blocking loops until the expected sensor or
limit switch is triggered. Ensure sensors and switches are connected and
working before issuing a dispense or calibration command.

## HTTP endpoints

The device provides these local endpoints after connecting to Wi-Fi:

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/status` | Returns the machine ID and online status |
| `POST` | `/api/dispense` | Runs a dispense cycle; optionally provide `motor_channel` |
| `POST` | `/api/refill` | Returns a motor to its start position; optionally provide `motor_channel` |

Backend communication uses the `/api/v1/iot/poll/` path for polling and
`/api/v1/logs/machine/` for machine logs. Requests authenticate with the
firmware's configured API key.

## Configuration and security

The machine ID and backend API key are currently constants in
`src/main.cpp`. Do not publish real credentials in source control; use a
per-device secret and move secrets to a provisioning mechanism before
deployment. The setup page and local device endpoints do not implement
authentication, so use them only on a trusted network.
