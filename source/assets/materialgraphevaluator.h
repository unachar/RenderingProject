#pragma once
// ============================================================================
// MaterialGraphEvaluator：マテリアルグラフの CPU 評価器（ImGui / D3D12 に依存しない）
//  ・ノードプレビュー（MaterialGraphPreview）とエンジン用テクスチャベイク（MaterialGraphBake）で共有する
//  ・全ノード対応。DDX / DDY / Normal From Height は UV を 1 画素ずらして上流を再評価する差分で求める
//  ・Texture Sample はサンプラー関数経由（プレビューは縮小キャッシュ、ベイクはフル解像度）
// ============================================================================
#include "materialgraph.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace MaterialGraphEval
{
    struct Value
    {
        float V[4]{};
        int N = 1; // 成分数（1〜4）
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
    };

    // raw = true なら格納値（8 ビット画像の sRGB 線形化をしない値）を返す。未読み込みなら false
    using TextureSampler = std::function<bool(AssetHandle texture, float u, float v, bool raw, float out[4])>;

    struct Topology
    {
        std::unordered_map<uint64_t, size_t> Index; // ノード ID → Nodes の添字
        std::vector<uint8_t> Spatial;               // 法線・位置・カメラに依存する
        std::vector<uint8_t> TimeDependent;         // Time に依存する
        std::vector<uint8_t> ViewDependent;         // カメラ方向に依存する（ベイクでは正面から見た値になる）
        bool Animated = false;
    };

    void BuildTopology(const MaterialGraph& graph, Topology& topology);

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
        const Context& Ctx() const
        {
            return m_Context;
        }

        // Material Output の入力を、指定の UV で評価する（POM / 高さの差分用）。コンテキストは元に戻らない
        Value InputAt(const MaterialGraphNode& node, size_t input, float u, float v);

      private:
        Value Compute(const MaterialGraphNode& node, int depth);
        Value Shifted(const MaterialGraphNode& node, size_t input, int axis, int depth);
        int Gather(const MaterialGraphNode& node, int depth, Value* inputs, size_t count, size_t promoteCount);

        const MaterialGraph& m_Graph;
        const Topology& m_Topology;
        TextureSampler m_Sampler;
        Context m_Context;
        std::vector<Value> m_Memo;
        std::vector<uint8_t> m_Done;
        std::unique_ptr<Evaluator> m_Child; // 差分用（UV をずらした評価）。必要になったときだけ作る
        int m_Level = 0;
    };

    // 値ユーティリティ
    Value Promote(Value value, int count);
} // namespace MaterialGraphEval
