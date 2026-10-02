# Agrisentinel Smart Bird Detection System

A practical ESP32-based bird deterrent system that keeps the original pump + valve behavior but upgrades it into a smarter, safer, and more modular controller.

The system watches for motion, scans with a pan/tilt camera, optionally analyzes the image using a backend + OpenRouter vision model, and only triggers a short deterrent burst after validation. If AI is unavailable, it falls back to the sensor-based mode instead of failing completely.

It is designed to be safe, configurable, and easy to expand later with a dashboard or remote monitoring.

## Final project structure

This project is intentionally organized into a controller sketch plus a backend service:

```text
C:\Users\dions\OneDrive\Desktop\Agrisentinel\agrisentinel
├── agrisentinel.ino
├── README.md
├── .gitignore
├── backend/
│   ├── server.js
│   ├── package.json
│   ├── .env.example
│   └── node_modules/   (after npm install)
└── session files / local project artifacts
```

Important design rule:
- The ESP32 firmware does not contain the OpenRouter API key.
- The backend folder contains the secure API layer and environment configuration.
- The ESP32 only communicates with the backend URL.

## System overview

```text
             ┌───────────────┐
             │   ESP32-CAM   │
             │ Image Capture │
             └───────┬───────┘
                     │
                     ▼
              Wi-Fi / Network
                     │
                     ▼
             ┌───────────────┐
             │   Backend API │
             │  /api/analyze  │
             └───────┬───────┘
                     │
                     ▼
             ┌───────────────┐
             │  OpenRouter   │
             │ Vision Model  │
             └───────────────┘

Sensor ───────────────► ESP32
                         │
                         ├── Pan Servo
                         ├── Tilt Servo
                         │
                         ▼
                  Detection Decision
                         │
                         ▼
                  Relay Channel 1
                   ┌──────────────┐
                   ▼              ▼
                 Pump           Valve
```

## What this project does

- Monitors motion with a sensor
- Moves the camera through a scan pattern
- Captures an image when motion is detected
- Sends the image to the backend API if AI mode is available
- Validates AI output before triggering deterrence
- Falls back to sensor mode when AI is unavailable
- Drives the pump + valve only for a short, event-based spray burst
- Enforces a cooldown to avoid repeated spraying
- Defaults to pump OFF and valve OFF during faults or startup

## Hardware used

- ESP32 development board
- ESP32-CAM module
- 2-channel relay module
- 12V pump
- 12V valve
- 2 servo motors
- motion / movement sensor
- Wi-Fi network
- backend host with internet access

## Core behavior

### Normal state

```text
Pump OFF
Valve OFF
Camera monitoring
Sensor monitoring
```

### Bird detected

```text
Pump ON
Valve ON
for 10 seconds
then OFF for at least 3 seconds
then return to monitoring
```

### Important safety rule

The old behavior of continuously switching on/off in a fixed infinite loop is removed. The new system triggers deterrence only when a real detection event occurs, and it then waits through a configurable cooldown before allowing another spray cycle.

## Wiring and connection guide

### 1) Relay wiring

Use Relay Channel 1 only for the pump and valve combined output.

```text
ESP32 pin GPIO25  ───────► Relay CH1 IN
ESP32 GND        ───────► Relay GND
ESP32 3.3V/5V     ───────► Relay VCC (depends on your relay board, check logic level compatibility)

Relay CH1 NO/COM/NC       ───────► Pump + Valve control path
```

For most popular 5V relay boards, the logic is active-low:

```cpp
#define RELAY_ON  LOW
#define RELAY_OFF HIGH
```

This means the relay is normally OFF unless commanded ON.

### 2) Pan servo wiring

```text
ESP32 GPIO13  ───────► Pan servo signal
ESP32 5V      ───────► Pan servo VCC
ESP32 GND     ───────► Pan servo GND
```

Example pan positions:

```cpp
const int PAN_LEFT  = 30;
const int PAN_CENTER = 90;
const int PAN_RIGHT = 150;
```

### 3) Tilt servo wiring

```text
ESP32 GPIO14  ───────► Tilt servo signal
ESP32 5V      ───────► Tilt servo VCC
ESP32 GND     ───────► Tilt servo GND
```

Tilt movement is restricted to a safe down-only range:

```cpp
const int TILT_DOWN = 45;
const int TILT_STRAIGHT = 90;
const int TILT_MIN = 45;
const int TILT_MAX = 90;
```

The tilt servo should never be commanded beyond the configured safe range.

### 4) Motion sensor wiring

```text
ESP32 GPIO34  ───────► Motion sensor OUT
ESP32 3.3V    ───────► Motion sensor VCC
ESP32 GND     ───────► Motion sensor GND
```

Use a sensor that outputs HIGH when motion is detected, and LOW when idle.

### 5) ESP32-CAM wiring

The ESP32-CAM uses the board's specific camera bus pins. Keep them on the exact ESP32-CAM model's recommended pin map.

Be careful with GPIO restrictions:
- Some ESP32 pins are input-only
- Some pins are boot-related and not safe for normal use
- Camera bus pins must match the board pin definition

For the AI-Thinker ESP32-CAM model, the firmware uses the standard camera pin mapping and special camera initialization from the `esp_camera.h` configuration.

## Pin assignment table

| Component | Suggested GPIO | Notes |
|---|---:|---|
| Relay Control | GPIO25 | Safe digital output used for CH1 |
| Pan Servo | GPIO13 | PWM-capable GPIO |
| Tilt Servo | GPIO14 | PWM-capable GPIO |
| Motion Sensor | GPIO34 | Good input pin for motion detection |
| ESP32-CAM | Board-specific | Must use the correct camera pin map |

## Recommended firmware settings

These values are defined near the top of the firmware and can be tuned in the field:

```cpp
#define RELAY_PIN 25
#define PAN_SERVO_PIN 13
#define TILT_SERVO_PIN 14
#define MOTION_SENSOR_PIN 34

const int PAN_LEFT = 30;
const int PAN_CENTER = 90;
const int PAN_RIGHT = 150;

const int TILT_DOWN = 45;
const int TILT_STRAIGHT = 90;
const int TILT_MIN = 45;
const int TILT_MAX = 90;

const unsigned long SPRAY_TIME = 10000;
const unsigned long SPRAY_COOLDOWN = 10000;
const unsigned long CAMERA_SETTLE_TIME = 400;
const unsigned long AI_TIMEOUT = 7000;
const float BIRD_CONFIDENCE_THRESHOLD = 0.75;
```

## Firmware logic and system states

The firmware uses a state machine and `millis()` instead of long blocking delays.

Suggested states:

```text
STARTUP
MONITORING
SENSOR_DETECTED
SCANNING
AI_ANALYSIS
BIRD_DETECTED
SPRAYING
COOLDOWN
AI_UNAVAILABLE
```

This keeps the ESP32 responsive for:
- sensor monitoring
- servo movement
- Wi-Fi checks
- camera operation
- backend communication
- relay safety checks

## AI mode and sensor fallback mode

### AI mode

If Wi-Fi, ESP32-CAM, backend, and OpenRouter are available:

1. Movement is detected
2. Camera scans left/center/right/center positions
3. Image is captured at each stop
4. Image is uploaded to backend
5. Backend validates the image and calls OpenRouter
6. AI returns a structured JSON result
7. Deterrent triggers only if:
   - `bird_detected == true`
   - `confidence >= threshold`

### Sensor mode

If any of the following happens:
- Wi-Fi disconnected
- ESP32-CAM unavailable
- backend unavailable
- OpenRouter unavailable
- AI request times out
- AI returns malformed or invalid JSON

then the system automatically falls back to sensor-based detection.

This is a fallback, not a replacement for the AI path. The sensor is there to keep the device operating even when AI is unavailable.

## Backend design

The backend is the secure layer between the ESP32 and OpenRouter.

### Required backend endpoints

```text
GET /api/health
POST /api/analyze-bird
```

### Backend responsibilities

- accept image data from ESP32
- validate that a real image payload exists
- send the image to OpenRouter
- request structured JSON output
- parse JSON safely
- validate required keys such as `bird_detected` and `confidence`
- reject invalid or malformed AI responses
- return a clean JSON response to the ESP32

Example success response:

```json
{
  "success": true,
  "bird_detected": true,
  "confidence": 0.92,
  "bird_type": "unknown",
  "description": "Bird detected near the protected area",
  "recommended_action": "deter"
}
```

## OpenRouter model configuration

The backend uses a configurable model variable.

Example in `.env`:

```env
OPENROUTER_MODEL=openai/gpt-4o-mini
```

If the model is unavailable or deprecated, replace it with another supported vision-capable model without changing the firmware logic.

Do not place the OpenRouter API key in the ESP32 firmware.

## Backend environment setup

Create a `.env` file in the backend folder using the example:

```env
PORT=3001
OPENROUTER_API_KEY=your_openrouter_api_key_here
OPENROUTER_MODEL=openai/gpt-4o-mini
BACKEND_HOST=http://localhost:3001
ALLOWED_ORIGINS=http://localhost:3000
```

Then install the backend dependencies:

```bash
cd C:\Users\dions\OneDrive\Desktop\Agrisentinel\agrisentinel\backend
npm install
```

Then start the backend:

```bash
npm start
```

## Required Arduino libraries

Install these in the Arduino IDE or Arduino CLI:

- `WiFi.h`
- `HTTPClient.h`
- `ArduinoJson.h`
- `ESP32Servo.h`
- `esp_camera.h`

## Required backend dependencies

```bash
npm install express dotenv
```

## Connection and setup tutorial

### Step 1: prepare the hardware

- Connect the 2-channel relay to the ESP32
- Keep channel 1 for the pump + valve output
- Connect the pan and tilt servos
- Connect the sensor and camera
- Verify the pump and valve are fed from a controlled 12V source

### Step 2: prepare the backend

- Open the `backend` folder
- Create a `.env` file from `.env.example`
- Add your OpenRouter API key
- Set the model name and port
- Install packages with `npm install`
- Start with `npm start`

### Step 3: confirm backend health

Open:

```text
http://localhost:3001/api/health
```

Expected result:

```json
{
  "success": true,
  "status": "ok",
  "backend": "ready"
}
```

### Step 4: configure the firmware

Open [agrisentinel.ino](C:/Users/dions/OneDrive/Desktop/Agrisentinel/agrisentinel/agrisentinel.ino) and update:

- Wi-Fi SSID
- Wi-Fi password
- backend URL
- servo pin numbers if needed
- spray timing if needed
- confidence threshold

### Step 5: upload the firmware

Upload the sketch to the ESP32 using the Arduino IDE.

### Step 6: observe the serial monitor

You should see log output similar to this:

```text
====================================
SMART BIRD DETECTION SYSTEM
====================================
ESP32: OK
ESP32-CAM: CONNECTED
WiFi: CONNECTED
AI BACKEND: AVAILABLE
Sensor: READY
Pan Servo: READY
Tilt Servo: READY
Relay: READY
SYSTEM MODE: AI
STATUS: MONITORING
```

On detection:

```text
[Sensor] Movement detected
[Pan/Tilt] Starting scan
[Camera] Capturing image
[AI] Sending image to backend
[AI] Bird detected
[AI] Confidence: 0.92
[ACTION] Bird confirmed
[RELAY] Pump + Valve ON
[ACTION] Spraying for 10 seconds
[RELAY] Pump + Valve OFF
[COOLDOWN] Starting
[SYSTEM] Monitoring resumed
```

If AI fails:

```text
[AI] Request failed
[AI] Backend unavailable
[SYSTEM] Switching to SENSOR MODE
[Sensor] Movement detected
[Sensor] Triggering deterrent
```

## Safety protections built in

This system includes protections for:

- repeated spraying
- stuck motion sensor
- repeated AI detections of the same bird
- network failure
- ESP32-CAM failure
- servo movement outside configured safe limits
- relay staying ON accidentally
- pump running indefinitely
- valve running indefinitely

The default safe state is:

```text
Pump OFF
Valve OFF
```

## Detecting birds reliably

To improve accuracy:

- provide good lighting
- keep the camera lens clean
- keep the background simple
- avoid motion from tree branches or wind-blown objects if possible
- use multiple camera angles instead of a single fixed view
- tune the confidence threshold based on field testing

## Reducing false positives

- Ignore very short motion spikes
- Require valid AI output before spraying
- Use a minimum confidence threshold
- Restrict the camera scan to a controlled range
- Avoid vibration near the sensor or servo mounts

## Pump, valve, relay, and servo protection

- use a flyback protection approach for inductive loads
- power the relay/valve circuit independently when possible
- keep the relay default OFF at startup
- use a fuse or current-limited power path
- protect the servo travel range
- avoid repeated commands that force mechanical stops

## Why the backend should stay separate from the ESP32 firmware

The backend is the secure place for:
- OpenRouter API credentials
- validation logic
- image processing
- model configuration

The ESP32 firmware should be a thin field device that does:
- sensor detection
- camera capture
- servo motion
- relay control
- HTTP upload to backend

This keeps system security and maintainability much stronger.

## How the ESP32-CAM image upload works

The ESP32-CAM captures image bytes and sends them as JPEG data to the backend API. The backend receives the raw image and forwards it to OpenRouter through a secure server-side call.

This is safer and more maintainable than embedding the API key in the firmware.

## Future extension: web dashboard

The current design is modular and ready for a dashboard layer later, for example:

- current system state
- recent detections
- sensor status
- camera health
- spray history
- AI confidence logs
- remote configuration

This is possible because the logic is separated into a clear firmware layer and a backend API layer.

## Quick-start summary

1. Connect the hardware as shown above
2. Install backend packages
3. Set the `.env` file with the OpenRouter key
4. Run `npm start` in the backend folder
5. Verify `/api/health`
6. Upload the Arduino sketch
7. Open the serial monitor
8. Confirm the system is monitoring and ready

## Recommended next steps

- test the motion sensor first without spraying
- test the scan pattern and camera capture
- test the backend AI endpoint separately
- verify the spray cycle only runs once per valid trigger
- tune confidence values and spray cooldown after real-world testing

## Final note

This project keeps the original pump + valve spray deterrent function, but replaces the fixed endless pattern with a safer, smarter, and more robust system. It now includes AI bird detection, sensor fallback, pan/tilt scanning, validation logic, and default-safe behavior for faults.

That makes it much more practical for a real field deployment while still being easy to extend later.
