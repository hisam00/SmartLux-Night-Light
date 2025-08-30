#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <time.h>

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

void connectWiFi();
bool enqueueSensorData(int ldr, bool motion, float hum, float temp);
void firebaseTask(void* pvParameters);

// ISR for motion (RISING)
void IRAM_ATTR motionISR() {
  motionTriggeredISR = true;
  // capture tick count in ms in ISR-safe way
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

  // create queue for sensor samples
  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  if (!sensorQueue) {
    Serial.println("Failed to create queue!");
  } else {
    // Firebase sender task pinned to core 1
    xTaskCreatePinnedToCore(firebaseTask, "firebaseTask", 8192, NULL, 1, NULL, 1);
  }

  // attach interrupt
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
  if (nowMs - lastDhtRead >= dhtInterval) {
    lastDhtRead = nowMs;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h)) humidity = h;
    if (!isnan(t)) temperature = t;
    Serial.printf("[DHT] T=%.1f H=%.1f\n", (isnan(temperature)?0.0:temperature), (isnan(humidity)?0.0:humidity));
  }

  // compute brightness (0..maxBrightness)
  int brightness = map(lightValue, darkThreshold, lightThreshold, 0, maxBrightness);
  brightness = constrain(brightness, 0, maxBrightness);

  // map logical brightness (0..maxBrightness) to PWM 0..255 for analogWrite
  int pwmVal = map(brightness, 0, maxBrightness, 0, 255);

  int redPWM = 0, greenPWM = 0, bluePWM = 0;
  if (ledsActive) {
    if (!isnan(temperature)) {
      if (temperature < coldThreshold) {
        bluePWM = pwmVal;
      } else if (temperature > hotThreshold) {
        redPWM = pwmVal;
        greenPWM = (int)(pwmVal * 0.45);
      } else {
        greenPWM = pwmVal;
      }
    } else {
      greenPWM = pwmVal;
    }
  }

  analogWrite(redPin, redPWM);
  analogWrite(greenPin, greenPWM);
  analogWrite(bluePin, bluePWM);

  // debug
  Serial.printf("LDR=%d Motion=%d Bright=%d Temp=%.1f Hum=%.1f LED[R,G,B]=[%d,%d,%d]\n",
                lightValue, motionDetected, brightness,
                (isnan(temperature)?0.0:temperature),
                (isnan(humidity)?0.0:humidity),
                redPWM, greenPWM, bluePWM);

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

// Firebase task runs separately
void firebaseTask(void* pvParameters) {
  WiFiClientSecure *client = new WiFiClientSecure();
  client->setInsecure();
  HTTPClient https;

  for (;;) {
    SensorData data;
    if (xQueueReceive(sensorQueue, &data, portMAX_DELAY) == pdTRUE) {
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
        Serial.println("[FirebaseTask] begin failed");
      }
      delay(10);
    }
  }

  delete client;
  vTaskDelete(NULL);
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
