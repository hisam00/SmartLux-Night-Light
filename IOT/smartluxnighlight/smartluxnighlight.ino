#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#define DHTType DHT11

const int lightSensorPin  = 25;
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

    if (temperature < coldThreshold) {
      bluePWM = brightness;
    }
    else if (temperature > hotThreshold) {
      redPWM   = brightness;
      greenPWM = brightness * 0.45;
    }
    else {
      greenPWM = brightness;
    }

  }

  analogWrite(redPin,   redPWM);
  analogWrite(greenPin, greenPWM);
  analogWrite(bluePin,  bluePWM);

 //debug
 Serial.printf("LDR=%d  Motion=%d  Brightness=%d  Temp=%.1f°C  Hum=%.1f%%  LED[R,G,B]=[%d,%d,%d]\n",
                lightValue, motionDetected, brightness, temperature, humidity, redPWM, greenPWM, bluePWM);
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
