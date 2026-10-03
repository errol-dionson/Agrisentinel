/*
  Agrisentinel ESP32-CAM
  Captures an image on scan requests, asks the backend to analyze it,
  then reports the validated result to the controller ESP32 over ESP-NOW.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include "esp_camera.h"
#include <math.h>
#include <string.h>

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* BACKEND_URL = "http://192.168.1.5:3001/api/analyze-bird";

const uint16_t BIRD_CONFIDENCE_THRESHOLD_PERMILLE = 750;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
const unsigned long AI_HTTP_TIMEOUT = 9000;

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

portMUX_TYPE requestMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool scanRequestPending = false;
volatile uint16_t pendingSequence = 0;
uint8_t controllerMac[6] = {};
bool controllerKnown = false;
bool cameraReady = false;

bool addControllerPeer() {
  if (!controllerKnown || esp_now_is_peer_exist(controllerMac)) {
    return controllerKnown;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, controllerMac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  return esp_now_add_peer(&peer) == ESP_OK;
}

void sendScanResult(uint16_t sequence, uint8_t status, bool birdDetected, uint16_t confidence) {
  if (!addControllerPeer()) {
    Serial.println("[ESP-NOW] Could not add controller peer");
    return;
  }

  ScanMessage result = {};
  result.type = MSG_SCAN_RESULT;
  result.sequence = sequence;
  result.status = status;
  result.birdDetected = birdDetected ? 1 : 0;
  result.confidencePermille = confidence;

  esp_err_t error = esp_now_send(controllerMac, (const uint8_t*)&result, sizeof(result));
  if (error != ESP_OK) {
    Serial.printf("[ESP-NOW] Result send failed: %d\n", error);
  }
}

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (length != sizeof(ScanMessage)) {
    return;
  }

  ScanMessage message;
  memcpy(&message, data, sizeof(message));
  if (message.type != MSG_SCAN_REQUEST) {
    return;
  }

  memcpy(controllerMac, info->src_addr, sizeof(controllerMac));
  controllerKnown = true;

  portENTER_CRITICAL(&requestMux);
  pendingSequence = message.sequence;
  scanRequestPending = true;
  portEXIT_CRITICAL(&requestMux);
}

bool initializeCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("[Camera] Initialization failed: 0x%x\n", error);
    return false;
  }

  Serial.println("[Camera] READY");
  return true;
}

bool analyzeCapturedImage(bool& birdDetected, uint16_t& confidencePermille) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AI] Wi-Fi unavailable");
    return false;
  }

  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == nullptr) {
    Serial.println("[Camera] Image capture failed");
    return false;
  }
  Serial.printf("[Camera] Captured %u bytes\n", static_cast<unsigned int>(frame->len));

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(AI_HTTP_TIMEOUT);
  http.setConnectTimeout(AI_HTTP_TIMEOUT);

  if (!http.begin(client, BACKEND_URL)) {
    esp_camera_fb_return(frame);
    Serial.println("[AI] Could not connect to backend");
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");
  int statusCode = http.POST(frame->buf, frame->len);
  String responseBody = http.getString();
  http.end();
  esp_camera_fb_return(frame);

  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("[AI] Backend request failed with HTTP %d\n", statusCode);
    return false;
  }

  StaticJsonDocument<768> response;
  DeserializationError parseError = deserializeJson(response, responseBody);
  if (parseError ||
      !response["success"].is<bool>() ||
      !response["bird_detected"].is<bool>() ||
      !response["confidence"].is<float>() ||
      !response["recommended_action"].is<const char*>()) {
    Serial.println("[AI] Invalid backend response");
    return false;
  }

  float confidence = response["confidence"].as<float>();
  const char* action = response["recommended_action"].as<const char*>();
  if (!response["success"].as<bool>() ||
      !isfinite(confidence) ||
      confidence < 0.0f ||
      confidence > 1.0f) {
    Serial.println("[AI] Backend returned an invalid analysis");
    return false;
  }

  confidencePermille = static_cast<uint16_t>(confidence * 1000.0f + 0.5f);
  birdDetected = response["bird_detected"].as<bool>() &&
                 confidencePermille >= BIRD_CONFIDENCE_THRESHOLD_PERMILLE &&
                 strcmp(action, "deter") == 0;

  Serial.printf("[AI] Bird %s, confidence %.2f\n",
                birdDetected ? "detected" : "not confirmed",
                confidence);
  return true;
}

void processScanRequest(uint16_t sequence) {
  Serial.printf("[ESP-NOW] Scan request %u received\n", sequence);
  if (!cameraReady) {
    sendScanResult(sequence, RESULT_CAMERA_ERROR, false, 0);
    return;
  }

  bool birdDetected = false;
  uint16_t confidencePermille = 0;
  if (!analyzeCapturedImage(birdDetected, confidencePermille)) {
    sendScanResult(sequence, RESULT_AI_UNAVAILABLE, false, 0);
    return;
  }

  sendScanResult(sequence, RESULT_AI_VALID, birdDetected, confidencePermille);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("====================================");
  Serial.println("AGRISENTINEL ESP32-CAM");
  Serial.println("====================================");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected; ESP-NOW channel ");
    Serial.println(WiFi.channel());
  } else {
    Serial.println("[WiFi] Unavailable; AI analysis will use fallback");
  }

  Serial.print("[ESP-NOW] Camera MAC: ");
  Serial.println(WiFi.macAddress());

  cameraReady = initializeCamera();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Initialization failed");
    return;
  }

  esp_now_register_recv_cb(onEspNowReceive);
  Serial.println("[ESP-NOW] READY; waiting for scan commands");
}

void loop() {
  bool requestReady;
  uint16_t sequence;

  portENTER_CRITICAL(&requestMux);
  requestReady = scanRequestPending;
  sequence = pendingSequence;
  if (requestReady) {
    scanRequestPending = false;
  }
  portEXIT_CRITICAL(&requestMux);

  if (requestReady) {
    processScanRequest(sequence);
  }

  delay(5);
}
