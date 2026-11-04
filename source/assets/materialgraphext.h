#pragma once
// ============================================================================
// 追加ノード 33 種の CPU 評価（エディターのノードプレビュー / ベイク）。MaterialGraphEval::Evaluator から呼ばれる
//  ・式は materialgraphnodes.h（純粋関数）と shader/hlsl/MaterialGraphNodesExt.hlsli（GPU）に揃えてある
//  ・Material Function Call / Texture Parameter 2D は評価の前に materialgraphlower.h でインライン展開 / 解決する
// ============================================================================
#include "materialgraphevaluator.h"
#include <string>

namespace MaterialGraphExt
{
    struct Result
    {
        MaterialGraphEval::Value Primary;
        MaterialGraphEval::Value Slots[8];
        int SlotCount = 0;
    };

    // inputs = 全入力ピンの値（未接続のピンは定義の既定値 / 特別な既定入力）。linkedMask の bit i = 入力 i が接続されている
    // false = このノードは CPU では評価できない（Function Call が未展開など）。result は 0 のまま
    bool Evaluate(const MaterialGraphNode& node, const MaterialGraphEval::Context& context,
                  const MaterialGraphEval::TextureSampler& sampler, const MaterialGraphEval::Value* inputs, size_t inputCount,
                  uint32_t linkedMask, Result& result);

    // プレビュー用の擬似スペクトル（音声が無いときのエディター表示）。バンド 0..kAudioBands-1、0〜1
    float SyntheticSpectrum(int band, float time);
    // 周波数（Hz）→ バンド番号（20Hz〜20kHz の対数。Audio Spectrum の Value[0] == 1 のとき）
    float HzToBand(float hz);
    float BandToHz(float band);

    // Custom HLSL：入力の次元から型検査した結果（キャッシュ付き）。エラーは Diagnostics が集める
    struct CustomCompile
    {
        std::shared_ptr<const void> Keep; // Program を保持
        bool Ok = false;
        std::string Error;
    };
} // namespace MaterialGraphExt
