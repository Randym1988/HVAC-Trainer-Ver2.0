#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  Serial.println("Heat Pump trainer online");
}

void loop() {
  delay(1000);
}
