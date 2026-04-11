# ESP32P4_ULP — Arduino Library

An Arduino library that exposes the **LP (Low Power) core** of the **ESP32-P4** to Arduino sketches despite the `ulp` component missing from Arduino-esp32.

> [!INFORMATION]  
> This library is highly experimental and subject to breakage with new Arduino-esp32 releases. Currently tested with Arduino-esp32 3.3.7 on Arduino IDE 2.3.8.

---

## Table of Contents

- [Background](#background)
- [How It Works](#how-it-works)
- [Installation](#installation)
- [Hardware: LP IO Pins](#hardware-lp-io-pins)
- [API Reference](#api-reference)
- [Example](#example)
- [Shared Memory Layout](#shared-memory-layout)
- [Further Guidance](#guidance)

---

## Background

The ESP32-P4 includes a dedicated **LP (Low Power) core** — a full RV32IMC RISC-V processor that runs independently while the main dual-core CPU is in deep sleep.  It has access to LP peripherals (LP IO, LP I2C, LP UART, LP SPI) and can wake the HP core when a condition is met.

ESP-IDF exposes this through its `ulp` component (`ulp_lp_core_load_binary()`, `ulp_lp_core_run()`, etc.).  **Arduino-esp32 does not include that component library**, so those functions cannot be called from an Arduino sketch.

This library solves this by:

1. Re-implementing the necessary hardware interaction using **register-level headers** that *are* present in Arduino-esp32's bundled ESP-IDF tree (`hal/lp_core_ll.h`, `soc/pmu_reg.h`, `soc/lp_timer_reg.h`, etc.).
2. Shipping a **pre-compiled LP core binary** as a `const uint8_t[]` array embedded in a header file, so Arduino only needs to copy bytes into LP SRAM — no need to compile LP core code.
3. Defining a **shared memory contract** that lets the main CPU pass configuration to the LP program and read results back, using a typed struct mapped at a fixed offset in LP SRAM.

---

## How It Works

```
┌─────────────────────────────────────────────────────────────────┐
│  Arduino sketch (HP core)                                       │
│                                                                 │
│  #include <ESP32P4_ULP.h>                                       │
│  ULP.wakeOnGPIO(LP_IO_8, HIGH);                                 │
│  ULP.clearWakeupPending();                                      │
│  esp_deep_sleep_start();                                        │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  ESP32P4_ULP.cpp  (C++ wrapper)                                 │
│  Populates shared memory config, calls ulp_hal_run()            │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  src/ulp_hal/ulp_hal.c  (register-level HAL)                    │
│                                                                 │
│  Uses ONLY header-only APIs:                                    │
│    hal/lp_core_ll.h   ← static-inline, always available         │
│    soc/pmu_reg.h      ← register addresses, always available    │
│    soc/lp_timer_reg.h ← register addresses, always available    │
│                                                                 │
│  Does NOT link against: libulp.a or ulp component               │
└──────────────────────┬──────────────────────────────────────────┘
                       │  memcpy binary into LP SRAM
                       │  write config into ulp_shared_mem_t
                       │  configure LP timer / PMU registers
                       │  release LP CPU reset
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  LP SRAM  (0x50108000, 32 KB, visible to both cores)            │
│                                                                 │
│  ┌────────────────────────────────┐ ← offset 32                 │
│  │  ulp_shared_mem_t  (64 bytes) │   HP writes config           │
│  │  magic / program_id / config  │   LP writes status / data    │
│  ├────────────────────────────────┤ ← offset 256                │
│  │  LP core binary               │   pre-compiled RV32 code     │
│  │  (copied from uint8_t array)  │                              │
│  └────────────────────────────────┘                             │
└──────────────────────┬──────────────────────────────────────────┘
                       │  LP CPU executes
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  LP core program  (RV32IMC, pre-compiled with ESP-IDF)          │
│                                                                 │
│  Reads config from shared header                                │
│  Polls LP IO pin at 1 ms intervals                              │
│  Writes sampled level back to shared header                     │
│  Calls ulp_lp_core_wakeup_main_processor() when matched         │
└─────────────────────────────────────────────────────────────────┘
```

---

## Installation

### Arduino IDE

1. Download or clone this repository.
2. **Sketch → Include Library → Add .ZIP Library…** and select the repository folder.
3. Select board: **ESP32P4 Dev Module** (or any ESP32-P4 board in Arduino-esp32 ≥ 3.x).

---

## Hardware: LP IO Pins

The ESP32-P4 has **16 LP IO pins** (`LP_IO_0` … `LP_IO_15`).  These map 1:1 to GPIO0–GPIO15.  Before using any LP IO pin, we must route it through the LP IO mux:

```cpp
#include "driver/rtc_io.h"

rtc_gpio_init(GPIO_NUM_8);
rtc_gpio_set_direction(GPIO_NUM_8, RTC_GPIO_MODE_INPUT_ONLY);
rtc_gpio_pulldown_en(GPIO_NUM_8);
rtc_gpio_pullup_dis(GPIO_NUM_8);
```

---

## API Reference

```cpp
#include <ESP32P4_ULP.h>
```

### wakeOnGPIO()

```cpp
bool ULP.wakeOnGPIO(uint8_t lp_gpio_num, uint8_t wake_level,
                    uint32_t debounce = 0);
```

Load the `gpio_wakeup` LP program and arm it.  The LP core polls the specified pin at ~1 ms intervals.  When the level matches `wake_level` (confirmed for `debounce` consecutive cycles if non-zero), the LP core wakes the HP core from deep sleep.

| Parameter | Description |
|---|---|
| `lp_gpio_num` | LP IO pin number (0–15); use `LP_IO_0`…`LP_IO_15` |
| `wake_level` | `HIGH` or `LOW` |
| `debounce` | Consecutive matching cycles required (0 = immediate) |

**Returns:** `true` on success; `false` if the binary is a placeholder or load fails.

### clearWakeupPending()

```cpp
void ULP.clearWakeupPending(void);
```

Clear the stale LP→HP wakeup interrupt.  **Must be called before `esp_deep_sleep_start()`** to prevent the PMU from immediately re-waking on the previous cycle's trigger.

### Other Methods

| Method | Description |
|---|---|
| `void stop()` | Stop the LP core and disable all wakeup sources |
| `bool isRunning()` | `true` if the LP program has set `ULP_STATUS_RUNNING` |
| `uint32_t getData(uint8_t index)` | Read `data[0..7]` written by the LP core |
| `ulp_shared_mem_t *sharedMem()` | Direct pointer to the shared memory struct |

---

## Example

### WakeOnGPIO

`examples/WakeOnGPIO/WakeOnGPIO.ino`

Enters deep sleep and wakes when GPIO8 goes HIGH.  Connect a button between GPIO8 and 3.3V.

```cpp
#include <Arduino.h>
#include "ESP32P4_ULP.h"
#include "driver/rtc_io.h"

void setup() {
    Serial.begin(115200);
    delay(500);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_ULP) {
        Serial.printf("Woke from ULP — counter: %lu, level: %lu\n",
                      (uint32_t)ULP.sharedMem()->lp_counter, ULP.getData(0));
    } else {
        Serial.printf("First boot, cause=%d\n", (int)cause);
    }

    rtc_gpio_init(GPIO_NUM_8);
    rtc_gpio_set_direction(GPIO_NUM_8, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(GPIO_NUM_8);
    rtc_gpio_pullup_dis(GPIO_NUM_8);

    ULP.wakeOnGPIO(LP_IO_8, HIGH);
    ULP.clearWakeupPending();
    esp_deep_sleep_start();
}

void loop() {}
```

---

## Shared Memory Layout

The `ulp_shared_mem_t` struct is 64 bytes, mapped at offset 32 in LP SRAM (`0x50108020`).

```
Offset  Field          Written by  Description
──────  ─────────────  ──────────  ────────────────────────────────
0x00    magic          HP          0x554C5000 ("ULP\0")
0x04    program_id     HP          ULP_PROG_ID_GPIO_WAKEUP (0x01)
0x08    config0        HP          Target LP IO number (0–15)
0x0C    config1        HP          Wake level (0=LOW, 1=HIGH)
0x10    config2        HP          Debounce cycle count
0x14    reserved0      —           Reserved
0x18    status         LP          ULP_STATUS_* bitmask
0x1C    lp_counter     LP          Incremented each LP cycle
0x20    data[0]        LP          Last sampled GPIO level
0x24    data[1..7]     LP          Available for future use
```

---

## Guidance

- If the device is in deep sleep, enter DFU mode to flash, then re-power the device to run new code
- **ESP32-P4 only.**  Other ESP32 variants use different ULP types and are not supported.
- **One LP program at a time.**  Only one program can be loaded into LP SRAM.
- **LP IO only.**  The LP core can only access LP IO pins (GPIO0–GPIO15).
