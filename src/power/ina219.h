// src/power/ina219.h
#pragma once
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <iostream>
#include <thread>
#include <chrono>

enum class Register : uint8_t
{
    CONFIG = 0x00,
    SHUNT_VOLTAGE = 0x01,
    BUS_VOLTAGE = 0x02,
    POWER = 0x03,
    CURRENT = 0x04,
    CALIBRATION = 0x05,
};

enum class BusVoltageRange : uint8_t
{
    RANGE_16V = 0x00,
    RANGE_32V = 0x01,
};

enum class Gain : uint8_t
{
    DIV_1_40MV = 0x00,
    DIV_2_80MV = 0x01,
    DIV_4_160MV = 0x02,
    DIV_8_320MV = 0x03,
};

enum class ADCResolution : uint8_t
{
    ADCRES_9BIT_1S = 0x00,    // 9 bit,   1 sample,  84us
    ADCRES_10BIT_1S = 0x01,   // 10 bit,  1 sample,  148us
    ADCRES_11BIT_1S = 0x02,   // 11 bit,  1 sample,  276us
    ADCRES_12BIT_1S = 0x03,   // 12 bit,  1 sample,  532us
    ADCRES_12BIT_2S = 0x09,   // 12 bit,  2 samples,  1.06ms
    ADCRES_12BIT_4S = 0x0A,   // 12 bit,  4 samples,  2.13ms
    ADCRES_12BIT_8S = 0x0B,   // 12bit,   8 samples,  4.26ms
    ADCRES_12BIT_16S = 0x0C,  // 12bit,  16 samples,  8.51ms
    ADCRES_12BIT_32S = 0x0D,  // 12bit,  32 samples, 17.02ms
    ADCRES_12BIT_64S = 0x0E,  // 12bit,  64 samples, 34.05ms
    ADCRES_12BIT_128S = 0x0F, // 12bit, 128 samples, 68.10ms
};

enum class OperatingMode : uint8_t
{
    POWER_DOWN = 0x00,
    SHUNT_VOLTAGE_TRIGGERED = 0x01,
    BUS_VOLTAGE_TRIGGERED = 0x02,
    SHUNT_AND_BUS_TRIGGERED = 0x03,
    ADC_OFF = 0x04,
    SHUNT_VOLTAGE_CONTINUOUS = 0x05,
    BUS_VOLTAGE_CONTINUOUS = 0x06,
    SHUNT_AND_BUS_CONTINUOUS = 0x07,
};

class INA219
{

public:
    explicit INA219(const std::string &i2c_dev = "/dev/i2c-1",
                    uint8_t address = 0x43);
    ~INA219();

    bool initialize();

    float read_bus_voltage_v();    // battery terminal voltage in volts
    float read_shunt_voltage_mv(); // shunt voltage in millivolts
    float read_current_ma();       // current in mA (negative = discharging)
    float read_power_mw();         // power in mW

    // Derived values
    int read_percentage(); // 0-100 (based on voltage curve)
    bool is_charging();    // current > 0

    // Shunt resistor on your board (check schematic)
    static constexpr float R_SHUNT_OHM = 0.01f;

    // Calibration — doubled for ×2 resolution gain
    static constexpr uint16_t MAX_CURR_BEFORE_OVERFLOW = 32767; // Max current before overflow 3.2767A at 10mΩ shunt

    // Factor to calculate power LSB from current LSB (20 for ×2 gain)
    static constexpr float POWER_LSB_FACTOR = 20.0f;

    // Max expected current — 5 A expressed in mA for consistent units with current_lsb_ma_
    static constexpr float MAX_EXPECTED_CURRENT_MA = 5000.0f; // 5 A in mA

    // ADAC scaling based constant
    static constexpr float ADAC_SCALING_CONSTANT = 0.04096f;

    // Shunt voltage register LSB weight: 10 µV/bit = 0.01 mV/bit (INA219 datasheet)
    static constexpr float SHUNT_VOLTAGE_LSB_MV = 0.01f;

    static constexpr float    BUS_VOLTAGE_LSB_V      = 0.004f;  // 4 mV/LSB (INA219 datasheet)
    static constexpr int      BUS_VOLTAGE_DATA_SHIFT = 3;      // bits 2:0 are status flags

    // CONFIG register value after power-on or soft reset (INA219 datasheet §8.6.1)
    // BRNG=1(32V) PGA=11(÷8,320mV) BADC=0011(12bit) SADC=0011(12bit) MODE=111(continuous)
    static constexpr uint16_t CONFIG_RESET_VALUE     = 0x399F;

    // RST bit (bit 15) in CONFIG triggers a soft reset; self-clears after reset completes
    static constexpr uint16_t CONFIG_RST_BIT         = 0x8000;

private:
    int fd_; // I2C file descriptor
    uint8_t addr_;
    uint16_t read_register(Register reg);
    float current_lsb_ma_;       // Current LSB in mA
    float power_lsb_mw_;         // Power LSB in mW
    uint16_t calibration_value_; // Calibration register value (integer)

    void write_register(Register reg, uint16_t value);
    void calibrate();
    void configure(BusVoltageRange bus_range, Gain gain, ADCResolution bus_adc, ADCResolution shunt_adc, OperatingMode mode);
};
