#include "pch.h"
#include "niagarawindow.h"
#include "imguimanager.h"
#include "editorworkspace.h"
#include "animationpreviewwindow.h"
#include "proceduralstudiowindow.h"
#include "sequencerbridge.h"
#include "editorshell.h"

void ImGuiManager::InitializeWorkspaces()
{
    auto& workspaces = EditorWorkspaceManager::Get();
    auto& registry = workspaces.Registry();
    if (registry.Windows().empty())
    {
        registry.RegisterWindow({"scene", "3D Viewport", "シーンビュー", &m_ShowSceneViewWindow, DrawSceneViewWindow});
        registry.RegisterWindow(
            {"hierarchy", "Scene Outliner", "ヒエラルキー", &m_ShowHierarchyWindow, DrawHierarchyWindow});
        registry.RegisterWindow(
            {"inspector", "Inspector", "インスペクター", &m_ShowInspectorWindow, DrawInspectorWindow});
        registry.RegisterWindow(
            {"assets", "Content Browser", "プロジェクト", &m_ShowAssetBrowser, DrawAssetBrowserWindow});
        registry.RegisterWindow({"material", "Material Editor", "マテリアルエディター", &m_ShowMaterialEditorWindow,
                                 DrawMaterialEditorWindow});
        registry.RegisterWindow(
            {"procedural_studio", "Procedural Studio", "プロシージャルスタジオ", &m_ShowProceduralStudioWindow, [] {
                 ProceduralStudioWindow::Draw(&m_ShowProceduralStudioWindow);
             }});
        registry.RegisterWindow({"procedural", "Procedural Parameters", "プロシージャルエディター",
                                 &m_ShowProceduralWindow, DrawProceduralWindow});
        registry.RegisterWindow(
            {"animation", "Animation Preview", "アニメーションプレビュー", &m_ShowAnimationPreviewWindow, [] {
                 AnimationPreviewWindow::Draw(&m_ShowAnimationPreviewWindow);
             }});
        registry.RegisterWindow({"niagara", "Niagara", "Niagara", &m_ShowNiagaraWindow, [] {
            NiagaraWindow::Draw(&m_ShowNiagaraWindow);
        }});
        registry.RegisterWindow({"sequencer", "Sequencer", "シーケンサー", &m_ShowSequencerWindow, [] {
                                     SequencerEditorBridge::Draw(&m_ShowSequencerWindow);
                                 }});
        registry.RegisterWindow({"render_settings", "Rendering Settings", "レンダーコントロール",
                                 &m_ShowAdjustmentPanel, DrawAdjustmentPanel});
        registry.RegisterWindow(
            {"render_debug", "Rendering Overview", "描画概要", &m_ShowRenderDebugger, DrawRenderDebuggerWindow});
        registry.RegisterWindow(
            {"performance", "Performance", "パフォーマンス", &m_ShowPerformanceWindow, DrawPerformanceWindow});
        registry.RegisterWindow({"gbuffer", "GBuffer Viewer", "Gバッファ", &m_ShowGBufferWindow, DrawGBufferWindow});
        registry.RegisterWindow({"console", "Console", "ログ", &m_ShowLogWindow, DrawLogWindow});
        registry.RegisterWindow({"gpu_scene", "GPU Scene", "GPU Scene", &m_ShowGpuSceneWindow, {}});
        registry.RegisterWindow(
            {"gpu_driven", "GPU Driven Rendering", "GPU Driven Rendering", &m_ShowGpuDrivenWindow, {}});
        registry.RegisterWindow({"visibility", "Visibility", "Visibility", &m_ShowVisibilityWindow, {}});
        registry.RegisterWindow({"mesh_shader", "Mesh Shader", "Mesh Shader", &m_ShowMeshShaderWindow, {}});
        registry.RegisterWindow({"virtual_geometry", "Nanite / LOD", "Nanite / LOD", &m_ShowVirtualGeometryWindow, {}});
        registry.RegisterWindow({"clouds", "プロシージャルクラウド", "プロシージャルクラウド", &m_ShowCloudWindow, {}});
        registry.RegisterWindow({"textures", "Texture Viewer", "テクスチャ一覧", &m_ShowTextureListWindow, {}});
        registry.RegisterWindow({"rim", "リム設定", "リム設定", &m_ShowRimSettingsWindow, DrawRimSettingsWindow});
        registry.RegisterWindow({"outline", "メッシュ単位のアウトライン", "メッシュ単位のアウトライン",
                                 &m_ShowMeshOutlineWindow, DrawMeshOutlineWindow});
        registry.RegisterWindow({"mesh_shading", "メッシュ単位のシェーディング", "メッシュ単位のシェーディング",
                                 &m_ShowMeshShadingWindow, DrawMeshShadingWindow});
        registry.RegisterWindow({"atmosphere", "大気シミュレーション", "大気シミュレーション", &m_ShowAtmosphereWindow,
                                 DrawAtmosphereWindow});
        registry.RegisterWindow(
            {"raytracing", "レイトレーシング", "レイトレーシング", &m_ShowRaytracingWindow, DrawRaytracingWindow});
        registry.RegisterWindow(
            {"project_settings", "環境設定", "環境設定", &m_ShowProjectSettingsWindow, DrawProjectSettingsWindow});
        registry.RegisterWindow(
            {"physics_settings", "物理設定", "物理設定", &m_ShowPhysicsSettingsWindow, DrawPhysicsSettingsWindow});
        registry.RegisterWindow(
            {"history", "Undo History", "Undo History", &m_ShowHistoryWindow, EditorShell::DrawHistoryWindow});
        registry.RegisterWindow({"preferences", "Editor Preferences", "エディター設定", &m_ShowPreferencesWindow,
                                 EditorShell::DrawPreferencesWindow});
    }
    // 最適化：環境変数の確認は初回だけ（以前は毎フレーム GetEnvironmentVariableA を呼んでいた）
    static const bool validation = [] {
        char validationMode[8]{};
        return GetEnvironmentVariableA("DX12_VALIDATION_MODE", validationMode, sizeof(validationMode)) > 0 &&
               atoi(validationMode) != 0;
    }();
    workspaces.Initialize(!validation);
}
