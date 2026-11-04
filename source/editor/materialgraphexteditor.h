#pragma once
// ============================================================================
// 追加マテリアルノード 33 種のノード内エディター UI（ランプ / カーブ / テクスチャ / Custom HLSL / Material Function）
//  ・materialgrapheditor.cpp の GraphDocument / imgui-node-editor に依存しない（EditHost 経由）。単体で型チェックできる
//  ・編集は「BeginEdit（Undo 用のスナップショットを 1 回）→ 変更 → MarkChanged → EndEdit（ドラッグ 1 回 = Undo 1 回）」
// ============================================================================
#include "materialgraph.h"
#include <string>
#include <vector>

struct ImVec2;

namespace MaterialGraphExtEditor
{
    class EditHost
    {
      public:
        virtual ~EditHost() = default;
        virtual MaterialGraph& Graph() = 0;
        virtual void BeginEdit(uint64_t node) = 0; // まだなら Undo 用に現在のグラフを記録する
        virtual void MarkChanged() = 0;            // Revision / Dirty を更新する
        virtual void EndEdit(uint64_t node) = 0;   // 操作が終わっていれば Undo 履歴へ確定する
        virtual void Tooltip(const char* text) = 0;
        virtual float ScreenToCanvasX(float x) = 0;
        virtual float ScreenToCanvasY(float y) = 0;
        virtual std::string AssetFileName(AssetHandle handle) = 0;
        virtual bool IsAssetType(AssetHandle handle, AssetType type) = 0;
        virtual std::vector<AssetHandle> FunctionAssets() = 0;
        virtual AssetHandle CreateFunctionAsset() = 0; // 新しい .matfunc を作って登録する（失敗は 0）
        virtual void OpenFunctionAsset(AssetHandle handle) = 0;
    };

    // ノード内の編集 UI。追加ノード（IsExtKind）なら true（描画した）。それ以外は false
    bool DrawNodeEditor(EditHost& host, MaterialGraphNode& node, float width);
    // 入力ピンの表示名（Custom HLSL / Material Function はユーザー定義の名前）。nullptr = 定義の名前
    const char* PinLabel(const MaterialGraphNode& node, size_t input, std::string& storage);
    // ピンを薄く表示するか（Custom HLSL の未使用入力 / Function の存在しない入力）
    bool IsPinUnused(const MaterialGraphNode& node, size_t input);
    // 出力スロットの表示名（固定スロットを持つノード）。Material Function は関数の出力名
    const char* SlotLabel(const MaterialGraphNode& node, size_t slot, std::string& storage);
    // 追加メニューに出すか
    bool ShowInAddMenu(MaterialNodeKind kind, const MaterialGraph& graph);
} // namespace MaterialGraphExtEditor
