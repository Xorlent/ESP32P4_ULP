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

    // esp_sleep_get_wakeup_cause() cannot detect ULP wakeup because
    // Arduino-esp32 lacks CONFIG_ULP_COPROC_ENABLED.  Check shared memory instead.
    if (ULP.sharedMem()->magic == ULP_SHARED_MAGIC &&
        ULP.sharedMem()->lp_counter > 0) {
        Serial.printf("Woke from ULP — LP counter: %lu, GPIO level: %lu\n",
                      (uint32_t)ULP.sharedMem()->lp_counter,
                      ULP.getData(0));
    } else {
        Serial.println("First boot");
    }

    // Configure GPIO8 as LP IO input with pull-down
    rtc_gpio_init(GPIO_NUM_8);
    rtc_gpio_set_direction(GPIO_NUM_8, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(GPIO_NUM_8);
    rtc_gpio_pullup_dis(GPIO_NUM_8);

    // Arm the LP core to poll GPIO8 and wake HP when it goes HIGH
    if (!ULP.wakeOnGPIO(LP_IO_8, HIGH)) {
        Serial.println("ERROR: LP binary not built — run tools/build_all_lp_programs.py");
        while (true) delay(1000);
    }

    Serial.println("Entering deep sleep. Connect GPIO8 to 3.3V to wake.");
    Serial.flush();

    ULP.clearWakeupPending();
    esp_deep_sleep_start();
}

void loop()
{
}
