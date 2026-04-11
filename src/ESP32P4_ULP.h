/**
 * @file ESP32P4_ULP.h
 * @brief Arduino C++ API for the ESP32-P4 LP (ULP) core.
 *
 * Wake from deep sleep when LP_IO_8 goes HIGH:
 *
 *   #include <ESP32P4_ULP.h>
 *   #include "driver/rtc_io.h"
 *
 *   void setup() {
 *       rtc_gpio_init(GPIO_NUM_8);
 *       rtc_gpio_set_direction(GPIO_NUM_8, RTC_GPIO_MODE_INPUT_ONLY);
 *       rtc_gpio_pulldown_en(GPIO_NUM_8);
 *       rtc_gpio_pullup_dis(GPIO_NUM_8);
 *
 *       ULP.wakeOnGPIO(LP_IO_8, HIGH);
 *       ULP.clearWakeupPending();
 *       esp_deep_sleep_start();
 *   }
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

extern "C" {
#include "ulp_hal/ulp_hal.h"
#include "programs/ulp_shared.h"
#include "programs/ulp_programs.h"
}

/* LP IO pin numbers (matches lp_io_num_t in IDF) */
#ifndef LP_IO_NUM_0
enum lp_io_num_arduino : uint8_t {
    LP_IO_0  = 0,  LP_IO_1  = 1,  LP_IO_2  = 2,  LP_IO_3  = 3,
    LP_IO_4  = 4,  LP_IO_5  = 5,  LP_IO_6  = 6,  LP_IO_7  = 7,
    LP_IO_8  = 8,  LP_IO_9  = 9,  LP_IO_10 = 10, LP_IO_11 = 11,
    LP_IO_12 = 12, LP_IO_13 = 13, LP_IO_14 = 14, LP_IO_15 = 15,
};
#endif

class ESP32P4ULPClass {
public:
    /**
     * Load the gpio_wakeup LP program and arm it.
     * The LP core polls lp_gpio_num and triggers HP wakeup when it matches wake_level.
     */
    bool wakeOnGPIO(uint8_t lp_gpio_num, uint8_t wake_level,
                    uint32_t debounce = 0);

    /** Stop the currently running LP program. */
    void stop(void);

    /** Return true if the LP program has set ULP_STATUS_RUNNING. */
    bool isRunning(void);

    /** Read a data word written by the LP core (index 0–7). */
    uint32_t getData(uint8_t index);

    /**
     * Clear the pending LP→HP wakeup interrupt.
     * Call before esp_deep_sleep_start() to prevent immediate re-wake.
     */
    void clearWakeupPending(void);

    /** Return a pointer to the shared memory struct in LP SRAM. */
    ulp_shared_mem_t *sharedMem(void);

private:
    bool _loadAndRun(uint32_t prog_id, const ulp_hal_cfg_t *cfg);
    uint32_t _currentProgId = ULP_PROG_ID_NONE;
};

extern ESP32P4ULPClass ULP;
