/*
  Agrisentinel controller ESP32
  Owns the motion sensor, pan/tilt servos, and pump/valve relay.
  Requests camera scans and receives validated AI detections over ESP-NOW.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <ESP32Servo.h>

#define RELAY_PIN 25
#define PAN_SERVO_PIN 13
#define TILT_SERVO_PIN 14
#define MOTION_SENSOR_PIN 34

#define RELAY_ON LOW
#define RELAY_OFF HIGH

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

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
const unsigned long SENSOR_REARM_TIME = 1500;
const uint16_t BIRD_CONFIDENCE_THRESHOLD_PERMILLE = 750;

const uint8_t MSG_SCAN_REQUEST = 1;
const uint8_t MSG_SCAN_RESULT = 2;
const uint8_t RESULT_AI_VALID = 0;
const uint8_t RESULT_AI_UNAVAILABLE = 1;
const uint8_t RESULT_CAMERA_ERROR = 2;

struct __attribute__((packed)) ScanMessage {
  uint8_t type;
  uint16_t sequence;
  uint8_t status;
  uint8_t birdDetected;
  uint16_t confidencePermille;
};

const uint8_t BROADCAST_MAC[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const int SCAN_PAN_ANGLES[] = {
  PAN_CENTER,
  PAN_LEFT,
  PAN_CENTER,
  PAN_RIGHT,
  PAN_CENTER
};
const size_t SCAN_POSITION_COUNT = sizeof(SCAN_PAN_ANGLES) / sizeof(SCAN_PAN_ANGLES[0]);

enum SystemState {
  MONITORING,
  WAITING_FOR_CAMERA_SETTLE,
  WAITING_FOR_CAMERA_RESULT,
  SPRAYING,
  COOLDOWN
};

Servo panServo;
Servo tiltServo;
SystemState systemState = MONITORING;

portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool cameraResultReady = false;
volatile uint16_t cameraResultSequence = 0;
volatile uint8_t cameraResultStatus = RESULT_CAMERA_ERROR;
volatile uint8_t cameraBirdDetected = 0;
volatile uint16_t cameraConfidencePermille = 0;

bool espNowReady = false;
bool relayActive = false;
bool sensorArmed = true;
bool sensorWasHigh = false;
bool hasSprayed = false;
uint16_t scanSequence = 0;
size_t scanPosition = 0;
unsigned long stateStartedAt = 0;
unsigned long sprayStartedAt = 0;
unsigned long cooldownStartedAt = 0;
unsigned long sensorLowStartedAt = 0;

void setRelay(bool enabled) {
  relayActive = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ON : RELAY_OFF);
}

void setPanAngle(int angle) {
  panServo.write(constrain(angle, PAN_LEFT, PAN_RIGHT));
}

void setTiltAngle(int angle) {
  tiltServo.write(constrain(angle, TILT_MIN, TILT_MAX));
}

void enterMonitoring() {
  systemState = MONITORING;
  Serial.println("[SYSTEM] Monitoring");
}

void startSpray(const char* reason) {
  if (systemState == SPRAYING || systemState == COOLDOWN) {
    return;
  }

  if (hasSprayed && millis() - cooldownStartedAt < SPRAY_COOLDOWN) {
    Serial.println("[SAFETY] Spray request ignored during cooldown");
    systemState = COOLDOWN;
    stateStartedAt = millis();
    return;
  }

  Serial.printf("[ACTION] Deterrent triggered: %s\n", reason);
  setRelay(true);
  sprayStartedAt = millis();
  systemState = SPRAYING;
  stateStartedAt = sprayStartedAt;
  Serial.println("[RELAY] Pump + Valve ON");
  Serial.printf("[ACTION] Spraying for %lu ms\n", SPRAY_TIME);
}

bool addBroadcastPeer() {
  if (esp_now_is_peer_exist(BROADCAST_MAC)) {
    return true;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  return esp_now_add_peer(&peer) == ESP_OK;
}

bool sendScanRequest() {
  if (!espNowReady || !addBroadcastPeer()) {
    return false;
  }

  ScanMessage request = {};
  request.type = MSG_SCAN_REQUEST;
  request.sequence = ++scanSequence;
  request.status = 0;
  request.birdDetected = 0;
  request.confidencePermille = 0;

  portENTER_CRITICAL(&resultMux);
  cameraResultReady = false;
  portEXIT_CRITICAL(&resultMux);

  esp_err_t result = esp_now_send(BROADCAST_MAC, (const uint8_t*)&request, sizeof(request));
  if (result != ESP_OK) {
    Serial.printf("[ESP-NOW] Scan request failed: %d\n", result);
    return false;
  }

  Serial.printf("[ESP-NOW] Requested camera scan %u at pan %d\n",
                request.sequence, SCAN_PAN_ANGLES[scanPosition]);
  return true;
}

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (length != sizeof(ScanMessage)) {
    return;
  }

  ScanMessage message;
  memcpy(&message, data, sizeof(message));
  if (message.type != MSG_SCAN_RESULT) {
    return;
  }

  portENTER_CRITICAL(&resultMux);
  cameraResultSequence = message.sequence;
  cameraResultStatus = message.status;
  cameraBirdDetected = message.birdDetected;
  cameraConfidencePermille = message.confidencePermille;
  cameraResultReady = true;
  portEXIT_CRITICAL(&resultMux);
}

void beginScan() {
  scanPosition = 0;
  setPanAngle(SCAN_PAN_ANGLES[scanPosition]);
  setTiltAngle(TILT_STRAIGHT);
  stateStartedAt = millis();
  systemState = WAITING_FOR_CAMERA_SETTLE;
  Serial.println("[Sensor] Movement detected");
  Serial.println("[Pan/Tilt] Starting scan");
}

void handleMonitoring() {
  bool motionHigh = digitalRead(MOTION_SENSOR_PIN) == HIGH;
  if (!motionHigh) {
    if (sensorWasHigh) {
      sensorLowStartedAt = millis();
      sensorWasHigh = false;
    }
    if (!sensorArmed && millis() - sensorLowStartedAt >= SENSOR_REARM_TIME) {
      sensorArmed = true;
    }
    return;
  }

  if (!sensorArmed) {
    return;
  }

  sensorWasHigh = true;
  sensorArmed = false;
  if (digitalRead(MOTION_SENSOR_PIN) == HIGH) {
    if (!espNowReady) {
      Serial.println("[ESP-NOW] Camera link unavailable; using sensor fallback");
      startSpray("sensor fallback");
      return;
    }
    beginScan();
  }
}

void handleCameraSettle() {
  if (millis() - stateStartedAt < CAMERA_SETTLE_TIME) {
    return;
  }

  if (!sendScanRequest()) {
    Serial.println("[ESP-NOW] Camera request could not be sent; using sensor fallback");
    startSpray("camera link failure");
    return;
  }

  stateStartedAt = millis();
  systemState = WAITING_FOR_CAMERA_RESULT;
}

void handleCameraResult() {
  bool ready;
  uint16_t sequence;
  uint8_t status;
  uint8_t birdDetected;
  uint16_t confidence;

  portENTER_CRITICAL(&resultMux);
  ready = cameraResultReady;
  sequence = cameraResultSequence;
  status = cameraResultStatus;
  birdDetected = cameraBirdDetected;
  confidence = cameraConfidencePermille;
  if (ready) {
    cameraResultReady = false;
  }
  portEXIT_CRITICAL(&resultMux);

  if (ready && sequence == scanSequence) {
    if (status != RESULT_AI_VALID) {
      Serial.printf("[AI] Camera/backend unavailable (status %u); sensor fallback\n", status);
      startSpray("AI unavailable");
      return;
    }

    Serial.printf("[AI] Confidence: %.2f\n", confidence / 1000.0f);
    if (birdDetected == 1 && confidence >= BIRD_CONFIDENCE_THRESHOLD_PERMILLE) {
      Serial.println("[AI] Bird confirmed by camera");
      startSpray("AI-confirmed bird");
      return;
    }

    Serial.println("[AI] No bird above confidence threshold");
    scanPosition++;
    if (scanPosition >= SCAN_POSITION_COUNT) {
      setPanAngle(PAN_CENTER);
      enterMonitoring();
      return;
    }

    setPanAngle(SCAN_PAN_ANGLES[scanPosition]);
    setTiltAngle(TILT_STRAIGHT);
    stateStartedAt = millis();
    systemState = WAITING_FOR_CAMERA_SETTLE;
    return;
  }

  if (millis() - stateStartedAt >= CAMERA_RESPONSE_TIMEOUT) {
    Serial.println("[ESP-NOW] Camera response timeout; using sensor fallback");
    startSpray("camera response timeout");
  }
}

void handleSpraying() {
  if (millis() - sprayStartedAt < SPRAY_TIME) {
    return;
  }

  setRelay(false);
  hasSprayed = true;
  cooldownStartedAt = millis();
  Serial.println("[RELAY] Pump + Valve OFF");
  Serial.println("[COOLDOWN] Starting");
  systemState = COOLDOWN;
  stateStartedAt = cooldownStartedAt;
}

void handleCooldown() {
  setRelay(false);
  if (millis() - cooldownStartedAt >= SPRAY_COOLDOWN) {
    sensorArmed = digitalRead(MOTION_SENSOR_PIN) == LOW;
    sensorLowStartedAt = millis();
    enterMonitoring();
  }
}

void setup() {
  Serial.begin(115200);

  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  relayActive = false;
  pinMode(MOTION_SENSOR_PIN, INPUT);

  panServo.attach(PAN_SERVO_PIN);
  tiltServo.attach(TILT_SERVO_PIN);
  setPanAngle(PAN_CENTER);
  setTiltAngle(TILT_STRAIGHT);

  Serial.println();
  Serial.println("====================================");
  Serial.println("AGRISENTINEL CONTROLLER ESP32");
  Serial.println("====================================");
  Serial.println("[RELAY] Pump + Valve default OFF");
  Serial.println("[Sensor] READY");
  Serial.println("[Pan Servo] READY");
  Serial.println("[Tilt Servo] READY");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected; ESP-NOW channel ");
    Serial.println(WiFi.channel());
  } else {
    Serial.println("[WiFi] Unavailable; AI camera link may not work");
  }

  Serial.print("[ESP-NOW] Controller MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() == ESP_OK) {
    espNowReady = true;
    esp_now_register_recv_cb(onEspNowReceive);
    if (!addBroadcastPeer()) {
      espNowReady = false;
    }
  }

  if (espNowReady) {
    Serial.println("[ESP-NOW] READY");
  } else {
    Serial.println("[ESP-NOW] Initialization failed; sensor fallback enabled");
  }

  enterMonitoring();
}

void loop() {
  switch (systemState) {
    case MONITORING:
      handleMonitoring();
      break;
    case WAITING_FOR_CAMERA_SETTLE:
      handleCameraSettle();
      break;
    case WAITING_FOR_CAMERA_RESULT:
      handleCameraResult();
      break;
    case SPRAYING:
      handleSpraying();
      break;
    case COOLDOWN:
      handleCooldown();
      break;
  }

  delay(5);
}
