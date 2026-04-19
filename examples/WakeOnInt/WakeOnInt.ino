/**
 * WakeOnInt.ino
 *
 * Put the ESP32-P4 into deep sleep and wake on a GPIO9 rising edge using the
 * int_wakeup LP program.
 * Connect a button between GPIO9 and 3.3 V.
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
        Serial.printf("Woke from LP IO interrupt handled by the LP core - interrupt count: %lu, GPIO level: %lu\n",
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

    if (!ULP.wakeOnInt(LP_IO_9, LP_GPIO_INT_LOW_TO_HIGH)) {
        Serial.println("Failed to start the LP interrupt wake program");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("Entering deep sleep. Drive GPIO9 from LOW to HIGH to wake.");
    Serial.flush();

    while (Serial.available()) {
        Serial.read();
    }

    ULP.clearWakeupPending();
    esp_deep_sleep_start();
}

void loop()
{
}