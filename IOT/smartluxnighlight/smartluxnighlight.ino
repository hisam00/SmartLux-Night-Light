#include <DHT.h>
#define DHTType DHT11

const int lightSensorPin  = 25;
const int motionSensorPin = 26;
const int DHTPin          = 4;

DHT dht(DHTPin, DHTType);


void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  pinMode(lightSensorPin,  INPUT);
  pinMode(motionSensorPin, INPUT);
  dht.begin();

}

void loop() {
  // put your main code here, to run repeatedly:

}
