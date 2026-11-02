#pragma once
// ============================================================================
// MaterialGraphTextures：マテリアルグラフの CPU プレビュー用テクスチャキャッシュ
//  ・Texture Sample ノードのテクスチャを別スレッドで読み込み、最大 256px に縮小して保持する
//  ・読み込みが終わるたびに Generation() が増える（プレビューの再評価に使う）
// ============================================================================
#include "materialasset.h"
#include <cstdint>

namespace MaterialGraphTextures
{
    // 線形 RGBA を返す。未読み込み（読み込み中 / 失敗）なら false。初回呼び出しで読み込みを開始する
    bool Sample(AssetHandle texture, float u, float v, float out[4]);
    bool IsReady(AssetHandle texture);
    uint64_t Generation();
    void Clear();
} // namespace MaterialGraphTextures
