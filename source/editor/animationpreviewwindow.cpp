#include "pch.h"
#include "animationpreviewwindow.h"
#include "animationpreviewstate.h"
#include "animationpreviewcontext.h"
#include "animator.h"
#include "editordescriptors.h"
#include "graphicsdevice.h"
#include "imguimanager.h"
#include "materialmanager.h"
#include "modelmanager.h"
#include "physicssystem.h"
#include "proceduralio.h"
#include "projectmanager.h"
#include "systemmanager.h"
#include "texturemanager.h"
#include "world.h"
#include "gpuscene/gpuscenechanges.h"

namespace
{
    constexpr wchar_t modelFilter[] = L"モデル (*.pmx;*.fbx;*.vrm;*.gltf;*.glb)\0*.pmx;*.fbx;*.vrm;*.gltf;*.glb\0";
    constexpr wchar_t clipFilter[] =
        L"アニメーション (*.vmd;*.fbx;*.gltf;*.glb;*.dae;*.bvh)\0*.vmd;*.fbx;*.gltf;*.glb;*.dae;*.bvh\0";

    struct PreviewState
    {
        unique_ptr<AnimationModelResource> Model;
        unique_ptr<AnimationPreviewContext> Context;
        shared_ptr<EditorDescriptorLease> Descriptor;
        vector<AnimationPreview::Clip> Clips;
        vector<MaterialComponent> Materials;
        AnimationPreview::Playback Playback;
        string ModelPath;
        bool Convert = true;
        int SourceModelId = -1;
        shared_ptr<AnimationModelResource> SourceOwner;
        int SelectedClip = -1;
        bool Visible = false;
        bool ImportSelection = false;
        char ModelInput[2048]{};
        char ClipInput[2048]{};
        char NameInput[256]{};
        string Error;
        string Status;
    };

    PreviewState preview;

    struct TextureLoadingScope
    {
        TextureLoadingScope()
        {
            TextureManager::BeginTextureLoading();
        }

        ~TextureLoadingScope()
        {
            TextureManager::EndTextureLoading();
        }
    };

    string AssetPath(const filesystem::path& path)
    {
        error_code error;
        const auto relative = filesystem::relative(path, filesystem::current_path(), error);
        return ProceduralIO::ToUtf8(error ? path : relative);
    }

    bool SameModel(const AnimationModelComponent& animation)
    {
        error_code error;
        return animation.IsConvert == preview.Convert &&
               filesystem::equivalent(filesystem::u8path(animation.ModelPath), filesystem::u8path(preview.ModelPath),
                                      error);
    }

    const char* ApplyBlockReason(EntityID target)
    {
        if (!preview.Model)
        {
            return "モデルを読み込んでください。";
        }
        if (!Registry::IsAlive(target) || !ComponentManager::HasComponent<AnimationModelComponent>(target))
        {
            return "ヒエラルキーでアニメーションモデルを選択してください。";
        }
        if (ProjectManager::IsPlaying() || ProjectManager::IsPaused())
        {
            return "シーンの再生を停止してから適用してください。";
        }
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(target);
        if (animation.TimelineDriven)
        {
            return "タイムラインが制御中のモデルには適用できません。";
        }
        if (!SameModel(animation))
        {
            return "対象のモデルが異なります。「選択モデルを取り込む」で切り替えてください。";
        }
        if (none_of(preview.Clips.begin(), preview.Clips.end(), [](const auto& clip) {
                return clip.Enabled;
            }))
        {
            return "再生するクリップを1つ以上有効にしてください。";
        }
        return nullptr;
    }

    vector<MaterialComponent> PreviewMaterials(const AnimationModelResource& model, const MaterialComponent& original)
    {
        vector<MaterialComponent> materials;
        for (UINT index = 0; index < model.GetMeshCount(); ++index)
        {
            const auto& mesh = model.GetMeshData(index);
            auto material = MaterialManager::Resolve(original, static_cast<size_t>(max(0, mesh.MaterialIndex)));
            if (material.RuntimeHandle == 0 && !material.LiveSurfaceOverride && mesh.TextureIndex >= 0)
            {
                material.TextureID = mesh.TextureIndex;
                material.UseTexture = true;
            }
            materials.push_back(move(material));
        }
        return materials;
    }

    unique_ptr<AnimationModelResource> BuildModel(const string& path, bool convert, int sourceId,
                                                  vector<AnimationPreview::Clip>& clips,
                                                  const AnimationModelResource* sourceOverride = nullptr)
    try
    {
        if (!AnimationPreview::ValidateClips(clips))
        {
            preview.Error = "クリップ名の重複、または空のファイル指定があります。";
            return {};
        }
        auto model = make_unique<AnimationModelResource>();
        const auto* source = sourceOverride ? sourceOverride : ModelManager::GetAnimModel(sourceId);
        bool loaded = false;
        {
            TextureLoadingScope loading;
            loaded = source ? model->LoadPoseInstance(*source, GraphicsDevice::GetDevice(), false)
                            : model->Load(path.c_str(), GraphicsDevice::GetDevice(), convert);
        }
        if (!loaded)
        {
            preview.Error = "モデルを読み込めません: " + path;
            return {};
        }
        for (auto& clip : clips)
        {
            if (!model->LoadAnimation(clip.Path.c_str(), clip.Name.c_str()))
            {
                preview.Error = "アニメーションを読み込めません（現在のプレビューは保持します）: " + clip.Path;
                return {};
            }
            clip.Seconds = model->GetAnimationDuration(clip.Name);
        }
        return model;
    }
    catch (const exception& error)
    {
        preview.Error = "読み込みを中止しました（現在のプレビューは保持します）: " + string(error.what());
        return {};
    }

    void UpdatePose()
    {
        if (!preview.Model)
        {
            return;
        }
        vector<AnimationPlaybackLayer> layers;
        for (const auto& clip : preview.Clips)
        {
            if (clip.Enabled)
            {
                const float time =
                    AnimationPreview::LayerSampleTime(preview.Playback.Time, clip.Seconds,
                                                      AnimationPreview::Duration(preview.Clips), preview.Playback.Loop);
                layers.push_back({clip.Name, time});
            }
        }
        if (layers.empty())
        {
            preview.Model->ResetBoneMatricesToBindPose();
        }
        else
        {
            preview.Model->UpdateBoneMatrices(layers);
        }
    }

    bool CommitPreview(unique_ptr<AnimationModelResource> model, vector<AnimationPreview::Clip> clips,
                       const string& path, bool convert, int sourceId, const MaterialComponent& material)
    {
        if (!model)
        {
            return false;
        }
        auto descriptor = EditorDescriptors::Allocate();
        auto context = make_unique<AnimationPreviewContext>();
        if (!descriptor || !context->Initialize(768, descriptor->Index, model->GetMeshCount()))
        {
            preview.Error = "プレビュー描画先を作成できません。";
            return false;
        }
        context->FrameModel(model->GetAabbCenter(), model->GetAabbExtents());
        // This runs during UI construction, before this frame records any preview draws.
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            preview.Error = "GPU の完了を待機できません。";
            return false;
        }
        preview.Materials = PreviewMaterials(*model, material);
        preview.Model = move(model);
        preview.Context = move(context);
        preview.Descriptor = move(descriptor);
        preview.Clips = move(clips);
        preview.ModelPath = path;
        preview.Convert = convert;
        preview.SourceModelId = sourceId;
        if (sourceId < 0)
        {
            preview.SourceOwner.reset();
        }
        preview.SelectedClip = preview.Clips.empty() ? -1 : 0;
        preview.Playback.Time = 0.0f;
        preview.Playback.Playing = AnimationPreview::Duration(preview.Clips) > 0.0f;
        strncpy_s(preview.ModelInput, path.c_str(), _TRUNCATE);
        preview.Error.clear();
        preview.Status = "プレビューを読み込みました。シーンへの反映は「選択モデルに適用」で行います。";
        UpdatePose();
        return true;
    }

    void ImportSelectedModel()
    {
        const auto target = ImGuiManager::GetSelectedEntity();
        if (!Registry::IsAlive(target) || !ComponentManager::HasComponent<AnimationModelComponent>(target))
        {
            preview.Error = "ヒエラルキーでアニメーションモデルを選択してください。";
            return;
        }
        const auto animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(target);
        vector<AnimationPreview::Clip> clips;
        for (size_t index = 0; index < animation.Animations.size(); ++index)
        {
            const auto& name = animation.Animations[index];
            const string path = index < animation.AnimationPaths.size() && !animation.AnimationPaths[index].empty()
                                    ? animation.AnimationPaths[index]
                                    : animation.ModelPath;
            bool enabled = name == animation.CurrentAnimation;
            if (!animation.ActiveAnimationLayers.empty())
            {
                enabled = any_of(animation.ActiveAnimationLayers.begin(), animation.ActiveAnimationLayers.end(),
                                 [&](const auto& layer) {
                                     return layer.AnimationName == name;
                                 });
            }
            clips.push_back({name, path, enabled});
        }
        MaterialComponent material;
        if (ComponentManager::HasComponent<MaterialComponent>(target))
        {
            material = ComponentManager::GetComponentUnchecked<MaterialComponent>(target);
        }
        auto model = BuildModel(animation.ModelPath, animation.IsConvert, animation.ModelId, clips);
        if (CommitPreview(move(model), move(clips), animation.ModelPath, animation.IsConvert, animation.ModelId,
                          material))
        {
            preview.Playback.Speed = clamp(animation.Speed, 0.0f, 4.0f);
            preview.SourceOwner = animation.PoseOwner;
        }
    }

    void AcceptAssetPath(char* buffer, size_t capacity)
    {
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
            {
                if (payload->Data && payload->DataSize > 0)
                {
                    const string path(static_cast<const char*>(payload->Data),
                                      strnlen_s(static_cast<const char*>(payload->Data), payload->DataSize));
                    strncpy_s(buffer, capacity, path.c_str(), _TRUNCATE);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    bool PathInput(const char* label, char* buffer, size_t capacity, const wchar_t* filter)
    {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-65.0f);
        ImGui::InputText("##Path", buffer, capacity);
        AcceptAssetPath(buffer, capacity);
        ImGui::SameLine();
        bool picked = false;
        if (ImGui::Button("参照"))
        {
            filesystem::path path;
            if (ProceduralIO::OpenFileDialog(filter, path))
            {
                strncpy_s(buffer, capacity, AssetPath(path).c_str(), _TRUNCATE);
                picked = true;
            }
        }
        ImGui::PopID();
        return picked;
    }

    void DrawModelSettings()
    {
        ImGui::SeparatorText("プレビューモデル");
        if (ImGui::Button("選択モデルを取り込む", ImVec2(-1, 0)))
        {
            ImportSelectedModel();
        }
        PathInput("モデルファイル", preview.ModelInput, sizeof(preview.ModelInput), modelFilter);
        static bool convert = true;
        ImGui::Checkbox("座標変換して読み込む", &convert);
        ImGui::BeginDisabled(preview.ModelInput[0] == '\0');
        if (ImGui::Button("モデルを読み込む", ImVec2(-1, 0)))
        {
            const string path = preview.ModelInput;
            vector<AnimationPreview::Clip> clips;
            auto model = BuildModel(path, convert, -1, clips);
            CommitPreview(move(model), move(clips), path, convert, -1, MaterialComponent{});
        }
        ImGui::EndDisabled();
        if (!preview.ModelPath.empty())
        {
            ImGui::TextWrapped("表示中: %s", preview.ModelPath.c_str());
        }
    }

    void SelectClip(int index)
    {
        preview.SelectedClip = index;
        const auto& clip = preview.Clips[index];
        strncpy_s(preview.ClipInput, clip.Path.c_str(), _TRUNCATE);
        strncpy_s(preview.NameInput, clip.Name.c_str(), _TRUNCATE);
    }

    void LoadClip(bool replace)
    {
        auto clips = preview.Clips;
        if (replace)
        {
            clips[preview.SelectedClip].Path = preview.ClipInput;
            AnimationPreview::SetClipEnabled(clips, preview.SelectedClip, true);
        }
        else
        {
            string name = preview.NameInput;
            if (name.empty())
            {
                name = ProceduralIO::ToUtf8(filesystem::u8path(preview.ClipInput).stem());
            }
            const string baseName = name;
            for (int suffix = 2; any_of(clips.begin(), clips.end(),
                                        [&](const auto& clip) {
                                            return clip.Name == name;
                                        });
                 ++suffix)
            {
                name = baseName + " (" + to_string(suffix) + ")";
            }
            clips.push_back({name, preview.ClipInput, true});
            AnimationPreview::SetClipEnabled(clips, clips.size() - 1, true);
        }
        auto model = BuildModel(preview.ModelPath, preview.Convert, preview.SourceModelId, clips);
        MaterialComponent original;
        const auto target = ImGuiManager::GetSelectedEntity();
        if (Registry::IsAlive(target) && ComponentManager::HasComponent<AnimationModelComponent>(target) &&
            SameModel(ComponentManager::GetComponentUnchecked<AnimationModelComponent>(target)) &&
            ComponentManager::HasComponent<MaterialComponent>(target))
        {
            original = ComponentManager::GetComponentUnchecked<MaterialComponent>(target);
        }
        const int selection = replace ? preview.SelectedClip : static_cast<int>(clips.size()) - 1;
        if (CommitPreview(move(model), move(clips), preview.ModelPath, preview.Convert, preview.SourceModelId,
                          original))
        {
            SelectClip(selection);
        }
    }

    void DrawClipSettings()
    {
        ImGui::SeparatorText("アニメーションクリップ");
        ImGui::BeginDisabled(!preview.Model);
        if (ImGui::BeginListBox("##Clips", ImVec2(-1, 155)))
        {
            for (int index = 0; index < static_cast<int>(preview.Clips.size()); ++index)
            {
                auto& clip = preview.Clips[index];
                ImGui::PushID(index);
                if (ImGui::Checkbox("##Enabled", &clip.Enabled))
                {
                    AnimationPreview::SetClipEnabled(preview.Clips, index, clip.Enabled);
                    preview.Playback.Seek(preview.Playback.Time, AnimationPreview::Duration(preview.Clips));
                }
                ImGui::SameLine();
                if (ImGui::Selectable(clip.Name.c_str(), preview.SelectedClip == index))
                {
                    SelectClip(index);
                }
                ImGui::SetItemTooltip("%s\n%.2f 秒", clip.Path.c_str(), clip.Seconds);
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::TextWrapped(
            "VMD は身体と表情を同時再生できます。同じボーンは下のクリップを優先します。FBX などは1つずつ再生します。");
        if (PathInput("アニメーションファイル", preview.ClipInput, sizeof(preview.ClipInput), clipFilter))
        {
            const auto name = ProceduralIO::ToUtf8(filesystem::u8path(preview.ClipInput).stem());
            strncpy_s(preview.NameInput, name.c_str(), _TRUNCATE);
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ClipName", "追加するクリップの名前", preview.NameInput, sizeof(preview.NameInput));
        ImGui::BeginDisabled(preview.ClipInput[0] == '\0');
        if (ImGui::Button("追加"))
        {
            LoadClip(false);
        }
        ImGui::SameLine();
        const bool validSelection =
            preview.SelectedClip >= 0 && preview.SelectedClip < static_cast<int>(preview.Clips.size());
        ImGui::BeginDisabled(!validSelection);
        if (ImGui::Button("選択クリップを付け替え"))
        {
            LoadClip(true);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!validSelection);
        if (ImGui::Button("削除"))
        {
            preview.Clips.erase(preview.Clips.begin() + preview.SelectedClip);
            preview.SelectedClip = -1;
            preview.Playback.Seek(0.0f, AnimationPreview::Duration(preview.Clips));
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }

    void DrawPlayback()
    {
        ImGui::SeparatorText("再生プレビュー");
        const float duration = AnimationPreview::Duration(preview.Clips);
        ImGui::BeginDisabled(!preview.Model || duration <= 0.0f);
        if (ImGui::Button(preview.Playback.Playing ? "一時停止" : "再生"))
        {
            if (!preview.Playback.Playing && preview.Playback.Time >= duration)
            {
                preview.Playback.Time = 0.0f;
            }
            preview.Playback.Playing = !preview.Playback.Playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("先頭へ"))
        {
            preview.Playback.Time = 0.0f;
        }
        ImGui::Checkbox("プレビューをループ", &preview.Playback.Loop);
        ImGui::SliderFloat("速度", &preview.Playback.Speed, 0.0f, 4.0f, "%.2f 倍");
        if (ImGui::SliderFloat("再生位置", &preview.Playback.Time, 0.0f, duration, "%.2f 秒"))
        {
            preview.Playback.Playing = false;
        }
        ImGui::Text("%.2f / %.2f 秒", preview.Playback.Time, duration);
        ImGui::EndDisabled();
    }

    void DrawPreviewImage()
    {
        ImGui::TextUnformatted("プレビュー");
        if (!preview.Model || !preview.Context || !preview.Descriptor)
        {
            ImGui::TextWrapped("左側で選択モデルを取り込むか、モデルファイルを読み込んでください。");
            return;
        }
        if (ImGui::Button("全体を表示"))
        {
            preview.Context->FrameModel(preview.Model->GetAabbCenter(), preview.Model->GetAabbExtents());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("ドラッグ: 回転 / ホイール: ズーム");
        const auto available = ImGui::GetContentRegionAvail();
        const float side = max(1.0f, min(available.x, available.y));
        ImGui::ImageButton("##AnimationOrbit", reinterpret_cast<ImTextureData*>(preview.Descriptor->Gpu.ptr),
                           ImVec2(side, side), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0));
        const auto& input = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            preview.Context->Orbit(input.MouseDelta.x * 0.01f, input.MouseDelta.y * 0.01f, 0.0f);
        }
        if (ImGui::IsItemHovered())
        {
            preview.Context->Orbit(0.0f, 0.0f, input.MouseWheel);
        }
    }
}

void AnimationPreviewWindow::OpenForSelection()
{
    ImGuiManager::m_ShowAnimationPreviewWindow = true;
    preview.ImportSelection = true;
}

void AnimationPreviewWindow::ApplyToSelection()
{
    const auto target = ImGuiManager::GetSelectedEntity();
    if (const char* reason = ApplyBlockReason(target))
    {
        preview.Error = reason;
        return;
    }
    // Stage every load before changing the scene; the old model remains available to Undo.
    auto clips = preview.Clips;
    auto model = BuildModel(preview.ModelPath, preview.Convert, -1, clips, preview.Model.get());
    if (!model)
    {
        preview.Error = "適用用モデルを準備できません。シーンの設定は変更していません。";
        return;
    }
    const auto before = ImGuiManager::CaptureEntity(target);
    auto animation = before.AnimationModel;
    const auto selection = AnimationPreview::BuildSelection(preview.Clips);
    animation.Animations = selection.Names;
    animation.AnimationPaths = selection.Paths;
    animation.UseIndependentPose = true;
    animation.Speed = preview.Playback.Speed;
    Animator::Play(animation, selection.PlayingNames);
    animation.ModelId = ModelManager::RegisterAnimModelInstance(move(model), animation.PoseOwner);
    if (animation.ModelId < 0)
    {
        preview.Error = "適用用モデルを登録できません。";
        return;
    }
    auto* resource = ModelManager::GetAnimModel(animation.ModelId);
    if (animation.ActiveAnimationLayers.size() > 1)
    {
        resource->UpdateBoneMatrices(animation.ActiveAnimationLayers);
    }
    else
    {
        resource->UpdateBoneMatrices(animation.CurrentAnimation.c_str(), 0.0f, animation.CurrentAnimation.c_str(), 0.0f,
                                     0.0f);
    }
    ComponentManager::GetComponentUnchecked<AnimationModelComponent>(target) = move(animation);
    GpuSceneChanges::Notify(target);
    if (auto* physics = SystemManager::GetSystem<PhysicsSystem>())
    {
        physics->ResetSimulation();
    }
    ImGuiManager::PushUndoSnapshot(before);
    preview.Error.clear();
    preview.Status = "選択モデルに適用しました。シーンを保存すると、次回もこの設定で開きます。";
}

void AnimationPreviewWindow::Draw(bool* open)
{
    preview.Visible = false;
    if (!open || !*open)
    {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(1150, 780), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("アニメーションプレビュー", open))
    {
        ImGui::End();
        return;
    }
    preview.Visible = true;
    if (preview.ImportSelection)
    {
        preview.ImportSelection = false;
        ImportSelectedModel();
    }
    if (ImGui::BeginTable("##AnimationLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("設定", ImGuiTableColumnFlags_WidthFixed, 380.0f);
        ImGui::TableSetupColumn("プレビュー", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##AnimationSettings", ImVec2(0, 0), ImGuiChildFlags_None);
        DrawModelSettings();
        DrawClipSettings();
        DrawPlayback();
        ImGui::SeparatorText("シーンへ適用");
        const auto target = ImGuiManager::GetSelectedEntity();
        if (Registry::IsAlive(target) && ComponentManager::HasComponent<NameComponent>(target))
        {
            ImGui::TextWrapped("適用先: %s",
                               ComponentManager::GetComponentUnchecked<NameComponent>(target).Name.c_str());
        }
        const char* reason = ApplyBlockReason(target);
        ImGui::BeginDisabled(reason != nullptr);
        if (ImGui::Button("選択モデルに適用", ImVec2(-1, 0)))
        {
            ApplyToSelection();
        }
        ImGui::EndDisabled();
        if (reason)
        {
            ImGui::TextWrapped("%s", reason);
        }
        ImGui::TextWrapped(
            "有効クリップと速度を適用し、先頭から再生します。位置・回転・スケール・マテリアルは維持します。");
        if (!preview.Error.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1), "%s", preview.Error.c_str());
        }
        if (!preview.Status.empty())
        {
            ImGui::TextWrapped("%s", preview.Status.c_str());
        }
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##AnimationPreview", ImVec2(0, 0), ImGuiChildFlags_None);
        DrawPreviewImage();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    preview.Playback.Advance(ImGui::GetIO().DeltaTime, AnimationPreview::Duration(preview.Clips));
    UpdatePose();
    ImGui::End();
}

void AnimationPreviewWindow::Render()
{
    if (preview.Visible && preview.Model && preview.Context &&
        !preview.Context->Render(*preview.Model, preview.Materials))
    {
        preview.Error = "プレビューを描画できません: " + preview.Context->Error();
    }
}

void AnimationPreviewWindow::Shutdown()
{
    preview = PreviewState{};
}
