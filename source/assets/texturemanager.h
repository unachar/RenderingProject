#pragma once
#include "main.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <array>
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
    static int GetDefaultTextureIndex();
    static ComPtr<ID3D12Resource> GetTextureResource(int srvIndex);
    static UINT GetTextureResidentMip(int srvIndex);
    static int GetErrorTextureIndex();
    static bool GetTextureSize(int srvIndex, UINT& outWidth, UINT& outHeight);
    static vector<TextureInfo> GetLoadedTextureInfos();
    static int FindLoadedTexture(const filesystem::path& path);
    static void TouchTexture(int srvIndex);
    // ファイルを書き換えたテクスチャ（マテリアルグラフのベイク結果など）を、同じ SRV 番号のまま読み直す。
    // 実際の差し替えは次の UpdateStreaming（フレーム先頭、GPU アイドル待ち後）で行う。未読み込みなら何もしない。
    static void RequestReload(const filesystem::path& path);
    // 読み込み済みテクスチャ（path）の中身を、同じ SRV 番号のまま CPU のピクセルで上書きする（マテリアルグラフのライブ更新）。
    // 初回とサイズ / 形式が変わったときだけ GPU アイドル待ちで非圧縮の動的リソースへ差し替え、以降は毎フレームのコピーのみ。
    // pixels：RGBA、行の詰めなし。bytesPerChannel = 1（8 ビット）/ 2（16 ビット UNORM）。
    // srgbEncoded = true の 8 ビットは sRGB 値（元のテクスチャが sRGB 形式なら _SRGB で作る）。どのスレッドからでも呼べる
    // ---- データテクスチャ（R32G32B32A32_FLOAT、高さ 1）。マテリアルグラフの描画時評価プログラムに使う
    // テクスチャ表の SRV を 1 つ確保して返す（失敗は -1）。中身は UpdateDataTexture で書き込む
    static int CreateDataTexture(const string& name, int width);
    // texels を firstTexel から書き込む（実際のコピーは次の UpdateStreaming。同じ範囲の古い要求は上書き）
    static void UpdateDataTexture(int srvIndex, uint32_t firstTexel, vector<array<float, 4>> texels);
    // 一度でも書き込みが記録されたか（シェーダーから読んでよいか）
    static bool IsDataTextureReady(int srvIndex);
    static void UpdateDynamicPixels(const filesystem::path& path, int width, int height, int bytesPerChannel,
                                    bool srgbEncoded, vector<uint8_t> pixels);
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



