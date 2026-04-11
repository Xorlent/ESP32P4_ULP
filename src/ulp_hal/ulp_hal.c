/**
 * @file ulp_hal.c
 * @brief Register-level HAL implementation for the ESP32-P4 LP core.
 *
 * Design rationale
 * ────────────────
 * Arduino-esp32 ships the full ESP-IDF SOC/HAL header tree but does NOT
 * expose the compiled `ulp` component library.  The functions we need
 * (ulp_lp_core_load_binary, ulp_lp_core_run, …) live in that library, so
 * we cannot call them from an Arduino sketch.
 *
 * However, those functions ultimately reduce to a handful of register writes.
 * All register definitions are in *header* files that ARE present:
 *
 *   hal/lp_core_ll.h       — Static-inline LL driver (header-only, zero link)
 *   soc/pmu_reg.h          — PMU register addresses & bit fields
 *   soc/lp_sys_reg.h       — LP CPU reset / clock gate registers
 *   soc/lp_timer_reg.h     — LP timer alarm registers
 *   soc/rtc_io_reg.h       — LP IO wakeup pin registers
 *
 * We prefer hal/lp_core_ll.h because it isolates hardware differences; we
 * fall back to raw REG_WRITE macros where the LL header is not available.
 *
 * References
 * ──────────
 * ESP32-P4 TRM §LP Core, §PMU, §LP Timer
 * ESP-IDF components/hal/esp32p4/include/hal/lp_core_ll.h
 * ESP-IDF components/ulp/lp_core/src/ulp_lp_core.c
 */

#include "ulp_hal.h"

#include <string.h>   /* memcpy */
#include <stdint.h>

/* ── SOC / HAL headers — all present in Arduino-esp32 ────────────────────── */
#include "soc/soc.h"              /* REG_WRITE, READ_PERI_REG, SET_PERI_REG_BITS … */
#include "soc/chip_revision.h"    /* esp_efuse_get_chip_ver() if needed            */

/* lp_core_ll.h provides static-inline functions that compile to the same
 * register writes as the ulp component, but without any linkage dependency.
 * It is distributed as part of the `hal` component headers.              */
#if __has_include("hal/lp_core_ll.h")
#  include "hal/lp_core_ll.h"
#  define HAVE_LP_CORE_LL 1
#else
#  define HAVE_LP_CORE_LL 0
#endif

/* LP timer — alarm sets the periodic wakeup interval.
 * lp_timer_ll.h includes lp_timer_reg.h and lp_timer_struct.h, giving us
 * both the LP_TIMER device instance and register macros. */
#if __has_include("hal/lp_timer_ll.h")
#  include "hal/lp_timer_ll.h"
#  define HAVE_LP_TIMER_REG 1
#elif __has_include("soc/lp_timer_reg.h")
#  include "soc/lp_timer_reg.h"
#  define HAVE_LP_TIMER_REG 1
#else
#  define HAVE_LP_TIMER_REG 0
#endif

/* RTC slow-clock calibration — for converting µs to LP timer ticks.
 * The LP timer counter is clocked by the RTC slow clock (not LP_FAST_CLK). */
#if __has_include("esp_clk.h")
#  include "esp_clk.h"          /* esp_clk_slowclk_cal_get() */
#  define HAVE_ESP_CLK 1
#else
#  define HAVE_ESP_CLK 0
#endif
#if __has_include("soc/rtc.h")
#  include "soc/rtc.h"          /* RTC_CLK_CAL_FRACT */
#  define HAVE_RTC_H 1
#else
#  define HAVE_RTC_H 0
#  define RTC_CLK_CAL_FRACT 19  /* fallback: standard IDF value */
#endif

/* PMU register — controls LP CPU power and SW interrupt lines */
#if __has_include("soc/pmu_reg.h")
#  include "soc/pmu_reg.h"
#  define HAVE_PMU_REG 1
#else
#  define HAVE_PMU_REG 0
#endif

/* LP system register — LP CPU reset and clock gate */
#if __has_include("soc/lp_sys_reg.h")
#  include "soc/lp_sys_reg.h"
#  define HAVE_LP_SYS_REG 1
#else
#  define HAVE_LP_SYS_REG 0
#endif

/* ROM delay helper — available in all IDF versions */
#if __has_include("esp_rom_sys.h")
#  include "esp_rom_sys.h"
#  define ROM_DELAY_US(us)  esp_rom_delay_us(us)
#else
#  include "rom/ets_sys.h"
#  define ROM_DELAY_US(us)  ets_delay_us(us)
#endif

/* RCC atomic wrapper — required by lp_core_ll_enable_bus_clock and
 * lp_core_ll_reset_register macros which reference __DECLARE_RCC_ATOMIC_ENV */
#if __has_include("esp_private/periph_ctrl.h")
#  include "esp_private/periph_ctrl.h"
#else
#  define PERIPH_RCC_ATOMIC()  /* no-op: macros won't need atomic env */
#endif

/* ── Arduino-esp32 / IDF 5.5+ compatibility shims ────────────────────────── *
 * The Arduino-esp32 LL headers renamed several functions and registers.
 * We map the original names used in this file to the new names here so the
 * source works with both old ESP-IDF and Arduino-esp32 without edits elsewhere.
 */
#if HAVE_LP_CORE_LL

#ifndef lp_core_ll_reset_lp_cpu
#  include "soc/lpperi_struct.h"
#  define lp_core_ll_reset_lp_cpu() \
       do { LPPERI.reset_en.rst_en_lp_core = 1; \
            LPPERI.reset_en.rst_en_lp_core = 0; } while (0)
#endif

#ifndef lp_core_ll_run_lp_cpu
#  define lp_core_ll_run_lp_cpu()     lp_core_ll_hp_wake_lp()
#endif

#ifndef lp_core_ll_halt_lp_cpu
#  define lp_core_ll_halt_lp_cpu()    lp_core_ll_request_sleep()
#endif

#ifndef lp_core_ll_set_lp_rom_boot_en
#  define lp_core_ll_set_lp_rom_boot_en(use_rom) \
       do { if (!(use_rom)) { \
           lp_core_ll_set_boot_address( \
               (intptr_t)(SOC_LP_RAM_LOW + ULP_BINARY_LOAD_OFFSET_BYTES)); \
       } } while (0)
#endif

#ifndef lp_core_ll_sw_intr_trigger
#  define lp_core_ll_sw_intr_trigger() lp_core_ll_hp_wake_lp()
#endif

#endif /* HAVE_LP_CORE_LL */

#if HAVE_LP_TIMER_REG
#  ifndef LP_TIMER_LP_TIMER_TARGET_LO_REG
#    define LP_TIMER_LP_TIMER_TARGET_LO_REG       LP_TIMER_TAR1_LOW_REG
#    define LP_TIMER_LP_TIMER_TARGET_HI_REG       LP_TIMER_TAR1_HIGH_REG
#    define LP_TIMER_MAIN_TIMER_TARGET_WORK_EN_M  LP_TIMER_MAIN_TIMER_TAR_EN1_M
#  endif
#endif

#if HAVE_PMU_REG
#  ifndef PMU_LP_CPU_SLP_WAITI_EN_M
#    define PMU_LP_CPU_SLP_WAITI_EN_M  PMU_LP_CPU_SLP_WAITI_FLAG_EN_M
#  endif
#endif

/* ── LP SRAM layout constants ────────────────────────────────────────────── */
/*
 * On ESP32-P4 the IDF linker script places the LP binary at:
 *   SOC_LP_RAM_LOW + RESERVE_RTC_MEM = 0x50108000 + ALIGN_UP(24, 256) = 0x50108100
 *
 * The first 0x80 bytes at that offset are the vector table; the reset vector
 * (start of .text) is at offset LP_BINARY_LOAD_OFFSET + LP_VECTOR_TABLE_SIZE.
 *
 * The shared memory struct sits at LP address 0 = HP address SOC_LP_RAM_LOW,
 * safely below the 256-byte reserved region (HP RTC timer uses only bytes 0..23).
 */
#define LP_BINARY_LOAD_OFFSET    256u    /* = ALIGN_UP(RTC_TIMER_RESERVE_RTC=24, 256) */
#define LP_VECTOR_TABLE_SIZE     0x80u   /* per lp_core_riscv.ld */
#define LP_RESET_VECTOR_OFFSET   (LP_BINARY_LOAD_OFFSET + LP_VECTOR_TABLE_SIZE)  /* 0x180 */

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/** Pointer to LP SRAM shared memory header (HP-core virtual address). */
static inline ulp_shared_mem_t *s_shared(void)
{
    return (ulp_shared_mem_t *)(SOC_LP_RAM_LOW + ULP_SHARED_MEM_OFFSET_BYTES);
}

/** Pointer to the load area inside LP SRAM (binary starts after reserved region). */
static inline uint8_t *s_load_addr(void)
{
    return (uint8_t *)(SOC_LP_RAM_LOW + LP_BINARY_LOAD_OFFSET);
}

/** Maximum number of bytes available for the LP binary. */
static inline size_t s_max_binary_size(void)
{
    return ULP_HAL_LP_RAM_SIZE - LP_BINARY_LOAD_OFFSET;
}

/* ── LP CPU reset / run — prefer LL, fall back to direct register writes ─── */

static void s_reset_lp_cpu(void)
{
#if HAVE_LP_CORE_LL
    lp_core_ll_reset_lp_cpu();
#elif HAVE_LP_SYS_REG
    /* Assert reset */
    SET_PERI_REG_MASK(LP_SYS_LP_CPU_STATUS_REG, LP_SYS_LP_CPU_RESET_EN_M);
    ROM_DELAY_US(10);
    /* De-assert reset */
    CLEAR_PERI_REG_MASK(LP_SYS_LP_CPU_STATUS_REG, LP_SYS_LP_CPU_RESET_EN_M);
#else
#  error "Cannot reset LP CPU: neither hal/lp_core_ll.h nor soc/lp_sys_reg.h found"
#endif
}

static void __attribute__((unused)) s_run_lp_cpu(void)
{
#if HAVE_LP_CORE_LL
    lp_core_ll_run_lp_cpu();
#elif HAVE_PMU_REG
    /* Release LP CPU from halt */
    SET_PERI_REG_MASK(PMU_LP_CPU_PWR0_REG, PMU_LP_CPU_WAKEUP_EN_M);
#else
#  error "Cannot run LP CPU: neither hal/lp_core_ll.h nor soc/pmu_reg.h found"
#endif
}

static void s_halt_lp_cpu(void)
{
#if HAVE_LP_CORE_LL
    lp_core_ll_halt_lp_cpu();
#elif HAVE_PMU_REG
    CLEAR_PERI_REG_MASK(PMU_LP_CPU_PWR0_REG, PMU_LP_CPU_WAKEUP_EN_M);
#endif
}

/* ── Wakeup source configuration ─────────────────────────────────────────── */

/* Track the active wakeup sources */
static uint32_t s_active_ll_wakeup = 0;

/* Translate ULP_HAL_WAKE_* flags → hardware bit positions for
 * lp_core_ll_set_wakeup_source().  LP_CORE_LL_WAKEUP_SOURCE_* constants
 * are defined in hal/lp_core_ll.h and differ from our compact flag values. */
static uint32_t s_hal_flags_to_ll(uint32_t flags)
{
    uint32_t ll = 0;
#if HAVE_LP_CORE_LL
    if (flags & ULP_HAL_WAKE_HP_CPU)   ll |= LP_CORE_LL_WAKEUP_SOURCE_HP_CPU;
    if (flags & ULP_HAL_WAKE_LP_TIMER) ll |= LP_CORE_LL_WAKEUP_SOURCE_LP_TIMER;
    if (flags & ULP_HAL_WAKE_ETM)      ll |= LP_CORE_LL_WAKEUP_SOURCE_ETM;
    if (flags & ULP_HAL_WAKE_LP_IO)    ll |= LP_CORE_LL_WAKEUP_SOURCE_LP_IO;
    if (flags & ULP_HAL_WAKE_LP_UART)  ll |= LP_CORE_LL_WAKEUP_SOURCE_LP_UART;
#endif
    return ll;
}

static void s_configure_wakeup_sources(const ulp_hal_cfg_t *cfg)
{
#if HAVE_LP_CORE_LL
    s_active_ll_wakeup = s_hal_flags_to_ll(cfg->wakeup_source);
    lp_core_ll_set_wakeup_source(s_active_ll_wakeup);
#else
    /*
     * Fallback: program PMU wakeup enable bits directly.
     * Bit positions from ESP32-P4 TRM §PMU_LP_CPU_PWR0_REG.
     */
#if HAVE_PMU_REG
    uint32_t pmu_val = READ_PERI_REG(PMU_LP_CPU_PWR0_REG);

    /* Clear all wakeup enables first */
    pmu_val &= ~(PMU_LP_CPU_WAKEUP_EN_M);

    if (cfg->wakeup_source & ULP_HAL_WAKE_HP_CPU)
        pmu_val |= PMU_LP_CPU_WAKEUP_EN_M;  /* HP→LP SW interrupt wake */

    /* Timer and peripheral wakeup bits differ per register set; set them
     * via the LP_TIMER and PMU_HP_SLEEP registers as appropriate. */
    REG_WRITE(PMU_LP_CPU_PWR0_REG, pmu_val);
#endif /* HAVE_PMU_REG */
#endif /* HAVE_LP_CORE_LL */

    /* ── LP Timer period (if requested) ──────────────────────────────── */
#if HAVE_LP_TIMER_REG
    if ((cfg->wakeup_source & ULP_HAL_WAKE_LP_TIMER) && cfg->lp_timer_period_us > 0) {
        /*
         * LP Timer is a free-running counter.  We must set an ABSOLUTE target
         * = current_count + period_ticks.  Writing a small literal value (as
         * was done before) programs a target already in the past and the alarm
         * never fires.
         *
         * LP_FAST_CLK is ~16 MHz on ESP32-P4 (LP_RC_FAST oscillator).
         * ticks_per_us ≈ 16.
         */
        /* Snapshot the current counter value.
         * The LP timer runs on RTC_SLOW_CLK — counter snapshot reads from
         * index 0 (matches IDF ulp_lp_core_lp_timer_shared.c). */
        lp_timer_ll_counter_snapshot(&LP_TIMER);
        uint64_t now = ((uint64_t)lp_timer_ll_get_counter_value_high(&LP_TIMER, 0) << 32)
                     |  (uint64_t)lp_timer_ll_get_counter_value_low(&LP_TIMER, 0);

        /* Convert µs to slow-clock ticks using the calibrated value.
         * Formula matches IDF ulp_lp_core_lp_timer_calculate_sleep_ticks():
         *   ticks = us * (1 << RTC_CLK_CAL_FRACT) / esp_clk_slowclk_cal_get() */
#if HAVE_ESP_CLK
        uint32_t cal = esp_clk_slowclk_cal_get();
        uint64_t ticks = (cal > 0)
            ? ((uint64_t)cfg->lp_timer_period_us * (1u << RTC_CLK_CAL_FRACT) / cal)
            : ((uint64_t)cfg->lp_timer_period_us * 150u / 1000u); /* 150 kHz fallback */
#else
        /* No calibration API — assume 150 kHz RC slow clock (~6.67 µs/tick) */
        uint64_t ticks = (uint64_t)cfg->lp_timer_period_us * 150u / 1000u;
#endif
        uint64_t target = now + ticks;

        /* Write sleep_duration_ticks to the IDF LP core shared config area.
         * After main() returns, lp_core_startup() reads this value and
         * re-arms the LP timer.  Without this write the LP only runs once.
         *
         * Address formula (from ulp_lp_core_memory_shared.c):
         *   base + ALIGN_DOWN(CONFIG_ULP_COPROC_RESERVE_MEM, 8) - CONFIG_ULP_SHARED_MEM
         *   = 0x50108100 + 0x1FF0 = 0x5010A0F0
         * sizeof(ulp_lp_core_memory_shared_cfg_t) = 16  (two uint64_t)
         * CONFIG_ULP_SHARED_MEM = 16 (confirmed by ESP_STATIC_ASSERT in IDF)
         */
#ifndef CONFIG_ULP_COPROC_RESERVE_MEM
#  define CONFIG_ULP_COPROC_RESERVE_MEM  8192u  /* matches lp_core/sdkconfig.defaults */
#endif
        typedef struct { uint64_t sleep_duration_us; uint64_t sleep_duration_ticks; } idf_lp_shared_cfg_t;
        idf_lp_shared_cfg_t *idf_cfg = (idf_lp_shared_cfg_t *)(
            SOC_LP_RAM_LOW + LP_BINARY_LOAD_OFFSET +
            ((CONFIG_ULP_COPROC_RESERVE_MEM) & ~7u) - 16u);
        idf_cfg->sleep_duration_us     = cfg->lp_timer_period_us;
        idf_cfg->sleep_duration_ticks  = ticks;

        /* Clear stale alarm interrupt, then arm the new target (slot 1) */
        lp_timer_ll_clear_lp_alarm_intr_status(&LP_TIMER);
        lp_timer_ll_set_target_enable(&LP_TIMER, 1, false);
        lp_timer_ll_set_alarm_target(&LP_TIMER, 1, target);
        lp_timer_ll_set_target_enable(&LP_TIMER, 1, true);
    }
#endif /* HAVE_LP_TIMER_REG */
}

/* ══════════════════════════════════════════════════════════════════════════ *
 *  Public API implementation
 * ══════════════════════════════════════════════════════════════════════════ */

ulp_hal_err_t ulp_hal_load_binary(const uint8_t *bin, size_t len)
{
    if (bin == NULL || len == 0) return ULP_HAL_ERR_INVALID_ARG;
    if (len > s_max_binary_size())  return ULP_HAL_ERR_INVALID_BINARY;

    /* Clear the shared memory header so the LP program starts fresh */
    memset(s_shared(), 0, sizeof(ulp_shared_mem_t));

    /* Copy binary into LP SRAM at the correct link address (offset 256) */
    memcpy(s_load_addr(), bin, len);

    return ULP_HAL_OK;
}

ulp_hal_err_t ulp_hal_run(const ulp_hal_cfg_t *cfg)
{
    if (cfg == NULL) return ULP_HAL_ERR_INVALID_ARG;

#if HAVE_LP_CORE_LL
    /* 1. Set boot address BEFORE reset — LP core fetches from here on
     *    reset de-assertion (matches IDF lp_core.c: set_boot_address before
     *    LP_CORE_RCC_ATOMIC block).
     *
     *    We bypass LP ROM to avoid a hang: LP ROM initialises LP UART using
     *    the XTAL clock (off during deep sleep).  Our start.S handles stack,
     *    BSS and .data initialisation itself, so LP ROM is not needed.
     */
    {
        intptr_t reset_vec = (intptr_t)(SOC_LP_RAM_LOW + LP_RESET_VECTOR_OFFSET);
        lp_core_ll_set_boot_address(reset_vec);
        lp_core_ll_set_app_boot_address(reset_vec);
    }

    /* 2. Reset LP core peripheral then enable its bus clock — exactly as IDF
     *    LP_CORE_RCC_ATOMIC() { lp_core_ll_reset_register();
     *                            lp_core_ll_enable_bus_clock(true); }
     *    Reset de-assertion with clock enabled starts the LP CPU from boot_addr.
     */
    PERIPH_RCC_ATOMIC() {
        lp_core_ll_reset_register();
        lp_core_ll_enable_bus_clock(true);
    }

    /* 3. Sleep / stall behaviour — mirrors IDF order exactly */
    lp_core_ll_stall_at_sleep_request(true);
    lp_core_ll_rst_at_sleep_enable(true);

    /* 4. Enable JTAG debug module (IDF always does this) */
    lp_core_ll_debug_module_enable(true);

#else
    s_reset_lp_cpu();
    ROM_DELAY_US(20);
#endif

    /* 5. Program wakeup sources and LP timer alarm */
    s_configure_wakeup_sources(cfg);

    /* 6. For HP_CPU wakeup source, kick LP core now — IDF does the same */
#if HAVE_LP_CORE_LL
    if (cfg->wakeup_source & ULP_HAL_WAKE_HP_CPU) {
        lp_core_ll_hp_wake_lp();
    }
#endif

    return ULP_HAL_OK;
}

void ulp_hal_stop(void)
{
#if HAVE_LP_CORE_LL
    lp_core_ll_set_wakeup_source(0);
#elif HAVE_PMU_REG
    CLEAR_PERI_REG_MASK(PMU_LP_CPU_PWR0_REG, PMU_LP_CPU_WAKEUP_EN_M);
#endif

#if HAVE_LP_TIMER_REG
    lp_timer_ll_set_target_enable(&LP_TIMER, 1, false);
#endif

    s_halt_lp_cpu();
}

bool ulp_hal_lp_is_running(void)
{
    volatile ulp_shared_mem_t *sh = s_shared();
    return (sh->magic == ULP_SHARED_MAGIC) &&
           (sh->status & ULP_STATUS_RUNNING);
}

ulp_shared_mem_t *ulp_hal_shared_mem(void)
{
    return s_shared();
}
