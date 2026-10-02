/*
  Agrisentinel Smart Bird Detection Controller
  - Maintains the original pump + valve deterrent behavior
  - Adds AI detection via backend + OpenRouter
  - Falls back to sensor-driven motion detection if AI is unavailable
  - Uses a non-blocking state machine and millis() timing
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <esp_camera.h>
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#define RELAY_PIN 25
#define PAN_SERVO_PIN 13
#define TILT_SERVO_PIN 14
#define MOTION_SENSOR_PIN 34

// Most relay modules are active-low
#define RELAY_ON LOW
#define RELAY_OFF HIGH

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* BACKEND_URL = "http://192.168.1.50:3001/api/analyze-bird";

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

enum SystemState {
  STARTUP,
  MONITORING,
  SENSOR_DETECTED,
  SCANNING,
  AI_ANALYSIS,
  BIRD_DETECTED,
  SPRAYING,
  COOLDOWN,
  AI_UNAVAILABLE
};

Servo panServo;
Servo tiltServo;
SystemState currentState = STARTUP;
unsigned long stateEnteredAt = 0;
unsigned long sprayStartedAt = 0;
unsigned long lastSprayAt = 0;
unsigned long lastMotionAt = 0;
unsigned long lastScanStepAt = 0;

bool relayActive = false;
bool motionDetected = false;
bool birdConfirmed = false;
int scanIndex = 0;
int scanOrder[] = { PAN_CENTER, PAN_LEFT, PAN_CENTER, PAN_RIGHT, PAN_CENTER };
int scanCount = 5;

void printBanner() {
  Serial.println();
  Serial.println("====================================");
  Serial.println("SMART BIRD DETECTION SYSTEM");
  Serial.println("====================================");
  Serial.println("ESP32: OK");
  Serial.println("ESP32-CAM: READY");
  Serial.println("Sensor: READY");
  Serial.println("Pan Servo: READY");
  Serial.println("Tilt Servo: READY");
  Serial.println("Relay: READY");
}

void changeState(SystemState newState) {
  currentState = newState;
  stateEnteredAt = millis();

  switch (currentState) {
    case STARTUP:
      Serial.println("[SYSTEM] STARTUP");
      break;
    case MONITORING:
      Serial.println("[SYSTEM] MONITORING");
      break;
    case SENSOR_DETECTED:
      Serial.println("[Sensor] Movement detected");
      break;
    case SCANNING:
      Serial.println("[Pan/Tilt] Starting scan");
      break;
    case AI_ANALYSIS:
      Serial.println("[AI] Sending image to backend");
      break;
    case BIRD_DETECTED:
      Serial.println("[AI] Bird detected");
      break;
    case SPRAYING:
      Serial.println("[RELAY] Pump + Valve ON");
      break;
    case COOLDOWN:
      Serial.println("[COOLDOWN] Starting");
      break;
    case AI_UNAVAILABLE:
      Serial.println("[SYSTEM] Switching to SENSOR MODE");
      break;
    default:
      break;
  }
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WiFi] CONNECTED");
    return true;
  }

  Serial.println();
  Serial.println("[WiFi] FAILED");
  return false;
}

void setRelay(bool enabled) {
  relayActive = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ON : RELAY_OFF);
}

void setPanAngle(int angle) {
  int clamped = constrain(angle, PAN_LEFT, PAN_RIGHT);
  panServo.write(clamped);
}

void setTiltAngle(int angle) {
  int clamped = constrain(angle, TILT_MIN, TILT_MAX);
  tiltServo.write(clamped);
}

void moveCameraToPosition(int panAngle, int tiltAngle) {
  setPanAngle(panAngle);
  setTiltAngle(tiltAngle);
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] init failed with error 0x%x\n", err);
    return false;
  }

  Serial.println("[Camera] CONNECTED");
  return true;
}

bool isMotionDetected() {
  return digitalRead(MOTION_SENSOR_PIN) == HIGH;
}

bool analyzeImageWithBackend() {
  if (!connectWiFi()) {
    Serial.println("[AI] Backend unavailable: Wi-Fi is disconnected");
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Camera] Failed to capture image");
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(AI_TIMEOUT);

  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("[AI] HTTP begin failed");
    esp_camera_fb_return(fb);
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");

  int httpCode = http.POST(fb->buf, fb->len);
  String payload = http.getString();
  http.end();
  esp_camera_fb_return(fb);

  if (httpCode != 200) {
    Serial.printf("[AI] Request failed: HTTP %d | %s\n", httpCode, payload.substring(0, 120).c_str());
    return false;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("[AI] Malformed JSON response");
    return false;
  }

  if (!doc.containsKey("bird_detected") || !doc.containsKey("confidence")) {
    Serial.println("[AI] Invalid response schema");
    return false;
  }

  bool birdDetected = doc["bird_detected"] | false;
  float confidence = doc["confidence"] | 0.0f;

  if (birdDetected && confidence >= BIRD_CONFIDENCE_THRESHOLD) {
    Serial.printf("[AI] Confidence: %.2f\n", confidence);
    return true;
  }

  Serial.printf("[AI] Below threshold: %.2f\n", confidence);
  return false;
}

void triggerSpray() {
  if (millis() - lastSprayAt < SPRAY_COOLDOWN) {
    Serial.println("[COOLDOWN] Spray still cooling down");
    return;
  }

  setRelay(true);
  sprayStartedAt = millis();
  birdConfirmed = true;
  changeState(SPRAYING);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(MOTION_SENSOR_PIN, INPUT);

  setRelay(false);

  panServo.attach(PAN_SERVO_PIN);
  tiltServo.attach(TILT_SERVO_PIN);

  moveCameraToPosition(PAN_CENTER, TILT_STRAIGHT);
  delay(150);

  printBanner();

  bool wifiConnected = connectWiFi();
  bool cameraReady = initCamera();

  if (wifiConnected) {
    Serial.println("[AI BACKEND] AVAILABLE");
  } else {
    Serial.println("[AI BACKEND] UNAVAILABLE");
  }

  if (!cameraReady) {
    Serial.println("[SYSTEM] Camera unavailable. Sensor mode will be used.");
  }

  changeState(STARTUP);
  delay(500);
  changeState(MONITORING);
}

void handleMonitoring() {
  if (isMotionDetected()) {
    lastMotionAt = millis();
    scanIndex = 0;
    changeState(SENSOR_DETECTED);
  }
}

void handleSensorDetected() {
  moveCameraToPosition(PAN_CENTER, TILT_STRAIGHT);
  lastScanStepAt = millis();
  changeState(SCANNING);
}

void handleScanning() {
  static unsigned long positionSettleSince = 0;

  if (positionSettleSince == 0) {
    positionSettleSince = millis();
    moveCameraToPosition(scanOrder[scanIndex], TILT_STRAIGHT);
    Serial.printf("[Camera] Position %d -> %d\n", scanIndex, scanOrder[scanIndex]);
  }

  if (millis() - positionSettleSince > CAMERA_SETTLE_TIME) {
    positionSettleSince = 0;
    changeState(AI_ANALYSIS);
  }
}

void handleAiAnalysis() {
  static bool analysisStarted = false;

  if (!analysisStarted) {
    analysisStarted = true;
    bool aiResult = analyzeImageWithBackend();

    if (aiResult) {
      analysisStarted = false;
      changeState(BIRD_DETECTED);
      return;
    }

    if (scanIndex >= scanCount - 1) {
      analysisStarted = false;
      changeState(MONITORING);
      return;
    }

    scanIndex++;
    analysisStarted = false;
    changeState(SCANNING);
  }
}

void handleBirdDetected() {
  triggerSpray();
}

void handleSpraying() {
  if (!relayActive) {
    setRelay(true);
  }

  if (millis() - sprayStartedAt >= SPRAY_TIME) {
    setRelay(false);
    lastSprayAt = millis();
    Serial.println("[RELAY] Pump + Valve OFF");
    changeState(COOLDOWN);
  }
}

void handleCooldown() {
  if (millis() - lastSprayAt >= SPRAY_COOLDOWN) {
    changeState(MONITORING);
  }
}

void handleAiUnavailable() {
  if (isMotionDetected()) {
    triggerSpray();
  } else {
    changeState(MONITORING);
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] WiFi connected");
  }

  switch (currentState) {
    case STARTUP:
      break;
    case MONITORING:
      handleMonitoring();
      break;
    case SENSOR_DETECTED:
      handleSensorDetected();
      break;
    case SCANNING:
      handleScanning();
      break;
    case AI_ANALYSIS:
      handleAiAnalysis();
      break;
    case BIRD_DETECTED:
      handleBirdDetected();
      break;
    case SPRAYING:
      handleSpraying();
      break;
    case COOLDOWN:
      handleCooldown();
      break;
    case AI_UNAVAILABLE:
      handleAiUnavailable();
      break;
    default:
      break;
  }

  if (WiFi.status() != WL_CONNECTED && currentState != SPRAYING && currentState != COOLDOWN) {
    if (motionDetected && currentState != AI_UNAVAILABLE) {
      changeState(AI_UNAVAILABLE);
    }
  }

  delay(100);
}