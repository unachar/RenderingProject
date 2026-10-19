#include "virtualgeometry/virtualgeometryselection.h"
#include "virtualgeometry_selection_fixture.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void TestVirtualGeometrySelection();

void ExpectCut(const VirtualGeometrySelection::Result& result, std::vector<uint32_t> expected)
{
    auto actual = result.Clusters;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    if (!result.Error.empty() || actual != expected)
    {
        throw std::runtime_error("Incorrect orthographic shadow hierarchy cut: " + result.Error);
    }
}

int main()
{
    try
    {
        TestVirtualGeometrySelection();
        auto graph = MakeSharedVirtualGeometryGraph();
        VirtualGeometrySelection::View view;
        view.Orthographic = true;
        view.ProjectionScalePixels = 0.3f;
        VirtualGeometrySelection::Settings settings;
        ExpectCut(VirtualGeometrySelection::Select(graph, view, settings), {2, 3});
        view.Camera = {0, 0, 10000};
        ExpectCut(VirtualGeometrySelection::Select(graph, view, settings), {2, 3});
        view.ProjectionScalePixels = 0.1f;
        ExpectCut(VirtualGeometrySelection::Select(graph, view, settings), {4, 5});
        view.ProjectionScalePixels = 1.0f;
        ExpectCut(VirtualGeometrySelection::Select(graph, view, settings), {0, 1});
        view.ProjectionScalePixels = 0.1f;
        view.WorldScale = 10.0f;
        ExpectCut(VirtualGeometrySelection::Select(graph, view, settings), {0, 1});
        settings.ForceHierarchyLevel = 1;
        ExpectCut(VirtualGeometrySelection::Select(graph, view, settings), {2, 3});
        settings.ForceHierarchyLevel = -1;
        view.WorldScale = 1.0f;
        view.ProjectionScalePixels = 1.0f;
        const std::vector<uint32_t> pages{0, 1, 2, 3, 4, 5};
        std::vector<uint8_t> resident{0, 0, 1, 1, 1, 1};
        const auto fallback = VirtualGeometrySelection::SelectResident(graph, view, settings, pages, resident);
        ExpectCut(fallback, {2, 3});
        auto requests = fallback.RequestedPages;
        std::sort(requests.begin(), requests.end());
        if (requests != std::vector<uint32_t>{0, 1})
        {
            throw std::runtime_error("Shadow refinement did not request missing detail pages exactly once");
        }
        resident[2] = 0;
        ExpectCut(VirtualGeometrySelection::SelectResident(graph, view, settings, pages, resident), {3, 4});
        resident[4] = 0;
        if (VirtualGeometrySelection::SelectResident(graph, view, settings, pages, resident).Error.empty())
        {
            throw std::runtime_error("A missing shadow root must request the legacy instance fallback");
        }
        std::cout << "Orthographic shadow and perspective hierarchy tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
