/**
 * WakeOnGPIO.ino
 *
 * Put the ESP32-P4 into deep sleep and wake it when GPIO8 goes HIGH.
 * Connect a button between GPIO8 and 3.3 V.
 */

#include <Arduino.h>
#include "ESP32P4_ULP.h"
#include "driver/rtc_io.h"

void setup()
{
    Serial.begin(115200);
    delay(500);

    // Check shared memory for wakeup cause.
    bool wokeFromULP = (ULP.sharedMem()->magic == ULP_SHARED_MAGIC &&
                        ULP.sharedMem()->lp_counter > 0);
    
    if (wokeFromULP) {
        Serial.printf("Woke from ULP — LP counter: %lu, GPIO level: %lu\n",
                      (uint32_t)ULP.sharedMem()->lp_counter,
                      ULP.getData(0));
        
        // Prompt user before sleeping again to prevent having to force DFU mode to flash device
        Serial.print("Enter deep sleep again? ('Y' or ENTER to sleep): ");
        Serial.flush();
        
        while (true) {
            if (Serial.available()) {
                char response = Serial.read();
                if (response == 'Y' || response == 'y' || 
                    response == '\n' || response == '\r') {
                    Serial.println("Y");
                    break;
                }
            }
            delay(10);
        }
    } else {
        Serial.println("First boot...");
    }

    // Configure GPIO8 as LP IO input with pull-down
    rtc_gpio_init(GPIO_NUM_8);
    rtc_gpio_set_direction(GPIO_NUM_8, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(GPIO_NUM_8);
    rtc_gpio_pullup_dis(GPIO_NUM_8);

    // Arm the LP core to poll GPIO8 and wake HP when it goes HIGH
    if (!ULP.wakeOnGPIO(LP_IO_8, HIGH)) {
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
