#include "pch.h"
#include "imguimanager.h"
#include "modelmanager.h"
#include "world.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "texturemanager.h"
#include "materialsystem.h"
#include "light.h"
#include "sun.h"
#include "atmosphere.h"
#include "renderersettings.h"
#include "localheightfog.h"
#include "debugsystem.h"
#include "meshshaderpipeline.h"
#include "animator.h"
#include "projectmanager.h"
#include "physicssystem.h"
#include "systemmanager.h"
#include "timelinesystem.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>
#include "rendertargets.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "renderconfiguration.h"


#include "editorwidgets.h"

using namespace EditorWidgets;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool ImGuiManager::Init(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* commandQueue, int numFrames, DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* cbvHeap, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigDockingWithShift = false;

	StyleModernSlim();

	const char* fontPath = "C:\\Windows\\Fonts\\msgothic.ttc";
	if (filesystem::exists(fontPath))
	{
		io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
	}

	if (!ImGui_ImplWin32_Init(hwnd)) return false;

	ImGui_ImplDX12_InitInfo initInfo {};
	initInfo.Device = device;
	initInfo.CommandQueue = commandQueue;
	initInfo.NumFramesInFlight = numFrames;
	initInfo.RTVFormat = rtvFormat;
	initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	initInfo.SrvDescriptorHeap = cbvHeap;
	initInfo.LegacySingleSrvCpuDescriptor = cpuHandle;
	initInfo.LegacySingleSrvGpuDescriptor = gpuHandle;
	initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
		{
		*out_cpu = info->LegacySingleSrvCpuDescriptor;
		*out_gpu = info->LegacySingleSrvGpuDescriptor;
		};
	initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};

	if (!ImGui_ImplDX12_Init(&initInfo)) return false;

	return true;
}

void ImGuiManager::Uninit()
{
	SaveProjectSettings();
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void ImGuiManager::Update()
{
	if (!m_ProjectSettingsLoaded)
	{
		LoadProjectSettings();
		m_ProjectSettingsLoaded = true;
	}

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();
	DebugSystem::SetShowLightDebug(m_ShowLightDebug);

	ImGuiIO& io = ImGui::GetIO();
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
	{
		if (io.KeyShift) Redo();
		else Undo();
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
	{
		Redo();
	}

	ProcessDroppedFiles();
	const bool canSwitchGizmo =
		!io.WantTextInput &&
		!ImGui::IsAnyItemActive() &&
		!ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
		!ImGuizmo::IsUsing();
	if (canSwitchGizmo)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) m_GizmoOperation = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_GizmoOperation = 1;
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_GizmoOperation = 2;
	}
	if (m_SelectedEntity != g_kINVALID_ENTITY &&
		Registry::IsAlive(m_SelectedEntity) &&
		ComponentManager::HasComponent<NameComponent>(m_SelectedEntity) &&
		ImGui::IsKeyPressed(ImGuiKey_F2, false))
	{
		BeginRename(m_SelectedEntity);
	}
	if (m_RenamingEntity != g_kINVALID_ENTITY && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		CancelRename();
	}
	if (m_SelectedEntity != g_kINVALID_ENTITY &&
		Registry::IsAlive(m_SelectedEntity) &&
		m_RenamingEntity == g_kINVALID_ENTITY &&
		!io.WantTextInput &&
		!ImGui::IsAnyItemActive() &&
		ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		DeleteSelectedEntity();
	}

	DrawDockSpace();
	DrawSceneViewWindow();
	PickEntityFromMouse();
	DrawEditorMainMenu();
	if (m_ShowEditorWindows)
	{
		DrawHierarchyWindow();
		DrawInspectorWindow();
	}
	if (m_ShowAssetBrowser) DrawAssetBrowserWindow();
	if (m_ShowRenderDebugger) DrawRenderDebuggerWindow();
	if (m_ShowGBufferWindow) DrawGBufferWindow();
	if (m_ShowLogWindow) DrawLogWindow();
	if (m_ShowPerformanceWindow) DrawPerformanceWindow();
	if (m_ShowMaterialEditorWindow) DrawMaterialEditorWindow();
	if (m_ShowRimSettingsWindow) DrawRimSettingsWindow();
	if (m_ShowMeshOutlineWindow) DrawMeshOutlineWindow();
	if (m_ShowMeshShadingWindow) DrawMeshShadingWindow();
	if (m_ShowAtmosphereWindow) DrawAtmosphereWindow();
	if (m_ShowProjectSettingsWindow) DrawProjectSettingsWindow();
	if (m_ShowPhysicsSettingsWindow) DrawPhysicsSettingsWindow();

	if (m_ShowAdjustmentPanel)
	{
		ImGui::SetNextWindowSize(ImVec2(360.0f, 520.0f), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("レンダーコントロール", &m_ShowAdjustmentPanel))
		{
			const float fps = World::GetFrameRate();
			ImVec4 healthyColor;
			if (fps >= 55.0f)
			{
				healthyColor = ImVec4(0.34f, 0.86f, 0.62f, 1.0f);
			}
			else
			{
				ImVec4 statusColor;
				if (fps >= 30.0f)
				{
					statusColor = ImVec4(0.96f, 0.72f, 0.28f, 1.0f);
				}
				else
				{
					statusColor = ImVec4(0.96f, 0.38f, 0.38f, 1.0f);
				}
				healthyColor = (statusColor);
			}
			ImGui::TextColored(ImVec4(0.28f, 0.78f, 0.92f, 1.0f), "DIRECTX 12  /  DEFERRED");
			ImGui::SameLine();
			ImGui::TextColored(healthyColor, "%.1f FPS", fps);
			ImGui::TextDisabled(
				"%u x %u  |  internal %u x %u  |  %.2f ms",
				GraphicsDevice::GetWidth(),
				GraphicsDevice::GetHeight(),
				GraphicsDevice::GetSceneWidth(),
				GraphicsDevice::GetSceneHeight(),
				World::GetFrameTimeMs());

			ImGui::Spacing();
			ImGui::SeparatorText("映像パイプライン");
			m_cameraPostProcess = static_cast<int>(Camera::GetCameraPostProcess());
			if (m_cameraPostProcess < 0 || m_cameraPostProcess >= static_cast<int>(PostProcessType::COUNT))
			{
				m_cameraPostProcess = 0;
			}
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::Combo(
				"##CameraPostProcess",
				&m_cameraPostProcess,
				m_cameraPostProcessModeItems,
				IM_ARRAYSIZE(m_cameraPostProcessModeItems)))
			{
				Camera::SetCameraPostProcess(static_cast<PostProcessType>(m_cameraPostProcess));
			}

			m_antiAliasingMode = static_cast<int>(RendererState::m_AntiAliasingMode);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::Combo(
				"アンチエイリアシング",
				&m_antiAliasingMode,
				m_antiAliasingModeItems,
				IM_ARRAYSIZE(m_antiAliasingModeItems)))
			{
				RendererState::m_AntiAliasingMode = static_cast<AntiAliasingMode>(m_antiAliasingMode);
				RendererState::m_TaaFrameIndex = 0;
			}

			DrawUpscaleControls();

			ImGui::Spacing();
			ImGui::SeparatorText("カラー");
			if (ImGui::Checkbox("HDR シーンカラー", &m_HdrEnabled))
			{
				RenderConfiguration::SetHdr(m_HdrEnabled);
			}
			ImGui::SameLine();
			ImGui::Checkbox("ACES", &m_ToneMapEnabled);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat(
				"露光",
				&m_Exposure,
				0.01f,
				10.0f,
				"%.2f",
				ImGuiSliderFlags_Logarithmic);

			float postProcessIntensity = Camera::GetCameraPostProcessIntensity();
			const PostProcessType activePostProcess = Camera::GetCameraPostProcess();
			ImGui::BeginDisabled(activePostProcess == PostProcessType::NONE);
			ImGui::SetNextItemWidth(-1.0f);
			float maxPostProcessIntensity;
			if (activePostProcess == PostProcessType::BLOOM)
			{
				maxPostProcessIntensity = 5.0f;
			}
			else
			{
				maxPostProcessIntensity = 1.0f;
			}
			if (ImGui::SliderFloat("エフェクト強度", &postProcessIntensity, 0.0f, maxPostProcessIntensity, "%.2f"))
			{
				Camera::SetCameraPostProcessIntensity(postProcessIntensity);
			}
			if (activePostProcess == PostProcessType::BLOOM)
			{
				float bloomThreshold = Camera::GetCameraBloomThreshold();
				float bloomSoftKnee = Camera::GetCameraBloomSoftKnee();
				float bloomRadius = Camera::GetCameraBloomRadius();
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("Bloom しきい値", &bloomThreshold, 0.0f, 16.0f, "%.2f"))
				{
					Camera::SetCameraBloomThreshold(bloomThreshold);
				}
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("Bloom Soft Knee", &bloomSoftKnee, 0.0f, 1.0f, "%.2f"))
				{
					Camera::SetCameraBloomSoftKnee(bloomSoftKnee);
				}
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("Bloom 半径", &bloomRadius, 0.25f, 4.0f, "%.2f"))
				{
					Camera::SetCameraBloomRadius(bloomRadius);
				}
			}
			ImGui::EndDisabled();

			ImGui::Spacing();
			ImGui::SeparatorText("フレーム制御");
			DrawFpsMeter();
			bool vsyncEnabled = World::IsVSyncEnabled();
			if (ImGui::Checkbox("垂直同期", &vsyncEnabled))
			{
				World::SetVSyncEnabled(vsyncEnabled);
			}
			ImGui::SameLine();
			bool fixedFrameRateEnabled = World::IsFixedFrameRateEnabled();
			if (ImGui::Checkbox("FPS 固定", &fixedFrameRateEnabled))
			{
				World::SetFixedFrameRateEnabled(fixedFrameRateEnabled);
			}
			int targetFrameRate = World::GetTargetFrameRate();
			ImGui::BeginDisabled(!fixedFrameRateEnabled);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::SliderInt("目標 FPS", &targetFrameRate, 15, 360))
			{
				World::SetTargetFrameRate(targetFrameRate);
			}
			ImGui::EndDisabled();

			ImGui::Spacing();
			if (ImGui::Button("GBuffer を表示", ImVec2(-1.0f, 0.0f)))
			{
				m_ShowGBufferWindow = true;
			}
		}
		ImGui::End();
	}

	FinalizeUndoCaptureIfIdle();
}
void ImGuiManager::DrawSceneEditor()
{
ImGui::SeparatorText("セクション");

	const char* selectedName = GetEntityDisplayName(m_SelectedEntity);
	ImGui::Text("選択中: %s", selectedName);

	if (m_SelectedEntity != g_kINVALID_ENTITY &&
		Registry::IsAlive(m_SelectedEntity) &&
		ComponentManager::HasComponent<TransformComponent>(m_SelectedEntity))
	{
		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(m_SelectedEntity);
		EntitySnapshot before = CaptureEntity(m_SelectedEntity);
		bool changed = false;
		changed |= DrawAxisFloat3("位置", transform.Position, 0.01f);
		changed |= DrawAxisFloat3("回転", transform.Rotation, 0.01f);
		changed |= DrawScaleAxisFloat3("スケール", transform.Scale, 0.01f, 0.001f, 100.0f);
		if (changed)
		{
			BeginUndoCapture(m_SelectedEntity, before);
			transform.WorldMatrix = {};
			XMStoreFloat4x4(&transform.WorldMatrix, BuildWorldMatrix(transform));
			transform.IsDirty = true;
		}
	}

	ImGui::Text("Q/W/E: 移動 / 回転 / スケール（現在: %s）", GetGizmoOperationLabel(m_GizmoOperation));
}

void ImGuiManager::DrawDockSpace()
{
	ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGuiWindowFlags hostFlags =
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoNavFocus |
		ImGuiWindowFlags_NoBackground;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("EditorDockSpace", nullptr, hostFlags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspaceId = ImGui::GetID("MainEditorDockSpace");
	ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);
	ImGui::End();
}


void ImGuiManager::DrawEditorMainMenu()
{
	if (!ImGui::BeginMainMenuBar())
	{
		return;
	}

	ImGui::TextUnformatted("DirectX12 エディター");
	ImGui::Separator();
	if (ImGui::Button("元に戻す"))
	{
		Undo();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Z");
	ImGui::SameLine();
	if (ImGui::Button("やり直し"))
	{
		Redo();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Y / Ctrl+Shift+Z");
	ImGui::SameLine();

	ImGui::Separator();

	if (ImGui::BeginMenu("Window"))
	{
		ImGui::MenuItem("エディター", nullptr, &m_ShowEditorWindows);
		ImGui::MenuItem("レンダーコントロール", nullptr, &m_ShowAdjustmentPanel);
		ImGui::MenuItem("アセット", nullptr, &m_ShowAssetBrowser);
		ImGui::MenuItem("描画デバッグ", nullptr, &m_ShowRenderDebugger);
		ImGui::MenuItem("Gバッファ", nullptr, &m_ShowGBufferWindow);
		ImGui::MenuItem("ログ", nullptr, &m_ShowLogWindow);
		ImGui::MenuItem("パフォーマンス", nullptr, &m_ShowPerformanceWindow);
		ImGui::MenuItem("環境設定", nullptr, &m_ShowProjectSettingsWindow);
		ImGui::MenuItem("物理設定", nullptr, &m_ShowPhysicsSettingsWindow);
		ImGui::MenuItem("マテリアルエディター", nullptr, &m_ShowMaterialEditorWindow);
		ImGui::MenuItem("リム設定", nullptr, &m_ShowRimSettingsWindow);
		ImGui::MenuItem("大気シミュレーション", nullptr, &m_ShowAtmosphereWindow);
		ImGui::Separator();
		ImGui::MenuItem("メッシュ単位のアウトライン", nullptr, &m_ShowMeshOutlineWindow);
		ImGui::MenuItem("メッシュ単位のシェーディング", nullptr, &m_ShowMeshShadingWindow);
		ImGui::EndMenu();
	}
	ImGui::SameLine();
	ImGui::Checkbox("ライト可視化", &m_ShowLightDebug);
	ImGui::SameLine();
	if (ImGui::BeginMenu("ライト追加"))
	{
		if (ImGui::MenuItem("Directional"))
		{
			m_SelectedEntity = CreateLightEntity(LightType::Directional);
		}
		if (ImGui::MenuItem("Point"))
		{
			m_SelectedEntity = CreateLightEntity(LightType::Point);
		}
		if (ImGui::MenuItem("Spot"))
		{
			m_SelectedEntity = CreateLightEntity(LightType::Spot);
		}
		if (ImGui::MenuItem("Volume"))
		{
			m_SelectedEntity = CreateLightEntity(LightType::Volume);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Sun"))
		{
			m_SelectedEntity = Sun::CreateDefault();
		}
		ImGui::EndMenu();
	}


	const float playControlsWidth = 184.0f;
	const float centeredX = max(
		(ImGui::GetIO().DisplaySize.x - playControlsWidth) * 0.5f,
		ImGui::GetCursorPosX() + 12.0f);
	ImGui::SameLine();
	ImGui::SetCursorPosX(centeredX);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.58f, 0.25f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.72f, 0.31f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.46f, 0.20f, 1.0f));
	const char* statusText;
	if (ProjectManager::IsPlaying())
	{
		statusText = "■ Stop##ProjectPlay";
	}
	else
	{
		statusText = "▶ Play##ProjectPlay";
	}
	if (ImGui::Button(statusText,
		ImVec2(88.0f, 0.0f)))
	{
		ProjectManager::TogglePlay();
	}
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::BeginDisabled(!ProjectManager::IsPlaying());
	const char* statusTextValue;
	if (ProjectManager::IsPaused())
	{
		statusTextValue = "▶ Resume##ProjectPause";
	}
	else
	{
		statusTextValue = "Ⅱ Pause##ProjectPause";
	}
	if (ImGui::Button(statusTextValue,
		ImVec2(88.0f, 0.0f)))
	{
		ProjectManager::TogglePause();
	}
	ImGui::EndDisabled();

	if (m_SelectedEntity != g_kINVALID_ENTITY)
	{
		ImGui::SameLine();
		ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	}

	ImGui::EndMainMenuBar();
}


void ImGuiManager::Draw(ID3D12GraphicsCommandList* commandList)
{
	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

bool ImGuiManager::HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui::GetCurrentContext() == nullptr)
	{
		return false;
	}
	return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) != 0;
}

void ImGuiManager::HandleDroppedFile(const char* path)
{
	if (path && path[0] != '\0')
	{
		m_DroppedFiles.push_back(path);
	}
}

void ImGuiManager::AddLog(const char* fmt, ...)
{
	char buffer[1024]{};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	m_Logs.emplace_back(buffer);
	if (m_Logs.size() > 1000)
	{
		m_Logs.erase(m_Logs.begin());
	}
	Debug::Log("%s\n", buffer);
}

void ImGuiManager::StyleModernSlim()
{
	auto& style = ImGui::GetStyle();
	ImVec4* colors = style.Colors;

	colors[ImGuiCol_Text] = ImVec4(0.86f, 0.89f, 0.92f, 1.00f);
	colors[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.46f, 0.50f, 1.00f);
	colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.080f, 0.090f, 0.98f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.060f, 0.065f, 0.074f, 0.82f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.080f, 0.086f, 0.098f, 0.98f);
	colors[ImGuiCol_Border] = ImVec4(0.20f, 0.23f, 0.27f, 0.72f);
	colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_FrameBg] = ImVec4(0.115f, 0.125f, 0.142f, 1.00f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.155f, 0.175f, 0.205f, 1.00f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(0.105f, 0.190f, 0.255f, 1.00f);
	colors[ImGuiCol_TitleBg] = ImVec4(0.050f, 0.055f, 0.064f, 1.00f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.080f, 0.092f, 0.108f, 1.00f);
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.050f, 0.055f, 0.064f, 0.92f);
	colors[ImGuiCol_MenuBarBg] = ImVec4(0.070f, 0.077f, 0.090f, 1.00f);
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.055f, 0.060f, 0.070f, 0.82f);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.25f, 0.29f, 0.86f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.35f, 0.40f, 0.92f);
	colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.38f, 0.44f, 0.50f, 1.00f);
	colors[ImGuiCol_CheckMark] = ImVec4(0.18f, 0.68f, 0.82f, 1.00f);
	colors[ImGuiCol_SliderGrab] = ImVec4(0.18f, 0.62f, 0.76f, 1.00f);
	colors[ImGuiCol_SliderGrabActive] = ImVec4(0.28f, 0.78f, 0.92f, 1.00f);
	colors[ImGuiCol_Button] = ImVec4(0.13f, 0.16f, 0.19f, 1.00f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.23f, 0.27f, 1.00f);
	colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.44f, 0.56f, 1.00f);
	colors[ImGuiCol_Header] = ImVec4(0.13f, 0.18f, 0.22f, 0.78f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.34f, 0.42f, 0.86f);
	colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.50f, 0.62f, 0.96f);
	colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
	colors[ImGuiCol_SeparatorHovered] = ImVec4(0.18f, 0.68f, 0.82f, 0.78f);
	colors[ImGuiCol_SeparatorActive] = ImVec4(0.18f, 0.68f, 0.82f, 1.00f);
	colors[ImGuiCol_ResizeGrip] = ImVec4(0.18f, 0.68f, 0.82f, 0.18f);
	colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.18f, 0.68f, 0.82f, 0.58f);
	colors[ImGuiCol_ResizeGripActive] = ImVec4(0.18f, 0.68f, 0.82f, 0.92f);

	auto lerp = [](ImVec4 a, ImVec4 b, float t)
		{
			return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
		};

	colors[ImGuiCol_Tab] = lerp(colors[ImGuiCol_Header], colors[ImGuiCol_TitleBgActive], 0.80f);
	colors[ImGuiCol_TabHovered] = colors[ImGuiCol_HeaderHovered];
	colors[ImGuiCol_TabActive] = lerp(colors[ImGuiCol_HeaderActive], colors[ImGuiCol_TitleBgActive], 0.60f);
	colors[ImGuiCol_TabUnfocused] = lerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
	colors[ImGuiCol_TabUnfocusedActive] = lerp(colors[ImGuiCol_TabActive], colors[ImGuiCol_TitleBg], 0.40f);
	colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.28f, 0.78f, 0.92f, 0.88f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.18f, 0.52f, 0.62f, 0.58f);

	ImVec4 ha = colors[ImGuiCol_HeaderActive];
	colors[ImGuiCol_DockingPreview] = ImVec4(ha.x, ha.y, ha.z, ha.w * 0.7f);

	colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.060f, 0.065f, 0.074f, 1.00f);
	colors[ImGuiCol_TableHeaderBg] = ImVec4(0.095f, 0.110f, 0.128f, 1.00f);
	colors[ImGuiCol_TableBorderStrong] = ImVec4(0.19f, 0.22f, 0.26f, 1.00f);
	colors[ImGuiCol_TableBorderLight] = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
	colors[ImGuiCol_TableRowBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.13f, 0.15f, 0.18f, 0.28f);
	colors[ImGuiCol_PlotLines] = ImVec4(0.52f, 0.58f, 0.64f, 1.00f);
	colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.28f, 0.78f, 0.92f, 1.00f);
	colors[ImGuiCol_PlotHistogram] = ImVec4(0.18f, 0.68f, 0.82f, 1.00f);
	colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.36f, 0.82f, 0.96f, 1.00f);
	colors[ImGuiCol_TextSelectedBg] = ImVec4(0.18f, 0.68f, 0.82f, 0.28f);
	colors[ImGuiCol_DragDropTarget] = ImVec4(0.28f, 0.78f, 0.92f, 0.86f);
	colors[ImGuiCol_NavHighlight] = ImVec4(0.28f, 0.78f, 0.92f, 0.96f);
	colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.82f, 0.90f, 0.96f, 0.72f);
	colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.02f, 0.025f, 0.030f, 0.48f);
	colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.025f, 0.030f, 0.62f);

	style.WindowPadding = ImVec2(8.0f, 6.0f);
	style.FramePadding = ImVec2(6.0f, 3.0f);
	style.CellPadding = ImVec2(5.0f, 3.0f);
	style.ItemSpacing = ImVec2(7.0f, 4.0f);
	style.ItemInnerSpacing = ImVec2(5.0f, 3.0f);
	style.IndentSpacing = 14.0f;
	style.ScrollbarSize = 11.0f;
	style.GrabMinSize = 8.0f;

	style.WindowBorderSize = 1.0f;
	style.ChildBorderSize = 1.0f;
	style.PopupBorderSize = 1.0f;
	style.FrameBorderSize = 1.0f;
	style.TabBorderSize = 0.0f;
	style.TabBarBorderSize = 1.0f;
	style.TabBarOverlineSize = 1.5f;

	style.WindowRounding = 3.0f;
	style.ChildRounding = 2.0f;
	style.FrameRounding = 2.0f;
	style.GrabRounding = 2.0f;
	style.PopupRounding = 3.0f;
	style.ScrollbarRounding = 2.0f;
	style.TabRounding = 2.0f;
	style.WindowMenuButtonPosition = ImGuiDir_Right;
}
