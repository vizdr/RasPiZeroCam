#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <sys/utsname.h>
#include "power/ina219.h"

int main(int argc, char *argv[])
{
    std::cout << std::unitbuf; // flush after every output operation
    std::cerr << std::unitbuf;

    std::cout << "=== Pi Zero 2 W — INA219 Power Monitor ===\n\n";

    struct utsname info;
    uname(&info);
    std::cout << "Running on:  " << info.nodename << "\n";
    std::cout << "Kernel:      " << info.release << "\n";
    std::cout << "Machine:     " << info.machine << "\n";
    std::cout << "Compiled at: " << __DATE__ << " " << __TIME__ << "\n\n";

    // Construct locally so constructor failure can be handled in main()
    INA219 sensor("/dev/i2c-1", 0x43);

    if (!sensor.initialize())
    {
        std::cerr << "INA219 initialization failed\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(3);

    // Continuous monitoring — matches Python __main__ loop
    while (true)
    {
        float bus_v = sensor.read_bus_voltage_v();
        float shunt_v = sensor.read_shunt_voltage_mv() / 1000.0f; // mV → V
        float current = sensor.read_current_ma();
        float power = sensor.read_power_mw() / 1000.0f; // mW → W
        int pct = sensor.read_percentage();

        // PSU voltage = bus voltage + shunt voltage (load is on bus side)
        std::cout << "Load Voltage:  " << std::setw(6) << bus_v << " V\n";
        std::cout << "Current:       " << std::setw(6) << current / 1000.0f << " A\n";
        std::cout << "Power:         " << std::setw(6) << power << " W\n";
        std::cout << "Percent:       " << std::setprecision(1)
                  << std::setw(4) << pct << "%\n";
        std::cout << "Shunt Voltage: " << std::setw(6) << shunt_v << " V    \n";

        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    return 0;
}
