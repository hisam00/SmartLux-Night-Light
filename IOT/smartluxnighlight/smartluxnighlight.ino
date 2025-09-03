#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>

#define DHTType DHT11

// Pins
const int lightSensorPin  = 34;
const int motionSensorPin = 26;
const int DHTPin          = 4;
const int redPin          = 14;
const int greenPin        = 12;
const int bluePin         = 13;

DHT dht(DHTPin, DHTType);

// thresholds & timings
const int lightThreshold = 4095;
const int darkThreshold  = 3700;
const int maxBrightness  = 100;
const float coldThreshold  = 22.0;   // both in celcius
const float hotThreshold   = 33.0;

const unsigned long motionTimeout = 60000; // milisecond tips 1 min = 60000 mili
const unsigned long dhtInterval = 2000;    // ms
const unsigned long sendInterval = 10000;  // ms

// WiFi / Firebase
const char* WIFI_SSID     = "secret";
const char* WIFI_PASSWORD = "secret";

const char* DEVICE_ID     = "device01";
const char* DEVICE_LABEL  = "Bedroom Lamp";

const char* FIREBASE_DATABASE_URL = "https://smartlux-night-light-default-rtdb.asia-southeast1.firebasedatabase.app";

// ISR-shared state
volatile bool motionTriggeredISR = false;
volatile unsigned long lastMotionTime = 0;
bool ledsActive = false;

// Remote override state
// Path used: devices/<device_id>/control/led.json  (value: 0, 1 or 2 — 2 = automatic)
// 2 => automatic (no override), 0 => forced OFF, 1 => forced ON
volatile int remoteLedState = 2;
unsigned long lastLedFetchMs = 0;
const unsigned long ledPollIntervalMs = 1000; // poll every 1s

// remote reboot control
volatile int remoteRebootState = 0;               // 0 = no-reboot requested, 1 = reboot requested
unsigned long lastRebootFetchMs = 0;
const unsigned long rebootPollIntervalMs = 1000; // polling interval (can be same as ledPollIntervalMs)
const unsigned long rebootSoftWaitMs = 5000;     // soft wait before reboot (ms) — adjust as needed
const int rebootPutMaxAttempts = 5;               // attempts to PUT 0 back before giving up for this cycle

// sensor data queue
typedef struct {
  int ldr;
  bool motion;
  float humidity;
  float temperature;
  unsigned long epoch_ms;
  char iso[32];
} SensorData;

QueueHandle_t sensorQueue = nullptr;

// --- Notify config + immediate notify queue/task ---
typedef struct {
  String option;           // "none", "hass", "all"
  String motion_url;       // URL to call for motion
  String temp_url;         // URL to call for high temperature
  float high_temp_threshold; // temperature threshold for high-alert
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
// -----------------------------------------

void connectWiFi();
bool enqueueSensorData(int ldr, bool motion, float hum, float temp);
void firebaseTask(void* pvParameters);
void notifyTask(void* pvParameters);
void fetchNotifyConfigOnce(); // helper to fetch notify config immediately

// ISR for motion (RISING)
void IRAM_ATTR motionISR() {
  motionTriggeredISR = true;
  TickType_t t = xTaskGetTickCountFromISR();
  lastMotionTime = (unsigned long)t * portTICK_PERIOD_MS;
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

  // ensure PWM starts off
  analogWrite(redPin, 0);
  analogWrite(greenPin, 0);
  analogWrite(bluePin, 0);

  connectWiFi();

  // create queues
  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  notifyQueue = xQueueCreate(32, sizeof(NotifyEvent)); // larger so we don't drop events

  if (!sensorQueue) {
    Serial.println("Failed to create sensor queue!");
  } else {
    // Firebase sender task pinned to core 1
    xTaskCreatePinnedToCore(firebaseTask, "firebaseTask", 16384, NULL, 1, NULL, 1);
  }

  if (!notifyQueue) {
    Serial.println("Failed to create notify queue!");
  } else {
    // Notify task pinned to core 0
    xTaskCreatePinnedToCore(notifyTask, "notifyTask", 12288, NULL, 2, NULL, 0);
  }

  // attach interrupt (ISR only sets a flag)
  attachInterrupt(digitalPinToInterrupt(motionSensorPin), motionISR, RISING);

  // NTP
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.println("Waiting for NTP time sync (up to 10s)...");
  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1600000000ULL && millis() - start < 10000) {
    delay(200);
    now = time(nullptr);
  }
  if (now >= 1600000000ULL) Serial.println("NTP time synced.");
  else Serial.println("NTP time not available; will use millis() timestamps as fallback.");

  // Fetch notify config once right away so notifyTask has latest settings
  fetchNotifyConfigOnce();
}

unsigned long lastDhtRead = 0;
unsigned long lastSendTime = 0;

void loop() {
  // FAST sensor reads
  int lightValue = analogRead(lightSensorPin);

  // motion: check ISR flag first, fallback to polling
  bool motionDetected = false;
  if (motionTriggeredISR) {
    motionTriggeredISR = false; // clear flag
    motionDetected = true;
    // lastMotionTime updated in ISR
  } else {
    motionDetected = (digitalRead(motionSensorPin) == HIGH);
    if (motionDetected) lastMotionTime = millis();
  }

  // update ledsActive based on timeout
  if (motionDetected) ledsActive = true;
  else if (ledsActive && (millis() - lastMotionTime >= motionTimeout)) ledsActive = false;

  // DHT read only every dhtInterval ms
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

  // Apply override from remote control if present (remoteLedState)
  // remoteLedState == 1 => force ON; == 0 => force OFF; == 2 => automatic
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
    } else {
      greenPWM = brightness;
    }
  } else if (ledsActive && (remoteLedState == 1)) {
    redPWM = brightness;
    greenPWM = brightness;
    bluePWM = brightness;
  }

  // apply PWM using analogWrite
  analogWrite(redPin, redPWM);
  analogWrite(greenPin, greenPWM);
  analogWrite(bluePin, bluePWM);

  // debug
  Serial.printf("LDR=%d Motion=%d Bright=%d Temp=%.1f Hum=%.1f LED[R,G,B]=[%d,%d,%d] remote=%d\n",
                lightValue, motionDetected, brightness,
                (isnan(temperature)?0.0:temperature),
                (isnan(humidity)?0.0:humidity),
                redPWM, greenPWM, bluePWM,
                remoteLedState);

  // If motion detected: push immediate notify event
  if (motionDetected) {
    NotifyEvent ev;
    time_t nowt = time(nullptr);
    ev.epoch_ms = (nowt > 0) ? (unsigned long)nowt * 1000UL : millis();
    ev.type = EVT_MOTION;
    ev.ldr = lightValue;
    ev.temperature = isnan(temperature) ? NAN : temperature;
    if (notifyQueue) xQueueSend(notifyQueue, &ev, 0);
  }

  // If DHT read just happened and temperature > threshold: push immediate high-temp event
  if (didDhtRead && !isnan(temperature)) {
    float threshold = notifyCfg.high_temp_threshold;
    if (temperature > threshold) {
      NotifyEvent ev;
      time_t nowt = time(nullptr);
      ev.epoch_ms = (nowt > 0) ? (unsigned long)nowt * 1000UL : millis();
      ev.type = EVT_HIGH_TEMP;
      ev.ldr = lightValue;
      ev.temperature = temperature;
      if (notifyQueue) xQueueSend(notifyQueue, &ev, 0);
    }
  }

  // enqueue sensor data periodically
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    bool ok = enqueueSensorData(lightValue, motionDetected, humidity, temperature);
    if (!ok) Serial.println("Warning: queue full, dropped data");
  }

  delay(20); // keep loop responsive
}

// Enqueue sensor sample
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

  // optional: drop oldest and try once more
  SensorData tmp;
  if (xQueueReceive(sensorQueue, &tmp, 0) == pdTRUE) {
    if (xQueueSend(sensorQueue, &d, 0) == pdTRUE) return true;
  }
  return false;
}

// Notify task: processes NotifyEvents immediately and calls webhook URLs
void notifyTask(void* pvParameters) {
  WiFiClient *client = new WiFiClient();
  HTTPClient http;


  for (;;) {
    NotifyEvent ev;
    if (xQueueReceive(notifyQueue, &ev, portMAX_DELAY) == pdTRUE) {
      // Only proceed if notify is enabled (option != "none") and URL present
      if (notifyCfg.option == "none") {
        Serial.println("[notifyTask] notify.option == none, skipping event.");
        continue;
      }

      String targetUrl = "";
      if (ev.type == EVT_MOTION) targetUrl = notifyCfg.motion_url;
      else if (ev.type == EVT_HIGH_TEMP) targetUrl = notifyCfg.temp_url;

      if (targetUrl.length() == 0) {
        Serial.println("[notifyTask] No target URL configured for this event, skipping.");
        continue;
      }

      // Build payload
      char payload[256];
      if (ev.type == EVT_MOTION) {
        if (isnan(ev.temperature)) {
          snprintf(payload, sizeof(payload),
            "{\"device\":\"%s\",\"event\":\"motion\",\"timestamp_ms\":%lu,\"ldr\":%d}",
            DEVICE_ID, ev.epoch_ms, ev.ldr);
        } else {
          snprintf(payload, sizeof(payload),
            "{\"device\":\"%s\",\"event\":\"motion\",\"timestamp_ms\":%lu,\"ldr\":%d,\"temperature\":%.2f}",
            DEVICE_ID, ev.epoch_ms, ev.ldr, ev.temperature);
        }
      } else {
        snprintf(payload, sizeof(payload),
          "{\"device\":\"%s\",\"event\":\"high_temperature\",\"timestamp_ms\":%lu,\"temperature\":%.2f,\"threshold\":%.2f}",
          DEVICE_ID, ev.epoch_ms, ev.temperature, notifyCfg.high_temp_threshold);
      }

      Serial.print("[notifyTask] POST -> ");
      Serial.println(targetUrl);
      Serial.println(payload);

  if (http.begin(*client, targetUrl)) {
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(String(payload));
    if (httpCode > 0) {
      Serial.printf("[notifyTask] HTTP %d\n", httpCode);
      String resp = http.getString();
      Serial.println(resp);
    } else {
      Serial.printf("[notifyTask] POST failed: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("[notifyTask] begin failed (notify)");
  }


      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
  }

  delete client;
  vTaskDelete(NULL);
}

// Firebase task runs separately
void firebaseTask(void* pvParameters) {
  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient https;

  for (;;) {
    SensorData data;
    // wait for sensor data, but timeout so we can periodically refresh notify config
    if (xQueueReceive(sensorQueue, &data, 2000 / portTICK_PERIOD_MS) == pdTRUE) {
      // Post sensor data
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

      delay(10); // small pause after processing this queue item
    }

    // Periodically fetch notify config from Firebase so notifyTask has latest info
    unsigned long tNow = millis();
    if (tNow - notifyCfg.lastFetchedMs >= 10000UL) { // every 10s
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

    // Periodically fetch remote LED control value from Firebase
    if (tNow - lastLedFetchMs >= ledPollIntervalMs) {
      lastLedFetchMs = tNow;

      String ledUrl = String(FIREBASE_DATABASE_URL);
      if (!ledUrl.endsWith("/")) ledUrl += "/";
      ledUrl += "devices/";
      ledUrl += DEVICE_ID;
      ledUrl += "/control/led.json"; // <-- path expected by website

      Serial.print("[FirebaseTask] GET remote led state ");
      Serial.println(ledUrl);

      if (https.begin(*client, ledUrl)) {
        int httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
          String body = https.getString();
          body.trim();
          Serial.print("[FirebaseTask] led body: ");
          Serial.println(body);

          // Possible responses: "0", "1", null ("null")
          if (body == "0") {
            remoteLedState = 0;
          } else if (body == "1") {
            remoteLedState = 1;
          } else if (body == "null" || body.length() == 0) {
            remoteLedState = 2; // clear override
          } else {
            // try to parse as JSON number
            StaticJsonDocument<16> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (!err) {
              int v = doc.as<int>();
              if (v == 0) remoteLedState = 0;
              else if (v == 1) remoteLedState = 1;
              else remoteLedState = 2;
            } else {
              remoteLedState = 2;
            }
          }

        } else {
          Serial.printf("[FirebaseTask] GET led failed: %d\n", httpCode);
        }
        https.end();
      } else {
        Serial.println("[FirebaseTask] begin failed (GET led)");
      }
    }

    // Periodically fetch remote REBOOT control value from Firebase
    if (tNow - lastRebootFetchMs >= rebootPollIntervalMs) {
      lastRebootFetchMs = tNow;

      String rebootUrl = String(FIREBASE_DATABASE_URL);
      if (!rebootUrl.endsWith("/")) rebootUrl += "/";
      rebootUrl += "devices/";
      rebootUrl += DEVICE_ID;
      rebootUrl += "/control/reboot.json"; // <-- reboot path

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
            // request reboot
            remoteRebootState = 1;
          } else if (body == "0") {
            remoteRebootState = 0;
          } else if (body == "null" || body.length() == 0) {
            remoteRebootState = 0;
          } else {
            // try parse number
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

    // Handle reboot request (only when remoteRebootState == 1)
    if (remoteRebootState == 1) {
      Serial.println("[FirebaseTask] Reboot requested. Soft wait then attempt to set reboot=0 before restarting.");

      // Soft wait to allow UI feedback, etc.
      vTaskDelay(rebootSoftWaitMs / portTICK_PERIOD_MS);

      // Attempt to PUT 0 back to the reboot path; only restart after a successful PUT (HTTP 2xx)
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
          // Send raw 0 (a numeric JSON value). Firebase RTDB will set that path to 0.
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
          // small delay before retry
          vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
      }

      if (putSuccess) {
        Serial.println("[FirebaseTask] Reboot flag cleared on server — restarting now.");
        // Optionally short delay to flush serial
        delay(200);
        ESP.restart(); // soft restart
      } else {
        Serial.println("[FirebaseTask] Failed to clear reboot flag on server. Will retry next polling cycle.");
        // Don't restart — keep remoteRebootState==1 so next cycle tries again.
        // Optionally you may want to back off; current code will retry on next poll.
      }
    }

    // small idle to yield
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  delete client;
  vTaskDelete(NULL);
}

// Fetch notify config once at startup (called from setup)
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
