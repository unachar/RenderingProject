#pragma once
#include "imguimanager.h"
#include <ImGuizmo.h>

namespace EditorWidgets
{
    inline const ImVec4 kPrefabColor{0.48f, 0.78f, 1.0f, 1.0f}; // 旧名。EditorPreferences::PrefabColor() を使う

    // ---- #6 インスペクターの検索とカテゴリ
    enum class InspectorCategory : int
    {
        All = 0,
        Transform,
        Material,
        Rendering,
        Physics,
        Other,
        Count
    };
    inline InspectorCategory g_InspectorCategory = InspectorCategory::All;
    inline ImGuiTextFilter g_InspectorFilter;
    inline bool g_InspectorFiltering = false; // DrawInspectorWindow の間だけ true（ほかの窓では絞り込まない）
    // インスペクター上部の検索欄とカテゴリのタブ
    void DrawInspectorFilterBar();
    // CollapsingHeader の代わり。カテゴリ / 検索に合わないセクションは描かずに false を返す
    bool InspectorSection(const char* label, ImGuiTreeNodeFlags flags = 0);
    // 既定値に戻すボタン（値が既定と違うときだけ有効）。押されたら true
    bool ResetButton(const char* id, bool modified, const char* tooltip = "既定値に戻す");
    inline constexpr float kRadToDeg = 180.0f / XM_PI;
    inline constexpr float kDegToRad = XM_PI / 180.0f;
    void DrawSearchField(const char* id, const char* hint, ImGuiTextFilter& filter);
    void DrawUpscaleControls();
    XMMATRIX BuildWorldMatrix(const TransformComponent& transform);
    // defaults を渡すと、行の右端に「既定値に戻す」ボタンを付ける（#6）
    bool DrawAxisFloat3(const char* label, float* values, float speed, float minValue = 0.0f, float maxValue = 0.0f,
                        const float* defaults = nullptr);
    bool DrawAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue = 0.0f, float maxValue = 0.0f,
                        const float* defaults = nullptr);
    bool DrawMaterialPartParams(const char* label, MaterialPartParams& params);
    XMFLOAT3 ClampScale(const float* scale, bool swapScaleYZ);
    bool ShouldConvertModelByPath(const filesystem::path& path);
    XMFLOAT3 GetDefaultModelRotationByPath(const filesystem::path& path, bool isConvert);
    bool DrawScaleAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue, float maxValue,
                             const float* defaults = nullptr);
    void DrawFpsMeter();
    ImGuizmo::OPERATION GetGizmoOperationFromIndex(int operation);
    const char* GetGizmoOperationLabel(int operation);
    bool GetLocalAabb(EntityID entity, XMFLOAT3& center, XMFLOAT3& extents);
    bool IsModelMaterialEntity(EntityID entity);
    void ApplyLightEntityToRuntime(EntityID entity);
    void RefreshLightEntityName(EntityID entity, LightType type);
} // 名前空間 EditorWidgets の終わり。

