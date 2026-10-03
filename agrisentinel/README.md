# Agrisentinel Smart Bird Deterrent

Agrisentinel uses two ESP32 boards: a controller ESP32 handles the sensor, pan/tilt servos, and pump/valve relay; an ESP32-CAM captures images and asks a private backend to analyze them with OpenRouter. The two boards exchange scan requests and validated bird results over ESP-NOW. If AI or the camera link fails after motion is detected, the controller falls back to the motion sensor and can trigger the deterrent. The relay is normally OFF and spraying is limited by a timed burst and cooldown.

## Project layout

```text
agrisentinel\
├── agrisentinel.ino        Controller ESP32 sketch
├── esp32cam\
│   └── esp32cam.ino        Separate ESP32-CAM sketch
├── backend\
│   ├── server.js           Backend API + OpenRouter client
│   ├── package.json
│   ├── package-lock.json
│   └── .env                 Local secret/config file; do not commit
├── README.md
└── .gitignore
```

The OpenRouter API key belongs only in the backend's local `.env` file. Do not add it to either sketch, documentation, or a public repository.

## How the two boards work together

```text
PIR sensor ──► Controller ESP32 ──ESP-NOW scan request──► ESP32-CAM
                    │                                      │
              Pan/tilt servos                              ├─ captures JPEG
                    │                                      ├─ Wi-Fi upload
              Relay CH1 ◄──ESP-NOW bird result─────────────┤
               ┌────┴────┐                                 ▼
             12V pump  12V valve                 Backend → OpenRouter
```

1. The controller detects motion, moves the camera through center/left/center/right/center, and allows it to settle.
2. At each position, the controller sends an ESP-NOW scan request.
3. The ESP32-CAM captures a JPEG, sends it to `POST /api/analyze-bird`, validates the response, and returns the result over ESP-NOW.
4. The controller sprays only for a valid bird result at or above the confidence threshold.
5. If the camera/backend/AI fails, the controller falls back to the motion event. It never sprays repeatedly in its idle loop.

ESP-NOW does not need a signal wire. Both boards should join the same 2.4 GHz Wi-Fi access point so their ESP-NOW radios use the same channel. The controller broadcasts scan requests; the camera replies directly to the controller that sent the request.

## Hardware and wiring

### Controller ESP32 pin assignments

| Device | Controller ESP32 connection | Notes |
|---|---|---|
| Relay CH1 input | GPIO25 | Controls pump and valve together |
| Pan servo signal | GPIO13 | Left/center/right |
| Tilt servo signal | GPIO14 | Constrained to configured safe angles |
| PIR/motion sensor OUT | GPIO34 | Input-only pin; sensor must output a safe 3.3 V logic level |
| Relay VCC/GND | Relay supply and controller GND | Check the relay board's voltage and input compatibility |
| Servo V+/GND | Separate regulated 5 V supply | Size supply for both servos' stall current; share GND with controller |
| PIR VCC/GND | Sensor-rated supply and controller GND | Check the sensor's rated voltage |

GPIO34 is input-only and has no internal pull-up/down. Use a sensor with a defined digital output; do not apply 5 V logic to an ESP32 GPIO.

### Relay and 12 V loads

The controller's GPIO25 drives only the relay input. Use relay Channel 1 contacts to switch the 12 V load supply. Connect the pump and valve in parallel only if the relay contact rating and 12 V supply can safely handle their combined current. Use the relay's COM and NO contacts so the loads are normally off. Confirm the relay contact rating for DC motor/inductive loads, not only its printed resistive-load rating. Use appropriate fusing and suppression (such as correctly rated flyback protection) for the pump and valve.

The relay module's active level varies. The sketch defaults to active-low (`RELAY_ON LOW`, `RELAY_OFF HIGH`); verify the module with the pump disconnected before connecting the loads. Keep the controller relay output OFF at startup.

### Servo power

Do not power two servos from the ESP32 3.3 V pin. Use a regulated external supply sized for the servos. Connect external supply ground to controller ESP32 ground so the signal has a reference. Keep high-current pump wiring away from sensor and camera wiring.

### ESP32-CAM connections

For an AI-Thinker ESP32-CAM, the camera uses the module's onboard camera GPIOs. Do not connect its camera bus to the controller. Power the ESP32-CAM from a stable supply suitable for its peak current (typically a regulated 5 V supply with adequate current capacity; follow the exact board documentation). ESP-NOW is wireless, so no UART or GPIO connection between the two ESP32 boards is required.

The camera sketch's inline pin map is specifically for the AI-Thinker ESP32-CAM. Other camera boards need their own pin mapping.

## Configuration

### Configure both ESP32 sketches

Set the same 2.4 GHz Wi-Fi SSID and password in:

- `agrisentinel.ino`
- `esp32cam\esp32cam.ino`

In the camera sketch, set the backend URL to the computer running the backend. For the address supplied for this project, it is:

```text
http://192.168.1.5:3001/api/analyze-bird
```

This is the ESP32-CAM's upload destination, not an endpoint for a normal browser page. A browser health check uses:

```text
http://192.168.1.5:3001/api/health
```

Keep the computer's backend running, permit Node.js through Windows Firewall on a private network if prompted, and make sure the boards and computer are on the same LAN.

### Adjustable controller settings

Near the top of `agrisentinel.ino`, adjust the GPIO assignments and limits/timing for the actual hardware:

```cpp
const int PAN_LEFT = 30;
const int PAN_CENTER = 90;
const int PAN_RIGHT = 150;
const int TILT_DOWN = 45;
const int TILT_STRAIGHT = 90;
const int TILT_MIN = 45;
const int TILT_MAX = 90;

const unsigned long SPRAY_TIME = 10000;
const unsigned long SPRAY_COOLDOWN = 10000;
const unsigned long CAMERA_SETTLE_TIME = 500;
const unsigned long CAMERA_RESPONSE_TIMEOUT = 13000;
const uint16_t BIRD_CONFIDENCE_THRESHOLD_PERMILLE = 750;
```

Keep the tilt safe limits within the camera mount's mechanical range. The camera sketch uses the same confidence threshold; keep both values aligned if you change it.

## Backend and OpenRouter setup

The backend runs on the computer or another server with internet access. From PowerShell:

```powershell
cd C:\Users\dions\OneDrive\Desktop\Agrisentinel\agrisentinel\backend
npm install
```

Create or edit `backend\.env` locally. Set `PORT=3001`, `OPENROUTER_MODEL=google/gemini-2.5-flash`, and set your OpenRouter key there privately. Do not paste the key into either sketch or this README. Then start the service:

```powershell
npm start
```

Check that it is running by opening `http://192.168.1.5:3001/api/health` from a device on the same network. `/api/analyze-bird` accepts JPEG bytes from the ESP32-CAM; it is not intended to be visited directly in a browser.

Backend dependencies are listed in `backend\package.json` (Express and dotenv). The model is configurable through `OPENROUTER_MODEL`; replace it in `.env` with another currently supported vision model if needed. The backend alone calls OpenRouter and holds the secret.

## Arduino setup and upload tutorial

Use Arduino IDE 2.x and install/select the ESP32 board platform. The sketches use the ESP32 Arduino core 3.x API for ESP-NOW receive callbacks.

### Controller ESP32

1. Open `agrisentinel.ino` in Arduino IDE.
2. Set Wi-Fi name/password and verify the controller pin settings.
3. Install `ESP32Servo` through Library Manager if not installed.
4. Select the correct ESP32 board and serial port.
5. Upload and open Serial Monitor at 115200 baud.
6. Note that the controller logs its Wi-Fi channel and ESP-NOW initialization status.

### ESP32-CAM

1. Open `esp32cam\esp32cam.ino` in Arduino IDE as a separate sketch.
2. Set the same Wi-Fi name/password and confirm the backend URL.
3. Select the correct ESP32-CAM board (the provided camera pin map is AI-Thinker).
4. Connect the board as required by its USB-to-serial adapter/programmer. For common ESP32-CAM programming adapters, GPIO0 is held low during upload; disconnect GPIO0 from GND and reset/power-cycle to run the sketch after uploading.
5. Upload and open its Serial Monitor at 115200 baud. Confirm Wi-Fi, camera, and ESP-NOW initialize.

Both sketches use ESP-NOW broadcasts for scan requests and respond to the sender's MAC automatically; no MAC address needs to be copied into the code.

## Expected serial output

Controller startup:

```text
AGRISENTINEL CONTROLLER ESP32
[RELAY] Pump + Valve default OFF
[Sensor] READY
[Pan Servo] READY
[Tilt Servo] READY
[WiFi] Connected; ESP-NOW channel ...
[ESP-NOW] Controller MAC: ...
[ESP-NOW] READY
[SYSTEM] Monitoring
```

Camera startup:

```text
AGRISENTINEL ESP32-CAM
[WiFi] Connected; ESP-NOW channel ...
[ESP-NOW] Camera MAC: ...
[Camera] READY
[ESP-NOW] READY; waiting for scan commands
```

On a successful detection, the controller logs the scan, confidence, relay ON for the configured spray time, relay OFF, then cooldown. The camera logs the capture and analysis result.

## AI and fallback behavior

- AI mode requires the camera, Wi-Fi, backend, and OpenRouter to be available.
- The backend returns structured JSON; the camera verifies HTTP success, JSON fields, `success`, boolean `bird_detected`, confidence in the 0–1 range, and `recommended_action`.
- The controller independently checks the confidence threshold before spraying.
- If backend/AI fails, the camera reports AI unavailable; the controller uses the motion event as a fallback deterrent.
- If ESP-NOW is unavailable, the controller can still trigger a sensor fallback, but camera-assisted analysis cannot operate.
- If a valid AI response says no bird is present, the controller continues scanning and does not spray for that scan.
- The relay is switched OFF after `SPRAY_TIME`, and the controller waits through `SPRAY_COOLDOWN` before rearming.

## Safety and field testing

1. First test with the pump and valve disconnected. Verify relay polarity and that it powers up OFF.
2. Test sensor triggers, scan positions, and camera/backend response before attaching liquid plumbing.
3. Check servo ranges mechanically before allowing full pan/tilt travel.
4. Use a fuse and correctly rated power supplies for the pump and valve; do not power servos from the ESP32 3.3 V rail.
5. Keep water away from the electronics and use a weatherproof enclosure.
6. Verify the pump/valve current and relay DC contact ratings before operating them together.

AI vision confidence is not a guarantee of species identification or detection. Tune the threshold in field trials, control lighting/glare, stabilize the camera, and test false positives before unattended use.
