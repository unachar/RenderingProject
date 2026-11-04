#pragma once
// ============================================================================
// MaterialGraphEvaluator：マテリアルグラフの CPU 評価器（ImGui / D3D12 に依存しない）
//  ・ノードプレビュー（MaterialGraphPreview）とエンジン用テクスチャベイク（MaterialGraphBake）で共有する
//  ・全ノード対応。DDX / DDY / Normal From Height は UV を 1 画素ずらして上流を再評価する差分で求める
//  ・Texture Sample はサンプラー関数経由（プレビューは縮小キャッシュ、ベイクはフル解像度）
// ============================================================================
#include "materialgraph.h"
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MaterialGraphEval
{
    struct Value
    {
        float V[4]{};
        int N = 1; // 成分数（1〜4）
    };

    // 音声のスペクトル（Audio Spectrum ノード用）。エンジンは AudioMaterialBridge（audio/audiospectrum.h）から渡す。
    // 渡さないプレビューでは擬似スペクトルを使う
    struct AudioSnapshot
    {
        static constexpr int kBands = 32;
        float Bands[kBands]{}; // 0〜1（対数周波数 20Hz〜20kHz を kBands に分けた大きさ）
        double Time = 0.0;     // 音声クロック（秒）
    };

    // 評価コンテキスト。接空間は T = +U、B = +V（エンジンの ApplyNormalMapCommon と同じ向き）
    struct Context
    {
        float UV[2]{};
        float Position[3]{};
        float Normal[3]{0.0f, 0.0f, -1.0f};
        float Camera[3]{0.0f, 0.0f, -1.0f}; // 表面 → カメラ
        float Tangent[3]{1.0f, 0.0f, 0.0f};
        float Bitangent[3]{0.0f, -1.0f, 0.0f};
        float Time = 0.0f;
        float PixelSize = 1.0f / 64.0f; // UV 上の 1 画素（DDX / DDY / Normal From Height の差分幅）
        // ---- 2026-11 追加ノード用（無ければプレビュー用の値）
        float LightDirection[3]{0.40824829f, 0.81649658f, 0.40824829f}; // 表面 → メインライト
        float Curvature = 0.0f;                                         // 曲率（正 = 凸）
        bool FrontFacing = true;                                        // 表面（Two Sided Sign）
        float SceneDepth = 100.0f;                                      // 不透明の奥行き（ワールド単位。Scene Depth / Depth Fade）
        float PixelDepth = 3.0f;                                        // 画素の奥行き（ワールド単位）
        float SceneColor[3]{0.5f, 0.5f, 0.5f};
        float NearPlane = 0.1f;
        float FarPlane = 1000.0f;
        bool ReverseZ = true; // 深度バッファの逆 Z（Scene Depth の「深度値」出力）
        double AudioTime = 0.0;
        const AudioSnapshot* Audio = nullptr;
    };

    // raw = true なら格納値（8 ビット画像の sRGB 線形化をしない値）を返す。未読み込みなら false
    using TextureSampler = std::function<bool(AssetHandle texture, float u, float v, bool raw, float out[4])>;

    // 入力ピンの接続先（BuildTopology で 1 回だけ解決する。評価中は線形探索をしない）
    struct InputSource
    {
        int32_t Node = -1;   // 接続元ノードの添字（-1 = 未接続 / 見つからない）
        int32_t Output = 0;  // 接続元の出力（0 = 通常の出力、1〜4 = 分割した X / Y / Z / W）
        bool Linked = false; // リンクがある（MaterialGraph::InputLink と同じ判定。モードで無効なピンは false）
    };

    struct Topology
    {
        std::unordered_map<uint64_t, size_t> Index; // ノード ID → Nodes の添字
        std::vector<uint32_t> InputOffset;          // ノード i の入力は Inputs[InputOffset[i] + ピン]
        std::vector<InputSource> Inputs;
        std::vector<uint8_t> Spatial;               // 法線・位置・カメラに依存する
        std::vector<uint8_t> TimeDependent;         // Time に依存する
        std::vector<uint8_t> ViewDependent;         // カメラ方向に依存する（ベイクでは正面から見た値になる）
        bool Animated = false;
        // Material Function の呼び出し / Texture Parameter 2D を展開したグラフ（無ければ元のグラフをそのまま評価する）。
        // ノードの添字は元のグラフと同じ（展開で増えるノードは末尾）。Evaluator はこのグラフで評価する
        std::shared_ptr<const MaterialGraph> Lowered;
        std::string LowerError; // 展開に失敗した理由（空 = 成功 / 不要）
    };

    void BuildTopology(const MaterialGraph& sourceGraph, Topology& topology);

    // 中継ノードを飛ばして、入力ピンに実際に繋がっているノードを返す
    const MaterialGraphNode* ResolveSource(const MaterialGraph& graph, const MaterialGraphNode& node, size_t input);

    float LinearToSrgb(float v);
    float SrgbToLinear(float v);

    class Evaluator
    {
      public:
        Evaluator(const MaterialGraph& graph, const Topology& topology, TextureSampler sampler);
        ~Evaluator();
        Evaluator(const Evaluator&) = delete;
        Evaluator& operator=(const Evaluator&) = delete;

        void Reset(const Context& context);
        Value Node(size_t index, int depth = 0);
        Value Input(const MaterialGraphNode& node, size_t input, int depth = 0);
        // 入力ピンにリンクがあるか（Topology の表を使う。InputLink の線形探索より速い）
        bool Linked(const MaterialGraphNode& node, size_t input) const;
        const Context& Ctx() const
        {
            return m_Context;
        }

        // Material Output の入力を、指定の UV で評価する（POM / 高さの差分用）。コンテキストは元に戻らない
        Value InputAt(const MaterialGraphNode& node, size_t input, float u, float v);

        // 固定の出力スロット（Packed Texture Channels / Flow Map / Beat Phase / Material Layer Blend / Thickness Map）の値
        Value SlotValue(size_t node, int slot);

      private:
        Value ComputeExt(const MaterialGraphNode& node, size_t index, int depth);
        const InputSource* Source(const MaterialGraphNode& node, size_t input) const;
        Value Compute(const MaterialGraphNode& node, int depth);
        Value Shifted(const MaterialGraphNode& node, size_t input, int axis, int depth);
        int Gather(const MaterialGraphNode& node, int depth, Value* inputs, size_t count, size_t promoteCount);

        const MaterialGraph& m_Graph;
        const Topology& m_Topology;
        TextureSampler m_Sampler;
        Context m_Context;
        std::vector<Value> m_Memo;
        std::vector<uint8_t> m_Done;
        std::vector<std::array<Value, 8>> m_Slots; // 固定出力スロットを持つグラフだけ確保する
        bool m_HasSlots = false;
        std::unique_ptr<Evaluator> m_Child; // 差分用（UV をずらした評価）。必要になったときだけ作る
        int m_Level = 0;
    };

    // 値ユーティリティ
    Value Promote(Value value, int count);
} // namespace MaterialGraphEval


