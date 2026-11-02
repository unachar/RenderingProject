#pragma once
// ============================================================================
// MaterialGraphPreview：マテリアルグラフのノードプレビュー（Unreal Engine のノードプレビュー風）
//  ・各ノードの出力を CPU で評価して画像にする（UV 平面、法線などに依存する場合は球）
//  ・Material Output は簡易 PBR で球にシェーディング（Normal / テクスチャも反映）
//  ・Texture Sample は実際のテクスチャを CPU に読み込んでサンプリングする
//  ・画像は ImGui 1.92 の ImTextureData で GPU に送る（未対応バックエンドでは矩形描画）
//  ・グラフの内容が変わったときだけ再評価。Time / Panner / Rotator を含むと 15Hz で更新
// ============================================================================
#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>
#include "materialasset.h"

class MaterialGraph;
struct MaterialGraphNode;

namespace MaterialGraphPreview
{
    // ノードの中（Nodes::BeginNode〜EndNode の間）で呼ぶ。width × height の領域を確保し、
    // 画像は縦横比を保ったまま中央に表示する（height <= 0 なら width と同じ）
    void DrawNodePreview(const MaterialGraph& graph, const MaterialGraphNode& node, float width, float height = 0.0f);
    bool HasPreview(const MaterialGraphNode& node);
    bool IsNodePreviewCollapsed(const MaterialGraph& graph, uint64_t node);
    void ToggleNodePreview(const MaterialGraph& graph, uint64_t node);

    // グラフ内容（ノード値・接続・テクスチャ）のハッシュ。Undo/Redo を含むあらゆる変更で値が変わる
    uint64_t ContentSignature(const MaterialGraph& graph);

    // グラフの Material Output をマテリアルアセット（エンジンが描画に使う PBR パラメーター）へ焼き込む。
    //  ・定数として評価できる入力（定数 / パラメーター / それらの演算）→ 数値
    //  ・Texture Sample（UV 未接続 / UV / TexCoord(1,1)）→ テクスチャスロット。Multiply(Texture, Color) も可
    //  ・UV / Fresnel / Time などピクセルごとに変わる式はエンジン側未対応のため反映しない（Notes に記録）
    struct BakeResult
    {
        MaterialAssetData Data;
        uint32_t ManagedSlots = 0;
        std::vector<std::string> Notes;
        bool HasOutput = false;
    };
    BakeResult BakeToMaterial(const MaterialGraph& graph, const MaterialAssetData& current, uint32_t managedSlots);

    // グラフドキュメントを破棄するときに呼ぶ（キャッシュと GPU 画像の解放）
    void Forget(const MaterialGraph& graph);
} // namespace MaterialGraphPreview
