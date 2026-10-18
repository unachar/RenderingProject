#include "virtualgeometry/virtualgeometrypageio.h"
#include "virtualgeometry/virtualgeometryserializer.h"
#include <fstream>
#include <latch>
#include <stdexcept>

void TestVirtualGeometryPageIo()
{
    using namespace VirtualGeometryStreaming;
    auto manifest = std::make_shared<VirtualGeometryPageCache::PageManifest>();
    manifest->Path = std::filesystem::current_path() / "vg_io_test.pages";
    std::vector<uint8_t> expected(65536, 42);
    manifest->FileBytes = 65536;
    manifest->Pages.push_back({0, 65536, VirtualGeometrySerializer::Hash(expected)});
    {
        std::ofstream file(manifest->Path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(expected.data()), expected.size());
    }
    PageIo io;
    if (!io.Submit(manifest, {0, 0, 1}, 0))
    {
        throw std::runtime_error("SSD read submission rejected");
    }
    std::vector<PageIoCompletion> results;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (results.empty() && std::chrono::steady_clock::now() < deadline)
    {
        results = io.Poll();
        std::this_thread::yield();
    }
    if (results.size() != 1 || !results[0].Error.empty() || results[0].Payload != expected ||
        results[0].ReadBytes != 65536)
    {
        throw std::runtime_error("SSD Page bytes/read count mismatch");
    }
    io.Shutdown();
    std::filesystem::remove(manifest->Path);

    std::latch started(4);
    std::latch release(1);
    PageIo delayed([&](const auto&, uint32_t, std::span<uint8_t> bytes, std::string&) {
        started.count_down();
        release.wait();
        std::fill(bytes.begin(), bytes.end(), 7);
        return true;
    });
    for (uint32_t page = 0; page < 4; ++page)
    {
        if (!delayed.Submit(manifest, {page, page, 1}, 0))
        {
            release.count_down();
            throw std::runtime_error("Concurrent read submission failed");
        }
    }
    started.wait();
    bool pollWasEmpty = delayed.Poll().empty();
    delayed.Cancel({0, 0, 1});
    release.count_down();
    delayed.Shutdown();
    delayed.Cancel({1, 1, 1});
    results = delayed.Poll();
    if (!pollWasEmpty || results.size() != 4 || delayed.StagingBytes() != 0)
    {
        throw std::runtime_error("Nonblocking poll or shutdown staging ownership failed");
    }
    bool cancelled = false;
    bool completedCancellation = false;
    for (const auto& result : results)
    {
        if (result.Ticket.Page == 0)
        {
            cancelled = !result.Error.empty() && result.Payload.empty();
        }
        if (result.Ticket.Page == 1)
        {
            completedCancellation = !result.Error.empty() && result.Payload.empty();
        }
    }
    if (!cancelled)
    {
        throw std::runtime_error("Cancelled delayed I/O result was published");
    }
    if (!completedCancellation)
    {
        throw std::runtime_error("Cancellation between I/O completion and CPU collection was ignored");
    }
}
