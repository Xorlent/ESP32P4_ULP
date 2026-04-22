# ESP32P4_ULP

Arduino support for the **ESP32-P4 LP (Low Power) core** using bundled pre-built LP binaries and a small register-level HAL, so Arduino sketches can use LP-core wake programs even though Arduino-esp32 does not ship the ESP-IDF `ulp` component.

## Quick Start

1. Install the "ESP32P4_ULP by Xorlent" Arduino library
2. Get deep sleep wake on GPIO 8 by copying the example below

```cpp
#include <ESP32P4_ULP.h>

void setup()
{
    if (ULP.wokeFromULP()) {
        ULP.stop();
        // Add your wake program code/function calls here
    }

    ULP.wakeOnGPIO(LP_IO_8, HIGH);
    ULP.clearWakeupPending();
    esp_deep_sleep_start();
}

void loop()
{
}
```

---

## Table of Contents

- [Background](#background)
- [What This Library Includes](#what-this-library-includes)
- [Installation](#installation)
- [Hardware: LP IO Pins](#hardware-lp-io-pins)
- [API Reference](#api-reference)
- [Examples](#examples)
- [Shared Memory Layout](#shared-memory-layout)
- [Guidance](#guidance)
- [Reference Material](#reference-material)

---

> [!IMPORTANT]
> This library is experimental and subject to breakage with new Arduino-esp32 releases. The current implementation targets Arduino-esp32 3.3.x and was verified on Arduino-esp32 3.3.8 on Arduino IDE 2.3.8.

## Background

The ESP32-P4 includes a dedicated **LP (Low Power) core** - a full RISC-V processor that runs independently while the main dual-core CPU is in deep sleep. It has access to LP peripherals (LP IO, LP I2C, LP UART, LP SPI) and can wake the HP core when a condition is met.

ESP-IDF exposes this through its `ulp` component (`ulp_lp_core_load_binary()`, `ulp_lp_core_run()`, etc.). **Arduino-esp32 does not include that component library**, so those functions cannot be called from an Arduino sketch.

This library solves this by:

1. Re-implementing the necessary hardware interaction using **register-level headers** that *are* present in Arduino-esp32's bundled ESP-IDF tree (`hal/lp_core_ll.h`, `soc/pmu_reg.h`, `soc/lp_timer_reg.h`, etc.).
2. Shipping a **pre-compiled LP core binary** as a `const uint8_t[]` array embedded in a header file, so Arduino only needs to copy bytes into LP SRAM - no need to compile LP core code.
3. Defining a **shared memory contract** that lets the main CPU pass configuration to the LP program and read results back, using a typed struct mapped at a fixed offset in LP SRAM.
4. Working around an Arduino-esp32 limitation on ESP32-P4 where LP-core wakes may reboot through the HP watchdog path instead of being reported as `ESP_SLEEP_WAKEUP_ULP`.

## What This Library Includes

The Arduino-facing wrapper in `ESP32P4_ULP.h` currently exposes three bundled LP programs:

1. `gpio_wakeup`: Poll an LP IO pin on the LP timer and wake on a target level with optional debounce.
2. `int_wakeup`: Configure an LP IO interrupt source and wake when the selected LP GPIO trigger fires.
3. `soft_i2c_temp_wakeup`: Bit-bang software I2C on two LP IO pins, read an SHT4X sensor at address `0x44`, and wake when temperature or humidity leaves a configured range.

---

## Installation

### Arduino IDE

1. Install via the Arduino Library Manager (ESP32P4_ULP by Xorlent) or using the Download or clone this repository.
2. If downloading/cloning the repository:
    - **Sketch -> Include Library -> Add .ZIP Library...** and select a `.zip` archive of this repository.
    - Copy the repository folder into your Arduino `libraries` directory.
3. Select board: **ESP32P4 Dev Module** (or any ESP32-P4 board in Arduino-esp32 3.x or later).
4. Select "Tools" and ensure USB CDC On Boot is "Enabled," Chip Variant is "Before v.3.00," and USB Mode is "Hardware CDC and JTAG"

No extra LP build step is required for Arduino use. The LP binaries are already embedded under `src/programs/`.

---

## Hardware: LP IO Pins

The ESP32-P4 has **16 LP IO pins** (`LP_IO_0` through `LP_IO_15`). These map 1:1 to GPIO0-GPIO15.

- `wakeOnGPIO()` and `wakeOnInt()` use one LP IO pin as the wake source.
- `wakeOnSoftwareI2CSHT4x()` uses any two distinct LP IO pins for SDA and SCL.

The wrapper routes the selected pins through the LP IO mux before starting the LP core. GPIO and interrupt wake modes also apply a default internal pull that matches the requested trigger.

```cpp
ULP.wakeOnGPIO(LP_IO_8, HIGH);
```

---

## API Reference

```cpp
#include <ESP32P4_ULP.h>
```

### wakeOnGPIO()

```cpp
bool ULP.wakeOnGPIO(uint8_t lp_gpio_num, uint8_t wake_level,
                    uint32_t period = 1, uint32_t debounce = 0);
```

Load the `gpio_wakeup` LP program and arm it. The LP core polls the specified pin every `period` milliseconds. When the level matches `wake_level` (confirmed for `debounce` consecutive sample cycles if non-zero), the LP core wakes the HP core from deep sleep.  This provides more flexibility than implementing an interrupt-only GPIO feature.

The wrapper also routes the pin through the RTC/LP mux and applies a default internal pull that matches `wake_level`.

| Parameter | Description |
|---|---|
| `lp_gpio_num` | LP IO pin number (0-15); use `LP_IO_0` through `LP_IO_15` |
| `wake_level` | `HIGH` or `LOW` |
| `period` | LP sample period in milliseconds (default: 1) |
| `debounce` | Consecutive matching sample cycles required (0 = immediate) |

**Returns:** `true` on success; `false` if the binary is a placeholder or load fails.

### wakeOnInt()

```cpp
bool ULP.wakeOnInt(uint8_t lp_gpio_num, uint8_t wake_level);
```

Load the `int_wakeup` LP program and arm LP IO interrupt forwarding to the HP core. The HP side routes the selected GPIO through the LP mux, applies a matching default pull for rising/high or falling/low triggers, and passes the target pin and trigger mode to LP shared memory. The LP program then enables LP GPIO wake/interrupt handling on that pin, waits for the LP IO interrupt, and wakes the HP core when it fires. `LP_GPIO_INT_ANY_EDGE` leaves both pulls disabled.

| Parameter | Description |
|---|---|
| `lp_gpio_num` | LP IO pin number (0-15); use `LP_IO_0` through `LP_IO_15` |
| `wake_level` | Trigger type passed to the LP program: `LP_GPIO_INT_LOW_TO_HIGH` (1), `LP_GPIO_INT_HIGH_TO_LOW` (2), `LP_GPIO_INT_ANY_EDGE` (3), `LP_GPIO_INT_LOW_LEVEL` (4), or `LP_GPIO_INT_HIGH_LEVEL` (5) |

Call `ULP.clearWakeupPending()` before `esp_deep_sleep_start()` to clear any stale LP-to-HP wake request from a previous cycle.

**Returns:** `true` on success; `false` if the pin or trigger is invalid or the LP binary cannot be loaded.

### wakeOnSoftwareI2CSHT4x()

> [!IMPORTANT]
> The software-I2C requires **external pullups** on SDA and SCL.

```text
             3.3V
              |
          +---+---+
          |       |
        10kOhm  10kOhm
          |       |
    SDA wire to  SCL wire to
    ESP32-P4    ESP32-P4
          |       |
    LP_IO_x /    LP_IO_y /
    sensor SDA   sensor SCL
```

```cpp
bool ULP.wakeOnSoftwareI2CSHT4x(uint8_t sda_lp_gpio_num,
                                uint8_t scl_lp_gpio_num,
                                int16_t low_limit_c_deg,
                                int16_t high_limit_c_deg,
                                uint32_t period_ms = 300000,
                                int16_t low_limit_c_hum = 1,
                                int16_t high_limit_c_hum = 0);
```

`wakeOnSoftwareI2CTemperature()`, remains available as a backward-compatible wrapper.

Loads the `soft_i2c_temp_wakeup` LP program and arm timer-based polling for an SHT4X temperature sensor on I2C address `0x44` using any available LP GPIO pins. The LP core bit-bangs a measurement command (`0xFD`), waits for the conversion to complete, reads the 6-byte result, validates both CRC bytes, stores the latest raw temperature and humidity samples in shared memory, and wakes the HP core when either enabled threshold range is violated. The wrapper configures the selected SDA/SCL pins for LP ownership, expects external pullups on both lines, packs the temperature thresholds into LP shared memory as raw SHT4X values, converts humidity thresholds from centi-percent RH into raw SHT4X values, and disables humidity wake by default when `low_limit_c_hum > high_limit_c_hum`.

| Parameter | Description |
|---|---|
| `sda_lp_gpio_num` | SDA LP IO pin number (0-15); use `LP_IO_0` through `LP_IO_15` |
| `scl_lp_gpio_num` | SCL LP IO pin number (0-15); use `LP_IO_0` through `LP_IO_15` |
| `low_limit_c_deg` | Lower temperature threshold in centi-degrees C |
| `high_limit_c_deg` | Upper temperature threshold in centi-degrees C |
| `period_ms` | LP polling period in milliseconds (minimum: 50) |
| `low_limit_c_hum` | Optional lower humidity threshold in centi-percent RH; humidity wake disabled when this is greater than `high_limit_c_hum` |
| `high_limit_c_hum` | Optional upper humidity threshold in centi-percent RH |

**Returns:** `true` on success; `false` if arguments are invalid, `period_ms` is below 50 ms, or the LP binary cannot be loaded.

### clearWakeupPending()

```cpp
void ULP.clearWakeupPending(void);
```

Clear the stale LP-to-HP wake request. Both `wakeOnGPIO()` and `wakeOnInt()` use the LP core to wake the HP core through the PMU software wake path, so this should be called before `esp_deep_sleep_start()` to clear both the PMU wake latch and the shared-memory `ULP_STATUS_WAKEUP_PENDING` flag from a previous cycle.

### wokeFromULP()

```cpp
bool ULP.wokeFromULP(void);
```

Return `true` if the current boot was caused by an LP wake handled by this library. This works around Arduino-esp32 not always reporting `ESP_SLEEP_WAKEUP_ULP` for ESP32-P4 LP-core wakes and falls back to the shared LP state after an HP watchdog reboot.

### Other Methods

| Method | Description |
|---|---|
| `void stop()` | Stop the LP core and disable all wakeup sources |
| `bool wokeFromULP()` | Detect an LP wake using the normal wake cause or the Arduino watchdog-reset fallback |
| `bool isRunning()` | `true` if the LP program has set `ULP_STATUS_RUNNING` |
| `uint32_t getData(uint8_t index)` | Read `data[0..7]` written by the LP core |
| `ulp_shared_mem_t *sharedMem()` | Direct pointer to the shared memory struct |

### Low-Level Headers

If you need direct access to the LP loader or shared-memory definitions from an Arduino sketch, these headers are public:

- `src/ulp_hal/ulp_hal.h`
- `src/programs/ulp_shared.h`
- `src/programs/ulp_programs.h`

---

## Examples

### WakeOnGPIO

[examples/WakeOnGPIO/WakeOnGPIO.ino](examples/WakeOnGPIO/WakeOnGPIO.ino)

Enters deep sleep and wakes when GPIO8 goes HIGH. Connect a button between GPIO8 and 3.3V.

### WakeOnInt

[examples/WakeOnInt/WakeOnInt.ino](examples/WakeOnInt/WakeOnInt.ino)

Enters deep sleep and wakes when GPIO9 transitions HIGH using the `int_wakeup` LP program, which waits for an LP IO interrupt and then wakes the HP core.

### WakeOnI2CTemp

[examples/WakeOnI2CTemp/WakeOnI2CTemp.ino](examples/WakeOnI2CTemp/WakeOnI2CTemp.ino)

Uses LP_IO_8 as SDA and LP_IO_9 as SCL to monitor an SHT4X sensor and wake the HP core when the measured temperature falls outside the configured range. The example defaults to a narrow `0.00 C` to `10.00 C` window so it will usually wake after one poll in a normal room-temperature environment.

### WakeOnI2CTemp_Hum

[examples/WakeOnI2CTemp_Hum/WakeOnI2CTemp_Hum.ino](examples/WakeOnI2CTemp_Hum/WakeOnI2CTemp_Hum.ino)

Uses LP_IO_8 as SDA and LP_IO_9 as SCL to monitor an SHT4X sensor and wake the HP core when either the measured temperature or humidity falls outside the configured range. The example defaults to `10.00 C` to `30.00 C` and `20.00 %RH` to `75.00 %RH`.

---

## Shared Memory Layout

The `ulp_shared_mem_t` struct is 64 bytes, mapped at offset 32 in LP SRAM (`0x50108020`).

```
Offset  Field          Written by  Description
------  -------------  ----------  -------------------------------
0x00    magic          HP          0x554C5000 ("ULP\0")
0x04    program_id     HP          Loaded LP program ID (GPIO_WAKEUP, INT_WAKEUP, SOFT_I2C_TEMP_WAKEUP)
0x08    config0        HP          Target LP IO number or SDA LP IO number
0x0C    config1        HP          Program-specific trigger config or SCL LP IO number
0x10    config2        HP          Program-specific extra config / packed raw temperature thresholds
0x14    config3        HP          Program-specific extra config / packed raw humidity thresholds
0x18    status         LP          ULP_STATUS_* bitmask
0x1C    lp_counter     LP          Incremented each LP cycle
0x20    data[0]        LP          Last sampled GPIO level / raw SHT4X temperature
0x24    data[1]        LP          LP status / software-I2C status code
0x28    data[2]        LP          Raw SHT4X humidity sample
0x2C    data[3..7]     LP          Available for future use
```

For `soft_i2c_temp_wakeup`, `config2` stores the low and high raw SHT4X temperature thresholds packed into a single `uint32_t`, and `data[1]` reports the last protocol status (`0 = OK`, `1 = bus not idle`, `2 = command address NACK`, `3 = command byte NACK`, `4 = read address NACK`, `5 = temperature CRC fail`, `6 = humidity CRC fail`, `7 = STOP failed / bus stuck low`).

---

## Guidance

- After programming new firmware, fully re-power the device to boot the new code.
- If the device is already in deep sleep, enter DFU mode to flash (usually holding a reset button for 5-6 seconds).
- **ESP32-P4 only.** Other ESP32 variants use different ULP types and are not supported.
- **One LP program at a time.** Only one program can be loaded into LP SRAM.
- **LP IO only.** The LP core can only access LP IO pins (GPIO0-GPIO15).
- **Software-I2C support is currently SHT4X-specific.** The bundled LP program talks to address `0x44` and validates the returned CRC bytes.
- **Arduino users consume pre-built LP binaries.** The `lp_core/` directory is for maintaining or rebuilding those binaries outside the normal Arduino workflow.
- This library is designed for ESP32-P4 revisions before 3.00.
- Likely due to the lack of ULP support in Arduino, the device will show WDT resets on wake occasionally.  This library will still correctly identify a wake from deep sleep.

## Reference Material

- The source code used to generate the binaries embedded in this library can be seen at [https://github.com/Xorlent/ESP32P4_ULP-LP_Core](https://github.com/Xorlent/ESP32P4_ULP-LP_Core)
