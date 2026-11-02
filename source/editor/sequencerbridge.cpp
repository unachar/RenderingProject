#include "pch.h" // プロジェクトのプリコンパイル済みヘッダー（テストでは空のスタブ）
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "sequencerbridge.h"
#include "sequencerwindow.h"
#include "sequencerwaveform.h"
#include "imguimanager.h"
#include "componentmanager.h"
#include "timelinesystem.h"
#include "projectmanager.h"
#include "camera.h"
#include "world.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include <filesystem>
#include <memory>

// ImGuiManager の既存機能（選択・Undo 履歴・VMD / 連番 UI）を ISequencerBackend として公開する。
class SequencerEditorBridgeImpl final : public ISequencerBackend
{
  public:
    static SequencerEditorBridgeImpl& Instance()
    {
        static SequencerEditorBridgeImpl bridge;
        return bridge;
    }

    SequencerWindow& Window()
    {
        if (!m_Window)
        {
            m_Window = std::make_unique<SequencerWindow>(*this);
        }
        return *m_Window;
    }

    void Reset()
    {
        m_Window.reset();
        SequencerWaveform::Clear();
    }

    // ---- シーン参照
    void CollectDirectors(std::vector<EntityID>& out) override
    {
        ComponentManager::ForEachComponent<TimelineComponent>([&](EntityID entity, TimelineComponent&) {
            out.push_back(entity);
        });
    }

    void CollectTrackableEntities(std::vector<EntityID>& out) override
    {
        const auto entities = ImGuiManager::CollectSceneEntities();
        out.insert(out.end(), entities.begin(), entities.end());
    }

    void CollectCameras(std::vector<EntityID>& out) override
    {
        const EntityID editorCamera = Camera::GetEditorCameraEntity();
        for (EntityID entity : World::GetView<CameraComponent>())
        {
            if (entity != editorCamera)
            {
                out.push_back(entity);
            }
        }
    }

    TimelineComponent* GetTimeline(EntityID director) override
    {
        if (director == g_kINVALID_ENTITY || !Registry::IsAlive(director) ||
            !ComponentManager::HasComponent<TimelineComponent>(director))
        {
            return nullptr;
        }
        return &ComponentManager::GetComponentUnchecked<TimelineComponent>(director);
    }

    bool IsAlive(EntityID entity) override
    {
        return entity != g_kINVALID_ENTITY && Registry::IsAlive(entity);
    }

    const char* GetEntityName(EntityID entity) override
    {
        if (!IsAlive(entity))
        {
            return "(なし)";
        }
        return ImGuiManager::GetEntityDisplayName(entity);
    }

    SequencerEntityKind GetEntityKind(EntityID entity) override
    {
        if (!IsAlive(entity))
        {
            return SequencerEntityKind::Object;
        }
        if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
        {
            return SequencerEntityKind::Character;
        }
        if (ComponentManager::HasComponent<CameraComponent>(entity))
        {
            return SequencerEntityKind::Camera;
        }
        if (ComponentManager::HasComponent<LightComponent>(entity))
        {
            return SequencerEntityKind::Light;
        }
        return SequencerEntityKind::Object;
    }

    EntityID GetEditorSelection() override
    {
        return ImGuiManager::m_SelectedEntity;
    }

    void SetEditorSelection(EntityID entity) override
    {
        if (IsAlive(entity))
        {
            ImGuiManager::SelectEntity(entity);
        }
    }

    bool ReadProperty(EntityID target, TimelineProperty property, int fogVolume, DirectX::XMFLOAT4& value) override
    {
        bool valid = false;
        value = TimeLineSystem::ReadProperty(target, property, &valid, fogVolume);
        return valid;
    }

    bool IsSpotLight(EntityID entity) override
    {
        return IsAlive(entity) && ComponentManager::HasComponent<LightComponent>(entity) &&
               ComponentManager::GetComponentUnchecked<LightComponent>(entity).Type == LightType::Spot;
    }

    bool IsPhysicallyBasedLight(EntityID entity) override
    {
        return IsAlive(entity) && ComponentManager::HasComponent<LightComponent>(entity) &&
               ComponentManager::GetComponentUnchecked<LightComponent>(entity).PhysicallyBased;
    }

    // ---- 再生・評価
    bool IsSimulationRunning() override
    {
        // Playing 中は TimeLineSystem::Update（Play 専用システム）が毎フレーム評価する
        return ProjectManager::IsSimulationRunning();
    }

    bool IsProjectPlaying() override
    {
        return ProjectManager::IsPlaying();
    }

    float GetDeltaTime() override
    {
        // 編集中は World の時間が進まないため、エディターの実時間で進める
        return ImGui::GetIO().DeltaTime;
    }

    float GetProjectFrameRate() override
    {
        return static_cast<float>(World::GetTargetFrameRate());
    }

    void Evaluate(EntityID, TimelineComponent& timeline, bool cut) override
    {
        if (cut)
        {
            Camera::NotifyCut();
        }
        TimeLineSystem::EvaluateComponent(timeline);
    }

    void Advance(EntityID director, TimelineComponent& timeline, float deltaTime) override
    {
        TimeLineSystem::AdvanceComponent(director, timeline, deltaTime);
    }

    // ---- Undo / Redo（ImGuiManager の履歴に統合）
    void BeginEdit(EntityID director) override
    {
        if (!m_EditOpen && IsAlive(director))
        {
            m_EditOpen = true;
            m_EditDirector = director;
            m_Before = ImGuiManager::CaptureEntity(director);
        }
    }

    void CommitEdit(bool changed) override
    {
        if (!m_EditOpen)
        {
            return;
        }
        m_EditOpen = false;
        if (changed && IsAlive(m_EditDirector))
        {
            ImGuiManager::BeginUndoCapture(m_EditDirector, m_Before);
            ImGuiManager::FinalizeUndoCaptureIfIdle(true);
        }
        m_Before = {};
    }

    void CaptureFrameState(EntityID director) override
    {
        // ウィジェット操作中は再取得しない（操作開始時の状態を Undo に使う）
        if (!m_EditOpen && IsAlive(director) && (!ImGui::IsAnyItemActive() || m_FrameDirector != director))
        {
            m_FrameDirector = director;
            m_FrameState = ImGuiManager::CaptureEntity(director);
        }
    }

    void BeginEditFromFrameState() override
    {
        if (!m_EditOpen && IsAlive(m_FrameDirector))
        {
            m_EditOpen = true;
            m_EditDirector = m_FrameDirector;
            m_Before = m_FrameState;
        }
    }

    uint64_t GetHistoryRevision() override
    {
        return ImGuiManager::m_EditRevision;
    }

    // ---- 既存 UI の再利用
    bool DrawAnimationLayers(EntityID director, size_t trackIndex, TimelineTrackData& track) override
    {
        if (!ComponentManager::HasComponent<AnimationModelComponent>(track.Target))
        {
            ImGui::TextWrapped("対象 Entity にアニメーションモデルがありません。");
            return false;
        }
        return ImGuiManager::DrawTimelineAnimations(director, trackIndex, track);
    }

    bool AppendAnimation(TimelineTrackData& track, const std::string& path) override
    {
        return ImGuiManager::AppendTimelineAnimations(track, {filesystem::u8path(path)});
    }

    bool DrawMonitorFrames(EntityID director, size_t monitorIndex, LiveMonitorBinding& monitor) override
    {
        bool changed = false;
        auto editEntity = [&](const char* label, EntityID& target) {
            int id = target == g_kINVALID_ENTITY ? -1 : static_cast<int>(target);
            if (ImGui::InputInt(label, &id))
            {
                target = id < 0 ? g_kINVALID_ENTITY : static_cast<EntityID>(id);
                changed = true;
            }
        };
        editEntity("発光メッシュ Entity", monitor.Target);
        editEntity("周辺照明 Rect Entity", monitor.SurroundLight);
        changed |= ImGuiManager::DrawLiveMonitorFrames(director, monitorIndex, monitor);
        return changed;
    }

    float GetPreviewAspect() override
    {
        return GraphicsDevice::GetSceneAspectRatio();
    }

    bool DrawPreviewImage(float width, float height) override
    {
        // シーンビューと同じ描画結果を表示する（追加の描画パス・GPU 同期なし）
        const D3D12_GPU_DESCRIPTOR_HANDLE handle = RenderTargets::GetEditorSceneSrvHandle();
        if (handle.ptr == 0)
        {
            return false;
        }
        // ImTextureData* にキャストすると ImGui 1.92 では「管理テクスチャ」として扱われ、
        // GPU ディスクリプタのアドレスを CPU ポインタとして参照してしまう。ImTextureID で渡す。
        ImGui::Image(static_cast<ImTextureID>(handle.ptr), ImVec2(width, height));
        return true;
    }

    const char* GetPreviewLabel() override
    {
        return ProjectManager::IsPlaying() ? "Game Camera" : "Editor Camera（シーンビュー）";
    }

    std::string MakeAssetRelativePath(const std::string& path) override
    {
        return ImGuiManager::MakeRelativeAssetPath(filesystem::u8path(path));
    }

    void Log(const char* message) override
    {
        ImGuiManager::AddLog("%s", message);
    }

  private:
    std::unique_ptr<SequencerWindow> m_Window;
    bool m_EditOpen = false;
    EntityID m_EditDirector = g_kINVALID_ENTITY;
    EntityID m_FrameDirector = g_kINVALID_ENTITY;
    ImGuiManager::EntitySnapshot m_Before{};
    ImGuiManager::EntitySnapshot m_FrameState{};
};

void SequencerEditorBridge::RegisterSettings()
{
    SequencerWindow::RegisterSettingsHandler();
}

void SequencerEditorBridge::Draw(bool* open)
{
    if (!open || !*open)
    {
        return;
    }
    SequencerEditorBridgeImpl::Instance().Window().Draw(open);
}

void SequencerEditorBridge::Shutdown()
{
    SequencerEditorBridgeImpl::Instance().Reset();
}
