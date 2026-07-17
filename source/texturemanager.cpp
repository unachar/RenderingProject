#include "pch.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "DirectXTex.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "renderersettings.h"
#include <thread>
#include <memory>
#include <future>
#include <cctype>
#include <filesystem>

unordered_map<string, TextureManager::TextureData> TextureManager::m_Textures;
int TextureManager::m_NextSrvIndex = 0;
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
	UINT MostDetailedResidentMip = 0;
	UINT StandardMipCount = 0;
	UINT TotalMipCount = 0;
	uint64_t LastTouchedFrame = 0;
};

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
struct UploadPoolEntry { ComPtr<ID3D12Resource> Resource; UINT64 Size; };
static vector<UploadPoolEntry> g_UploadPool;
static vector<UploadPoolEntry> g_DeferredUploadReturns;
struct StreamingUploadEntry
{
	ComPtr<ID3D12Resource> Resource;
	UINT64 Size = 0;
	uint64_t RetireFrame = 0;
};
static vector<StreamingUploadEntry> g_StreamingUploads;
static uint64_t g_StreamingFrame = 1;
static bool g_ReservedResourcesSupported = false;
// Material and environment roughness select a mip explicitly in the lighting
// shaders. Exposing only mip 0 makes roughness=1 sample the sharp environment
// and gives both Lit and PBR materials an incorrect mirror-like appearance.
static constexpr bool g_MaterialMipMapsEnabled = true;
// Reserved-resource streaming is available again after the diagnostic path.
// Hardware support and the project setting are still checked before use.
static constexpr bool g_ReservedResourceStreamingSafe = true;

struct DecodedEntry
{
	string Key{};
	int SrvIndex = -1;
	TexMetadata Metadata{};
	unique_ptr<ScratchImage> Image = nullptr;
	promise<bool> Promise{};
	future<bool> Future{};
};
static vector<shared_ptr<DecodedEntry>> g_PendingTextures;

namespace
{
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
		transform(key.begin(), key.end(), key.begin(),
			[](unsigned char c) { return static_cast<char>(tolower(c)); });
#endif
		return key;
	}

	HRESULT LoadImageFromFile(const wchar_t* wPath, TexMetadata& metadata, ScratchImage& image)
	{
		filesystem::path path(wPath);
		string ext = path.extension().string();
		transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c) { return static_cast<char>(tolower(c)); });

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
			if (SUCCEEDED(hr)) return hr;
		}

		hr = LoadFromWICMemory(data, size, WIC_FLAGS_NONE, &metadata, image);
		if (SUCCEEDED(hr)) return hr;

		return LoadFromTGAMemory(data, size, &metadata, image);
	}

	void EnsureMipChain(TexMetadata& metadata, ScratchImage& image)
	{
		if (!g_MaterialMipMapsEnabled) return;
		if (metadata.mipLevels > 1 || metadata.arraySize != 1 || metadata.dimension != TEX_DIMENSION_TEXTURE2D ||
			metadata.width < 512 || metadata.height < 512 || IsCompressed(metadata.format))
		{
			return;
		}
		const Image* baseImage = image.GetImage(0, 0, 0);
		if (!baseImage) return;
		ScratchImage mipChain;
		if (SUCCEEDED(GenerateMipMaps(*baseImage, TEX_FILTER_FANT, 0, mipChain)))
		{
			image = move(mipChain);
			metadata = image.GetMetadata();
		}
	}

	ComPtr<ID3D12Heap> CreateTextureTileHeap(ID3D12Device* device, UINT tileCount)
	{
		if (!device || tileCount == 0) return nullptr;
		D3D12_HEAP_DESC heapDesc{};
		heapDesc.SizeInBytes = static_cast<UINT64>(tileCount) * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
		ComPtr<ID3D12Heap> heap;
		if (FAILED(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return nullptr;
		return heap;
	}

	ComPtr<ID3D12Resource> LoadReservedTextureToDevice(
		ID3D12Device* device,
		const TexMetadata& metadata,
		const ScratchImage& image,
		shared_ptr<TextureStreamingState>& outStreaming)
	{
		outStreaming.reset();
		if (!g_ReservedResourceStreamingSafe ||
			!device || !g_ReservedResourcesSupported ||
			!RendererSettings::GetTextureStreamingEnabled() ||
			!RendererSettings::GetReservedResourcesEnabled() ||
			metadata.dimension != TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 ||
			metadata.mipLevels < 4 || metadata.width < 512 || metadata.height < 512)
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
		HRESULT hr = device->CreateReservedResource(
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
		if (FAILED(hr)) return nullptr;

		UINT totalTiles = 0;
		D3D12_PACKED_MIP_INFO packed{};
		D3D12_TILE_SHAPE tileShape{};
		UINT tilingCount = static_cast<UINT>(metadata.mipLevels);
		vector<D3D12_SUBRESOURCE_TILING> tilings(tilingCount);
		device->GetResourceTiling(texture.Get(), &totalTiles, &packed, &tileShape, &tilingCount, 0, tilings.data());
		if (totalTiles == 0 || tilingCount == 0) return nullptr;

		const UINT standardMipCount = packed.NumStandardMips;
		const UINT firstResidentMip = standardMipCount > 2 ? standardMipCount - 2 : 0;
		const UINT firstResidentTile = firstResidentMip < standardMipCount
			? tilings[firstResidentMip].StartTileIndexInOverallResource
			: packed.StartTileIndexInOverallResource;
		const UINT residentTileCount = totalTiles - firstResidentTile;
		ComPtr<ID3D12Heap> residentHeap = CreateTextureTileHeap(device, residentTileCount);
		if (!residentHeap) return nullptr;

		ID3D12CommandQueue* queue = RendererCore::GetCommandQueue();
		D3D12_TILED_RESOURCE_COORDINATE residentCoordinate{};
		residentCoordinate.Subresource = firstResidentMip;
		D3D12_TILE_REGION_SIZE residentRegion{};
		residentRegion.NumTiles = residentTileCount;
		D3D12_TILE_RANGE_FLAGS residentFlag = D3D12_TILE_RANGE_FLAG_NONE;
		UINT residentOffset = 0;
		queue->UpdateTileMappings(
			texture.Get(), 1, &residentCoordinate, &residentRegion,
			residentHeap.Get(), 1, &residentFlag, &residentOffset, &residentTileCount,
			D3D12_TILE_MAPPING_FLAG_NONE);

		// Until a detailed mip arrives, map it to the first resident tile.  The SRV
		// remains immutable while progressive remaps replace these fallback tiles.
		for (UINT mip = 0; mip < firstResidentMip; ++mip)
		{
			const UINT mipTileCount = tilings[mip].WidthInTiles *
				tilings[mip].HeightInTiles * tilings[mip].DepthInTiles;
			if (mipTileCount == 0) continue;
			D3D12_TILED_RESOURCE_COORDINATE coordinate{};
			coordinate.Subresource = mip;
			D3D12_TILE_REGION_SIZE region{};
			region.NumTiles = mipTileCount;
			D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
			UINT heapOffset = 0;
			queue->UpdateTileMappings(
				texture.Get(), 1, &coordinate, &region,
				residentHeap.Get(), 1, &flag, &heapOffset, &mipTileCount,
				D3D12_TILE_MAPPING_FLAG_NONE);
		}

		const UINT uploadCount = static_cast<UINT>(metadata.mipLevels) - firstResidentMip;
		vector<D3D12_SUBRESOURCE_DATA> subresources(uploadCount);
		for (UINT i = 0; i < uploadCount; ++i)
		{
			const Image* mipImage = image.GetImage(firstResidentMip + i, 0, 0);
			if (!mipImage) return nullptr;
			subresources[i] = { mipImage->pixels, static_cast<LONG_PTR>(mipImage->rowPitch), static_cast<LONG_PTR>(mipImage->slicePitch) };
		}
		const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), firstResidentMip, uploadCount);
		ComPtr<ID3D12Resource> upload = TextureManager::AcquireUploadBuffer(device, uploadSize);
		if (!upload) return nullptr;

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
				FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&ownedList))))
			{
				return nullptr;
			}
			commandList = ownedList.Get();
		}
		if (!commandList) return nullptr;
		UpdateSubresources(commandList, texture.Get(), upload.Get(), 0, firstResidentMip, uploadCount, subresources.data());
		auto toShader = CD3DX12_RESOURCE_BARRIER::Transition(
			texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &toShader);
		if (batch)
		{
			TextureManager::ReleaseUploadBuffer(upload, uploadSize, true);
		}
		else
		{
			if (!TextureManager::ExecuteCommandListAndSync(commandList)) return nullptr;
			TextureManager::ReleaseUploadBuffer(upload, uploadSize, false);
		}

		auto streaming = make_shared<TextureStreamingState>();
		streaming->MostDetailedResidentMip = firstResidentMip;
		streaming->StandardMipCount = standardMipCount;
		streaming->TotalMipCount = static_cast<UINT>(metadata.mipLevels);
		streaming->LastTouchedFrame = g_StreamingFrame;
		streaming->TileHeaps.push_back(residentHeap);
		streaming->Mips.resize(metadata.mipLevels);
		for (UINT mip = 0; mip < metadata.mipLevels; ++mip)
		{
			const Image* mipImage = image.GetImage(mip, 0, 0);
			if (!mipImage) continue;
			auto& destination = streaming->Mips[mip];
			destination.Pixels.assign(mipImage->pixels, mipImage->pixels + mipImage->slicePitch);
			destination.RowPitch = static_cast<LONG_PTR>(mipImage->rowPitch);
			destination.SlicePitch = static_cast<LONG_PTR>(mipImage->slicePitch);
			if (mip < standardMipCount)
			{
				destination.TileCount = tilings[mip].WidthInTiles *
					tilings[mip].HeightInTiles * tilings[mip].DepthInTiles;
			}
		}
		outStreaming = move(streaming);
		return texture;
	}

	ComPtr<ID3D12Resource> LoadTextureToDevice(ID3D12Device* device, const TexMetadata& metadata, const ScratchImage& image)
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

		auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		HRESULT hr = device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&texture));

		if (FAILED(hr))
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
		device->GetCopyableFootprints(&desc, 0, static_cast<UINT>(numSubresources), 0, nullptr, nullptr, nullptr, &uploadBufferSize);

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
			hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
			if (FAILED(hr))
			{
				Debug::Log("ERROR: CreateCommandList failed (HRESULT: 0x%08X)\n", hr);
				return nullptr;
			}
		}

		UpdateSubresources(cmdList.Get(), texture.Get(), uploadBuffer.Get(), 0, 0,
			static_cast<UINT>(numSubresources), subresources.data());

		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
	HRESULT hr = device->CreateCommittedResource(
		&uploadHeapProps,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadBuffer));

	if (FAILED(hr))
	{
		Debug::Log("ERROR: AcquireUploadBuffer CreateCommittedResource failed\n");
		return nullptr;
	}
	return uploadBuffer;
}

void TextureManager::ReleaseUploadBuffer(ComPtr<ID3D12Resource> upload, UINT64 size, bool defer)
{
	if (!upload) return;
	lock_guard<mutex> lock(g_UploadMutex);
	if (defer)
	{
		g_DeferredUploadReturns.push_back({ upload, size });
	}
	else
	{
		g_UploadPool.push_back({ upload, size });
	}
}

bool TextureManager::ExecuteCommandListAndSync(ID3D12GraphicsCommandList* cmdList, bool closeList)
{
	if (!cmdList) return false;
	if (closeList)
	{
		HRESULT hr = cmdList->Close();
		if (FAILED(hr))
		{
			Debug::Log("ERROR: CommandList Close failed (HRESULT: 0x%08X)\n", hr);
			return false;
		}
	}

	ID3D12CommandQueue* queue = RendererCore::GetCommandQueue();
	ID3D12CommandList* ppCmdLists[] = { cmdList };
	queue->ExecuteCommandLists(1, ppCmdLists);

	ID3D12Device* device = RendererCore::GetDevice();
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
	if (g_IsBatchLoading) return;
	ID3D12Device* device = RendererCore::GetDevice();

	if (!g_BatchCmdAlloc)
	{
		HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_BatchCmdAlloc));
		if (FAILED(hr)) { Debug::Log("ERROR: BeginTextureLoading CreateCommandAllocator failed\n"); return; }
	}
	else
	{
		HRESULT hr = g_BatchCmdAlloc->Reset();
		if (FAILED(hr)) { Debug::Log("ERROR: BeginTextureLoading Reset allocator failed\n"); return; }
	}

	if (!g_BatchCmdList)
	{
		HRESULT hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_BatchCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&g_BatchCmdList));
		if (FAILED(hr)) { Debug::Log("ERROR: BeginTextureLoading CreateCommandList failed\n"); return; }
	}
	else
	{
		HRESULT hr = g_BatchCmdList->Reset(g_BatchCmdAlloc.Get(), nullptr);
		if (FAILED(hr)) { Debug::Log("ERROR: BeginTextureLoading Reset command list failed\n"); return; }
	}

	g_IsBatchLoading = true;
}

void TextureManager::EndTextureLoading()
{
	if (!g_IsBatchLoading) return;

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
			decodeFutures.push_back(async(launch::deferred, [f = move(p->Future)]() mutable { f.wait(); }));
		}
	}
	for (auto& f : decodeFutures)
	{
		f.get();
	}

	ID3D12Device* device = RendererCore::GetDevice();
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

		TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), d.SrvIndex);
		td.Resource = resource;
		td.Streaming = move(streaming);
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

	{
		lock_guard<mutex> lock(g_UploadMutex);
		for (auto& e : g_DeferredUploadReturns) g_UploadPool.push_back(e);
		g_DeferredUploadReturns.clear();
	}

	g_IsBatchLoading = false;
}

ComPtr<ID3D12Resource> TextureManager::CreateSolidColorTexture(ID3D12Device* device, uint32_t color, int srvIndex)
{
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = 1;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	ComPtr<ID3D12Resource> texture;
	auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
	if (FAILED(hr) || !texture)
	{
		Debug::Log("ERROR: Failed to create solid color texture resource\n");
		return nullptr;
	}

	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
	if (uploadSize < sizeof(uint32_t)) uploadSize = 256;
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
		if (FAILED(hr)) { Debug::Log("ERROR: CreateCommandAllocator failed (solid color)\n"); return nullptr; }
		hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
		if (FAILED(hr)) { Debug::Log("ERROR: CreateCommandList failed (solid color)\n"); return nullptr; }
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
	device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);

	CD3DX12_TEXTURE_COPY_LOCATION dst(texture.Get(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION src(uploadBuffer.Get(), footprint);
	cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
	m_NextSrvIndex = RendererResource::g_kTEXTURE_SRV_START_INDEX;
	ID3D12Device* device = RendererCore::GetDevice();

	int whiteIndex = m_NextSrvIndex++;
	int pinkIndex = m_NextSrvIndex++;

	m_DefaultTexture = CreateSolidColorTexture(device, 0xFFFFFFFF, whiteIndex);
	m_ErrorTexture = CreateSolidColorTexture(device, 0xFFFF00FF, pinkIndex);
	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	g_ReservedResourcesSupported = SUCCEEDED(device->CheckFeatureSupport(
		D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
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
	m_Textures.clear();
	m_DefaultTexture.Reset();
	m_ErrorTexture.Reset();
	g_StreamingUploads.clear();
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

bool TextureManager::CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_RESOURCE_DESC& desc, int srvIndex)
{
	ID3D12Device* device = RendererCore::GetDevice();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = g_MaterialMipMapsEnabled ? desc.MipLevels : 1;

	UINT incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE handle(RendererResource::GetCbvHeap()->GetCPUDescriptorHandleForHeapStart());
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

int TextureManager::LoadTexture(const filesystem::path& fileName)
{
	if (fileName.empty())
	{
		Debug::Log("ERROR: Texture path is empty.\n");
		return GetErrorTextureIndex();
	}

	const string key = MakeTextureCacheKey(fileName);
	const string displayPath = PathToUtf8(fileName);
	{
		lock_guard<mutex> lock(g_TextureMutex);
		auto it = m_Textures.find(key);
		if (it != m_Textures.end())
		{
			return it->second.SrvIndex;
		}
	}

	const int maxSrvIndex = RendererResource::g_kTEXTURE_SRV_START_INDEX + RendererResource::g_kMAX_SRVS;
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
		entry->SrvIndex = srvIndex;
		entry->Future = entry->Promise.get_future();

		wstring pathString = fileName.wstring();
		thread([entry, pathString]() mutable {
			TexMetadata metadata;
			auto image = make_unique<ScratchImage>();
			HRESULT hr = LoadImageFromFile(pathString.c_str(), metadata, *image);
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

	HRESULT hr = LoadImageFromFile(fileName.c_str(), metadata, image);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: Failed to load texture: %s (HRESULT: 0x%08X)\n", displayPath.c_str(), hr);
		return GetErrorTextureIndex();
	}
	EnsureMipChain(metadata, image);

	ID3D12Device* device = RendererCore::GetDevice();
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

	TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), srvIndex);

	TextureData tData;
	tData.Resource = resource;
	tData.SrvIndex = srvIndex;
	tData.Streaming = move(streaming);
	{
		lock_guard<mutex> lock(g_TextureMutex);
		m_Textures.emplace(key, tData);
	}

	return srvIndex;
}

int TextureManager::LoadNormalTexture(const char* fileName)
{
	return LoadTexture(fileName);
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

	const int maxSrvIndex = RendererResource::g_kTEXTURE_SRV_START_INDEX + RendererResource::g_kMAX_SRVS;
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
	HRESULT hr = LoadImageFromMemory(pData, dataSize, metadata, image);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: Failed to load texture from memory: %s (HRESULT: 0x%08X)\n", name, hr);
		return GetErrorTextureIndex();
	}
	EnsureMipChain(metadata, image);

	ID3D12Device* device = RendererCore::GetDevice();
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

	TextureManager::CreateShaderResourceView(resource.Get(), resource->GetDesc(), srvIndex);

	TextureData tData;
	tData.Resource = resource;
	tData.SrvIndex = srvIndex;
	tData.Streaming = move(streaming);
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
		TextureInfo info;
		info.Path = pair.first;
		info.SrvIndex = pair.second.SrvIndex;
		if (pair.second.Resource)
		{
			D3D12_RESOURCE_DESC desc = pair.second.Resource->GetDesc();
			info.Width = static_cast<UINT>(desc.Width);
			info.Height = desc.Height;
		}
		infos.push_back(info);
	}
	sort(infos.begin(), infos.end(), [](const TextureInfo& a, const TextureInfo& b)
		{
			return a.Path < b.Path;
		});
	return infos;
}

void TextureManager::TouchTexture(int srvIndex)
{
	if (srvIndex < 0) return;
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

void TextureManager::UpdateStreaming(ID3D12GraphicsCommandList* commandList)
{
	if (!g_ReservedResourceStreamingSafe ||
		!commandList || !g_ReservedResourcesSupported ||
		!RendererSettings::GetTextureStreamingEnabled() ||
		!RendererSettings::GetReservedResourcesEnabled())
	{
		return;
	}
	++g_StreamingFrame;

	for (auto it = g_StreamingUploads.begin(); it != g_StreamingUploads.end();)
	{
		if (it->RetireFrame <= g_StreamingFrame)
		{
			ReleaseUploadBuffer(it->Resource, it->Size, false);
			it = g_StreamingUploads.erase(it);
		}
		else
		{
			++it;
		}
	}

	lock_guard<mutex> lock(g_TextureMutex);
	for (auto& pair : m_Textures)
	{
		auto& texture = pair.second;
		auto state = texture.Streaming;
		if (!state || !texture.Resource || state->MostDetailedResidentMip == 0 ||
			g_StreamingFrame > state->LastTouchedFrame + 30)
		{
			continue;
		}

		const UINT mip = state->MostDetailedResidentMip - 1;
		if (mip >= state->Mips.size()) continue;
		auto& mipData = state->Mips[mip];
		if (mipData.Pixels.empty() || mipData.TileCount == 0) continue;

		ComPtr<ID3D12Heap> mipHeap = CreateTextureTileHeap(RendererCore::GetDevice(), mipData.TileCount);
		if (!mipHeap) continue;
		D3D12_TILED_RESOURCE_COORDINATE coordinate{};
		coordinate.Subresource = mip;
		D3D12_TILE_REGION_SIZE region{};
		region.NumTiles = mipData.TileCount;
		D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
		UINT heapOffset = 0;
		RendererCore::GetCommandQueue()->UpdateTileMappings(
			texture.Resource.Get(), 1, &coordinate, &region,
			mipHeap.Get(), 1, &rangeFlag, &heapOffset, &mipData.TileCount,
			D3D12_TILE_MAPPING_FLAG_NONE);

		const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Resource.Get(), mip, 1);
		ComPtr<ID3D12Resource> upload = AcquireUploadBuffer(RendererCore::GetDevice(), uploadSize);
		if (!upload) continue;
		D3D12_SUBRESOURCE_DATA subresource{};
		subresource.pData = mipData.Pixels.data();
		subresource.RowPitch = mipData.RowPitch;
		subresource.SlicePitch = mipData.SlicePitch;
		auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
			texture.Resource.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST);
		commandList->ResourceBarrier(1, &toCopy);
		UpdateSubresources(commandList, texture.Resource.Get(), upload.Get(), 0, mip, 1, &subresource);
		auto toShader = CD3DX12_RESOURCE_BARRIER::Transition(
			texture.Resource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &toShader);

		state->TileHeaps.push_back(move(mipHeap));
		state->MostDetailedResidentMip = mip;
		g_StreamingUploads.push_back({ move(upload), uploadSize, g_StreamingFrame + 4 });
		// One detailed mip per frame limits both upload bandwidth and tile-map churn.
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

int TextureManager::LoadTextureUV(const char* fileName, SpriteComponent& sprite, const XMFLOAT4& uvRect)
{
	sprite.UseUvTransform = true;
	sprite.UvOffset = { uvRect.x, uvRect.y };
	sprite.UvScale = { uvRect.z, uvRect.w };
	return LoadTexture(fileName);
}

