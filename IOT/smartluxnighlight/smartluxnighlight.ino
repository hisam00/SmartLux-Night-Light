#include <DHT.h>
#define DHTType DHT11

const int lightSensorPin  = 25;
const int motionSensorPin = 26;
const int DHTPin          = 4;
const int redPin          = 14;
const int greenPin        = 12;
const int bluePin         = 13;

DHT dht(DHTPin, DHTType);

const float coldThreshold  = 18.0;   

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

  int redPWM   = 0;
  int greenPWM = 0;
  int bluePWM  = 0;

  if (temperature < coldThreshold) {
    bluePWM = 100;
  }
  
  analogWrite(redPin,   redPWM);
  analogWrite(greenPin, greenPWM);
  analogWrite(bluePin,  bluePWM);

 delay(2000);
}
