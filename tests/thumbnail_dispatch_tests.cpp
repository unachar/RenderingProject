// Exercise the actual UI scheduling branch with a deliberately stalled GPU-preparation worker.
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

enum class AssetType
{
    Material,
    Texture,
    Mesh
};

struct ThumbnailPrepared
{
    std::string Error;
    bool Cached = false;
    AssetType Type = AssetType::Mesh;
    std::shared_ptr<int> PreviewDescriptor;
    std::shared_ptr<int> Preview;
};

struct EditorDescriptors
{
    static std::shared_ptr<int> Allocate()
    {
        return std::make_shared<int>(42);
    }
};

std::promise<void> workerStarted;
std::promise<void> releaseWorker;
std::atomic<unsigned> preparations = 0;
const auto uiThread = std::this_thread::get_id();

struct ThumbnailRenderer
{
    static ThumbnailPrepared PreparePreview(ThumbnailPrepared input)
    {
        assert(std::this_thread::get_id() != uiThread);
        ++preparations;
        workerStarted.set_value();
        releaseWorker.get_future().wait();
        input.Preview = std::make_shared<int>(7);
        return input;
    }
};

std::optional<ThumbnailPrepared> prepared;
std::future<ThumbnailPrepared> preparation;

void Dispatch()
{
#include "preview-dispatch.inc"
}

int main()
{
    prepared.emplace();
    prepared->Type = AssetType::Texture;
    Dispatch();
    assert(prepared && !preparation.valid());
    prepared->Type = AssetType::Material;
    prepared->Cached = true;
    Dispatch();
    assert(prepared && !preparation.valid());
    prepared->Cached = false;
    prepared->Error = "invalid asset";
    Dispatch();
    assert(prepared && !preparation.valid());
    prepared->Error.clear();
    prepared->Type = AssetType::Mesh;
    const auto start = std::chrono::steady_clock::now();
    Dispatch();
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    assert(!prepared && preparation.valid());
    assert(workerStarted.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    assert(preparation.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    // UI code can run while shader/PSO preparation is still blocked.
    unsigned uiFrames = 0;
    for (; uiFrames < 100; ++uiFrames)
    {
        assert(preparation.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    }
    releaseWorker.set_value();
    assert(preparation.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    prepared = preparation.get();
    assert(prepared->Preview && prepared->PreviewDescriptor);
    Dispatch();
    assert(preparations == 1 && !preparation.valid());
    std::cout
        << "Thumbnail dispatch: UI progresses during blocked preparation; result reused; texture/cache/error bypass PASS\n";
}
