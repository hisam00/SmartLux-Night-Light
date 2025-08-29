#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <time.h>

#define DHTType DHT11

const int lightSensorPin  = 34;
const int motionSensorPin = 26;
const int DHTPin          = 4;
const int redPin          = 14;
const int greenPin        = 12;
const int bluePin         = 13;

DHT dht(DHTPin, DHTType);

const int lightThreshold = 4095;
const int darkThreshold  = 3700;
const int maxBrightness  = 100;
const float coldThreshold  = 22.0;   // both in celcius
const float hotThreshold   = 33.0;

const unsigned long motionTimeout = 60000; // milisecond tips 1 min = 60000 mili
unsigned long lastMotionTime = 0;
bool ledsActive = false;

const char* WIFI_SSID     = "secret";
const char* WIFI_PASSWORD = "secret";

const char* DEVICE_ID    = "device01"; // Device ID (unique per device)
const char* DEVICE_LABEL = "Bedroom Lamp";

const char* FIREBASE_DATABASE_URL = "https://smartlux-night-light-default-rtdb.asia-southeast1.firebasedatabase.app";

const unsigned long sendInterval = 10000; // send every 10 seconds
unsigned long lastSendTime = 0;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  pinMode(lightSensorPin,  INPUT);
  pinMode(motionSensorPin, INPUT);
  pinMode(redPin,          OUTPUT);
  pinMode(greenPin,        OUTPUT);
  pinMode(bluePin,         OUTPUT);
  dht.begin();

  analogWrite(redPin,   0);
  analogWrite(greenPin, 0);
  analogWrite(bluePin,  0);

  connectWiFi();
  
  // NTP as main --> millis as fallback (timestamp)
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.println("Waiting for NTP time sync (up to 10s)...");
  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1600000000ULL && millis() - start < 10000) { // wait up to 10s for a plausible time
    delay(200);
    now = time(nullptr);
  }
  if (now >= 1600000000ULL) {
    Serial.println("NTP time synced.");
  } else {
    Serial.println("NTP time not available; will use millis() timestamps as fallback.");
  }

}

void loop() {
  // put your main code here, to run repeatedly:
  float humidity       = dht.readHumidity();
  float temperature    = dht.readTemperature();
  int   lightValue     = analogRead(lightSensorPin);
  bool  motionDetected = (digitalRead(motionSensorPin) == HIGH);

  int brightness = map(lightValue, darkThreshold, lightThreshold, 0, maxBrightness);
  brightness = constrain(brightness, 0, maxBrightness);

  int redPWM   = 0;
  int greenPWM = 0;
  int bluePWM  = 0;

  if (motionDetected) {
    lastMotionTime = millis();
    ledsActive = true;
  } else if (ledsActive && (millis() - lastMotionTime >= motionTimeout)) {
    ledsActive = false;
  }

  if (ledsActive) {
    if (!isnan(temperature)) {
      if (temperature < coldThreshold) {
        bluePWM = brightness;
      } else if (temperature > hotThreshold) {
        redPWM   = brightness;
        greenPWM = (int)(brightness * 0.45);
      } else {
        greenPWM = brightness;
      }
    } else {
      greenPWM = brightness;
    }
  }

  analogWrite(redPin,   redPWM);
  analogWrite(greenPin, greenPWM);
  analogWrite(bluePin,  bluePWM);

  // debug
  Serial.printf("LDR=%d  Motion=%d  Brightness=%d  Temp=%.1f°C  Hum=%.1f%%  LED[R,G,B]=[%d,%d,%d]\n",
                lightValue, motionDetected, brightness,
                (isnan(temperature) ? 0.0 : temperature),
                (isnan(humidity) ? 0.0 : humidity),
                redPWM, greenPWM, bluePWM);

  // Send to Firebase periodically
  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    bool ok = sendToFirebase(lightValue, motionDetected, humidity, temperature);
    if (!ok) {
      Serial.println("Warning: failed to send data to Firebase.");
    }
  }

  delay(2000);
}

void connectWiFi() {
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    // Optional timeout to allow retry loop rather than blocking forever
    if (millis() - start > 15000) {
      Serial.println("\nWiFi connect timed out; retrying...");
      start = millis();
    }
  }
  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

// Send a POST to Firebase to push the sensor object under /devices/<DEVICE_ID>/sensors
bool sendToFirebase(int ldr, bool motion, float hum, float temp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping send.");
    return false;
  }

  // Build device path: /devices/<DEVICE_ID>/sensors.json
  String url = String(FIREBASE_DATABASE_URL);
  if (!url.endsWith("/")) url += "/";
  url += "devices/";
  url += DEVICE_ID;
  url += "/sensors.json";

  // Get epoch seconds and epoch ms (if NTP worked) and ISO string; fallback to millis()
  time_t now = time(nullptr);
  unsigned long epoch_ms;
  char iso[32] = {0};
  if (now > 0) {
    epoch_ms = (unsigned long)now * 1000UL;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo); // UTC
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  } else {
    // fallback
    epoch_ms = (unsigned long) millis();
    strcpy(iso, ""); // empty ISO indicates fallback
  }

  // Build JSON payload including device info and human label
  char payload[512];

  if (!isnan(hum) && !isnan(temp)) {
    snprintf(payload, sizeof(payload),
      "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":%.2f,\"temperature\":%.2f}",
      DEVICE_ID, DEVICE_LABEL, (unsigned long)(epoch_ms/1000UL), epoch_ms, iso, ldr, motion ? "true" : "false", hum, temp);
  } else if (!isnan(hum)) {
    snprintf(payload, sizeof(payload),
      "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":%.2f,\"temperature\":null}",
      DEVICE_ID, DEVICE_LABEL, (unsigned long)(epoch_ms/1000UL), epoch_ms, iso, ldr, motion ? "true" : "false", hum);
  } else if (!isnan(temp)) {
    snprintf(payload, sizeof(payload),
      "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":null,\"temperature\":%.2f}",
      DEVICE_ID, DEVICE_LABEL, (unsigned long)(epoch_ms/1000UL), epoch_ms, iso, ldr, motion ? "true" : "false", temp);
  } else {
    snprintf(payload, sizeof(payload),
      "{\"device\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu,\"timestamp_ms\":%lu,\"ts_iso\":\"%s\",\"ldr\":%d,\"motion\":%s,\"humidity\":null,\"temperature\":null}",
      DEVICE_ID, DEVICE_LABEL, (unsigned long)(epoch_ms/1000UL), epoch_ms, iso, ldr, motion ? "true" : "false");
  }

  Serial.print("Sending to ");
  Serial.println(url);
  Serial.print("Payload: ");
  Serial.println(payload);

  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();

  HTTPClient https;
  bool success = false;
  if (https.begin(*client, url)) {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.POST(payload);
    if (httpCode > 0) {
      String response = https.getString();
      Serial.printf("Firebase response code: %d\n", httpCode);
      Serial.println(response);
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) success = true;
    } else {
      Serial.printf("HTTP POST failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.println("Unable to start HTTPS connection");
  }
  delete client;
  return success;
}
