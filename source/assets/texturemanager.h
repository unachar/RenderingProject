#pragma once
#include "main.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <future>
#include <filesystem>
#include "texturecompression.h"

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
        int64_t SourceTime = 0;
        uint64_t CompressionCacheKey = 0;
    };

    static unordered_map<string, TextureData> m_Textures;
    static int m_NextSrvIndex;
    static ComPtr<ID3D12Resource> m_DefaultTexture;
    static ComPtr<ID3D12Resource> m_ErrorTexture;

    static bool CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_RESOURCE_DESC& desc, int srvIndex,
                                         UINT minimumMip = 0);
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
    static int LoadTexture(const filesystem::path& fileName);
    // 用途別に BC 圧縮（Color=BC7 / Normal=BC5 / TwoChannel=BC5 / SingleChannel=BC4）して読み込む。
    static int LoadTexture(const filesystem::path& fileName, TextureUsage usage);
    static int LoadNormalTexture(const char* fileName);
    // 線形の角度別配光テクスチャ。読込失敗時は -1 を返す。
    static int LoadIesProfile(const filesystem::path& fileName);
    static uint64_t WriteIesProfileViews(D3D12_CPU_DESCRIPTOR_HANDLE destination, uint64_t previousRevision);
    static int LoadTextureFromMemory(const char* name, const uint8_t* pData, size_t dataSize);
    // Registers an externally created texture (e.g. a runtime-generated
    // probe prefilter result) in the shared SRV heap under `name` and
    // returns its SRV index, or the error texture index on failure.
    // Re-registering the same name returns the existing index.
    static int RegisterExternalTexture(const char* name, ComPtr<ID3D12Resource> resource);
    // Like RegisterExternalTexture, but when the name is already
    // registered the SRV slot is re-pointed at the new resource
    // (same index) and the previous resource is retired until the
    // submitted-frame fence passes. For producers that refresh
    // their output in place (live probe captures).
    static int ReplaceExternalTexture(const char* name, ComPtr<ID3D12Resource> resource);
    static int GetDefaultTextureIndex();
    static ComPtr<ID3D12Resource> GetTextureResource(int srvIndex);
    static UINT GetTextureResidentMip(int srvIndex);
    static int GetErrorTextureIndex();
    static bool GetTextureSize(int srvIndex, UINT& outWidth, UINT& outHeight);
    static vector<TextureInfo> GetLoadedTextureInfos();
    static int FindLoadedTexture(const filesystem::path& path);
    static void TouchTexture(int srvIndex);
    static void UpdateStreaming(ID3D12GraphicsCommandList* commandList);
    static void OnFrameSubmitted(uint64_t fence);
    static bool IsReservedResourceStreamingSupported();
    static bool IsReservedResourceStreamingAvailable();

    static void BeginTextureLoading();
    static void EndTextureLoading();

    struct ScopedBatch
    {
        ScopedBatch()
        {
            TextureManager::BeginTextureLoading();
        }

        ~ScopedBatch()
        {
            TextureManager::EndTextureLoading();
        }
    };

    static bool IsBatchLoading();
    static ID3D12GraphicsCommandList* GetBatchCommandList();
    static ComPtr<ID3D12Resource> AcquireUploadBuffer(ID3D12Device* device, UINT64 size);
    static void ReleaseUploadBuffer(ComPtr<ID3D12Resource> upload, UINT64 size, bool defer = false);
    static bool ExecuteCommandListAndSync(ID3D12GraphicsCommandList* cmdList, bool closeList = true);
};
