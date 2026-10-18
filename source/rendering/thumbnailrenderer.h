#pragma once
#include "materialasset.h"
#include "staticmodel.h"
#include "editordescriptors.h"
#include "thumbnailpreviewcontext.h"
#include <DirectXTex.h>
#include <future>

struct ThumbnailSourceImage
{
    std::filesystem::path Path;
    ComPtr<ID3D12Resource> Borrowed;
    int BorrowedDescriptor = -1;
    std::unique_ptr<DirectX::ScratchImage> Decoded;
    std::vector<uint8_t> Encoded;
    std::string Identity;

    std::string IdentityKey() const
    {
        return Identity.empty() ? Path.lexically_normal().generic_string() : Identity;
    }
};

struct ThumbnailPrepared
{
    AssetHandle Handle = 0;
    AssetType Type = AssetType::Unknown;
    MaterialAssetData Material;
    std::array<ThumbnailSourceImage, 5> Textures;
    std::vector<StaticModelVertex> Geometry;

    struct MeshPart
    {
        MaterialAssetData Material;
        std::array<ThumbnailSourceImage, 5> Textures;
        UINT FirstVertex = 0;
        UINT VertexCount = 0;
    };

    std::vector<MeshPart> MeshParts;
    std::vector<std::pair<std::filesystem::path, uint64_t>> SourceDependencies;
    uint64_t Key = 0;
    UINT Size = 128;
    std::filesystem::path SourcePath;
    bool UseDiskCache = true;
    bool BypassDiskCache = false;
    bool Cached = false;
    bool AllowBorrow = true;
    std::string Error;
};

struct ThumbnailGpuImage
{
    ComPtr<ID3D12Resource> Resource;
    std::shared_ptr<EditorDescriptorLease> Descriptor;
    D3D12_GPU_DESCRIPTOR_HANDLE BorrowedDescriptor{};
    ComPtr<ID3D12Resource> Upload;
    std::shared_ptr<ThumbnailPreviewContext> Preview;
    std::vector<std::shared_ptr<ThumbnailGpuImage>> Dependencies;
    ComPtr<ID3D12Resource> Readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
    uint64_t SubmissionFence = 0;
    uint64_t Bytes = 0;
    UINT Width = 0;
    UINT Height = 0;

    D3D12_GPU_DESCRIPTOR_HANDLE Srv() const
    {
        return Descriptor ? Descriptor->Gpu : BorrowedDescriptor;
    }
};

class ThumbnailRenderer
{
  public:
    static ThumbnailPrepared Prepare(ThumbnailPrepared input);
    static std::shared_ptr<ThumbnailGpuImage> Generate(ThumbnailPrepared& input);
    static uint64_t SourceRevision(const std::filesystem::path& path);
    static std::filesystem::path CachePath(AssetHandle handle);
    static std::future<void> SaveCache(const ThumbnailPrepared& input, const std::shared_ptr<ThumbnailGpuImage>& image);
    static bool BeginReadback(const std::shared_ptr<ThumbnailGpuImage>& image);
    static void Collect();
    static uint64_t InFlightBytes();
    static void Shutdown();
};
