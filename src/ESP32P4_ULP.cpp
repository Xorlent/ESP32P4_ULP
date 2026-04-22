/**
 * @file ESP32P4_ULP.cpp
 * @brief Implementation of the ESP32P4ULPClass Arduino wrapper.
 * This library provides a wrapper around the ESP-IDF ULP APIs and pre-compiled LP core binaries for common wakeup use cases. It is designed to be used in the Arduino environment.
 */

#include "ESP32P4_ULP.h"
#include "driver/rtc_io.h"
#include "soc/soc.h"
#include "soc/pmu_reg.h"
#include "hal/rtc_io_ll.h"
#include "esp_sleep.h"
#include "esp_system.h"

/* PMU_SLP_WAKEUP_CNTL2_REG on ESP32-P4 ECO2.
 * Bits [30:0] are PMU_WAKEUP_ENA sources. */
#define ULP_PMU_SLP_WAKEUP_CNTL2_REG 0x50115128u
#define ULP_PMU_LP_CORE_WAKEUP_EN    (1u << 1)

static constexpr uint32_t ULP_SOFT_I2C_MIN_PERIOD_MS = 50u;
static constexpr uint16_t ULP_SOFT_I2C_POWER_PIN_NONE = 0xFFFFu;

ESP32P4ULPClass ULP;

static uint16_t s_sht4x_cdeg_to_raw(int16_t temp_c_deg)
{
    int32_t clamped = temp_c_deg;

    if (clamped < -4500) {
        clamped = -4500;
    } else if (clamped > 13000) {
        clamped = 13000;
    }

    const uint32_t numerator = (uint32_t)(clamped + 4500) * 65535u + 8750u;
    return (uint16_t)(numerator / 17500u);
}

static uint16_t s_sht4x_rh_cpercent_to_raw(int16_t humidity_cpercent)
{
    int32_t clamped = humidity_cpercent;

    if (clamped < 0) {
        clamped = 0;
    } else if (clamped > 10000) {
        clamped = 10000;
    }

    const uint32_t numerator = (uint32_t)(clamped + 600) * 65535u + 6250u;
    return (uint16_t)(numerator / 12500u);
}

static void s_configure_lp_io_input(uint8_t lp_gpio_num, bool pullup, bool pulldown)
{
    ulp_hal_enable_lp_io_clock();
    rtc_gpio_init((gpio_num_t)lp_gpio_num);
    rtcio_ll_function_select(lp_gpio_num, RTCIO_LL_FUNC_RTC);
    rtcio_ll_input_enable(lp_gpio_num);
    rtcio_ll_output_disable(lp_gpio_num);

    if (pullup) {
        rtc_gpio_pullup_en((gpio_num_t)lp_gpio_num);
    } else {
        rtc_gpio_pullup_dis((gpio_num_t)lp_gpio_num);
    }

    if (pulldown) {
        rtc_gpio_pulldown_en((gpio_num_t)lp_gpio_num);
    } else {
        rtc_gpio_pulldown_dis((gpio_num_t)lp_gpio_num);
    }
}

static void s_configure_lp_io_i2c(uint8_t sda_lp_gpio_num, uint8_t scl_lp_gpio_num)
{
    ulp_hal_enable_lp_io_clock();

    rtc_gpio_init((gpio_num_t)sda_lp_gpio_num);
    rtc_gpio_set_direction((gpio_num_t)sda_lp_gpio_num, RTC_GPIO_MODE_INPUT_OUTPUT_OD);
    rtc_gpio_set_level((gpio_num_t)sda_lp_gpio_num, 1);
    rtc_gpio_hold_dis((gpio_num_t)sda_lp_gpio_num);
    rtcio_ll_function_select(sda_lp_gpio_num, RTCIO_LL_FUNC_RTC);
    rtcio_ll_input_enable(sda_lp_gpio_num);
    rtc_gpio_pullup_dis((gpio_num_t)sda_lp_gpio_num);
    rtc_gpio_pulldown_dis((gpio_num_t)sda_lp_gpio_num);

    rtc_gpio_init((gpio_num_t)scl_lp_gpio_num);
    rtc_gpio_set_direction((gpio_num_t)scl_lp_gpio_num, RTC_GPIO_MODE_INPUT_OUTPUT_OD);
    rtc_gpio_set_level((gpio_num_t)scl_lp_gpio_num, 1);
    rtc_gpio_hold_dis((gpio_num_t)scl_lp_gpio_num);
    rtcio_ll_function_select(scl_lp_gpio_num, RTCIO_LL_FUNC_RTC);
    rtcio_ll_input_enable(scl_lp_gpio_num);
    rtc_gpio_pullup_dis((gpio_num_t)scl_lp_gpio_num);
    rtc_gpio_pulldown_dis((gpio_num_t)scl_lp_gpio_num);
}

/* Private helpers */

bool ESP32P4ULPClass::_loadProgram(uint32_t prog_id)
{
    if (_currentProgId != ULP_PROG_ID_NONE) {
        ulp_hal_stop();
        _currentProgId = ULP_PROG_ID_NONE;
    }

    const ulp_program_desc_t *desc = ulp_program_get(prog_id);
    if (desc == NULL || desc->data == NULL || desc->size == 0)
        return false;

    if (ulp_hal_load_binary(desc->data, desc->size) != ULP_HAL_OK)
        return false;

    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->magic      = ULP_SHARED_MAGIC;
    sh->program_id = prog_id;

    return true;
}

bool ESP32P4ULPClass::_runLoaded(uint32_t prog_id, const ulp_hal_cfg_t *cfg)
{
    if (ulp_hal_run(cfg) != ULP_HAL_OK)
        return false;

    _currentProgId = prog_id;
    return true;
}

/* Public API */

void ESP32P4ULPClass::clearWakeupPending(void)
{
    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->status &= ~ULP_STATUS_WAKEUP_PENDING;
    REG_WRITE(PMU_HP_INT_CLR_REG, PMU_SW_INT_CLR_M);
}

bool ESP32P4ULPClass::wakeOnGPIO(uint8_t lp_gpio_num,
                                  uint8_t wake_level,
                                  uint32_t period,
                                  uint32_t debounce)
{
    if (lp_gpio_num > 15 || period == 0 || period > (UINT32_MAX / 1000u)) return false;

    ulp_hal_cfg_t cfg = {
        .wakeup_source      = ULP_HAL_WAKE_LP_TIMER,
        .lp_timer_period_us = period * 1000u,
    };

    if (!_loadProgram(ULP_PROG_ID_GPIO_WAKEUP))
        return false;

    s_configure_lp_io_input(lp_gpio_num, wake_level == LOW, wake_level == HIGH);

    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->config0 = lp_gpio_num;
    sh->config1 = (wake_level == HIGH) ? 1u : 0u;
    sh->config2 = debounce;

    if (!_runLoaded(ULP_PROG_ID_GPIO_WAKEUP, &cfg))
        return false;

    esp_sleep_enable_ulp_wakeup();
    REG_SET_BIT(ULP_PMU_SLP_WAKEUP_CNTL2_REG, ULP_PMU_LP_CORE_WAKEUP_EN);

    return true;
}

bool ESP32P4ULPClass::wakeOnInt(uint8_t lp_gpio_num, uint8_t wake_level)
{
    if (lp_gpio_num > 15 || wake_level < LP_GPIO_INT_LOW_TO_HIGH ||
        wake_level > LP_GPIO_INT_HIGH_LEVEL) {
        return false;
    }

    stop();

    bool pullup = false;
    bool pulldown = false;
    switch (wake_level) {
        case LP_GPIO_INT_LOW_TO_HIGH:
        case LP_GPIO_INT_HIGH_LEVEL:
            pulldown = true;
            break;
        case LP_GPIO_INT_HIGH_TO_LOW:
        case LP_GPIO_INT_LOW_LEVEL:
            pullup = true;
            break;
        default:
            break;
    }

    s_configure_lp_io_input(lp_gpio_num, pullup, pulldown);

    ulp_hal_cfg_t cfg = {
        .wakeup_source      = ULP_HAL_WAKE_HP_CPU, // | ULP_HAL_WAKE_LP_IO
        .lp_timer_period_us = 0,
    };

    if (!_loadProgram(ULP_PROG_ID_INT_WAKEUP)) {
        return false;
    }

    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->config0 = lp_gpio_num;
    sh->config1 = wake_level;

    if (!_runLoaded(ULP_PROG_ID_INT_WAKEUP, &cfg)) {
        return false;
    }

    esp_sleep_enable_ulp_wakeup();
    REG_SET_BIT(ULP_PMU_SLP_WAKEUP_CNTL2_REG, ULP_PMU_LP_CORE_WAKEUP_EN);

    return true;
}

bool ESP32P4ULPClass::wakeOnSoftwareI2CSHT4x(uint8_t sda_lp_gpio_num,
                                             uint8_t scl_lp_gpio_num,
                                             int16_t low_limit_c_deg,
                                             int16_t high_limit_c_deg,
                                             uint32_t period_ms,
                                             int16_t low_limit_c_hum,
                                             int16_t high_limit_c_hum,
                                             uint8_t i2c_pwr_gpio_num)
{
    if (sda_lp_gpio_num > 15 || scl_lp_gpio_num > 15 ||
        sda_lp_gpio_num == scl_lp_gpio_num ||
        ((i2c_pwr_gpio_num != 0xFFu) && (i2c_pwr_gpio_num > 15 ||
         i2c_pwr_gpio_num == sda_lp_gpio_num ||
         i2c_pwr_gpio_num == scl_lp_gpio_num)) ||
        low_limit_c_deg > high_limit_c_deg ||
        period_ms < ULP_SOFT_I2C_MIN_PERIOD_MS ||
        period_ms > (UINT32_MAX / 1000u)) {
        return false;
    }

    const ulp_program_desc_t *desc = ulp_program_get(ULP_PROG_ID_SOFT_I2C_TEMP_WAKEUP);
    if (desc == NULL || desc->data == NULL || desc->size == 0) {
        return false;
    }

    stop();
    s_configure_lp_io_i2c(sda_lp_gpio_num, scl_lp_gpio_num);

    if (!_loadProgram(ULP_PROG_ID_SOFT_I2C_TEMP_WAKEUP)) {
        return false;
    }

    const uint16_t raw_low_limit = s_sht4x_cdeg_to_raw(low_limit_c_deg);
    const uint16_t raw_high_limit = s_sht4x_cdeg_to_raw(high_limit_c_deg);
    uint16_t raw_humidity_low_limit = 1;
    uint16_t raw_humidity_high_limit = 0;

    if (low_limit_c_hum <= high_limit_c_hum) {
        raw_humidity_low_limit = s_sht4x_rh_cpercent_to_raw(low_limit_c_hum);
        raw_humidity_high_limit = s_sht4x_rh_cpercent_to_raw(high_limit_c_hum);
    }

    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    sh->config0 = ULP_PACK_U16_PAIR(sda_lp_gpio_num, scl_lp_gpio_num);
    sh->config1 = ULP_PACK_U16_PAIR(
        (i2c_pwr_gpio_num == 0xFFu) ? ULP_SOFT_I2C_POWER_PIN_NONE : i2c_pwr_gpio_num,
        0u);
    sh->config2 = ULP_PACK_U16_PAIR(raw_low_limit, raw_high_limit);
    sh->config3 = ULP_PACK_U16_PAIR(raw_humidity_low_limit, raw_humidity_high_limit);

    ulp_hal_cfg_t cfg = {
        .wakeup_source      = ULP_HAL_WAKE_LP_TIMER,
        .lp_timer_period_us = period_ms * 1000u,
    };

    if (!_runLoaded(ULP_PROG_ID_SOFT_I2C_TEMP_WAKEUP, &cfg)) {
        return false;
    }

    esp_sleep_enable_ulp_wakeup();
    REG_SET_BIT(ULP_PMU_SLP_WAKEUP_CNTL2_REG, ULP_PMU_LP_CORE_WAKEUP_EN);

    return true;
}

void ESP32P4ULPClass::stop(void)
{
    REG_CLR_BIT(ULP_PMU_SLP_WAKEUP_CNTL2_REG, ULP_PMU_LP_CORE_WAKEUP_EN);
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

bool ESP32P4ULPClass::wokeFromULP(void)
{
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_ULP) {
        return true;
    }

    if (esp_reset_reason() != ESP_RST_WDT) {
        return false;
    }

    volatile ulp_shared_mem_t *sh = ulp_hal_shared_mem();
    return (sh->magic == ULP_SHARED_MAGIC) &&
           (sh->program_id != ULP_PROG_ID_NONE) &&
           ((sh->status & ULP_STATUS_WAKEUP_PENDING) != 0);
}
