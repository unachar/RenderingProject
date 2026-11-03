#pragma once
// ============================================================================
// MaterialGraphPreview：マテリアルグラフのノードプレビュー（Unreal Engine のノードプレビュー風）
//  ・各ノードの出力を GPU で評価して画像にする（MaterialGraphGpu。エンジンの描画と同じ命令列）。
//    GPU で評価できないノード（DDX / DDY / Normal From Height / Bump Offset）に依存するものだけ CPU で評価する
//  ・UV 平面、法線などに依存する場合は球
//  ・定数 / パラメーター / Time / オブジェクト・カメラの情報など、画像にしても意味の無いノードにはプレビューを出さない
//  ・Material Output は簡易 PBR で球にシェーディング（Normal / Normal Intensity / Height(POM) / テクスチャも反映）
//  ・Texture Sample は実際のテクスチャを CPU に読み込んでサンプリングする（Sampler Type 対応）
//  ・画像は ImGui 1.92 の ImTextureData で GPU に送る（未対応バックエンドでは矩形描画）
//  ・グラフの内容が変わったときだけ再評価。Time / Panner / Rotator を含むと 15Hz で更新
// ============================================================================
#include <imgui.h>
#include <array>
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

    // グラフ内容（ノード値・接続・テクスチャ・グラフ設定）のハッシュ。Undo/Redo を含むあらゆる変更で値が変わる
    uint64_t ContentSignature(const MaterialGraph& graph);

    // ベイク結果の BaseColor アルファに何を入れるか
    enum class AlphaSource : int
    {
        BaseColor,  // Base Color のアルファ
        Opacity,    // Opacity（半透明）
        OpacityMask // Opacity Mask（Masked のクリップ）
    };

    // ピクセルごとの式をテクスチャへ焼いた結果（MaterialGraphBake が作る）
    struct BakedTextures
    {
        uint64_t Signature = 0; // 焼いたときのグラフ内容（ContentSignature + 解像度 + 時刻）
        uint32_t Mask = 0;      // 焼いたスロット（bit = MaterialTextureSlot）
        std::array<AssetHandle, static_cast<size_t>(MaterialTextureSlot::Count)> Textures{};
        float EmissivePeak = 1.0f; // Emissive は最大値で正規化して保存している
    };

    // グラフの Material Output をマテリアルアセット（エンジンが描画に使うパラメーター）へ焼き込む。
    //  ・定数として評価できる入力（定数 / パラメーター / それらの演算）→ 数値
    //  ・Texture Sample（UV 未接続 / UV / TexCoord(1,1)）とその色乗算 / チャンネル抽出 → テクスチャスロットを直接使う
    //  ・それ以外のピクセルごとに変わる式（UV 演算 / Noise / Voronoi / Blend / Normal From Height など全ノード）
    //    → TextureBakeMask に記録。baked にあるテクスチャ（MaterialGraphBake の結果）を割り当てる
    //  ・Height / Normal Intensity / Parallax Depth / Steps / IOR / 透過 / 吸収 / シェーディング強度にも対応
    //  ・グラフ設定（Blend Mode / 両面 / Shading Model / 影 / クリップ値）で上書きする
    struct BakeResult
    {
        MaterialAssetData Data;
        uint32_t ManagedSlots = 0;
        std::vector<std::string> Notes;
        bool HasOutput = false;
        uint32_t TextureBakeMask = 0; // テクスチャへ焼く必要があるスロット
        AlphaSource Alpha = AlphaSource::BaseColor;
        bool BakeMissing = false; // 焼く必要があるのに、まだ結果が無いスロットがある
    };
    BakeResult BakeToMaterial(const MaterialGraph& graph, const MaterialAssetData& current, uint32_t managedSlots,
                              const BakedTextures* baked = nullptr);

    // グラフドキュメントを破棄するときに呼ぶ（キャッシュと GPU 画像の解放）
    void Forget(const MaterialGraph& graph);
} // namespace MaterialGraphPreview

