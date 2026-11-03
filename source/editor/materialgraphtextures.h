#pragma once
// ============================================================================
// MaterialGraphTextures：マテリアルグラフの CPU 評価用テクスチャ
//  ・プレビュー：Texture Sample ノードのテクスチャを別スレッドで読み込み、最大 256px に縮小して保持する
//  ・ベイク：LoadForBake でフル解像度（上限指定）の画像をワーカースレッドから同期で読む
//  ・読み込みが終わるたびに Generation() が増える（プレビューの再評価に使う）
// ============================================================================
#include "materialasset.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace MaterialGraphTextures
{
    struct CpuTexture
    {
        int Width = 0;
        int Height = 0;
        std::vector<float> Pixels; // RGBA 線形
        bool SrgbEncoded = false;  // 8 ビット画像（sRGB として線形化済み）。raw 取得時は sRGB に戻す
    };

    // 線形 RGBA を返す。未読み込み（読み込み中 / 失敗）なら false。初回呼び出しで読み込みを開始する
    bool Sample(AssetHandle texture, float u, float v, float out[4]);
    // raw = true なら格納値（法線マップ / マスク用）
    bool Sample(AssetHandle texture, float u, float v, bool raw, float out[4]);
    bool IsReady(AssetHandle texture);
    uint64_t Generation();
    void Clear();
    // ファイルを書き換えたテクスチャ（ベイク結果など）を次回アクセス時に読み直す
    void Invalidate(AssetHandle texture);

    // ベイク用：ワーカースレッドで呼ぶ（COM はこの関数内で初期化する）。maxSize で長辺を制限する
    std::shared_ptr<const CpuTexture> LoadForBake(const std::filesystem::path& path, int maxSize);
    // 折り返し + 双線形補間（raw の扱いは Sample と同じ）
    void SampleTexture(const CpuTexture& texture, float u, float v, bool raw, float out[4]);
} // namespace MaterialGraphTextures
