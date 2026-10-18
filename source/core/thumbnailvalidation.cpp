#include "pch.h"
#include "thumbnailvalidation.h"
#include "assetthumbnailmanager.h"
#include "materialmanager.h"
#include "rendercapture.h"
#include "graphicslog.h"
#include "graphicsdevice.h"
#include "world.h"
#include "imguimanager.h"
#include "renderprofiler.h"
#include "texturemanager.h"
#include <DirectXTex.h>
#include <fstream>

namespace
{
std::filesystem::path root;
std::vector<std::filesystem::path> files;
std::vector<AssetHandle> handles;
MaterialAssetData material;
AssetHandle surface = 0, texture = 0, mesh = 0, broken = 0;
uint64_t materialRevision = 0;
uint64_t colorHash = 0;
uint64_t cacheHits = 0;
unsigned int stage = 0, failures = 0;
bool complete = false;
size_t entityCount = 0;
UINT renderedBuffer = 0;
XMFLOAT2 surfaceCell{};
bool surfaceSelected = false;
UINT inputStep = 0, visibleCells = 0, maximumVisibleCells = 0;
ComPtr<ID3D12Resource> existingTexture;

void Check(bool condition, const char* name)
{
    WriteGraphicsLog("Thumbnail validation: %s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
    {
        ++failures;
    }
}

bool MakeImage(const std::filesystem::path& path, UINT width, UINT height, uint32_t color)
{
    DirectX::ScratchImage image;
    if (FAILED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1)))
    {
        return false;
    }
    const auto* pixels = image.GetImage(0, 0, 0);
    for (UINT row = 0; row < height; ++row)
    {
        auto* line = reinterpret_cast<uint32_t*>(pixels->pixels + row * pixels->rowPitch);
        for (UINT column = 0; column < width; ++column)
        {
            line[column] = color;
        }
    }
    if (path.extension() == ".dds")
    {
        return SUCCEEDED(DirectX::SaveToDDSFile(*pixels, DirectX::DDS_FLAGS_NONE, path.c_str()));
    }
    if (path.extension() == ".tga")
    {
        return SUCCEEDED(DirectX::SaveToTGAFile(*pixels, path.c_str()));
    }
    return SUCCEEDED(DirectX::SaveToWICFile(
        *pixels, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(path.extension() == ".jpg" ? DirectX::WIC_CODEC_JPEG : DirectX::WIC_CODEC_PNG),
        path.c_str()));
}

AssetHandle Register(const std::filesystem::path& path)
{
    std::string error;
    const auto handle = MaterialManager::Registry().Register(path, AssetRegistry::Classify(path), error);
    Check(handle != 0, "fixture GUID registration");
    handles.push_back(handle);
    files.push_back(path);
    files.emplace_back(path.wstring() + L".meta");
    return handle;
}

uint64_t HashImage(const ThumbnailHandle& thumbnail)
{
    RenderCapture::TextureData pixels;
    if (!thumbnail.Image || !RenderCapture::ReadTexture(thumbnail.Image->Resource.Get(), pixels))
    {
        return 0;
    }
    uint64_t hash = 1469598103934665603ull;
    for (auto byte : pixels.Bytes)
    {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}

void CaptureBrowser()
{
    RenderCapture::TextureData pixels;
    auto* resource = GraphicsDevice::GetBackBuffer(renderedBuffer);
    if (!resource || !RenderCapture::ReadTextureMip(resource, pixels, 0, D3D12_RESOURCE_STATE_PRESENT))
    {
        return;
    }
    DirectX::ScratchImage image;
    if (FAILED(image.Initialize2D(pixels.Format, pixels.Width, pixels.Height, 1, 1)))
    {
        return;
    }
    const auto* destination = image.GetImage(0, 0, 0);
    for (UINT row = 0; row < pixels.Height; ++row)
    {
        memcpy(destination->pixels + row * destination->rowPitch, pixels.Bytes.data() + row * pixels.RowPitch,
               destination->rowPitch);
    }
    Check(SUCCEEDED(DirectX::SaveToWICFile(*destination, DirectX::WIC_FLAGS_NONE,
                                           DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                           L"build/thumbnail-tests/browser.png")),
          "capture browser grid");
}
} // namespace

bool ThumbnailValidation::Enabled()
{
    char value[8]{};
    return GetEnvironmentVariableA("DX12_THUMBNAIL_TEST", value, sizeof(value)) > 0 && atoi(value) != 0;
}

void ThumbnailValidation::BeforeFrame(uint64_t frame)
{
    renderedBuffer = GraphicsDevice::GetFrameIndex();
    if (frame != 0)
    {
        return;
    }
    root = std::filesystem::path("asset") / ("__thumbnail_validation_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    std::filesystem::create_directories("build/thumbnail-tests");
    entityCount = Registry::GetEntities().size();
    for (const auto& name : {"Wide.png", "Square.jpg", "Square.dds", "Tall.tga"})
    {
        const auto path = root / name;
        Check(MakeImage(path, name == std::string("Tall.tga") ? 64 : 192,
                        name == std::string("Tall.tga")   ? 192
                        : name == std::string("Wide.png") ? 108
                                                          : 192,
                        0xff3030e0),
              "write image fixture");
        const auto handle = Register(path);
        if (name == std::string("Wide.png"))
        {
            texture = handle;
        }
    }
    material.Textures[0] = texture;
    // Fixture setup may synchronously preload; thumbnail Request/Generate must reuse this resource.
    existingTexture = TextureManager::GetTextureResource(TextureManager::LoadTexture(root / "Wide.png"));
    material.Roughness = 0.25f;
    material.Metallic = 0.5f;
    std::string error;
    Check(MaterialSerializer::Save(root / "Surface.material", material, error), "write material fixture");
    surface = Register(root / "Surface.material");
    std::ofstream model(root / "Offset.obj");
    model
        << "mtllib Shared.mtl\nv 99 -1 0\nv 101 -1 0\nv 100 1 0\nv 100 0 2\nusemtl Red\nf 1 3 2\nf 1 2 4\nusemtl Blue\nf 2 3 4\nf 3 1 4\n";
    model.close();
    std::ofstream(root / "Shared.mtl") << "newmtl Red\nKd 0.9 0.1 0.1\nnewmtl Blue\nKd 0.1 0.1 0.9\n";
    files.push_back(root / "Shared.mtl");
    mesh = Register(root / "Offset.obj");
    std::ofstream(root / "Broken.png") << "invalid image";
    broken = Register(root / "Broken.png");
    RenderProfiler::SetWindowVisible(false);
    ImGuiManager::SetAssetBrowserDirectory(root, true);
    // The fixture also exercises row clipping and large-directory enumeration.
    for (UINT index = 0; index < 1000; ++index)
    {
        const auto path = root / ("Unused_" + std::to_string(index) + ".txt");
        std::ofstream(path) << "unsupported fallback";
        files.push_back(path);
    }
}

void ThumbnailValidation::AfterFrame(uint64_t frame)
{
    if (complete)
    {
        return;
    }
    const auto thumbnail = AssetThumbnailManager::Request(surface, ThumbnailPriority::Selected);
    if (stage == 0)
    {
        bool ready = true;
        for (auto handle : handles)
        {
            const auto item = AssetThumbnailManager::Request(handle);
            if (handle == broken)
            {
                ready = ready && item.State == ThumbnailState::Failed;
            }
            else
            {
                ready = ready && item.State == ThumbnailState::Ready;
            }
        }
        if (!ready)
        {
            return;
        }
        Check(HashImage(thumbnail) != 0, "material sphere renders");
        const auto wide = AssetThumbnailManager::Request(texture);
        Check(wide.Image->Width * 9 == wide.Image->Height * 16, "wide texture keeps aspect ratio");
        Check(wide.Image->Resource == existingTexture && !wide.Image->Descriptor,
              "loaded texture resource and SRV reused");
        const auto meshThumbnail = AssetThumbnailManager::Request(mesh);
        Check(HashImage(meshThumbnail) != 0, "offset mesh renders with camera fit");
        RenderCapture::SaveTexturePng(thumbnail.Image->Resource.Get(), L"build/thumbnail-tests/material.png");
        RenderCapture::SaveTexturePng(meshThumbnail.Image->Resource.Get(), L"build/thumbnail-tests/mesh.png");
        CaptureBrowser();
        colorHash = HashImage(thumbnail);
        materialRevision = thumbnail.Revision;
        material.BaseColor = {0.2f, 0.9f, 0.3f, 1.0f};
        std::string error;
        Check(MaterialSerializer::Save(root / "Surface.material", material, error), "save changed material");
        AssetThumbnailManager::MarkDirty(surface);
        ++stage;
    }
    else if (stage == 1 && thumbnail.State == ThumbnailState::Ready && thumbnail.Revision != materialRevision)
    {
        Check(HashImage(thumbnail) != colorHash, "material edit updates thumbnail");
        colorHash = HashImage(thumbnail);
        materialRevision = thumbnail.Revision;
        Check(MakeImage(root / "Wide.png", 192, 108, 0xff30e030), "replace source texture");
        AssetThumbnailManager::MarkDirty(texture);
        ++stage;
    }
    else if (stage == 2 && thumbnail.State == ThumbnailState::Ready && thumbnail.Revision != materialRevision &&
             AssetThumbnailManager::Request(texture).State == ThumbnailState::Ready)
    {
        Check(HashImage(thumbnail) != colorHash, "texture dependency updates material");
        const auto before = AssetThumbnailManager::Request(texture);
        std::string error;
        Check(MaterialManager::Registry().Move(texture, root / "Renamed.png", error), "rename/move keeps GUID");
        files.push_back(root / "Renamed.png");
        files.emplace_back((root / "Renamed.png").wstring() + L".meta");
        Check(AssetThumbnailManager::Request(texture).Image == before.Image, "rename preserves thumbnail identity");
        cacheHits = AssetThumbnailManager::CacheHits();
        ++stage;
    }
    else if (stage == 3)
    {
        uint64_t savedKey = 0;
        std::ifstream manifest(ThumbnailRenderer::CachePath(surface).string() + ".key");
        if (!(manifest >> savedKey) || savedKey != AssetThumbnailManager::LastCacheKey(surface))
        {
            return;
        }
        AssetThumbnailManager::Remove(surface);
        AssetThumbnailManager::Request(surface);
        ++stage;
    }
    else if (stage == 4 && thumbnail.State == ThumbnailState::Ready)
    {
        Check(AssetThumbnailManager::CacheHits() > cacheHits, "disk cache reused after eviction");
        Check(Registry::GetEntities().size() == entityCount, "preview adds no world entities");
        Check(AssetThumbnailManager::MemoryBytes() <= AssetThumbnailManager::Settings().MemoryBudget,
              "memory remains inside budget");
        const auto model = AssetThumbnailManager::Request(mesh);
        if (model.State != ThumbnailState::Ready)
        {
            return;
        }
        colorHash = HashImage(model);
        materialRevision = model.Revision;
        std::ofstream(root / "Shared.mtl") << "newmtl Red\nKd 0.1 0.9 0.1\nnewmtl Blue\nKd 0.9 0.8 0.1\n";
        ++stage;
    }
    else if (stage == 5)
    {
        const auto model = AssetThumbnailManager::Request(mesh);
        if (model.State != ThumbnailState::Ready || model.Revision == materialRevision)
        {
            return;
        }
        Check(HashImage(model) != colorHash, "actual external MTL change invalidates mesh");
        ++stage;
    }
    else if (stage == 6 && inputStep >= 2 && surfaceSelected)
    {
        Check(true, "thumbnail cell selects material");
        inputStep = 0;
        ++stage;
    }
    else if (stage == 7 && inputStep >= 16)
    {
        Check(maximumVisibleCells > 0 && maximumVisibleCells < 64, "1000 asset scroll clips offscreen UI");
        AssetThumbnailManager::Settings().Size = 64;
        ++stage;
    }
    else if (stage == 8 && thumbnail.State == ThumbnailState::Ready && thumbnail.Image->Width == 64)
    {
        Check(true, "thumbnail size setting regenerates preview");
        AssetThumbnailManager::Settings().Size = 128;
        ++stage;
    }
    else if (stage == 9 && thumbnail.State == ThumbnailState::Ready && thumbnail.Image->Width == 128)
    {
#ifdef _DEBUG
        Check(RenderCapture::ReportGraphicsErrors() == 0, "DX12 debug layer has no errors");
#else
        WriteGraphicsLog("Thumbnail validation: Release debug layer disabled; debug-layer checks run in Debug\n");
#endif
        complete = true;
    }
    if (frame == 299 && !complete)
    {
        Check(false, "thumbnail sequence completes");
    }
}

void ThumbnailValidation::BeforeEditorFrame()
{
    if (!Enabled())
    {
        return;
    }
    visibleCells = 0;
    if (stage == 6 && inputStep < 2)
    {
        auto& input = ImGui::GetIO();
        input.AddMousePosEvent(surfaceCell.x, surfaceCell.y);
        input.AddMouseButtonEvent(ImGuiMouseButton_Left, inputStep == 0);
        ++inputStep;
    }
    else if (stage == 7 && inputStep < 16)
    {
        auto& input = ImGui::GetIO();
        input.AddMousePosEvent(surfaceCell.x, surfaceCell.y);
        input.AddMouseWheelEvent(0, inputStep < 8 ? -8.0f : 8.0f);
        ++inputStep;
    }
}

void ThumbnailValidation::CaptureAssetCell(const char* fileName, float x, float y, bool selected)
{
    if (!Enabled())
    {
        return;
    }
    maximumVisibleCells = std::max(maximumVisibleCells, ++visibleCells);
    if (std::string(fileName) == "Surface.material")
    {
        surfaceCell = {x + 48, y + 48};
        surfaceSelected = selected;
    }
}

bool ThumbnailValidation::Finish()
{
    Check(complete, "all thumbnail stages completed");
    WriteGraphicsLog("Thumbnail validation failures = %u\n", failures);
    for (auto handle : handles)
    {
        AssetThumbnailManager::Remove(handle);
    }
    existingTexture.Reset();
    std::error_code error;
    for (const auto& file : files)
    {
        std::filesystem::remove(file, error);
    }
    if (!root.empty())
    {
        std::filesystem::remove(root, error);
    }
    MaterialManager::Refresh();
    return complete && failures == 0;
}
