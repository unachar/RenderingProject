#pragma once
#include "pch.h"
#include "ecs.h"
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
private:
	inline static bool m_IsClosing = false;
	inline static int m_renderMode = -1;
	inline static int m_cameraPostProcess = -1;
	inline static const char* m_renderModeItems[] = { "フォワード", "ディファード" };
	inline static const char* m_cameraPostProcessModeItems[] = { "なし", "ブラー", "セピア", "グレースケール", "反転" };
	inline static int m_antiAliasingMode = 0;
	inline static const char* m_antiAliasingModeItems[] = { "なし", "FXAA", "TAA" };
	inline static EntityID m_SelectedEntity = g_kINVALID_ENTITY;
	inline static bool m_ShowEditorWindows = true;
	inline static bool m_ShowAdjustmentPanel = true;
	inline static bool m_ShowAssetBrowser = true;
	inline static bool m_ShowRenderDebugger = false;
	inline static bool m_ShowGBufferWindow = false;
	inline static bool m_ShowLogWindow = false;
	inline static bool m_ShowPerformanceWindow = false;
	inline static bool m_ShowMaterialEditorWindow = false;
	inline static bool m_ShowRimSettingsWindow = false;
	inline static bool m_ShowMeshOutlineWindow = false;
	inline static bool m_ShowMeshShadingWindow = false;
	inline static bool m_ShowAtmosphereWindow = false;
	inline static bool m_ShowProjectSettingsWindow = false;
	inline static bool m_ProjectSettingsLoaded = false;
	inline static bool m_ShowLightDebug = false;
	inline static bool m_HdrEnabled = false;
	inline static bool m_ToneMapEnabled = true;
	inline static float m_Exposure = 1.0f;
	inline static int m_GizmoOperation = 0;
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
	static void DrawSceneEditor();
	static void DrawEditorMainMenu();
	static void DrawHierarchyWindow();
	static void DrawInspectorWindow();
	static void DrawAssetBrowserWindow();
	static void DrawRenderDebuggerWindow();
	static void DrawGBufferWindow();
	static void DrawLogWindow();
	static void DrawPerformanceWindow();
	static void DrawMaterialEditorWindow();
	static void DrawRimSettingsWindow();
	static void DrawMeshOutlineWindow();
	static void DrawMeshShadingWindow();
	static void DrawAtmosphereWindow();
	static void DrawProjectSettingsWindow();
	static void LoadProjectSettings();
	static void SaveProjectSettings();
	static void DrawMaterialInspector(EntityID entity);
	static void ApplyMeshShadingOverridesToModel(EntityID entity);
	static void DrawToonMeshOutlineInspector(EntityID entity, bool embeddedInInspector = true);
	static void DrawMeshShadingInspector(EntityID entity, bool embeddedInInspector = true);
	static void DrawLightInspector(EntityID entity);
	static void DrawComponentInspector(EntityID entity);
	static void DeleteSelectedEntity();
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

public:
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

