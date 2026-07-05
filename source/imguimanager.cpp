#include "pch.h"
#include "imguimanager.h"
#include "modelmanager.h"
#include "world.h"
#include "camera.h"
#include "renderercore.h"
#include "rendererresource.h"
#include "texturemanager.h"
#include "materialsystem.h"
#include "light.h"
#include "debugsystem.h"
#include "animator.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	constexpr float kRadToDeg = 180.0f / XM_PI;
	constexpr float kDegToRad = XM_PI / 180.0f;

	XMMATRIX BuildWorldMatrix(const TransformComponent& transform)
	{
		return XMMatrixScaling(transform.Scale.x, transform.Scale.y, transform.Scale.z) *
			XMMatrixRotationX(transform.Rotation.x) *
			XMMatrixRotationY(transform.Rotation.y) *
			XMMatrixRotationZ(transform.Rotation.z) *
			XMMatrixTranslation(transform.Position.x, transform.Position.y, transform.Position.z);
	}

	bool DrawAxisFloat3(const char* label, float* values, float speed, float minValue = 0.0f, float maxValue = 0.0f)
	{
		bool changed = false;
		ImGui::PushID(label);
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 2.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.025f, 0.027f, 0.030f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.050f, 0.055f, 0.064f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.070f, 0.082f, 0.100f, 1.00f));
		if (ImGui::BeginTable("AxisFloat3", 4, ImGuiTableFlags_SizingStretchProp))
		{
			const char* axisIds[3] = { "##R", "##G", "##B" };
			const ImU32 axisColors[3] =
			{
				ImGui::GetColorU32(ImVec4(0.86f, 0.18f, 0.14f, 1.00f)),
				ImGui::GetColorU32(ImVec4(0.24f, 0.68f, 0.25f, 1.00f)),
				ImGui::GetColorU32(ImVec4(0.18f, 0.42f, 0.86f, 1.00f))
			};

			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("G", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			{
				const float width = ImGui::GetContentRegionAvail().x;
				const float height = ImGui::GetFrameHeight();
				ImGui::InvisibleButton("##AxisLabel", ImVec2(width, height));

				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.095f, 0.100f, 0.108f, 1.00f)), 2.0f);
				drawList->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.18f, 0.19f, 0.20f, 1.00f)), 2.0f);

				const float textY = min.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
				drawList->AddText(ImVec2(min.x + 8.0f, textY), ImGui::GetColorU32(ImGuiCol_Text), label);

				const float arrowX = max.x - 13.0f;
				const float arrowY = min.y + height * 0.5f - 1.0f;
				drawList->AddTriangleFilled(
					ImVec2(arrowX - 4.0f, arrowY - 2.0f),
					ImVec2(arrowX + 4.0f, arrowY - 2.0f),
					ImVec2(arrowX, arrowY + 3.0f),
					ImGui::GetColorU32(ImVec4(0.62f, 0.66f, 0.70f, 1.00f)));
			}

			for (int i = 0; i < 3; ++i)
			{
				ImGui::TableNextColumn();
				const float height = ImGui::GetFrameHeight();
				ImGui::Dummy(ImVec2(3.0f, height));
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImGui::GetItemRectMin(),
					ImGui::GetItemRectMax(),
					axisColors[i],
					1.0f);
				ImGui::SameLine(0.0f, 2.0f);
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed |= ImGui::DragFloat(axisIds[i], &values[i], speed, minValue, maxValue, "%.3f");
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(3);
		ImGui::PopID();
		return changed;
	}

	bool DrawAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue = 0.0f, float maxValue = 0.0f)
	{
		float values[3] = { value.x, value.y, value.z };
		if (!DrawAxisFloat3(label, values, speed, minValue, maxValue))
		{
			return false;
		}

		value = { values[0], values[1], values[2] };
		return true;
	}

	bool DrawMaterialPartParams(const char* label, MaterialPartParams& params)
	{
		bool changed = false;
		ImGui::PushID(label);
		if (ImGui::TreeNode(label))
		{
			changed |= ImGui::SliderFloat("メタリック", &params.Metallic, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("ラフネス", &params.Roughness, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("フレネル", &params.Fresnel, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("法線ブレンド", &params.NormalBlend, 0.0f, 1.0f);
			changed |= ImGui::SliderFloat("法線バイアス", &params.NormalBias, -1.0f, 1.0f);
			changed |= ImGui::SliderFloat("ベース彩度", &params.BaseSaturation, 0.0f, 3.0f);
			changed |= ImGui::SliderFloat("ベース明度", &params.BaseBrightness, 0.0f, 3.0f);
			changed |= ImGui::SliderFloat("かわいいブレンド", &params.KawaiiBlend, 0.0f, 1.0f);

			if (ImGui::TreeNode("影"))
			{
				changed |= ImGui::SliderFloat("影しきい値", &params.ShadowThreshold, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("影ぼかし", &params.ShadowSoftness, 0.0f, 0.5f);
				changed |= ImGui::SliderFloat("影の強さ", &params.ShadowStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("中間色の強さ", &params.MidStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("明部の強さ", &params.LitStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("影しきい値", &params.CastShadowThreshold, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("影ぼかし", &params.CastShadowSoftness, 0.0f, 0.5f);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("ハイライト"))
			{
				changed |= ImGui::SliderFloat("リム強度", &params.RimStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("リムしきい値", &params.RimThreshold, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("スペキュラ強度", &params.SpecularStrength, 0.0f, 2.0f);
				changed |= ImGui::SliderFloat("スペキュラしきい値", &params.SpecularThreshold, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("肌"))
			{
				changed |= ImGui::SliderFloat("肌散乱強度", &params.SkinScatterStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("肌散乱ラップ", &params.SkinScatterWrap, 0.0f, 1.0f);
				changed |= ImGui::SliderFloat("肌逆光強度", &params.SkinBacklightStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("肌リム散乱強度", &params.SkinRimScatterStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("スペキュラ強度", &params.SkinOilSpecularStrength, 0.0f, 3.0f);
				changed |= ImGui::SliderFloat("肌影散乱", &params.SkinShadowScatter, 0.0f, 1.0f);
				ImGui::TreePop();
			}

			ImGui::TreePop();
		}
		ImGui::PopID();
		return changed;
	}

	XMFLOAT3 ClampScale(const float* scale, bool swapScaleYZ)
	{
		return
		{
			max(scale[0], 0.001f),
			max(swapScaleYZ ? scale[2] : scale[1], 0.001f),
			max(swapScaleYZ ? scale[1] : scale[2], 0.001f)
		};
	}

	bool ShouldConvertModelByPath(const filesystem::path& path)
	{
		string lower = path.generic_string();
		transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
			{
				return static_cast<char>(tolower(c));
			});

		//trueなら90度回転させる
		//falseなら回転させない

		if (lower.find("xbot") != string::npos)
		{
			return false;
		}

		if (lower.find("gusoku") != string::npos)
		{
			return false;
		}

		if (lower.find("tree") != string::npos)
		{
			return false;
		}

		if (lower.find("kacchatta_hone") != string::npos)
		{
			return false;
		}

		if (lower.find("alicia") != string::npos)
		{
			return true;
		}

		if (lower.find("moca") != string::npos)
		{
			return true;
		}
		
		// それ以外のモデルは基本的に回転させる
		return true;
	}

	XMFLOAT3 GetDefaultModelRotationByPath(const filesystem::path& path, bool isConvert)
	{
		string lower = path.generic_string();
		transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
			{
				return static_cast<char>(tolower(c));
			});

		if (lower.find("moca") != string::npos)
		{
			return { XM_PIDIV2, 0.0f, 0.0f };
		}

		if (lower.find("kacchatta_hone") != string::npos)
		{
			return { 0.0f, 0.0f, 0.0f };
		}

		if (filesystem::path(lower).extension() == ".vrm")
		{
			return { 0.0f, 0.0f, 0.0f };
		}

		if (lower.find("xbot") != string::npos)
		{
			return { 0.0f, XMConvertToRadians(160.0f), 0.0f };
		}

		return isConvert ? XMFLOAT3(XM_PIDIV2, 0.0f, 0.0f) : XMFLOAT3(0.0f, 0.0f, 0.0f);
	}

	bool DrawScaleAxisFloat3(const char* label, XMFLOAT3& value, float speed, float minValue, float maxValue)
	{
		float values[3] = { value.x, value.y, value.z };
		if (!DrawAxisFloat3(label, values, speed, minValue, maxValue))
		{
			return false;
		}

		value = { values[0], values[1], values[2] };
		return true;
	}

	void DrawFpsMeter()
	{
		const float fps = World::GetFrameRate();
		const int targetFrameRate = World::GetTargetFrameRate();
		const float referenceFps = static_cast<float>(max(targetFrameRate, 60));
		const float fraction = referenceFps > 0.0f ? min(fps / referenceFps, 1.0f) : 0.0f;
		char overlay[32]{};
		snprintf(overlay, sizeof(overlay), "%.0f / %.0f FPS", fps, referenceFps);
		const float overlayWidth = ImGui::CalcTextSize(overlay).x;
		const float barWidth = max(80.0f, ImGui::GetContentRegionAvail().x - overlayWidth - 10.0f);

		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.18f, 0.68f, 0.82f, 1.00f));
		ImGui::ProgressBar(fraction, ImVec2(barWidth, 7.0f), "");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextDisabled("%s", overlay);
	}

	bool GetLocalAabb(EntityID entity, XMFLOAT3& center, XMFLOAT3& extents)
	{
		if (ComponentManager::HasComponent<AABBComponent>(entity))
		{
			const auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
			center = aabb.Center;
			extents = aabb.Extents;
			return true;
		}

		if (ComponentManager::HasComponent<StaticModelComponent>(entity))
		{
			const auto& model = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
			if (auto* resource = ModelManager::GetStaticModel(model.ModelId))
			{
				center = resource->GetAabbCenter();
				extents = resource->GetAabbExtents();
				return true;
			}
		}

		if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
		{
			const auto& model = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			if (auto* resource = ModelManager::GetAnimModel(model.ModelId))
			{
				center = resource->GetAabbCenter();
				extents = resource->GetAabbExtents();
			}
			else
			{
				center = { 0.0f, 1.0f, 0.0f };
				extents = { 0.7f, 1.8f, 0.7f };
			}
			return true;
		}

		if (ComponentManager::HasComponent<SpriteComponent>(entity))
		{
			center = { 0.0f, 0.0f, 0.0f };
			extents = { 1.0f, 1.0f, 0.05f };
			return true;
		}

		return false;
	}

	bool IsModelMaterialEntity(EntityID entity)
	{
		return ComponentManager::HasComponent<StaticModelComponent>(entity) ||
			ComponentManager::HasComponent<AnimationModelComponent>(entity);
	}

	template<typename T>
	void RestoreSnapshotComponent(EntityID entity, const T& component)
	{
		if (!ComponentManager::HasComponent<T>(entity))
		{
			ComponentManager::AddComponent(entity, ComponentTypeTraits<T>::value());
		}
		ComponentManager::GetComponentUnchecked<T>(entity) = component;
	}

	void ApplyLightEntityToRuntime(EntityID entity)
	{
		if (entity == g_kINVALID_ENTITY ||
			!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<LightComponent>(entity) ||
			!ComponentManager::HasComponent<TransformComponent>(entity))
		{
			return;
		}

		auto& lightComponent = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (!lightComponent.IsActive)
		{
			return;
		}

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		if (lightComponent.Type == LightType::Spot || lightComponent.Type == LightType::Volume)
		{
			XMFLOAT3 target = { 0.0f, 0.0f, 0.0f };
			Entity alicia = World::GetEntityByName("Alicia");
			if (alicia.IsValid() &&
				Registry::IsAlive(alicia.GetID()) &&
				ComponentManager::HasComponent<TransformComponent>(alicia.GetID()))
			{
				const auto& aliciaTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(alicia.GetID());
				target = { aliciaTransform.Position.x, aliciaTransform.Position.y, aliciaTransform.Position.z };
			}

			XMVECTOR dir = XMVectorSubtract(XMLoadFloat3(&target), XMLoadFloat3(&transform.Position));
			if (XMVectorGetX(XMVector3LengthSq(dir)) > 0.000001f)
			{
				XMStoreFloat3(&lightComponent.Direction, XMVector3Normalize(dir));
			}
		}

	}

	void RefreshLightEntityName(EntityID entity, LightType type)
	{
		if (entity == g_kINVALID_ENTITY ||
			!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<NameComponent>(entity))
		{
			return;
		}

		auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
		const char* typeName = "Unknown";
		switch (type)
		{
		case LightType::Directional: typeName = "Directional"; break;
		case LightType::Point: typeName = "Point"; break;
		case LightType::Spot: typeName = "Spot"; break;
		case LightType::Volume: typeName = "Volume"; break;
		}
		name.Name = string(typeName) + " Light";
		World::RegisterName(entity, name.Name);
	}

}

bool ImGuiManager::Init(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* commandQueue, int numFrames, DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* cbvHeap, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
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
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void ImGuiManager::Update()
{
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
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
	if (m_ShowAabbForSelectedOnly)
	{
		for (EntityID entity : World::GetView<AABBComponent>())
		{
			ComponentManager::GetComponentUnchecked<AABBComponent>(entity).DrawDebug = (entity == m_SelectedEntity);
		}
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
	if (m_ShowAssetBrowser)
	{
		DrawAssetBrowserWindow();
	}
	if (m_ShowRenderDebugger)
	{
		DrawRenderDebuggerWindow();
	}
	if (m_ShowGBufferWindow)
	{
		DrawGBufferWindow();
	}
	if (m_ShowLogWindow)
	{
		DrawLogWindow();
	}
	if (m_ShowPerformanceWindow)
	{
		DrawPerformanceWindow();
	}
	if (m_ShowMaterialEditorWindow)
	{
		DrawMaterialEditorWindow();
	}
	if (m_ShowMeshOutlineWindow)
	{
		DrawMeshOutlineWindow();
	}
	if (m_ShowMeshShadingWindow)
	{
		DrawMeshShadingWindow();
	}

	if (!m_ShowAdjustmentPanel)
	{
		FinalizeUndoCaptureIfIdle();
		return;
	}
ImGui::Begin("調整");

	if (RendererCore::GetRequestedRenderMode() == RenderMode::DEFERRED)
	{
		m_renderMode = 1;
	}
	else
	{
		Debug::Log("不明な描画モード: %d", static_cast<int>(RendererCore::GetRequestedRenderMode()));
	}

	if (Camera::GetCameraPostProcess() == PostProcessType::NONE)
	{
		m_cameraPostProcess = 0;
	}
	else if (Camera::GetCameraPostProcess() == PostProcessType::BLUR)
	{
		m_cameraPostProcess = 1;
	}
	else if (Camera::GetCameraPostProcess() == PostProcessType::SEPIA)
	{
		m_cameraPostProcess = 2;
	}
	else if (Camera::GetCameraPostProcess() == PostProcessType::GRAYSCALE)
	{
		m_cameraPostProcess = 3;
	}
	else if (Camera::GetCameraPostProcess() == PostProcessType::INVERT)
	{
		m_cameraPostProcess = 4;
	}
	else
	{
		Debug::Log("不明なカメラポストプロセス: %d", static_cast<int>(Camera::GetCameraPostProcess()));
	}

	ImGui::TextUnformatted("描画モード: Deferred");

	if (ImGui::Combo("カメラポストプロセス", &m_cameraPostProcess, m_cameraPostProcessModeItems, IM_ARRAYSIZE(m_cameraPostProcessModeItems)))
	{
		if (m_cameraPostProcess == 0)
		{
			Camera::SetCameraPostProcess(PostProcessType::NONE);
		}
		else if (m_cameraPostProcess == 1)
		{
			Camera::SetCameraPostProcess(PostProcessType::BLUR);
		}
		else if (m_cameraPostProcess == 2)
		{
			Camera::SetCameraPostProcess(PostProcessType::SEPIA);
		}
		else if (m_cameraPostProcess == 3)
		{
			Camera::SetCameraPostProcess(PostProcessType::GRAYSCALE);
		}
		else if (m_cameraPostProcess == 4)
		{
			Camera::SetCameraPostProcess(PostProcessType::INVERT);
		}
		else
		{
			Debug::Log("不明なカメラポストプロセス: %d", m_cameraPostProcess);
		}
	}

	ImGui::SeparatorText("セクション");
	if (ImGui::Checkbox("HDR有効", &m_HdrEnabled))
	{
		RendererCore::SetHdr(m_HdrEnabled);
	}
	ImGui::Checkbox("ACESトーンマップ", &m_ToneMapEnabled);
	ImGui::SetNextItemWidth(200.0f);
	ImGui::SliderFloat("露光", &m_Exposure, 0.01f, 10.0f, "%.2f");

	ImGui::SeparatorText("セクション");
	ImGui::SetNextItemWidth(200.0f);
	float ppIntensity = Camera::GetCameraPostProcessIntensity();
	if (ImGui::SliderFloat("ポストプロセス強度", &ppIntensity, 0.01f, 1.0f, "%.2f"))
	{
		Camera::SetCameraPostProcessIntensity(ppIntensity);
	}

	ImGui::Checkbox("Gバッファ", &m_ShowGBufferWindow);

	ImGui::SeparatorText("パフォーマンス");
	ImGui::Text("FPS: %.1f", World::GetFrameRate());
	DrawFpsMeter();
	ImGui::Text("Frame: %.2f ms", World::GetFrameTimeMs());
	bool vsyncEnabled = World::IsVSyncEnabled();
	if (ImGui::Checkbox("垂直同期", &vsyncEnabled))
	{
		World::SetVSyncEnabled(vsyncEnabled);
	}
	bool fixedFrameRateEnabled = World::IsFixedFrameRateEnabled();
	if (ImGui::Checkbox("FPS固定", &fixedFrameRateEnabled))
	{
		World::SetFixedFrameRateEnabled(fixedFrameRateEnabled);
	}
	int targetFrameRate = World::GetTargetFrameRate();
	ImGui::SetNextItemWidth(120.0f);
	if (ImGui::SliderInt("目標FPS", &targetFrameRate, 15, 360))
	{
		World::SetTargetFrameRate(targetFrameRate);
	}
	ImGui::TextDisabled("VSync有効時はモニターの更新間隔も上限になります");
	ImGui::End();
	FinalizeUndoCaptureIfIdle();
	return;

	LightComponent* activeLight = nullptr;
	for (EntityID entity : World::GetView<LightComponent>())
	{
		auto& candidate = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (candidate.IsActive)
		{
			activeLight = &candidate;
			break;
		}
		if (!activeLight)
		{
			activeLight = &candidate;
		}
	}
	XMFLOAT3 lightDirection = activeLight ? activeLight->Direction : XMFLOAT3(0.0f, 1.0f, 0.0f);
	XMFLOAT4 lightColor = activeLight ? activeLight->Color : XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	float lightIntensity = activeLight ? activeLight->Intensity : 1.0f;
	float lightRange = activeLight ? activeLight->Range : 1.0f;

	EntityID materialEntity = g_kINVALID_ENTITY;
	if (m_SelectedEntity != g_kINVALID_ENTITY &&
		Registry::IsAlive(m_SelectedEntity) &&
		ComponentManager::HasComponent<MaterialComponent>(m_SelectedEntity))
	{
		materialEntity = m_SelectedEntity;
	}
	else
	{
		auto materialEntities = World::GetView<MaterialComponent>();
		for (EntityID entity : materialEntities)
		{
			materialEntity = entity;
			break;
		}
	}
	MaterialComponent* material = (materialEntity != g_kINVALID_ENTITY)
		? &ComponentManager::GetComponentUnchecked<MaterialComponent>(materialEntity)
		: nullptr;
	EntitySnapshot materialBefore = (materialEntity != g_kINVALID_ENTITY) ? CaptureEntity(materialEntity) : EntitySnapshot{};
	auto captureMaterialUndo = [&]()
		{
			if (materialEntity != g_kINVALID_ENTITY)
			{
				BeginUndoCapture(materialEntity, materialBefore);
			}
		};

	MaterialComponent defaultMaterial{};
	MaterialComponent& materialValues = material ? *material : defaultMaterial;
	float normalBlend = materialValues.NormalBlend;
	float normalBias = materialValues.NormalBias;
	float baseSaturation = materialValues.BaseSaturation;
	float baseBrightness = materialValues.BaseBrightness;
	float shadowThreshold = materialValues.ShadowThreshold;
	float shadowSoftness = materialValues.ShadowSoftness;
	float shadowStrength = materialValues.ShadowStrength;
	float midStrength = materialValues.MidStrength;
	float litStrength = materialValues.LitStrength;
	float rimStrength = materialValues.RimStrength;
	float rimThreshold = materialValues.RimThreshold;
	float specularStrength = materialValues.SpecularStrength;
	float specularThreshold = materialValues.SpecularThreshold;
	float kawaiiBlend = materialValues.KawaiiBlend;
	float skinScatterStrength = materialValues.SkinScatterStrength;
	float skinScatterWrap = materialValues.SkinScatterWrap;
	float skinBacklightStrength = materialValues.SkinBacklightStrength;
	float skinRimScatterStrength = materialValues.SkinRimScatterStrength;
	float skinOilSpecularStrength = materialValues.SkinOilSpecularStrength;
	float skinShadowScatter = materialValues.SkinShadowScatter;
	float castShadowThreshold = materialValues.CastShadowThreshold;
	float castShadowSoftness = materialValues.CastShadowSoftness;

	bool changed = false;
ImGui::SeparatorText("セクション");
	if (changed |= ImGui::DragFloat3("ライト方向", &lightDirection.x, 0.01f, -1.0f, 1.0f))
	{
		if (activeLight) activeLight->Direction = lightDirection;
	}
	if (changed |= ImGui::ColorEdit3("ライト色", &lightColor.x))
	{
		if (activeLight) activeLight->Color = lightColor;
	}
if (changed |= ImGui::DragFloat("ライト強度", &lightIntensity, 0.01f, 0.0f, 5.0f))
	{
		if (activeLight) activeLight->Intensity = lightIntensity;
	}
if (ImGui::DragFloat("ライト範囲", &lightRange, 0.05f, 0.01f, 8.0f))
	{
		if (activeLight) activeLight->Range = lightRange;
	}

	ImGui::SeparatorText("セクション");
	if (!material)
	{
		ImGui::BeginDisabled();
	}
	if (ImGui::SliderFloat("法線ブレンド", &normalBlend, 0.0f, 1.0f))
	{
		captureMaterialUndo();
		material->NormalBlend = normalBlend;
	}
	if (ImGui::SliderFloat("法線バイアス", &normalBias, -1.0f, 1.0f))
	{
		captureMaterialUndo();
		material->NormalBias = normalBias;
	}
	if (ImGui::SliderFloat("ベース彩度", &baseSaturation, 0.0f, 3.0f))
	{
		captureMaterialUndo();
		material->BaseSaturation = baseSaturation;
	}
if (ImGui::SliderFloat("ベース明度", &baseBrightness, 0.0f, 3.0f))
	{
		captureMaterialUndo();
		material->BaseBrightness = baseBrightness;
	}
if (ImGui::SliderFloat("かわいいブレンド", &kawaiiBlend, 0.0f, 1.0f))
	{
		captureMaterialUndo();
		material->KawaiiBlend = kawaiiBlend;
	}

	if (ImGui::TreeNode("影"))
	{
		if (ImGui::SliderFloat("影しきい値", &shadowThreshold, 0.0f, 1.0f))
		{
			captureMaterialUndo();
			material->ShadowThreshold = shadowThreshold;
		}
		if (ImGui::SliderFloat("影ぼかし", &shadowSoftness, 0.0f, 0.5f))
		{
			captureMaterialUndo();
			material->ShadowSoftness = shadowSoftness;
		}
		if (ImGui::SliderFloat("影の強さ", &shadowStrength, 0.0f, 2.0f))
		{
			captureMaterialUndo();
			material->ShadowStrength = shadowStrength;
		}
		if (ImGui::SliderFloat("中間色の強さ", &midStrength, 0.0f, 2.0f))
		{
			captureMaterialUndo();
			material->MidStrength = midStrength;
		}
		if (ImGui::SliderFloat("明部の強さ", &litStrength, 0.0f, 2.0f))
		{
			captureMaterialUndo();
			material->LitStrength = litStrength;
		}
		if (ImGui::SliderFloat("影しきい値", &castShadowThreshold, 0.0f, 1.0f))
		{
			captureMaterialUndo();
			material->CastShadowThreshold = castShadowThreshold;
		}
		if (ImGui::SliderFloat("影ぼかし", &castShadowSoftness, 0.0f, 0.5f))
		{
			captureMaterialUndo();
			material->CastShadowSoftness = castShadowSoftness;
		}
		ImGui::TreePop();
	}

		if (ImGui::TreeNode("ハイライト"))
		{
		if (ImGui::SliderFloat("リム強度", &rimStrength, 0.0f, 2.0f))
		{
			captureMaterialUndo();
			material->RimStrength = rimStrength;
		}
		if (ImGui::SliderFloat("リムしきい値", &rimThreshold, 0.0f, 1.0f))
		{
			captureMaterialUndo();
			material->RimThreshold = rimThreshold;
		}
		if (ImGui::SliderFloat("スペキュラ強度", &specularStrength, 0.0f, 2.0f))
		{
			captureMaterialUndo();
			material->SpecularStrength = specularStrength;
		}
		if (ImGui::SliderFloat("スペキュラしきい値", &specularThreshold, 0.0f, 1.0f))
		{
			captureMaterialUndo();
			material->SpecularThreshold = specularThreshold;
		}
		ImGui::TreePop();
		}
	
	if (ImGui::TreeNode("肌"))
	{
		if (ImGui::SliderFloat("肌散乱強度", &skinScatterStrength, 0.0f, 3.0f))
		{
			captureMaterialUndo();
			material->SkinScatterStrength = skinScatterStrength;
		}
		if (ImGui::SliderFloat("肌散乱ラップ", &skinScatterWrap, 0.0f, 1.0f))
		{
			captureMaterialUndo();
			material->SkinScatterWrap = skinScatterWrap;
		}
		if (ImGui::SliderFloat("肌逆光強度", &skinBacklightStrength, 0.0f, 3.0f))
		{
			captureMaterialUndo();
			material->SkinBacklightStrength = skinBacklightStrength;
		}
		if (ImGui::SliderFloat("肌リム散乱強度", &skinRimScatterStrength, 0.0f, 3.0f))
		{
			captureMaterialUndo();
			material->SkinRimScatterStrength = skinRimScatterStrength;
		}
		if (ImGui::SliderFloat("スペキュラ強度", &skinOilSpecularStrength, 0.0f, 3.0f))
		{
			captureMaterialUndo();
			material->SkinOilSpecularStrength = skinOilSpecularStrength;
		}
		if (ImGui::SliderFloat("肌影散乱", &skinShadowScatter, 0.0f, 1.0f))
		{
			captureMaterialUndo();
			material->SkinShadowScatter = skinShadowScatter;
		}
		ImGui::TreePop();
	}
	if (!material)
	{
		ImGui::EndDisabled();
	}

	float cameraPostProcessIntensity = Camera::GetCameraPostProcessIntensity();
	ImGui::SeparatorText("セクション");
	if (ImGui::SliderFloat("ポストプロセス強度", &cameraPostProcessIntensity, 0.01f, 1.0f))
	{
		Camera::SetCameraPostProcessIntensity(cameraPostProcessIntensity);
	}

	if (ImGui::Button("ライトをリセット"))
	{
		if (activeLight)
		{
			activeLight->Type = LightType::Directional;
			activeLight->Direction = { 0.0f, 1.0f, 0.0f };
			activeLight->Color = { 1.0f, 1.0f, 1.0f, 1.0f };
			activeLight->Intensity = 1.0f;
			activeLight->Range = 1.0f;
			activeLight->InnerAngle = 18.0f;
			activeLight->OuterAngle = 32.0f;
			activeLight->VolumeDensity = 0.35f;
			activeLight->VolumeShape = 0;
			activeLight->IsActive = true;
		}
	}

	if (RenderMode::DEFERRED == RendererCore::GetRenderMode())
	{
		ImGui::SeparatorText("セクション");
		ImGui::BeginChild("Gバッファ", ImVec2(0, 600), true);

		const ImVec2 previewSize(320.0f, 180.0f);
		const float cellSpacing = 12.0f;
		const int columns = 2;

		const struct
		{
			const char* Label;
			GBufferType Type;
		}

		cells[] =
		{
		{ "ベースカラー",     GBufferType::BASE_COLOR },
		{ "法線",       GBufferType::NORMAL },
		{ "位置", GBufferType::POSITION },
		{ "深度",       GBufferType::DEPTH },
		{ "マテリアル", GBufferType::MATERIAL },
		};

		const int cellCount = int(size(cells));
		const int rowCount = (cellCount + columns - 1) / columns;

		for (int row = 0; row < rowCount; ++row)
		{
			for (int col = 0; col < columns; ++col)
			{
				const int index = row * columns + col;
				if (index >= cellCount)
				{
					break;
				}

				ImGui::BeginGroup();
				ImGui::TextUnformatted(cells[index].Label);
				ImGui::Image(
					ImTextureID(RendererDraw::GetGBufferSrvHandle(cells[index].Type).ptr),
					previewSize);
				ImGui::EndGroup();

				if (col < columns - 1 && index + 1 < cellCount)
				{
					ImGui::SameLine(0.0f, cellSpacing);
				}
			}

			if (row < rowCount - 1)
			{
				ImGui::Dummy(ImVec2(0.0f, cellSpacing));
			}
		}

		ImGui::EndChild();
	}

	ImGui::End();
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

	ImGui::TextUnformatted("W/E/R: 移動 / 回転 / 拡縮");
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

void ImGuiManager::DrawSceneViewWindow()
{
	ImGui::SetNextWindowSize(ImVec2(860.0f, 520.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("シーンビュー"))
	{
		ImGui::End();
		return;
	}

	ImVec2 available = ImGui::GetContentRegionAvail();
	const float sceneAspect = RendererCore::GetSceneAspectRatio();
	if (available.x <= 1.0f || available.y <= 1.0f)
	{
		ImGui::End();
		return;
	}

	ImVec2 imageSize = available;
	if (sceneAspect > 0.001f)
	{
		const float availableAspect = available.x / available.y;
		if (availableAspect > sceneAspect)
		{
			imageSize.x = available.y * sceneAspect;
		}
		else
		{
			imageSize.y = available.x / sceneAspect;
		}
	}

	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	m_SceneViewPos = cursor;
	m_SceneViewSize = imageSize;

	D3D12_GPU_DESCRIPTOR_HANDLE handle = RendererDraw::GetEditorSceneSrvHandle();
	ImGui::Image((ImTextureData*)handle.ptr, imageSize);
	m_IsSceneViewHovered = ImGui::IsItemHovered();
	if (m_IsSceneViewHovered && fabsf(ImGui::GetIO().MouseWheel) > 0.001f)
	{
		EntityID cameraEntity = Camera::GetCameraEntity();
		if (cameraEntity != g_kINVALID_ENTITY &&
			Registry::IsAlive(cameraEntity) &&
			ComponentManager::HasComponent<CameraComponent>(cameraEntity) &&
			ComponentManager::HasComponent<TransformComponent>(cameraEntity))
		{
			auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
			auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
			XMVECTOR eye = XMLoadFloat3(&transform.Position);
			XMVECTOR target = XMLoadFloat3(&camera.Target);
			XMVECTOR forward = XMVectorSubtract(target, eye);
			if (XMVectorGetX(XMVector3LengthSq(forward)) <= 0.000001f)
			{
				forward = XMVector3TransformNormal(
					XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
					XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, transform.Rotation.z));
			}
			forward = XMVector3Normalize(forward);
			const float zoomStep = 1.15f;
			const float wheel = ImGui::GetIO().MouseWheel;
			XMVECTOR delta = XMVectorScale(forward, wheel * zoomStep);
			eye = XMVectorAdd(eye, delta);
			target = XMVectorAdd(target, delta);
			XMStoreFloat3(&transform.Position, eye);
			XMStoreFloat3(&camera.Target, target);
			camera.LockOnTarget = g_kINVALID_ENTITY;
		transform.IsDirty = true;
		}
	}

	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
		{
			const char* path = static_cast<const char*>(payload->Data);
			if (path)
			{
				ImVec2 sceneMouse = ImGui::GetIO().MousePos;
				sceneMouse.x -= m_SceneViewPos.x;
				sceneMouse.y -= m_SceneViewPos.y;
				PlaceAssetInScene(path, sceneMouse);
			}
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::TextUnformatted("右ドラッグ: カメラ / 左クリック: 選択 / ドラッグ&ドロップ: 配置または適用");
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
		ImGui::MenuItem("調整", nullptr, &m_ShowAdjustmentPanel);
		ImGui::MenuItem("アセット", nullptr, &m_ShowAssetBrowser);
		ImGui::MenuItem("描画デバッグ", nullptr, &m_ShowRenderDebugger);
		ImGui::MenuItem("Gバッファ", nullptr, &m_ShowGBufferWindow);
		ImGui::MenuItem("ログ", nullptr, &m_ShowLogWindow);
		ImGui::MenuItem("パフォーマンス", nullptr, &m_ShowPerformanceWindow);
		ImGui::MenuItem("マテリアルエディター", nullptr, &m_ShowMaterialEditorWindow);
		ImGui::Separator();
		ImGui::MenuItem("メッシュ単位のアウトライン", nullptr, &m_ShowMeshOutlineWindow);
		ImGui::MenuItem("メッシュ単位のシェーディング", nullptr, &m_ShowMeshShadingWindow);
		ImGui::EndMenu();
	}
	ImGui::SameLine();
	ImGui::Checkbox("選択AABBのみ", &m_ShowAabbForSelectedOnly);

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
		ImGui::EndMenu();
	}

	if (m_SelectedEntity != g_kINVALID_ENTITY)
	{
		ImGui::SameLine();
		ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	}

	ImGui::EndMainMenuBar();
}

void ImGuiManager::DrawPerformanceWindow()
{
	ImGui::SetNextWindowSize(ImVec2(260.0f, 150.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("パフォーマンス", &m_ShowPerformanceWindow, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("FPS: %.1f", World::GetFrameRate());
	DrawFpsMeter();
	ImGui::Text("Frame: %.2f ms", World::GetFrameTimeMs());
	ImGui::Separator();

	bool vsyncEnabled = World::IsVSyncEnabled();
	if (ImGui::Checkbox("垂直同期", &vsyncEnabled))
	{
		World::SetVSyncEnabled(vsyncEnabled);
	}

	bool fixedFrameRateEnabled = World::IsFixedFrameRateEnabled();
	if (ImGui::Checkbox("FPS固定", &fixedFrameRateEnabled))
	{
		World::SetFixedFrameRateEnabled(fixedFrameRateEnabled);
	}

	int targetFrameRate = World::GetTargetFrameRate();
	ImGui::BeginDisabled(!fixedFrameRateEnabled);
	ImGui::SetNextItemWidth(120.0f);
	if (ImGui::SliderInt("目標FPS", &targetFrameRate, 15, 360))
	{
		World::SetTargetFrameRate(targetFrameRate);
	}
	ImGui::EndDisabled();

	ImGui::End();
}

void ImGuiManager::DrawAssetBrowserWindow()
{
	ImGui::SetNextWindowSize(ImVec2(720.0f, 260.0f), ImGuiCond_FirstUseEver);
if (!ImGui::Begin("プロジェクト", &m_ShowAssetBrowser))
	{
		ImGui::End();
		return;
	}

	error_code ec;
	if (!filesystem::exists(m_AssetRoot, ec))
	{
		filesystem::create_directories(m_AssetRoot, ec);
	}
	if (!filesystem::exists(m_CurrentAssetDirectory, ec) || !filesystem::is_directory(m_CurrentAssetDirectory, ec))
	{
		m_CurrentAssetDirectory = m_AssetRoot;
	}

	ImGui::Text("現在: %s", m_CurrentAssetDirectory.generic_string().c_str());
	if (m_CurrentAssetDirectory != m_AssetRoot)
	{
		ImGui::SameLine();
		if (ImGui::Button("上へ"))
		{
			m_CurrentAssetDirectory = m_CurrentAssetDirectory.parent_path();
		}
	}

	static char folderName[128] = "NewFolder";
	static char fileName[128] = "NewMaterial.txt";
	ImGui::InputText("フォルダ名", folderName, IM_ARRAYSIZE(folderName));
	ImGui::SameLine();
	if (ImGui::Button("フォルダ作成") && folderName[0] != '\0')
	{
		filesystem::create_directories(m_CurrentAssetDirectory / folderName, ec);
	}
	ImGui::InputText("ファイル名", fileName, IM_ARRAYSIZE(fileName));
	ImGui::SameLine();
	if (ImGui::Button("ファイル作成") && fileName[0] != '\0')
	{
		const filesystem::path newPath = m_CurrentAssetDirectory / fileName;
		CreateAssetFile(newPath);
	}

	ImGui::SeparatorText("セクション");
	ImGui::BeginChild("AssetList", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
	if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_SelectedAssetPath.empty())
	{
		DeleteAssetPath(m_SelectedAssetPath);
		m_SelectedAssetPath.clear();
	}
	if (ImGui::BeginPopupContextWindow("AssetBackgroundContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		if (ImGui::MenuItem("フォルダ作成"))
		{
			filesystem::path path = m_CurrentAssetDirectory / folderName;
			filesystem::create_directories(path, ec);
			AddLog("フォルダ作成: %s", path.generic_string().c_str());
		}
		if (ImGui::MenuItem("空ファイル作成"))
		{
			CreateAssetFile(m_CurrentAssetDirectory / fileName);
		}
		if (ImGui::MenuItem("マテリアルファイル作成"))
		{
			CreateAssetFile(m_CurrentAssetDirectory / "NewMaterial.material");
		}
		ImGui::EndPopup();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
		{
			const char* src = static_cast<const char*>(payload->Data);
			if (src && src[0] != '\0')
			{
				ImportAssetFile(src);
			}
		}
		ImGui::EndDragDropTarget();
	}

	vector<filesystem::directory_entry> entries;
	for (const auto& entry : filesystem::directory_iterator(m_CurrentAssetDirectory, ec))
	{
		entries.push_back(entry);
	}
	sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
		{
			if (a.is_directory() != b.is_directory())
			{
				return a.is_directory() > b.is_directory();
			}
			return a.path().filename().generic_string() < b.path().filename().generic_string();
		});

	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(entries.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			const filesystem::path path = entries[i].path();
			const bool isDir = entries[i].is_directory();
			const string fileName = path.filename().generic_string();
			const string displayName = fileName.empty() ? path.generic_string() : fileName;
			const string itemId = path.generic_string();

			bool selected = false;
			if (!m_SelectedAssetPath.empty())
			{
				selected = filesystem::equivalent(path, m_SelectedAssetPath, ec);
				ec.clear();
			}

			ImGui::PushID(itemId.c_str());

			ImVec2 cursorPos = ImGui::GetCursorScreenPos();
			ImVec2 iconSize(16, 16);

			if (isDir)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				ImVec4 yellow(1.0f, 0.85f, 0.2f, 1.0f);
				drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + iconSize.x, cursorPos.y + iconSize.y), ImGui::ColorConvertFloat4ToU32(yellow), 2.0f);
				drawList->AddRect(ImVec2(cursorPos.x + 3, cursorPos.y + 2), ImVec2(cursorPos.x + 11, cursorPos.y + 8), ImGui::ColorConvertFloat4ToU32(ImVec4(0.8f, 0.65f, 0.1f, 1.0f)), 1.0f);
				drawList->AddRectFilled(ImVec2(cursorPos.x + 3, cursorPos.y + 7), ImVec2(cursorPos.x + 13, cursorPos.y + 14), ImGui::ColorConvertFloat4ToU32(yellow), 2.0f);
			}
			else
			{
				const string ext = path.extension().generic_string();
				ImVec4 fileColor(0.6f, 0.8f, 1.0f, 1.0f);
				if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".tga")
					fileColor = ImVec4(0.4f, 0.9f, 0.5f, 1.0f);
				else if (ext == ".fbx" || ext == ".obj")
					fileColor = ImVec4(1.0f, 0.6f, 0.3f, 1.0f);
				else if (ext == ".hlsl" || ext == ".slang")
					fileColor = ImVec4(0.8f, 0.4f, 1.0f, 1.0f);

				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + iconSize.x - 2, cursorPos.y + iconSize.y), ImGui::ColorConvertFloat4ToU32(fileColor), 2.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 2), ImVec2(cursorPos.x + 12, cursorPos.y + 2), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 6), ImVec2(cursorPos.x + 12, cursorPos.y + 6), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
				drawList->AddLine(ImVec2(cursorPos.x + 6, cursorPos.y + 10), ImVec2(cursorPos.x + 10, cursorPos.y + 10), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.6f)), 1.0f);
			}

			ImGui::Dummy(iconSize);
			ImGui::SameLine(0.0f, 6.0f);

			if (ImGui::Selectable(displayName.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
			{
				m_SelectedAssetPath = path;
				if (isDir)
				{
					m_CurrentAssetDirectory = path;
					m_SelectedAssetPath.clear();
				}
				else if (IsTextureFile(path))
				{
					const string relative = MakeRelativeAssetPath(path);
					TextureManager::LoadTexture(relative.c_str());
					if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
					{
						MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
					}
				}
			}

			if (ImGui::BeginPopupContextItem("AssetItemContext"))
			{
				m_SelectedAssetPath = path;
				ImGui::TextUnformatted(path.filename().generic_string().c_str());
				ImGui::Separator();
				if (isDir && ImGui::MenuItem("開く"))
				{
					m_CurrentAssetDirectory = path;
					m_SelectedAssetPath.clear();
				}
				if (!isDir && IsTextureFile(path) && ImGui::MenuItem("選択中にテクスチャ適用"))
				{
					const string relative = MakeRelativeAssetPath(path);
					if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
					{
						MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
						AddLog("テクスチャ適用: %s", relative.c_str());
					}
				}
				if (ImGui::MenuItem("削除"))
				{
					DeleteAssetPath(path);
				}
				ImGui::EndPopup();
			}

			if (!isDir && ImGui::BeginDragDropSource())
			{
				const string relative = MakeRelativeAssetPath(path);
				ImGui::SetDragDropPayload("ASSET_PATH", relative.c_str(), relative.size() + 1);
				ImGui::TextUnformatted(relative.c_str());
				ImGui::EndDragDropSource();
			}

			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

void ImGuiManager::DrawRenderDebuggerWindow()
{
	ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("描画デバッガー", &m_ShowRenderDebugger))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("描画モード: %s", RendererCore::GetRenderMode() == RenderMode::DEFERRED ? "ディファード" : "フォワード");
	ImGui::Text("画面: %u x %u", RendererCore::GetSceneWidth(), RendererCore::GetSceneHeight());
	ImGui::Checkbox("選択AABBのみ", &m_ShowAabbForSelectedOnly);

	ImGui::SeparatorText("セクション");
	vector<TextureManager::TextureInfo> textures = TextureManager::GetLoadedTextureInfos();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(textures.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			const auto& tex = textures[i];
			ImGui::Text("#%d %ux%u %s", tex.SrvIndex, tex.Width, tex.Height, tex.Path.c_str());
		}
	}
	ImGui::End();
}

void ImGuiManager::DrawGBufferWindow()
{
	ImGui::SetNextWindowSize(ImVec2(700.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Gバッファ", &m_ShowGBufferWindow))
	{
		ImGui::End();
		return;
	}

	if (RenderMode::DEFERRED != RendererCore::GetRenderMode())
	{
		ImGui::TextUnformatted("ディファードモードのみ");
		ImGui::End();
		return;
	}

	ImGui::BeginChild("GBufferPreview", ImVec2(0, 0), true);

	const ImVec2 previewSize(320.0f, 180.0f);
	const float cellSpacing = 12.0f;
	const int columns = 2;
	const struct
	{
		const char* Label;
		GBufferType Type;
	}
	cells[] =
	{
		{ "ベースカラー",     GBufferType::BASE_COLOR },
		{ "法線",       GBufferType::NORMAL },
		{ "位置", GBufferType::POSITION },
		{ "深度",       GBufferType::DEPTH },
		{ "マテリアル", GBufferType::MATERIAL },
	};

	const int cellCount = int(size(cells));
	const int rowCount = (cellCount + columns - 1) / columns;
	for (int row = 0; row < rowCount; ++row)
	{
		for (int col = 0; col < columns; ++col)
		{
			const int index = row * columns + col;
			if (index >= cellCount)
			{
				break;
			}

			ImGui::BeginGroup();
			ImGui::TextUnformatted(cells[index].Label);
			ImGui::Image(
				ImTextureID(RendererDraw::GetGBufferSrvHandle(cells[index].Type).ptr),
				previewSize);
			ImGui::EndGroup();

			if (col < columns - 1 && index + 1 < cellCount)
			{
				ImGui::SameLine(0.0f, cellSpacing);
			}
		}

		if (row < rowCount - 1)
		{
			ImGui::Dummy(ImVec2(0.0f, cellSpacing));
		}
	}

	ImGui::EndChild();
	ImGui::End();
}

void ImGuiManager::DrawLogWindow()
{
	ImGui::SetNextWindowSize(ImVec2(620.0f, 220.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("ログ", &m_ShowLogWindow))
	{
		ImGui::End();
		return;
	}

	if (ImGui::Button("クリア"))
	{
		m_Logs.clear();
	}
	ImGui::SameLine();
	ImGui::Text("件数: %d", static_cast<int>(m_Logs.size()));
	ImGui::Separator();

	ImGui::BeginChild("LogScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(m_Logs.size()));
	while (clipper.Step())
	{
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			ImGui::TextUnformatted(m_Logs[i].c_str());
		}
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
	{
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::End();
}

void ImGuiManager::DrawMeshOutlineWindow()
{
	ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("メッシュ単位のアウトライン", &m_ShowMeshOutlineWindow))
	{
		ImGui::End();
		return;
	}

	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		ImGui::TextUnformatted("オブジェクト未選択");
		ImGui::End();
		return;
	}

	ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	ImGui::Separator();
	if (!IsModelMaterialEntity(m_SelectedEntity))
	{
		ImGui::TextUnformatted("メッシュ付きモデルを選択してください");
		ImGui::End();
		return;
	}

	DrawToonMeshOutlineInspector(m_SelectedEntity, false);
	ImGui::End();
}

void ImGuiManager::DrawMeshShadingWindow()
{
	ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("メッシュ単位のシェーディング", &m_ShowMeshShadingWindow))
	{
		ImGui::End();
		return;
	}

	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		ImGui::TextUnformatted("オブジェクト未選択");
		ImGui::End();
		return;
	}

	ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	ImGui::Separator();
	if (!IsModelMaterialEntity(m_SelectedEntity))
	{
		ImGui::TextUnformatted("メッシュ付きモデルを選択してください");
		ImGui::End();
		return;
	}

	DrawMeshShadingInspector(m_SelectedEntity, false);
	ImGui::End();
}

void ImGuiManager::DrawHierarchyWindow()
{
	ImGui::SetNextWindowPos(ImVec2(8.0f, 32.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(260.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("ヒエラルキー", &m_ShowEditorWindows))
	{
		ImGui::End();
		return;
	}

	if (ImGui::Button("選択解除"))
	{
		m_SelectedEntity = g_kINVALID_ENTITY;
	}

	ImGui::SeparatorText("セクション");
	for (EntityID entity : World::GetView<TransformComponent>())
	{
		if (!IsEditableEntity(entity))
		{
			continue;
		}

		const bool selected = entity == m_SelectedEntity;
		if (m_RenamingEntity == entity)
		{
			DrawRenameInput(entity);
		}
		else if (ImGui::Selectable(GetEntityDisplayName(entity), selected))
		{
			m_SelectedEntity = entity;
		}
	}

	ImGui::End();
}

void ImGuiManager::DrawInspectorWindow()
{
	ImGui::SetNextWindowSize(ImVec2(312.0f, 520.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("インスペクター"))
	{
		ImGui::End();
		return;
	}

	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
ImGui::TextUnformatted("オブジェクト未選択");
		ImGui::End();
		return;
	}

	if (m_RenamingEntity == m_SelectedEntity)
	{
		DrawRenameInput(m_SelectedEntity);
	}
	else
	{
		ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	}
	ImGui::Separator();

	if (ImGui::BeginPopupContextWindow("InspectorEntityContext", ImGuiPopupFlags_MouseButtonRight))
	{
		if (ImGui::MenuItem("削除"))
		{
			DeleteSelectedEntity();
			ImGui::EndPopup();
			ImGui::End();
			return;
		}
		ImGui::EndPopup();
	}

	if (ComponentManager::HasComponent<TransformComponent>(m_SelectedEntity) &&
		ImGui::CollapsingHeader("トランスフォーム", ImGuiTreeNodeFlags_DefaultOpen))
	{
		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(m_SelectedEntity);
		EntitySnapshot before = CaptureEntity(m_SelectedEntity);
		float rotationDeg[3] =
		{
			transform.Rotation.x * kRadToDeg,
			transform.Rotation.y * kRadToDeg,
			transform.Rotation.z * kRadToDeg
		};

		bool changed = false;
		changed |= DrawAxisFloat3("位置", transform.Position, 0.01f);
		if (DrawAxisFloat3("回転", rotationDeg, 0.1f))
		{
			transform.Rotation =
			{
				rotationDeg[0] * kDegToRad,
				rotationDeg[1] * kDegToRad,
				rotationDeg[2] * kDegToRad
			};
			changed = true;
		}
		changed |= DrawScaleAxisFloat3("スケール", transform.Scale, 0.01f, 0.001f, 100.0f);

		if (changed)
		{
			BeginUndoCapture(m_SelectedEntity, before);
			XMStoreFloat4x4(&transform.WorldMatrix, BuildWorldMatrix(transform));
			transform.IsDirty = true;
		}
	}

	if (ComponentManager::HasComponent<AABBComponent>(m_SelectedEntity) &&
		ImGui::CollapsingHeader("AABB", ImGuiTreeNodeFlags_DefaultOpen))
	{
		auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(m_SelectedEntity);
		EntitySnapshot before = CaptureEntity(m_SelectedEntity);
		bool changed = false;
		changed |= ImGui::Checkbox("デバッグ描画", &aabb.DrawDebug);
		changed |= ImGui::DragFloat3("AABB中心", &aabb.Center.x, 0.01f);
		changed |= ImGui::DragFloat3("AABBサイズ", &aabb.Extents.x, 0.01f, 0.001f, 100.0f);
		if (changed)
		{
			BeginUndoCapture(m_SelectedEntity, before);
		}

	}

	if (ComponentManager::HasComponent<LightComponent>(m_SelectedEntity))
	{
		DrawLightInspector(m_SelectedEntity);
	}
	DrawComponentInspector(m_SelectedEntity);

	ImGui::TextUnformatted("右クリック: カメラ / 左クリック: 選択");
	ImGui::End();
}

void ImGuiManager::DrawMaterialEditorWindow()
{
	ImGui::SetNextWindowSize(ImVec2(380.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("マテリアルエディター", &m_ShowMaterialEditorWindow))
	{
		ImGui::End();
		return;
	}

	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		ImGui::TextUnformatted("マテリアルを持つオブジェクトを選択してください");
		ImGui::End();
		return;
	}

	ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
	ImGui::Separator();

	if (!ComponentManager::HasComponent<MaterialComponent>(m_SelectedEntity))
	{
		ImGui::TextUnformatted("選択中のオブジェクトにマテリアルがありません");
		ImGui::End();
		return;
	}

	DrawMaterialInspector(m_SelectedEntity);
	ImGui::End();
}

void ImGuiManager::DrawMaterialInspector(EntityID entity)
{
	if (!ComponentManager::HasComponent<MaterialComponent>(entity))
	{
		return;
	}

	if (!ImGui::CollapsingHeader("マテリアル", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}

	auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
	EntitySnapshot before = CaptureEntity(entity);
	bool changed = false;

	const char* shaderModes[] = { "自動", "手動" };
	int shaderClassMode = static_cast<int>(material.ShaderClassMode);
	if (ImGui::Combo("シェーダークラスモード", &shaderClassMode, shaderModes, IM_ARRAYSIZE(shaderModes)))
	{
		material.ShaderClassMode = static_cast<MaterialMode>(shaderClassMode);
		changed = true;
	}
	const bool isManualMode = material.ShaderClassMode == MaterialMode::Manual;

	const char* shaderClasses[] = { "Transparent", "Hair", "Cloth", "Skin", "Toon", "Unlit", "Metallic", "Lit", "Eye", "PBR", "BRDF", "BTDF", "BSDF" };
	const int shaderClassValues[] =
	{
		static_cast<int>(ShaderClass::Transparent),
		static_cast<int>(ShaderClass::Hair),
		static_cast<int>(ShaderClass::Cloth),
		static_cast<int>(ShaderClass::Skin),
		static_cast<int>(ShaderClass::Toon),
		static_cast<int>(ShaderClass::Unlit),
		static_cast<int>(ShaderClass::Metallic),
		static_cast<int>(ShaderClass::Lit),
		static_cast<int>(ShaderClass::Eye),
		static_cast<int>(ShaderClass::PBR),
		static_cast<int>(ShaderClass::BRDF),
		static_cast<int>(ShaderClass::BTDF),
		static_cast<int>(ShaderClass::BSDF)
	};
	int shaderClassIndex = 5;
	for (int idx = 0; idx < IM_ARRAYSIZE(shaderClassValues); ++idx)
	{
		if (static_cast<int>(material.ShaderClass) == shaderClassValues[idx])
		{
			shaderClassIndex = idx;
			break;
		}
	}
	if (isManualMode)
	{
		if (ImGui::Combo("シェーダークラス", &shaderClassIndex, shaderClasses, IM_ARRAYSIZE(shaderClasses)))
		{
			const ShaderClass newShaderClass = static_cast<ShaderClass>(shaderClassValues[shaderClassIndex]);
			material.ShaderClass = newShaderClass;
			if (material.Alpha >= 0.999f)
			{
				if (newShaderClass == ShaderClass::BTDF)
				{
					material.Alpha = 0.45f;
				}
				else if (newShaderClass == ShaderClass::BSDF)
				{
					material.Alpha = 0.65f;
				}
				else if (newShaderClass == ShaderClass::Transparent)
				{
					material.Alpha = 0.5f;
				}
			}
			changed = true;
		}
		changed |= ImGui::SliderFloat("Metallic", &material.Metallic, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Roughness", &material.Roughness, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Fresnel", &material.Fresnel, 0.0f, 1.0f);
	}
	changed |= ImGui::SliderFloat("Alpha", &material.Alpha, 0.0f, 1.0f);

	ImGui::SeparatorText("セクション");
	const char* outlineModes[] = { "押し出し", "TEO", "MIX" };
	int outlineMode = static_cast<int>(material.ToonOutlineRenderMode);
	if (ImGui::Combo("アウトラインモード", &outlineMode, outlineModes, IM_ARRAYSIZE(outlineModes)))
	{
		material.ToonOutlineRenderMode = static_cast<ToonOutlineMode>(outlineMode);
		changed = true;
	}
	const char* teoModes[] = { "バランス", "境界", "ハードエッジ", "クリーン" };
	int teoMode = static_cast<int>(material.ToonTeoRenderMode);
	if (ImGui::Combo("TEOモード", &teoMode, teoModes, IM_ARRAYSIZE(teoModes)))
	{
		material.ToonTeoRenderMode = static_cast<ToonTeoMode>(teoMode);
		changed = true;
	}
	const char* outlineWidthModes[] = { "ワールド単位", "スクリーンピクセル" };
	int outlineWidthMode = static_cast<int>(material.ToonOutlineWidthModeSetting);
	if (ImGui::Combo("幅モード", &outlineWidthMode, outlineWidthModes, IM_ARRAYSIZE(outlineWidthModes)))
	{
		material.ToonOutlineWidthModeSetting = static_cast<ToonOutlineWidthMode>(outlineWidthMode);
		changed = true;
	}
	changed |= ImGui::SliderFloat("アウトライン幅", &material.ToonOutlineWidth, 0.0f, 0.20f);
	changed |= ImGui::SliderFloat("スクリーン幅px", &material.ToonOutlineScreenWidth, 0.0f, 24.0f, "%.1f");
	changed |= ImGui::SliderFloat("TEO幅スケール", &material.ToonOutlineTeoWidthScale, 0.0f, 3.0f);

	if (changed)
	{
		BeginUndoCapture(entity, before);
	}

	if (!isManualMode && IsModelMaterialEntity(entity))
	{
		if (ImGui::CollapsingHeader("シェーディングパラメータ", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool partChanged = false;
			for (int i = 0; i < IM_ARRAYSIZE(shaderClassValues); ++i)
			{
				const int classId = shaderClassValues[i];
				if (classId >= 0 && classId < kMaterialPartParamCount)
				{
					partChanged |= DrawMaterialPartParams(shaderClasses[i], material.PartParams[classId]);
				}
			}
			if (partChanged)
			{
				BeginUndoCapture(entity, before);
			}
		}
	}

	if (isManualMode && IsModelMaterialEntity(entity))
	{
		if (ImGui::CollapsingHeader("ディファード Toon / PBR", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool toonChanged = false;
			toonChanged |= ImGui::SliderFloat("法線ブレンド", &material.NormalBlend, 0.0f, 1.0f);
			toonChanged |= ImGui::SliderFloat("法線バイアス", &material.NormalBias, -1.0f, 1.0f);
			toonChanged |= ImGui::SliderFloat("ベース彩度", &material.BaseSaturation, 0.0f, 3.0f);
			toonChanged |= ImGui::SliderFloat("ベース明度", &material.BaseBrightness, 0.0f, 3.0f);
			toonChanged |= ImGui::SliderFloat("かわいいブレンド", &material.KawaiiBlend, 0.0f, 1.0f);
			if (toonChanged)
			{
				BeginUndoCapture(entity, before);
			}
		}

		if (ImGui::CollapsingHeader("影", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool shadowChanged = false;
			shadowChanged |= ImGui::SliderFloat("影しきい値", &material.ShadowThreshold, 0.0f, 1.0f);
			shadowChanged |= ImGui::SliderFloat("影ぼかし", &material.ShadowSoftness, 0.0f, 0.5f);
			shadowChanged |= ImGui::SliderFloat("影の強さ", &material.ShadowStrength, 0.0f, 2.0f);
			shadowChanged |= ImGui::SliderFloat("中間色の強さ", &material.MidStrength, 0.0f, 2.0f);
			shadowChanged |= ImGui::SliderFloat("明部の強さ", &material.LitStrength, 0.0f, 2.0f);
			shadowChanged |= ImGui::SliderFloat("影しきい値", &material.CastShadowThreshold, 0.0f, 1.0f);
			shadowChanged |= ImGui::SliderFloat("影ぼかし", &material.CastShadowSoftness, 0.0f, 0.5f);
			if (shadowChanged)
			{
				BeginUndoCapture(entity, before);
			}
		}

		if (ImGui::CollapsingHeader("ライティング", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool lightingChanged = false;
			lightingChanged |= ImGui::SliderFloat("リム強度", &material.RimStrength, 0.0f, 2.0f);
			lightingChanged |= ImGui::SliderFloat("リムしきい値", &material.RimThreshold, 0.0f, 1.0f);
			lightingChanged |= ImGui::SliderFloat("スペキュラ強度", &material.SpecularStrength, 0.0f, 2.0f);
			lightingChanged |= ImGui::SliderFloat("スペキュラしきい値", &material.SpecularThreshold, 0.0f, 1.0f);
			if (lightingChanged)
			{
				BeginUndoCapture(entity, before);
			}
		}

		if (ImGui::CollapsingHeader("肌", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool skinChanged = false;
			skinChanged |= ImGui::SliderFloat("肌散乱強度", &material.SkinScatterStrength, 0.0f, 3.0f);
			skinChanged |= ImGui::SliderFloat("肌散乱ラップ", &material.SkinScatterWrap, 0.0f, 1.0f);
			skinChanged |= ImGui::SliderFloat("肌逆光強度", &material.SkinBacklightStrength, 0.0f, 3.0f);
			skinChanged |= ImGui::SliderFloat("肌リム散乱強度", &material.SkinRimScatterStrength, 0.0f, 3.0f);
			skinChanged |= ImGui::SliderFloat("スペキュラ強度", &material.SkinOilSpecularStrength, 0.0f, 3.0f);
			skinChanged |= ImGui::SliderFloat("肌影散乱", &material.SkinShadowScatter, 0.0f, 1.0f);
			if (skinChanged)
			{
				BeginUndoCapture(entity, before);
			}
		}
	}

	bool useTexture = material.UseTexture != 0;
if (ImGui::Checkbox("テクスチャ使用", &useTexture))
	{
		BeginUndoCapture(entity, before);
		material.UseTexture = useTexture ? 1 : 0;
	}
	if (ImGui::Checkbox("透明", &material.IsTransparent))
	{
		BeginUndoCapture(entity, before);
	}
	if (ImGui::Checkbox("ポストプロセスを受ける", &material.ReceivingPostProcess))
	{
		BeginUndoCapture(entity, before);
	}
	ImGui::Text("テクスチャID: %d", material.TextureID);
	ImGui::Text("テクスチャパス: %s", material.TexturePath.empty() ? "(なし)" : material.TexturePath.c_str());

	static EntityID editingEntity = g_kINVALID_ENTITY;
	static char texturePath[260]{};
	if (editingEntity != entity)
	{
		editingEntity = entity;
		strncpy_s(texturePath, material.TexturePath.c_str(), _TRUNCATE);
	}

	ImGui::InputText("テクスチャパス", texturePath, IM_ARRAYSIZE(texturePath));
	if (ImGui::Button("テクスチャ適用"))
	{
		BeginUndoCapture(entity, before);
		MaterialSystem::SetTexture(entity, texturePath);
	}
	ImGui::SameLine();
	if (ImGui::Button("テクスチャ解除"))
	{
		texturePath[0] = '\0';
		BeginUndoCapture(entity, before);
		MaterialSystem::SetTexture(entity, "");
	}

	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
		{
			const char* path = static_cast<const char*>(payload->Data);
			if (path && IsTextureFile(path))
			{
				strncpy_s(texturePath, path, _TRUNCATE);
				BeginUndoCapture(entity, before);
				MaterialSystem::SetTexture(entity, path);
			}
		}
		ImGui::EndDragDropTarget();
	}

	UINT width = 0;
	UINT height = 0;
	if (TextureManager::GetTextureSize(material.TextureID, width, height))
	{
		ImGui::Text("サイズ: %u x %u", width, height);
		D3D12_GPU_DESCRIPTOR_HANDLE handle = RendererResource::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(material.TextureID) * RendererResource::GetCbvIncrementSize();
		ImGui::Image((ImTextureID)handle.ptr, ImVec2(96.0f, 96.0f));
	}
}

void ImGuiManager::DrawToonMeshOutlineInspector(EntityID entity, bool embeddedInInspector)
{
	if (!ComponentManager::HasComponent<MaterialComponent>(entity))
	{
		return;
	}

	auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);

	UINT meshCount = 0;
	auto getStaticMesh = [&](UINT index) -> const StaticMeshData*
		{
			if (!ComponentManager::HasComponent<StaticModelComponent>(entity))
			{
				return nullptr;
			}
			const auto& comp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
			auto* model = ModelManager::GetStaticModel(comp.ModelId);
			if (!model || index >= model->GetMeshCount())
			{
				return nullptr;
			}
			return &model->GetMeshData(index);
		};
	auto getAnimMesh = [&](UINT index) -> const MeshData*
		{
			if (!ComponentManager::HasComponent<AnimationModelComponent>(entity))
			{
				return nullptr;
			}
			const auto& comp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			auto* model = ModelManager::GetAnimModel(comp.ModelId);
			if (!model || index >= model->GetMeshCount())
			{
				return nullptr;
			}
			return &model->GetMeshData(index);
		};

	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{
		const auto& comp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
		if (auto* model = ModelManager::GetStaticModel(comp.ModelId))
		{
			meshCount = model->GetMeshCount();
		}
	}
	else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		const auto& comp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		if (auto* model = ModelManager::GetAnimModel(comp.ModelId))
		{
			meshCount = model->GetMeshCount();
		}
	}

	if (meshCount == 0)
	{
		if (!embeddedInInspector)
		{
			ImGui::TextUnformatted("メッシュがありません");
		}
		return;
	}

	bool opened = true;
	if (embeddedInInspector)
	{
		opened = ImGui::TreeNode("メッシュアウトライン上書き");
	}
	if (!opened)
	{
		return;
	}

	EntitySnapshot before = CaptureEntity(entity);
	bool changed = false;
	auto ensureOverrideSize = [&]()
		{
			if (material.ToonMeshOutlineOverrides.size() < meshCount)
			{
				material.ToonMeshOutlineOverrides.resize(meshCount, MeshOutlineOverride::Auto);
			}
		};
	auto ensureWidthScaleSize = [&]()
		{
			if (material.ToonMeshOutlineWidthScales.size() < meshCount)
			{
				material.ToonMeshOutlineWidthScales.resize(meshCount, 1.0f);
			}
		};

	if (ImGui::Button("すべて自動"))
	{
		material.ToonMeshOutlineOverrides.assign(meshCount, MeshOutlineOverride::Auto);
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("すべてオン"))
	{
		material.ToonMeshOutlineOverrides.assign(meshCount, MeshOutlineOverride::ForceOn);
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("すべてオフ"))
	{
		material.ToonMeshOutlineOverrides.assign(meshCount, MeshOutlineOverride::ForceOff);
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("幅リセット"))
	{
		material.ToonMeshOutlineWidthScales.assign(meshCount, 1.0f);
		changed = true;
	}

	const char* overrideItems[] = { "自動", "オン", "オフ" };
	constexpr ImGuiTableFlags tableFlags =
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollY;

	const float tableHeight = embeddedInInspector
		? 260.0f
		: max(180.0f, ImGui::GetContentRegionAvail().y);
	if (ImGui::BeginTable("MeshOutlineOverrideTable", 7, tableFlags, ImVec2(0.0f, tableHeight)))
	{
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
		ImGui::TableSetupColumn("メッシュ");
		ImGui::TableSetupColumn("マテリアル");
		ImGui::TableSetupColumn("部位", ImGuiTableColumnFlags_WidthFixed, 44.0f);
		ImGui::TableSetupColumn("自動", ImGuiTableColumnFlags_WidthFixed, 48.0f);
		ImGui::TableSetupColumn("上書き", ImGuiTableColumnFlags_WidthFixed, 96.0f);
		ImGui::TableSetupColumn("幅倍率", ImGuiTableColumnFlags_WidthFixed, 112.0f);
		ImGui::TableHeadersRow();

		for (UINT i = 0; i < meshCount; ++i)
		{
			string meshName;
			string materialName;
			float materialPartId = 10.0f;
			bool defaultOutline = true;
			if (const auto* mesh = getStaticMesh(i))
			{
				meshName = mesh->MeshName;
				materialName = mesh->MaterialName;
				materialPartId = mesh->MaterialPartId;
				defaultOutline = mesh->DefaultToonOutlineEnabled;
			}
			else if (const auto* mesh = getAnimMesh(i))
			{
				meshName = mesh->MeshName;
				materialName = mesh->MaterialName;
				materialPartId = mesh->MaterialPartId;
				defaultOutline = mesh->DefaultToonOutlineEnabled;
			}

			if (meshName.empty())
			{
				meshName = "(unnamed)";
			}
			if (materialName.empty())
			{
				materialName = "(none)";
			}

			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%u", i);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(meshName.c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(materialName.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.0f", materialPartId);
			ImGui::TableSetColumnIndex(4);
			ImGui::TextUnformatted(defaultOutline ? "オン" : "オフ");
			ImGui::TableSetColumnIndex(5);

			MeshOutlineOverride overrideValue = MeshOutlineOverride::Auto;
			if (i < material.ToonMeshOutlineOverrides.size())
			{
				overrideValue = material.ToonMeshOutlineOverrides[i];
			}
			int overrideIndex = static_cast<int>(overrideValue);
			if (ImGui::SetNextItemWidth(-FLT_MIN), ImGui::Combo("##Override", &overrideIndex, overrideItems, IM_ARRAYSIZE(overrideItems)))
			{
				ensureOverrideSize();
				material.ToonMeshOutlineOverrides[i] = static_cast<MeshOutlineOverride>(overrideIndex);
				changed = true;
			}
			ImGui::TableSetColumnIndex(6);
			float widthScale = 1.0f;
			if (i < material.ToonMeshOutlineWidthScales.size())
			{
				widthScale = material.ToonMeshOutlineWidthScales[i];
			}
			if (ImGui::SetNextItemWidth(-FLT_MIN), ImGui::SliderFloat("##WidthScale", &widthScale, 0.0f, 3.0f, "%.2f"))
			{
				ensureWidthScaleSize();
				material.ToonMeshOutlineWidthScales[i] = widthScale;
				changed = true;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (changed)
	{
		BeginUndoCapture(entity, before);
	}
	if (embeddedInInspector)
	{
		ImGui::TreePop();
	}
}

void ImGuiManager::ApplyMeshShadingOverridesToModel(EntityID entity)
{
	if (!Registry::IsAlive(entity) ||
		!ComponentManager::HasComponent<MaterialComponent>(entity))
	{
		return;
	}

	auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
	vector<int> overridePartIds;
	overridePartIds.reserve(material.MeshShadingOverrides.size());
	for (MeshShadingOverride value : material.MeshShadingOverrides)
	{
		overridePartIds.push_back(static_cast<int>(value));
	}
	if (!overridePartIds.empty() && IsModelMaterialEntity(entity))
	{
		material.ShaderClassMode = MaterialMode::Auto;
	}

	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{
		const auto& comp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
		if (auto* model = ModelManager::GetStaticModel(comp.ModelId))
		{
			model->ApplyMeshShadingOverridePartIds(RendererCore::GetDevice(), overridePartIds);
		}
	}
	else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		const auto& comp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		if (auto* model = ModelManager::GetAnimModel(comp.ModelId))
		{
			model->ApplyMeshShadingOverridePartIds(overridePartIds);
		}
	}
}

void ImGuiManager::DrawMeshShadingInspector(EntityID entity, bool embeddedInInspector)
{
	if (!ComponentManager::HasComponent<MaterialComponent>(entity))
	{
		return;
	}

	auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);

	UINT meshCount = 0;
	auto getStaticMesh = [&](UINT index) -> const StaticMeshData*
		{
			if (!ComponentManager::HasComponent<StaticModelComponent>(entity))
			{
				return nullptr;
			}
			const auto& comp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
			auto* model = ModelManager::GetStaticModel(comp.ModelId);
			if (!model || index >= model->GetMeshCount())
			{
				return nullptr;
			}
			return &model->GetMeshData(index);
		};
	auto getAnimMesh = [&](UINT index) -> const MeshData*
		{
			if (!ComponentManager::HasComponent<AnimationModelComponent>(entity))
			{
				return nullptr;
			}
			const auto& comp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
			auto* model = ModelManager::GetAnimModel(comp.ModelId);
			if (!model || index >= model->GetMeshCount())
			{
				return nullptr;
			}
			return &model->GetMeshData(index);
		};

	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{
		const auto& comp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
		if (auto* model = ModelManager::GetStaticModel(comp.ModelId))
		{
			meshCount = model->GetMeshCount();
		}
	}
	else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		const auto& comp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		if (auto* model = ModelManager::GetAnimModel(comp.ModelId))
		{
			meshCount = model->GetMeshCount();
		}
	}

	if (meshCount == 0)
	{
		if (!embeddedInInspector)
		{
			ImGui::TextUnformatted("メッシュがありません");
		}
		return;
	}

	bool opened = true;
	if (embeddedInInspector)
	{
		opened = ImGui::TreeNode("メッシュシェーディング上書き");
	}
	if (!opened)
	{
		return;
	}

	EntitySnapshot before = CaptureEntity(entity);
	bool changed = false;
	auto ensureOverrideSize = [&]()
		{
			if (material.MeshShadingOverrides.size() < meshCount)
			{
				material.MeshShadingOverrides.resize(meshCount, MeshShadingOverride::Auto);
			}
		};

	if (ImGui::Button("すべて自動"))
	{
		material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::Auto);
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("すべて髪##MeshShading"))
	{
		material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::Hair);
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("すべて肌##MeshShading"))
	{
		material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::Skin);
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("すべてPBR##MeshShading"))
	{
		material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::PBR);
		changed = true;
	}

	const char* shadingItems[] =
	{
		"自動", "透明", "髪", "服", "肌", "トゥーン",
		"影", "メタリック", "セルフシャドウ", "ライティング", "目", "非ライティング", "PBR",
		"BRDF", "BTDF", "BSDF"
	};
	const int shadingValues[] =
	{
		static_cast<int>(MeshShadingOverride::Auto),
		static_cast<int>(MeshShadingOverride::Transparent),
		static_cast<int>(MeshShadingOverride::Hair),
		static_cast<int>(MeshShadingOverride::Cloth),
		static_cast<int>(MeshShadingOverride::Skin),
		static_cast<int>(MeshShadingOverride::Toon),
		static_cast<int>(MeshShadingOverride::Shadow),
		static_cast<int>(MeshShadingOverride::Metallic),
		static_cast<int>(MeshShadingOverride::SelfShadow),
		static_cast<int>(MeshShadingOverride::Lit),
		static_cast<int>(MeshShadingOverride::Eye),
		static_cast<int>(MeshShadingOverride::Unlit),
		static_cast<int>(MeshShadingOverride::PBR),
		static_cast<int>(MeshShadingOverride::BRDF),
		static_cast<int>(MeshShadingOverride::BTDF),
		static_cast<int>(MeshShadingOverride::BSDF),
	};

	auto getAutoLabel = [](float materialPartId) -> const char*
		{
			const int part = static_cast<int>(materialPartId + 0.5f);
			switch (part)
			{
			case static_cast<int>(ShaderClass::Hair): return "髪";
			case static_cast<int>(ShaderClass::Cloth): return "服";
			case static_cast<int>(ShaderClass::Skin): return "肌";
			case static_cast<int>(ShaderClass::Toon): return "トゥーン";
			case static_cast<int>(ShaderClass::PBR): return "PBR";
			case static_cast<int>(ShaderClass::BRDF): return "BRDF";
			case static_cast<int>(ShaderClass::BTDF): return "BTDF";
			case static_cast<int>(ShaderClass::BSDF): return "BSDF";
			default: return "自動";
			}
		};

	constexpr ImGuiTableFlags tableFlags =
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollY;

	const float tableHeight = embeddedInInspector
		? 260.0f
		: max(180.0f, ImGui::GetContentRegionAvail().y);
	if (ImGui::BeginTable("MeshShadingOverrideTable", 5, tableFlags, ImVec2(0.0f, tableHeight)))
	{
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
		ImGui::TableSetupColumn("メッシュ");
		ImGui::TableSetupColumn("マテリアル");
		ImGui::TableSetupColumn("部位", ImGuiTableColumnFlags_WidthFixed, 44.0f);
		ImGui::TableSetupColumn("シェーディング", ImGuiTableColumnFlags_WidthFixed, 128.0f);
		ImGui::TableHeadersRow();

		for (UINT i = 0; i < meshCount; ++i)
		{
			string meshName;
			string materialName;
			float materialPartId = 10.0f;
			if (const auto* mesh = getStaticMesh(i))
			{
				meshName = mesh->MeshName;
				materialName = mesh->MaterialName;
				materialPartId = mesh->MaterialPartId;
			}
			else if (const auto* mesh = getAnimMesh(i))
			{
				meshName = mesh->MeshName;
				materialName = mesh->MaterialName;
				materialPartId = mesh->MaterialPartId;
			}

			if (meshName.empty()) meshName = "(unnamed)";
			if (materialName.empty()) materialName = "(none)";

			MeshShadingOverride overrideValue = MeshShadingOverride::Auto;
			if (i < material.MeshShadingOverrides.size())
			{
				overrideValue = material.MeshShadingOverrides[i];
			}
			int comboIndex = 0;
			for (int valueIndex = 0; valueIndex < IM_ARRAYSIZE(shadingValues); ++valueIndex)
			{
				if (static_cast<int>(overrideValue) == shadingValues[valueIndex])
				{
					comboIndex = valueIndex;
					break;
				}
			}

			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%u", i);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(meshName.c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(materialName.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.0f", materialPartId);
			ImGui::TableSetColumnIndex(4);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::Combo("##ShadingOverride", &comboIndex, shadingItems, IM_ARRAYSIZE(shadingItems)))
			{
				ensureOverrideSize();
				material.MeshShadingOverrides[i] = static_cast<MeshShadingOverride>(shadingValues[comboIndex]);
				changed = true;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (changed)
	{
		material.ShaderClassMode = MaterialMode::Auto;
		ApplyMeshShadingOverridesToModel(entity);
		BeginUndoCapture(entity, before);
	}
	if (embeddedInInspector)
	{
		ImGui::TreePop();
	}
}

void ImGuiManager::DrawLightInspector(EntityID entity)
{
	if (!ComponentManager::HasComponent<LightComponent>(entity))
	{
		return;
	}

	if (!ImGui::CollapsingHeader("ライト", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}

	auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
	EntitySnapshot before = CaptureEntity(entity);
	bool changed = false;
	const char* lightTypes[] = { "Directional", "Point", "Spot", "Volume" };
	int typeIndex = static_cast<int>(light.Type);
	if (ImGui::Combo("ライトタイプ", &typeIndex, lightTypes, IM_ARRAYSIZE(lightTypes)))
	{
		changed = true;
		light.Type = static_cast<LightType>(typeIndex);
		RefreshLightEntityName(entity, light.Type);
		ApplyLightEntityToRuntime(entity);
	}

	changed |= ImGui::Checkbox("有効", &light.IsActive);
	changed |= ImGui::Checkbox("デバッグ描画", &light.DrawDebug);
	changed |= ImGui::ColorEdit3("色", &light.Color.x);
	changed |= ImGui::DragFloat("ライト強度", &light.Intensity, 0.01f, 0.0f, 20.0f);
	changed |= ImGui::DragFloat("範囲", &light.Range, 0.05f, 0.1f, 100.0f);
	changed |= ImGui::DragFloat3("ライト方向", &light.Direction.x, 0.01f, -1.0f, 1.0f);

	if (light.Type == LightType::Spot)
	{
		changed |= ImGui::DragFloat("内角", &light.InnerAngle, 0.2f, 0.1f, 89.0f);
		changed |= ImGui::DragFloat("外角", &light.OuterAngle, 0.2f, light.InnerAngle + 0.1f, 89.5f);
	}

	if (light.Type == LightType::Volume)
	{
		const char* volumeShapes[] = { "円錐", "円柱" };
		int volumeShape = light.VolumeShape;
		if (ImGui::Combo("ボリューム形状", &volumeShape, volumeShapes, IM_ARRAYSIZE(volumeShapes)))
		{
			light.VolumeShape = volumeShape;
			changed = true;
		}
		changed |= ImGui::DragFloat("内角", &light.InnerAngle, 0.2f, 0.1f, 89.0f);
		changed |= ImGui::DragFloat("外角", &light.OuterAngle, 0.2f, light.InnerAngle + 0.1f, 89.5f);
		changed |= ImGui::DragFloat("ボリューム密度", &light.VolumeDensity, 0.01f, 0.0f, 3.0f);
	}

	if (ImGui::Button("メインライトに設定"))
	{
		changed = true;
		light.IsActive = true;
	}

	if (changed)
	{
		BeginUndoCapture(entity, before);
		ApplyLightEntityToRuntime(entity);
	}
}

void ImGuiManager::DrawComponentInspector(EntityID entity)
{
	if (ImGui::CollapsingHeader("コンポーネント"))
	{
		ImGui::Text("エンティティID: %u", entity);
		ImGui::Text("名前: %s", ComponentManager::HasComponent<NameComponent>(entity) ? ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name.c_str() : "(none)");
		if (!ComponentManager::HasComponent<AABBComponent>(entity) && ImGui::Button("AABB追加"))
		{
			XMFLOAT3 center{};
			XMFLOAT3 extents{};
			const bool hasModelAabb = GetLocalAabb(entity, center, extents);
			ComponentManager::AddComponent(entity, ComponentType::AABB);
			if (hasModelAabb)
			{
				auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
				aabb.Center = center;
				aabb.Extents = extents;
			}
		}
		const bool isLightEntity = ComponentManager::HasComponent<LightComponent>(entity);
		if (!isLightEntity && !ComponentManager::HasComponent<MaterialComponent>(entity) && ImGui::Button("マテリアル追加"))
		{
			ComponentManager::AddComponent(entity, ComponentType::MATERIAL);
			auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
			mat.TextureID = TextureManager::GetDefaultTextureIndex();
			mat.UseTexture = false;
		}
ImGui::Text("メッシュ: %s", ComponentManager::HasComponent<MeshComponent>(entity) ? "あり" : "なし");
	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{
		auto& staticModel = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
		ImGui::Text("静的モデル: あり");
		ImGui::Checkbox("座標変換", &staticModel.IsConvert);
	}
	else
	{
		ImGui::Text("静的モデル: なし");
	}
	if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		auto& animModel = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		ImGui::Text("アニメーションモデル: あり");
		ImGui::Checkbox("座標変換", &animModel.IsConvert);
	}
	else
	{
		ImGui::Text("アニメーションモデル: なし");
	}
	ImGui::Text("スプライト: %s", ComponentManager::HasComponent<SpriteComponent>(entity) ? "あり" : "なし");
	}

	if (ComponentManager::HasComponent<SpriteComponent>(entity) && ImGui::CollapsingHeader("スプライト"))
	{
		auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
		ImGui::Checkbox("3D", &sprite.Is3D);
		ImGui::Checkbox("UV変換", &sprite.UseUvTransform);
		ImGui::DragFloat2("UVオフセット", &sprite.UvOffset.x, 0.001f);
		ImGui::DragFloat2("UVスケール", &sprite.UvScale.x, 0.001f);
		ImGui::Checkbox("ポストプロセス使用", &sprite.UsePostProcess);
	}
}

void ImGuiManager::DeleteSelectedEntity()
{
	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		return;
	}

	const EntityID entity = m_SelectedEntity;
	PushUndoSnapshot(CaptureEntity(entity));
	AddLog("エンティティ削除: %s", GetEntityDisplayName(entity));
	if (m_RenamingEntity == entity)
	{
		CancelRename();
	}
	World::DestroyEntity(Entity(entity));
	m_SelectedEntity = g_kINVALID_ENTITY;
}

EntityID ImGuiManager::CreateLightEntity(LightType type)
{
	Light::CreateDesc desc = Light::MakeDefaultDesc(type);
	desc.Name = string(GetLightTypeName(type)) + " Light";
	const EntityID entity = Light::Create(desc);
	ApplyLightEntityToRuntime(entity);
	AddLog("ライト追加: %s #%u", GetEntityDisplayName(entity), entity);
	return entity;
}

void ImGuiManager::ProcessDroppedFiles()
{
	if (m_DroppedFiles.empty())
	{
		return;
	}

	vector<string> dropped;
	dropped.swap(m_DroppedFiles);
	for (const string& path : dropped)
	{
		ImportAssetFile(path);
	}
}

void ImGuiManager::ImportAssetFile(const filesystem::path& sourcePath)
{
	error_code ec;
	if (!filesystem::exists(sourcePath, ec))
	{
		return;
	}

	filesystem::path destination = m_CurrentAssetDirectory / sourcePath.filename();
	if (filesystem::equivalent(sourcePath, destination, ec))
	{
		return;
	}

	if (filesystem::is_directory(sourcePath, ec))
	{
		filesystem::copy(sourcePath, destination, filesystem::copy_options::recursive | filesystem::copy_options::overwrite_existing, ec);
		if (!ec)
		{
			AddLog("アセット取り込み: %s", destination.generic_string().c_str());
		}
		return;
	}

	filesystem::copy_file(sourcePath, destination, filesystem::copy_options::overwrite_existing, ec);
	if (!ec && IsTextureFile(destination))
	{
		const string relative = MakeRelativeAssetPath(destination);
		TextureManager::LoadTexture(relative.c_str());
		AddLog("テクスチャ取り込み: %s", relative.c_str());
	}
	else if (!ec)
	{
		AddLog("アセット取り込み: %s", destination.generic_string().c_str());
	}
}

void ImGuiManager::CreateAssetFile(const filesystem::path& path)
{
	error_code ec;
	if (filesystem::exists(path, ec))
	{
		AddLog("作成スキップ（既に存在）: %s", path.generic_string().c_str());
		return;
	}

	ofstream file(path);
	if (!file)
	{
		AddLog("作成失敗: %s", path.generic_string().c_str());
		return;
	}

	string ext = path.extension().generic_string();
	transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	if (ext == ".material")
	{
		file << "{\n  \"type\": \"material\",\n  \"texture\": \"\"\n}\n";
	}
	else
	{
		file << "# 新規アセット\n";
	}
	AddLog("ファイル作成: %s", path.generic_string().c_str());
}

void ImGuiManager::DeleteAssetPath(const filesystem::path& path)
{
	error_code ec;
	if (path.empty() || !filesystem::exists(path, ec))
	{
		return;
	}

	filesystem::path relativeToRoot = filesystem::relative(path, m_AssetRoot, ec);
	if (ec || relativeToRoot.empty() || relativeToRoot.generic_string().starts_with(".."))
	{
		AddLog("削除中止（asset外）: %s", path.generic_string().c_str());
		return;
	}

	if (filesystem::is_directory(path, ec))
	{
		filesystem::remove_all(path, ec);
	}
	else
	{
		filesystem::remove(path, ec);
	}

	if (ec)
	{
		AddLog("削除失敗: %s", path.generic_string().c_str());
	}
	else
	{
		AddLog("削除: %s", path.generic_string().c_str());
	}
}

void ImGuiManager::PlaceAssetInScene(const filesystem::path& path, const ImVec2& sceneMouse)
{
	const string relative = MakeRelativeAssetPath(path);
	if (IsTextureFile(path))
	{
		TextureManager::LoadTexture(relative.c_str());
		if (m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(m_SelectedEntity))
		{
			MaterialSystem::SetTexture(m_SelectedEntity, relative.c_str());
			AddLog("ドラッグ&ドロップでテクスチャ適用: %s", relative.c_str());
		}
		return;
	}

	if (!IsModelFile(path))
	{
		AddLog("シーンD&D無視: %s", relative.c_str());
		return;
	}

	XMVECTOR rayOrigin;
	XMVECTOR rayDir;
	if (!GetSceneViewRay(sceneMouse, rayOrigin, rayDir))
	{
		return;
	}

	float y = 0.0f;
	float originY = XMVectorGetY(rayOrigin);
	float dirY = XMVectorGetY(rayDir);
	float t = fabsf(dirY) > 0.0001f ? (y - originY) / dirY : 5.0f;
	if (t < 0.0f) t = 5.0f;
	XMVECTOR hit = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t));
	XMFLOAT3 pos{};
	XMStoreFloat3(&pos, hit);

	auto& entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<AABBComponent>()
		.Add<MaterialComponent>()
		.Add<StaticModelComponent>();

	entity.SetName(path.stem().generic_string());
	const bool isConvert = ShouldConvertModelByPath(relative);
	auto& transform = entity.Get<TransformComponent>();
	transform.Position = pos;
	transform.Scale = { 0.03f, 0.03f, 0.03f };
	transform.Rotation = GetDefaultModelRotationByPath(relative, isConvert);
	transform.IsDirty = true;

	entity.Get<StaticModelComponent>().IsConvert = isConvert;
	entity.Get<StaticModelComponent>().ModelPath = relative;
	const int modelId = ModelManager::LoadStaticModel(relative.c_str(), isConvert);
	if (modelId < 0)
	{
		AddLog("モデル読み込み失敗: %s", relative.c_str());
		World::DestroyEntity(entity);
		return;
	}
	entity.Get<StaticModelComponent>().ModelId = modelId;
	if (auto* model = ModelManager::GetStaticModel(modelId))
	{
		entity.Get<AABBComponent>().Center = model->GetAabbCenter();
		entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
	}
	else
	{
		entity.Get<AABBComponent>().Center = { 0.0f, 1.0f, 0.0f };
		entity.Get<AABBComponent>().Extents = { 0.7f, 1.0f, 0.7f };
	}

	auto& material = entity.Get<MaterialComponent>();
	material.TextureID = TextureManager::GetDefaultTextureIndex();
	material.UseTexture = true;
	material.ReceivingPostProcess = true;

	m_SelectedEntity = entity.GetID();
	AddLog("モデル配置: %s at %.2f %.2f %.2f", relative.c_str(), pos.x, pos.y, pos.z);
}

bool ImGuiManager::GetSceneViewRay(const ImVec2& sceneMouse, XMVECTOR& outOrigin, XMVECTOR& outDirection)
{
	if (m_SceneViewSize.x <= 1.0f || m_SceneViewSize.y <= 1.0f)
	{
		return false;
	}

	EntityID cameraEntity = Camera::GetCameraEntity();
	if (cameraEntity == g_kINVALID_ENTITY || !Registry::IsAlive(cameraEntity))
	{
		return false;
	}

	XMMATRIX view;
	XMMATRIX proj;
	Camera::GetCameraMatrices(cameraEntity, view, proj);
	outOrigin = XMVector3Unproject(
		XMVectorSet(sceneMouse.x, sceneMouse.y, 0.0f, 1.0f),
		0.0f, 0.0f, m_SceneViewSize.x, m_SceneViewSize.y, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	XMVECTOR farPoint = XMVector3Unproject(
		XMVectorSet(sceneMouse.x, sceneMouse.y, 1.0f, 1.0f),
		0.0f, 0.0f, m_SceneViewSize.x, m_SceneViewSize.y, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	outDirection = XMVector3Normalize(XMVectorSubtract(farPoint, outOrigin));
	return true;
}

bool ImGuiManager::IsTextureFile(const filesystem::path& path)
{
	string ext = path.extension().generic_string();
	transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".dds";
}

bool ImGuiManager::IsModelFile(const filesystem::path& path)
{
	string ext = path.extension().generic_string();
	transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return ext == ".fbx" || ext == ".obj" || ext == ".vrm" || ext == ".pmx";
}

string ImGuiManager::MakeRelativeAssetPath(const filesystem::path& path)
{
	if (path.is_relative())
	{
		return path.generic_string();
	}

	error_code ec;
	filesystem::path relative = filesystem::relative(path, filesystem::current_path(), ec);
	if (ec)
	{
		relative = path;
	}
	return relative.generic_string();
}

void ImGuiManager::PickEntityFromMouse()
{
	ImGuiIO& io = ImGui::GetIO();
	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
		!m_IsSceneViewHovered)
	{
		return;
	}

	const float width = m_SceneViewSize.x;
	const float height = m_SceneViewSize.y;
	if (width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	ImVec2 mouse = io.MousePos;
	mouse.x -= m_SceneViewPos.x;
	mouse.y -= m_SceneViewPos.y;
	if (mouse.x < 0.0f || mouse.y < 0.0f || mouse.x >= width || mouse.y >= height)
	{
		return;
	}

	EntityID cameraEntity = Camera::GetCameraEntity();
	if (cameraEntity == g_kINVALID_ENTITY || !Registry::IsAlive(cameraEntity))
	{
		return;
	}

	XMMATRIX view;
	XMMATRIX proj;
	Camera::GetCameraMatrices(cameraEntity, view, proj);

	XMVECTOR nearPoint = XMVector3Unproject(
		XMVectorSet(mouse.x, mouse.y, 0.0f, 1.0f),
		0.0f, 0.0f, width, height, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	XMVECTOR farPoint = XMVector3Unproject(
		XMVectorSet(mouse.x, mouse.y, 1.0f, 1.0f),
		0.0f, 0.0f, width, height, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));

	EntityID bestEntity = g_kINVALID_ENTITY;
	float bestDistance = FLT_MAX;

	for (EntityID entity : World::GetView<TransformComponent>())
	{
		if (!IsEditableEntity(entity))
		{
			continue;
		}

		XMFLOAT3 center{};
		XMFLOAT3 extents{};
		if (!GetLocalAabb(entity, center, extents))
		{
			continue;
		}

		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		BoundingOrientedBox localBox(center, extents, XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
		BoundingOrientedBox worldBox;
		localBox.Transform(worldBox, BuildWorldMatrix(transform));

		float distance = 0.0f;
		if (worldBox.Intersects(nearPoint, rayDir, distance) && distance < bestDistance)
		{
			bestDistance = distance;
			bestEntity = entity;
		}
	}

	m_SelectedEntity = bestEntity;
}

bool ImGuiManager::IsEditableEntity(EntityID entity)
{
	if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<TransformComponent>(entity))
	{
		return false;
	}

	if (ComponentManager::HasComponent<CameraComponent>(entity))
	{
		return false;
	}

	if (ComponentManager::HasComponent<MeshComponent>(entity) ||
		ComponentManager::HasComponent<StaticModelComponent>(entity) ||
		ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		return true;
	}

	if (ComponentManager::HasComponent<SpriteComponent>(entity))
	{
		const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
		return sprite.Is3D;
	}

	if (ComponentManager::HasComponent<LightComponent>(entity))
	{
		return true;
	}

	return false;
}

void ImGuiManager::BeginRename(EntityID entity)
{
	if (entity == g_kINVALID_ENTITY ||
		!Registry::IsAlive(entity) ||
		!ComponentManager::HasComponent<NameComponent>(entity))
	{
		return;
	}

	m_RenamingEntity = entity;
	m_RenameBuffer = ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name;
}

void ImGuiManager::CommitRename()
{
	if (m_RenamingEntity == g_kINVALID_ENTITY ||
		!Registry::IsAlive(m_RenamingEntity) ||
		!ComponentManager::HasComponent<NameComponent>(m_RenamingEntity))
	{
		CancelRename();
		return;
	}

	if (!m_RenameBuffer.empty())
	{
		PushUndoSnapshot(CaptureEntity(m_RenamingEntity));
		auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(m_RenamingEntity);
		name.Name = m_RenameBuffer;
		World::RegisterName(m_RenamingEntity, name.Name);
		AddLog("リネーム: %s #%u", name.Name.c_str(), m_RenamingEntity);
	}

	CancelRename();
}

void ImGuiManager::CancelRename()
{
	m_RenamingEntity = g_kINVALID_ENTITY;
	m_RenameBuffer.clear();
}

bool ImGuiManager::DrawRenameInput(EntityID entity)
{
	if (entity != m_RenamingEntity)
	{
		return false;
	}

	char buffer[256] {};
	strncpy_s(buffer, m_RenameBuffer.c_str(), _TRUNCATE);
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::PushID(static_cast<int>(entity));
	const bool submitted = ImGui::InputText("##Rename", buffer, IM_ARRAYSIZE(buffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	if (ImGui::IsItemActivated())
	{
		ImGui::SetKeyboardFocusHere(-1);
	}
	m_RenameBuffer = buffer;
	const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::PopID();

	if (submitted || deactivated)
	{
		CommitRename();
		return true;
	}
	return false;
}

ImGuiManager::EntitySnapshot ImGuiManager::CaptureEntity(EntityID entity)
{
	EntitySnapshot snapshot{};
	snapshot.Entity = entity;
	if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity)) return snapshot;
	snapshot.WasAlive = true;
	if (ComponentManager::HasComponent<NameComponent>(entity))
	{ 
		snapshot.HasName = true; 
		snapshot.Name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
	}

	if (ComponentManager::HasComponent<TransformComponent>(entity))
	{ 
		snapshot.HasTransform = true;
		snapshot.Transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity); 
	}

	if (ComponentManager::HasComponent<ShaderComponent>(entity)) 
	{	snapshot.HasShader = true;
		snapshot.Shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity); 
	}

	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{	snapshot.HasStaticModel = true;
		snapshot.StaticModel = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity); 
	}

	if (ComponentManager::HasComponent<AnimationModelComponent>(entity)) 
	{	snapshot.HasAnimationModel = true; 
		snapshot.AnimationModel = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
	}

	if (ComponentManager::HasComponent<LightComponent>(entity)) 
	{	snapshot.HasLight = true;
		snapshot.Light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
	}

	if (ComponentManager::HasComponent<MaterialComponent>(entity))
	{	snapshot.HasMaterial = true; 
		snapshot.Material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
	}

	if (ComponentManager::HasComponent<AABBComponent>(entity))
	{	snapshot.HasAabb = true;
		snapshot.Aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
	}

	if (ComponentManager::HasComponent<SpriteComponent>(entity))
	{	snapshot.HasSprite = true;
		snapshot.Sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
	}

	if (ComponentManager::HasComponent<MeshComponent>(entity))
	{	snapshot.HasMesh = true; 
		snapshot.Mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
	}

	if (ComponentManager::HasComponent<CameraComponent>(entity)) 
	{	snapshot.HasCamera = true;
		snapshot.Camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity); 
	}

	if (ComponentManager::HasComponent<PostProcessComponent>(entity))
	{	snapshot.HasPostProcess = true;
		snapshot.PostProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(entity); 
	}

	if (ComponentManager::HasComponent<InputComponent>(entity))
	{	snapshot.HasInput = true;
		snapshot.Input = ComponentManager::GetComponentUnchecked<InputComponent>(entity);
	}

	if (ComponentManager::HasComponent<MoveComponent>(entity)) 
	{	snapshot.HasMove = true; 
		snapshot.Move = ComponentManager::GetComponentUnchecked<MoveComponent>(entity);
	}

	if (ComponentManager::HasComponent<PhysicsComponent>(entity)) 
	{	snapshot.HasPhysics = true; 
		snapshot.Physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
	}

	if (ComponentManager::HasComponent<OBBComponent>(entity)) 
	{	snapshot.HasObb = true; 
		snapshot.Obb = ComponentManager::GetComponentUnchecked<OBBComponent>(entity);
	}

	return snapshot;
}

void ImGuiManager::ApplySnapshot(const EntitySnapshot& snapshot)
{
	if (snapshot.Entity == g_kINVALID_ENTITY) return;
	if (!snapshot.WasAlive)
	{
		if (Registry::IsAlive(snapshot.Entity))
		{
			World::DestroyEntity(Entity(snapshot.Entity));
		}
		return;
	}

	if (!Registry::IsAlive(snapshot.Entity) && !Registry::RestoreEntity(snapshot.Entity))
	{
		return;
	}

	if (snapshot.HasName) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Name); World::RegisterName(snapshot.Entity, snapshot.Name.Name); }
	if (snapshot.HasTransform) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Transform); ComponentManager::GetComponentUnchecked<TransformComponent>(snapshot.Entity).IsDirty = true; }
	if (snapshot.HasShader) RestoreSnapshotComponent(snapshot.Entity, snapshot.Shader);
	if (snapshot.HasStaticModel) RestoreSnapshotComponent(snapshot.Entity, snapshot.StaticModel);
	if (snapshot.HasAnimationModel) RestoreSnapshotComponent(snapshot.Entity, snapshot.AnimationModel);
	if (snapshot.HasLight) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Light); ApplyLightEntityToRuntime(snapshot.Entity); }
	if (snapshot.HasMaterial) RestoreSnapshotComponent(snapshot.Entity, snapshot.Material);
	if (snapshot.HasAabb) RestoreSnapshotComponent(snapshot.Entity, snapshot.Aabb);
	if (snapshot.HasSprite) RestoreSnapshotComponent(snapshot.Entity, snapshot.Sprite);
	if (snapshot.HasMesh) RestoreSnapshotComponent(snapshot.Entity, snapshot.Mesh);
	if (snapshot.HasCamera) RestoreSnapshotComponent(snapshot.Entity, snapshot.Camera);
	if (snapshot.HasPostProcess) RestoreSnapshotComponent(snapshot.Entity, snapshot.PostProcess);
	if (snapshot.HasInput) RestoreSnapshotComponent(snapshot.Entity, snapshot.Input);
	if (snapshot.HasMove) RestoreSnapshotComponent(snapshot.Entity, snapshot.Move);
	if (snapshot.HasPhysics) RestoreSnapshotComponent(snapshot.Entity, snapshot.Physics);
	if (snapshot.HasObb) RestoreSnapshotComponent(snapshot.Entity, snapshot.Obb);
}

void ImGuiManager::BeginUndoCapture(EntityID entity, const EntitySnapshot& before)
{
	if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity)) return;
	if (!m_HasPendingUndo) { m_PendingUndo = before; m_HasPendingUndo = true; }
}

void ImGuiManager::FinalizeUndoCaptureIfIdle()
{
	if (!m_HasPendingUndo || ImGui::IsAnyItemActive()) return;
	PushUndoSnapshot(m_PendingUndo);
	m_HasPendingUndo = false;
	m_PendingUndo = {};
}

void ImGuiManager::PushUndoSnapshot(const EntitySnapshot& snapshot)
{
	if (snapshot.Entity == g_kINVALID_ENTITY) return;
	m_UndoStack.push_back(snapshot);
	if (m_UndoStack.size() > 10) m_UndoStack.erase(m_UndoStack.begin());
	m_RedoStack.clear();
}

void ImGuiManager::Undo()
{
	FinalizeUndoCaptureIfIdle();
	if (m_UndoStack.empty()) return;
	EntitySnapshot snapshot = m_UndoStack.back();
	m_UndoStack.pop_back();
	m_RedoStack.push_back(CaptureEntity(snapshot.Entity));
	if (m_RedoStack.size() > 10) m_RedoStack.erase(m_RedoStack.begin());
	ApplySnapshot(snapshot);
	m_SelectedEntity = snapshot.WasAlive ? snapshot.Entity : g_kINVALID_ENTITY;
}

void ImGuiManager::Redo()
{
	if (m_RedoStack.empty()) return;
	EntitySnapshot snapshot = m_RedoStack.back();
	m_RedoStack.pop_back();
	m_UndoStack.push_back(CaptureEntity(snapshot.Entity));
	if (m_UndoStack.size() > 10) m_UndoStack.erase(m_UndoStack.begin());
	ApplySnapshot(snapshot);
	m_SelectedEntity = snapshot.WasAlive ? snapshot.Entity : g_kINVALID_ENTITY;
}
const char* ImGuiManager::GetLightTypeName(LightType type)
{
	switch (type)
	{
	case LightType::Directional: return "Directional";
	case LightType::Point: return "Point";
	case LightType::Spot: return "Spot";
	case LightType::Volume: return "Volume";
	default: return "Unknown";
	}
}

const char* ImGuiManager::GetEntityDisplayName(EntityID entity)
{
	static string label;
	if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity))
	{
return "なし";
	}

	if (ComponentManager::HasComponent<NameComponent>(entity))
	{
		label = ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name;
	}
	else
	{
		label = "エンティティ";
	}
	label += " #";
	label += to_string(entity);
	return label.c_str();
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

