#include "../External/tracy/public/tracy/Tracy.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    constexpr int iterations = 100000;
    volatile unsigned int checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < iterations; ++frame)
    {
        ZoneScopedN("CPU trace probe");
        checksum += static_cast<unsigned int>(frame);
    }
    const double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::thread worker([] {
        ZoneScopedN("Worker trace probe");
    });
    worker.join();
    FrameMark;
#ifdef TRACY_ENABLE
    std::cout << "enabled=1 connected=" << TracyIsConnected;
#else
    std::cout << "enabled=0 connected=0";
#endif
    std::cout << " scopes=" << iterations << " cpuMs=" << milliseconds << " checksum=" << checksum << '\n';
    return 0;
}
