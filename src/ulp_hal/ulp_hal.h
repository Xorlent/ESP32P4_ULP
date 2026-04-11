/**
 * @file ulp_hal.h
 * @brief Register-level HAL for the ESP32-P4 LP core.
 *
 * Re-implements the minimum subset of ESP-IDF `ulp` component functions
 * using only headers present in Arduino-esp32.  Nothing links against libulp.a.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "programs/ulp_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ULP_HAL_OK                  =  0,
    ULP_HAL_ERR_INVALID_ARG     = -1,
    ULP_HAL_ERR_INVALID_BINARY  = -2,
    ULP_HAL_ERR_NOT_P4          = -3,
    ULP_HAL_ERR_TIMEOUT         = -4,
} ulp_hal_err_t;

/* Wakeup source flags */
#define ULP_HAL_WAKE_HP_CPU     (1u << 0)
#define ULP_HAL_WAKE_LP_TIMER   (1u << 1)
#define ULP_HAL_WAKE_ETM        (1u << 2)
#define ULP_HAL_WAKE_LP_IO      (1u << 3)
#define ULP_HAL_WAKE_LP_UART    (1u << 4)

typedef struct {
    uint32_t wakeup_source;
    uint32_t lp_timer_period_us;
    bool     skip_lp_rom_boot;
} ulp_hal_cfg_t;

#ifndef SOC_LP_RAM_LOW
#define SOC_LP_RAM_LOW  0x50108000UL
#endif
#ifndef SOC_LP_RAM_HIGH
#define SOC_LP_RAM_HIGH 0x50110000UL
#endif

#define ULP_HAL_LP_RAM_SIZE  (SOC_LP_RAM_HIGH - SOC_LP_RAM_LOW)

/** Copy a pre-built LP core binary into LP SRAM. */
ulp_hal_err_t ulp_hal_load_binary(const uint8_t *bin, size_t len);

/** Start the LP core with the given configuration. */
ulp_hal_err_t ulp_hal_run(const ulp_hal_cfg_t *cfg);

/** Stop the LP core and disable all wakeup sources. */
void ulp_hal_stop(void);

/** Return true if the LP core has set ULP_STATUS_RUNNING. */
bool ulp_hal_lp_is_running(void);

/** Return a pointer to the shared memory header in LP SRAM. */
ulp_shared_mem_t *ulp_hal_shared_mem(void);

#ifdef __cplusplus
}
#endif
