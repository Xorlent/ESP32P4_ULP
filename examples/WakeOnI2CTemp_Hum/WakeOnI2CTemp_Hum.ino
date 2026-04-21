/**
 * WakeOnI2CTemp_Hum.ino
 *
 * Put the ESP32-P4 into deep sleep and wake when an SHT4X temperature sample
 * falls outside the configured range.
 *
 * Wiring:
 *   - SDA: GPIO8 / LP_IO_8
 *   - SCL: GPIO9 / LP_IO_9
 *   - SHT4X address: 0x44
 *   - External pullups required on SDA and SCL
 */

#include <Arduino.h>
#include <Wire.h>
#include "ESP32P4_ULP.h"

static constexpr uint8_t SDA_LP_IO = LP_IO_8;
static constexpr uint8_t SCL_LP_IO = LP_IO_9;
static constexpr int16_t LOW_LIMIT_C_DEG = 1000;       // 10.00 C
static constexpr int16_t HIGH_LIMIT_C_DEG = 3000;      // 30.00 C
static constexpr int16_t LOW_LIMIT_C_HUM = 2000;       // 20.00 %RH
static constexpr int16_t HIGH_LIMIT_C_HUM = 7500;      // 75.00 %RH
static constexpr uint32_t POLL_PERIOD_MS = 5000;
static constexpr uint8_t SHT4X_I2C_ADDR = 0x44;
static constexpr uint8_t SHT4X_CMD_MEASURE_HIGH_REPEAT = 0xFD;

static const char *statusText(uint32_t status)
{
    switch (status) {
        case 0: return "OK";
        case 1: return "bus not idle at START";
        case 2: return "write address NACK";
        case 3: return "measurement command NACK";
        case 4: return "read address NACK";
        case 5: return "temperature CRC fail";
        case 6: return "humidity CRC fail";
        case 7: return "STOP failed / bus stuck";
        default: return "unknown";
    }
}

static float rawTemperatureToC(uint16_t rawTemperature)
{
    constexpr float scale = 175.0f / 65535.0f;
    constexpr float offset = 45.0f;
    return rawTemperature * scale - offset;
}

static float rawHumidityToPercent(uint16_t rawHumidity)
{
    constexpr float scale = 125.0f / 65535.0f;
    constexpr float offset = 6.0f;
    return rawHumidity * scale - offset;
}

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFFu;
    for (size_t index = 0; index < len; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((crc & 0x80u) != 0) {
                crc = (uint8_t)((crc << 1) ^ 0x31u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static bool probeSHT4XAwake(void)
{
    Wire.beginTransmission(SHT4X_I2C_ADDR);
    return Wire.endTransmission() == 0;
}

static bool readSHT4XAwake(uint16_t &rawTemperature, uint16_t &rawHumidity)
{
    Wire.beginTransmission(SHT4X_I2C_ADDR);
    Wire.write(SHT4X_CMD_MEASURE_HIGH_REPEAT);
    if (Wire.endTransmission() != 0) {
        return false;
    }

    delay(10);

    const uint8_t bytesRequested = 6;
    if (Wire.requestFrom((int)SHT4X_I2C_ADDR, (int)bytesRequested) != bytesRequested) {
        return false;
    }

    uint8_t buffer[6];
    for (uint8_t index = 0; index < bytesRequested; ++index) {
        if (!Wire.available()) {
            return false;
        }
        buffer[index] = (uint8_t)Wire.read();
    }

    if ((crc8(buffer, 2) != buffer[2]) || (crc8(&buffer[3], 2) != buffer[5])) {
        return false;
    }

    rawTemperature = (uint16_t)(((uint16_t)buffer[0] << 8) | buffer[1]);
    rawHumidity = (uint16_t)(((uint16_t)buffer[3] << 8) | buffer[4]);
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    const bool wokeFromULP = ULP.wokeFromULP();
    if (wokeFromULP) {
        ULP.stop();
    }
    else {
        Wire.begin(SDA_LP_IO, SCL_LP_IO);
        Wire.setClock(100000);

        Serial.printf("HP awake-mode probe on SDA=%u, SCL=%u -> %s\n",
                    SDA_LP_IO,
                    SCL_LP_IO,
                    probeSHT4XAwake() ? "ACK from 0x44" : "no ACK from 0x44");

        uint16_t hpRawTemperature = 0;
        uint16_t hpRawHumidity = 0;
        if (readSHT4XAwake(hpRawTemperature, hpRawHumidity)) {
            Serial.printf("HP awake-mode read OK: temp raw=%u (%.2f C), humidity raw=%u (%.2f %%RH)\n",
                        hpRawTemperature,
                        rawTemperatureToC(hpRawTemperature),
                        hpRawHumidity,
                        rawHumidityToPercent(hpRawHumidity));
        } else {
            Serial.println("HP awake-mode read failed");
        }
    }

    if (wokeFromULP) {
        const uint16_t rawTemperature = (uint16_t)(ULP.getData(0) & 0xFFFFu);
        const uint16_t rawHumidity = (uint16_t)(ULP.getData(2) & 0xFFFFu);
        const uint32_t status = ULP.getData(1);

        Serial.printf("Woke from LP software-I2C temperature monitor - poll count: %lu, status: %lu (%s)\n",
                      (uint32_t)ULP.sharedMem()->lp_counter,
                      status,
                      statusText(status));
        if (status == 0) {
            Serial.printf("Temperature raw: %u, %.2f C\n",
                          rawTemperature,
                          rawTemperatureToC(rawTemperature));
            Serial.printf("Humidity raw: %u, %.2f %%RH\n",
                          rawHumidity,
                          rawHumidityToPercent(rawHumidity));
        } else {
            Serial.println("LP SHT4X read failed; no threshold wake should occur until a valid sample is captured.");
        }
    } else {
        Serial.println("First boot...");
    }

    Serial.println("This example wakes when the measured SHT4X temperature or humidity is outside the configured range.");
    Serial.println("Adjust LOW_LIMIT and HIGH_LIMIT values for your application.");
    Serial.print("Enter deep sleep? (ENTER to sleep): ");
    Serial.flush();

    while (true) {
        if (Serial.available()) {
            String response = Serial.readStringUntil('\n');
            response.trim();
            if (response.length() == 0) {
                Serial.println("Y");
                break;
            }
        }
        delay(10);
    }

    if (!ULP.wakeOnSoftwareI2CSHT4x(SDA_LP_IO,
                                    SCL_LP_IO,
                                    LOW_LIMIT_C_DEG,
                                    HIGH_LIMIT_C_DEG,
                                    POLL_PERIOD_MS,
                                    LOW_LIMIT_C_HUM,
                                    HIGH_LIMIT_C_HUM)) {
        Serial.println("Failed to start the LP software-I2C temperature/humidity program");
        while (true) {
            delay(1000);
        }
    }

    Serial.printf("SHT4X monitor armed on SDA=%u, SCL=%u, period=%lu ms\n",
                  SDA_LP_IO,
                  SCL_LP_IO,
                  POLL_PERIOD_MS);
    Serial.printf("Wake when temperature is outside %.2f C to %.2f C\n",
                  LOW_LIMIT_C_DEG / 100.0f,
                  HIGH_LIMIT_C_DEG / 100.0f);
    Serial.printf("Wake when humidity is outside %.2f %%RH to %.2f %%RH\n",
                  LOW_LIMIT_C_HUM / 100.0f,
                  HIGH_LIMIT_C_HUM / 100.0f);
    Serial.println("Entering deep sleep.");
    Serial.flush();

    while (Serial.available()) {
        Serial.read();
    }

    ULP.clearWakeupPending();
    esp_deep_sleep_start();
}

void loop()
{
}
