#include <Arduino.h>
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

void setup() {
  Serial.begin(115200);

  SerialBT.begin("ESP32");

  delay(500);

  Serial.print("Bluetooth MAC: ");
  Serial.println(SerialBT.getBtAddressString());
}

void loop() {
}