/**
 * @file ulp_hal.c
 * @brief Register-level HAL for the ESP32-P4 LP core.
 *
 * Targets the ESP32-P4 LP core using the register layout bundled with Arduino-esp32.
 */

#include "ulp_hal.h"

#include <string.h>

#include "soc/soc.h"
#include "hal/lp_core_ll.h"
#include "hal/lp_timer_ll.h"
#include "hal/rtc_io_ll.h"
#include "soc/pmu_reg.h"
#include "esp_private/esp_clk.h"
#include "soc/rtc.h"
#include "esp_rom_sys.h"
#include "esp_private/periph_ctrl.h"

/* LP SRAM layout constants */

/* ESP-IDF aligns the front RTC-reserved area up to 256 bytes when LP ROM + ULP
 * are enabled on ESP32-P4, so the reserved ULP window starts at base + 0x100.
 * We mirror that layout here and load the LP binary at the start of that window. */
#define LP_BINARY_LOAD_OFFSET   256u
/* Vector table size per lp_core_riscv.ld */
#define LP_VECTOR_TABLE_SIZE    0x80u
/* Reset vector (start of .text) offset from LP SRAM base */
#define LP_RESET_VECTOR_OFFSET  (LP_BINARY_LOAD_OFFSET + LP_VECTOR_TABLE_SIZE)

/* Internal helpers */

static inline ulp_shared_mem_t *s_shared(void)
{
    return (ulp_shared_mem_t *)(SOC_LP_RAM_LOW + ULP_SHARED_MEM_OFFSET_BYTES);
}

static inline uint8_t *s_load_addr(void)
{
    return (uint8_t *)(SOC_LP_RAM_LOW + LP_BINARY_LOAD_OFFSET);
}

static inline size_t s_max_binary_size(void)
{
    return ULP_HAL_LP_RAM_SIZE - LP_BINARY_LOAD_OFFSET;
}

/* Wakeup source configuration */

static void s_configure_wakeup_sources(const ulp_hal_cfg_t *cfg)
{
    uint32_t ll = 0;
    if (cfg->wakeup_source & ULP_HAL_WAKE_HP_CPU)   ll |= LP_CORE_LL_WAKEUP_SOURCE_HP_CPU;
    if (cfg->wakeup_source & ULP_HAL_WAKE_LP_TIMER) ll |= LP_CORE_LL_WAKEUP_SOURCE_LP_TIMER;
    if (cfg->wakeup_source & ULP_HAL_WAKE_ETM)      ll |= LP_CORE_LL_WAKEUP_SOURCE_ETM;
    if (cfg->wakeup_source & ULP_HAL_WAKE_LP_IO)    ll |= LP_CORE_LL_WAKEUP_SOURCE_LP_IO;
    if (cfg->wakeup_source & ULP_HAL_WAKE_LP_UART)  ll |= LP_CORE_LL_WAKEUP_SOURCE_LP_UART;
    lp_core_ll_set_wakeup_source(ll);

    if ((cfg->wakeup_source & ULP_HAL_WAKE_LP_TIMER) && cfg->lp_timer_period_us > 0) {
        /* LP Timer is a free-running counter; target must be absolute (now + period). */
        lp_timer_ll_counter_snapshot(&LP_TIMER);
        uint64_t now = ((uint64_t)lp_timer_ll_get_counter_value_high(&LP_TIMER, 0) << 32)
                     |  (uint64_t)lp_timer_ll_get_counter_value_low(&LP_TIMER, 0);

        /* Convert us to RTC slow-clock ticks using the calibrated period.
         * Formula: ticks = us * (1 << RTC_CLK_CAL_FRACT) / esp_clk_slowclk_cal_get() */
        uint32_t cal   = esp_clk_slowclk_cal_get();
        uint64_t ticks = (cal > 0)
            ? ((uint64_t)cfg->lp_timer_period_us * (1u << RTC_CLK_CAL_FRACT) / cal)
            : ((uint64_t)cfg->lp_timer_period_us * 150u / 1000u);  /* 150 kHz fallback */
        uint64_t target = now + ticks;

        /* Write sleep duration to the IDF LP core shared config area so
         * lp_core_startup() re-arms the LP timer after each main() return.
         * ESP-IDF computes this as:
         *   _rtc_ulp_memory_start + ALIGN_DOWN(CONFIG_ULP_COPROC_RESERVE_MEM, 8)
         *   - CONFIG_ULP_SHARED_MEM
         * In this library, _rtc_ulp_memory_start maps to
         *   SOC_LP_RAM_LOW + LP_BINARY_LOAD_OFFSET. */
#ifndef CONFIG_ULP_COPROC_RESERVE_MEM
#  define CONFIG_ULP_COPROC_RESERVE_MEM  8192u  /* matches lp_core/sdkconfig.defaults */
#endif
        typedef struct { uint64_t sleep_duration_us; uint64_t sleep_duration_ticks; } idf_lp_shared_cfg_t;
        idf_lp_shared_cfg_t *idf_cfg = (idf_lp_shared_cfg_t *)(
            SOC_LP_RAM_LOW + LP_BINARY_LOAD_OFFSET +
            (CONFIG_ULP_COPROC_RESERVE_MEM & ~7u) - 16u);
        idf_cfg->sleep_duration_us     = cfg->lp_timer_period_us;
        idf_cfg->sleep_duration_ticks  = ticks;

        lp_timer_ll_clear_lp_alarm_intr_status(&LP_TIMER);
        lp_timer_ll_set_target_enable(&LP_TIMER, 1, false);
        lp_timer_ll_set_alarm_target(&LP_TIMER, 1, target);
        lp_timer_ll_set_target_enable(&LP_TIMER, 1, true);
    }
}

/* Public API */

ulp_hal_err_t ulp_hal_load_binary(const uint8_t *bin, size_t len)
{
    if (bin == NULL || len == 0) return ULP_HAL_ERR_INVALID_ARG;
    if (len > s_max_binary_size())  return ULP_HAL_ERR_INVALID_BINARY;

    memset(s_shared(), 0, sizeof(ulp_shared_mem_t));
    memcpy(s_load_addr(), bin, len);

    return ULP_HAL_OK;
}

ulp_hal_err_t ulp_hal_run(const ulp_hal_cfg_t *cfg)
{
    if (cfg == NULL) return ULP_HAL_ERR_INVALID_ARG;

    /* Set boot address before reset - bypasses LP ROM and avoids XTAL dependency. */
    intptr_t reset_vec = (intptr_t)(SOC_LP_RAM_LOW + LP_RESET_VECTOR_OFFSET);
    lp_core_ll_set_boot_address(reset_vec);
    lp_core_ll_set_app_boot_address(reset_vec);

    /* Reset LP core peripheral and enable its bus clock. */
    PERIPH_RCC_ATOMIC() {
        lp_core_ll_reset_register();
        lp_core_ll_enable_bus_clock(true);
    }

    lp_core_ll_stall_at_sleep_request(true);
    lp_core_ll_rst_at_sleep_enable(true);
    lp_core_ll_debug_module_enable(true);

    s_configure_wakeup_sources(cfg);

    /* For HP_CPU wakeup source, kick LP core immediately. */
    if (cfg->wakeup_source & ULP_HAL_WAKE_HP_CPU) {
        lp_core_ll_hp_wake_lp();
    }

    return ULP_HAL_OK;
}

void ulp_hal_stop(void)
{
    lp_core_ll_set_wakeup_source(0);
    lp_timer_ll_set_target_enable(&LP_TIMER, 1, false);
    lp_core_ll_request_sleep();
    /* Assert LP core reset and hold it - LP SRAM is now safe to overwrite.
     * ulp_hal_run() releases this via its own lp_core_ll_reset_register() call. */
    PERIPH_RCC_ATOMIC() {
        LPPERI.reset_en.rst_en_lp_core = 1;
    }
}

bool ulp_hal_lp_is_running(void)
{
    volatile ulp_shared_mem_t *sh = s_shared();
    return (sh->magic == ULP_SHARED_MAGIC) &&
           (sh->status & ULP_STATUS_RUNNING);
}

void ulp_hal_enable_lp_io_clock(void)
{
    PERIPH_RCC_ATOMIC() {
        _rtcio_ll_enable_io_clock(true);
    }
}

ulp_shared_mem_t *ulp_hal_shared_mem(void)
{
    return s_shared();
}
