#include "pch.h"
#include "memoryallocator.h"
#include "thumbnailrenderer.h"
#include "thumbnailmeshio.h"
#include "graphicsdevice.h"
#include "frameconstants.h"
#include "texturemanager.h"
#include "materialmanager.h"
#include "assets/noiseasset.h"
#include "assets/thumbnailcache.h"
#include "assets/thumbnailmanifest.h"
#include <cimport.h>
#include <postprocess.h>
#include <scene.h>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace
{
    struct InFlightImage
    {
        uint64_t Fence;
        std::shared_ptr<ThumbnailGpuImage> Image;
    };

    std::vector<InFlightImage> inFlight;

    void Retain(const std::shared_ptr<ThumbnailGpuImage>& image)
    {
        inFlight.push_back({GraphicsDevice::GetPendingSubmissionFence(), image});
    }

    uint64_t Combine(uint64_t hash, uint64_t value)
    {
        return (hash ^ value) * 1099511628211ull;
    }

    bool Decode(ThumbnailSourceImage& source, UINT maximumSize, std::string& error)
    {
        if (source.Borrowed || source.Decoded || (source.Path.empty() && source.Encoded.empty()))
        {
            return true;
        }
        DirectX::ScratchImage image;
        DirectX::TexMetadata metadata;
        auto extension = source.Path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        HRESULT result = E_FAIL;
        if (!source.Encoded.empty())
        {
            result = DirectX::LoadFromWICMemory(source.Encoded.data(), source.Encoded.size(), DirectX::WIC_FLAGS_NONE,
                                                &metadata, image);
            if (FAILED(result))
            {
                result = DirectX::LoadFromDDSMemory(source.Encoded.data(), source.Encoded.size(),
                                                    DirectX::DDS_FLAGS_NONE, &metadata, image);
            }
            if (FAILED(result))
            {
                result = DirectX::LoadFromTGAMemory(source.Encoded.data(), source.Encoded.size(), &metadata, image);
            }
        }
        else if (extension == ".dds")
        {
            result = DirectX::LoadFromDDSFile(source.Path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
        }
        else if (extension == ".tga")
        {
            result = DirectX::LoadFromTGAFile(source.Path.c_str(), &metadata, image);
        }
        else if (extension == ".hdr")
        {
            result = DirectX::LoadFromHDRFile(source.Path.c_str(), &metadata, image);
        }
        else
        {
            result = DirectX::LoadFromWICFile(source.Path.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, image);
        }
        if (FAILED(result) || metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 ||
            metadata.IsCubemap())
        {
            error = "Unsupported or unreadable 2D texture: " + source.Path.generic_string();
            return false;
        }
        DirectX::ScratchImage converted;
        if (DirectX::IsCompressed(metadata.format))
        {
            result = DirectX::Decompress(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, converted);
        }
        else if (metadata.format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            result = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                      0, converted);
        }
        else
        {
            result = S_OK;
        }
        if (FAILED(result))
        {
            error = "Unsupported texture format";
            return false;
        }
        const auto* pixels = converted.GetImageCount() ? converted.GetImage(0, 0, 0) : image.GetImage(0, 0, 0);
        const float scale = std::min(1.0f, static_cast<float>(maximumSize) /
                                               static_cast<float>(std::max(pixels->width, pixels->height)));
        source.Decoded = std::make_unique<DirectX::ScratchImage>();
        result = DirectX::Resize(*pixels, std::max<size_t>(1, static_cast<size_t>(pixels->width * scale)),
                                 std::max<size_t>(1, static_cast<size_t>(pixels->height * scale)),
                                 DirectX::TEX_FILTER_DEFAULT, *source.Decoded);
        if (FAILED(result))
        {
            error = "Thumbnail resize failed";
            return false;
        }
        return true;
    }

    ThumbnailPrepared::MeshPart ReadMeshMaterial(const aiScene& scene, const aiMesh& mesh,
                                                 const std::filesystem::path& modelPath)
    {
        ThumbnailPrepared::MeshPart part;
        if (mesh.mMaterialIndex >= scene.mNumMaterials)
        {
            return part;
        }
        const auto& source = *scene.mMaterials[mesh.mMaterialIndex];
        aiColor4D color(1, 1, 1, 1);
        if (source.Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
        {
            source.Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
        part.Material.BaseColor = {color.r, color.g, color.b, color.a};
        source.Get(AI_MATKEY_METALLIC_FACTOR, part.Material.Metallic);
        source.Get(AI_MATKEY_ROUGHNESS_FACTOR, part.Material.Roughness);
        float opacity = 1;
        source.Get(AI_MATKEY_OPACITY, opacity);
        part.Material.BaseColor[3] *= opacity;
        if (part.Material.BaseColor[3] < 1)
        {
            part.Material.Surface = MaterialSurface::Transparent;
        }
        int twoSided = 0;
        source.Get(AI_MATKEY_TWOSIDED, twoSided);
        if (twoSided)
        {
            part.Material.Cull = MaterialCull::None;
        }
        const aiTextureType types[] = {aiTextureType_BASE_COLOR,        aiTextureType_NORMALS,  aiTextureType_UNKNOWN,
                                       aiTextureType_AMBIENT_OCCLUSION, aiTextureType_EMISSIVE, aiTextureType_HEIGHT};
        for (size_t slot = 0; slot < part.Textures.size(); ++slot)
        {
            aiString reference;
            auto status = source.GetTexture(types[slot], 0, &reference);
            if (slot == 0 && status != AI_SUCCESS)
            {
                status = source.GetTexture(aiTextureType_DIFFUSE, 0, &reference);
            }
            if (status != AI_SUCCESS)
            {
                continue;
            }
            auto& texture = part.Textures[slot];
            if (const auto* embedded = scene.GetEmbeddedTexture(reference.C_Str()))
            {
                texture.Identity = modelPath.generic_string() + "|" + reference.C_Str();
                if (!embedded->mHeight)
                {
                    const auto* bytes = reinterpret_cast<const uint8_t*>(embedded->pcData);
                    texture.Encoded.assign(bytes, bytes + embedded->mWidth);
                }
                else
                {
                    DirectX::ScratchImage decoded;
                    if (SUCCEEDED(decoded.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, embedded->mWidth, embedded->mHeight,
                                                       1, 1)))
                    {
                        const auto* image = decoded.GetImage(0, 0, 0);
                        for (UINT row = 0; row < embedded->mHeight; ++row)
                        {
                            auto* destination = image->pixels + row * image->rowPitch;
                            for (UINT column = 0; column < embedded->mWidth; ++column)
                            {
                                const auto pixel = embedded->pcData[row * embedded->mWidth + column];
                                destination[column * 4] = pixel.r;
                                destination[column * 4 + 1] = pixel.g;
                                destination[column * 4 + 2] = pixel.b;
                                destination[column * 4 + 3] = pixel.a;
                            }
                        }
                        texture.Decoded = std::make_unique<DirectX::ScratchImage>();
                        const float scale = std::min(1.0f, 256.0f / std::max(embedded->mWidth, embedded->mHeight));
                        if (FAILED(DirectX::Resize(*image,
                                                   std::max<size_t>(1, static_cast<size_t>(embedded->mWidth * scale)),
                                                   std::max<size_t>(1, static_cast<size_t>(embedded->mHeight * scale)),
                                                   DirectX::TEX_FILTER_DEFAULT, *texture.Decoded)))
                        {
                            texture.Decoded.reset();
                        }
                    }
                }
            }
            else
            {
                texture.Path =
                    (modelPath.parent_path() / std::filesystem::u8path(reference.C_Str())).lexically_normal();
                texture.BorrowedDescriptor = TextureManager::FindLoadedTexture(texture.Path);
                if (texture.BorrowedDescriptor >= 0)
                {
                    texture.Borrowed = TextureManager::GetTextureResource(texture.BorrowedDescriptor);
                }
            }
        }
        return part;
    }

    void ReadMeshNode(const aiScene& scene, const aiNode& node, const aiMatrix4x4& parent, ThumbnailPrepared& input)
    {
        auto& vertices = input.Geometry;
        const auto transform = parent * node.mTransformation;
        auto normalTransform = aiMatrix3x3(transform);
        normalTransform.Inverse().Transpose();
        for (unsigned int index = 0; index < node.mNumMeshes; ++index)
        {
            const auto& mesh = *scene.mMeshes[node.mMeshes[index]];
            auto part = ReadMeshMaterial(scene, mesh, input.SourcePath);
            part.FirstVertex = static_cast<UINT>(vertices.size());
            aiColor4D color(1, 1, 1, 1);
            if (mesh.mMaterialIndex < scene.mNumMaterials)
            {
                scene.mMaterials[mesh.mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, color);
            }
            for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex)
            {
                const auto& face = mesh.mFaces[faceIndex];
                if (face.mNumIndices != 3)
                {
                    continue;
                }
                for (unsigned int corner = 0; corner < 3; ++corner)
                {
                    const auto vertexIndex = face.mIndices[corner];
                    const auto position = transform * mesh.mVertices[vertexIndex];
                    auto normal =
                        mesh.HasNormals() ? normalTransform * mesh.mNormals[vertexIndex] : aiVector3D(0, 1, 0);
                    normal.Normalize();
                    const auto uv = mesh.HasTextureCoords(0) ? mesh.mTextureCoords[0][vertexIndex] : aiVector3D();
                    vertices.push_back({{position.x, position.y, position.z},
                                        {normal.x, normal.y, normal.z},
                                        {uv.x, uv.y},
                                        {1, 1, 1, 10}});
                }
            }
            part.VertexCount = static_cast<UINT>(vertices.size()) - part.FirstVertex;
            if (part.VertexCount)
            {
                input.MeshParts.push_back(std::move(part));
            }
        }
        for (unsigned int child = 0; child < node.mNumChildren; ++child)
        {
            ReadMeshNode(scene, *node.mChildren[child], transform, input);
        }
    }

    std::shared_ptr<ThumbnailGpuImage> Upload(ThumbnailSourceImage& source, bool alias)
    {
        auto image = std::make_shared<ThumbnailGpuImage>();
        if (!source.Borrowed || alias)
        {
            image->Descriptor = EditorDescriptors::Allocate();
            if (!image->Descriptor)
            {
                return {};
            }
        }
        if (source.Borrowed)
        {
            const auto description = source.Borrowed->GetDesc();
            if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1 ||
                description.SampleDesc.Count != 1 || description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            {
                return {};
            }
            image->Resource = source.Borrowed;
            image->Width = static_cast<UINT>(description.Width);
            image->Height = description.Height;
            image->BorrowedDescriptor =
                CD3DX12_GPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(),
                                              source.BorrowedDescriptor, FrameConstants::GetCbvIncrementSize());
            if (!alias)
            {
                return image;
            }
        }
        else
        {
            if (!source.Decoded)
            {
                return {};
            }
            const auto* decoded = source.Decoded->GetImage(0, 0, 0);
            image->Width = static_cast<UINT>(decoded->width);
            image->Height = static_cast<UINT>(decoded->height);
            const auto description = CD3DX12_RESOURCE_DESC::Tex2D(decoded->format, image->Width, image->Height, 1, 1);
            auto* device = GraphicsDevice::GetDevice();
            // Through D3D12MA when the flag is on; committed fallback otherwise.
            if (ID3D12Resource* allocated = MemoryAllocator::CreateResourceAutoLifetime(
                    description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr))
            {
                image->Resource.Attach(allocated);
            }
            else
            {
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&image->Resource))))
                {
                    return {};
                }
            }
            const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto uploadSize = GetRequiredIntermediateSize(image->Resource.Get(), 0, 1);
            const auto uploadDescription = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
            if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&image->Upload))))
            {
                return {};
            }
            D3D12_SUBRESOURCE_DATA data{decoded->pixels, static_cast<LONG_PTR>(decoded->rowPitch),
                                        static_cast<LONG_PTR>(decoded->slicePitch)};
            image->Bytes = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes +
                           device->GetResourceAllocationInfo(0, 1, &uploadDescription).SizeInBytes;
            Retain(image); // コマンドがアップロード元や転送先を参照した後の失敗経路も含む。
            if (!UpdateSubresources(GraphicsDevice::GetCommandList(), image->Resource.Get(), image->Upload.Get(), 0, 0,
                                    1, &data))
            {
                return {};
            }
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(image->Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            GraphicsDevice::GetCommandList()->ResourceBarrier(1, &barrier);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = image->Resource->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = image->Resource->GetDesc().MipLevels;
        if (source.Borrowed)
        {
            srv.Texture2D.MostDetailedMip = TextureManager::GetTextureResidentMip(source.BorrowedDescriptor);
            srv.Texture2D.MipLevels -= srv.Texture2D.MostDetailedMip;
            // シェーダー可視ヒープは CopyDescriptors のコピー元として読めない。
            GraphicsDevice::GetDevice()->CreateShaderResourceView(image->Resource.Get(), &srv, image->Descriptor->Cpu);
            TextureManager::TouchTexture(source.BorrowedDescriptor);
        }
        else
        {
            GraphicsDevice::GetDevice()->CreateShaderResourceView(image->Resource.Get(), &srv, image->Descriptor->Cpu);
        }
        image->SubmissionFence = GraphicsDevice::GetPendingSubmissionFence();
        return image;
    }
} // 名前空間

uint64_t ThumbnailRenderer::SourceRevision(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return 0;
    }
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return 0;
    }
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? 0 : Combine(static_cast<uint64_t>(time.time_since_epoch().count()), bytes);
}

std::filesystem::path ThumbnailRenderer::CachePath(AssetHandle handle)
{
    return std::filesystem::path("EditorCache/Thumbnails") / ThumbnailCacheFileName(handle);
}

ThumbnailPrepared ThumbnailRenderer::Prepare(ThumbnailPrepared input)
{
    if (!input.Error.empty())
    {
        return input;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    struct ComScope
    {
        bool Owned;

        ~ComScope()
        {
            if (Owned)
            {
                CoUninitialize();
            }
        }
    } scope{SUCCEEDED(com)};

    const auto cachePath = CachePath(input.Handle);
    input.BaseKey = Combine(Combine(input.Key, ThumbnailGeneratorVersion), input.Size);
    const auto loadCachedImage = [&]() {
        auto cached = std::make_unique<DirectX::ScratchImage>();
        DirectX::TexMetadata metadata;
        if (SUCCEEDED(DirectX::LoadFromDDSFile(cachePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, *cached)) &&
            metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE2D && metadata.arraySize == 1 &&
            !metadata.IsCubemap() && metadata.width <= input.Size && metadata.height <= input.Size)
        {
            input.Cached = true;
            input.Textures[0].Borrowed.Reset();
            input.Textures[0].Decoded = std::move(cached);
            return true;
        }
        return false;
    };
    if (input.Type == AssetType::Mesh && input.UseDiskCache && !input.BypassDiskCache)
    {
        std::ifstream stream(cachePath.wstring() + L".key");
        ThumbnailManifest manifest;
        if (manifest.Read(stream) &&
            manifest.Matches(input.BaseKey, input.SourcePath,
                             [](const auto& path) {
                                 return SourceRevision(path) ^ SourceRevision(path.wstring() + L".meta");
                             }) &&
            loadCachedImage())
        {
            input.Key = manifest.Key;
            input.SourceDependencies = std::move(manifest.Dependencies);
            return input;
        }
    }

    if (input.Type == AssetType::Mesh)
    {
        std::vector<std::filesystem::path> importedFiles;
        ThumbnailMeshIO io(importedFiles);
        auto callbacks = io.Callbacks();
        const auto utf8Path = input.SourcePath.generic_u8string();
        const auto* scene = aiImportFileEx(std::string(utf8Path.begin(), utf8Path.end()).c_str(),
                                           aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                                               aiProcess_ConvertToLeftHanded | aiProcess_ValidateDataStructure,
                                           &callbacks);
        const std::unique_ptr<const aiScene, decltype(&aiReleaseImport)> owner(scene, aiReleaseImport);
        if (!scene || !scene->mRootNode)
        {
            input.Error = aiGetErrorString();
        }
        else
        {
            ReadMeshNode(*scene, *scene->mRootNode, aiMatrix4x4(), input);
        }
        if (input.Geometry.empty())
        {
            input.Error = "Mesh contains no triangles";
        }
        for (const auto& file : importedFiles)
        {
            if (file != input.SourcePath)
            {
                input.SourceDependencies.push_back(
                    {file, SourceRevision(file) ^ SourceRevision(file.wstring() + L".meta")});
            }
        }
    }
    for (auto& part : input.MeshParts)
    {
        for (auto& texture : part.Textures)
        {
            if (!texture.Path.empty())
            {
                input.SourceDependencies.push_back(
                    {texture.Path, SourceRevision(texture.Path) ^ SourceRevision(texture.Path.wstring() + L".meta")});
            }
        }
    }
    auto key = input.BaseKey;
    for (auto& texture : input.Textures)
    {
        key = Combine(key, SourceRevision(texture.Path));
        key = Combine(key, SourceRevision(texture.Path.wstring() + L".meta"));
    }
    for (const auto& dependency : input.SourceDependencies)
    {
        key = Combine(key, dependency.second);
    }
    input.Key = key;
    uint64_t cachedKey = 0;
    std::ifstream manifest(cachePath.string() + ".key");
    if (input.Type != AssetType::Mesh && input.UseDiskCache && !input.BypassDiskCache &&
        !(input.Type == AssetType::Texture && input.Textures[0].Borrowed) && manifest >> cachedKey && cachedKey == key)
    {
        if (loadCachedImage())
        {
            return input;
        }
    }
    if (input.Type == AssetType::Noise)
    {
        ProcStudio::TextureParams definition;
        if (!NoiseSerializer::Load(input.SourcePath, definition, input.Error))
        {
            return input;
        }
        input.Noise[0] = ProcStudio::MakeTextureGpuParams(definition);
        input.Noise[0].Width = 1;
    }
    for (size_t slot = 0; slot < input.Textures.size(); ++slot)
    {
        auto& texture = input.Textures[slot];
        if (AssetRegistry::Classify(texture.Path) == AssetType::Noise)
        {
            const int channel = slot == 0 ? 0 : slot == 1 ? 1 : slot == 5 ? 2 : -1;
            ProcStudio::TextureParams definition;
            if (channel < 0 || !NoiseSerializer::Load(texture.Path, definition, input.Error))
            {
                if (input.Error.empty())
                {
                    input.Error = "Unsupported .noise material slot";
                }
                return input;
            }
            input.Noise[channel] = ProcStudio::MakeTextureGpuParams(definition);
            input.Noise[channel].Width = 1;
        }
        else if (input.Type != AssetType::Mesh &&
                 !Decode(texture, input.Type == AssetType::Texture ? input.Size : 256, input.Error))
        {
            return input;
        }
    }
    for (auto& part : input.MeshParts)
    {
        for (auto& texture : part.Textures)
        {
            if (!Decode(texture, 256, input.Error))
            {
                return input;
            }
        }
    }
    return input;
}

ThumbnailPrepared ThumbnailRenderer::PreparePreview(ThumbnailPrepared input)
{
    try
    {
        input.Preview = std::make_shared<ThumbnailPreviewContext>();
        if (!input.PreviewDescriptor || !input.Preview->Initialize(input.Size, input.PreviewDescriptor->Index))
        {
            input.Error = "Failed to initialize thumbnail preview";
            return input;
        }
        std::array<int, static_cast<size_t>(MaterialTextureSlot::Count)> descriptors;
        descriptors.fill(-1);
        std::vector<ThumbnailPreviewPart> parts;
        for (const auto& part : input.MeshParts)
        {
            parts.push_back(
                {MaterialManager::BuildPreviewBinding(part.Material, descriptors), part.FirstVertex, part.VertexCount});
        }
        const auto material = MaterialManager::BuildPreviewBinding(input.Material, descriptors);
        if (!input.Preview->PreparePipelines(material, parts))
        {
            input.Error = "Failed to prepare thumbnail pipelines";
        }
    }
    catch (const std::exception& error)
    {
        // Return the lease even on failure so the UI thread retires its descriptor.
        input.Error = error.what();
    }
    catch (...)
    {
        input.Error = "Unexpected thumbnail preparation failure";
    }
    return input;
}

std::shared_ptr<ThumbnailGpuImage> ThumbnailRenderer::Generate(ThumbnailPrepared& input)
{
    if (!input.Error.empty())
    {
        return {};
    }
    if (input.Type == AssetType::Texture || input.Cached)
    {
        return Upload(input.Textures[0], false);
    }
    auto result = std::make_shared<ThumbnailGpuImage>();
    result->Descriptor = std::move(input.PreviewDescriptor);
    result->Preview = std::move(input.Preview);
    if (!result->Descriptor || !result->Preview)
    {
        return {};
    }
    result->Resource = result->Preview->GetTarget();
    const auto initialPreviewBytes = result->Preview->MemoryBytes();
    result->Bytes = initialPreviewBytes;
    Retain(result);
    std::array<int, static_cast<size_t>(MaterialTextureSlot::Count)> descriptors{-1, -1, -1, -1, -1, -1};
    std::unordered_map<std::string, std::shared_ptr<ThumbnailGpuImage>> textureImages;
    const auto uploadDependency = [&](ThumbnailSourceImage& source) {
        const auto key = source.IdentityKey();
        if (const auto found = textureImages.find(key); !key.empty() && found != textureImages.end())
        {
            return found->second;
        }
        auto image = Upload(source, true);
        if (image)
        {
            if (!key.empty())
            {
                textureImages.emplace(key, image);
            }
            result->Bytes += image->Bytes;
            result->Dependencies.push_back(image);
        }
        return image;
    };
    if (input.Type == AssetType::Material)
    {
        for (size_t slot = 0; slot < input.Textures.size(); ++slot)
        {
            auto& source = input.Textures[slot];
            if (source.Path.empty() || AssetRegistry::Classify(source.Path) == AssetType::Noise)
            {
                continue;
            }
            auto texture = uploadDependency(source);
            if (!texture || !texture->Descriptor)
            {
                return {};
            }
            descriptors[slot] = static_cast<int>(texture->Descriptor->Index);
        }
    }
    std::vector<ThumbnailPreviewPart> parts;
    for (auto& part : input.MeshParts)
    {
        std::array<int, static_cast<size_t>(MaterialTextureSlot::Count)> textureDescriptors{-1, -1, -1, -1, -1, -1};
        for (size_t slot = 0; slot < part.Textures.size(); ++slot)
        {
            auto& source = part.Textures[slot];
            if (source.Path.empty() && !source.Decoded && source.Encoded.empty())
            {
                continue;
            }
            auto texture = uploadDependency(source);
            if (!texture || !texture->Descriptor)
            {
                return {};
            }
            textureDescriptors[slot] = static_cast<int>(texture->Descriptor->Index);
        }
        parts.push_back({MaterialManager::BuildPreviewBinding(part.Material, textureDescriptors), part.FirstVertex,
                         part.VertexCount});
    }
    auto material = MaterialManager::BuildPreviewBinding(input.Material, descriptors);
    material.Noise = input.Noise;
    if (!result->Preview->Render(material, input.Geometry, parts))
    {
        return {};
    }
    result->Resource = result->Preview->GetTarget();
    result->Width = result->Height = input.Size;
    result->Bytes = result->Bytes - initialPreviewBytes + result->Preview->MemoryBytes();
    result->SubmissionFence = GraphicsDevice::GetPendingSubmissionFence();
    return result;
}

bool ThumbnailRenderer::BeginReadback(const std::shared_ptr<ThumbnailGpuImage>& image)
{
    const auto description = image->Resource->GetDesc();
    UINT64 bytes = 0;
    GraphicsDevice::GetDevice()->GetCopyableFootprints(&description, 0, 1, 0, &image->Footprint, nullptr, nullptr,
                                                       &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                    IID_PPV_ARGS(&image->Readback))))
    {
        return false;
    }
    image->Bytes += GraphicsDevice::GetDevice()->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes;
    auto* commands = GraphicsDevice::GetCommandList();
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        image->Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &barrier);
    const CD3DX12_TEXTURE_COPY_LOCATION source(image->Resource.Get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(image->Readback.Get(), image->Footprint);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    image->SubmissionFence = GraphicsDevice::GetPendingSubmissionFence();
    return true;
}

std::future<void> ThumbnailRenderer::SaveCache(const ThumbnailPrepared& input,
                                               const std::shared_ptr<ThumbnailGpuImage>& image)
{
    const auto path = CachePath(input.Handle);
    const auto key = input.Key;
    ThumbnailManifest manifest{input.BaseKey, input.Key, input.SourceDependencies, input.SourcePath};
    return std::async(
        std::launch::async, [path, key, image, mesh = input.Type == AssetType::Mesh, manifest = std::move(manifest)] {
            DirectX::ScratchImage pixels;
            if (FAILED(pixels.Initialize2D(image->Resource->GetDesc().Format, image->Width, image->Height, 1, 1)))
            {
                return;
            }
            const auto* target = pixels.GetImage(0, 0, 0);
            uint8_t* source = nullptr;
            D3D12_RANGE read{0, static_cast<SIZE_T>(image->Readback->GetDesc().Width)};
            if (FAILED(image->Readback->Map(0, &read, reinterpret_cast<void**>(&source))))
            {
                return;
            }
            for (UINT row = 0; row < image->Height; ++row)
            {
                memcpy(target->pixels + row * target->rowPitch,
                       source + image->Footprint.Offset + row * image->Footprint.Footprint.RowPitch, target->rowPitch);
            }
            const D3D12_RANGE noWrite{0, 0};
            image->Readback->Unmap(0, &noWrite);
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return;
            }
            const std::filesystem::path temporary(path.wstring() + L".tmp");
            if (FAILED(DirectX::SaveToDDSFile(pixels.GetImages(), pixels.GetImageCount(), pixels.GetMetadata(),
                                              DirectX::DDS_FLAGS_NONE, temporary.c_str())))
            {
                return;
            }
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return;
            }
            std::string message;
            std::ostringstream metadata;
            if (mesh)
            {
                manifest.Write(metadata);
            }
            else
            {
                metadata << key;
            }
            WriteAssetFile(path.string() + ".key", metadata.str(), message);
        });
}

void ThumbnailRenderer::Collect()
{
    const auto completed = GraphicsDevice::GetCompletedFence();
    std::erase_if(inFlight, [completed](const auto& item) {
        return item.Fence <= completed;
    });
}

uint64_t ThumbnailRenderer::InFlightBytes()
{
    uint64_t bytes = 0;
    for (const auto& item : inFlight)
    {
        bytes += item.Image->Bytes;
    }
    return bytes;
}

void ThumbnailRenderer::Shutdown()
{
    inFlight.clear();
}
