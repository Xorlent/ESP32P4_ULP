/**
 * @file ESP32P4_ULP.cpp
 * @brief Implementation of the ESP32P4ULPClass Arduino wrapper.
 */

#include "ESP32P4_ULP.h"

ESP32P4ULPClass ULP;

/* ── Private ─────────────────────────────────────────────────────────────── */

bool ESP32P4ULPClass::_loadAndRun(uint32_t prog_id, const ulp_hal_cfg_t *cfg)
{
    if (_currentProgId != ULP_PROG_ID_NONE) {
        ulp_hal_stop();
        _currentProgId = ULP_PROG_ID_NONE;
    }

    const ulp_program_desc_t *desc = ulp_program_get(prog_id);
    if (desc == NULL || desc->data == NULL || desc->size == 0)
        return false;
    if (desc->size == 1 && desc->data[0] == 0x00)
        return false;

    if (ulp_hal_load_binary(desc->data, desc->size) != ULP_HAL_OK)
        return false;

    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->magic      = ULP_SHARED_MAGIC;
    sh->program_id = prog_id;

    if (ulp_hal_run(cfg) != ULP_HAL_OK)
        return false;

    _currentProgId = prog_id;
    return true;
}

/* ── Public ──────────────────────────────────────────────────────────────── */

void ESP32P4ULPClass::clearWakeupPending(void)
{
    /* PMU_HP_INT_CLR_REG (0x50115170), BIT(29) = PMU_SW_INT_CLR.
     * Clears the latched LP→HP wakeup set by ulp_lp_core_wakeup_main_processor(). */
    *(volatile uint32_t *)0x50115170u = (1u << 29);
}

bool ESP32P4ULPClass::wakeOnGPIO(uint8_t lp_gpio_num,
                                  uint8_t wake_level,
                                  uint32_t debounce)
{
    if (lp_gpio_num > 15) return false;

    ulp_hal_cfg_t cfg = {
        .wakeup_source       = ULP_HAL_WAKE_LP_TIMER,
        .lp_timer_period_us  = 1000,
        .skip_lp_rom_boot    = false,
    };

    if (!_loadAndRun(ULP_PROG_ID_GPIO_WAKEUP, &cfg))
        return false;

    /* Write config AFTER _loadAndRun() — load_binary zeroes shared mem. */
    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->config0 = lp_gpio_num;
    sh->config1 = (wake_level == HIGH) ? 1u : 0u;
    sh->config2 = debounce;

    return true;
}

void ESP32P4ULPClass::stop(void)
{
    ulp_hal_stop();
    _currentProgId = ULP_PROG_ID_NONE;
}

bool ESP32P4ULPClass::isRunning(void)
{
    return ulp_hal_lp_is_running();
}

uint32_t ESP32P4ULPClass::getData(uint8_t index)
{
    if (index >= 8) return 0;
    return ulp_hal_shared_mem()->data[index];
}

ulp_shared_mem_t *ESP32P4ULPClass::sharedMem(void)
{
    return ulp_hal_shared_mem();
}
