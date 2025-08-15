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
const float coldThreshold  = 18.0;   // both in celcius
const float hotThreshold   = 33.0;

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
