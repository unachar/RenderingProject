#pragma once
#include "materialasset.h"

class MaterialGraphEditor
{
  public:
    static void BeginFrame();
    static void Draw(AssetHandle material);
    // UE 風の左パネル：ライブプレビュー（グラフ）/ エンジン描画プレビュー、選択ノードのプレビュー
    static void DrawPreviewPanel(AssetHandle material);
    static bool HasFocus();
    static bool HasUnsavedChanges(AssetHandle material);
    static bool Save(AssetHandle material);
    static const char* LastError(AssetHandle material);
    static void Revert(AssetHandle material);
    static void Undo(bool redo);
    static void SaveRecovery();
    static void Shutdown();
};
