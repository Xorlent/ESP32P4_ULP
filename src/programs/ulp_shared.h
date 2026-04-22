/**
 * @file ulp_shared.h
 * @brief Shared memory layout between the HP core and every pre-built LP core
 *        program in this library.
 *
 * This header is included by BOTH sides:
 *   - HP side  : src/ulp_hal/ulp_hal.c  (via the Arduino library)
 *   - LP side  : lp_core/<program>/main.c  (compiled with ESP-IDF toolchain)
 *
 * The struct is mapped at a fixed offset inside LP SRAM so that both cores
 * can find it without any dynamic linking.
 *
 * LP SRAM layout (SOC_LP_RAM_LOW = 0x50108000, 32 KB):
 *   offset  32 : ulp_shared_mem_t (64 bytes)
 *   offset 256 : LP binary vector table (128 bytes)
 *   offset 384 : LP binary .text / reset vector
 *
 * HP core writes header/config fields BEFORE calling ulp_hal_run().
 * LP core reads config on every wake-up cycle.
 * LP core writes status/data fields; HP core reads them at leisure.
 */

#pragma once
#ifndef ULP_SHARED_H
#define ULP_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic value written by the HP core before starting an LP program */
#define ULP_SHARED_MAGIC        0x554C5000UL   /* "ULP\0" */

/* LP SRAM base address as seen by both HP core and LP core (RISC-V).
 * The LP core linker script uses 0x50108xxx physical addresses, so a
 * pointer with value 0 is a null pointer from the LP core's perspective.
 * All accesses to the shared struct must use this base. */
#ifndef ULP_LP_SRAM_BASE
#define ULP_LP_SRAM_BASE  0x50108000UL
#endif

/* Byte offset inside LP RAM where this struct lives.
 * The first 24 bytes of LP SRAM are reserved by IDF for the RTC timer
 * calibration data (RESERVE_RTC_MEM = 24). Our struct must sit after
 * that region. We use byte 32 for 8-byte alignment safety. */
#ifndef ULP_SHARED_MEM_OFFSET_BYTES
#define ULP_SHARED_MEM_OFFSET_BYTES  32
#endif

/* Absolute address of the shared struct (use this in LP core code) */
#define ULP_SHARED_MEM_ADDR  (ULP_LP_SRAM_BASE + ULP_SHARED_MEM_OFFSET_BYTES)

/* Byte offset inside LP RAM where the binary is loaded.
 * RESERVE_RTC_MEM = 24 bytes, rounded up to 256-byte alignment. */
#define ULP_BINARY_LOAD_OFFSET_BYTES  256u

/* Program IDs */
#define ULP_PROG_ID_NONE            0x00
#define ULP_PROG_ID_GPIO_WAKEUP     0x01
#define ULP_PROG_ID_INT_WAKEUP      0x02
#define ULP_PROG_ID_SOFT_I2C_TEMP_WAKEUP 0x03

/* Status flags (status field bitmask) */
#define ULP_STATUS_RUNNING          (1u << 0)
#define ULP_STATUS_WAKEUP_PENDING   (1u << 1)

/* Helpers for program-specific packed 16-bit values stored in 32-bit words. */
#define ULP_PACK_U16_PAIR(low16, high16) \
    ((((uint32_t)(uint16_t)(low16)) & 0xFFFFu) | \
     (((uint32_t)(uint16_t)(high16)) << 16))
#define ULP_UNPACK_U16_PAIR_LOW(word32)   ((uint16_t)((word32) & 0xFFFFu))
#define ULP_UNPACK_U16_PAIR_HIGH(word32)  ((uint16_t)(((word32) >> 16) & 0xFFFFu))

/**
 * @brief Shared memory header placed at offset 32 of LP SRAM.
 *
 * All fields are volatile because both cores can modify them concurrently.
 * Reads/writes are 32-bit aligned so that the hardware performs them
 * atomically on both RV32 and Xtensa.
 *
 * Total size: 64 bytes (16 x uint32_t).
 */
typedef struct __attribute__((packed, aligned(4))) {
    /* Written by HP core before ulp_hal_run() */
    volatile uint32_t magic;        /**< ULP_SHARED_MAGIC when HP has initialized */
    volatile uint32_t program_id;   /**< Which program is loaded (ULP_PROG_ID_*) */
    volatile uint32_t config0;      /**< Program-specific: see per-program docs */
    volatile uint32_t config1;      /**< Program-specific */
    volatile uint32_t config2;      /**< Program-specific */
    volatile uint32_t config3;      /**< Program-specific */

    /* Written by LP core after start */
    volatile uint32_t status;       /**< ULP_STATUS_* bitmask */
    volatile uint32_t lp_counter;   /**< Incremented each LP wake cycle */
    volatile uint32_t data[8];      /**< General-purpose data exchange */
} ulp_shared_mem_t;

/* Compile-time size check - both sides must agree */
typedef char ulp_shared_mem_size_check[
    (sizeof(ulp_shared_mem_t) == 64) ? 1 : -1];

/*
 * GPIO_WAKEUP config fields:
 *   config0 : target LP IO number  (0-15, matches lp_io_num_t)
 *   config1 : wake level           (0 = wake on LOW, 1 = wake on HIGH)
 *   config2 : debounce cycles      (0 = no debounce)
 *   data[0] : last sampled GPIO level
 *
 * INT_WAKEUP config fields:
 *   config0 : target LP IO number  (0-15, matches lp_io_num_t)
 *   config1 : LP GPIO interrupt type (1-5, matches LP_GPIO_INT_*)
 *   config2 : reserved
 *   data[0] : last sampled GPIO level seen by the LP ISR
 *
 * SOFT_I2C_TEMP_WAKEUP config fields:
 *   config0 : packed LP IO numbers
 *             low halfword  = SDA LP IO number (0-15, matches lp_io_num_t)
 *             high halfword = SCL LP IO number (0-15, matches lp_io_num_t)
 *   config1 : packed extra pin config
 *             low halfword  = optional LP IO power pin (0-15, matches lp_io_num_t)
 *                             disabled when this halfword is greater than 15
 *             high halfword = reserved
 *   config2 : packed raw temperature thresholds
 *             low halfword  = raw low temperature threshold
 *             high halfword = raw high temperature threshold
 *             disabled when low halfword > high halfword
 *   config3 : packed raw humidity thresholds
 *             low halfword  = raw low humidity threshold
 *             high halfword = raw high humidity threshold
 *             disabled when low halfword > high halfword
 *   data[0] : last raw temperature sample (SHT4X 16-bit sample in low halfword)
 *   data[1] : last software-I2C / protocol status code
 *             0 = OK, 1 = bus not idle, 2 = command address NACK,
 *             3 = command byte NACK, 4 = read address NACK,
 *             5 = temperature CRC fail, 6 = humidity CRC fail,
 *             7 = STOP failed / bus stuck low
 *   data[2] : last raw humidity sample (SHT4X 16-bit sample in low halfword)
 */

#ifdef __cplusplus
}
#endif

#endif /* ULP_SHARED_H */
