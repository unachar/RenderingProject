#include "virtualgeometryselection.h"
#include "virtualgeometrytemporal.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace VirtualGeometrySelection
{
namespace
{
constexpr uint32_t Invalid = VirtualGeometryConfig::InvalidIndex;

bool ValidSphere(const VirtualGeometrySphere& sphere)
{
    return std::isfinite(sphere.Radius) && sphere.Radius >= 0 &&
           std::all_of(sphere.Center.begin(), sphere.Center.end(), [](float value) {
               return std::isfinite(value);
           });
}

bool ValidateLinks(const VirtualGeometryAsset& asset)
{
    if (asset.Clusters.empty() || asset.RootClusters.empty())
    {
        return false;
    }
    std::vector<uint32_t> consumers(asset.Clusters.size(), Invalid);
    std::vector<uint32_t> producers(asset.Clusters.size(), Invalid);
    std::vector<bool> roots(asset.Clusters.size());
    for (uint32_t root : asset.RootClusters)
    {
        if (root >= roots.size() || roots[root])
        {
            return false;
        }
        roots[root] = true;
    }
    for (uint32_t index = 0; index < asset.Groups.size(); ++index)
    {
        const auto& group = asset.Groups[index];
        if (group.Level >= 31 || !group.ChildCount || !group.ParentCount ||
            uint64_t(group.FirstChild) + group.ChildCount > asset.HierarchyIndices.size() ||
            uint64_t(group.FirstParent) + group.ParentCount > asset.HierarchyIndices.size() ||
            !ValidSphere(group.Bounds) || !std::isfinite(group.MaxError) || group.MaxError < 0 ||
            !std::isfinite(group.MinError) || group.MinError < 0 || group.MinError > group.MaxError)
        {
            return false;
        }
        for (uint32_t child = 0; child < group.ChildCount; ++child)
        {
            uint32_t cluster = asset.HierarchyIndices[group.FirstChild + child];
            if (cluster >= consumers.size() || consumers[cluster] != Invalid ||
                asset.Clusters[cluster].Level != group.Level || asset.Clusters[cluster].GroupIndex != index)
            {
                return false;
            }
            consumers[cluster] = index;
        }
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            uint32_t cluster = asset.HierarchyIndices[group.FirstParent + parent];
            if (cluster >= producers.size() || producers[cluster] != Invalid ||
                asset.Clusters[cluster].Level != group.Level + 1 ||
                asset.Clusters[cluster].GeneratingGroupIndex != index)
            {
                return false;
            }
            producers[cluster] = index;
        }
    }
    for (size_t index = 0; index < asset.Clusters.size(); ++index)
    {
        const auto& cluster = asset.Clusters[index];
        if (cluster.Level >= 32 || !std::isfinite(cluster.LodError) || cluster.LodError < 0 ||
            cluster.GroupIndex != consumers[index] || cluster.GeneratingGroupIndex != producers[index] ||
            roots[index] != (consumers[index] == Invalid) || (cluster.Level != 0 && producers[index] == Invalid))
        {
            return false;
        }
    }
    return true;
}
} // namespace

static TemporalResult SelectInternal(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                                     std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages,
                                     std::span<const uint8_t> previousRefine = {}, bool historyValid = false,
                                     float fraction = 0)
{
    auto fail = [](const char* error) {
        TemporalResult result;
        result.Error = error;
        return result;
    };
    bool streaming = !clusterPages.empty();
    if (historyValid && previousRefine.size() != asset.Groups.size())
    {
        return fail("Invalid LOD history size");
    }
    if (!std::isfinite(view.WorldScale) || view.WorldScale <= 0 || !std::isfinite(view.MinimumWorldScale) ||
        view.MinimumWorldScale <= 0 || view.MinimumWorldScale > view.WorldScale ||
        !std::isfinite(view.ProjectionScalePixels) || view.ProjectionScalePixels <= 0 ||
        !std::all_of(view.Camera.begin(), view.Camera.end(),
                     [](float value) {
                         return std::isfinite(value);
                     }) ||
        !std::isfinite(settings.TargetErrorPixels) || settings.TargetErrorPixels <= 0 ||
        settings.ForceHierarchyLevel < -1 || settings.ForceHierarchyLevel > 31)
    {
        return fail("Invalid hierarchy selection view/settings");
    }
    if (!ValidateLinks(asset))
    {
        return fail("Invalid hierarchy selection graph");
    }
    if (streaming)
    {
        if (clusterPages.size() != asset.Clusters.size())
        {
            return fail("Invalid Cluster to Page mapping");
        }
        for (uint32_t page : clusterPages)
        {
            if (page >= residentPages.size())
            {
                return fail("Page mapping outside Residency Table");
            }
        }
        for (uint32_t root : asset.RootClusters)
        {
            if (!residentPages[clusterPages[root]])
            {
                return fail("Missing resident root");
            }
        }
    }

    std::vector<uint32_t> order(asset.Groups.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t first, uint32_t second) {
        return asset.Groups[first].Level < asset.Groups[second].Level;
    });
    std::vector<bool> refine(asset.Groups.size());
    std::vector<uint8_t> desired(asset.Groups.size());
    for (uint32_t index : order)
    {
        const auto& group = asset.Groups[index];
        double errorPixels = double(group.MaxError) * view.WorldScale * view.ProjectionScalePixels;
        if (!view.Orthographic)
        {
            double distanceSquared = 0;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                double difference = double(view.Camera[axis]) - group.Bounds.Center[axis];
                distanceSquared += difference * difference;
            }
            // Minimum scale keeps distance conservative; maximum scale bounds error.
            const double distance =
                std::max((std::sqrt(distanceSquared) - group.Bounds.Radius) * view.MinimumWorldScale, 1e-6);
            errorPixels /= distance;
        }
        bool requested =
            settings.ForceHierarchyLevel >= 0
                ? group.Level >= uint32_t(settings.ForceHierarchyLevel)
                : VirtualGeometryTemporal::Refine(errorPixels, settings.TargetErrorPixels, fraction,
                                                  historyValid && previousRefine[index] != 0, historyValid);
        desired[index] = uint8_t(requested);
        refine[index] = refine[index] || requested;
        if (!refine[index])
        {
            continue;
        }
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            uint32_t cluster = asset.HierarchyIndices[group.FirstParent + parent];
            uint32_t consumer = asset.Clusters[cluster].GroupIndex;
            if (consumer != Invalid)
            {
                refine[consumer] = true;
            }
        }
    }

    std::vector<bool> active(asset.Clusters.size());
    for (uint32_t root : asset.RootClusters)
    {
        active[root] = true;
    }
    TemporalResult result;
    result.DesiredRefine = std::move(desired);
    std::vector<uint8_t> requested(residentPages.size());
    for (auto entry = order.rbegin(); entry != order.rend(); ++entry)
    {
        uint32_t index = *entry;
        if (!refine[index])
        {
            continue;
        }
        const auto& group = asset.Groups[index];
        bool parentsActive = true;
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            if (!active[asset.HierarchyIndices[group.FirstParent + parent]])
            {
                parentsActive = false;
            }
        }
        if (!parentsActive)
        {
            if (streaming)
            {
                continue;
            }
            return fail("Incomplete group parent coverage");
        }
        bool childrenResident = true;
        if (streaming)
        {
            for (uint32_t child = 0; child < group.ChildCount; ++child)
            {
                uint32_t page = clusterPages[asset.HierarchyIndices[group.FirstChild + child]];
                if (!residentPages[page])
                {
                    childrenResident = false;
                    if (!requested[page])
                    {
                        requested[page] = 1;
                        result.RequestedPages.push_back(page);
                    }
                }
            }
        }
        if (!childrenResident)
        {
            continue;
        }
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            active[asset.HierarchyIndices[group.FirstParent + parent]] = false;
        }
        for (uint32_t child = 0; child < group.ChildCount; ++child)
        {
            active[asset.HierarchyIndices[group.FirstChild + child]] = true;
        }
    }
    for (uint32_t cluster = 0; cluster < active.size(); ++cluster)
    {
        if (active[cluster])
        {
            result.Clusters.push_back(cluster);
        }
    }
    return result;
}

Result Select(const VirtualGeometryAsset& asset, const View& view, const Settings& settings)
{
    return SelectInternal(asset, view, settings, {}, {});
}

TemporalResult SelectResidentTemporal(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                                      std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages,
                                      std::span<const uint8_t> previousRefine, bool historyValid, float fraction)
{
    if (clusterPages.empty())
    {
        TemporalResult result;
        result.Error = "Empty Cluster to Page mapping";
        return result;
    }
    return SelectInternal(asset, view, settings, clusterPages, residentPages, previousRefine, historyValid, fraction);
}

ResidentResult SelectResident(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                              std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages)
{
    if (clusterPages.empty())
    {
        ResidentResult result;
        result.Error = "Empty Cluster to Page mapping";
        return result;
    }
    return SelectInternal(asset, view, settings, clusterPages, residentPages);
}
} // namespace VirtualGeometrySelection
