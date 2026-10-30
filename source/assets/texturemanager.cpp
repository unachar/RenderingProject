#include "pch.h"
#include "graphicsdevice.h"
#include "DirectXTex.h"
#include "texturemanager.h"
#include "texturecompression.h"
#include "iesprofile.h"
#include "componentmanager.h"
#include "renderersettings.h"
#include <thread>
#include <memory>
#include <future>
#include <cctype>
#include <filesystem>
#include "frameconstants.h"
#include "gpumemorybudget.h"
#include "memoryallocator.h"
#include "graphicslog.h"
#include <atomic>
#include <unordered_set>

unordered_map<string, TextureManager::TextureData> TextureManager::m_Textures;
int TextureManager::m_NextSrvIndex = 0;
static uint64_t g_IesProfileRevision = 1;
ComPtr<ID3D12Resource> TextureManager::m_DefaultTexture;
ComPtr<ID3D12Resource> TextureManager::m_ErrorTexture;

struct TextureMipUploadData
{
    vector<uint8_t> Pixels{};
    LONG_PTR RowPitch = 0;
    LONG_PTR SlicePitch = 0;
    UINT TileCount = 0;
};

struct TextureStreamingState
{
    vector<TextureMipUploadData> Mips{};
    vector<ComPtr<ID3D12Heap>> TileHeaps{};
    ComPtr<ID3D12Heap> TailHeap;
    UINT TailFirstMip = 0;
    uint64_t LastChangedFrame = 0;
    UINT MostDetailedResidentMip = 0;
    UINT StandardMipCount = 0;
    UINT TotalMipCount = 0;
    uint64_t LastTouchedFrame = 0;
};

// A borrowed reserved resource (thumbnails, ray descriptor caches) must keep its
// backing heaps alive even after the texture manager publishes a BC replacement.
class TextureHeapLifetime final : public IUnknown
{
  public:
    explicit TextureHeapLifetime(shared_ptr<TextureStreamingState> state) : m_State(move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output)
        {
            return E_POINTER;
        }
        *output = nullptr;
        if (iid != __uuidof(IUnknown))
        {
            return E_NOINTERFACE;
        }
        *output = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_References;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_References;
        if (references == 0)
        {
            delete this;
        }
        return references;
    }

  private:
    atomic<ULONG> m_References{1};
    shared_ptr<TextureStreamingState> m_State;
};

static constexpr GUID g_TextureHeapLifetimeId = {
    0x7be881b2, 0x677d, 0x4c7a, {0x8d, 0xaf, 0x99, 0x57, 0x2c, 0xba, 0x1e, 0x31}};

static ComPtr<ID3D12CommandAllocator> g_BatchCmdAlloc;
static ComPtr<ID3D12GraphicsCommandList> g_BatchCmdList;
static bool g_IsBatchLoading = false;
static ComPtr<ID3D12Fence> g_BatchFence;
static HANDLE g_BatchFenceEvent = nullptr;
static UINT64 g_BatchFenceValue = 0;
static ComPtr<ID3D12Fence> g_SingleFence;
static HANDLE g_SingleFenceEvent = nullptr;
static UINT64 g_SingleFenceValue = 0;
static mutex g_TextureMutex;
static mutex g_UploadMutex;

struct UploadPoolEntry
{
    ComPtr<ID3D12Resource> Resource;
    UINT64 Size;
};

static vector<UploadPoolEntry> g_UploadPool;
static vector<UploadPoolEntry> g_DeferredUploadReturns;

struct StreamingUploadEntry
{
    ComPtr<ID3D12Resource> Resource;
    UINT64 Size = 0;
    uint64_t Fence = 0;
};

static vector<StreamingUploadEntry> g_StreamingUploads;

struct RetiredTextureHeap
{
    ComPtr<ID3D12Heap> Heap;
    uint64_t Fence = 0;
};

static vector<RetiredTextureHeap> g_RetiredTextureHeaps;
static uint64_t g_StreamingFrame = 1;
static bool g_ReservedResourcesSupported = false;

static constexpr bool g_MaterialMipMapsEnabled = true;

static constexpr bool g_ReservedResourceStreamingSafe = true;

struct DecodedEntry
{
    string Key{};
    filesystem::path SourcePath{};
    TextureUsage Usage = TextureUsage::Color;
    int SrvIndex = -1;
    TexMetadata Metadata{};
    unique_ptr<ScratchImage> Image = nullptr;
    promise<bool> Promise{};
    future<bool> Future{};
};

static vector<shared_ptr<DecodedEntry>> g_PendingTextures;

string PathToUtf8(const filesystem::path& path)
{
    const auto value = path.u8string();
    return string(reinterpret_cast<const char*>(value.data()), value.size());
}

string MakeTextureCacheKey(const filesystem::path& sourcePath)
{
    error_code ec;
    filesystem::path path = filesystem::absolute(sourcePath, ec);
    if (ec)
    {
        path = sourcePath;
    }
    path = path.lexically_normal();
    string key = PathToUtf8(path);
#ifdef _WIN32
    transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
#endif
    return key;
}

HRESULT LoadImageFromFile(const wchar_t* wPath, TexMetadata& metadata, ScratchImage& image)
{
    filesystem::path path(wPath);
    string ext = path.extension().string();
    transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });

    if (ext == ".dds")
    {
        return LoadFromDDSFile(wPath, DDS_FLAGS_NONE, &metadata, image);
    }

    HRESULT hr = LoadFromWICFile(wPath, WIC_FLAGS_NONE, &metadata, image);
    if (FAILED(hr))
    {
        hr = LoadFromTGAFile(wPath, &metadata, image);
    }
    return hr;
}

HRESULT LoadImageFromMemory(const uint8_t* data, size_t size, TexMetadata& metadata, ScratchImage& image)
{
    if (!data || size == 0)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = E_FAIL;
    if (size >= 4 && memcmp(data, "DDS ", 4) == 0)
    {
        hr = LoadFromDDSMemory(data, size, DDS_FLAGS_NONE, &metadata, image);
        if (SUCCEEDED(hr))
        {
            return hr;
        }
    }

    hr = LoadFromWICMemory(data, size, WIC_FLAGS_NONE, &metadata, image);
    if (SUCCEEDED(hr))
    {
        return hr;
    }

    return LoadFromTGAMemory(data, size, &metadata, image);
}

void EnsureMipChain(TexMetadata& metadata, ScratchImage& image)
{
    if (!g_MaterialMipMapsEnabled)
    {
        return;
    }
    // Perf: 256px 以上にも mip を生成してエイリアシングと帯域を削減 (従来は 512px 以上のみ)。
    if (metadata.mipLevels > 1 || metadata.arraySize != 1 || metadata.dimension != TEX_DIMENSION_TEXTURE2D ||
        metadata.width < 256 || metadata.height < 256 || IsCompressed(metadata.format))
    {
        return;
    }
    const Image* baseImage = image.GetImage(0, 0, 0);
    if (!baseImage)
    {
        return;
    }
    ScratchImage mipChain;
    if (SUCCEEDED(GenerateMipMaps(*baseImage, TEX_FILTER_FANT, 0, mipChain)))
    {
        image = move(mipChain);
        metadata = image.GetMetadata();
    }
}

ComPtr<ID3D12Heap> CreateTextureTileHeap(ID3D12Device* device, UINT tileCount)
{
    if (!device || tileCount == 0)
    {
        return nullptr;
    }
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = static_cast<UINT64>(tileCount) * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    if (FAILED(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))))
    {
        return nullptr;
    }
    return heap;
}

ComPtr<ID3D12Resource> LoadReservedTextureToDevice(ID3D12Device* device, const TexMetadata& metadata,
                                                   const ScratchImage& image,
                                                   shared_ptr<TextureStreamingState>& outStreaming)
{
    outStreaming.reset();
    if (!g_ReservedResourceStreamingSafe || !device || !g_ReservedResourcesSupported ||
        !RendererSettings::GetTextureStreamingEnabled() || !RendererSettings::GetReservedResourcesEnabled() ||
        metadata.dimension != TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.mipLevels < 4 ||
        metadata.width < 512 || metadata.height < 512)
    {
        return nullptr;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = metadata.width;
    desc.Height = static_cast<UINT>(metadata.height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
    desc.Format = metadata.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> texture;
    HRESULT hr = device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr))
    {
        return nullptr;
    }

    UINT totalTiles = 0;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE tileShape{};
    UINT tilingCount = static_cast<UINT>(metadata.mipLevels);
    vector<D3D12_SUBRESOURCE_TILING> tilings(tilingCount);
    device->GetResourceTiling(texture.Get(), &totalTiles, &packed, &tileShape, &tilingCount, 0, tilings.data());
    if (totalTiles == 0 || tilingCount == 0)
    {
        return nullptr;
    }

    const UINT standardMipCount = packed.NumStandardMips;
    UINT firstResidentMip;
    if (standardMipCount > 2)
    {
        firstResidentMip = standardMipCount - 2;
    }
    else
    {
        firstResidentMip = 0;
    }
    UINT firstResidentTile;
    if (firstResidentMip < standardMipCount)
    {
        firstResidentTile = tilings[firstResidentMip].StartTileIndexInOverallResource;
    }
    else
    {
        firstResidentTile = packed.StartTileIndexInOverallResource;
    }
    const UINT residentTileCount = totalTiles - firstResidentTile;
    ComPtr<ID3D12Heap> residentHeap = CreateTextureTileHeap(device, residentTileCount);
    if (!residentHeap)
    {
        return nullptr;
    }

    ID3D12CommandQueue* queue = GraphicsDevice::GetCommandQueue();
    D3D12_TILED_RESOURCE_COORDINATE residentCoordinate{};
    residentCoordinate.Subresource = firstResidentMip;
    D3D12_TILE_REGION_SIZE residentRegion{};
    residentRegion.NumTiles = residentTileCount;
    D3D12_TILE_RANGE_FLAGS residentFlag = D3D12_TILE_RANGE_FLAG_NONE;
    UINT residentOffset = 0;
    queue->UpdateTileMappings(texture.Get(), 1, &residentCoordinate, &residentRegion, residentHeap.Get(), 1,
                              &residentFlag, &residentOffset, &residentTileCount, D3D12_TILE_MAPPING_FLAG_NONE);

    for (UINT mip = 0; mip < firstResidentMip; ++mip)
    {
        const UINT mipTileCount = tilings[mip].WidthInTiles * tilings[mip].HeightInTiles * tilings[mip].DepthInTiles;
        if (mipTileCount == 0)
        {
            continue;
        }
        D3D12_TILED_RESOURCE_COORDINATE coordinate{};
        coordinate.Subresource = mip;
        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = mipTileCount;
        D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
        UINT heapOffset = 0;
        queue->UpdateTileMappings(texture.Get(), 1, &coordinate, &region, residentHeap.Get(), 1, &flag, &heapOffset,
                                  &mipTileCount, D3D12_TILE_MAPPING_FLAG_NONE);
    }

    const UINT uploadCount = static_cast<UINT>(metadata.mipLevels) - firstResidentMip;
    vector<D3D12_SUBRESOURCE_DATA> subresources(uploadCount);
    for (UINT i = 0; i < uploadCount; ++i)
    {
        const Image* mipImage = image.GetImage(firstResidentMip + i, 0, 0);
        if (!mipImage)
        {
            return nullptr;
        }
        subresources[i] = {mipImage->pixels, static_cast<LONG_PTR>(mipImage->rowPitch),
                           static_cast<LONG_PTR>(mipImage->slicePitch)};
    }
    const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), firstResidentMip, uploadCount);
    ComPtr<ID3D12Resource> upload = TextureManager::AcquireUploadBuffer(device, uploadSize);
    if (!upload)
    {
        return nullptr;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> ownedList;
    ID3D12GraphicsCommandList* commandList = nullptr;
    const bool batch = TextureManager::IsBatchLoading();
    if (batch)
    {
        commandList = TextureManager::GetBatchCommandList();
    }
    else
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&ownedList))))
        {
            return nullptr;
        }
        commandList = ownedList.Get();
    }
    if (!commandList)
    {
        return nullptr;
    }
    UpdateSubresources(commandList, texture.Get(), upload.Get(), 0, firstResidentMip, uploadCount, subresources.data());
    auto toShader = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShader);
    if (batch)
    {
        TextureManager::ReleaseUploadBuffer(upload, uploadSize, true);
    }
    else
    {
        if (!TextureManager::ExecuteCommandListAndSync(commandList))
        {
            return nullptr;
        }
        TextureManager::ReleaseUploadBuffer(upload, uploadSize, false);
    }

    auto streaming = make_shared<TextureStreamingState>();
    streaming->MostDetailedResidentMip = firstResidentMip;
    streaming->StandardMipCount = standardMipCount;
    streaming->TotalMipCount = static_cast<UINT>(metadata.mipLevels);
    streaming->LastTouchedFrame = g_StreamingFrame;
    streaming->TailHeap = residentHeap;
    streaming->TailFirstMip = firstResidentMip;
    streaming->TileHeaps.resize(firstResidentMip);
    streaming->Mips.resize(metadata.mipLevels);
    for (UINT mip = 0; mip < metadata.mipLevels; ++mip)
    {
        const Image* mipImage = image.GetImage(mip, 0, 0);
        if (!mipImage)
        {
            continue;
        }
        auto& destination = streaming->Mips[mip];
        destination.Pixels.assign(mipImage->pixels, mipImage->pixels + mipImage->slicePitch);
        destination.RowPitch = static_cast<LONG_PTR>(mipImage->rowPitch);
        destination.SlicePitch = static_cast<LONG_PTR>(mipImage->slicePitch);
        if (mip < standardMipCount)
        {
            destination.TileCount = tilings[mip].WidthInTiles * tilings[mip].HeightInTiles * tilings[mip].DepthInTiles;
        }
    }
    auto* lifetime = new TextureHeapLifetime(streaming);
    const HRESULT lifetimeResult = texture->SetPrivateDataInterface(g_TextureHeapLifetimeId, lifetime);
    lifetime->Release();
    if (FAILED(lifetimeResult))
    {
        return nullptr;
    }
    outStreaming = move(streaming);
    return texture;
}

ComPtr<ID3D12Resource> LoadTextureToDevice(ID3D12Device* device, const TexMetadata& metadata, const ScratchImage& image,
                                           D3D12_RESOURCE_STATES readState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
{
    ComPtr<ID3D12Resource> texture;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(metadata.dimension);
    desc.Width = metadata.width;
    desc.Height = static_cast<UINT>(metadata.height);
    desc.DepthOrArraySize = static_cast<UINT16>(metadata.arraySize);
    desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
    desc.Format = metadata.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = S_OK;
    if (ID3D12Resource* allocated =
            MemoryAllocator::CreateResourceAutoLifetime(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr))
    {
        texture.Attach(allocated);
    }
    else
    {
        auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    }

    if (FAILED(hr) || !texture)
    {
        Debug::Log("ERROR: CreateCommittedResource(Texture) failed (HRESULT: 0x%08X)\n", hr);
        return nullptr;
    }

    const size_t numSubresources = metadata.mipLevels * metadata.arraySize;
    vector<D3D12_SUBRESOURCE_DATA> subresources(numSubresources);

    for (size_t i = 0; i < numSubresources; ++i)
    {
        size_t mip = i % metadata.mipLevels;
        size_t slice = i / metadata.mipLevels;
        const Image* img = image.GetImage(mip, slice, 0);
        if (img)
        {
            subresources[i].pData = img->pixels;
            subresources[i].RowPitch = img->rowPitch;
            subresources[i].SlicePitch = img->slicePitch;
        }
    }

    UINT64 uploadBufferSize = 0;
    device->GetCopyableFootprints(&desc, 0, static_cast<UINT>(numSubresources), 0, nullptr, nullptr, nullptr,
                                  &uploadBufferSize);

    ComPtr<ID3D12Resource> uploadBuffer = TextureManager::AcquireUploadBuffer(device, uploadBufferSize);
    if (!uploadBuffer)
    {
        return nullptr;
    }

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;

    const bool batchMode = TextureManager::IsBatchLoading();
    if (batchMode)
    {
        cmdList = TextureManager::GetBatchCommandList();
        if (!cmdList)
        {
            Debug::Log("ERROR: Batch command list is not initialized\n");
            return nullptr;
        }
    }
    else
    {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
        if (FAILED(hr))
        {
            Debug::Log("ERROR: CreateCommandAllocator failed (HRESULT: 0x%08X)\n", hr);
            return nullptr;
        }
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr,
                                       IID_PPV_ARGS(&cmdList));
        if (FAILED(hr))
        {
            Debug::Log("ERROR: CreateCommandList failed (HRESULT: 0x%08X)\n", hr);
            return nullptr;
        }
    }

    UpdateSubresources(cmdList.Get(), texture.Get(), uploadBuffer.Get(), 0, 0, static_cast<UINT>(numSubresources),
                       subresources.data());

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readState);
    cmdList->ResourceBarrier(1, &barrier);

    if (batchMode)
    {
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize, true);
    }
    else
    {
        if (!TextureManager::ExecuteCommandListAndSync(cmdList.Get()))
        {
            Debug::Log("ERROR: Texture upload failed\n");
            return nullptr;
        }
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadBufferSize, false);
    }

    return texture;
}

bool TextureManager::IsBatchLoading()
{
    return g_IsBatchLoading;
}

ID3D12GraphicsCommandList* TextureManager::GetBatchCommandList()
{
    return g_BatchCmdList.Get();
}

ComPtr<ID3D12Resource> TextureManager::AcquireUploadBuffer(ID3D12Device* device, UINT64 size)
{
    lock_guard<mutex> lock(g_UploadMutex);
    for (auto it = g_UploadPool.begin(); it != g_UploadPool.end(); ++it)
    {
        if (it->Size >= size)
        {
            ComPtr<ID3D12Resource> res = it->Resource;
            g_UploadPool.erase(it);
            return res;
        }
    }

    ComPtr<ID3D12Resource> uploadBuffer;
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(size);
    HRESULT hr =
        device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(hr))
    {
        Debug::Log("ERROR: AcquireUploadBuffer CreateCommittedResource failed\n");
        return nullptr;
    }
    return uploadBuffer;
}

void TextureManager::ReleaseUploadBuffer(ComPtr<ID3D12Resource> upload, UINT64 size, bool defer)
{
    if (!upload)
    {
        return;
    }
    lock_guard<mutex> lock(g_UploadMutex);
    if (defer)
    {
        g_DeferredUploadReturns.push_back({upload, size});
    }
    else
    {
        // Upload heaps are system memory on discrete GPUs. Bound the cache as well,
        // so a single large import does not remain allocated for the whole session.
        constexpr uint64_t poolLimit = 32ull * 1024 * 1024;
        uint64_t pooled = 0;
        for (const auto& entry : g_UploadPool)
        {
            pooled += entry.Size;
        }
        if (size <= poolLimit && pooled + size <= poolLimit)
        {
            g_UploadPool.push_back({upload, size});
        }
    }
}

bool TextureManager::ExecuteCommandListAndSync(ID3D12GraphicsCommandList* cmdList, bool closeList)
{
    if (!cmdList)
    {
        return false;
    }
    if (closeList)
    {
        HRESULT hr = cmdList->Close();
        if (FAILED(hr))
        {
            Debug::Log("ERROR: CommandList Close failed (HRESULT: 0x%08X)\n", hr);
            return false;
        }
    }

    ID3D12CommandQueue* queue = GraphicsDevice::GetCommandQueue();
    ID3D12CommandList* ppCmdLists[] = {cmdList};
    queue->ExecuteCommandLists(1, ppCmdLists);

    ID3D12Device* device = GraphicsDevice::GetDevice();
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue;
    HANDLE waitEvent;

    if (cmdList == g_BatchCmdList.Get())
    {
        if (!g_BatchFence)
        {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_BatchFence))))
            {
                Debug::Log("ERROR: CreateFence for batch failed\n");
                return false;
            }
        }
        if (!g_BatchFenceEvent)
        {
            g_BatchFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!g_BatchFenceEvent)
            {
                Debug::Log("ERROR: CreateEvent for batch failed\n");
                return false;
            }
        }
        fence = g_BatchFence;
        g_BatchFenceValue++;
        fenceValue = g_BatchFenceValue;
        waitEvent = g_BatchFenceEvent;
    }
    else
    {
        if (!g_SingleFence)
        {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_SingleFence))))
            {
                Debug::Log("ERROR: CreateFence failed\n");
                return false;
            }
        }
        if (!g_SingleFenceEvent)
        {
            g_SingleFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!g_SingleFenceEvent)
            {
                Debug::Log("ERROR: CreateEvent failed\n");
                return false;
            }
        }
        fence = g_SingleFence;
        g_SingleFenceValue++;
        fenceValue = g_SingleFenceValue;
        waitEvent = g_SingleFenceEvent;
    }

    queue->Signal(fence.Get(), fenceValue);
    if (fence->GetCompletedValue() < fenceValue)
    {
        if (FAILED(fence->SetEventOnCompletion(fenceValue, waitEvent)))
        {
            Debug::Log("ERROR: SetEventOnCompletion failed\n");
            return false;
        }
        WaitForSingleObject(waitEvent, INFINITE);
    }

    return true;
}

void TextureManager::BeginTextureLoading()
{
    if (g_IsBatchLoading)
    {
        return;
    }
    ID3D12Device* device = GraphicsDevice::GetDevice();

    if (!g_BatchCmdAlloc)
    {
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_BatchCmdAlloc));
        if (FAILED(hr))
        {
            Debug::Log("ERROR: BeginTextureLoading CreateCommandAllocator failed\n");
            return;
        }
    }
    else
    {
        HRESULT hr = g_BatchCmdAlloc->Reset();
        if (FAILED(hr))
        {
            Debug::Log("ERROR: BeginTextureLoading Reset allocator failed\n");
            return;
        }
    }

    if (!g_BatchCmdList)
    {
        HRESULT hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_BatchCmdAlloc.Get(), nullptr,
                                               IID_PPV_ARGS(&g_BatchCmdList));
        if (FAILED(hr))
        {
            Debug::Log("ERROR: BeginTextureLoading CreateCommandList failed\n");
            return;
        }
    }
    else
    {
        HRESULT hr = g_BatchCmdList->Reset(g_BatchCmdAlloc.Get(), nullptr);
        if (FAILED(hr))
        {
            Debug::Log("ERROR: BeginTextureLoading Reset command list failed\n");
            return;
        }
    }

    g_IsBatchLoading = true;
}

void TextureManager::EndTextureLoading()
{
    if (!g_IsBatchLoading)
    {
        return;
    }

    vector<shared_ptr<DecodedEntry>> pending;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        pending.swap(g_PendingTextures);
    }

    vector<future<void>> decodeFutures;
    decodeFutures.reserve(pending.size());
    for (auto& p : pending)
    {
        if (p->Future.valid())
        {
            decodeFutures.push_back(async(launch::deferred, [f = move(p->Future)]() mutable {
                f.wait();
            }));
        }
    }
    for (auto& f : decodeFutures)
    {
        f.get();
    }

    ID3D12Device* device = GraphicsDevice::GetDevice();
    bool ok = true;

    struct TextureUploadJob
    {
        shared_ptr<DecodedEntry> Entry;
        ComPtr<ID3D12Resource> Resource;
    };

    vector<TextureUploadJob> jobs;
    jobs.reserve(pending.size());

    for (auto& p : pending)
    {
        auto& d = *p;
        TextureData td;
        td.SrvIndex = d.SrvIndex;

        if (!d.Image)
        {
            td.Resource = m_ErrorTexture;
            if (m_ErrorTexture)
            {
                TextureManager::CreateShaderResourceView(m_ErrorTexture.Get(), m_ErrorTexture->GetDesc(), d.SrvIndex);
            }
            lock_guard<mutex> lock(g_TextureMutex);
            m_Textures[d.Key] = td;
            continue;
        }

        EnsureMipChain(d.Metadata, *d.Image);
        shared_ptr<TextureStreamingState> streaming;
        ComPtr<ID3D12Resource> resource = LoadReservedTextureToDevice(device, d.Metadata, *d.Image, streaming);
        if (!resource)
        {
            resource = LoadTextureToDevice(device, d.Metadata, *d.Image);
        }
        if (!resource)
        {
            ok = false;
            td.Resource = m_ErrorTexture;
            if (m_ErrorTexture)
            {
                TextureManager::CreateShaderResourceView(m_ErrorTexture.Get(), m_ErrorTexture->GetDesc(), d.SrvIndex);
            }
            lock_guard<mutex> lock(g_TextureMutex);
            m_Textures[d.Key] = td;
            continue;
        }

        TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), d.SrvIndex,
                                                 streaming ? streaming->MostDetailedResidentMip : 0);
        td.Resource = resource;
        td.Streaming = move(streaming);
        if (!IsCompressed(d.Metadata.format))
        {
            td.CompressionCacheKey = TextureCompression::FileCacheKey(d.SourcePath, d.Usage);
        }
        error_code timestampError;
        const auto timestamp = filesystem::last_write_time(d.SourcePath, timestampError);
        if (!timestampError)
        {
            td.SourceTime = timestamp.time_since_epoch().count();
        }
        lock_guard<mutex> lock(g_TextureMutex);
        m_Textures[d.Key] = td;
    }

    if (!ok)
    {
        Debug::Log("ERROR: One or more batch texture uploads failed\n");
    }

    if (!TextureManager::ExecuteCommandListAndSync(g_BatchCmdList.Get()))
    {
        Debug::Log("ERROR: Batch command list execution failed\n");
    }

    vector<UploadPoolEntry> completedUploads;
    {
        lock_guard<mutex> lock(g_UploadMutex);
        completedUploads.swap(g_DeferredUploadReturns);
    }
    for (auto& entry : completedUploads)
    {
        ReleaseUploadBuffer(move(entry.Resource), entry.Size, false);
    }

    g_IsBatchLoading = false;
}

ComPtr<ID3D12Resource> TextureManager::CreateSolidColorTexture(ID3D12Device* device, uint32_t color, int srvIndex)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> texture;
    HRESULT hr = S_OK;
    if (ID3D12Resource* allocated =
            MemoryAllocator::CreateResourceAutoLifetime(desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr))
    {
        texture.Attach(allocated);
    }
    else
    {
        auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    }
    if (FAILED(hr) || !texture)
    {
        Debug::Log("ERROR: Failed to create solid color texture resource\n");
        return nullptr;
    }

    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    if (uploadSize < sizeof(uint32_t))
    {
        uploadSize = 256;
    }
    ComPtr<ID3D12Resource> uploadBuffer = AcquireUploadBuffer(device, uploadSize);
    if (!uploadBuffer)
    {
        auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
        if (FAILED(hr) || !uploadBuffer)
        {
            Debug::Log("ERROR: Failed to create upload buffer for solid color texture\n");
            return nullptr;
        }
    }

    void* pData = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &pData);
    if (FAILED(hr) || !pData)
    {
        Debug::Log("ERROR: Failed to map upload buffer for solid color texture\n");
        return nullptr;
    }
    memcpy(pData, &color, sizeof(uint32_t));
    uploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;

    const bool batchMode = g_IsBatchLoading;
    if (batchMode)
    {
        cmdList = g_BatchCmdList;
        if (!cmdList)
        {
            Debug::Log("ERROR: Batch command list is not initialized (solid color)\n");
            return nullptr;
        }
    }
    else
    {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
        if (FAILED(hr))
        {
            Debug::Log("ERROR: CreateCommandAllocator failed (solid color)\n");
            return nullptr;
        }
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr,
                                       IID_PPV_ARGS(&cmdList));
        if (FAILED(hr))
        {
            Debug::Log("ERROR: CreateCommandList failed (solid color)\n");
            return nullptr;
        }
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);

    CD3DX12_TEXTURE_COPY_LOCATION dst(texture.Get(), 0);
    CD3DX12_TEXTURE_COPY_LOCATION src(uploadBuffer.Get(), footprint);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);

    if (batchMode)
    {
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadSize, true);
    }
    else
    {
        if (!TextureManager::ExecuteCommandListAndSync(cmdList.Get()))
        {
            Debug::Log("ERROR: Solid color texture upload failed\n");
            return nullptr;
        }
        TextureManager::ReleaseUploadBuffer(uploadBuffer, uploadSize, false);
    }

    TextureManager::CreateShaderResourceView(texture.Get(), desc, srvIndex);

    return texture;
}

void TextureManager::Init()
{
    m_NextSrvIndex = FrameConstants::g_kTEXTURE_SRV_START_INDEX;
    ID3D12Device* device = GraphicsDevice::GetDevice();

    int whiteIndex = m_NextSrvIndex++;
    int pinkIndex = m_NextSrvIndex++;

    m_DefaultTexture = CreateSolidColorTexture(device, 0xFFFFFFFF, whiteIndex);
    m_ErrorTexture = CreateSolidColorTexture(device, 0xFFFF00FF, pinkIndex);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    g_ReservedResourcesSupported =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;

    lock_guard<mutex> lock(g_TextureMutex);
    TextureData whiteData;
    whiteData.Resource = m_DefaultTexture;
    whiteData.SrvIndex = whiteIndex;
    m_Textures.emplace("DEFAULT", whiteData);

    TextureData pinkData;
    pinkData.Resource = m_ErrorTexture;
    pinkData.SrvIndex = pinkIndex;
    m_Textures.emplace("ERROR", pinkData);
}

void TextureManager::Uninit()
{
    ++g_IesProfileRevision;
    m_Textures.clear();
    m_DefaultTexture.Reset();
    m_ErrorTexture.Reset();
    g_StreamingUploads.clear();
    g_RetiredTextureHeaps.clear();
    g_UploadPool.clear();
    g_DeferredUploadReturns.clear();
    g_ReservedResourcesSupported = false;
    g_StreamingFrame = 1;
    g_SingleFence.Reset();
    g_BatchFence.Reset();
    if (g_SingleFenceEvent)
    {
        CloseHandle(g_SingleFenceEvent);
        g_SingleFenceEvent = nullptr;
    }
    if (g_BatchFenceEvent)
    {
        CloseHandle(g_BatchFenceEvent);
        g_BatchFenceEvent = nullptr;
    }
}

bool TextureManager::CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_RESOURCE_DESC& desc, int srvIndex,
                                              UINT minimumMip)
{
    ID3D12Device* device = GraphicsDevice::GetDevice();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Texture2D.ResourceMinLODClamp = static_cast<float>(minimumMip);
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (g_MaterialMipMapsEnabled)
    {
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
    }
    else
    {
        srvDesc.Texture2D.MipLevels = 1;
    }

    UINT incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(FrameConstants::GetCbvHeap()->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(srvIndex, incrementSize);

    device->CreateShaderResourceView(pResource, &srvDesc, handle);
    return true;
}

int TextureManager::LoadTexture(const char* fileName)
{
    if (!fileName || fileName[0] == '\0')
    {
        Debug::Log("ERROR: Texture path is empty.\n");
        return GetErrorTextureIndex();
    }
    return LoadTexture(filesystem::u8path(fileName));
}

int TextureManager::FindLoadedTexture(const filesystem::path& path)
{
    lock_guard<mutex> lock(g_TextureMutex);
    const auto found = m_Textures.find(MakeTextureCacheKey(path));
    if (found == m_Textures.end())
    {
        return -1;
    }
    error_code error;
    const auto modified = filesystem::last_write_time(path, error);
    if (error || found->second.SourceTime != modified.time_since_epoch().count())
    {
        return -1;
    }
    return found->second.SrvIndex;
}

int TextureManager::LoadTexture(const filesystem::path& fileName)
{
    return LoadTexture(fileName, TextureUsage::Color);
}

// ソース画像を読み込み、BC 圧縮キャッシュがあればそれを使う。inlineCompress が偽なら、未キャッシュ時は
// 非圧縮のまま返し、圧縮はバックグラウンドで行う（次回読み込みから BC）。
static HRESULT LoadCompressedOrSource(const filesystem::path& path, TextureUsage usage, bool inlineCompress,
                                      TexMetadata& metadata, ScratchImage& image)
{
    const uint64_t cacheKey = TextureCompression::FileCacheKey(path, usage);
    if (TextureCompression::TryLoadCached(cacheKey, metadata, image))
    {
        return S_OK;
    }
    HRESULT hr = LoadImageFromFile(path.c_str(), metadata, image);
    if (FAILED(hr))
    {
        return hr;
    }
    EnsureMipChain(metadata, image);
    if (TextureCompression::GetSettings().Enabled && TextureCompression::IsCompressible(metadata))
    {
        if (inlineCompress)
        {
            TextureCompression::CompressAndCache(cacheKey, usage, metadata, image);
        }
        else
        {
            TextureCompression::CompressFileInBackground(cacheKey, usage, path);
        }
    }
    return S_OK;
}

int TextureManager::LoadTexture(const filesystem::path& fileName, TextureUsage usage)
{
    if (fileName.empty())
    {
        Debug::Log("ERROR: Texture path is empty.\n");
        return GetErrorTextureIndex();
    }

    const string key = MakeTextureCacheKey(fileName) + TextureCompression::KeySuffix(usage);
    const string displayPath = PathToUtf8(fileName);
    {
        lock_guard<mutex> lock(g_TextureMutex);
        auto it = m_Textures.find(key);
        if (it != m_Textures.end())
        {
            return it->second.SrvIndex;
        }
    }

    const int maxSrvIndex = FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS;
    int srvIndex = -1;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        if (m_NextSrvIndex >= maxSrvIndex)
        {
            Debug::Log("ERROR: SRV heap is full. Cannot load more textures.\n");
            return GetErrorTextureIndex();
        }
        srvIndex = m_NextSrvIndex++;
    }

    if (IsBatchLoading())
    {
        auto entry = make_shared<DecodedEntry>();
        entry->Key = key;
        entry->SourcePath = fileName;
        entry->Usage = usage;
        entry->SrvIndex = srvIndex;
        entry->Future = entry->Promise.get_future();

        filesystem::path sourcePath = fileName;
        const bool inlineCompress = !TextureCompression::GetSettings().BackgroundForSynchronousLoads;
        thread([entry, sourcePath, usage, inlineCompress]() mutable {
            TexMetadata metadata;
            auto image = make_unique<ScratchImage>();
            // 未キャッシュ時の圧縮は設定に従う（既定はバックグラウンド。シーン読み込みを初回でも遅くしない）。
            HRESULT hr = LoadCompressedOrSource(sourcePath, usage, inlineCompress, metadata, *image);
            if (SUCCEEDED(hr))
            {
                entry->Metadata = metadata;
                entry->Image = move(image);
                entry->Promise.set_value(true);
            }
            else
            {
                entry->Promise.set_value(false);
            }
        }).detach();

        TextureData tData;
        tData.Resource = m_ErrorTexture;
        tData.SrvIndex = srvIndex;
        {
            lock_guard<mutex> lock(g_TextureMutex);
            m_Textures.emplace(key, tData);
            g_PendingTextures.push_back(entry);
        }
        return srvIndex;
    }

    TexMetadata metadata;
    ScratchImage image;

    // メインスレッドを止めないよう、未キャッシュ時の圧縮は既定でバックグラウンドへ回す（設定で変更可）。
    HRESULT hr = LoadCompressedOrSource(
        fileName, usage, !TextureCompression::GetSettings().BackgroundForSynchronousLoads, metadata, image);
    if (FAILED(hr))
    {
        Debug::Log("ERROR: Failed to load texture: %s (HRESULT: 0x%08X)\n", displayPath.c_str(), hr);
        return GetErrorTextureIndex();
    }
    EnsureMipChain(metadata, image);

    ID3D12Device* device = GraphicsDevice::GetDevice();
    shared_ptr<TextureStreamingState> streaming;
    ComPtr<ID3D12Resource> resource = LoadReservedTextureToDevice(device, metadata, image, streaming);
    if (!resource)
    {
        resource = LoadTextureToDevice(device, metadata, image);
    }
    if (!resource)
    {
        return GetErrorTextureIndex();
    }

    TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), srvIndex,
                                             streaming ? streaming->MostDetailedResidentMip : 0);

    TextureData tData;
    tData.Resource = resource;
    tData.SrvIndex = srvIndex;
    tData.Streaming = move(streaming);
    if (!IsCompressed(metadata.format))
    {
        tData.CompressionCacheKey = TextureCompression::FileCacheKey(fileName, usage);
    }
    error_code timestampError;
    const auto timestamp = filesystem::last_write_time(fileName, timestampError);
    if (!timestampError)
    {
        tData.SourceTime = timestamp.time_since_epoch().count();
    }
    {
        lock_guard<mutex> lock(g_TextureMutex);
        m_Textures.emplace(key, tData);
    }

    return srvIndex;
}

int TextureManager::LoadNormalTexture(const char* fileName)
{
    if (!fileName || fileName[0] == '\0')
    {
        Debug::Log("ERROR: Texture path is empty.\n");
        return GetErrorTextureIndex();
    }
    // BC5（RG）。Z は NormalMapDecode.hlsli で再構成する。
    return LoadTexture(filesystem::u8path(fileName), TextureUsage::Normal);
}

int TextureManager::LoadIesProfile(const filesystem::path& fileName)
{
    const string key = "IES:" + MakeTextureCacheKey(fileName);
    {
        lock_guard<mutex> lock(g_TextureMutex);
        const auto found = m_Textures.find(key);
        if (found != m_Textures.end())
        {
            return found->second.SrvIndex;
        }
    }

    IesProfiles::Profile profile;
    string error;
    TextureData data;
    if (!IesProfiles::Load(fileName, profile, error))
    {
        Debug::Log("IES profile unavailable: %s (%s)\n", PathToUtf8(fileName).c_str(), error.c_str());
    }
    else
    {
        constexpr UINT width = 256, height = 128;
        const auto pixels = IesProfiles::Bake(profile, width, height);
        ScratchImage image;
        if (SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R32_FLOAT, width, height, 1, 1)))
        {
            const Image* target = image.GetImage(0, 0, 0);
            for (UINT row = 0; row < height; ++row)
            {
                memcpy(target->pixels + row * target->rowPitch, pixels.data() + row * width, width * sizeof(float));
            }
            data.Resource = LoadTextureToDevice(GraphicsDevice::GetDevice(), image.GetMetadata(), image,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (data.Resource)
            {
                lock_guard<mutex> lock(g_TextureMutex);
                const int end = FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS;
                if (m_NextSrvIndex < end)
                {
                    data.SrvIndex = m_NextSrvIndex++;
                    CreateShaderResourceView(data.Resource.Get(), data.Resource->GetDesc(), data.SrvIndex);
                }
            }
        }
    }
    const int result = data.SrvIndex;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        m_Textures.emplace(key, std::move(data));
        if (result >= 0)
        {
            ++g_IesProfileRevision;
        }
    }
    return result;
}

uint64_t TextureManager::WriteIesProfileViews(D3D12_CPU_DESCRIPTOR_HANDLE destination, uint64_t previousRevision)
{
    lock_guard<mutex> lock(g_TextureMutex);
    if (previousRevision == g_IesProfileRevision)
    {
        return previousRevision;
    }
    auto* device = GraphicsDevice::GetDevice();
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    // シェーダー可視ヒープはコピー元にできないため、呼び出し元のヒープに直接 SRV を作る。
    for (UINT index = 0; index < FrameConstants::g_kMAX_SRVS; ++index)
    {
        device->CreateShaderResourceView(nullptr, &view, CD3DX12_CPU_DESCRIPTOR_HANDLE(destination, index, increment));
    }
    for (const auto& entry : m_Textures)
    {
        const auto& texture = entry.second;
        const int index = texture.SrvIndex - static_cast<int>(FrameConstants::g_kTEXTURE_SRV_START_INDEX);
        if (entry.first.rfind("IES:", 0) == 0 && texture.Resource && index >= 0 &&
            index < static_cast<int>(FrameConstants::g_kMAX_SRVS))
        {
            device->CreateShaderResourceView(texture.Resource.Get(), &view,
                                             CD3DX12_CPU_DESCRIPTOR_HANDLE(destination, index, increment));
        }
    }
    return g_IesProfileRevision;
}

int TextureManager::LoadTextureFromMemory(const char* name, const uint8_t* pData, size_t dataSize)
{
    string key = name;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        auto it = m_Textures.find(key);
        if (it != m_Textures.end())
        {
            return it->second.SrvIndex;
        }
    }

    const int maxSrvIndex = FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS;
    int srvIndex = -1;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        if (m_NextSrvIndex >= maxSrvIndex)
        {
            Debug::Log("ERROR: SRV heap is full. Cannot load more textures.\n");
            return GetErrorTextureIndex();
        }
        srvIndex = m_NextSrvIndex++;
    }

    TexMetadata metadata;
    ScratchImage image;
    // モデル埋め込みテクスチャはベースカラー扱い（BC7）。内容ハッシュでキャッシュする。
    const uint64_t cacheKey = TextureCompression::MemoryCacheKey(pData, dataSize, TextureUsage::Color);
    if (!TextureCompression::TryLoadCached(cacheKey, metadata, image))
    {
        HRESULT hr = LoadImageFromMemory(pData, dataSize, metadata, image);
        if (FAILED(hr))
        {
            Debug::Log("ERROR: Failed to load texture from memory: %s (HRESULT: 0x%08X)\n", name, hr);
            return GetErrorTextureIndex();
        }
        EnsureMipChain(metadata, image);
        if (TextureCompression::GetSettings().Enabled && TextureCompression::IsCompressible(metadata))
        {
            if (TextureCompression::GetSettings().BackgroundForSynchronousLoads)
            {
                TextureCompression::CompressMemoryInBackground(cacheKey, TextureUsage::Color, pData, dataSize);
            }
            else
            {
                TextureCompression::CompressAndCache(cacheKey, TextureUsage::Color, metadata, image);
            }
        }
    }

    ID3D12Device* device = GraphicsDevice::GetDevice();
    shared_ptr<TextureStreamingState> streaming;
    ComPtr<ID3D12Resource> resource = LoadReservedTextureToDevice(device, metadata, image, streaming);
    if (!resource)
    {
        resource = LoadTextureToDevice(device, metadata, image);
    }
    if (!resource)
    {
        return GetErrorTextureIndex();
    }

    TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), srvIndex,
                                             streaming ? streaming->MostDetailedResidentMip : 0);

    TextureData tData;
    tData.Resource = resource;
    tData.SrvIndex = srvIndex;
    tData.Streaming = move(streaming);
    if (!IsCompressed(metadata.format))
    {
        tData.CompressionCacheKey = cacheKey;
    }
    {
        lock_guard<mutex> lock(g_TextureMutex);
        m_Textures.emplace(key, tData);
    }

    return srvIndex;
}

int TextureManager::RegisterExternalTexture(const char* name, ComPtr<ID3D12Resource> resource)
{
    if (name == nullptr || !resource)
    {
        return GetErrorTextureIndex();
    }
    const string key = name;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        auto it = m_Textures.find(key);
        if (it != m_Textures.end())
        {
            return it->second.SrvIndex;
        }
    }
    const int maxSrvIndex = FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS;
    int srvIndex = -1;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        if (m_NextSrvIndex >= maxSrvIndex)
        {
            Debug::Log("ERROR: SRV heap is full. Cannot register external texture.\n");
            return GetErrorTextureIndex();
        }
        srvIndex = m_NextSrvIndex++;
    }
    TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), srvIndex, 0);
    TextureData tData;
    tData.Resource = resource;
    tData.SrvIndex = srvIndex;
    {
        lock_guard<mutex> lock(g_TextureMutex);
        m_Textures.emplace(key, tData);
    }
    return srvIndex;
}

int TextureManager::GetDefaultTextureIndex()
{
    lock_guard<mutex> lock(g_TextureMutex);
    auto defaultIt = m_Textures.find("DEFAULT");
    if (defaultIt != m_Textures.end())
    {
        return defaultIt->second.SrvIndex;
    }
    return -1;
}

ComPtr<ID3D12Resource> TextureManager::GetTextureResource(int srvIndex)
{
    lock_guard<mutex> lock(g_TextureMutex);
    for (const auto& pair : m_Textures)
    {
        if (pair.second.SrvIndex == srvIndex)
        {
            return pair.second.Resource;
        }
    }
    return {};
}

UINT TextureManager::GetTextureResidentMip(int srvIndex)
{
    lock_guard<mutex> lock(g_TextureMutex);
    for (const auto& pair : m_Textures)
    {
        if (pair.second.SrvIndex == srvIndex)
        {
            return pair.second.Streaming ? pair.second.Streaming->MostDetailedResidentMip : 0;
        }
    }
    return 0;
}

int TextureManager::GetErrorTextureIndex()
{
    lock_guard<mutex> lock(g_TextureMutex);
    auto it = m_Textures.find("ERROR");
    if (it != m_Textures.end())
    {
        return it->second.SrvIndex;
    }
    return -1;
}

bool TextureManager::GetTextureSize(int srvIndex, UINT& outWidth, UINT& outHeight)
{
    lock_guard<mutex> lock(g_TextureMutex);
    for (const auto& pair : m_Textures)
    {
        if (pair.second.SrvIndex == srvIndex && pair.second.Resource)
        {
            D3D12_RESOURCE_DESC desc = pair.second.Resource->GetDesc();
            outWidth = static_cast<UINT>(desc.Width);
            outHeight = desc.Height;
            return true;
        }
    }
    outWidth = 0;
    outHeight = 0;
    return false;
}

vector<TextureManager::TextureInfo> TextureManager::GetLoadedTextureInfos()
{
    vector<TextureInfo> infos;
    lock_guard<mutex> lock(g_TextureMutex);
    infos.reserve(m_Textures.size());
    for (const auto& pair : m_Textures)
    {
        // IES は画像ファイルではなく、ライトの指定から再生成する実行時データ。
        // 画像一覧・画像パスの保存対象へ混ぜない。
        if (pair.first.rfind("IES:", 0) == 0)
        {
            continue;
        }
        TextureInfo info;
        info.Path = TextureCompression::StripKeySuffix(pair.first);
        info.SrvIndex = pair.second.SrvIndex;
        if (pair.second.Resource)
        {
            D3D12_RESOURCE_DESC desc = pair.second.Resource->GetDesc();
            info.Width = static_cast<UINT>(desc.Width);
            info.Height = desc.Height;
        }
        infos.push_back(info);
    }
    sort(infos.begin(), infos.end(), [](const TextureInfo& a, const TextureInfo& b) {
        return a.Path < b.Path;
    });
    return infos;
}

void TextureManager::TouchTexture(int srvIndex)
{
    if (srvIndex < 0)
    {
        return;
    }
    lock_guard<mutex> lock(g_TextureMutex);
    for (auto& pair : m_Textures)
    {
        auto& texture = pair.second;
        if (texture.SrvIndex == srvIndex && texture.Streaming)
        {
            texture.Streaming->LastTouchedFrame = g_StreamingFrame;
            return;
        }
    }
}

void TextureManager::OnFrameSubmitted(uint64_t fence)
{
    for (auto& retired : g_RetiredTextureHeaps)
    {
        if (retired.Fence == 0)
        {
            retired.Fence = fence;
        }
    }
    for (auto& upload : g_StreamingUploads)
    {
        if (upload.Fence == 0)
        {
            upload.Fence = fence;
        }
    }
}

void TextureManager::UpdateStreaming(ID3D12GraphicsCommandList* commandList)
{
    ++g_StreamingFrame;
    const uint64_t completed = GraphicsDevice::GetCompletedFence();
    g_RetiredTextureHeaps.erase(remove_if(g_RetiredTextureHeaps.begin(), g_RetiredTextureHeaps.end(),
                                          [completed](const auto& retired) {
                                              return retired.Fence != 0 && retired.Fence <= completed;
                                          }),
                                g_RetiredTextureHeaps.end());
    for (auto it = g_StreamingUploads.begin(); it != g_StreamingUploads.end();)
    {
        if (it->Fence != 0 && it->Fence <= completed)
        {
            ReleaseUploadBuffer(it->Resource, it->Size, false);
            it = g_StreamingUploads.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (!commandList)
    {
        return;
    }
    lock_guard<mutex> lock(g_TextureMutex);
    auto* device = GraphicsDevice::GetDevice();
    auto& stats = GpuMemoryBudget::Stats;
    const auto& options = GpuMemoryBudget::Options;
    const auto allocationBytes = [&](const TextureData& texture) -> uint64_t {
        if (!texture.Resource)
        {
            return 0;
        }
        if (texture.Streaming)
        {
            uint64_t bytes = texture.Streaming->TailHeap->GetDesc().SizeInBytes;
            for (const auto& heap : texture.Streaming->TileHeaps)
            {
                if (heap)
                {
                    bytes += heap->GetDesc().SizeInBytes;
                }
            }
            return bytes;
        }
        const auto desc = texture.Resource->GetDesc();
        const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
        return info.SizeInBytes == UINT64_MAX ? 0 : info.SizeInBytes;
    };
    stats.TextureBytes = 0;
    unordered_set<ID3D12Resource*> counted;
    for (const auto& pair : m_Textures)
    {
        if (counted.insert(pair.second.Resource.Get()).second)
        {
            stats.TextureBytes += allocationBytes(pair.second);
        }
    }
    GpuMemoryBudget::Update();
    if (g_StreamingFrame % 120 == 0)
    {
        WriteGraphicsLog(
            "VRAM: policy=%u local=%.2f budget=%.2f textures=%.2f texture_budget=%.2f evicted=%.2f BC=%llu MiB\n",
            unsigned(options.Enabled), stats.Usage / 1048576.0, stats.Budget / 1048576.0,
            stats.TextureBytes / 1048576.0, stats.TextureBudget / 1048576.0, stats.EvictedBytes / 1048576.0,
            stats.CompressedReplacements);
    }
    bool idle = false;
    const auto ensureIdle = [&]() {
        if (!idle)
        {
            idle = GraphicsDevice::WaitForGpuIdle();
        }
        return idle;
    };
    // Publish completed BC caches before recording draws. Descriptor indices stay stable.
    if (options.Enabled && TextureCompression::GetSettings().Enabled && g_StreamingFrame % 60 == 0)
    {
        for (auto& pair : m_Textures)
        {
            auto& texture = pair.second;
            if (texture.CompressionCacheKey == 0)
            {
                continue;
            }
            TexMetadata metadata{};
            ScratchImage image;
            if (!TextureCompression::TryLoadCached(texture.CompressionCacheKey, metadata, image))
            {
                continue;
            }
            if (!ensureIdle())
            {
                return;
            }
            shared_ptr<TextureStreamingState> streaming;
            auto replacement = LoadReservedTextureToDevice(device, metadata, image, streaming);
            if (!replacement)
            {
                replacement = LoadTextureToDevice(device, metadata, image);
            }
            if (!replacement)
            {
                break;
            }
            const uint64_t previous = allocationBytes(texture);
            texture.Resource = move(replacement);
            texture.Streaming = move(streaming);
            texture.CompressionCacheKey = 0;
            CreateShaderResourceView(texture.Resource.Get(), texture.Resource->GetDesc(), texture.SrvIndex,
                                     texture.Streaming ? texture.Streaming->MostDetailedResidentMip : 0);
            stats.TextureBytes = stats.TextureBytes - previous + allocationBytes(texture);
            ++stats.CompressedReplacements;
            break;
        }
    }
    if (!g_ReservedResourcesSupported || !g_ReservedResourceStreamingSafe ||
        !RendererSettings::GetReservedResourcesEnabled() || !RendererSettings::GetTextureStreamingEnabled())
    {
        return;
    }
    if (options.Enabled && g_StreamingFrame % 16 == 0)
    {
        vector<TextureData*> candidates;
        for (auto& pair : m_Textures)
        {
            if (pair.second.Streaming)
            {
                candidates.push_back(&pair.second);
            }
        }
        stable_sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
            return a->Streaming->LastTouchedFrame < b->Streaming->LastTouchedFrame;
        });
        for (auto* texture : candidates)
        {
            auto& state = *texture->Streaming;
            const bool unused = g_StreamingFrame > state.LastTouchedFrame + options.IdleTextureFrames;
            while (state.MostDetailedResidentMip < state.TailFirstMip &&
                   (unused || stats.TextureBytes > stats.TextureBudget))
            {
                if (!ensureIdle())
                {
                    return;
                }
                const UINT mip = state.MostDetailedResidentMip;
                D3D12_TILED_RESOURCE_COORDINATE coordinate{};
                coordinate.Subresource = mip;
                D3D12_TILE_REGION_SIZE region{};
                region.NumTiles = state.Mips[mip].TileCount;
                D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NULL;
                UINT offset = 0;
                GraphicsDevice::GetCommandQueue()->UpdateTileMappings(texture->Resource.Get(), 1, &coordinate, &region,
                                                                      nullptr, 1, &flag, &offset, &region.NumTiles,
                                                                      D3D12_TILE_MAPPING_FLAG_NONE);
                // Retain backing storage through the submission that contains the unmap.
                const uint64_t bytes = state.TileHeaps[mip]->GetDesc().SizeInBytes;
                g_RetiredTextureHeaps.push_back({move(state.TileHeaps[mip]), 0});
                ++state.MostDetailedResidentMip;
                state.LastChangedFrame = g_StreamingFrame;
                stats.TextureBytes -= bytes;
                stats.EvictedBytes += bytes;
                CreateShaderResourceView(texture->Resource.Get(), texture->Resource->GetDesc(), texture->SrvIndex,
                                         state.MostDetailedResidentMip);
            }
        }
    }
    const uint64_t uploadLimit = uint64_t(std::clamp(options.UploadMiBPerFrame, 1u, 64u)) * 1024 * 1024;
    uint64_t uploadsInFlight = 0;
    for (const auto& upload : g_StreamingUploads)
    {
        uploadsInFlight += upload.Size;
    }
    for (auto& pair : m_Textures)
    {
        auto& texture = pair.second;
        auto state = texture.Streaming;
        if (!state || state->MostDetailedResidentMip == 0 || g_StreamingFrame > state->LastTouchedFrame + 30 ||
            (state->LastChangedFrame != 0 && g_StreamingFrame < state->LastChangedFrame + 120))
        {
            continue;
        }
        const UINT mip = state->MostDetailedResidentMip - 1;
        const auto& mipData = state->Mips[mip];
        const uint64_t bytes = uint64_t(mipData.TileCount) * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Resource.Get(), mip, 1);
        if (mipData.Pixels.empty() || bytes == 0 ||
            (uploadsInFlight != 0 && uploadsInFlight + uploadSize > uploadLimit) ||
            (options.Enabled && (stats.TextureBytes + bytes > stats.TextureBudget || GpuMemoryBudget::UnderPressure())))
        {
            continue;
        }
        auto upload = AcquireUploadBuffer(device, uploadSize);
        auto mipHeap = CreateTextureTileHeap(device, mipData.TileCount);
        if (!upload || !mipHeap || !ensureIdle())
        {
            continue;
        }
        D3D12_TILED_RESOURCE_COORDINATE coordinate{};
        coordinate.Subresource = mip;
        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = mipData.TileCount;
        D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT offset = 0;
        GraphicsDevice::GetCommandQueue()->UpdateTileMappings(texture.Resource.Get(), 1, &coordinate, &region,
                                                              mipHeap.Get(), 1, &flag, &offset, &mipData.TileCount,
                                                              D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_SUBRESOURCE_DATA subresource{mipData.Pixels.data(), mipData.RowPitch, mipData.SlicePitch};
        auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            texture.Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toCopy);
        UpdateSubresources(commandList, texture.Resource.Get(), upload.Get(), 0, mip, 1, &subresource);
        auto toShader = CD3DX12_RESOURCE_BARRIER::Transition(texture.Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toShader);
        state->TileHeaps[mip] = move(mipHeap);
        state->MostDetailedResidentMip = mip;
        stats.TextureBytes += bytes;
        CreateShaderResourceView(texture.Resource.Get(), texture.Resource->GetDesc(), texture.SrvIndex, mip);
        g_StreamingUploads.push_back({move(upload), uploadSize, 0});
        break;
    }
}

bool TextureManager::IsReservedResourceStreamingSupported()
{
    return g_ReservedResourcesSupported;
}

bool TextureManager::IsReservedResourceStreamingAvailable()
{
    return g_ReservedResourcesSupported && g_ReservedResourceStreamingSafe;
}

#include "texturecompression.inl"
