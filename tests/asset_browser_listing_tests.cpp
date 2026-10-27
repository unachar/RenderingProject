#include "editor/assetbrowserlisting.h"
#include "assets/thumbnailmanifest.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

static void FinishScan(AssetBrowserListing& listing, const std::filesystem::path& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        listing.Update(path, false, 0.0);
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (listing.Loading());
}

int main()
{
    const auto root = std::filesystem::absolute("build/asset-browser-tests/fixture");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Folder");
    std::ofstream(root / "visible.obj") << "mesh";
    std::ofstream(root / "visible.obj.meta") << "metadata";
    std::ofstream(root / "hidden.mtl") << "material";
    std::filesystem::create_directories(root / "__thumbnail_validation_test");
    for (int index = 0; index < 1500; ++index)
    {
        std::ofstream(root / ("file_" + std::to_string(index) + ".txt"));
    }

    AssetBrowserListing listing;
    listing.Update(root, false, 0.0);
    FinishScan(listing, root);
    assert(!listing.Error());
    assert(listing.Items().size() == 1502);
    assert(listing.Items().front().IsDirectory);
    assert(listing.Items().front().Name == "Folder");
    const auto findMesh = [&]() -> const AssetBrowserItem& {
        return *std::find_if(listing.Items().begin(), listing.Items().end(), [](const auto& item) {
            return item.Name == "visible.obj";
        });
    };
    const auto metadataTime = findMesh().MetadataTime;
    std::filesystem::last_write_time(root / "visible.obj.meta", metadataTime + std::chrono::seconds(2));
    listing.Update(root, true, 0.0);
    FinishScan(listing, root);
    assert(findMesh().MetadataTime != metadataTime);
    for (const auto& item : listing.Items())
    {
        assert(item.Name == item.path().filename().generic_string());
        assert(item.Key == item.path().generic_string());
    }

    std::ofstream(root / "new.txt");
    listing.Update(root, false, 0.1);
    assert(!listing.Loading());
    assert(listing.Items().size() == 1502);
    listing.Update(root, true, 0.1);
    FinishScan(listing, root);
    assert(listing.Items().size() == 1503);

    // Navigation while a scan is in flight must never publish the previous directory.
    listing.Update(root, true, 0.2);
    listing.Update(root / "Folder", false, 0.2);
    assert(listing.Items().empty());
    FinishScan(listing, root / "Folder");
    assert(listing.Items().empty());
    assert(!listing.Error());

    listing.Update(root / "missing", false, 0.2);
    FinishScan(listing, root / "missing");
    assert(listing.Error());
    listing.Update(root / "visible.obj", false, 0.2);
    FinishScan(listing, root / "visible.obj");
    assert(listing.Error() == std::errc::not_a_directory);
    listing.Update(root, false, 0.2);
    FinishScan(listing, root);
    assert(!listing.Error());
    assert(listing.Items().size() == 1503);
    ThumbnailManifest manifest;
    manifest.BaseKey = 123;
    manifest.Key = 456;
    manifest.SourcePath = root / "visible.obj";
    manifest.Dependencies.push_back({root / "external material.mtl", 789});
    std::stringstream serialized;
    manifest.Write(serialized);
    ThumbnailManifest loaded;
    assert(loaded.Read(serialized));
    assert(loaded.Matches(123, manifest.SourcePath, [](const auto&) {
        return uint64_t{789};
    }));
    assert(!loaded.Matches(124, manifest.SourcePath, [](const auto&) {
        return uint64_t{789};
    }));
    assert(!loaded.Matches(123, manifest.SourcePath, [](const auto&) {
        return uint64_t{790};
    }));
    assert(loaded.Dependencies == manifest.Dependencies);
    assert(!loaded.Matches(123, root / "Moved.obj", [](const auto&) {
        return uint64_t{789};
    }));
    std::stringstream oldManifest("456");
    assert(!loaded.Read(oldManifest));
    std::stringstream truncated("thumbnail-v2 123 456 1");
    assert(!loaded.Read(truncated));
    std::filesystem::remove_all(root);
    std::cout << "Asset browser listing tests passed\n";
}
