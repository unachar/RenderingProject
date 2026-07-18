#pragma once
#include "main.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <future>
#include <filesystem>

struct SpriteComponent;
struct TextureStreamingState;

class TextureManager
{
private:
	struct TextureData
	{
		ComPtr<ID3D12Resource> Resource{};
		int SrvIndex = -1;
		shared_ptr<TextureStreamingState> Streaming{};
	};

	static unordered_map<string, TextureData> m_Textures;
	static int m_NextSrvIndex;
	static ComPtr<ID3D12Resource> m_DefaultTexture;
	static ComPtr<ID3D12Resource> m_ErrorTexture;

	static bool CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_RESOURCE_DESC& desc, int srvIndex);
	static ComPtr<ID3D12Resource> CreateSolidColorTexture(ID3D12Device* device, uint32_t color, int srvIndex);

public:
	struct TextureInfo
	{
		string Path;
		int SrvIndex = -1;
		UINT Width = 0;
		UINT Height = 0;
	};

	static void Init();
	static void Uninit();

	static int LoadTexture(const char* fileName);
	static int LoadTexture(const std::filesystem::path& fileName);
	static int LoadNormalTexture(const char* fileName);
	static int LoadTextureFromMemory(const char* name, const uint8_t* pData, size_t dataSize);
	static int GetDefaultTextureIndex();
	static int GetErrorTextureIndex();
	static bool GetTextureSize(int srvIndex, UINT& outWidth, UINT& outHeight);
	static vector<TextureInfo> GetLoadedTextureInfos();
	static int LoadTextureUV(const char* fileName, SpriteComponent& sprite, const XMFLOAT4& uvRect);
	static void TouchTexture(int srvIndex);
	static void UpdateStreaming(ID3D12GraphicsCommandList* commandList);
	static bool IsReservedResourceStreamingSupported();
	static bool IsReservedResourceStreamingAvailable();

	static void BeginTextureLoading();
	static void EndTextureLoading();

	struct ScopedBatch
	{
		ScopedBatch() { TextureManager::BeginTextureLoading(); }
		~ScopedBatch() { TextureManager::EndTextureLoading(); }
	};

	static bool IsBatchLoading();
	static ID3D12GraphicsCommandList* GetBatchCommandList();
	static ComPtr<ID3D12Resource> AcquireUploadBuffer(ID3D12Device* device, UINT64 size);
	static void ReleaseUploadBuffer(ComPtr<ID3D12Resource> upload, UINT64 size, bool defer = false);
	static bool ExecuteCommandListAndSync(ID3D12GraphicsCommandList* cmdList, bool closeList = true);
};

