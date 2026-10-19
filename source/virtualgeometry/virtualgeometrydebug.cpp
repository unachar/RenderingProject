#include "pch.h"
#include "virtualgeometrydebug.h"
#include "virtualgeometrytraversal.h"
#include "virtualgeometrybinning.h"
#include "virtualgeometrystreaming.h"
#include "virtualgeometryruntime.h"
#include "virtualgeometryresources.h"
#include "virtualgeometryproduction.h"
#include "renderprofiler.h"
#include "occlusionculling.h"
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

VGStatistics GetVGStatistics()
{
    VGStatistics result;
    result.RegisteredAssets = VirtualGeometryResources::AssetCount();
    result.RegisteredClusters = VirtualGeometryResources::RegisteredClusterCount();
    const auto& streaming = VirtualGeometryStreaming::GetStatistics();
    result.ResidentPages = streaming.Resident;
    result.RequestedPages = streaming.Requested;
    result.Evictions = streaming.Evictions;
    result.ResidentAllocationBytes = streaming.PoolBytes;
    result.BudgetBytes = streaming.BudgetBytes;
    result.MetadataBytes = streaming.MetadataBytes;
    result.SubmittedBytes = streaming.SubmittedBytes;
    result.CompletedBytes = streaming.CompletedBytes;
    result.IoErrors = streaming.IoErrors;
    result.RequestOverflow = streaming.RequestOverflow;
    const auto compression = VirtualGeometryResources::CompressionMetrics();
    result.RawGeometryBytes = compression.RawBytes;
    result.EncodedGeometryBytes = compression.EncodedBytes;
    const auto& visibility = VirtualGeometryTraversal::GetTemporalStatistics();
    result.HasVisibilitySample = visibility.HasSample && VirtualGeometryTraversal::Active() &&
                                 MeshShaderRenderer::IsActive() && VirtualGeometryTraversal::GetSettings().Enabled &&
                                 GetGeometryRenderingMode() != GeometryRenderingMode::Legacy;
    result.VisibilitySampleSerial = visibility.SampleSerial;
    result.FirstVisibleClusters = visibility.FirstVisible;
    result.SecondVisibleClusters = visibility.SecondVisible;
    result.CapacityFallbacks = visibility.CapacityFallbacks;
    result.VisibleTriangles = VirtualGeometryTraversal::DebugCounters()[6];
    const auto& sourceCounts = VirtualGeometryResources::SourceTriangleCounts();
    for (const auto count : sourceCounts)
    {
        result.SourceTriangles += count;
    }
    result.SceneInstances = GpuScene::Statistics().ActiveInstances;
    const auto& overflow = GPUDrivenRenderer::GetOverflowStatistics();
    result.VisibleInstanceOverflow = overflow.VisibleInstanceOverflow;
    result.InvalidIndirectCommands = overflow.InvalidIndirectCommands;
    result.VisibleInstances = VirtualGeometryTraversal::DebugCounters()[4];
    result.HasInstanceSample = result.HasVisibilitySample;
    for (uint32_t slot = 0; slot < GpuScene::Statistics().InstanceSlots; ++slot)
    {
        const auto* instance = GpuScene::GetInstance(slot);
        if (instance && (instance->Flags & 16u) && instance->VirtualGeometryAssetIndex < sourceCounts.size())
        {
            ++result.Instances;
            result.InstancedSourceTriangles += sourceCounts[instance->VirtualGeometryAssetIndex];
        }
    }
    for (const auto& phase : VirtualGeometryBinning::GetStatistics().Phases)
    {
        if (phase.HasSample)
        {
            result.MaterialBinOverflow += phase.OverflowClusters;
            result.InvalidMaterialRecords += phase.InvalidMaterials;
            result.BinningRejectedClusters += phase.RejectedClusters;
        }
    }
    return result;
}

namespace VirtualGeometryDebug
{
    namespace
    {
        VirtualGeometryDebug::Preferences& Settings()
        {
            static VirtualGeometryDebug::Preferences options;
            return options;
        }

        vector<const StaticMeshData*> Meshes(uint32_t entity)
        {
            vector<const StaticMeshData*> result;
            if (ComponentManager::HasComponent<StaticModelComponent>(entity))
            {
                auto* model = ModelManager::GetStaticModel(
                    ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
                if (model)
                {
                    for (UINT i = 0; i < model->GetMeshCount(); ++i)
                    {
                        result.push_back(&model->GetMeshData(i));
                    }
                }
            }
            else if (ComponentManager::HasComponent<MeshComponent>(entity))
            {
                const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
                if (mesh.NaniteMesh)
                {
                    result.push_back(mesh.NaniteMesh.get());
                }
            }
            return result;
        }

        const char* FallbackReason(uint32_t entity)
        {
            if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
            {
                return "Skinned mesh: traditional rendering";
            }
            if (VertexDeformation::IsRequested(entity))
            {
                return "Vertex deformation: traditional rendering";
            }
            if (!GPUDrivenRenderer::SupportsEntity(entity))
            {
                return "Material, shader or background: traditional rendering";
            }
            if (!MeshShaderRenderer::Supported())
            {
                return "Mesh Shader hardware unavailable";
            }
            if (!MeshShaderRenderer::IsActive())
            {
                return "Mesh Shader path disabled or render mode incompatible";
            }
            if (!GPUDrivenRenderer::HandlesEntity(entity))
            {
                return "GPU geometry not ready";
            }
            return "Automatic Nanite / leaf clusters (finest hierarchy level)";
        }

        const ImU32 LODColors[] = {IM_COL32(26, 255, 51, 255),  IM_COL32(26, 128, 255, 255),
                                   IM_COL32(255, 204, 26, 255), IM_COL32(255, 77, 26, 255),
                                   IM_COL32(179, 51, 255, 255), IM_COL32(51, 255, 255, 255),
                                   IM_COL32(255, 51, 179, 255), IM_COL32(204, 204, 204, 255)};
    }

    void DrawAssetInspector(const std::filesystem::path& path)
    {
        if (!ImGui::CollapsingHeader("Virtual Geometry", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }
        static std::filesystem::path selected;
        static VGImportSettings settings;
        static std::string error;
        if (selected != path)
        {
            selected = path;
            settings = {};
            VirtualGeometryImport::Load(path, settings, error);
        }
        ImGui::PushID(path.generic_string().c_str());
        ImGui::Checkbox("Enabled", &settings.Enabled);
        ImGui::Checkbox("Build in background", &settings.BackgroundBuild);
        int quality = static_cast<int>(settings.Quality);
        if (ImGui::Combo("Build quality", &quality, "Low\0Medium\0High\0Production\0Custom\0"))
        {
            settings.Quality = static_cast<VGQuality>(quality);
        }
        ImGui::InputFloat("Position error (0 = Auto)", &settings.PositionError, 0, 0, "%.6f");
        ImGui::InputFloat("LOD error (0 = Auto)", &settings.LODError);
        int priority = static_cast<int>(settings.StreamingPriority);
        if (ImGui::Combo("Streaming priority", &priority, "Low\0Normal\0High\0"))
        {
            settings.StreamingPriority = static_cast<uint32_t>(priority);
        }
        if (settings.Quality == VGQuality::Custom)
        {
            ImGui::SliderFloat("Simplification ratio", &settings.Custom.SimplificationRatio, 0.1f, 0.9f);
            ImGui::SliderFloat("Normal weight", &settings.Custom.NormalWeight, 0, 4);
            ImGui::SliderFloat("UV weight", &settings.Custom.TexCoordWeight, 0, 4);
        }
        if (ImGui::Button("Save and rebuild"))
        {
            ModelManager::SaveVirtualGeometrySettings(path, settings, error);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload settings"))
        {
            VirtualGeometryImport::Load(path, settings, error);
        }
        auto* model = ModelManager::FindLoadedStaticModel(path);
        if (!model)
        {
            ImGui::TextUnformatted("Status: not imported in this session");
            if (ImGui::Button("Import mesh"))
            {
                const auto encoded = path.u8string();
                const std::string source(reinterpret_cast<const char*>(encoded.data()), encoded.size());
                ModelManager::LoadStaticModel(source.c_str());
            }
        }
        else
        {
            uint64_t vertices = 0;
            uint64_t triangles = 0;
            uint64_t clusters = 0;
            uint64_t groups = 0;
            uint64_t rawBytes = 0;
            uint64_t encodedBytes = 0;
            uint64_t pages = 0;
            uint64_t pinned = 0;
            uint32_t levels = 0;
            bool building = false;
            for (uint32_t index = 0; index < model->GetMeshCount(); ++index)
            {
                const auto& mesh = model->GetMeshData(index);
                building |= mesh.VirtualGeometryBuilds[0].valid();
                const auto& asset = mesh.VirtualGeometryLODs[0];
                if (asset && asset->Error.empty())
                {
                    vertices += asset->Hierarchy.Stats.SourceVertices;
                    triangles += asset->Hierarchy.Stats.SourceTriangles;
                    clusters += asset->Hierarchy.Stats.TotalClusters;
                    groups += asset->Hierarchy.Stats.TotalGroups;
                    levels = std::max(levels, asset->Hierarchy.Stats.HierarchyLevels);
                    const auto manifest =
                        asset->Pages ? asset->Pages : VirtualGeometryResources::ManifestFor(asset.get());
                    if (manifest)
                    {
                        rawBytes += manifest->Compression.RawBytes;
                        encodedBytes += manifest->Compression.EncodedBytes;
                        pages += manifest->Pages.size();
                        pinned += std::count(manifest->PinnedPages.begin(), manifest->PinnedPages.end(), uint8_t(1));
                    }
                }
                else
                {
                    vertices += mesh.VertexCount;
                    triangles += mesh.IndexCount / 3;
                }
                if (!mesh.VirtualGeometryBuildError.empty())
                {
                    ImGui::TextWrapped("Fallback: %s", mesh.VirtualGeometryBuildError.c_str());
                }
            }
            const char* status = "Legacy fallback";
            if (building)
            {
                status = "Building (legacy mesh available)";
            }
            else if (clusters)
            {
                status = "Hierarchy built; streaming readiness is reported in the dashboard";
            }
            ImGui::TextWrapped("Status: %s", status);
            ImGui::Text("Source vertices %llu / triangles %llu", vertices, triangles);
            ImGui::Text("Clusters %llu / groups %llu / LOD levels %u", clusters, groups, levels);
            ImGui::Text("Raw / encoded %.2f / %.2f MiB / streaming pages %llu", double(rawBytes) / (1024 * 1024),
                        double(encodedBytes) / (1024 * 1024), pages);
            ImGui::Text("Pinned minimum %.2f MiB / resident upper estimate %.2f MiB",
                        double(pinned * VirtualGeometryPages::PageBytes) / (1024 * 1024),
                        double(std::min(pages * VirtualGeometryPages::PageBytes,
                                        VirtualGeometryStreaming::GetBudget().MaxResidentMemory)) /
                            (1024 * 1024));
            if (rawBytes)
            {
                ImGui::Text("Encoded / raw %.1f%%", 100.0 * double(encodedBytes) / rawBytes);
            }
        }
        if (!error.empty())
        {
            ImGui::TextWrapped("%s", error.c_str());
        }
        ImGui::PopID();
    }

    Preferences& GetPreferences()
    {
        return Settings();
    }

    void DrawControls()
    {
        const auto dashboard = GetVGStatistics();
        ImGui::Text("VG instances %u / source triangles %llu / instanced source triangles %llu", dashboard.Instances,
                    dashboard.SourceTriangles, dashboard.InstancedSourceTriangles);
        if (dashboard.HasInstanceSample)
        {
            ImGui::Text("Visible instances %u", dashboard.VisibleInstances);
        }
        else
        {
            ImGui::TextUnformatted("Visible instances: sample pending / GPU path inactive");
        }
        ImGui::Text("Clusters %llu / material overflow %llu / invalid materials %llu", dashboard.RegisteredClusters,
                    dashboard.MaterialBinOverflow, dashboard.InvalidMaterialRecords);
        ImGui::Text("Overflow: instances %llu / indirect rejected %llu / binned clusters rejected %llu",
                    dashboard.VisibleInstanceOverflow, dashboard.InvalidIndirectCommands,
                    dashboard.BinningRejectedClusters);
        if (RenderProfiler::HasLatest() && ImGui::TreeNode("VG GPU timings"))
        {
            const auto& profile = RenderProfiler::GetLatest();
            if (!profile.GpuTimingValid)
            {
                ImGui::TextUnformatted("GPU timestamps unavailable");
            }
            for (const auto& pass : profile.Passes)
            {
                if (pass.Name.find("Nanite") != std::string::npos || pass.Name.find("Native") != std::string::npos ||
                    pass.Name.find("Compact Visible") != std::string::npos ||
                    pass.Name.find("material bin") != std::string::npos)
                {
                    if (profile.GpuTimingValid)
                    {
                        ImGui::Text("%s: %.3f ms", pass.Name.c_str(), pass.GpuMs);
                    }
                }
            }
            ImGui::TextUnformatted(
                "Geometry decode is included in native raster; traversal combines hierarchy and cluster work.");
            ImGui::TreePop();
        }
        if (VirtualGeometryProduction::Recording())
        {
            if (ImGui::Button("Stop VG benchmark"))
            {
                VirtualGeometryProduction::EndBenchmark();
            }
        }
        else if (ImGui::Button("Record VG benchmark"))
        {
            std::string error;
            VirtualGeometryProduction::BeginBenchmark("build/virtualgeometry-phase11", error);
        }
        if (!VirtualGeometryProduction::LastError().empty())
        {
            ImGui::TextWrapped("%s", VirtualGeometryProduction::LastError().c_str());
        }
        auto& options = Settings();
        auto& visibility = GPUVisibility::GetSettings();
        auto& mesh = MeshShaderRenderer::GetSettings();
        auto& native = VirtualGeometryTraversal::GetSettings();
        int view = static_cast<int>(native.View);
        if (ImGui::Combo(
                "VG debug view", &view,
                "None\0LOD\0Cluster\0Hierarchy\0Material bin\0Compression\0Page residency\0Streaming priority\0Occlusion\0Overdraw\0Geometry error\0Object ID\0"))
        {
            native.View = static_cast<VirtualGeometryTraversal::DebugView>(view);
            native.LevelColors = false;
        }
        ImGui::Checkbox("Grow VG buffers from completed overflow feedback", &native.AutoGrowBuffers);
        ImGui::Checkbox("Predictive page prefetch", &native.PredictiveStreaming);
        ImGui::SliderFloat("Prefetch lookahead seconds", &native.PrefetchSeconds, 0, 1);
        ImGui::Text("Prefetch requests %llu (cumulative)", VirtualGeometryStreaming::GetStatistics().PrefetchRequests);
        if (native.View == VirtualGeometryTraversal::DebugView::Occlusion)
        {
            ImGui::TextUnformatted(
                "Cyan: static selected geometry. Orange: moving geometry bypasses history occlusion.");
            ImGui::Text("Deferred %u / recovered %u", VirtualGeometryTraversal::GetTemporalStatistics().Candidates,
                        VirtualGeometryTraversal::GetTemporalStatistics().SecondVisible);
            if (OcclusionCulling::IsAvailable() && OcclusionCulling::GetMipCount())
            {
                const int maximum = static_cast<int>(OcclusionCulling::GetMipCount()) - 1;
                visibility.HiZMip = std::clamp(visibility.HiZMip, 0, maximum);
                ImGui::SliderInt("VG Hi-Z mip", &visibility.HiZMip, 0, maximum);
                OcclusionCulling::SetDebugPreview(static_cast<uint32_t>(visibility.HiZMip), true);
                ImGui::Image(
                    (ImTextureID)OcclusionCulling::GetDebugMipSrv(static_cast<uint32_t>(visibility.HiZMip)).ptr,
                    ImVec2(320, 180));
            }
        }
        if (native.View == VirtualGeometryTraversal::DebugView::PageResidency ||
            native.View == VirtualGeometryTraversal::DebugView::StreamingPriority)
        {
            if (ImGui::BeginTable("VG Page states", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders,
                                  ImVec2(0, 180)))
            {
                for (const auto* label : {"Page", "State", "Priority", "Pinned"})
                {
                    ImGui::TableSetupColumn(label);
                }
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(std::min(VirtualGeometryStreaming::PageCount(), uint32_t(INT_MAX))));
                while (clipper.Step())
                {
                    for (int page = clipper.DisplayStart; page < clipper.DisplayEnd; ++page)
                    {
                        VirtualGeometryStreaming::PageInfo info;
                        if (!VirtualGeometryStreaming::InspectPage(static_cast<uint32_t>(page), info))
                        {
                            continue;
                        }
                        constexpr const char* states[] = {"Evicted", "Requested", "Loading", "Resident"};
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", page);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(info.State < 4 ? states[info.State] : "Invalid");
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", info.Priority);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(info.Pinned ? "yes" : "no");
                    }
                }
                ImGui::EndTable();
            }
        }
        if (native.View == VirtualGeometryTraversal::DebugView::Overdraw)
        {
            ImGui::TextUnformatted(
                "Additive coverage of selected clusters; disable occlusion to inspect hidden surfaces.");
        }
        int geometryMode = static_cast<int>(GetGeometryRenderingMode());
        if (ImGui::Combo("Geometry rendering mode", &geometryMode, "Auto\0VirtualGeometry\0Legacy\0"))
        {
            GetGeometryRenderingMode() = static_cast<GeometryRenderingMode>(geometryMode);
            if (GetGeometryRenderingMode() != GeometryRenderingMode::Legacy)
            {
                GPUDrivenRenderer::SetEnabled(true);
                mesh.Enabled = true;
                native.Enabled = true;
            }
            GPUVisibility::Invalidate("Geometry rendering mode changed");
            VirtualGeometryTraversal::InvalidateHistory();
        }
        ImGui::TextUnformatted("Unsupported geometry always uses the fallback renderer.");
        ImGui::Text(
            "Native hardware: %s / frame path: %s", MeshShaderRenderer::Supported() ? "supported" : "unsupported",
            MeshShaderRenderer::IsActive() && VirtualGeometryTraversal::Active() && native.Enabled ? "active"
                                                                                                   : "inactive");
        ImGui::Checkbox("Native hierarchy rendering", &native.Enabled);
        ImGui::SliderFloat("Target Error Pixels", &native.TargetErrorPixels, 0.1f, 8.0f, "%.2f");
        ImGui::Checkbox("LOD hysteresis", &native.HysteresisEnabled);
        ImGui::SliderFloat("LOD hysteresis width", &native.HysteresisFraction, 0.0f, 0.5f, "%.2f");
        ImGui::Checkbox("Native two-pass occlusion", &native.TwoPassOcclusion);
        const auto& temporal = VirtualGeometryTraversal::GetTemporalStatistics();
        if (temporal.HasSample)
        {
            ImGui::Text("First %u / retest candidates %u / recovered %u / LOD transitions %u", temporal.FirstVisible,
                        temporal.Candidates, temporal.SecondVisible, temporal.LodTransitions);
            ImGui::Text("Native capacity fallbacks %u", temporal.CapacityFallbacks);
        }
        ImGui::Text("LOD history %.2f MiB / phase lists %.2f MiB", double(temporal.HistoryBytes) / (1024 * 1024),
                    double(temporal.CandidateBytes) / (1024 * 1024));
        ImGui::SliderInt("Force Hierarchy Level (-1 = Auto)", &native.ForceHierarchyLevel, -1, 31);
        ImGui::Checkbox("Selected Hierarchy Level Colors", &native.LevelColors);
        ImGui::Checkbox("Hierarchy Frustum Culling", &native.FrustumCulling);
        ImGui::Checkbox("Hierarchy Cone Culling", &native.ConeCulling);
        ImGui::Checkbox("Hierarchy Hi-Z Culling", &native.OcclusionCulling);
        ImGui::Checkbox("Native GPU Statistics (synchronous debug capture)", &native.DebugReadback);
        ImGui::Text("Native assets: %u / GPU memory %.2f MiB", VirtualGeometryResources::AssetCount(),
                    double(VirtualGeometryResources::MemoryBytes()) / (1024 * 1024));
        if (native.DebugReadback)
        {
            const auto& sample = VirtualGeometryTraversal::DebugCounters();
            ImGui::Text("Selected clusters %u / triangles %u / native instances %u / capacity fallbacks %u", sample[1],
                        sample[6], sample[4], sample[3]);
            for (uint32_t level = 0; level < 32; ++level)
            {
                if (sample[8 + level])
                {
                    ImGui::Text("Selected Level %u: %u clusters", level, sample[8 + level]);
                }
            }
        }
        auto& streaming = VirtualGeometryStreaming::GetSettings();
        int requestLimit = static_cast<int>(streaming.MaxPageRequestsPerFrame);
        if (ImGui::SliderInt("Page requests per frame", &requestLimit, 1, 4096))
        {
            streaming.MaxPageRequestsPerFrame = static_cast<uint32_t>(requestLimit);
        }
        int protectionFrames = static_cast<int>(streaming.RecentlyUsedProtectionFrames);
        if (ImGui::SliderInt("Recently used protection frames", &protectionFrames, 1, 120))
        {
            streaming.RecentlyUsedProtectionFrames = static_cast<uint32_t>(protectionFrames);
        }
        const auto& pages = VirtualGeometryStreaming::GetStatistics();
        const auto compression = VirtualGeometryResources::CompressionMetrics();
        ImGui::TextUnformatted("Virtual Geometry Compression");
        ImGui::Text("Encoded / reference raw %.2f / %.2f MiB", double(compression.EncodedBytes) / (1024 * 1024),
                    double(compression.RawBytes) / (1024 * 1024));
        if (compression.RawBytes)
        {
            ImGui::Text("Encoded / raw %.1f%% / codec fallbacks %llu",
                        100.0 * double(compression.EncodedBytes) / double(compression.RawBytes),
                        compression.FallbackCount);
        }
        ImGui::Text("Resident Page allocation %.2f MiB / cumulative uploads %.2f MiB",
                    double(pages.Resident * VirtualGeometryPages::PageBytes) / (1024 * 1024),
                    double(pages.SubmittedBytes) / (1024 * 1024));
        ImGui::Text("Quantized meshes %llu / half UV streams %llu / Index8 %llu / Index16 %llu",
                    compression.PositionQuantized, compression.HalfUV, compression.Index8Clusters,
                    compression.Index16Clusters);
        ImGui::TextDisabled("GPU decode runs inside native raster; separate decode timing is not sampled.");
        int budgetMiB = int(streaming.BudgetMiB);
        if (ImGui::SliderInt("Geometry VRAM Budget (MiB)", &budgetMiB, 1, 1024))
        {
            streaming.BudgetMiB = uint32_t(budgetMiB);
        }
        ImGui::Text("Pages Resident %llu / Requested %llu / Loading %llu / Evicted %llu", pages.Resident,
                    pages.Requested, pages.Loading, pages.Evicted);
        ImGui::Text("Pinned %llu / evictions %llu / request overflow %llu / I/O errors %llu", pages.Pinned,
                    pages.Evictions, pages.RequestOverflow, pages.IoErrors);
        ImGui::Text("Pool actual / reserved / target %.2f / %.2f / %.2f MiB / CPU staging %.2f MiB",
                    double(pages.PoolBytes) / (1024 * 1024), double(pages.ReservedBytes) / (1024 * 1024),
                    double(pages.BudgetBytes) / (1024 * 1024), double(pages.StagingBytes) / (1024 * 1024));
        auto& binning = VirtualGeometryBinning::GetSettings();
        ImGui::Checkbox("Native material binning", &binning.Enabled);
        int materialBins = int(binning.MaxMaterialBins);
        if (ImGui::SliderInt("Exact material bins", &materialBins, 1, 65535))
        {
            binning.MaxMaterialBins = uint32_t(materialBins);
        }
        const auto& binStats = VirtualGeometryBinning::GetStatistics();
        ImGui::TextDisabled("Last completed material bin samples");
        const char* phaseNames[] = {"First", "Second", "Velocity"};
        for (uint32_t phase = 0; phase < 3; ++phase)
        {
            const auto& sample = binStats.Phases[phase];
            if (!sample.HasSample)
            {
                continue;
            }
            ImGui::Text("%s bins %u / clusters %u / shared overflow %u / invalid materials %u", phaseNames[phase],
                        sample.ActiveBins, sample.Clusters, sample.OverflowClusters, sample.InvalidMaterials);
        }
        ImGui::Text("Material bin buffers %.2f MiB", double(binStats.BufferBytes) / (1024 * 1024));
        if (!VirtualGeometryBinning::UnavailableReason().empty())
        {
            ImGui::TextDisabled("%s", VirtualGeometryBinning::UnavailableReason().c_str());
        }
        const auto stats = VirtualGeometryRuntime::GetStatistics();
        ImGui::Text("Cache hits %llu / shared %llu / builds %llu / failures %llu", stats.CacheHits, stats.SharedHits,
                    stats.Builds, stats.Failures);
        ImGui::Text("Import/cache elapsed time: %.2f ms (cumulative)", stats.BuildMilliseconds);
        ImGui::Text("Geometry arena %.2f MiB / frame upload %.2f KiB",
                    double(GPUDrivenRenderer::GeometryArenaBytes()) / (1024 * 1024),
                    double(GPUDrivenRenderer::GeometryUploadBytes()) / 1024);
        if (ImGui::Checkbox("Automatic Nanite rendering", &mesh.Enabled))
        {
            GPUDrivenRenderer::SetEnabled(mesh.Enabled);
        }
        int mode = visibility.DebugMode == 2 ? 1 : mesh.Colors ? 2 : 0;
        if (ImGui::Combo("View", &mode, "Normal\0LOD View\0Meshlet colors\0"))
        {
            visibility.DebugMode = mode == 1 ? 2 : 0;
            mesh.Colors = mode == 2;
        }
        for (uint32_t lod = 0; lod < 8; ++lod)
        {
            if (lod)
            {
                ImGui::SameLine();
            }
            ImGui::ColorButton(("LOD" + to_string(lod)).c_str(), ImGui::ColorConvertU32ToFloat4(LODColors[lod]),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::Text("%u", lod);
        }
        ImGui::SliderInt("Force LOD (-1 = Auto)##Nanite", &visibility.ForceLOD, -1, 7);
        ImGui::Checkbox("Freeze culling camera (Hi-Z disabled)", &visibility.FreezeCamera);
        ImGui::Checkbox("LOD statistics (delayed GPU sample)", &visibility.DebugReadback);
        if (visibility.DebugReadback && GPUVisibility::HasStatisticsSample())
        {
            const auto& sample = GPUVisibility::LatestStatistics();
            ImGui::Text("Last completed sample: %u visible / %u triangles", sample.FinalVisible, sample.Triangles);
            for (uint32_t lod = 0; lod < 8; ++lod)
            {
                if (lod)
                {
                    ImGui::SameLine();
                }
                ImGui::Text("LOD%u:%u", lod, sample.LODCounts[lod]);
            }
        }
        else
        {
            ImGui::TextDisabled("LOD counts: sample not requested or not available yet");
        }
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
            for (const auto* source : Meshes(selected))
            {
                const auto& asset = source->VirtualGeometryLODs[options.SourceLOD];
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
                {
                    ImGui::TextWrapped("Hierarchy: %s (leaf rendering available)", asset->HierarchyWarning.c_str());
                }
                const auto& sourceStats = asset->Hierarchy.Stats;
                ImGui::Text("Source vertices %llu / triangles %llu", sourceStats.SourceVertices,
                            sourceStats.SourceTriangles);
                ImGui::Text("Clusters %u / hierarchy groups %u / LOD levels %u", sourceStats.TotalClusters,
                            sourceStats.TotalGroups, sourceStats.HierarchyLevels);
                ImGui::Text("Builder version %u / CPU reference version %u / production-capable preset",
                            VirtualGeometryConfig::BuilderVersion, asset->Hierarchy.Version);
                if (ImGui::BeginTable(("Hierarchy##" + source->MeshName + to_string(asset->Key)).c_str(), 4))
                {
                    ImGui::TableSetupColumn("Level");
                    ImGui::TableSetupColumn("Clusters");
                    ImGui::TableSetupColumn("Triangles");
                    ImGui::TableSetupColumn("Max error");
                    ImGui::TableHeadersRow();
                    for (size_t level = 0; level < asset->Hierarchy.Levels.size(); ++level)
                    {
                        const auto& item = asset->Hierarchy.Levels[level];
                        float error = 0;
                        for (const auto& cluster : asset->Hierarchy.Clusters)
                        {
                            if (cluster.Level == level)
                            {
                                error = max(error, cluster.LodError);
                            }
                        }
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
        uint32_t eligible = 0;
        uint32_t fallback = 0;
        for (uint32_t slot = 0; slot < GpuScene::Statistics().InstanceSlots; ++slot)
        {
            const auto* instance = GpuScene::GetInstance(slot);
            if (!instance || !(instance->Flags & 1u))
            {
                continue;
            }
            if (instance->Flags & 4u)
            {
                ++eligible;
            }
            else
            {
                ++fallback;
            }
        }
        ImGui::Text("Scene sections: GPU eligible %u / traditional %u", eligible, fallback);
        ImGui::TextDisabled("GPU eligible can still fall back on missing clusters or capacity overflow.");
    }

    void DrawViewport(ImDrawList* draw, const ImVec2& origin, const ImVec2& size, uint32_t selected)
    {
        auto& options = Settings();
        const auto& settings = MeshShaderRenderer::GetSettings();
        if (!options.HierarchyBounds && !options.Wireframe && !settings.MeshletBounds && !settings.ClusterBounds &&
            !settings.ConeAxes)
        {
            return;
        }
        const auto camera = Camera::GetCameraEntity();
        if (!Registry::IsAlive(camera))
        {
            return;
        }
        XMMATRIX view, projection;
        Camera::GetCameraMatrices(camera, view, projection);
        const XMMATRIX vp = view * projection;
        auto project = [&](XMVECTOR local, XMMATRIX world, ImVec2& point) {
            XMFLOAT4 clip;
            XMStoreFloat4(&clip, XMVector4Transform(XMVector4Transform(local, world), vp));
            if (clip.w <= 1e-5f || clip.z < 0 || clip.z > clip.w)
            {
                return false;
            }
            point = {origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x,
                     origin.y + (0.5f - clip.y / clip.w * 0.5f) * size.y};
            return true;
        };
        auto sphere = [&](const Meshlets::Bounds& bounds, XMMATRIX world, ImU32 color) {
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
                    {
                        draw->AddLine(previous, point, color);
                    }
                    previous = point;
                    have = valid;
                }
            }
        };
        draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
        uint32_t displayed = 0;
        if (settings.MeshletBounds || settings.ClusterBounds || settings.ConeAxes)
        {
            const auto& data = MeshletManager::AssetData();
            const ImU32 colors[] = {IM_COL32(150, 150, 150, 180), IM_COL32(40, 230, 230, 200),
                                    IM_COL32(200, 80, 240, 200),  IM_COL32(250, 60, 60, 200),
                                    IM_COL32(240, 220, 40, 200),  IM_COL32(250, 150, 40, 200)};
            for (const auto& record : MeshShaderRenderer::DebugRecords())
            {
                if (displayed >= uint32_t(options.Limit))
                {
                    break;
                }
                const auto* instance = GpuScene::GetInstance(record[0]);
                if (!instance || !(instance->Flags & 1u) || record[1] >= data.Bounds.size() ||
                    (options.SelectedOnly && instance->Entity != selected))
                {
                    continue;
                }
                ++displayed;
                const auto world = XMLoadFloat4x4(&instance->World);
                const auto& bounds = data.Bounds[record[1]];
                if (settings.MeshletBounds)
                {
                    sphere(bounds, world, colors[min(record[2], 5u)]);
                }
                if (settings.ClusterBounds && record[3] < data.Clusters.size() &&
                    record[1] == data.Clusters[record[3]].MeshletOffset)
                {
                    sphere(data.Clusters[record[3]].Bounds, world, IM_COL32(190, 190, 255, 200));
                }
                if (settings.ConeAxes && bounds.ConeCutoff < 1)
                {
                    ImVec2 start, end;
                    auto center = XMVectorSet(bounds.Center.X, bounds.Center.Y, bounds.Center.Z, 1);
                    auto tip = center +
                               XMVectorSet(bounds.ConeAxis.X, bounds.ConeAxis.Y, bounds.ConeAxis.Z, 0) * bounds.Radius;
                    if (project(center, world, start) && project(tip, world, end))
                    {
                        draw->AddLine(start, end, IM_COL32_WHITE, 2);
                    }
                }
            }
        }
        if ((options.HierarchyBounds || options.Wireframe) && Registry::IsAlive(selected) &&
            ComponentManager::HasComponent<TransformComponent>(selected))
        {
            const auto world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(selected).WorldMatrix);
            uint32_t previews = 0;
            for (const auto* source : Meshes(selected))
            {
                const auto& asset = source->VirtualGeometryLODs[options.SourceLOD];
                if (!asset || !asset->Error.empty())
                {
                    continue;
                }
                const auto& hierarchy = asset->Hierarchy;
                for (const auto& cluster : hierarchy.Clusters)
                {
                    if (previews >= uint32_t(options.Limit))
                    {
                        break;
                    }
                    const bool bounds = options.HierarchyBounds && cluster.Level == uint32_t(options.HierarchyLevel);
                    const bool wire = options.Wireframe && cluster.Level == 0;
                    if (!bounds && !wire)
                    {
                        continue;
                    }
                    ++previews;
                    const auto color = LODColors[min(cluster.Level, 7u)];
                    if (bounds)
                    {
                        sphere({{cluster.Bounds.Center[0], cluster.Bounds.Center[1], cluster.Bounds.Center[2]},
                                cluster.Bounds.Radius,
                                {},
                                1},
                               world, color);
                    }
                    if (wire)
                    {
                        for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
                        {
                            ImVec2 points[3];
                            bool valid = true;
                            for (uint32_t i = 0; i < 3; ++i)
                            {
                                const auto& vertex =
                                    hierarchy
                                        .Vertices[cluster.VertexOffset +
                                                  hierarchy.TriangleIndices[cluster.TriangleOffset + triangle * 3 + i]];
                                valid &=
                                    project(XMVectorSet(vertex.Position[0], vertex.Position[1], vertex.Position[2], 1),
                                            world, points[i]);
                            }
                            if (valid)
                            {
                                for (uint32_t i = 0; i < 3; ++i)
                                {
                                    draw->AddLine(points[i], points[(i + 1) % 3], color);
                                }
                            }
                        }
                    }
                }
            }
            displayed += previews;
        }
        draw->AddText(
            ImVec2(origin.x + 8, origin.y + 8), IM_COL32_WHITE,
            ("Nanite overlay: " + to_string(displayed) + " clusters (limit " + to_string(options.Limit) + ")").c_str());
        draw->PopClipRect();
    }
} // 名前空間 VirtualGeometryDebug の終わり。
