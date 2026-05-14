
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <numeric>
#include <atomic>
#include <sys/utsname.h>

int main(int argc, char *argv[])
{
    std::cout << "=== Pi Zero 2 W Cross-Compiled App ===\n\n";

    // Print system info
    struct utsname info;
    uname(&info);
    std::cout << "Running on:  " << info.nodename << "\n";
    std::cout << "Kernel:      " << info.release << "\n";
    std::cout << "Machine:     " << info.machine << "\n";
    std::cout << "Compiled at: " << __DATE__
              << " " << __TIME__ << "\n\n";

    // Check for libcamera
#ifdef HAVE_LIBCAMERA
    std::cout << "libcamera support: ENABLED\n";
#else
    std::cout << "libcamera support: NOT FOUND\n";
#endif

    std::cout << "-------------------------------------\n";

    // Simulate some work (e.g., camera initialization)
    std::cout << "Initializing camera...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "Camera initialized successfully!\n";

    // Simulate main loop
    for (int i = 0; i < 5; ++i)
    {
        std::cout << "Capturing frame " << (i + 1) << "...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Frame " << (i + 1) << " captured!\n";
    }

    std::cout << "\nExiting application.\n";
    return 0;
}
