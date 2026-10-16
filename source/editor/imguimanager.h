#pragma once
#include "pch.h"
#include "ecs.h"
#include "materialasset.h"
#include "componentmanager.h"
#include <d3d12.h>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;

class ImGuiManager
{
    friend class RenderingValidation;
    friend class MaterialValidation;
private:
	inline static bool m_IsClosing = false;
	inline static int m_renderMode = -1;
	inline static int m_cameraPostProcess = -1;
	inline static const char* m_renderModeItems[] = { "フォワード", "ディファード" };
	inline static const char* m_cameraPostProcessModeItems[] = { "なし", "ブラー", "セピア", "グレースケール", "反転", "ブルーム" };
	inline static int m_antiAliasingMode = 0;
	inline static const char* m_antiAliasingModeItems[] = { "なし", "FXAA", "TAA" };
	inline static EntityID m_SelectedEntity = g_kINVALID_ENTITY;
	inline static bool m_ShowHierarchyWindow = true;
	inline static bool m_ShowInspectorWindow = true;
	inline static bool m_ResetEditorLayout = false;
	inline static bool m_RenameNeedsFocus = false;
	inline static bool m_ShowAdjustmentPanel = true;
	inline static bool m_ShowAssetBrowser = true;
	inline static bool m_ShowRenderDebugger = false;
	inline static bool m_ShowGpuSceneWindow = false;
	inline static bool m_ShowGpuDrivenWindow = false;
	inline static bool m_ShowVisibilityWindow = false;
	inline static bool m_ShowMeshShaderWindow = false;
	inline static bool m_ShowVirtualGeometryWindow = false;
	inline static bool m_ShowCloudWindow = false;
	inline static bool m_ShowTextureListWindow = false;
	inline static bool m_ShowGBufferWindow = false;
	inline static bool m_ShowLogWindow = false;
	inline static bool m_LogAutoScroll = true;
	inline static bool m_ShowPerformanceWindow = false;
	inline static bool m_ShowMaterialEditorWindow = false;
	inline static bool m_ShowRimSettingsWindow = false;
	inline static bool m_ShowMeshOutlineWindow = false;
	inline static bool m_ShowMeshShadingWindow = false;
	inline static bool m_ShowAtmosphereWindow = false;
	inline static bool m_ShowProceduralWindow = false;
	inline static bool m_ShowRaytracingWindow = false;
	inline static bool m_ShowProjectSettingsWindow = false;
	inline static bool m_ShowPhysicsSettingsWindow = false;
	inline static bool m_ProjectSettingsLoaded = false;
	inline static bool m_ShowLightDebug = false;
	inline static bool m_HdrEnabled = false;
	inline static bool m_ToneMapEnabled = true;
	inline static float m_Exposure = 1.0f;
	inline static int m_GizmoOperation = 0; // 0:移動 1:回転 2:スケール 3:ハンド
	inline static bool m_GizmoLocalSpace = false;
	inline static ImVec2 m_SceneViewPos = { 0.0f, 0.0f };
	inline static ImVec2 m_SceneViewSize = { 1.0f, 1.0f };
	inline static bool m_IsSceneViewHovered = false;
	inline static ImVec2 m_LastPickMouse = { -10000.0f, -10000.0f };
	inline static vector<EntityID> m_LastPickCandidates;
	inline static filesystem::path m_AssetRoot = "asset";
	inline static filesystem::path m_CurrentAssetDirectory = "asset";
	inline static filesystem::path m_SelectedAssetPath;
	inline static string m_NewFolderName = "NewFolder";
	inline static string m_NewFileName = "NewMaterial.txt";
	inline static string m_TexturePathInput;
	inline static vector<string> m_DroppedFiles;
	inline static vector<string> m_Logs;
	inline static EntityID m_RenamingEntity = g_kINVALID_ENTITY;
	inline static string m_RenameBuffer;
	struct EntitySnapshot
	{
		EntityID Entity = g_kINVALID_ENTITY;
		bool WasAlive = false;
		bool HasName = false;
		NameComponent Name{};
		bool HasTransform = false;
		TransformComponent Transform{};
		bool HasShader = false;
		ShaderComponent Shader{};
		bool HasStaticModel = false;
		StaticModelComponent StaticModel{};
		bool HasAnimationModel = false;
		AnimationModelComponent AnimationModel{};
		bool HasLight = false;
		LightComponent Light{};
		bool HasSun = false;
		SunComponent Sun{};
		bool HasProceduralTerrain = false;
		ProceduralTerrainComponent ProceduralTerrain{};
		bool HasProceduralMotion = false;
		ProceduralMotionComponent ProceduralMotion{};
		bool HasMaterial = false;
		MaterialComponent Material{};
		bool HasAabb = false;
		AABBComponent Aabb{};
		bool HasSprite = false;
		SpriteComponent Sprite{};
		bool HasMesh = false;
		MeshComponent Mesh{};
		bool HasCamera = false;
		CameraComponent Camera{};
		bool HasPostProcess = false;
		PostProcessComponent PostProcess{};
		bool HasInput = false;
		InputComponent Input{};
		bool HasMove = false;
		MoveComponent Move{};
		bool HasPhysics = false;
		PhysicsComponent Physics{};
		bool HasTimeline = false;
		TimelineComponent Timeline{};
		bool HasObb = false;
		OBBComponent Obb{};
		bool HasLod = false;
		LODComponent Lod{};
	};
	inline static vector<EntitySnapshot> m_UndoStack;
	inline static vector<EntitySnapshot> m_RedoStack;
	inline static bool m_HasPendingUndo = false;
	inline static EntitySnapshot m_PendingUndo{};

	static void StyleModernSlim();
	static void DrawDockSpace();
	static void DrawSceneViewWindow();
	static void DrawTransformGizmo();
	static void DrawEditorMainMenu();
	static void DrawHierarchyWindow();
	static void DrawPrimitiveCreationMenu();
	static void DrawPrimitiveSettingsWindow();
	static void DrawInspectorWindow();
	static void DrawAssetBrowserWindow();
	static void DrawRenderDebuggerWindow();
	static void DrawRenderFeatureWindows();
	static void DrawTextureListContent();
	static void DrawGBufferWindow();
	static void DrawLogWindow();
	static void DrawPerformanceWindow();
	static void DrawMaterialEditorWindow();
    static void DrawLegacyMaterialEditorWindow();
    static void OpenMaterialAsset(AssetHandle handle);
    static void DrawMaterialAssetSlots(EntityID entity);
    static void DrawMaterialAssetClosePrompt();
    static void MaterialEditorUndo(bool redo);
    static bool MaterialEditorHasFocus();
    static void AssignMaterialAsset(EntityID entity, size_t slot, AssetHandle handle);
    static void DrawMaterialDropTarget();
	static void DrawRimSettingsWindow();
	static void DrawMeshOutlineWindow();
	static void DrawMeshShadingWindow();
	static void DrawAtmosphereWindow();
	static void DrawProceduralWindow();
	static void DrawRaytracingWindow();
	static void DrawProjectSettingsWindow();
	static void DrawPhysicsSettingsWindow();
	static void LoadProjectSettings();
	static bool PersistentSettings(std::ostream* output, const string& key = {}, const string& value = {});
	inline static string m_LastSavedProjectSettings;
	static void DrawMaterialInspector(EntityID entity);
	static void ApplyMeshShadingOverridesToModel(EntityID entity);
	static void DrawToonMeshOutlineInspector(EntityID entity, bool embeddedInInspector = true);
	static void DrawMeshShadingInspector(EntityID entity, bool embeddedInInspector = true);
	static void DrawLightInspector(EntityID entity);
	static void DrawComponentInspector(EntityID entity);
	static void DeleteSelectedEntity();
	static EntityID DuplicateSelectedEntity();
	static string MakeDuplicateName(const string& sourceName);
	static EntityID CreateLightEntity(LightType type);
	static void ProcessDroppedFiles();
	static void ImportAssetFile(const filesystem::path& sourcePath);
	static void CreateAssetFile(const filesystem::path& path);
	static void DeleteAssetPath(const filesystem::path& path);
	static void PlaceAssetInScene(const filesystem::path& path, const ImVec2& sceneMouse);
	static bool GetSceneViewRay(const ImVec2& sceneMouse, XMVECTOR& outOrigin, XMVECTOR& outDirection);
	static bool IsTextureFile(const filesystem::path& path);
	static bool IsModelFile(const filesystem::path& path);
	static string MakeRelativeAssetPath(const filesystem::path& path);
	static void PickEntityFromMouse();
	static bool IsEditableEntity(EntityID entity);
	static void BeginRename(EntityID entity);
	static void CommitRename();
	static void CancelRename();
	static bool DrawRenameInput(EntityID entity);
	static EntitySnapshot CaptureEntity(EntityID entity);
	static void ApplySnapshot(const EntitySnapshot& snapshot);
	static void BeginUndoCapture(EntityID entity, const EntitySnapshot& before);
	static void FinalizeUndoCaptureIfIdle();
	static void PushUndoSnapshot(const EntitySnapshot& snapshot);
	static void Undo();
	static void Redo();
	static const char* GetEntityDisplayName(EntityID entity);
	static const char* GetLightTypeName(LightType type);

	// ---------------------------------------------------------------- シーンシステム（editorscene.cpp）
	enum class SceneAction { None, Open, New, Quit };
	inline static filesystem::path m_ScenePath;               // 空 = 未保存の新規シーン
	inline static filesystem::path m_StartupScenePath;        // cfg の scene=
	inline static uint64_t m_SceneSavedHash = 0;              // 最後に保存/読み込みした時点
	inline static uint64_t m_SceneCurrentHash = 0;
	inline static bool m_SceneTracking = false;
	inline static double m_SceneNextHashTime = 0.0;
	inline static double m_SceneSettleUntil = 0.0;
	inline static uint64_t m_EditRevision = 0;                // Undoに積まれる編集のたびに増える
	inline static uint64_t m_SceneSettleRevision = 0;
	inline static SceneAction m_PendingSceneAction = SceneAction::None;
	inline static filesystem::path m_PendingScenePath;
	inline static bool m_ExecuteSceneAction = false;
	inline static bool m_RequestSaveChangesPopup = false;
	inline static bool m_RequestSaveAsPopup = false;
	inline static bool m_ContinueAfterSaveAs = false;
	inline static char m_SaveAsName[128] = "NewScene";
	inline static char m_SaveAsDirectory[260] = "asset/Scenes";
	inline static wstring m_LastWindowTitle;

	static bool SceneTrackingAllowed();
	static vector<EntityID> CollectSceneEntities();
	static uint64_t ComputeSceneHash();
	static string SerializeScene();
	static bool SaveScene(const filesystem::path& path);
	static bool LoadScene(const filesystem::path& path);
	static void DestroySceneEntities();
	static void CreateNewScene();
	static void RebuildLoadedEntity(EntityID entity, const void* record);
	static void BeginSceneBaseline(double settleSeconds);
	static void UpdateSceneTracking();
	static void ProcessSceneAction();
	static void RequestSceneAction(SceneAction action, const filesystem::path& path = {});
	static void SaveCurrentScene(bool saveAs);
	static void DrawScenePopups();
	static void DrawSceneFileMenu();
	static bool IsSceneFile(const filesystem::path& path);
	static string GetSceneDisplayName();

public:
	// シーン操作（未保存なら「保存しますか？」を出してから実行する）
	static void RequestOpenScene(const filesystem::path& path) { RequestSceneAction(SceneAction::Open, path); }
	static void RequestNewScene() { RequestSceneAction(SceneAction::New); }
	static void RequestQuit();
	static bool IsSceneDirty() { return m_SceneTracking && m_SceneCurrentHash != m_SceneSavedHash; }
	static const filesystem::path& GetScenePath() { return m_ScenePath; }

	static void SaveProjectSettings();
	static constexpr UINT kSrvDescriptorCount = 64;
    inline static bool m_AssetBrowserStandalone = false;
    static void SetAssetBrowserDirectory(const filesystem::path& directory, bool standalone = false)
    {
        m_CurrentAssetDirectory = directory;
        m_ShowAssetBrowser = true;
        m_AssetBrowserStandalone = standalone;
    }
	ImGuiManager() = delete;

	static bool Init(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* commandQueue, int numFrames, DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* cbvHeap, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
	static void Uninit();
	static void Update();
	static void Draw(ID3D12GraphicsCommandList* commandList);
	static bool HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static void HandleDroppedFile(const char* path);
	static void AddLog(const char* fmt, ...);

	static EntityID GetSelectedEntity() { return m_SelectedEntity; }
	static bool IsClosing() { return m_IsClosing; }
	static bool IsHdrEnabled() { return m_HdrEnabled; }
	static bool IsToneMapEnabled() { return m_ToneMapEnabled; }
	static float GetExposure() { return m_Exposure; }
};


