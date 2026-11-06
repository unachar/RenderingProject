#include "pch.h"
#include "niagarascene.h"
#include "niagara.h"
#include "niagararenderer.h"
#include "world.h"
#include "projectmanager.h"
#include "proceduralio.h"
#include "imguimanager.h"
#include "editorvisibility.h"
#include "camera.h"
#include <cfloat>
#include <imgui.h>
#include <chrono>
#include <cmath>

struct NiagaraSceneInstance
{
    Niagara::System System;
    Niagara::Runtime Runtime;
    std::string AssetPath;
    std::string Error;
    std::filesystem::file_time_type WriteTime{};
    std::chrono::steady_clock::time_point NextFileCheck{};
    uint64_t PlaySession = 0;
    int LoopOverride = -1;
    bool Loaded = false;
    bool AssetLoop = true;
    bool ReloadRequested = false;
    bool Playing = true;
    bool WasPlaying = false;
    bool ShowBoundsGrid = true;
};

namespace
{
    struct EffectBounds
    {
        XMFLOAT3 Min{FLT_MAX, FLT_MAX, FLT_MAX};
        XMFLOAT3 Max{-FLT_MAX, -FLT_MAX, -FLT_MAX};

        void Include(XMVECTOR point, float radius = 0)
        {
            XMFLOAT3 value;
            XMStoreFloat3(&value, point);
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) || !std::isfinite(radius))
            {
                return;
            }
            Min = {(std::min)(Min.x, value.x - radius), (std::min)(Min.y, value.y - radius),
                   (std::min)(Min.z, value.z - radius)};
            Max = {(std::max)(Max.x, value.x + radius), (std::max)(Max.y, value.y + radius),
                   (std::max)(Max.z, value.z + radius)};
        }
    };

    XMVECTOR BoundsCorner(const EffectBounds& bounds, int corner)
    {
        return XMVectorSet(corner & 1 ? bounds.Max.x : bounds.Min.x, corner & 2 ? bounds.Max.y : bounds.Min.y,
                           corner & 4 ? bounds.Max.z : bounds.Min.z, 1);
    }

    // 標準モジュールの寿命・初速・加速度から保守的に推定し、実際の粒子で補う。
    // カスタムグラフによる任意の移動を将来まで保証する固定Boundsではない。
    EffectBounds CalculateBounds(const NiagaraSceneInstance& instance, FXMMATRIX world)
    {
        EffectBounds result;
        result.Include(XMVector3TransformCoord(XMVectorZero(), world), 0.1f);
        const float worldScale =
            (std::max)({XMVectorGetX(XMVector3Length(world.r[0])), XMVectorGetX(XMVector3Length(world.r[1])),
                        XMVectorGetX(XMVector3Length(world.r[2]))});
        const bool solo =
            std::any_of(instance.System.Emitters.begin(), instance.System.Emitters.end(), [](const auto& emitter) {
                return emitter.Enabled && emitter.Solo;
            });
        for (const auto& emitter : instance.System.Emitters)
        {
            if (!emitter.Enabled || (solo && !emitter.Solo))
            {
                continue;
            }
            float lifetime = 2.0f;
            float spawnRadius = 0.0f;
            float size = 0.1f;
            float sizeScale = 1.0f;
            std::array<float, 3> velocity{}, randomVelocity{}, acceleration{};
            for (const auto& module : emitter.Modules)
            {
                if (!module.Enabled)
                {
                    continue;
                }
                auto value = module.Value;
                if (!module.ParameterName.empty())
                {
                    for (const auto& parameter : instance.System.Parameters)
                    {
                        if (parameter.Name == module.ParameterName)
                        {
                            value = parameter.Value;
                            break;
                        }
                    }
                }
                if (module.Kind == Niagara::ModuleKind::Initialize)
                {
                    lifetime = module.Secondary[0];
                    size = module.Secondary[1];
                    spawnRadius = std::abs(module.Secondary[2]);
                }
                else if (module.Kind == Niagara::ModuleKind::Velocity)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        velocity[axis] = value[axis];
                        randomVelocity[axis] = std::abs(module.Secondary[axis]);
                    }
                }
                else if (module.Kind == Niagara::ModuleKind::ConeVelocity)
                {
                    const float speed = (std::max)(0.0f, value[3] + module.Secondary[1]);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        randomVelocity[axis] += speed;
                    }
                }
                else if (module.Kind == Niagara::ModuleKind::PointAttraction ||
                         module.Kind == Niagara::ModuleKind::VortexForce ||
                         module.Kind == Niagara::ModuleKind::RenderRadialWave)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        acceleration[axis] += std::abs(value[3]);
                    }
                }
                else if (module.Kind == Niagara::ModuleKind::Gravity || module.Kind == Niagara::ModuleKind::Noise)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        acceleration[axis] += std::abs(value[axis]);
                    }
                }
                else if (module.Kind == Niagara::ModuleKind::ScaleOverLife)
                {
                    sizeScale = (std::max)({sizeScale, std::abs(value[0]), std::abs(module.Secondary[0])});
                    for (const auto& key : module.Curve)
                    {
                        sizeScale = (std::max)(sizeScale, std::abs(key.Value[0]));
                    }
                }
            }
            std::array<float, 3> extent{};
            for (int axis = 0; axis < 3; ++axis)
            {
                extent[axis] = spawnRadius + (std::abs(velocity[axis]) + randomVelocity[axis]) * lifetime;
                if (emitter.LocalSpace)
                {
                    extent[axis] += acceleration[axis] * lifetime * lifetime * 0.5f;
                }
            }
            EffectBounds local;
            local.Min = {-extent[0], -extent[1], -extent[2]};
            local.Max = {extent[0], extent[1], extent[2]};
            EffectBounds emitterBounds;
            for (int corner = 0; corner < 8; ++corner)
            {
                emitterBounds.Include(XMVector3TransformCoord(BoundsCorner(local, corner), world),
                                      std::abs(size) * sizeScale * (emitter.LocalSpace ? worldScale : 1.0f));
            }
            if (!emitter.LocalSpace)
            {
                const XMVECTOR accelerationExtent = XMVectorSet(acceleration[0], acceleration[1], acceleration[2], 0);
                const auto displacement = XMVectorScale(accelerationExtent, lifetime * lifetime * 0.5f);
                XMStoreFloat3(&emitterBounds.Min, XMVectorSubtract(XMLoadFloat3(&emitterBounds.Min), displacement));
                XMStoreFloat3(&emitterBounds.Max, XMVectorAdd(XMLoadFloat3(&emitterBounds.Max), displacement));
            }
            result.Include(XMLoadFloat3(&emitterBounds.Min));
            result.Include(XMLoadFloat3(&emitterBounds.Max));
        }
        for (const auto& particle : instance.Runtime.Particles())
        {
            const auto emitter =
                std::find_if(instance.System.Emitters.begin(), instance.System.Emitters.end(), [&](const auto& item) {
                    return item.Id == particle.EmitterId;
                });
            if (emitter == instance.System.Emitters.end())
            {
                continue;
            }
            auto position = XMLoadFloat3(&particle.Position);
            if (emitter->LocalSpace)
            {
                position = XMVector3TransformCoord(position, world);
            }
            result.Include(position, std::abs(particle.Size) * (emitter->LocalSpace ? worldScale : 1.0f));
        }
        return result;
    }

    void DrawBoundsLine(ImDrawList* draw, XMVECTOR start, XMVECTOR end, FXMMATRIX viewProjection, ImVec2 origin,
                        ImVec2 size, ImU32 color, float thickness)
    {
        auto first = XMVector4Transform(start, viewProjection);
        auto second = XMVector4Transform(end, viewProjection);
        const XMVECTOR planes[] = {XMVectorSet(1, 0, 0, 1),  XMVectorSet(-1, 0, 0, 1), XMVectorSet(0, 1, 0, 1),
                                   XMVectorSet(0, -1, 0, 1), XMVectorSet(0, 0, 1, 0),  XMVectorSet(0, 0, -1, 1)};
        // 同次座標で視錐台をクリップし、カメラ近傍で線が反転するのを防ぐ。
        for (const auto& plane : planes)
        {
            const float a = XMVectorGetX(XMVector4Dot(first, plane));
            const float b = XMVectorGetX(XMVector4Dot(second, plane));
            if (a < 0 && b < 0)
            {
                return;
            }
            if ((a < 0) != (b < 0))
            {
                const auto intersection = XMVectorLerp(first, second, a / (a - b));
                if (a < 0)
                {
                    first = intersection;
                }
                else
                {
                    second = intersection;
                }
            }
        }
        if (XMVectorGetW(first) < 1e-5f || XMVectorGetW(second) < 1e-5f)
        {
            return;
        }
        const auto project = [&](XMVECTOR clip) {
            const float inverseW = 1.0f / XMVectorGetW(clip);
            return ImVec2(origin.x + (XMVectorGetX(clip) * inverseW * 0.5f + 0.5f) * size.x,
                          origin.y + (0.5f - XMVectorGetY(clip) * inverseW * 0.5f) * size.y);
        };
        draw->AddLine(project(first), project(second), color, thickness);
    }

    std::string PathText(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto workingDirectory = std::filesystem::current_path(error);
        const auto relative = error ? path : std::filesystem::relative(path, workingDirectory, error);
        const auto text = (error || relative.empty() ? path : relative).lexically_normal().u8string();
        return std::string(text.begin(), text.end());
    }

    void Restart(NiagaraComponent& component)
    {
        auto& instance = *component.Instance;
        if (instance.Loaded)
        {
            instance.Runtime.Reset(instance.System);
        }
        instance.Playing = component.AutoPlay;
    }

    NiagaraSceneInstance& Prepare(NiagaraComponent& component)
    {
        if (!component.Instance)
        {
            component.Instance = std::make_shared<NiagaraSceneInstance>();
            component.Instance->Playing = component.AutoPlay;
        }
        auto& instance = *component.Instance;
        const auto now = std::chrono::steady_clock::now();
        const bool assetChanged = instance.AssetPath != component.AssetPath;
        if (instance.LoopOverride != component.LoopOverride)
        {
            instance.LoopOverride = component.LoopOverride;
            instance.System.Loop = component.LoopOverride < 0 ? instance.AssetLoop : component.LoopOverride == 1;
            Restart(component);
        }
        if (assetChanged || instance.ReloadRequested || now >= instance.NextFileCheck)
        {
            instance.NextFileCheck = now + std::chrono::seconds(1);
            std::error_code error;
            const auto path = std::filesystem::u8path(component.AssetPath);
            const auto writeTime = std::filesystem::last_write_time(path, error);
            if (assetChanged || instance.ReloadRequested || writeTime != instance.WriteTime ||
                (!instance.Loaded && instance.Error.empty()))
            {
                instance.ReloadRequested = false;
                instance.AssetPath = component.AssetPath;
                instance.LoopOverride = component.LoopOverride;
                instance.WriteTime = writeTime;
                Niagara::System system;
                std::string diagnostic;
                if (Niagara::Load(path, system, diagnostic))
                {
                    instance.AssetLoop = system.Loop;
                    if (component.LoopOverride >= 0)
                    {
                        system.Loop = component.LoopOverride == 1;
                    }
                    instance.System = std::move(system);
                    instance.Loaded = true;
                    instance.Error.clear();
                    Restart(component);
                }
                else
                {
                    instance.Error = diagnostic;
                    // A broken replacement keeps the last valid program for the same asset.
                    if (assetChanged)
                    {
                        instance.Loaded = false;
                    }
                }
            }
        }
        return instance;
    }
}

EntityID NiagaraScene::Create(const std::filesystem::path& path, std::string& error)
{
    Niagara::System system;
    if (!Niagara::Load(path, system, error))
    {
        return g_kINVALID_ENTITY;
    }
    Entity entity = World::CreateEntity();
    if (!entity.IsValid())
    {
        error = "Cannot create an effect: the scene entity limit was reached.";
        return g_kINVALID_ENTITY;
    }
    entity.Add<NameComponent>().Add<TransformComponent>().Add<NiagaraComponent>();
    entity.SetName(system.Name + " " + std::to_string(entity.GetID()));
    auto& transform = entity.Get<TransformComponent>();
    XMStoreFloat4x4(&transform.WorldMatrix, XMMatrixIdentity());
    auto& component = entity.Get<NiagaraComponent>();
    component.AssetPath = PathText(path.lexically_normal());
    component.Instance = std::make_shared<NiagaraSceneInstance>();
    auto& instance = *component.Instance;
    instance.AssetPath = component.AssetPath;
    instance.AssetLoop = system.Loop;
    instance.System = std::move(system);
    instance.Loaded = true;
    std::error_code fileError;
    instance.WriteTime = std::filesystem::last_write_time(path, fileError);
    instance.NextFileCheck = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    Restart(component);
    error.clear();
    return entity.GetID();
}

void NiagaraScene::Update()
{
    const bool playing = ProjectManager::IsPlaying();
    const uint64_t session = ProjectManager::GetPlaySession();
    const float delta = World::GetDeltaTime();
    ComponentManager::ForEach<NiagaraComponent, TransformComponent>(
        [&](EntityID, NiagaraComponent& component, TransformComponent& transform) {
            auto& instance = Prepare(component);
            if (instance.PlaySession != session || instance.WasPlaying != playing)
            {
                instance.PlaySession = session;
                instance.WasPlaying = playing;
                Restart(component);
            }
            instance.Runtime.SetTransform(transform.WorldMatrix);
            if (!instance.Loaded || !component.Enabled || ProjectManager::IsPaused())
            {
                return;
            }
            if (!playing && !component.PreviewInEditor)
            {
                return;
            }
            if (instance.Runtime.Seeking())
            {
                instance.Runtime.UpdateSeek();
            }
            else if (instance.Playing && std::isfinite(delta) && delta > 0.0f)
            {
                const float rate =
                    std::isfinite(component.PlaybackRate) ? std::clamp(component.PlaybackRate, 0.0f, 16.0f) : 1.0f;
                instance.Runtime.Advance(delta * rate);
            }
        });
}

void NiagaraScene::Draw()
{
    ComponentManager::ForEach<NiagaraComponent, TransformComponent>(
        [](EntityID entity, NiagaraComponent& component, TransformComponent& transform) {
            if (!EditorVisibility::IsHidden(entity) && component.Enabled && component.Instance &&
                component.Instance->Loaded)
            {
                const auto& instance = *component.Instance;
                NiagaraRenderer::DrawScene(instance.System, instance.Runtime.Particles(),
                                           XMLoadFloat4x4(&transform.WorldMatrix));
            }
        });
}

void NiagaraScene::DrawBoundsGrid(ImDrawList* draw, ImVec2 origin, ImVec2 size, EntityID selected)
{
    if (!draw || size.x <= 0 || size.y <= 0 || ProjectManager::IsPlaying() ||
        !ComponentManager::HasComponent<NiagaraComponent>(selected) ||
        !ComponentManager::HasComponent<TransformComponent>(selected) || EditorVisibility::IsHidden(selected))
    {
        return;
    }
    auto& component = ComponentManager::GetComponentUnchecked<NiagaraComponent>(selected);
    const auto& instance = Prepare(component);
    if (!instance.Loaded || !instance.ShowBoundsGrid)
    {
        return;
    }
    const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(selected);
    const auto bounds = CalculateBounds(instance, XMLoadFloat4x4(&transform.WorldMatrix));
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    const auto viewProjection = view * projection;
    const ImU32 edgeColor = IM_COL32(242, 176, 67, 235);
    const ImU32 gridColor = IM_COL32(242, 176, 67, 75);
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    for (int axis = 0; axis < 3; ++axis)
    {
        const int axisBit = 1 << axis;
        const int sideBit = 1 << ((axis + 1) % 3);
        const int otherBit = 1 << ((axis + 2) % 3);
        for (int corner = 0; corner < 8; ++corner)
        {
            if (!(corner & axisBit))
            {
                DrawBoundsLine(draw, BoundsCorner(bounds, corner), BoundsCorner(bounds, corner | axisBit),
                               viewProjection, origin, size, edgeColor, 1.5f);
            }
        }
        for (int division = 1; division < 5; ++division)
        {
            const float fraction = division / 5.0f;
            for (int face = 0; face < 2; ++face)
            {
                const int corner = face ? otherBit : 0;
                const auto start =
                    XMVectorLerp(BoundsCorner(bounds, corner), BoundsCorner(bounds, corner | axisBit), fraction);
                const auto end = XMVectorLerp(BoundsCorner(bounds, corner | sideBit),
                                              BoundsCorner(bounds, corner | sideBit | axisBit), fraction);
                DrawBoundsLine(draw, start, end, viewProjection, origin, size, gridColor, 1.0f);
                const auto crossStart =
                    XMVectorLerp(BoundsCorner(bounds, corner), BoundsCorner(bounds, corner | sideBit), fraction);
                const auto crossEnd = XMVectorLerp(BoundsCorner(bounds, corner | axisBit),
                                                   BoundsCorner(bounds, corner | axisBit | sideBit), fraction);
                DrawBoundsLine(draw, crossStart, crossEnd, viewProjection, origin, size, gridColor, 1.0f);
            }
        }
    }
    draw->PopClipRect();
}

void NiagaraScene::Shutdown()
{
    ComponentManager::ForEachComponent<NiagaraComponent>([](EntityID, NiagaraComponent& component) {
        component.Instance.reset();
    });
}

bool NiagaraScene::DrawInspector(EntityID entity)
{
    if (!ComponentManager::HasComponent<NiagaraComponent>(entity))
    {
        return false;
    }
    auto& component = ComponentManager::GetComponentUnchecked<NiagaraComponent>(entity);
    ImGui::SeparatorText("Niagara");
    ImGui::TextWrapped("Asset: %s", component.AssetPath.c_str());
    if (ImGui::Button("Open in Niagara Editor"))
    {
        ImGuiManager::ShowNiagaraAsset(std::filesystem::u8path(component.AssetPath));
    }
    ImGui::SameLine();
    bool changed = false;
    if (ImGui::Button("Choose effect##NiagaraAsset"))
    {
        std::filesystem::path path;
        if (ProceduralIO::OpenFileDialog(L"Niagara (*.naiagara)\0*.naiagara\0", path))
        {
            component.AssetPath = PathText(path.lexically_normal());
            component.Instance.reset();
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("Enabled##Niagara", &component.Enabled);
    changed |= ImGui::Checkbox("Auto play##Niagara", &component.AutoPlay);
    changed |= ImGui::Checkbox("Preview in editor##Niagara", &component.PreviewInEditor);
    changed |= ImGui::DragFloat("Playback rate##Niagara", &component.PlaybackRate, 0.01f, 0.0f, 16.0f);
    const char* loopModes[] = {"Follow asset", "Once", "Loop"};
    int loop = component.LoopOverride + 1;
    if (ImGui::Combo("Loop##Niagara", &loop, loopModes, IM_ARRAYSIZE(loopModes)))
    {
        component.LoopOverride = loop - 1;
        changed = true;
    }
    auto& instance = Prepare(component);
    ImGui::Checkbox("範囲グリッド##NiagaraBounds", &instance.ShowBoundsGrid);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "選択中に標準モジュールの推定範囲と現在の粒子を囲みます。\nカスタムグラフの将来の移動範囲は含まない場合があります。");
    }
    if (ImGui::Button(instance.Playing ? "Pause##NiagaraInstance" : "Play##NiagaraInstance"))
    {
        instance.Playing = !instance.Playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart##NiagaraInstance"))
    {
        Restart(component);
        instance.Playing = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload##NiagaraInstance"))
    {
        instance.ReloadRequested = true;
    }
    ImGui::Text("Time: %.2f s | Particles: %zu", instance.Runtime.Time(), instance.Runtime.Particles().size());
    if (!instance.Error.empty())
    {
        ImGui::TextWrapped("%s", instance.Error.c_str());
    }
    if (!instance.Runtime.Diagnostic().empty())
    {
        ImGui::TextWrapped("%s", instance.Runtime.Diagnostic().c_str());
    }
    return changed;
}
