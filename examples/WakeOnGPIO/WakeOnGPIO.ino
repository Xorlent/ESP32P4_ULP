/**
 * WakeOnGPIO.ino
 *
 * Put the ESP32-P4 into deep sleep and wake when GPIO8 goes HIGH.
 * Connect a button between GPIO8 and 3.3 V.
 */

#include <Arduino.h>
#include "ESP32P4_ULP.h"

void setup()
{
    bool wokeFromULP = ULP.wokeFromULP();
    if (wokeFromULP) {
        ULP.stop();
    }

    Serial.begin(115200);
    delay(500);
    
    if (wokeFromULP) {
        Serial.printf("Woke from ULP - LP sample counter: %lu, GPIO level: %lu\n",
                      (uint32_t)ULP.sharedMem()->lp_counter,
                      ULP.getData(0));
    } else {
        Serial.println("First boot...");
    }

    Serial.print("Enter deep sleep? (ENTER to sleep): ");
    Serial.flush();

    while (true) {
        if (Serial.available()) {
            String response = Serial.readStringUntil('\n');
            response.trim();
            if (response.length() == 0) {
                Serial.println("Y");
                break;
            }
        }
        delay(10);
    }

    // Arm the LP core to poll GPIO8 every 100 ms and wake HP core after 2 matching samples.
    if (!ULP.wakeOnGPIO(LP_IO_8, HIGH, 100, 2)) {
        Serial.println("Failed to start ULP program");
        while (true) delay(1000);
    }

    Serial.println("Entering deep sleep. Connect GPIO8 to 3.3V to wake.");
    Serial.flush();

    // Clear serial buffer
    while (Serial.available()) {
        Serial.read();
    }

    ULP.clearWakeupPending();
    esp_deep_sleep_start();
}

void loop()
{
}
