#pragma once
// ============================================================================
// Material Function（再利用できる関数グラフ）と、コンパイル前の展開（2026-11）
//  ・関数 = MaterialGraph（Settings.IsFunction = true）。Function Input / Function Output で入出力を定義する
//  ・呼び出し（Material Function Call）はコンパイル / 評価の前にインライン展開する（Lower）。
//    展開後のグラフは通常のノードだけなので、CPU 評価 / HLSL 生成 / GPU VM が追加の実装なしで同じ結果になる
//  ・再帰参照 / 循環依存は ValidateReferences が検出する。展開の深さ上限は 16
//  ・Texture Parameter 2D は、マテリアルインスタンスの上書き（MaterialGraph::InstanceTextures）を解決して Texture Sample にする
// ============================================================================
#include "materialgraph.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace MaterialFunctionLibrary
{
    struct Port
    {
        std::string Name;
        int Dims = 1;
        std::array<float, 4> Default{};
        uint64_t Node = 0;
    };
    struct Signature
    {
        std::vector<Port> Inputs;
        std::vector<Port> Outputs;
    };

    using Loader = std::function<std::shared_ptr<const MaterialGraph>(AssetHandle)>;
    // エンジンが登録する（アセットのパスから .matfunc を読む）。テストは Register で直接置く
    void SetLoader(Loader loader);
    void Register(AssetHandle handle, std::shared_ptr<const MaterialGraph> graph);
    void Unregister(AssetHandle handle);
    void Clear();
    std::shared_ptr<const MaterialGraph> Find(AssetHandle handle);
    // 関数の定義が変わった（保存した）ときに呼ぶ。キャッシュを捨てる
    void Invalidate(AssetHandle handle);
    uint64_t Revision(); // Register / Invalidate / Clear のたびに増える（マテリアル側の再コンパイル判定用）

    // 関数のシグネチャ（入力 / 出力）。番号の重複 / 抜け / 型の不一致（Function Output の宣言と実際の型）を検査する
    bool GetSignature(const MaterialGraph& function, Signature& signature, std::string& error);
    // 呼び出しノードにシグネチャのキャッシュ（Params / PinNames）を書き込む。関数を選んだとき / 関数を保存したときに呼ぶ
    bool RefreshCall(MaterialGraphNode& call, std::string& error);
    // 呼び出しの接続の型を検査する（スカラーは広がる。成分数が足りない接続は不可）
    bool ValidateCall(const MaterialGraph& graph, const MaterialGraphNode& call, std::string& error);
    // graph（handle = そのグラフ自身の関数アセット。0 = マテリアル）から辿れる関数が循環しないか
    bool ValidateReferences(const MaterialGraph& graph, AssetHandle self, std::string& error);
} // namespace MaterialFunctionLibrary

namespace MaterialGraphLower
{
    // 展開が必要か（Function Call / 解決できる Texture Parameter 2D がある）
    bool NeedsLowering(const MaterialGraph& graph);
    // 展開後のグラフを作る。ノード ID は元のまま（Function Call のノードは 1 本の Reroute になり、最初の出力を流す）
    bool Lower(const MaterialGraph& source, MaterialGraph& lowered, std::string& error);
    // 必要なら展開して返す。失敗したら error を入れて false。不要なら source のコピーを避けて同じ参照を返す
    const MaterialGraph* Prepare(const MaterialGraph& source, MaterialGraph& storage, std::string& error);

    // エディターのエラー / 警告表示用の診断（ノードごと）
    struct Diagnostic
    {
        uint64_t Node = 0;
        std::string Message;
        bool Warning = false;
    };
    std::vector<Diagnostic> Diagnose(const MaterialGraph& graph, AssetHandle self = 0);

    // 描画時評価（GPU）が使うリソースの依存（Scene Depth / Scene Color / 動的入力）
    struct Dependencies
    {
        bool SceneDepth = false;
        bool SceneColor = false;
        bool FrontFacing = false;
        bool Curvature = false;
        bool Audio = false;
        bool Beat = false;
        bool Time = false;
        bool PerLightToon = false; // Toon の陰影ピンが接続されている（ライトごとの評価が要る）
        bool CustomHlsl = false;
        int DynamicSwitchCount = 0;
        int StaticSwitchFolded = 0;
    };
    Dependencies AnalyzeDependencies(const MaterialGraph& graph);
} // namespace MaterialGraphLower
