#include "pch.h"
#include "optixinterop.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include <limits>

#ifdef RENDERINGPROJECT_USE_OPTIX
#pragma comment(lib, "advapi32.lib")
#include <cuda.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

namespace
{
    struct CudaDriver
    {
        HMODULE module = nullptr;
#define CUDA_ENTRY(name) decltype(&name) name = nullptr;
        CUDA_ENTRY(cuInit)
        CUDA_ENTRY(cuDeviceGetCount)
        CUDA_ENTRY(cuDeviceGet)
        CUDA_ENTRY(cuDeviceGetLuid)
        CUDA_ENTRY(cuDevicePrimaryCtxRetain)
        CUDA_ENTRY(cuDevicePrimaryCtxRelease)
        CUDA_ENTRY(cuCtxPushCurrent)
        CUDA_ENTRY(cuCtxPopCurrent)
        CUDA_ENTRY(cuStreamCreate)
        CUDA_ENTRY(cuStreamDestroy)
        CUDA_ENTRY(cuStreamSynchronize)
        CUDA_ENTRY(cuMemAlloc)
        CUDA_ENTRY(cuMemFree)
        CUDA_ENTRY(cuImportExternalMemory)
        CUDA_ENTRY(cuExternalMemoryGetMappedBuffer)
        CUDA_ENTRY(cuDestroyExternalMemory)
        CUDA_ENTRY(cuImportExternalSemaphore)
        CUDA_ENTRY(cuDestroyExternalSemaphore)
        CUDA_ENTRY(cuWaitExternalSemaphoresAsync)
        CUDA_ENTRY(cuSignalExternalSemaphoresAsync)
#undef CUDA_ENTRY

        bool Load()
        {
            module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
            {
                return false;
            }
#define CUDA_LOAD(name, symbol)                                                                                        \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(module, symbol));                                           \
    if (!name)                                                                                                         \
    {                                                                                                                  \
        return false;                                                                                                  \
    }
            CUDA_LOAD(cuInit, "cuInit")
            CUDA_LOAD(cuDeviceGetCount, "cuDeviceGetCount")
            CUDA_LOAD(cuDeviceGet, "cuDeviceGet")
            CUDA_LOAD(cuDeviceGetLuid, "cuDeviceGetLuid")
            CUDA_LOAD(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain")
            CUDA_LOAD(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2")
            CUDA_LOAD(cuCtxPushCurrent, "cuCtxPushCurrent_v2")
            CUDA_LOAD(cuCtxPopCurrent, "cuCtxPopCurrent_v2")
            CUDA_LOAD(cuStreamCreate, "cuStreamCreate")
            CUDA_LOAD(cuStreamDestroy, "cuStreamDestroy_v2")
            CUDA_LOAD(cuStreamSynchronize, "cuStreamSynchronize")
            CUDA_LOAD(cuMemAlloc, "cuMemAlloc_v2")
            CUDA_LOAD(cuMemFree, "cuMemFree_v2")
            CUDA_LOAD(cuImportExternalMemory, "cuImportExternalMemory")
            CUDA_LOAD(cuExternalMemoryGetMappedBuffer, "cuExternalMemoryGetMappedBuffer")
            CUDA_LOAD(cuDestroyExternalMemory, "cuDestroyExternalMemory")
            CUDA_LOAD(cuImportExternalSemaphore, "cuImportExternalSemaphore")
            CUDA_LOAD(cuDestroyExternalSemaphore, "cuDestroyExternalSemaphore")
            CUDA_LOAD(cuWaitExternalSemaphoresAsync, "cuWaitExternalSemaphoresAsync")
            CUDA_LOAD(cuSignalExternalSemaphoresAsync, "cuSignalExternalSemaphoresAsync")
#undef CUDA_LOAD
            return true;
        }
    };

    CudaDriver driver;
    ComPtr<ID3D12Device> device;
    CUdevice cudaDevice = 0;
    CUcontext context = nullptr;
    CUstream stream = nullptr;
    OptixDeviceContext optixContext = nullptr;
    void* optixLibrary = nullptr;
    bool available = false;
    std::string status = "OptiX has not been initialized";

    bool Fail(const char* operation, int code)
    {
        status = std::string(operation) + " failed (" + std::to_string(code) + ")";
        WriteGraphicsLog("OptiX: %s\n", status.c_str());
        return false;
    }

    struct ContextScope
    {
        bool active = false;

        ContextScope()
        {
            if (context)
            {
                active = driver.cuCtxPushCurrent(context) == CUDA_SUCCESS;
            }
        }

        ~ContextScope()
        {
            if (active)
            {
                CUcontext previous = nullptr;
                driver.cuCtxPopCurrent(&previous);
            }
        }
    };

    bool Cuda(CUresult result, const char* operation)
    {
        if (result != CUDA_SUCCESS)
        {
            return Fail(operation, static_cast<int>(result));
        }
        return true;
    }

    bool Optix(OptixResult result, const char* operation)
    {
        if (result != OPTIX_SUCCESS)
        {
            return Fail(operation, static_cast<int>(result));
        }
        return true;
    }
} // 名前空間

struct OptixInterop::Impl
{
    struct Slot
    {
        ComPtr<ID3D12Resource> buffer;
        CUexternalMemory memory = nullptr;
        CUdeviceptr pointer = 0;
    };

    static std::vector<Impl*> instances;
    std::vector<Slot> slots;
    ComPtr<ID3D12Fence> fence;
    CUexternalSemaphore semaphore = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT64 fenceValue = 0;
    size_t planeBytes = 0;
    OptixDenoiser denoiser = nullptr;
    CUdeviceptr state = 0;
    CUdeviceptr scratch = 0;
    CUdeviceptr intensity = 0;
    size_t stateBytes = 0;
    size_t scratchBytes = 0;

    Impl()
    {
        instances.push_back(this);
    }

    ~Impl()
    {
        Release();
        instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end());
    }

    void Release()
    {
        if (!context)
        {
            return;
        }
        // リサイズと破棄はまれなので、共有ストレージのマップ解除前に両方の API の処理完了を待つ。
        GraphicsDevice::WaitForGpuIdle();
        ContextScope scope;
        if (!scope.active)
        {
            return;
        }
        driver.cuStreamSynchronize(stream);
        for (Slot& slot : slots)
        {
            if (slot.pointer)
            {
                driver.cuMemFree(slot.pointer);
            }
            if (slot.memory)
            {
                driver.cuDestroyExternalMemory(slot.memory);
            }
        }
        slots.clear();
        if (semaphore)
        {
            driver.cuDestroyExternalSemaphore(semaphore);
            semaphore = nullptr;
        }
        fence.Reset();
        if (denoiser)
        {
            optixDenoiserDestroy(denoiser);
            denoiser = nullptr;
        }
        if (state)
        {
            driver.cuMemFree(state);
            state = 0;
        }
        if (scratch)
        {
            driver.cuMemFree(scratch);
            scratch = 0;
        }
        if (intensity)
        {
            driver.cuMemFree(intensity);
            intensity = 0;
        }
        width = 0;
        height = 0;
        fenceValue = 0;
    }

    bool CreateWorkspace(UINT newWidth, UINT newHeight)
    {
        OptixDenoiserOptions options = {};
        options.guideAlbedo = 1;
        options.guideNormal = 1;
        options.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;
        if (!Optix(optixDenoiserCreate(optixContext, OPTIX_DENOISER_MODEL_KIND_HDR, &options, &denoiser),
                   "create HDR denoiser"))
        {
            return false;
        }
        OptixDenoiserSizes sizes = {};
        if (!Optix(optixDenoiserComputeMemoryResources(denoiser, newWidth, newHeight, &sizes),
                   "query denoiser workspace"))
        {
            return false;
        }
        stateBytes = sizes.stateSizeInBytes;
        scratchBytes = (std::max)(sizes.withoutOverlapScratchSizeInBytes, sizes.computeIntensitySizeInBytes);
        if (!Cuda(driver.cuMemAlloc(&state, stateBytes), "allocate denoiser state") ||
            !Cuda(driver.cuMemAlloc(&scratch, scratchBytes), "allocate denoiser scratch") ||
            !Cuda(driver.cuMemAlloc(&intensity, sizeof(float)), "allocate HDR intensity"))
        {
            return false;
        }
        if (!Optix(optixDenoiserSetup(denoiser, stream, newWidth, newHeight, state, stateBytes, scratch, scratchBytes),
                   "set up denoiser"))
        {
            return false;
        }
        HRESULT result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        if (FAILED(result))
        {
            return Fail("create shared fence", result);
        }
        HANDLE sharedHandle = nullptr;
        result = device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
        if (FAILED(result))
        {
            return Fail("share D3D12 fence", result);
        }
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC descriptor = {};
        descriptor.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE;
        descriptor.handle.win32.handle = sharedHandle;
        CUresult imported = driver.cuImportExternalSemaphore(&semaphore, &descriptor);
        CloseHandle(sharedHandle);
        if (!Cuda(imported, "import shared D3D12 fence"))
        {
            return false;
        }
        width = newWidth;
        height = newHeight;
        planeBytes = static_cast<size_t>(width) * height * 16;
        return true;
    }

    bool CreateSlot(UINT index)
    {
        if (slots.size() <= index)
        {
            slots.resize(static_cast<size_t>(index) + 1);
        }
        Slot& slot = slots[index];
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description = {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = planeBytes * 4;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT result =
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description, D3D12_RESOURCE_STATE_COMMON,
                                            nullptr, IID_PPV_ARGS(&slot.buffer));
        if (FAILED(result))
        {
            return Fail("create shared image buffer", result);
        }
        HANDLE sharedHandle = nullptr;
        result = device->CreateSharedHandle(slot.buffer.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
        if (FAILED(result))
        {
            return Fail("share image buffer", result);
        }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC descriptor = {};
        descriptor.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
        descriptor.handle.win32.handle = sharedHandle;
        descriptor.size = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        descriptor.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
        CUresult imported = driver.cuImportExternalMemory(&slot.memory, &descriptor);
        CloseHandle(sharedHandle);
        if (!Cuda(imported, "import shared image buffer"))
        {
            return false;
        }
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapping = {};
        mapping.size = planeBytes * 4;
        return Cuda(driver.cuExternalMemoryGetMappedBuffer(&slot.pointer, slot.memory, &mapping),
                    "map shared image buffer");
    }

    OptixImage2D Image(CUdeviceptr pointer) const
    {
        OptixImage2D image = {};
        image.data = pointer;
        image.width = width;
        image.height = height;
        image.rowStrideInBytes = width * 16;
        image.pixelStrideInBytes = 16;
        image.format = OPTIX_PIXEL_FORMAT_FLOAT4;
        return image;
    }
};

std::vector<OptixInterop::Impl*> OptixInterop::Impl::instances;

OptixInterop::OptixInterop() : impl(std::make_unique<Impl>()) {}

OptixInterop::~OptixInterop() = default;

bool OptixInterop::Initialize(ID3D12Device* graphicsDevice)
{
    if (available)
    {
        return device.Get() == graphicsDevice;
    }
    Shutdown();
    if (!graphicsDevice)
    {
        return Fail("initialize without a D3D12 device", E_INVALIDARG);
    }
    if (!driver.Load())
    {
        Fail("load CUDA driver", GetLastError());
        Shutdown();
        return false;
    }
    if (!Cuda(driver.cuInit(0), "initialize CUDA"))
    {
        Shutdown();
        return false;
    }
    int deviceCount = 0;
    if (!Cuda(driver.cuDeviceGetCount(&deviceCount), "enumerate CUDA adapters"))
    {
        Shutdown();
        return false;
    }
    const LUID adapterLuid = graphicsDevice->GetAdapterLuid();
    bool matched = false;
    for (int index = 0; index < deviceCount; ++index)
    {
        CUdevice candidate = 0;
        char candidateLuid[8] = {};
        unsigned int nodeMask = 0;
        if (driver.cuDeviceGet(&candidate, index) == CUDA_SUCCESS &&
            driver.cuDeviceGetLuid(candidateLuid, &nodeMask, candidate) == CUDA_SUCCESS &&
            memcmp(candidateLuid, &adapterLuid, sizeof(adapterLuid)) == 0)
        {
            cudaDevice = candidate;
            matched = true;
            break;
        }
    }
    if (!matched)
    {
        Fail("match CUDA adapter to D3D12 LUID", -1);
        Shutdown();
        return false;
    }
    if (!Cuda(driver.cuDevicePrimaryCtxRetain(&context, cudaDevice), "retain CUDA primary context"))
    {
        Shutdown();
        return false;
    }
    bool initialized = false;
    {
        ContextScope scope;
        if (scope.active && Cuda(driver.cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "create CUDA stream") &&
            Optix(optixInitWithHandle(&optixLibrary), "load OptiX driver"))
        {
            OptixDeviceContextOptions options = {};
            initialized = Optix(optixDeviceContextCreate(context, &options, &optixContext), "create OptiX context");
        }
    }
    if (!initialized)
    {
        Shutdown();
        return false;
    }
    device = graphicsDevice;
    available = true;
    status = "OptiX HDR denoiser ready (CUDA/D3D12 shared GPU memory)";
    WriteGraphicsLog("OptiX: %s\n", status.c_str());
    return true;
}

void OptixInterop::Shutdown()
{
    available = false;
    for (Impl* instance : Impl::instances)
    {
        instance->Release();
    }
    if (context)
    {
        {
            ContextScope scope;
            if (scope.active)
            {
                if (stream)
                {
                    driver.cuStreamSynchronize(stream);
                }
                if (optixContext)
                {
                    optixDeviceContextDestroy(optixContext);
                    optixContext = nullptr;
                }
                if (stream)
                {
                    driver.cuStreamDestroy(stream);
                    stream = nullptr;
                }
            }
        }
        driver.cuDevicePrimaryCtxRelease(cudaDevice);
        context = nullptr;
    }
    if (optixLibrary)
    {
        optixUninitWithHandle(optixLibrary);
        optixLibrary = nullptr;
    }
    device.Reset();
    if (driver.module)
    {
        FreeLibrary(driver.module);
        driver = {};
    }
}

bool OptixInterop::IsAvailable()
{
    return available;
}

const char* OptixInterop::GetStatus()
{
    return status.c_str();
}

bool OptixInterop::Ensure(UINT width, UINT height, UINT frameIndex)
{
    if (!available || width == 0 || height == 0 || width > UINT_MAX / 16 ||
        static_cast<uint64_t>(width) * height > (std::numeric_limits<size_t>::max)() / 64)
    {
        return false;
    }
    if (impl->width != width || impl->height != height)
    {
        impl->Release();
    }
    ContextScope scope;
    if (!scope.active)
    {
        return Fail("push CUDA context", -1);
    }
    if (!impl->denoiser && !impl->CreateWorkspace(width, height))
    {
        impl->Release();
        return false;
    }
    if (frameIndex >= impl->slots.size() || !impl->slots[frameIndex].pointer)
    {
        if (!impl->CreateSlot(frameIndex))
        {
            impl->Release();
            return false;
        }
    }
    return true;
}

ID3D12Resource* OptixInterop::GetSharedBuffer(UINT frameIndex) const
{
    if (frameIndex >= impl->slots.size())
    {
        return nullptr;
    }
    return impl->slots[frameIndex].buffer.Get();
}

bool OptixInterop::Enqueue(ID3D12CommandQueue* queue, UINT frameIndex)
{
    if (!available || !queue || frameIndex >= impl->slots.size() || !impl->slots[frameIndex].pointer)
    {
        return false;
    }
    ContextScope scope;
    if (!scope.active)
    {
        return Fail("push CUDA context", -1);
    }
    const UINT64 readyValue = impl->fenceValue + 1;
    const UINT64 finishedValue = impl->fenceValue + 2;
    HRESULT result = queue->Signal(impl->fence.Get(), readyValue);
    if (FAILED(result))
    {
        return Fail("signal D3D12 input completion", result);
    }
    impl->fenceValue = finishedValue;
    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait = {};
    wait.params.fence.value = readyValue;
    if (!Cuda(driver.cuWaitExternalSemaphoresAsync(&impl->semaphore, &wait, 1, stream), "wait for D3D12 inputs"))
    {
        return false;
    }
    const CUdeviceptr pointer = impl->slots[frameIndex].pointer;
    OptixDenoiserLayer layer = {};
    layer.input = impl->Image(pointer);
    layer.output = impl->Image(pointer + impl->planeBytes * 3);
    OptixDenoiserGuideLayer guides = {};
    guides.normal = impl->Image(pointer + impl->planeBytes);
    guides.albedo = impl->Image(pointer + impl->planeBytes * 2);
    OptixDenoiserParams parameters = {};
    parameters.hdrIntensity = impl->intensity;
    bool denoised = Optix(optixDenoiserComputeIntensity(impl->denoiser, stream, &layer.input, impl->intensity,
                                                        impl->scratch, impl->scratchBytes),
                          "compute HDR intensity");
    if (denoised)
    {
        denoised = Optix(optixDenoiserInvoke(impl->denoiser, stream, &parameters, impl->state, impl->stateBytes,
                                             &guides, &layer, 1, 0, 0, impl->scratch, impl->scratchBytes),
                         "invoke HDR denoiser");
    }
    CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal = {};
    signal.params.fence.value = finishedValue;
    if (!Cuda(driver.cuSignalExternalSemaphoresAsync(&impl->semaphore, &signal, 1, stream), "signal CUDA completion"))
    {
        // 対応するシグナルを CUDA が受理するまで、グラフィックス側の待機を投入しない。
        // 例外時に一部だけ投入された処理は、呼び出し側がリソースを再利用する前に完了させる。
        driver.cuStreamSynchronize(stream);
        available = false;
        return false;
    }
    result = queue->Wait(impl->fence.Get(), finishedValue);
    if (FAILED(result))
    {
        driver.cuStreamSynchronize(stream);
        return Fail("wait for CUDA completion", result);
    }
    return denoised;
}
#else
struct OptixInterop::Impl
{
};

OptixInterop::OptixInterop() : impl(std::make_unique<Impl>()) {}

OptixInterop::~OptixInterop() = default;

bool OptixInterop::Initialize(ID3D12Device*)
{
    return false;
}

void OptixInterop::Shutdown() {}

bool OptixInterop::IsAvailable()
{
    return false;
}

const char* OptixInterop::GetStatus()
{
    return "OptiX support was not enabled in this build";
}

bool OptixInterop::Ensure(UINT, UINT, UINT)
{
    return false;
}

ID3D12Resource* OptixInterop::GetSharedBuffer(UINT) const
{
    return nullptr;
}

bool OptixInterop::Enqueue(ID3D12CommandQueue*, UINT)
{
    return false;
}
#endif
