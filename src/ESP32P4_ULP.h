/**
 * @file ESP32P4_ULP.h
 * @brief Arduino API for the ESP32-P4 LP (ULP) core.
 * This library provides a wrapper around the ESP-IDF ULP APIs and pre-compiled LP core binaries for common wakeup use cases. It is designed to be used in the Arduino environment.
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

enum lp_gpio_wakeup_int_type_t : uint8_t {
    LP_GPIO_INT_LOW_TO_HIGH = 1,
    LP_GPIO_INT_HIGH_TO_LOW = 2,
    LP_GPIO_INT_ANY_EDGE    = 3,
    LP_GPIO_INT_LOW_LEVEL   = 4,
    LP_GPIO_INT_HIGH_LEVEL  = 5,
};

class ESP32P4ULPClass {
public:
    /**
     * Return true if the current boot was caused by an LP wake handled by this library.
     * Works around Arduino-esp32 not always reporting ESP_SLEEP_WAKEUP_ULP for LP-core wakes.
     */
    bool wokeFromULP(void);

    /**
         * Load the gpio_wakeup LP program and arm it.
         * The LP core polls lp_gpio_num every period milliseconds and triggers HP wakeup
         * when it matches wake_level. The wrapper also routes the pin through the
         * RTC/LP mux and applies a default pull that matches the requested level.
     */
    bool wakeOnGPIO(uint8_t lp_gpio_num, uint8_t wake_level,
                    uint32_t period = 1, uint32_t debounce = 0);

    /**
     * Load the int_wakeup LP program and arm LP IO interrupt forwarding to HP.
     * wake_level accepts LP_GPIO_INT_* trigger values 1-5. The wrapper applies
     * a matching default pull for rising/high and falling/low triggers.
     */
    bool wakeOnInt(uint8_t lp_gpio_num, uint8_t wake_level);

    /**
     * Load the software-I2C temperature LP program and arm timer-based polling.
     * The LP core performs a high-repeatability SHT4X measurement over a bit-banged
     * software-I2C bus at address 0x44, validates both CRC bytes, stores the latest
     * raw temperature/humidity samples in shared memory, and wakes HP when the raw
     * temperature falls outside the inclusive [low_limit_c_deg, high_limit_c_deg]
    * range after conversion to raw SHT4X thresholds. External pullups on SDA and
    * SCL are required. period_ms must be at least 50 ms.
     */
    bool wakeOnSoftwareI2CTemperature(uint8_t sda_lp_gpio_num,
                                      uint8_t scl_lp_gpio_num,
                                      int16_t low_limit_c_deg,
                                      int16_t high_limit_c_deg,
                                      uint32_t period_ms = 300000);

    /** Stop the currently running LP program. */
    void stop(void);

    /** Return true if the LP program has set ULP_STATUS_RUNNING. */
    bool isRunning(void);

    /** Read a data word written by the LP core (index 0-7). */
    uint32_t getData(uint8_t index);

    /**
     * Clear the pending LP-to-HP wake request.
     * Call before esp_deep_sleep_start() to clear both the PMU wake latch and
     * the shared-memory ULP_STATUS_WAKEUP_PENDING flag from a previous cycle.
     */
    void clearWakeupPending(void);

    /** Return a pointer to the shared memory struct in LP SRAM. */
    ulp_shared_mem_t *sharedMem(void);

private:
    bool _loadProgram(uint32_t prog_id);
    bool _runLoaded(uint32_t prog_id, const ulp_hal_cfg_t *cfg);

    uint32_t _currentProgId = ULP_PROG_ID_NONE;
};

extern ESP32P4ULPClass ULP;
