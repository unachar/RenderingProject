#include "pch.h"
#include "editorworkspace.h"
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
#include "vmdretarget.h"
#include "gpuscene/gpuscenechanges.h"

namespace
{
    constexpr wchar_t modelFilter[] = L"モデル (*.pmx;*.fbx;*.vrm;*.gltf;*.glb)\0*.pmx;*.fbx;*.vrm;*.gltf;*.glb\0";
    constexpr wchar_t clipFilter[] =
        L"アニメーション (*.vmd;*.fbx;*.gltf;*.glb;*.dae;*.bvh)\0*.vmd;*.fbx;*.gltf;*.glb;*.dae;*.bvh\0";

    struct PreviewSpring
    {
        string Bone;
        string Tip;
        float Stiffness = 1.0f;
        float Drag = 0.4f;
        float Gravity = 0.0f;
        XMFLOAT3 GravityDirection{0.0f, -1.0f, 0.0f};
        XMFLOAT3 PreviousTip{};
        XMFLOAT3 CurrentTip{};
        bool Initialized = false;
    };

    struct PreviewState
    {
        unique_ptr<AnimationModelResource> Model;
        unique_ptr<AnimationPreviewContext> Context;
        shared_ptr<EditorDescriptorLease> Descriptor;
        vector<AnimationPreview::Clip> Clips;
        vector<MaterialComponent> Materials;
        AnimationPreview::Playback Playback;
        unique_ptr<IPhysicsBackend> Physics;
        PmxBoneRig PhysicsRig;
        bool PhysicsEnabled = true;
        bool ResetPhysics = true;
        double PhysicsTime = 0.0;
        string PhysicsError;
        vector<PreviewSpring> Springs;
        int SpringBone = 0;
        int SpringTip = 0;
        vector<string> Expressions;
        int Expression = 0;
        float ExpressionWeight = 0.0f;
        float AppliedExpressionWeight = 0.0f;
        int AppliedExpression = 0;
        int ExpressionDriver = 0;
        int ExpressionViewAxis = 0;
        float ExpressionAngleStart = 45.0f;
        float ExpressionAngleEnd = 90.0f;
        float ExpressionStart = 0.0f;
        float ExpressionEnd = 1.0f;
        string ModelPath;
        bool Convert = true;
        int SourceModelId = -1;
        shared_ptr<AnimationModelResource> SourceOwner;
        int SelectedClip = -1;
        bool Visible = false;
        bool ShowWireframe = false;
        bool ShowSkeleton = false;
        bool ShowWeights = false;
        int WeightBone = 0;
        int LastPlaybackFrame = -1;
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

    void SamplePose(float sampleTime)
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
                    AnimationPreview::LayerSampleTime(sampleTime, clip.Seconds,
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

    void ClearPreviewPhysics()
    {
        if (preview.Physics)
        {
            preview.PhysicsRig.Destroy(*preview.Physics);
            preview.Physics.reset();
        }
    }

    vector<PreviewSpring> LoadVrmSprings(const string& path, bool convert)
    {
        string extension = filesystem::u8path(path).extension().string();
        transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char letter) {
            return static_cast<char>(tolower(letter));
        });
        if (extension != ".vrm")
        {
            return {};
        }
        Json::Value root;
        if (!VmdRetarget::ReadGlbJson(filesystem::u8path(path), root))
        {
            return {};
        }

        const Json::Value& document = root;
        const auto& nodes = document["nodes"];
        const auto& springs = document["extensions"]["VRMC_springBone"]["springs"];
        const auto nodeName = [&nodes](const Json::Value& joint) -> string {
            const auto& index = joint["node"];
            if (!index.isUInt() || index.asUInt() >= nodes.size())
            {
                return {};
            }
            const string name = nodes[index.asUInt()]["name"].asString();
            return name.empty() ? "nodes[" + to_string(index.asUInt()) + "]" : name;
        };
        vector<PreviewSpring> result;
        for (const auto& chain : springs)
        {
            const auto& joints = chain["joints"];
            for (Json::ArrayIndex index = 1; index < joints.size(); ++index)
            {
                const auto& joint = joints[index - 1];
                PreviewSpring spring;
                spring.Bone = nodeName(joint);
                spring.Tip = nodeName(joints[index]);
                if (spring.Bone.empty() || spring.Tip.empty() || spring.Bone == spring.Tip)
                {
                    continue;
                }
                spring.Stiffness = joint.get("stiffness", 1.0f).asFloat();
                spring.Drag = joint.get("dragForce", 0.4f).asFloat();
                spring.Gravity = joint.get("gravityPower", 0.0f).asFloat();
                spring.Stiffness = clamp(spring.Stiffness, 0.0f, 100.0f);
                spring.Drag = clamp(spring.Drag, 0.0f, 1.0f);
                spring.Gravity = clamp(spring.Gravity, 0.0f, 100.0f);
                const auto& gravity = joint["gravityDir"];
                if (gravity.isArray() && gravity.size() == 3 && gravity[0].isNumeric() &&
                    gravity[1].isNumeric() && gravity[2].isNumeric())
                {
                    spring.GravityDirection = {gravity[0].asFloat(), gravity[1].asFloat(),
                                               gravity[2].asFloat() * (convert ? -1.0f : 1.0f)};
                }
                if (none_of(result.begin(), result.end(), [&](const PreviewSpring& existing) {
                        return existing.Bone == spring.Bone;
                    }))
                {
                    result.push_back(move(spring));
                }
            }
        }
        return result;
    }

    void ApplySpringPose(float step)
    {
        bool changed = false;
        for (auto& spring : preview.Springs)
        {
            XMFLOAT4X4 boneMatrix{}, tipMatrix{};
            if (!preview.Model->GetBoneGlobalTransform(spring.Bone, boneMatrix) ||
                !preview.Model->GetBoneGlobalTransform(spring.Tip, tipMatrix))
            {
                continue;
            }
            XMMATRIX bone = XMLoadFloat4x4(&boneMatrix);
            const XMVECTOR origin = bone.r[3];
            const XMVECTOR animatedTip = XMLoadFloat4x4(&tipMatrix).r[3];
            const XMVECTOR axis = animatedTip - origin;
            const float length = XMVectorGetX(XMVector3Length(axis));
            if (!(length > 0.00001f))
            {
                continue;
            }
            const XMVECTOR restDirection = axis / length;
            if (!spring.Initialized)
            {
                XMStoreFloat3(&spring.CurrentTip, animatedTip);
                spring.PreviousTip = spring.CurrentTip;
                spring.Initialized = true;
                continue;
            }
            XMVECTOR direction = XMLoadFloat3(&spring.CurrentTip) - origin;
            if (step > 0.0f)
            {
                direction += (XMLoadFloat3(&spring.CurrentTip) - XMLoadFloat3(&spring.PreviousTip)) *
                             (1.0f - clamp(spring.Drag, 0.0f, 1.0f));
                direction += (restDirection * spring.Stiffness +
                              XMLoadFloat3(&spring.GravityDirection) * spring.Gravity) * step;
            }
            const float directionLength = XMVectorGetX(XMVector3Length(direction));
            direction = directionLength > 0.00001f ? direction / directionLength : restDirection;
            if (step > 0.0f)
            {
                spring.PreviousTip = spring.CurrentTip;
                XMStoreFloat3(&spring.CurrentTip, origin + direction * length);
            }

            const float cosine = clamp(XMVectorGetX(XMVector3Dot(restDirection, direction)), -1.0f, 1.0f);
            if (cosine > 0.999999f)
            {
                continue;
            }
            XMVECTOR rotationAxis = XMVector3Cross(restDirection, direction);
            if (cosine < -0.999999f)
            {
                rotationAxis = XMVector3Orthogonal(restDirection);
            }
            const XMMATRIX rotation = XMMatrixRotationAxis(rotationAxis, acosf(cosine));
            bone.r[3] = XMVectorSet(0, 0, 0, 1);
            bone *= rotation;
            bone.r[3] = origin;
            XMStoreFloat4x4(&boneMatrix, bone);
            changed |= preview.Model->SetBoneGlobalTransform(spring.Bone, boneMatrix, true);
        }
        if (changed)
        {
            preview.Model->CommitPhysicsPose();
            preview.Model->InvalidateAnimationPoseCache();
        }
    }

    void UpdateSpringPose(float time)
    {
        constexpr double step = 1.0 / 60.0;
        if (preview.ResetPhysics || time < preview.PhysicsTime || time - preview.PhysicsTime > step * 8)
        {
            preview.ResetPhysics = false;
            preview.PhysicsTime = time;
            for (auto& spring : preview.Springs)
            {
                spring.Initialized = false;
            }
        }
        while (time - preview.PhysicsTime >= step)
        {
            preview.PhysicsTime += step;
            SamplePose(static_cast<float>(preview.PhysicsTime));
            ApplySpringPose(static_cast<float>(step));
        }
        SamplePose(time);
        ApplySpringPose(0.0f);
    }

    void UpdatePose()
    {
        if (!preview.Model)
        {
            return;
        }
        const float time = preview.Playback.Time;
        if (preview.PhysicsEnabled && !preview.Springs.empty())
        {
            UpdateSpringPose(time);
            return;
        }
        if (!preview.PhysicsEnabled || !preview.Model->HasPmxPhysics())
        {
            ClearPreviewPhysics();
            SamplePose(time);
            return;
        }

        const TransformComponent transform;
        constexpr double fixedStep = 1.0 / 60.0;
        constexpr int maxSteps = 8;
        if (preview.ResetPhysics || time < preview.PhysicsTime || time - preview.PhysicsTime > fixedStep * maxSteps)
        {
            ClearPreviewPhysics();
            preview.ResetPhysics = false;
            preview.PhysicsTime = time;
            preview.PhysicsError.clear();
            SamplePose(time);

            auto backend = CreateBulletPhysicsBackend();
            if (!backend->Init())
            {
                preview.PhysicsError = "プレビュー用の物理エンジンを初期化できません。";
                return;
            }
            if (const auto* scenePhysics = SystemManager::GetSystem<PhysicsSystem>())
            {
                backend->SetGravity(scenePhysics->GetSettings().Gravity);
            }
            preview.PhysicsRig.Build(*backend, *preview.Model, PhysicsComponent{}, transform);
            preview.PhysicsRig.SyncBodies(*backend, *preview.Model, transform, true);
            preview.Physics = move(backend);
            return;
        }

        if (preview.Physics)
        {
            for (int step = 0; step < maxSteps && time - preview.PhysicsTime >= fixedStep; ++step)
            {
                preview.PhysicsTime += fixedStep;
                SamplePose(static_cast<float>(preview.PhysicsTime));
                preview.PhysicsRig.SyncBodies(*preview.Physics, *preview.Model, transform);
                preview.Physics->Step(static_cast<float>(fixedStep));
            }
        }
        SamplePose(time);
        if (preview.Physics)
        {
            preview.PhysicsRig.Apply(*preview.Physics, *preview.Model, transform);
            preview.Model->InvalidateAnimationPoseCache();
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
        preview.ResetPhysics = true;
        preview.Model = move(model);
        if (preview.ModelPath != path || preview.Convert != convert)
        {
            preview.Springs.clear();
            try
            {
                preview.Springs = LoadVrmSprings(path, convert);
            }
            catch (const Json::Exception&)
            {
                preview.Springs.clear();
            }
        }
        preview.Expressions = preview.Model->GetPreviewExpressionNames();
        preview.Expression = 0;
        preview.AppliedExpression = 0;
        preview.AppliedExpressionWeight = 0.0f;
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
                    preview.ResetPhysics = true;
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
            preview.ResetPhysics = true;
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
                preview.ResetPhysics = true;
                preview.Playback.Time = 0.0f;
            }
            preview.Playback.Playing = !preview.Playback.Playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("先頭へ"))
        {
            preview.ResetPhysics = true;
            preview.Playback.Time = 0.0f;
        }
        if (ImGui::Checkbox("プレビューをループ", &preview.Playback.Loop))
        {
            preview.ResetPhysics = true;
        }
        ImGui::SliderFloat("速度", &preview.Playback.Speed, 0.0f, 4.0f, "%.2f 倍");
        if (ImGui::SliderFloat("再生位置", &preview.Playback.Time, 0.0f, duration, "%.2f 秒"))
        {
            preview.ResetPhysics = true;
            preview.Playback.Playing = false;
        }
        ImGui::Text("%.2f / %.2f 秒", preview.Playback.Time, duration);
        ImGui::EndDisabled();
        if (preview.Model && (preview.Model->HasPmxPhysics() || !preview.Springs.empty()))
        {
            if (ImGui::Checkbox("揺れものの物理を再生", &preview.PhysicsEnabled))
            {
                preview.ResetPhysics = true;
                preview.PhysicsError.clear();
            }
            ImGui::SetItemTooltip("シーク時に揺れをリセットします。");
            if (!preview.PhysicsError.empty())
            {
                ImGui::TextWrapped("%s", preview.PhysicsError.c_str());
            }
        }

    }

    void DrawExpressionPreview()
    {
        if (preview.Expressions.empty())
        {
            return;
        }
        ImGui::SeparatorText("表情モーフ");
        if (ImGui::BeginCombo("モーフ", preview.Expressions[preview.Expression].c_str()))
        {
            for (int index = 0; index < static_cast<int>(preview.Expressions.size()); ++index)
            {
                if (ImGui::Selectable(preview.Expressions[index].c_str(), preview.Expression == index))
                {
                    preview.Expression = index;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("表情の強さ", &preview.ExpressionWeight, 0.0f, 1.0f);
        const char* drivers[] = {"手動", "再生位置", "カメラ角度"};
        ImGui::Combo("モーフの制御", &preview.ExpressionDriver, drivers, IM_ARRAYSIZE(drivers));
        if (preview.ExpressionDriver == 2)
        {
            const char* axes[] = {"左右共通", "右側", "左側", "見上げ"};
            ImGui::Combo("補正する方向", &preview.ExpressionViewAxis, axes, IM_ARRAYSIZE(axes));
            ImGui::SliderFloat("補正開始角", &preview.ExpressionAngleStart, 0.0f, 90.0f, "%.1f°");
            ImGui::SliderFloat("補正最大角", &preview.ExpressionAngleEnd, 0.0f, 90.0f, "%.1f°");
            ImGui::SetItemTooltip("鼻ポチを後ろへ下げるモーフも、横から見たときに適用できます。");
        }
        if (preview.ExpressionDriver == 1)
        {
            ImGui::DragFloat("開始秒", &preview.ExpressionStart, 0.02f, 0.0f, 3600.0f);
            ImGui::DragFloat("終了秒", &preview.ExpressionEnd, 0.02f, 0.0f, 3600.0f);
        }
    }

    void UpdateExpressionPreview()
    {
        if (preview.Expressions.empty())
        {
            return;
        }
        float weight = preview.ExpressionWeight;
        if (preview.ExpressionDriver == 2 && preview.Context)
        {
            const float angle = preview.Context->ViewAngleDegrees(preview.ExpressionViewAxis);
            const float span = max(preview.ExpressionAngleEnd - preview.ExpressionAngleStart, 0.01f);
            const float amount = clamp((angle - preview.ExpressionAngleStart) / span, 0.0f, 1.0f);
            weight *= amount * amount * (3.0f - 2.0f * amount);
        }
        if (preview.ExpressionDriver == 1)
        {
            const float duration = max(preview.ExpressionEnd - preview.ExpressionStart, 0.001f);
            const float time = clamp((preview.Playback.Time - preview.ExpressionStart) / duration, 0.0f, 1.0f);
            weight *= time * time * (3.0f - 2.0f * time);
        }
        if (preview.AppliedExpression == preview.Expression && fabsf(preview.AppliedExpressionWeight - weight) < 1e-5f)
        {
            return;
        }
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            return;
        }
        if (preview.AppliedExpression != preview.Expression && preview.AppliedExpressionWeight > 0.0f)
        {
            preview.Model->SetPreviewExpression(preview.AppliedExpression, 0.0f);
        }
        if (preview.Model->SetPreviewExpression(preview.Expression, weight))
        {
            preview.AppliedExpression = preview.Expression;
            preview.AppliedExpressionWeight = weight;
        }
    }

    void DrawSpringPreview()
    {
        if (!preview.Model || preview.Model->HasPmxPhysics())
        {
            return;
        }
        const auto& names = preview.Model->GetBoneNames();
        if (names.size() < 2 || !ImGui::CollapsingHeader("揺れボーン設定"))
        {
            return;
        }
        auto selectBone = [&names](const char* label, int& selected) {
            selected = clamp(selected, 0, static_cast<int>(names.size()) - 1);
            if (ImGui::BeginCombo(label, names[selected].c_str()))
            {
                for (int index = 0; index < static_cast<int>(names.size()); ++index)
                {
                    if (ImGui::Selectable(names[index].c_str(), index == selected))
                    {
                        selected = index;
                    }
                }
                ImGui::EndCombo();
            }
        };
        selectBone("回転するボーン", preview.SpringBone);
        selectBone("先端ボーン", preview.SpringTip);
        const auto& bone = names[preview.SpringBone];
        const auto& tip = names[preview.SpringTip];
        const bool duplicate = any_of(preview.Springs.begin(), preview.Springs.end(),
                                      [&](const auto& spring) { return spring.Bone == bone; });
        ImGui::BeginDisabled(bone == tip || duplicate);
        if (ImGui::Button("揺れボーンを追加"))
        {
            preview.Springs.push_back({bone, tip});
            preview.ResetPhysics = true;
        }
        ImGui::EndDisabled();
        for (size_t index = 0; index < preview.Springs.size(); ++index)
        {
            auto& spring = preview.Springs[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TextWrapped("%s → %s", spring.Bone.c_str(), spring.Tip.c_str());
            ImGui::DragFloat("戻る強さ", &spring.Stiffness, 0.05f, 0.0f, 100.0f);
            ImGui::SliderFloat("減衰", &spring.Drag, 0.0f, 1.0f);
            ImGui::DragFloat("重力の強さ", &spring.Gravity, 0.01f, 0.0f, 100.0f);
            if (ImGui::Button("削除"))
            {
                preview.Springs.erase(preview.Springs.begin() + index);
                preview.ResetPhysics = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    void DrawDebugSettings()
    {
        ImGui::SeparatorText("デバッグ表示");
        ImGui::Checkbox("ワイヤーフレーム", &preview.ShowWireframe);
        ImGui::Checkbox("ボーンフレーム", &preview.ShowSkeleton);
        const auto& bones = preview.Model->GetBoneNames();
        ImGui::BeginDisabled(bones.empty());
        ImGui::Checkbox("スキンウェイト", &preview.ShowWeights);
        if (preview.ShowWeights && !bones.empty())
        {
            preview.WeightBone = clamp(preview.WeightBone, 0, static_cast<int>(bones.size()) - 1);
            if (ImGui::BeginCombo("対象ボーン", bones[preview.WeightBone].c_str()))
            {
                for (int index = 0; index < static_cast<int>(bones.size()); ++index)
                {
                    ImGui::PushID(index);
                    if (ImGui::Selectable(bones[index].c_str(), index == preview.WeightBone))
                    {
                        preview.WeightBone = index;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted("0: 濃紺  /  0.5: 緑  /  1: 橙");
        }
        ImGui::EndDisabled();
    }

    void DrawSkeletonOverlay(ImVec2 minimum, ImVec2 maximum)
    {
        const auto bones = preview.Model->GetDebugSkeleton();
        vector<ImVec2> points(bones.size());
        vector<bool> visible(bones.size());
        for (size_t index = 0; index < bones.size(); ++index)
        {
            XMFLOAT2 uv{};
            visible[index] = preview.Context->ProjectPoint(bones[index].Position, uv);
            if (visible[index])
            {
                points[index] = {minimum.x + uv.x * (maximum.x - minimum.x),
                                 minimum.y + uv.y * (maximum.y - minimum.y)};
            }
        }

        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(minimum, maximum, true);
        const auto& names = preview.Model->GetBoneNames();
        for (size_t index = 0; index < bones.size(); ++index)
        {
            if (!visible[index])
            {
                continue;
            }
            const auto& bone = bones[index];
            const bool selected = preview.ShowWeights && static_cast<size_t>(preview.WeightBone) < names.size() &&
                                  bone.Name == names[preview.WeightBone];
            const ImU32 color = selected ? IM_COL32(255, 166, 64, 255) : IM_COL32(109, 235, 255, 255);
            if (bone.Parent >= 0 && visible[bone.Parent])
            {
                draw->AddLine(points[bone.Parent], points[index], IM_COL32(15, 22, 34, 255), 3.0f);
                draw->AddLine(points[bone.Parent], points[index], color, 1.4f);
            }
            draw->AddCircleFilled(points[index], selected ? 4.0f : 2.5f, color);
        }
        draw->PopClipRect();
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
        ImGui::TextDisabled("左ドラッグ: 回転 / 中ドラッグ: 移動 / 中クリック: 中央へ / ホイール: ズーム");
        DrawDebugSettings();
        const auto available = ImGui::GetContentRegionAvail();
        const float side = max(1.0f, min(available.x, available.y));
        ImGui::InvisibleButton("##AnimationOrbit", ImVec2(side, side),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureData*>(preview.Descriptor->Gpu.ptr),
                                            ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const auto& input = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            preview.Context->Orbit(input.MouseDelta.x * 0.01f, input.MouseDelta.y * 0.01f, 0.0f);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            preview.Context->Pan(input.MouseDelta.x / side, input.MouseDelta.y / side);
        }
        if (ImGui::IsItemHovered())
        {
            if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Middle))
            {
                const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                if (drag.x == 0.0f && drag.y == 0.0f)
                {
                    const ImVec2 corner = ImGui::GetItemRectMin();
                    preview.Context->Pan(0.5f - (input.MousePos.x - corner.x) / side,
                                         0.5f - (input.MousePos.y - corner.y) / side);
                }
            }
            preview.Context->Orbit(0.0f, 0.0f, input.MouseWheel);
        }
        if (preview.ShowSkeleton)
        {
            DrawSkeletonOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
    }
}

void AnimationPreviewWindow::OpenForSelection()
{
    EditorWorkspaceManager::Get().Registry().ShowEditorType("animation");
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

void AnimationPreviewWindow::BeginFrame()
{
    preview.Visible = false;
}

void AnimationPreviewWindow::Draw(bool* open)
{
    if (!open || !*open)
    {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(1150, 780), ImGuiCond_FirstUseEver);
    if (!EditorWindowRegistry::BeginEditorWindow("アニメーションプレビュー", open))
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
        DrawExpressionPreview();
        DrawSpringPreview();
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
        if (preview.LastPlaybackFrame != ImGui::GetFrameCount())
        {
            preview.LastPlaybackFrame = ImGui::GetFrameCount();
            const float deltaTime = ImGui::GetIO().DeltaTime;
            const float duration = AnimationPreview::Duration(preview.Clips);
            if (preview.Playback.Playing && preview.Playback.Loop && duration > 0.0f &&
                preview.Playback.Time + deltaTime * preview.Playback.Speed >= duration)
            {
                preview.ResetPhysics = true;
            }
            preview.Playback.Advance(deltaTime, duration);
            UpdatePose();
            UpdateExpressionPreview();
        }
        DrawPreviewImage();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void AnimationPreviewWindow::Render()
{
    if (preview.Visible && preview.Model && preview.Context &&
        !preview.Context->Render(*preview.Model, preview.Materials, preview.ShowWireframe,
                                 preview.ShowWeights && !preview.Model->GetBoneNames().empty() ? preview.WeightBone : -1))
    {
        preview.Error = "プレビューを描画できません: " + preview.Context->Error();
    }
}

void AnimationPreviewWindow::Shutdown()
{
    ClearPreviewPhysics();
    preview = PreviewState{};
}
