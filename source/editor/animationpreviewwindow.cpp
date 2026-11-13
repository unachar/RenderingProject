#include "pch.h"
#include "editorworkspace.h"
#include "animationpreviewwindow.h"
#include "animationpreviewstate.h"
#include "animationpreviewcontext.h"
#include "materialpartresolver.h"
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

    struct SpringCollider
    {
        string Bone;
        XMFLOAT3 Offset{};
        XMFLOAT3 Tail{};
        float Radius = 0.05f;
        bool Manual = false;
        bool Generated = false;
    };

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
        float HitRadius = 0.0f;
        vector<size_t> Colliders;
    };

    struct PreviewSpringRig
    {
        vector<PreviewSpring> Springs;
        vector<SpringCollider> Colliders;
    };

    struct BulletSpringLink
    {
        size_t SpringIndex;
        PhysicsBodyHandle Anchor;
        PhysicsBodyHandle Body;
        XMFLOAT3 SolvedDirection{};
        bool AnimatedAnchor = true;
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
        unique_ptr<IPhysicsBackend> BulletSprings;
        vector<BulletSpringLink> BulletLinks;
        bool UseBulletSprings = false;
        float SpringMass = 0.08f;
        float SpringDamping = 0.55f;
        float SpringStiffness = 1.5f;
        float SpringAngleLimit = 32.0f;
        float SpringGravityScale = 0.35f;
        bool PhysicsEnabled = true;
        bool ResetPhysics = true;
        double PhysicsTime = 0.0;
        string PhysicsError;
        vector<PreviewSpring> Springs;
        vector<SpringCollider> Colliders;
        int ColliderBone = 0;
        int SpringBone = 0;
        int SpringTip = 0;
        int ChainStart = -1;
        int ChainEnd = -1;
        bool OrbitDragged = false;
        vector<AnimationModelResource::DebugBone> Skeleton;
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
        bool ShowColliders = false;
        bool ShowWeights = false;
        XMFLOAT3 WireframeColor = {0.25f, 0.85f, 1.0f};
        XMFLOAT3 BoneColor = {109.0f / 255.0f, 235.0f / 255.0f, 1.0f};
        XMFLOAT3 ColliderColor = {190.0f / 255.0f, 1.0f, 65.0f / 255.0f};
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

    PreviewSpringRig LoadVrmSprings(const string& path, bool convert)
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
        const auto& extensionData = document["extensions"]["VRMC_springBone"];
        const auto& groups = extensionData["colliderGroups"];
        const auto& springs = extensionData["springs"];
        const auto nodeName = [&nodes](const Json::Value& joint) -> string {
            const auto& index = joint["node"];
            if (!index.isUInt() || index.asUInt() >= nodes.size())
            {
                return {};
            }
            const string name = nodes[index.asUInt()]["name"].asString();
            return name.empty() ? "nodes[" + to_string(index.asUInt()) + "]" : name;
        };
        const auto readPosition = [convert](const Json::Value& position) -> XMFLOAT3 {
            if (!position.isArray() || position.size() != 3 || !position[0].isNumeric() ||
                !position[1].isNumeric() || !position[2].isNumeric())
            {
                return {};
            }
            return {position[0].asFloat(), position[1].asFloat(),
                    position[2].asFloat() * (convert ? -1.0f : 1.0f)};
        };
        PreviewSpringRig result;
        for (const auto& entry : extensionData["colliders"])
        {
            SpringCollider collider;
            collider.Bone = nodeName(entry);
            const auto& capsule = entry["shape"]["capsule"];
            const auto& sphere = entry["shape"]["sphere"];
            const auto& shape = capsule.isObject() ? capsule : sphere;
            collider.Offset = readPosition(shape["offset"]);
            collider.Tail = capsule.isObject() ? readPosition(shape["tail"]) : collider.Offset;
            collider.Radius = shape["radius"].isNumeric() ? max(0.0f, shape["radius"].asFloat()) : 0.0f;
            result.Colliders.push_back(move(collider)); // Preserve VRM indices even for unsupported shapes.
        }
        for (const auto& chain : springs)
        {
            vector<size_t> colliders;
            for (const auto& group : chain["colliderGroups"])
            {
                if (!group.isUInt() || group.asUInt() >= groups.size())
                {
                    continue;
                }
                for (const auto& collider : groups[group.asUInt()]["colliders"])
                {
                    if (collider.isUInt() && collider.asUInt() < result.Colliders.size())
                    {
                        colliders.push_back(collider.asUInt());
                    }
                }
            }
            sort(colliders.begin(), colliders.end());
            colliders.erase(unique(colliders.begin(), colliders.end()), colliders.end());
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
                spring.Colliders = colliders;
                spring.HitRadius = max(0.0f, joint.get("hitRadius", 0.0f).asFloat());
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
                if (none_of(result.Springs.begin(), result.Springs.end(), [&](const PreviewSpring& existing) {
                        return existing.Bone == spring.Bone;
                    }))
                {
                    result.Springs.push_back(move(spring));
                }
            }
        }
        return result;
    }

    bool LoadAliciaSpringPreset(const string& path, AnimationModelResource& model)
    {
        string name = filesystem::u8path(path).filename().string();
        transform(name.begin(), name.end(), name.begin(), [](unsigned char letter) {
            return static_cast<char>(tolower(letter));
        });
        if (name != "alicia.fbx" && name != "alicia_solid_mmd.fbx" && name != "alicia_solid_unity.fbx")
        {
            return false;
        }

        const auto skeleton = model.GetDebugSkeleton();
        const auto isSecondary = [](const string& bone) {
            return bone.starts_with("hair") || bone.starts_with("skirt_") ||
                   bone.starts_with("ribbon_") || bone == "flonthair";
        };
        unordered_set<string> registered;
        for (const auto& tip : skeleton)
        {
            if (tip.Parent < 0)
            {
                continue;
            }
            const auto& bone = skeleton[tip.Parent];
            const auto offset = XMLoadFloat3(&tip.Position) - XMLoadFloat3(&bone.Position);
            if (isSecondary(bone.Name) && isSecondary(tip.Name) &&
                XMVectorGetX(XMVector3LengthSq(offset)) > 1e-10f && registered.insert(bone.Name).second)
            {
                preview.Springs.push_back({bone.Name, tip.Name});
            }
        }
        return !preview.Springs.empty();
    }

    void BuildBulletSpringRig()
    {
        preview.BulletSprings.reset();
        preview.BulletLinks.clear();
        auto backend = CreateBulletPhysicsBackend();
        if (!backend->Init())
        {
            preview.PhysicsError = "揺れもの用のBulletを初期化できません。";
            return;
        }
        preview.PhysicsError.clear();
        const auto bounds = preview.Model->GetAabbExtents();
        const float modelScale = max(bounds.y, 0.01f);
        backend->SetGravity({0.0f, -9.81f * preview.SpringGravityScale * modelScale, 0.0f});
        // The model's skeleton order is parent-first, including manually assigned links.
        unordered_map<string, size_t> order;
        const auto skeleton = preview.Model->GetDebugSkeleton();
        for (size_t index = 0; index < skeleton.size(); ++index)
        {
            order.try_emplace(skeleton[index].Name, index);
        }
        const auto rank = [&](const string& name) {
            const auto found = order.find(name);
            return found == order.end() ? skeleton.size() : found->second;
        };
        stable_sort(preview.Springs.begin(), preview.Springs.end(), [&](const auto& left, const auto& right) {
            return rank(left.Bone) < rank(right.Bone);
        });
        unordered_map<string, PhysicsBodyHandle> chainTips;
        for (size_t index = 0; index < preview.Springs.size(); ++index)
        {
            const auto& spring = preview.Springs[index];
            XMFLOAT4X4 root{}, tip{};
            if (!preview.Model->GetBoneGlobalTransform(spring.Bone, root) ||
                !preview.Model->GetBoneGlobalTransform(spring.Tip, tip))
            {
                continue;
            }
            XMFLOAT3 offset{tip._41 - root._41, tip._42 - root._42, tip._43 - root._43};
            const float length = XMVectorGetX(XMVector3Length(XMLoadFloat3(&offset)));
            if (!(length > 0.0001f))
            {
                continue;
            }

            PhysicsBodyDesc anchor;
            anchor.BodyType = PhysicsBodyType::Kinematic;
            anchor.Shape = PhysicsShape::Sphere;
            anchor.Radius = max(length * 0.15f, 0.001f);
            anchor.Transform.Position = {root._41, root._42, root._43};
            XMVECTOR scale, rotation, translation;
            if (!XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&root)))
            {
                continue;
            }
            XMStoreFloat4(&anchor.Transform.Rotation, rotation);
            anchor.CollisionMask = 0;
            const auto parent = chainTips.find(spring.Bone);
            const bool animatedAnchor = parent == chainTips.end();
            const auto anchorId = animatedAnchor ? backend->CreateBody(anchor) : parent->second;

            PhysicsBodyDesc body = anchor;
            body.BodyType = PhysicsBodyType::Dynamic;
            body.Transform.Position = {tip._41, tip._42, tip._43};
            body.Mass = preview.SpringMass;
            body.LinearDamping = preview.SpringDamping;
            body.AngularDamping = preview.SpringDamping;
            body.AllowSleeping = false;
            const auto bodyId = backend->CreateBody(body);

            PhysicsJointDesc joint;
            joint.BodyA = anchorId;
            joint.BodyB = bodyId;
            joint.Frame = anchor.Transform;
            const float limit = XMConvertToRadians(preview.SpringAngleLimit);
            joint.AngularLimitMin = {-limit, -limit, -limit};
            joint.AngularLimitMax = {limit, limit, limit};
            const float stiffness = preview.SpringStiffness * length * length;
            joint.AngularSpring = {stiffness, stiffness, stiffness};
            if (backend->CreateJoint(joint) == kInvalidPhysicsJoint)
            {
                backend->DestroyBody(bodyId);
                if (animatedAnchor)
                {
                    backend->DestroyBody(anchorId);
                }
                continue;
            }
            BulletSpringLink link{index, anchorId, bodyId};
            link.AnimatedAnchor = animatedAnchor;
            XMStoreFloat3(&link.SolvedDirection, XMLoadFloat3(&offset) / length);
            preview.BulletLinks.push_back(link);
            chainTips.try_emplace(spring.Tip, bodyId);
        }
        if (preview.BulletLinks.empty())
        {
            preview.PhysicsError = "Bulletジョイントを作成できる揺れボーンがありません。";
            return;
        }
        preview.BulletSprings = move(backend);
    }

    void StepBulletSprings(float step)
    {
        if (!preview.BulletSprings)
        {
            return;
        }
        auto& backend = *preview.BulletSprings;
        for (const auto& link : preview.BulletLinks)
        {
            if (!link.AnimatedAnchor)
            {
                continue;
            }
            const auto& spring = preview.Springs[link.SpringIndex];
            XMFLOAT4X4 root{};
            if (!preview.Model->GetBoneGlobalTransform(spring.Bone, root))
            {
                continue;
            }
            XMVECTOR scale, rotation, translation;
            if (XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&root)))
            {
                PhysicsTransform anchor;
                XMStoreFloat3(&anchor.Position, translation);
                XMStoreFloat4(&anchor.Rotation, rotation);
                backend.SetBodyTransform(link.Anchor, anchor, false);
            }
        }
        backend.Step(step);
        for (auto& link : preview.BulletLinks)
        {
            PhysicsTransform anchor, body;
            if (!backend.GetBodyTransform(link.Anchor, anchor) || !backend.GetBodyTransform(link.Body, body))
            {
                continue;
            }
            const auto direction = XMLoadFloat3(&body.Position) - XMLoadFloat3(&anchor.Position);
            const float lengthSq = XMVectorGetX(XMVector3LengthSq(direction));
            if (std::isfinite(lengthSq) && lengthSq > 1e-10f)
            {
                XMStoreFloat3(&link.SolvedDirection, direction / sqrtf(lengthSq));
            }
        }
    }

    struct ModelSpaceCollider
    {
        XMFLOAT3 Start{};
        XMFLOAT3 End{};
        float Radius = 0.0f;
    };

    vector<ModelSpaceCollider> SampleSpringColliders()
    {
        vector<ModelSpaceCollider> result(preview.Colliders.size());
        unordered_map<string, XMFLOAT4X4> poses;
        for (size_t index = 0; index < preview.Colliders.size(); ++index)
        {
            const auto& collider = preview.Colliders[index];
            if (collider.Bone.empty() || !(collider.Radius > 0.0f))
            {
                continue;
            }
            auto [pose, inserted] = poses.try_emplace(collider.Bone);
            if (inserted && !preview.Model->GetBoneGlobalTransform(collider.Bone, pose->second))
            {
                poses.erase(pose);
                continue;
            }
            const auto transform = XMLoadFloat4x4(&pose->second);
            auto& target = result[index];
            XMStoreFloat3(&target.Start, XMVector3TransformCoord(XMLoadFloat3(&collider.Offset), transform));
            XMStoreFloat3(&target.End, XMVector3TransformCoord(XMLoadFloat3(&collider.Tail), transform));
            const float scale = max({XMVectorGetX(XMVector3Length(transform.r[0])),
                                     XMVectorGetX(XMVector3Length(transform.r[1])),
                                     XMVectorGetX(XMVector3Length(transform.r[2]))});
            target.Radius = collider.Radius * scale;
        }
        return result;
    }

    XMVECTOR ConstrainSpringDirection(FXMVECTOR origin, FXMVECTOR direction, FXMVECTOR restDirection, float length,
                                      const PreviewSpring& spring, const vector<ModelSpaceCollider>& colliders)
    {
        XMVECTOR result = direction;
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            bool corrected = false;
            const auto resolve = [&](size_t index) {
                const auto& collider = colliders[index];
                if (!(collider.Radius > 0.0f))
                {
                    return;
                }
                const XMVECTOR segment = result * length;
                const XMVECTOR start = XMLoadFloat3(&collider.Start);
                const XMVECTOR axis = XMLoadFloat3(&collider.End) - start;
                const XMVECTOR offset = origin - start;
                const float segmentLengthSq = length * length;
                const float axisLengthSq = XMVectorGetX(XMVector3LengthSq(axis));
                const float alignment = XMVectorGetX(XMVector3Dot(segment, axis));
                const float segmentOffset = XMVectorGetX(XMVector3Dot(segment, offset));
                const float axisOffset = XMVectorGetX(XMVector3Dot(axis, offset));
                float contact = clamp(-segmentOffset / segmentLengthSq, 0.0f, 1.0f);
                float along = 0.0f;
                if (axisLengthSq > 1e-10f)
                {
                    const float denominator = segmentLengthSq * axisLengthSq - alignment * alignment;
                    if (denominator > segmentLengthSq * axisLengthSq * 1e-6f)
                    {
                        contact = clamp((alignment * axisOffset - segmentOffset * axisLengthSq) / denominator,
                                        0.0f, 1.0f);
                    }
                    along = clamp((alignment * contact + axisOffset) / axisLengthSq, 0.0f, 1.0f);
                    contact = clamp((alignment * along - segmentOffset) / segmentLengthSq, 0.0f, 1.0f);
                }
                // An anchored root cannot be translated out of a body collider.
                // Keep tip collision when the closest point is at the root.
                if (contact < 0.05f)
                {
                    contact = 1.0f;
                    if (axisLengthSq > 1e-10f)
                    {
                        along = clamp((alignment + axisOffset) / axisLengthSq, 0.0f, 1.0f);
                    }
                }
                const XMVECTOR closest = start + axis * along;
                const XMVECTOR separation = origin + segment * contact - closest;
                const float distance = XMVectorGetX(XMVector3Length(separation));
                const float radius = collider.Radius + spring.HitRadius;
                if (distance >= radius - 1e-6f)
                {
                    return;
                }
                XMVECTOR normal = separation;
                if (distance < 1e-6f)
                {
                    normal = origin - closest;
                    if (axisLengthSq > 1e-10f)
                    {
                        normal -= axis * (XMVectorGetX(XMVector3Dot(normal, axis)) / axisLengthSq);
                    }
                    if (XMVectorGetX(XMVector3LengthSq(normal)) < 1e-10f)
                    {
                        normal = XMVector3Orthogonal(axisLengthSq > 1e-10f ? axis : restDirection);
                    }
                }
                const XMVECTOR pushed = closest + XMVector3Normalize(normal) * radius - origin;
                if (XMVectorGetX(XMVector3LengthSq(pushed)) > 1e-10f)
                {
                    result = XMVector3Normalize(pushed);
                    corrected = true;
                }
            };
            for (size_t index : spring.Colliders)
            {
                resolve(index);
            }
            for (size_t index = 0; index < preview.Colliders.size(); ++index)
            {
                if (preview.Colliders[index].Manual)
                {
                    resolve(index);
                }
            }
            if (!corrected)
            {
                break;
            }
        }
        return result;
    }

    void ApplySpringPose(float step)
    {
        const auto colliders = SampleSpringColliders();
        bool changed = false;
        size_t bulletIndex = 0;
        for (size_t index = 0; index < preview.Springs.size(); ++index)
        {
            auto& spring = preview.Springs[index];
            const BulletSpringLink* link = nullptr;
            if (preview.UseBulletSprings)
            {
                if (bulletIndex >= preview.BulletLinks.size() || preview.BulletLinks[bulletIndex].SpringIndex != index)
                {
                    continue;
                }
                link = &preview.BulletLinks[bulletIndex++];
            }
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
            const bool initialize = !spring.Initialized;
            if (initialize)
            {
                XMStoreFloat3(&spring.CurrentTip, animatedTip);
                spring.PreviousTip = spring.CurrentTip;
                spring.Initialized = true;
            }
            XMVECTOR direction = XMLoadFloat3(&spring.CurrentTip) - origin;
            if (step > 0.0f)
            {
                direction += (XMLoadFloat3(&spring.CurrentTip) - XMLoadFloat3(&spring.PreviousTip)) *
                             (1.0f - clamp(spring.Drag, 0.0f, 1.0f));
                direction += (restDirection * spring.Stiffness +
                              XMLoadFloat3(&spring.GravityDirection) * spring.Gravity) * step;
            }
            if (link)
            {
                // The joint chain has already propagated its parent's motion in model space.
                direction = XMLoadFloat3(&link->SolvedDirection);
            }
            const float directionLength = XMVectorGetX(XMVector3Length(direction));
            direction = directionLength > 0.00001f ? direction / directionLength : restDirection;
            direction = ConstrainSpringDirection(origin, direction, restDirection, length, spring, colliders);
            if (step > 0.0f || initialize)
            {
                spring.PreviousTip = spring.CurrentTip;
                XMStoreFloat3(&spring.CurrentTip, origin + direction * length);
                if (initialize)
                {
                    spring.PreviousTip = spring.CurrentTip;
                }
            }

            // Limit the rendered swing too: the solver constrains body rotation, while
            // this pose is driven by the solved tip position and collision projection.
            const float limit = link ? XMConvertToRadians(preview.SpringAngleLimit) : XM_PI;
            changed |= preview.Model->AimSpringBone(spring.Bone, restDirection, direction, limit);
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
            preview.BulletSprings.reset();
            preview.BulletLinks.clear();
            preview.PhysicsError.clear();
            for (auto& spring : preview.Springs)
            {
                spring.Initialized = false;
            }
        }
        if (preview.UseBulletSprings && !preview.BulletSprings && preview.PhysicsError.empty())
        {
            SamplePose(time);
            BuildBulletSpringRig();
        }
        while (time - preview.PhysicsTime >= step)
        {
            preview.PhysicsTime += step;
            SamplePose(static_cast<float>(preview.PhysicsTime));
            if (preview.UseBulletSprings)
            {
                StepBulletSprings(static_cast<float>(step));
                ApplySpringPose(0.0f);
            }
            else
            {
                ApplySpringPose(static_cast<float>(step));
            }
        }
        SamplePose(time);
        if (!preview.UseBulletSprings || preview.BulletSprings)
        {
            ApplySpringPose(0.0f);
        }
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
        preview.ChainStart = -1;
        preview.ChainEnd = -1;
        preview.Skeleton.clear();
        if (preview.ModelPath != path || preview.Convert != convert)
        {
            preview.Springs.clear();
            preview.Colliders.clear();
            preview.UseBulletSprings = false;
            try
            {
                auto rig = LoadVrmSprings(path, convert);
                preview.Springs = move(rig.Springs);
                preview.Colliders = move(rig.Colliders);
                if (preview.Springs.empty())
                {
                    preview.UseBulletSprings = LoadAliciaSpringPreset(path, *preview.Model);
                }
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

    void GenerateBodyColliders()
    {
        const auto& names = preview.Model->GetBoneNames();
        vector<bool> bodyBones(names.size());
        const auto& map = preview.Model->GetResourceMap();
        for (const char* part : {"head", "neck", "chest", "upperChest", "spine", "hips",
                                 "leftUpperArm", "rightUpperArm", "leftLowerArm", "rightLowerArm",
                                 "leftUpperLeg", "rightUpperLeg", "leftLowerLeg", "rightLowerLeg"})
        {
            if (const auto* indices = map.Find(ModelResourceMap::Humanoid, part); indices && indices->size() == 1 &&
                indices->front() < names.size())
            {
                bodyBones[indices->front()] = true;
            }
        }
        // Legacy FBX rigs may lack humanoid aliases.
        for (size_t index = 0; index < names.size(); ++index)
        {
            const auto& name = names[index];
            bodyBones[index] = bodyBones[index] || name == "head" || name == "neck" || name == "waist" ||
                name == "upperbody" || name == "upperbody01" || name == "arm_L" || name == "arm_R" ||
                name == "elbow_L" || name == "elbow_R" || name == "leg_L" || name == "leg_R";
        }

        struct Influence
        {
            size_t Bone = 0;
            aiMatrix4x4 Offset;
            bool Valid = false;
        };
        unordered_map<int, Influence> influences;
        vector<vector<aiVector3D>> points(names.size());
        for (UINT mesh = 0; mesh < preview.Model->GetMeshCount(); ++mesh)
        {
            const auto flags = preview.Model->GetMeshData(mesh).SemanticFlags;
            if ((flags & (MeshSemantic_Hair | MeshSemantic_Accessory | MeshSemantic_Eye | MeshSemantic_EyeHighlight)) != 0)
            {
                continue;
            }
            for (const auto& vertex : preview.Model->GetSkinInputVertices(mesh))
            {
                const int slot = static_cast<int>(max_element(vertex.BoneWeights, vertex.BoneWeights + 4) - vertex.BoneWeights);
                if (vertex.BoneWeights[slot] < 0.5f)
                {
                    continue;
                }
                const int index = vertex.BoneIndices[slot];
                auto [entry, inserted] = influences.try_emplace(index);
                auto& influence = entry->second;
                if (inserted)
                {
                    influence.Valid = preview.Model->GetSkinInfluenceOffset(index, influence.Bone, influence.Offset) &&
                                      bodyBones[influence.Bone];
                }
                if (influence.Valid)
                {
                    const auto point = influence.Offset * aiVector3D(vertex.Position.x, vertex.Position.y, vertex.Position.z);
                    if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z))
                    {
                        points[influence.Bone].push_back(point);
                    }
                }
            }
        }

        vector<SpringCollider> generated;
        for (size_t bone = 0; bone < names.size(); ++bone)
        {
            auto& vertices = points[bone];
            if (vertices.size() < 8)
            {
                continue;
            }
            float low[3]{}, high[3]{}, middle[3]{};
            for (int axis = 0; axis < 3; ++axis)
            {
                vector<float> coordinates;
                coordinates.reserve(vertices.size());
                for (const auto& point : vertices)
                {
                    coordinates.push_back(axis == 0 ? point.x : axis == 1 ? point.y : point.z);
                }
                sort(coordinates.begin(), coordinates.end());
                const size_t trim = coordinates.size() / 20;
                low[axis] = coordinates[trim];
                high[axis] = coordinates[coordinates.size() - 1 - trim];
                middle[axis] = (low[axis] + high[axis]) * 0.5f;
            }
            const float extents[] = {high[0] - low[0], high[1] - low[1], high[2] - low[2]};
            const int axis = static_cast<int>(max_element(extents, extents + 3) - extents);
            const float radius = max(extents[(axis + 1) % 3], extents[(axis + 2) % 3]) * 0.5f;
            if (radius <= 1e-5f)
            {
                continue;
            }
            SpringCollider collider;
            collider.Bone = names[bone];
            collider.Radius = radius;
            collider.Manual = true;
            collider.Generated = true;
            const float halfSegment = max(0.0f, extents[axis] * 0.5f - radius);
            middle[axis] -= halfSegment;
            collider.Offset = {middle[0], middle[1], middle[2]};
            middle[axis] += halfSegment * 2.0f;
            collider.Tail = {middle[0], middle[1], middle[2]};
            generated.push_back(move(collider));
        }
        if (generated.empty())
        {
            preview.Status = "体の衝突形状を生成できません。体ボーンのスキンウェイトを確認してください。";
            return;
        }
        erase_if(preview.Colliders, [](const SpringCollider& collider) { return collider.Generated; });
        preview.Colliders.insert(preview.Colliders.end(), generated.begin(), generated.end());
        preview.ShowColliders = true;
        preview.ResetPhysics = true;
        preview.Status = "体の衝突形状を " + to_string(generated.size()) + " 個生成しました。";
    }

    void DrawSpringColliders(const vector<string>& names)
    {
        if (!ImGui::TreeNode("揺れものの衝突形状"))
        {
            return;
        }
        if (ImGui::Button("体から衝突形状を生成"))
        {
            GenerateBodyColliders();
        }
        preview.ColliderBone = clamp(preview.ColliderBone, 0, static_cast<int>(names.size()) - 1);
        if (ImGui::BeginCombo("追従ボーン", names[preview.ColliderBone].c_str()))
        {
            for (int index = 0; index < static_cast<int>(names.size()); ++index)
            {
                ImGui::PushID(index);
                if (ImGui::Selectable(names[index].c_str(), index == preview.ColliderBone))
                {
                    preview.ColliderBone = index;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("カプセルを追加"))
        {
            SpringCollider collider;
            collider.Bone = names[preview.ColliderBone];
            collider.Tail.y = -0.15f;
            collider.Manual = true;
            preview.Colliders.push_back(move(collider));
            preview.ResetPhysics = true;
        }
        ImGui::TextWrapped("手動追加したカプセルは、すべての揺れボーンに適用します。");
        for (size_t index = 0; index < preview.Colliders.size(); ++index)
        {
            auto& collider = preview.Colliders[index];
            ImGui::PushID(static_cast<int>(index));
            const string label = collider.Bone + (collider.Generated ? "（自動）" : collider.Manual ? "（手動）" : "（VRM）");
            if (ImGui::TreeNode("##SpringCollider", "%s", label.c_str()))
            {
                bool changed = ImGui::DragFloat("半径", &collider.Radius, 0.005f, 0.0f, 100.0f, "%.3f");
                changed |= ImGui::DragFloat3("始点（ボーンローカル）", &collider.Offset.x, 0.005f);
                changed |= ImGui::DragFloat3("終点（ボーンローカル）", &collider.Tail.x, 0.005f);
                preview.ResetPhysics |= changed;
                if (collider.Manual && ImGui::Button("削除"))
                {
                    // Manual entries are appended after the VRM indices.
                    preview.Colliders.erase(preview.Colliders.begin() + index);
                    preview.ResetPhysics = true;
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
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
        preview.ResetPhysics |= ImGui::Checkbox("Bulletで揺れを計算", &preview.UseBulletSprings);
        if (preview.UseBulletSprings)
        {
            preview.ResetPhysics |= ImGui::DragFloat("剛体の質量", &preview.SpringMass, 0.005f, 0.01f, 5.0f);
            preview.ResetPhysics |= ImGui::SliderFloat("剛体の減衰", &preview.SpringDamping, 0.0f, 0.99f);
            preview.ResetPhysics |= ImGui::DragFloat("ジョイントのばね", &preview.SpringStiffness, 0.05f, 0.0f, 30.0f);
            preview.ResetPhysics |= ImGui::SliderFloat("ジョイントの角度制限", &preview.SpringAngleLimit, 1.0f, 85.0f, "%.0f°");
            preview.ResetPhysics |= ImGui::SliderFloat("重力倍率", &preview.SpringGravityScale, 0.0f, 2.0f);
            ImGui::Text("Bulletジョイント  %zu", preview.BulletLinks.size());
        }
        DrawSpringColliders(names);
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
            ImGui::BeginDisabled(preview.UseBulletSprings);
            ImGui::DragFloat("戻る強さ", &spring.Stiffness, 0.05f, 0.0f, 100.0f);
            ImGui::SliderFloat("減衰", &spring.Drag, 0.0f, 1.0f);
            ImGui::DragFloat("重力の強さ", &spring.Gravity, 0.01f, 0.0f, 100.0f);
            ImGui::EndDisabled();
            preview.ResetPhysics |= ImGui::DragFloat("先端の衝突半径", &spring.HitRadius, 0.005f, 0.0f, 100.0f, "%.3f");
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
        ImGui::Checkbox("衝突形状を表示", &preview.ShowColliders);
        ImGui::ColorEdit3("ワイヤーフレームの色", &preview.WireframeColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit3("ボーンの色", &preview.BoneColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit3("衝突形状の色", &preview.ColliderColor.x, ImGuiColorEditFlags_NoInputs);
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

    void SelectChainBone(int index)
    {
        if (preview.ChainStart < 0 || preview.ChainEnd >= 0)
        {
            preview.ChainStart = index;
            preview.ChainEnd = -1;
        }
        else if (index != preview.ChainStart)
        {
            preview.ChainEnd = index;
        }
    }

    vector<int> SelectedChain()
    {
        if (preview.ChainStart < 0 || preview.ChainEnd < 0)
        {
            return {};
        }
        const auto fromAncestor = [](int ancestor, int tip) -> vector<int> {
            vector<int> chain;
            for (int index = tip; index >= 0; index = preview.Skeleton[index].Parent)
            {
                chain.push_back(index);
                if (index == ancestor)
                {
                    reverse(chain.begin(), chain.end());
                    return chain;
                }
            }
            return {};
        };
        auto chain = fromAncestor(preview.ChainStart, preview.ChainEnd);
        return chain.empty() ? fromAncestor(preview.ChainEnd, preview.ChainStart) : chain;
    }

    void DrawBoneHierarchy()
    {
        ImGui::TextUnformatted("ボーン階層");
        if (preview.Skeleton.empty())
        {
            ImGui::TextDisabled("ボーンがありません。");
            return;
        }

        const auto chain = SelectedChain();
        ImGui::TextWrapped("階層またはボーンフレームで始点と終点を選択します。");
        if (preview.ChainStart >= 0)
        {
            ImGui::TextWrapped("始点  %s", preview.Skeleton[preview.ChainStart].Name.c_str());
        }
        if (preview.ChainEnd >= 0)
        {
            ImGui::TextWrapped("終点  %s", preview.Skeleton[preview.ChainEnd].Name.c_str());
        }

        vector<PreviewSpring> additions;
        bool conflict = false;
        for (size_t index = 1; index < chain.size(); ++index)
        {
            const auto& bone = preview.Skeleton[chain[index - 1]].Name;
            const auto& tip = preview.Skeleton[chain[index]].Name;
            const auto existing = find_if(preview.Springs.begin(), preview.Springs.end(),
                                          [&](const PreviewSpring& spring) { return spring.Bone == bone; });
            if (existing == preview.Springs.end())
            {
                additions.push_back({bone, tip});
            }
            else if (existing->Tip != tip)
            {
                conflict = true;
            }
        }
        const bool pmx = preview.Model->HasPmxPhysics();
        ImGui::BeginDisabled(additions.empty() || conflict || pmx);
        if (ImGui::Button("選択範囲を揺れボーンに追加"))
        {
            preview.Springs.insert(preview.Springs.end(), additions.begin(), additions.end());
            unordered_map<string, size_t> order;
            for (size_t index = 0; index < preview.Skeleton.size(); ++index)
            {
                order.emplace(preview.Skeleton[index].Name, index);
            }
            const auto rank = [&](const string& name) {
                const auto found = order.find(name);
                return found == order.end() ? preview.Skeleton.size() : found->second;
            };
            stable_sort(preview.Springs.begin(), preview.Springs.end(), [&](const auto& left, const auto& right) {
                return rank(left.Bone) < rank(right.Bone);
            });
            preview.PhysicsEnabled = true;
            preview.ResetPhysics = true;
        }
        ImGui::EndDisabled();
        if (pmx)
        {
            ImGui::TextWrapped("このPMXはモデル内の剛体設定を使用します。");
        }
        else if (preview.ChainEnd >= 0 && chain.empty())
        {
            ImGui::TextWrapped("同じ親子チェーン上の2点を選んでください。");
        }
        else if (conflict)
        {
            ImGui::TextWrapped("別の先端が設定されたボーンがあります。揺れボーン設定で確認してください。");
        }
        else if (!chain.empty() && additions.empty())
        {
            ImGui::TextDisabled("選択範囲は追加済みです。");
        }
        ImGui::BeginDisabled(preview.ChainStart < 0);
        if (ImGui::Button("選択解除"))
        {
            preview.ChainStart = -1;
            preview.ChainEnd = -1;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        vector<vector<int>> children(preview.Skeleton.size() + 1);
        for (int index = 0; index < static_cast<int>(preview.Skeleton.size()); ++index)
        {
            children[preview.Skeleton[index].Parent + 1].push_back(index);
        }
        const auto drawNode = [&](const auto& self, int index) -> void {
            const auto& bone = preview.Skeleton[index];
            const auto& branches = children[index + 1];
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen;
            if (branches.empty())
            {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (index == preview.ChainStart || index == preview.ChainEnd ||
                find(chain.begin(), chain.end(), index) != chain.end())
            {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            ImGui::PushID(index);
            const bool open = ImGui::TreeNodeEx("##Bone", flags, "%s", bone.Name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            {
                SelectChainBone(index);
            }
            if (open)
            {
                for (int child : branches)
                {
                    self(self, child);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        for (int root : children[0])
        {
            drawNode(drawNode, root);
        }
    }

    void DrawSkeletonOverlay(ImVec2 minimum, ImVec2 maximum, bool pickBone)
    {
        const auto& bones = preview.Skeleton;
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

        if (pickBone)
        {
            const auto mouse = ImGui::GetIO().MousePos;
            float nearest = 64.0f;
            int picked = -1;
            for (int index = 0; index < static_cast<int>(bones.size()); ++index)
            {
                if (!visible[index])
                {
                    continue;
                }
                const float dx = points[index].x - mouse.x;
                const float dy = points[index].y - mouse.y;
                const float distance = dx * dx + dy * dy;
                if (distance < nearest)
                {
                    nearest = distance;
                    picked = index;
                }
            }
            if (picked >= 0)
            {
                SelectChainBone(picked);
            }
        }
        const auto chain = SelectedChain();
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(minimum, maximum, true);
        const auto& names = preview.Model->GetBoneNames();
        const auto& boneColor = preview.BoneColor;
        const ImU32 baseColor = ImGui::ColorConvertFloat4ToU32(ImVec4(boneColor.x, boneColor.y, boneColor.z, 1.0f));
        for (size_t index = 0; index < bones.size(); ++index)
        {
            if (!visible[index])
            {
                continue;
            }
            const auto& bone = bones[index];
            const bool selected = preview.ShowWeights && static_cast<size_t>(preview.WeightBone) < names.size() &&
                                  bone.Name == names[preview.WeightBone];
            const int boneIndex = static_cast<int>(index);
            const bool endpoint = boneIndex == preview.ChainStart || boneIndex == preview.ChainEnd;
            const bool inChain = find(chain.begin(), chain.end(), boneIndex) != chain.end();
            const ImU32 color = endpoint || inChain ? IM_COL32(100, 255, 144, 255)
                                  : selected ? IM_COL32(255, 166, 64, 255) : baseColor;
            if (bone.Parent >= 0 && visible[bone.Parent])
            {
                draw->AddLine(points[bone.Parent], points[index], IM_COL32(15, 22, 34, 255), 3.0f);
                draw->AddLine(points[bone.Parent], points[index], color, 1.4f);
            }
            draw->AddCircleFilled(points[index], endpoint ? 5.0f : selected ? 4.0f : 2.5f, color);
        }
        draw->PopClipRect();
    }

    void DrawColliderOverlay(ImVec2 minimum, ImVec2 maximum)
    {
        const auto& colliderColor = preview.ColliderColor;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(colliderColor.x, colliderColor.y, colliderColor.z, 235.0f / 255.0f));
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(minimum, maximum, true);
        const auto project = [&](FXMVECTOR point, ImVec2& pixel) {
            XMFLOAT3 position;
            XMFLOAT2 uv;
            XMStoreFloat3(&position, point);
            if (!preview.Context->ProjectPoint(position, uv))
            {
                return false;
            }
            pixel = {minimum.x + uv.x * (maximum.x - minimum.x), minimum.y + uv.y * (maximum.y - minimum.y)};
            return true;
        };
        const auto line = [&](FXMVECTOR a, FXMVECTOR b) {
            ImVec2 start, end;
            if (project(a, start) && project(b, end))
            {
                draw->AddLine(start, end, color, 1.4f);
            }
        };
        for (const auto& collider : SampleSpringColliders())
        {
            if (!(collider.Radius > 0.0f))
            {
                continue;
            }
            const auto start = XMLoadFloat3(&collider.Start);
            const auto end = XMLoadFloat3(&collider.End);
            const auto axis = end - start;
            const auto direction = XMVectorGetX(XMVector3LengthSq(axis)) > 1e-10f
                                       ? XMVector3Normalize(axis) : XMVectorSet(0, 1, 0, 0);
            const auto right = XMVector3Normalize(XMVector3Orthogonal(direction));
            const auto forward = XMVector3Cross(direction, right);
            constexpr int segments = 20;
            for (int segment = 0; segment < segments; ++segment)
            {
                const float angle = XM_2PI * segment / segments;
                const float next = XM_2PI * (segment + 1) / segments;
                const auto radial = right * cosf(angle) + forward * sinf(angle);
                const auto offset = radial * collider.Radius;
                const auto nextOffset = (right * cosf(next) + forward * sinf(next)) * collider.Radius;
                line(start + offset, start + nextOffset);
                line(end + offset, end + nextOffset);
                if (segment % 5 == 0)
                {
                    line(start + offset, end + offset);
                    for (int step = 0; step < 5; ++step)
                    {
                        const float a = XM_PIDIV2 * step / 5;
                        const float b = XM_PIDIV2 * (step + 1) / 5;
                        line(start + (radial * cosf(a) - direction * sinf(a)) * collider.Radius,
                             start + (radial * cosf(b) - direction * sinf(b)) * collider.Radius);
                        line(end + (radial * cosf(a) + direction * sinf(a)) * collider.Radius,
                             end + (radial * cosf(b) + direction * sinf(b)) * collider.Radius);
                    }
                }
            }
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
        if (ImGui::IsItemActivated())
        {
            preview.OrbitDragged = false;
        }
        const bool pickBone = ImGui::IsItemHovered() && ImGui::IsItemDeactivated() &&
                              ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !preview.OrbitDragged;
        const auto& input = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            preview.OrbitDragged = true;
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
        if (preview.ShowColliders)
        {
            DrawColliderOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        if (preview.ShowSkeleton)
        {
            DrawSkeletonOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), pickBone);
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
    ImGui::SetNextWindowSize(ImVec2(1380, 780), ImGuiCond_FirstUseEver);
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
    if (ImGui::BeginTable("##AnimationLayout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("設定", ImGuiTableColumnFlags_WidthFixed, 380.0f);
        ImGui::TableSetupColumn("プレビュー", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ボーン階層", ImGuiTableColumnFlags_WidthFixed, 255.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##AnimationSettings", ImVec2(0, 0), ImGuiChildFlags_None);
        DrawModelSettings();
        if (preview.Model && ImGui::CollapsingHeader("モデル読み込み診断"))
        {
            ImGui::Text("メッシュ %u", preview.Model->GetMeshCount());
            ImGui::Text("ボーン %zu", preview.Model->GetBoneNames().size());
            ImGui::Text("表情モーフ %zu", preview.Expressions.size());
            ImGui::Text("揺れもの %zu / 衝突形状 %zu", preview.Springs.size(), preview.Colliders.size());
            if (ImGui::TreeNode("表情モーフ一覧"))
            {
                for (const auto& expression : preview.Expressions)
                {
                    ImGui::TextUnformatted(expression.c_str());
                }
                ImGui::TreePop();
            }
        }
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
        if (preview.Model)
        {
            preview.Skeleton = preview.Model->GetDebugSkeleton();
        }
        DrawPreviewImage();
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("##AnimationSkeleton", ImVec2(0, 0), ImGuiChildFlags_None);
        DrawBoneHierarchy();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void AnimationPreviewWindow::Render()
{
    if (preview.Visible && preview.Model && preview.Context &&
        !preview.Context->Render(*preview.Model, preview.Materials, preview.ShowWireframe, preview.WireframeColor,
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
