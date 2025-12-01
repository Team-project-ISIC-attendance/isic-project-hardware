#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    Serial.println("<Esp is ready>");
}

void loop() {
    // put your main code here, to run repeatedly:
    Serial.println("<Esp is running>");
    delay(1000);
}
