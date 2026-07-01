#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  Serial.println("AC/Gas Furnace trainer online");
}

void loop() {
  delay(1000);
}
