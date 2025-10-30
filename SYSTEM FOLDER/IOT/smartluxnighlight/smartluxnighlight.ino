#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#define DHTType DHT11
// Pins
const int lightSensorPin = 34;
const int motionSensorPin = 26;
const int DHTPin = 4;
const int redPin = 14;
const int greenPin = 12;
const int bluePin = 13;
DHT dht(DHTPin, DHTType);
// Thresholds and timings
const int lightThreshold = 4095;
const int darkThreshold = 3700;
const int maxBrightness = 100;
const float coldThreshold = 22.0;
const float hotThreshold = 30.0;
const unsigned long motionTimeout = 60000;
const unsigned long dhtInterval = 2000;
const unsigned long sendInterval = 10000;
const unsigned long debounceDelay = 50;
const unsigned long highTempDebounce = 60000;
// WiFi and Firebase configuration
const char* WIFI_SSID = "secret";
const char* WIFI_PASSWORD = "secret";
const char* DEVICE_ID = "device01";
const char* DEVICE_LABEL = "Bedroom Lamp";
const char* FIREBASE_DATABASE_URL = "https://smartlux-night-light-default-rtdb.asia-southeast1.firebasedatabase.app";
// ISR-shared state
volatile bool motionTriggeredISR = false;
volatile unsigned long lastMotionTime = 0;
volatile unsigned long lastTriggerTime = 0;
bool ledsActive = false;
// Remote override state (0: off, 1: on, 2: automatic)
volatile int remoteLedState = 2;
unsigned long lastLedFetchMs = 0;
const unsigned long ledPollIntervalMs = 60000;
// Remote reboot control
volatile int remoteRebootState = 0;
unsigned long lastRebootFetchMs = 0;
const unsigned long rebootPollIntervalMs = 1000;
const unsigned long rebootSoftWaitMs = 5000;
const int rebootPutMaxAttempts = 5;
// Sensor data queue
typedef struct {
  int ldr;
  bool motion;
  float humidity;
  float temperature;
  unsigned long epoch_ms;
  char iso[32];
} SensorData;
QueueHandle_t sensorQueue = nullptr;
// Notification configuration
typedef struct {
  String option; // "none", "hass", "all"
  String motion_url;
  String temp_url;
  float high_temp_threshold;
  unsigned long lastFetchedMs;
} NotifyConfig;
NotifyConfig notifyCfg = {
  .option = "none",
  .motion_url = "",
  .temp_url = "",
  .high_temp_threshold = hotThreshold,
  .lastFetchedMs = 0
};
enum EventType { EVT_MOTION = 0, EVT_HIGH_TEMP = 1 };
typedef struct {
  EventType type;
  unsigned long epoch_ms;
  int ldr;
  float temperature;
} NotifyEvent;
QueueHandle_t notifyQueue = nullptr;
// Function prototypes
void connectWiFi();
bool enqueueSensorData(int ldr, bool motion, float hum, float temp);
void firebaseTask(void* pvParameters);
void notifyTask(void* pvParameters);
void fetchNotifyConfigOnce();
void controlTask(void* pvParameters);
// Motion ISR
void IRAM_ATTR motionISR() {
  TickType_t t = xTaskGetTickCountFromISR();
  unsigned long now = (unsigned long)t * portTICK_PERIOD_MS;
  if (now - lastTriggerTime >= debounceDelay) {
    motionTriggeredISR = true;
    lastMotionTime = now;
    lastTriggerTime = now;
  }
}
void setup() {
  Serial.begin(115200);
  delay(10);
  pinMode(lightSensorPin, INPUT);
  pinMode(motionSensorPin, INPUT);
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  dht.begin();
  // Ensure PWM starts off
  analogWrite(redPin, 0);
  analogWrite(greenPin, 0);
  analogWrite(bluePin, 0);
  connectWiFi();
  // Create queues
  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  notifyQueue = xQueueCreate(32, sizeof(NotifyEvent));
  if (!sensorQueue) {
    Serial.println("Failed to create sensor queue!");
  } else {
    xTaskCreatePinnedToCore(firebaseTask, "firebaseTask", 16384, NULL, 1, NULL, 1);
  }
  if (!notifyQueue) {
    Serial.println("Failed to create notify queue!");
  } else {
    xTaskCreatePinnedToCore(notifyTask, "notifyTask", 12288, NULL, 2, NULL, 0);
  }
  // Attach interrupt
  attachInterrupt(digitalPinToInterrupt(motionSensorPin), motionISR, RISING);
  // NTP time sync
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  Serial.println("Waiting for NTP time sync (up to 10s)...");
  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1600000000ULL && millis() - start < 10000) {
    delay(200);
    now = time(nullptr);
  }
  if (now >= 1600000000ULL) Serial.println("NTP time synced.");
  else Serial.println("NTP time not available; will use millis() timestamps as fallback.");
  // Fetch initial notify config
  fetchNotifyConfigOnce();
  // Start control streaming task
  xTaskCreatePinnedToCore(controlTask, "controlTask", 8192, NULL, 3, NULL, 1);
}
unsigned long lastDhtRead = 0;
unsigned long lastSendTime = 0;
void loop() {
  int lightValue = analogRead(lightSensorPin);
  // Check motion
  bool motionDetected = false;
  static unsigned long lastPollTrigger = 0;
  static bool lastPollState = LOW;
  if (motionTriggeredISR) {
    motionTriggeredISR = false;
    motionDetected = true;
  } else {
    bool current = digitalRead(motionSensorPin);
    if (current == HIGH && lastPollState == LOW && millis() - lastPollTrigger >= debounceDelay) {
      motionDetected = true;
      lastMotionTime = millis();
      lastPollTrigger = millis();
    }
    lastPollState = current;
  }
  if (lastPollState == HIGH) {
    lastMotionTime = millis();
    ledsActive = true;
  }
  // Update LED state based on timeout
  if (motionDetected) ledsActive = true;
  else if (ledsActive && (millis() - lastMotionTime >= motionTimeout)) ledsActive = false;
  // Read DHT periodically
  static float humidity = NAN;
  static float temperature = NAN;
  unsigned long nowMs = millis();
  bool didDhtRead = false;
  if (nowMs - lastDhtRead >= dhtInterval) {
    lastDhtRead = nowMs;
    didDhtRead = true;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h)) humidity = h;
    if (!isnan(t)) temperature = t;
    Serial.printf("[DHT] T=%.1f H=%.1f\n", (isnan(temperature)?0.0:temperature), (isnan(humidity)?0.0:humidity));
  }
  int brightness;
  int redPWM = 0, greenPWM = 0, bluePWM = 0;
  // Apply remote override
  if (remoteLedState == 1) {
    ledsActive = true;
    brightness = maxBrightness;
  } else if (remoteLedState == 0) {
    ledsActive = false;
  } else {
    brightness = map(lightValue, darkThreshold, lightThreshold, 0, maxBrightness);
  }
  brightness = constrain(brightness, 0, maxBrightness);
  if (ledsActive && (remoteLedState == 2)) {
    if (!isnan(temperature)) {
      if (temperature < coldThreshold) {
        bluePWM = brightness;
      } else if (temperature > hotThreshold) {
        redPWM = brightness;
        greenPWM = (int)(brightness * 0.45);
      } else {
        greenPWM = brightness;
      }
    } 
  } else if (ledsActive && (remoteLedState == 1)) {
    redPWM = brightness;
    greenPWM = brightness;
    bluePWM = brightness;
  }
  // Apply PWM
  analogWrite(redPin, redPWM);
  analogWrite(greenPin, greenPWM);
  analogWrite(bluePin, bluePWM);
  // Debug output
  Serial.printf("LDR=%d Motion=%d Bright=%d Temp=%.1f Hum=%.1f LED[R,G,B]=[%d,%d,%d] remote=%d\n",
                lightValue, motionDetected, brightness,
                (isnan(temperature)?0.0:temperature),
                (isnan(humidity)?0.0:humidity),
                redPWM, greenPWM, bluePWM,
                remoteLedState);
  // Enqueue motion event
  if (motionDetected) {
    NotifyEvent ev;
    time_t nowt = time(nullptr);
    ev.epoch_ms = (nowt > 0) ? (unsigned long)nowt * 1000UL : millis();
    ev.type = EVT_MOTION;
    ev.ldr = lightValue;
    ev.temperature = isnan(temperature) ? NAN : temperature;
    if (notifyQueue) xQueueSend(notifyQueue, &ev, 0);
    bool ok = enqueueSensorData(lightValue, motionDetected, humidity, temperature);
    if (!ok) Serial.println("Warning: queue full, dropped motion data");
  }
  // Enqueue high temperature event
  static unsigned long lastHighTempTrigger = 0;
  if (didDhtRead && !isnan(temperature)) {
    float threshold = notifyCfg.high_temp_threshold;
    if (temperature > threshold && nowMs - lastHighTempTrigger >= highTempDebounce) {
      NotifyEvent ev;
      time_t nowt = time(nullptr);
      ev.epoch_ms = (nowt > 0) ? (unsigned long)nowt * 1000UL : millis();
      ev.type = EVT_HIGH_TEMP;
      ev.ldr = lightValue;
      ev.temperature = temperature;
      if (notifyQueue) xQueueSend(notifyQueue, &ev, 0);
      lastHighTempTrigger = nowMs;
    }
  }
  // Enqueue periodic sensor data
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    bool ok = enqueueSensorData(lightValue, motionDetected, humidity, temperature);
    if (!ok) Serial.println("Warning: queue full, dropped data");
  }
  delay(20);
}
// Enqueue sensor data
bool enqueueSensorData(int ldr, bool motion, float hum, float temp) {
  if (!sensorQueue) return false;
  SensorData d;
  d.ldr = ldr;
  d.motion = motion;
  d.humidity = hum;
  d.temperature = temp;
  time_t now = time(nullptr);
  if (now > 0) {
    d.epoch_ms = (unsigned long)now * 1000UL;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    strftime(d.iso, sizeof(d.iso), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  } else {
    d.epoch_ms = (unsigned long)millis();
    d.iso[0] = '\0';
  }
  BaseType_t res = xQueueSend(sensorQueue, &d, 0);
  if (res == pdTRUE) return true;
  // Drop oldest and retry
  SensorData tmp;
  if (xQueueReceive(sensorQueue, &tmp, 0) == pdTRUE) {
    if (xQueueSend(sensorQueue, &d, 0) == pdTRUE) return true;
  }
  return false;
}
// Notification task: processes events and sends webhooks
void notifyTask(void* pvParameters) {
  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient http;
  http.setConnectTimeout(60000);
  http.setTimeout(60000);
  for (;;) {
    NotifyEvent ev;
    if (xQueueReceive(notifyQueue, &ev, portMAX_DELAY) == pdTRUE) {
      if (notifyCfg.option == "none") {
        Serial.println("[notifyTask] notify.option == none, skipping event.");
        continue;
      }
      String targetUrl = "";
      if (ev.type == EVT_MOTION) targetUrl = notifyCfg.motion_url;
      else if (ev.type == EVT_HIGH_TEMP) targetUrl = notifyCfg.temp_url;
      if (targetUrl.length() == 0) {
        Serial.println("[notifyTask] No target URL configured, skipping.");
        continue;
      }
      // Force HTTPS
      if (targetUrl.startsWith("http://")) {
        targetUrl = "https://" + targetUrl.substring(7);
        Serial.printf("[notifyTask] Forced HTTPS: %s\n", targetUrl.c_str());
      }
      // Build payload
      StaticJsonDocument<256> doc;
      doc["device"] = DEVICE_ID;
      doc["event"] = (ev.type == EVT_MOTION) ? "motion" : "high_temperature";
      doc["timestamp_ms"] = ev.epoch_ms;
      if (ev.type == EVT_MOTION) {
        doc["ldr"] = ev.ldr;
        if (!isnan(ev.temperature)) doc["temperature"] = ev.temperature;
      } else {
        doc["temperature"] = ev.temperature;
        doc["threshold"] = notifyCfg.high_temp_threshold;
      }
      String payload;
      serializeJson(doc, payload);
      Serial.printf("[notifyTask] POST -> %s\n", targetUrl.c_str());
      Serial.printf("[notifyTask] Payload: %s\n", payload.c_str());
      Serial.printf("[notifyTask] Payload length: %d bytes\n", payload.length());
      // Reconnect WiFi if needed
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[notifyTask] WiFi disconnected, attempting reconnect...");
        connectWiFi();
      }
      bool success = false;
      const int maxRetries = 3;
      for (int retry = 0; retry < maxRetries; ++retry) {
        if (http.begin(*client, targetUrl)) {
          http.addHeader("Content-Type", "application/json");
          http.addHeader("Content-Length", String(payload.length()));
          http.addHeader("Connection", "close");
          int httpCode = http.POST(payload);
          if (httpCode > 0) {
            Serial.printf("[notifyTask] HTTP %d\n", httpCode);
            String resp = http.getString();
            Serial.printf("[notifyTask] Response: %s\n", resp.c_str());
            success = true;
          } else {
            Serial.printf("[notifyTask] POST failed: %s (retry %d/%d)\n", http.errorToString(httpCode).c_str(), retry + 1, maxRetries);
            Serial.printf("[notifyTask] WiFi status: %d\n", WiFi.status());
            Serial.printf("[notifyTask] Free heap: %d bytes\n", ESP.getFreeHeap());
          }
          http.end();
          if (success) break;
        } else {
          Serial.printf("[notifyTask] HTTP begin failed (retry %d/%d)\n", retry + 1, maxRetries);
          if (!client->connect(targetUrl.substring(8).c_str(), 443)) {
            Serial.println("[notifyTask] Direct connect to server failed");
          } else {
            Serial.println("[notifyTask] Direct connect succeeded, issue with HTTP setup");
            client->stop();
          }
        }
        vTaskDelay(5000 * (retry + 1) / portTICK_PERIOD_MS);
      }
      if (!success) {
        Serial.println("[notifyTask] All retries failed for this event.");
      }
      // Cleanup
      doc.clear();
      payload = "";
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }
  delete client;
  vTaskDelete(NULL);
}
// Firebase task: sends sensor data and handles periodic fetches
void firebaseTask(void* pvParameters) {
  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient https;
  for (;;) {
    SensorData data;
    // Process sensor queue
    while (sensorQueue && xQueueReceive(sensorQueue, &data, 0) == pdTRUE) {
      String url = String(FIREBASE_DATABASE_URL);
      if (!url.endsWith("/")) url += "/";
      url += "devices/";
      url += DEVICE_ID;
      url += "/sensors.json";
      char payload[512];
      if (!isnan(data.humidity) && !isnan(data.temperature)) {
        snprintf(payload, sizeof(payload),
          "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":%.2f,\"temperature\":%.2f}",
          DEVICE_ID, DEVICE_LABEL, (unsigned long)(data.epoch_ms/1000UL), data.epoch_ms, data.iso,
          data.ldr, data.motion ? "true" : "false", data.humidity, data.temperature);
      } else if (!isnan(data.humidity)) {
        snprintf(payload, sizeof(payload),
          "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":%.2f,\"temperature\":null}",
          DEVICE_ID, DEVICE_LABEL, (unsigned long)(data.epoch_ms/1000UL), data.epoch_ms, data.iso,
          data.ldr, data.motion ? "true" : "false", data.humidity);
      } else if (!isnan(data.temperature)) {
        snprintf(payload, sizeof(payload),
          "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":null,\"temperature\":%.2f}",
          DEVICE_ID, DEVICE_LABEL, (unsigned long)(data.epoch_ms/1000UL), data.epoch_ms, data.iso,
          data.ldr, data.motion ? "true" : "false", data.temperature);
      } else {
        snprintf(payload, sizeof(payload),
          "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":null,\"temperature\":null}",
          DEVICE_ID, DEVICE_LABEL, (unsigned long)(data.epoch_ms/1000UL), data.epoch_ms, data.iso,
          data.ldr, data.motion ? "true" : "false");
      }
      Serial.print("[FirebaseTask] POST ");
      Serial.println(url);
      Serial.println(payload);
      if (https.begin(*client, url)) {
        https.addHeader("Content-Type", "application/json");
        int httpCode = https.POST(String(payload));
        if (httpCode > 0) {
          String response = https.getString();
          Serial.printf("[FirebaseTask] HTTP %d\n", httpCode);
          Serial.println(response);
        } else {
          Serial.printf("[FirebaseTask] POST failed: %s\n", https.errorToString(httpCode).c_str());
        }
        https.end();
      } else {
        Serial.println("[FirebaseTask] begin failed (POST)");
      }
      delay(10);
    }
    // Fetch notify config periodically
    unsigned long tNow = millis();
    if (tNow - notifyCfg.lastFetchedMs >= 10000UL) {
      String cfgUrl = String(FIREBASE_DATABASE_URL);
      if (!cfgUrl.endsWith("/")) cfgUrl += "/";
      cfgUrl += "devices/";
      cfgUrl += DEVICE_ID;
      cfgUrl += "/notify.json";
      Serial.print("[FirebaseTask] GET notify config ");
      Serial.println(cfgUrl);
      if (https.begin(*client, cfgUrl)) {
        int httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
          String body = https.getString();
          Serial.println("[FirebaseTask] notify config body:");
          Serial.println(body);
          StaticJsonDocument<512> doc;
          DeserializationError err = deserializeJson(doc, body);
          if (!err) {
            if (doc.containsKey("option")) notifyCfg.option = String((const char*)doc["option"]);
            if (doc.containsKey("motion_url")) notifyCfg.motion_url = String((const char*)doc["motion_url"]);
            if (doc.containsKey("temp_url")) notifyCfg.temp_url = String((const char*)doc["temp_url"]);
            if (doc.containsKey("high_temp_threshold")) {
              notifyCfg.high_temp_threshold = doc["high_temp_threshold"].as<float>();
            } else {
              notifyCfg.high_temp_threshold = hotThreshold;
            }
            notifyCfg.lastFetchedMs = tNow;
            Serial.println("[FirebaseTask] notify config updated:");
            Serial.printf(" option=%s\n motion_url=%s\n temp_url=%s\n threshold=%.2f\n",
                          notifyCfg.option.c_str(),
                          notifyCfg.motion_url.c_str(),
                          notifyCfg.temp_url.c_str(),
                          notifyCfg.high_temp_threshold);
          } else {
            Serial.print("[FirebaseTask] JSON parse failed: ");
            Serial.println(err.c_str());
          }
        } else {
          Serial.printf("[FirebaseTask] GET notify failed: %d\n", httpCode);
        }
        https.end();
      } else {
        Serial.println("[FirebaseTask] begin failed (GET notify)");
      }
    }
    // Fetch remote reboot state
    if (tNow - lastRebootFetchMs >= rebootPollIntervalMs) {
      lastRebootFetchMs = tNow;
      String rebootUrl = String(FIREBASE_DATABASE_URL);
      if (!rebootUrl.endsWith("/")) rebootUrl += "/";
      rebootUrl += "devices/";
      rebootUrl += DEVICE_ID;
      rebootUrl += "/control/reboot.json";
      Serial.print("[FirebaseTask] GET remote reboot state ");
      Serial.println(rebootUrl);
      if (https.begin(*client, rebootUrl)) {
        int httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
          String body = https.getString();
          body.trim();
          Serial.print("[FirebaseTask] reboot body: ");
          Serial.println(body);
          if (body == "1") {
            remoteRebootState = 1;
          } else if (body == "0") {
            remoteRebootState = 0;
          } else if (body == "null" || body.length() == 0) {
            remoteRebootState = 0;
          } else {
            StaticJsonDocument<16> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (!err) {
              int v = doc.as<int>();
              remoteRebootState = (v == 1) ? 1 : 0;
            } else {
              remoteRebootState = 0;
            }
          }
        } else {
          Serial.printf("[FirebaseTask] GET reboot failed: %d\n", httpCode);
        }
        https.end();
      } else {
        Serial.println("[FirebaseTask] begin failed (GET reboot)");
      }
    }
    // Handle reboot
    if (remoteRebootState == 1) {
      Serial.println("[FirebaseTask] Reboot requested. Soft wait then attempt to set reboot=0 before restarting.");
      vTaskDelay(rebootSoftWaitMs / portTICK_PERIOD_MS);
      String rebootUrlPut = String(FIREBASE_DATABASE_URL);
      if (!rebootUrlPut.endsWith("/")) rebootUrlPut += "/";
      rebootUrlPut += "devices/";
      rebootUrlPut += DEVICE_ID;
      rebootUrlPut += "/control/reboot.json";
      bool putSuccess = false;
      for (int attempt = 0; attempt < rebootPutMaxAttempts && !putSuccess; ++attempt) {
        Serial.printf("[FirebaseTask] Attempting to PUT 0 -> %s (attempt %d)\n", rebootUrlPut.c_str(), attempt+1);
        if (https.begin(*client, rebootUrlPut)) {
          https.addHeader("Content-Type", "application/json");
          int putCode = https.PUT(String("0"));
          if (putCode > 0 && putCode >= 200 && putCode < 300) {
            Serial.printf("[FirebaseTask] PUT success HTTP %d\n", putCode);
            putSuccess = true;
          } else {
            Serial.printf("[FirebaseTask] PUT failed: code=%d error=%s\n", putCode, https.errorToString(putCode).c_str());
          }
          https.end();
        } else {
          Serial.println("[FirebaseTask] begin failed (PUT reboot)");
        }
        if (!putSuccess) {
          vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
      }
      if (putSuccess) {
        Serial.println("[FirebaseTask] Reboot flag cleared on server — restarting now.");
        delay(200);
        ESP.restart();
      } else {
        Serial.println("[FirebaseTask] Failed to clear reboot flag on server. Will retry next polling cycle.");
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  delete client;
  vTaskDelete(NULL);
}
// Fetch initial notify config
void fetchNotifyConfigOnce() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String cfgUrl = String(FIREBASE_DATABASE_URL);
  if (!cfgUrl.endsWith("/")) cfgUrl += "/";
  cfgUrl += "devices/";
  cfgUrl += DEVICE_ID;
  cfgUrl += "/notify.json";
  Serial.print("[fetchNotifyConfigOnce] GET ");
  Serial.println(cfgUrl);
  if (https.begin(client, cfgUrl)) {
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      String body = https.getString();
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, body);
      if (!err) {
        if (doc.containsKey("option")) notifyCfg.option = String((const char*)doc["option"]);
        if (doc.containsKey("motion_url")) notifyCfg.motion_url = String((const char*)doc["motion_url"]);
        if (doc.containsKey("temp_url")) notifyCfg.temp_url = String((const char*)doc["temp_url"]);
        if (doc.containsKey("high_temp_threshold")) {
          notifyCfg.high_temp_threshold = doc["high_temp_threshold"].as<float>();
        } else {
          notifyCfg.high_temp_threshold = hotThreshold;
        }
        notifyCfg.lastFetchedMs = millis();
        Serial.println("[fetchNotifyConfigOnce] notify config loaded");
      } else {
        Serial.print("[fetchNotifyConfigOnce] JSON parse failed: ");
        Serial.println(err.c_str());
      }
    } else {
      Serial.printf("[fetchNotifyConfigOnce] GET failed: %d\n", httpCode);
    }
    https.end();
  } else {
    Serial.println("[fetchNotifyConfigOnce] begin failed");
  }
}
void connectWiFi() {
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 15000) {
      Serial.println("\nWiFi connect timed out; retrying...");
      start = millis();
    }
  }
  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}
// Control task: listens for Firebase SSE updates
void controlTask(void* pvParameters) {
  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  String url = String(FIREBASE_DATABASE_URL);
  if (url.startsWith("https://")) url = url.substring(8);
  else if (url.startsWith("http://")) url = url.substring(7);
  int slash = url.indexOf('/');
  String host = (slash == -1) ? url : url.substring(0, slash);
  String basePath = (slash == -1) ? String("") : url.substring(slash);
  String streamPath = basePath + "/devices/" + DEVICE_ID + "/control.json";
  const unsigned long reconnectDelayMs = 500;
  const unsigned long socketIdleTimeoutMs = 30000;
  for (;;) {
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.printf("[controlTask] connecting to %s ...\n", host.c_str());
    if (!client->connect(host.c_str(), 443)) {
      Serial.println("[controlTask] connect failed, retrying...");
      vTaskDelay(reconnectDelayMs / portTICK_PERIOD_MS);
      continue;
    }
    // Send GET request for SSE
    String req = String("GET ") + streamPath + " HTTP/1.1\r\n"
                 "Host: " + host + "\r\n"
                 "Accept: text/event-stream\r\n"
                 "Connection: keep-alive\r\n\r\n";
    client->print(req);
    unsigned long lastRecv = millis();
    String line;
    String dataBuf;
    // Skip headers
    while (client->connected()) {
      line = client->readStringUntil('\n');
      line.trim();
      if (line.length() == 0) break;
      if (millis() - lastRecv > 5000) break;
    }
    Serial.println("[controlTask] stream established, listening...");
    // Process SSE
    while (client->connected()) {
      if (client->available()) {
        lastRecv = millis();
        line = client->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
          if (dataBuf.length() > 0) {
            StaticJsonDocument<768> doc;
            DeserializationError err = deserializeJson(doc, dataBuf);
            if (!err) {
              if (doc.containsKey("data")) {
                JsonVariant data = doc["data"];
                if (data.is<JsonObject>()) {
                  JsonObject obj = data.as<JsonObject>();
                  if (obj.containsKey("led")) {
                    JsonVariant v = obj["led"];
                    if (v.is<int>()) {
                      int ledv = v.as<int>();
                      if (ledv == 0) remoteLedState = 0;
                      else if (ledv == 1) remoteLedState = 1;
                      else remoteLedState = 2;
                      lastLedFetchMs = millis();
                      Serial.printf("[controlTask] streamed led=%d\n", remoteLedState);
                    } else if (v.isNull()) {
                      remoteLedState = 2;
                      lastLedFetchMs = millis();
                      Serial.println("[controlTask] streamed led=null -> cleared override");
                    } else {
                      String s; serializeJson(v, s); s.trim();
                      if (s == "0") remoteLedState = 0;
                      else if (s == "1") remoteLedState = 1;
                      else remoteLedState = 2;
                      lastLedFetchMs = millis();
                      Serial.printf("[controlTask] streamed led(str)=%d\n", remoteLedState);
                    }
                  }
                  if (obj.containsKey("reboot")) {
                    JsonVariant r = obj["reboot"];
                    if (r.is<int>()) {
                      remoteRebootState = (r.as<int>() == 1) ? 1 : 0;
                      lastRebootFetchMs = millis();
                      Serial.printf("[controlTask] streamed reboot=%d\n", remoteRebootState);
                    } else if (r.isNull()) {
                      remoteRebootState = 0;
                      lastRebootFetchMs = millis();
                      Serial.println("[controlTask] streamed reboot=null -> cleared");
                    } else {
                      String s; serializeJson(r, s); s.trim();
                      if (s == "1") remoteRebootState = 1;
                      else remoteRebootState = 0;
                      lastRebootFetchMs = millis();
                      Serial.printf("[controlTask] streamed reboot(str)=%d\n", remoteRebootState);
                    }
                  }
                } else {
                  if (data.is<int>()) {
                    int v = data.as<int>();
                    if (v == 0) remoteLedState = 0;
                    else if (v == 1) remoteLedState = 1;
                    else remoteLedState = 2;
                    lastLedFetchMs = millis();
                    Serial.printf("[controlTask] streamed primitive led=%d\n", remoteLedState);
                  } else if (data.isNull()) {
                    remoteLedState = 2;
                    lastLedFetchMs = millis();
                    Serial.println("[controlTask] streamed primitive null -> cleared led");
                  } else {
                    String s; serializeJson(data, s); s.trim();
                    if (s == "0") remoteLedState = 0;
                    else if (s == "1") remoteLedState = 1;
                    else if (s == "null") remoteLedState = 2;
                    lastLedFetchMs = millis();
                    Serial.printf("[controlTask] streamed primitive str led=%d\n", remoteLedState);
                  }
                }
              } else {
                String s = dataBuf;
                s.trim();
                if (s == "0") remoteLedState = 0;
                else if (s == "1") remoteLedState = 1;
                else if (s == "null") remoteLedState = 2;
                lastLedFetchMs = millis();
                Serial.printf("[controlTask] streamed raw fallback led=%d\n", remoteLedState);
              }
            } else {
              String s = dataBuf;
              s.trim();
              if (s.indexOf("\"led\"") != -1) {
                int idx = s.indexOf("\"led\"");
                int colon = s.indexOf(':', idx);
                if (colon > 0) {
                  String val = "";
                  for (int i = colon+1; i < (int)s.length(); ++i) {
                    char c = s[i];
                    if ((c >= '0' && c <= '9') || c == '-' ) val += c;
                    else if (val.length()>0) break;
                  }
                  if (val.length()>0) {
                    int lv = val.toInt();
                    if (lv == 0) remoteLedState = 0;
                    else if (lv == 1) remoteLedState = 1;
                    else remoteLedState = 2;
                    lastLedFetchMs = millis();
                    Serial.printf("[controlTask] crude parsed led=%d\n", remoteLedState);
                  }
                }
              }
            }
            dataBuf = "";
          }
        } else if (line.startsWith("data:")) {
          String payload = line.substring(5);
          payload.trim();
          if (payload.length() > 0) {
            if (dataBuf.length() > 0) dataBuf += "\n";
            dataBuf += payload;
          }
        } 
      } else {
        if (millis() - lastRecv > socketIdleTimeoutMs) {
          Serial.println("[controlTask] stream idle timeout, reconnecting...");
          break;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
    }
    client->stop();
    vTaskDelay(reconnectDelayMs / portTICK_PERIOD_MS);
  }
  delete client;
  vTaskDelete(NULL);
}