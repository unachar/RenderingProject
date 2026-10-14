#include "pch.h"
#include "virtualgeometrydebug.h"
#include "virtualgeometrytraversal.h"
#include "virtualgeometryruntime.h"
#include "staticmodel.h"
#include "modelmanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "camera.h"
#include "gpuscene/gpuscene.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "meshshader/meshshaderrenderer.h"
#include "meshlets/meshletmanager.h"
#include "visibility/gpuvisibility.h"
#include "vertexdeformation.h"
#include <functional>

namespace VirtualGeometryDebug
{
namespace
{
struct Options
{
    bool HierarchyBounds = false, Wireframe = false, SelectedOnly = true;
    int HierarchyLevel = 0, SourceLOD = 0, Limit = 512;
};
Options &Settings()
{
    static Options options;
    return options;
}
vector<const StaticMeshData *> Meshes(uint32_t entity)
{
    vector<const StaticMeshData *> result;
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        auto *model = ModelManager::GetStaticModel(
            ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (model)
            for (UINT i = 0; i < model->GetMeshCount(); ++i)
                result.push_back(&model->GetMeshData(i));
    }
    else if (ComponentManager::HasComponent<MeshComponent>(entity))
    {
        const auto &mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (mesh.NaniteMesh)
            result.push_back(mesh.NaniteMesh.get());
    }
    return result;
}
const char *FallbackReason(uint32_t entity)
{
    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
        return "Skinned mesh: traditional rendering";
    if (VertexDeformation::IsRequested(entity))
        return "Vertex deformation: traditional rendering";
    if (!GPUDrivenRenderer::SupportsEntity(entity))
        return "Material, shader or background: traditional rendering";
    if (!MeshShaderRenderer::Supported())
        return "Mesh Shader hardware unavailable";
    if (!MeshShaderRenderer::IsActive())
        return "Mesh Shader path disabled or render mode incompatible";
    if (!GPUDrivenRenderer::HandlesEntity(entity))
        return "GPU geometry not ready";
    return "Automatic Nanite / leaf clusters (finest hierarchy level)";
}
const ImU32 LODColors[] = {IM_COL32(26, 255, 51, 255),  IM_COL32(26, 128, 255, 255),
                           IM_COL32(255, 204, 26, 255), IM_COL32(255, 77, 26, 255),
                           IM_COL32(179, 51, 255, 255), IM_COL32(51, 255, 255, 255),
                           IM_COL32(255, 51, 179, 255), IM_COL32(204, 204, 204, 255)};
} // namespace

void DrawControls()
{
    auto &options = Settings();
    auto &visibility = GPUVisibility::GetSettings();
    auto &mesh = MeshShaderRenderer::GetSettings();
    auto& native = VirtualGeometryTraversal::GetSettings();
    ImGui::Text("Native hardware: %s / frame path: %s",MeshShaderRenderer::Supported() ? "supported" : "unsupported",
        MeshShaderRenderer::IsActive() && VirtualGeometryTraversal::Active() && native.Enabled ? "active" : "inactive");
    ImGui::Checkbox("Native hierarchy rendering",&native.Enabled);
    ImGui::SliderFloat("Target Error Pixels",&native.TargetErrorPixels,0.1f,8.0f,"%.2f");
    ImGui::SliderInt("Force Hierarchy Level (-1 = Auto)",&native.ForceHierarchyLevel,-1,31);
    ImGui::Checkbox("Selected Hierarchy Level Colors",&native.LevelColors);
    ImGui::Checkbox("Hierarchy Frustum Culling",&native.FrustumCulling);
    ImGui::Checkbox("Hierarchy Cone Culling",&native.ConeCulling);
    ImGui::Checkbox("Hierarchy Hi-Z Culling",&native.OcclusionCulling);
    ImGui::Checkbox("Native GPU Statistics (synchronous debug capture)",&native.DebugReadback);
    ImGui::Text("Native assets: %u / GPU memory %.2f MiB",VirtualGeometryResources::AssetCount(),
        double(VirtualGeometryResources::MemoryBytes())/(1024*1024));
    if (native.DebugReadback)
    {
        const auto& sample = VirtualGeometryTraversal::DebugCounters();
        ImGui::Text("Selected clusters %u / triangles %u / native instances %u / capacity fallbacks %u",
            sample[1],sample[6],sample[4],sample[3]);
        for (uint32_t level = 0; level < 32; ++level)
        {
            if (sample[8+level]) ImGui::Text("Selected Level %u: %u clusters",level,sample[8+level]);
        }
    }
    ImGui::TextDisabled("Streaming, hysteresis and raster binning: later phases");
    const auto stats = VirtualGeometryRuntime::GetStatistics();
    ImGui::Text("Cache hits %llu / shared %llu / builds %llu / failures %llu", stats.CacheHits,
                stats.SharedHits, stats.Builds, stats.Failures);
    ImGui::Text("Import/cache CPU time: %.2f ms (cumulative)", stats.BuildMilliseconds);
    ImGui::Text("Geometry arena %.2f MiB / frame upload %.2f KiB",
                double(GPUDrivenRenderer::GeometryArenaBytes()) / (1024 * 1024),
                double(GPUDrivenRenderer::GeometryUploadBytes()) / 1024);
    if (ImGui::Checkbox("Automatic Nanite rendering", &mesh.Enabled))
        GPUDrivenRenderer::SetEnabled(mesh.Enabled);
    int mode = visibility.DebugMode == 2 ? 1 : mesh.Colors ? 2 : 0;
    if (ImGui::Combo("View", &mode, "Normal\0LOD View\0Meshlet colors\0"))
    {
        visibility.DebugMode = mode == 1 ? 2 : 0;
        mesh.Colors = mode == 2;
    }
    for (uint32_t lod = 0; lod < 8; ++lod)
    {
        if (lod)
            ImGui::SameLine();
        ImGui::ColorButton(("LOD" + to_string(lod)).c_str(),
                           ImGui::ColorConvertU32ToFloat4(LODColors[lod]), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::Text("%u", lod);
    }
    ImGui::SliderInt("Force LOD (-1 = Auto)##Nanite", &visibility.ForceLOD, -1, 7);
    ImGui::Checkbox("Freeze culling camera (Hi-Z disabled)", &visibility.FreezeCamera);
    ImGui::Checkbox("LOD statistics (delayed GPU sample)", &visibility.DebugReadback);
    if (visibility.DebugReadback && GPUVisibility::HasStatisticsSample())
    {
        const auto &sample = GPUVisibility::LatestStatistics();
        ImGui::Text("Last completed sample: %u visible / %u triangles", sample.FinalVisible,
                    sample.Triangles);
        for (uint32_t lod = 0; lod < 8; ++lod)
        {
            if (lod)
                ImGui::SameLine();
            ImGui::Text("LOD%u:%u", lod, sample.LODCounts[lod]);
        }
    }
    else
        ImGui::TextDisabled("LOD counts: sample not requested or not available yet");
    ImGui::Checkbox("Selected entity only", &options.SelectedOnly);
    ImGui::SliderInt("Overlay cluster limit", &options.Limit, 16, 4096);
    ImGui::Checkbox("Viewport meshlet bounds", &mesh.MeshletBounds);
    ImGui::Checkbox("Viewport cluster bounds", &mesh.ClusterBounds);
    ImGui::Checkbox("Viewport normal cones", &mesh.ConeAxes);
    ImGui::Checkbox("Hierarchy bounds preview", &options.HierarchyBounds);
    ImGui::Checkbox("Leaf triangle wireframe", &options.Wireframe);
    ImGui::SliderInt("Preview source Discrete LOD", &options.SourceLOD, 0, 4);
    ImGui::SliderInt("Preview hierarchy level", &options.HierarchyLevel, 0, 31);
    ImGui::TextDisabled("Bounds overlay is CPU preview; Native Level Colors shows actual selected geometry.");
    ImGui::TextDisabled("Culling overlays sample at most 4096 candidates; readback stalls GPU.");
    ImGui::Checkbox("Meshlet frustum##Nanite", &mesh.Frustum);
    ImGui::Checkbox("Meshlet occlusion##Nanite", &mesh.Occlusion);
    ImGui::Checkbox("Cluster culling##Nanite", &mesh.Clusters);
    if (ImGui::Button("Return to normal view"))
    {
        visibility.DebugMode = 0;
        visibility.ForceLOD = -1;
        visibility.FreezeCamera = false;
        mesh.Colors = false;
        mesh.MeshletBounds = false;
        mesh.ClusterBounds = false;
        mesh.ConeAxes = false;
        mesh.DebugReadback = false;
        visibility.DebugReadback = false;
        options.HierarchyBounds = false;
        options.Wireframe = false;
    }
    const uint32_t selected = ImGuiManager::GetSelectedEntity();
    if (Registry::IsAlive(selected))
    {
        ImGui::Text("Selected entity: %u", selected);
        ImGui::TextWrapped("%s", FallbackReason(selected));
        for (const auto *source : Meshes(selected))
        {
            const auto &asset = source->VirtualGeometryLODs[options.SourceLOD];
            if (!asset)
            {
                ImGui::Text("%s: source LOD not available", source->MeshName.c_str());
                continue;
            }
            ImGui::Text("%s: %s, %.2f ms", source->MeshName.c_str(), asset->CacheHit ? "cache" : "built",
                        asset->BuildMilliseconds);
            if (!asset->Error.empty())
            {
                ImGui::TextWrapped("%s", asset->Error.c_str());
                continue;
            }
            if (!asset->HierarchyWarning.empty())
                ImGui::TextWrapped("Hierarchy: %s (leaf rendering available)",
                                   asset->HierarchyWarning.c_str());
            if (ImGui::BeginTable(("Hierarchy##" + source->MeshName + to_string(asset->Key)).c_str(), 4))
            {
                ImGui::TableSetupColumn("Level");
                ImGui::TableSetupColumn("Clusters");
                ImGui::TableSetupColumn("Triangles");
                ImGui::TableSetupColumn("Max error");
                ImGui::TableHeadersRow();
                for (size_t level = 0; level < asset->Hierarchy.Levels.size(); ++level)
                {
                    const auto &item = asset->Hierarchy.Levels[level];
                    float error = 0;
                    for (const auto &cluster : asset->Hierarchy.Clusters)
                        if (cluster.Level == level)
                            error = max(error, cluster.LodError);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", level);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", item.Clusters);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", item.Triangles);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.5f", error);
                }
                ImGui::EndTable();
            }
        }
    }
    uint32_t eligible = 0, fallback = 0;
    for (uint32_t slot = 0; slot < GpuScene::Statistics().InstanceSlots; ++slot)
    {
        const auto *instance = GpuScene::GetInstance(slot);
        if (!instance || !(instance->Flags & 1u))
            continue;
        if (instance->Flags & 4u)
            ++eligible;
        else
            ++fallback;
    }
    ImGui::Text("Scene sections: GPU eligible %u / traditional %u", eligible, fallback);
    ImGui::TextDisabled("GPU eligible can still fall back on missing clusters or capacity overflow.");
}

void DrawViewport(ImDrawList *draw, const ImVec2 &origin, const ImVec2 &size, uint32_t selected)
{
    auto &options = Settings();
    const auto &settings = MeshShaderRenderer::GetSettings();
    if (!options.HierarchyBounds && !options.Wireframe && !settings.MeshletBounds &&
        !settings.ClusterBounds && !settings.ConeAxes)
        return;
    const auto camera = Camera::GetCameraEntity();
    if (!Registry::IsAlive(camera))
        return;
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(camera, view, projection);
    const XMMATRIX vp = view * projection;
    auto project = [&](XMVECTOR local, XMMATRIX world, ImVec2 &point)
    {
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(XMVector4Transform(local, world), vp));
        if (clip.w <= 1e-5f || clip.z < 0 || clip.z > clip.w)
            return false;
        point = {origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x,
                 origin.y + (0.5f - clip.y / clip.w * 0.5f) * size.y};
        return true;
    };
    auto sphere = [&](const Meshlets::Bounds &bounds, XMMATRIX world, ImU32 color)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            ImVec2 previous{};
            bool have = false;
            for (uint32_t step = 0; step <= 24; ++step)
            {
                const float angle = XM_2PI * float(step) / 24;
                float xyz[] = {bounds.Center.X, bounds.Center.Y, bounds.Center.Z};
                xyz[axis] += cosf(angle) * bounds.Radius;
                xyz[(axis + 1) % 3] += sinf(angle) * bounds.Radius;
                ImVec2 point{};
                const bool valid = project(XMVectorSet(xyz[0], xyz[1], xyz[2], 1), world, point);
                if (have && valid)
                    draw->AddLine(previous, point, color);
                previous = point;
                have = valid;
            }
        }
    };
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    uint32_t displayed = 0;
    if (settings.MeshletBounds || settings.ClusterBounds || settings.ConeAxes)
    {
        const auto &data = MeshletManager::AssetData();
        const ImU32 colors[] = {IM_COL32(150, 150, 150, 180), IM_COL32(40, 230, 230, 200),
                                IM_COL32(200, 80, 240, 200),  IM_COL32(250, 60, 60, 200),
                                IM_COL32(240, 220, 40, 200),  IM_COL32(250, 150, 40, 200)};
        for (const auto &record : MeshShaderRenderer::DebugRecords())
        {
            if (displayed >= uint32_t(options.Limit))
                break;
            const auto *instance = GpuScene::GetInstance(record[0]);
            if (!instance || !(instance->Flags & 1u) || record[1] >= data.Bounds.size() ||
                (options.SelectedOnly && instance->Entity != selected))
                continue;
            ++displayed;
            const auto world = XMLoadFloat4x4(&instance->World);
            const auto &bounds = data.Bounds[record[1]];
            if (settings.MeshletBounds)
                sphere(bounds, world, colors[min(record[2], 5u)]);
            if (settings.ClusterBounds && record[3] < data.Clusters.size() &&
                record[1] == data.Clusters[record[3]].MeshletOffset)
                sphere(data.Clusters[record[3]].Bounds, world, IM_COL32(190, 190, 255, 200));
            if (settings.ConeAxes && bounds.ConeCutoff < 1)
            {
                ImVec2 start, end;
                auto center = XMVectorSet(bounds.Center.X, bounds.Center.Y, bounds.Center.Z, 1);
                auto tip = center + XMVectorSet(bounds.ConeAxis.X, bounds.ConeAxis.Y, bounds.ConeAxis.Z, 0) *
                                        bounds.Radius;
                if (project(center, world, start) && project(tip, world, end))
                    draw->AddLine(start, end, IM_COL32_WHITE, 2);
            }
        }
    }
    if ((options.HierarchyBounds || options.Wireframe) && Registry::IsAlive(selected) &&
        ComponentManager::HasComponent<TransformComponent>(selected))
    {
        const auto world = XMLoadFloat4x4(
            &ComponentManager::GetComponentUnchecked<TransformComponent>(selected).WorldMatrix);
        uint32_t previews = 0;
        for (const auto *source : Meshes(selected))
        {
            const auto &asset = source->VirtualGeometryLODs[options.SourceLOD];
            if (!asset || !asset->Error.empty())
                continue;
            const auto &hierarchy = asset->Hierarchy;
            for (const auto &cluster : hierarchy.Clusters)
            {
                if (previews >= uint32_t(options.Limit))
                    break;
                const bool bounds =
                    options.HierarchyBounds && cluster.Level == uint32_t(options.HierarchyLevel);
                const bool wire = options.Wireframe && cluster.Level == 0;
                if (!bounds && !wire)
                    continue;
                ++previews;
                const auto color = LODColors[min(cluster.Level, 7u)];
                if (bounds)
                    sphere({{cluster.Bounds.Center[0], cluster.Bounds.Center[1], cluster.Bounds.Center[2]},
                            cluster.Bounds.Radius,
                            {},
                            1},
                           world, color);
                if (wire)
                    for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
                    {
                        ImVec2 points[3];
                        bool valid = true;
                        for (uint32_t i = 0; i < 3; ++i)
                        {
                            const auto &vertex =
                                hierarchy.Vertices[cluster.VertexOffset +
                                                   hierarchy.TriangleIndices[cluster.TriangleOffset +
                                                                             triangle * 3 + i]];
                            valid &= project(
                                XMVectorSet(vertex.Position[0], vertex.Position[1], vertex.Position[2], 1),
                                world, points[i]);
                        }
                        if (valid)
                            for (uint32_t i = 0; i < 3; ++i)
                                draw->AddLine(points[i], points[(i + 1) % 3], color);
                    }
            }
        }
        displayed += previews;
    }
    draw->AddText(ImVec2(origin.x + 8, origin.y + 8), IM_COL32_WHITE,
                  ("Nanite overlay: " + to_string(displayed) + " clusters (limit " +
                   to_string(options.Limit) + ")")
                      .c_str());
    draw->PopClipRect();
}
} // namespace VirtualGeometryDebug
