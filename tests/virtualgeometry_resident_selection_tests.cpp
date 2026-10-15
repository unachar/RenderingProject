#include "virtualgeometry/virtualgeometryselection.h"
#include "virtualgeometry_selection_fixture.h"
#include <stdexcept>

void TestVirtualGeometryResidentSelection()
{
    auto asset = MakeSharedVirtualGeometryGraph();
    VirtualGeometrySelection::Settings settings;
    settings.ForceHierarchyLevel = 0;
    std::vector<uint32_t> pages{0, 1, 2, 3, 4, 5};
    for (uint32_t mask = 0; mask < 16; ++mask)
    {
        std::vector<uint8_t> resident(6, 1);
        for (uint32_t page = 0; page < 4; ++page)
        {
            resident[page] = uint8_t((mask >> page) & 1);
        }
        auto result = VirtualGeometrySelection::SelectResident(asset, {}, settings, pages, resident);
        if (!result.Error.empty())
        {
            throw std::runtime_error(result.Error);
        }
        std::vector<uint32_t> expected;
        if (resident[2] && resident[3] && resident[0] && resident[1])
        {
            expected = {0, 1};
        }
        else
        {
            expected.push_back(resident[2] ? 2u : 4u);
            expected.push_back(resident[3] ? 3u : 5u);
            std::sort(expected.begin(), expected.end());
        }
        if (result.Clusters != expected)
        {
            throw std::runtime_error("Shared-parent resident cut lost or duplicated coverage");
        }
        for (uint32_t cluster : result.Clusters)
        {
            if (!resident[pages[cluster]])
            {
                throw std::runtime_error("Nonresident cluster selected");
            }
        }
        auto requests = result.RequestedPages;
        std::sort(requests.begin(), requests.end());
        if (std::adjacent_find(requests.begin(), requests.end()) != requests.end())
        {
            throw std::runtime_error("Duplicate Page requests");
        }
    }
    std::vector<uint8_t> resident(6, 1);
    auto full = VirtualGeometrySelection::SelectResident(asset, {}, settings, pages, resident);
    if (full.Clusters != VirtualGeometrySelection::Select(asset, {}, settings).Clusters)
    {
        throw std::runtime_error("Resident promotion differs from Phase 6");
    }
    resident[4] = 0;
    if (VirtualGeometrySelection::SelectResident(asset, {}, settings, pages, resident).Error.empty())
    {
        throw std::runtime_error("Missing root did not fail the entire instance");
    }
}
