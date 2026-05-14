#include "ina219.h"
#include <cmath>

INA219::INA219(const std::string &i2c_dev, uint8_t i2cAddress) : fd_(-1),
                                                                 addr_(i2cAddress),
                                                                 current_lsb_ma_(0.0f),
                                                                 power_lsb_mw_(0.0f),
                                                                 calibration_value_(0)
{
    // Open I2C device
    fd_ = open(i2c_dev.c_str(), O_RDWR);
    if (fd_ < 0)
    {
        std::cerr << "Error opening I2C device: " << i2c_dev << "\n";
        return;
    }

    // Set I2C slave address
    if (ioctl(fd_, I2C_SLAVE, addr_) < 0)
    {
        std::cerr << "Error setting I2C address: 0x" << std::hex << static_cast<int>(addr_) << "\n";
        close(fd_);
        fd_ = -1;
        return;
    }

    // Calibrate the sensor
    calibrate();
    // Configure the sensor (example configuration)
    configure(BusVoltageRange::RANGE_16V, Gain::DIV_2_80MV, ADCResolution::ADCRES_12BIT_32S, ADCResolution::ADCRES_12BIT_32S, OperatingMode::SHUNT_AND_BUS_CONTINUOUS);
}

INA219::~INA219() {}

bool INA219::initialize()
{
    // Step 1 — verify I2C was opened successfully in constructor
    if (fd_ < 0)
    {
        std::cerr << "INA219::initialize: no I2C file descriptor\n";
        return false;
    }

    // Step 2 — soft reset: write RST bit to CONFIG
    //   The chip resets all registers to power-on defaults; RST self-clears.
    write_register(Register::CONFIG, CONFIG_RST_BIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Step 3 — verify chip is alive: CONFIG must equal the datasheet reset value
    uint16_t config = read_register(Register::CONFIG);
    if (config != CONFIG_RESET_VALUE)
    {
        std::cerr << "INA219::initialize: unexpected CONFIG after reset: 0x"
                  << std::hex << config << " (expected 0x" << CONFIG_RESET_VALUE << ")\n";
        return false;
    }

    // Step 4 — re-apply calibration and configuration (reset cleared them)
    calibrate();
    configure(BusVoltageRange::RANGE_16V,
              Gain::DIV_2_80MV,
              ADCResolution::ADCRES_12BIT_32S,
              ADCResolution::ADCRES_12BIT_32S,
              OperatingMode::SHUNT_AND_BUS_CONTINUOUS);

    // Step 5 — verify CALIBRATION register was written and read back correctly
    //   Proves both write and read paths are functional end-to-end.
    uint16_t cal = read_register(Register::CALIBRATION);
    if (cal != calibration_value_)
    {
        std::cerr << "INA219::initialize: CALIBRATION readback mismatch: wrote 0x"
                  << std::hex << calibration_value_ << " read 0x" << cal << "\n";
        return false;
    }

    return true;
}

bool INA219::is_charging()
{
    return read_current_ma() > 0.0f;
}

int INA219::read_percentage()
{
    // Linear LiPo approximation: 3.0 V = 0%, 4.2 V = 100%
    // Matches Python: p = (bus_voltage - 3) / 1.2 * 100
    float v = read_bus_voltage_v();
    int p = static_cast<int>((v - 3.0f) / 1.2f * 100.0f);
    if (p > 100)
        p = 100;
    if (p < 0)
        p = 0;
    return p;
}
float INA219::read_bus_voltage_v()
{
    write_register(Register::CALIBRATION, calibration_value_); // Ensure calibration is set before reading
    uint16_t raw = read_register(Register::BUS_VOLTAGE);
    // Each bit = 4mV, shift right 3 to discard status bits
    return ((raw >> BUS_VOLTAGE_DATA_SHIFT) * BUS_VOLTAGE_LSB_V); // Convert to volts
}
float INA219::read_current_ma()
{
    uint16_t raw = read_register(Register::CURRENT);
    return static_cast<int16_t>(raw) * current_lsb_ma_;
}
float INA219::read_power_mw()
{
    write_register(Register::CALIBRATION, calibration_value_); // Ensure calibration is set before reading power
    uint16_t raw = read_register(Register::POWER);
    return static_cast<int16_t>(raw) * power_lsb_mw_;
}
float INA219::read_shunt_voltage_mv()
{
    write_register(Register::CALIBRATION, calibration_value_); // Ensure calibration is set before reading shunt voltage
    uint16_t raw = read_register(Register::SHUNT_VOLTAGE);
    // SHUNT_VOLTAGE_LSB_MV = 0.01 mV/bit (10 µV/bit per INA219 datasheet)
    // Note: R_SHUNT_OHM is numerically equal but is the wrong constant to use here
    return static_cast<int16_t>(raw) * SHUNT_VOLTAGE_LSB_MV;
}

void INA219::calibrate()
{
    // Calibration code would go here
    // current_lsb_ma_ in mA/bit  — matches Python: _current_lsb = 0.1524 mA/bit
    current_lsb_ma_ = MAX_EXPECTED_CURRENT_MA / MAX_CURR_BEFORE_OVERFLOW;

    // power_lsb_mw_ in mW/bit  — matches Python: _power_lsb = 0.003048 W/bit = 3.048 mW/bit
    power_lsb_mw_ = current_lsb_ma_ * POWER_LSB_FACTOR;

    // Cal = 0.04096 / (Current_LSB_A * R_shunt)  — formula requires A/bit, not mA/bit
    calibration_value_ = static_cast<uint16_t>(
        trunc(ADAC_SCALING_CONSTANT / ((current_lsb_ma_ / 1000.0f) * R_SHUNT_OHM)));
    write_register(Register::CALIBRATION, calibration_value_);
}

void INA219::write_register(Register reg, uint16_t value)
{
    // Single transaction: [reg, high_byte, low_byte]
    // Matches Python: bus.write_i2c_block_data(addr, reg, [high, low])
    // INA219 requires all three bytes in one I2C transaction.
    uint8_t buf[3];
    buf[0] = static_cast<uint8_t>(reg);
    buf[1] = (value >> 8) & 0xFF; // high byte first (big-endian)
    buf[2] = value & 0xFF;        // low byte

    if (write(fd_, buf, 3) != 3)
    {
        std::cerr << "write_register failed: reg=0x" << std::hex
                  << static_cast<int>(reg) << "\n";
    }
}

uint16_t INA219::read_register(Register reg)
{
    // Step 1 — write register address to set the read pointer
    // Matches Python: read_i2c_block_data(addr, address, 2)
    // which sends the register byte then reads 2 bytes in one transaction.
    uint8_t reg_byte = static_cast<uint8_t>(reg);
    if (write(fd_, &reg_byte, 1) != 1)
    {
        std::cerr << "read_register: failed to set register pointer: 0x"
                  << std::hex << static_cast<int>(reg_byte) << "\n";
        return 0;
    }

    // Step 2 — read 2 bytes back (big-endian, high byte first)
    uint8_t buf[2];
    if (read(fd_, buf, 2) != 2)
    {
        std::cerr << "read_register: failed to read data from register: 0x"
                  << std::hex << static_cast<int>(reg_byte) << "\n";
        return 0;
    }

    // Matches Python: (data[0] * 256) + data[1]
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

void INA219::configure(BusVoltageRange bus_range, Gain gain, ADCResolution bus_adc, ADCResolution shunt_adc, OperatingMode mode)
{
    uint16_t config =
        (static_cast<uint16_t>(bus_range) << 13) |
        (static_cast<uint16_t>(gain) << 11) |
        (static_cast<uint16_t>(bus_adc) << 7) |
        (static_cast<uint16_t>(shunt_adc) << 3) |
        static_cast<uint16_t>(mode);

    write_register(Register::CONFIG, config);
}
