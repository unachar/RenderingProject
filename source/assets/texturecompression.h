#pragma once

// テクスチャ取り込み時の BC 圧縮とディスクキャッシュ。
//   Color       : BC7（sRGB 入力は BC7_UNORM_SRGB）   ベースカラー / エミッシブ / モデル埋め込み
//   Normal      : BC5（RG のみ保持。Z はシェーダーで再構成: NormalMapDecode.hlsli）
//   TwoChannel  : BC5（R=Metallic, G=Roughness。シェーダーは .rg のみ参照）
//   SingleChannel: BC4（AO / 高さ。シェーダーは .r のみ参照）
// 圧縮結果は <作業ディレクトリ>/Data/Cache/Textures/<hash>.dds に保存し、ソースのパス・サイズ・更新時刻・
// 用途・エンコーダー版が一致すれば再利用する。

#include <cstdint>
#include <filesystem>
#include <string>

namespace DirectX
{
    struct TexMetadata;
    class ScratchImage;
} // namespace DirectX

enum class TextureUsage : uint32_t
{
    Color = 0,
    Normal = 1,
    TwoChannel = 2,
    SingleChannel = 3,
};

namespace TextureCompression
{
    struct Settings
    {
        bool Enabled = true;
        // 未キャッシュのテクスチャはその場では圧縮せず非圧縮で使い、バックグラウンドでキャッシュを作る
        // （次回読み込みから BC）。偽にすると読み込み時に圧縮する（初回の読み込みが遅くなる）。
        bool BackgroundForSynchronousLoads = true;
    };

    struct Statistics
    {
        uint32_t CacheHits = 0;
        uint32_t Compressed = 0;
        uint32_t Skipped = 0;
        uint32_t BackgroundPending = 0;
        uint64_t SourceBytes = 0;
        uint64_t CompressedBytes = 0;
    };

    Settings& GetSettings();
    Statistics GetStatistics();

    const char* UsageName(TextureUsage usage);
    // m_Textures のキーへ付ける接尾辞（Color は空 = 従来キーのまま）。
    std::string KeySuffix(TextureUsage usage);
    // キーから接尾辞を取り除いたパス部分。
    std::string StripKeySuffix(const std::string& key);

    std::filesystem::path CacheDirectory();
    // ソースファイル用キャッシュキー（パス・サイズ・更新時刻・用途・エンコーダー版）。0 は失敗。
    uint64_t FileCacheKey(const std::filesystem::path& source, TextureUsage usage);
    // メモリ上の画像（モデル埋め込み）用キャッシュキー（内容・用途・エンコーダー版）。
    uint64_t MemoryCacheKey(const void* data, size_t size, TextureUsage usage);

    // キャッシュ済み BC テクスチャを読み込む。
    bool TryLoadCached(uint64_t cacheKey, DirectX::TexMetadata& metadata, DirectX::ScratchImage& image);
    // BC 圧縮の対象になる画像か（4 の倍数サイズの 2D・非 HDR・未圧縮）。
    bool IsCompressible(const DirectX::TexMetadata& metadata);
    // ミップ生成済みの画像をその場で BC 圧縮し、キャッシュへ保存する。圧縮対象外なら false（画像は不変）。
    bool CompressAndCache(uint64_t cacheKey, TextureUsage usage, DirectX::TexMetadata& metadata,
                          DirectX::ScratchImage& image);
    // バックグラウンドでソースファイルを読み込み・ミップ生成・圧縮・キャッシュ保存する。
    void CompressFileInBackground(uint64_t cacheKey, TextureUsage usage, const std::filesystem::path& source);
    void CompressMemoryInBackground(uint64_t cacheKey, TextureUsage usage, const void* data, size_t size);
} // namespace TextureCompression
