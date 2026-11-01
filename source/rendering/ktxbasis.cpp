#include "ktxbasis.h"
#include "../assets/texturecompression.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>

#if RENDERINGPROJECT_USE_KTX
#include <ktx.h>
#include "../../External/KTX-Software/upstream/lib/src/vkformat_enum.h"
#endif

namespace KtxBasis
{
    namespace
    {
        constexpr std::array<uint8_t, 12> Identifier{0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, 13, 10, 26, 10};
#if RENDERINGPROJECT_USE_KTX
        using Texture = std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)>;
        constexpr const char* AlphaKey = "RenderDDSAlphaMode";

        struct FormatPair
        {
            DXGI_FORMAT Dxgi;
            uint32_t Vk;
        };

        constexpr FormatPair Formats[] = {{DXGI_FORMAT_BC1_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
                                          {DXGI_FORMAT_BC1_UNORM_SRGB, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
                                          {DXGI_FORMAT_BC2_UNORM, VK_FORMAT_BC2_UNORM_BLOCK},
                                          {DXGI_FORMAT_BC2_UNORM_SRGB, VK_FORMAT_BC2_SRGB_BLOCK},
                                          {DXGI_FORMAT_BC3_UNORM, VK_FORMAT_BC3_UNORM_BLOCK},
                                          {DXGI_FORMAT_BC3_UNORM_SRGB, VK_FORMAT_BC3_SRGB_BLOCK},
                                          {DXGI_FORMAT_BC4_UNORM, VK_FORMAT_BC4_UNORM_BLOCK},
                                          {DXGI_FORMAT_BC4_SNORM, VK_FORMAT_BC4_SNORM_BLOCK},
                                          {DXGI_FORMAT_BC5_UNORM, VK_FORMAT_BC5_UNORM_BLOCK},
                                          {DXGI_FORMAT_BC5_SNORM, VK_FORMAT_BC5_SNORM_BLOCK},
                                          {DXGI_FORMAT_BC6H_UF16, VK_FORMAT_BC6H_UFLOAT_BLOCK},
                                          {DXGI_FORMAT_BC6H_SF16, VK_FORMAT_BC6H_SFLOAT_BLOCK},
                                          {DXGI_FORMAT_BC7_UNORM, VK_FORMAT_BC7_UNORM_BLOCK},
                                          {DXGI_FORMAT_BC7_UNORM_SRGB, VK_FORMAT_BC7_SRGB_BLOCK},
                                          {DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
                                          {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB},
                                          {DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT},
                                          {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
                                          {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT},
                                          {DXGI_FORMAT_R32G32_FLOAT, VK_FORMAT_R32G32_SFLOAT},
                                          {DXGI_FORMAT_R32G32B32A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}};

        DXGI_FORMAT ToDxgi(uint32_t format)
        {
            // Vulkan BC1_RGB has opaque alpha; DXGI BC1 uses RGBA decoding rules.
            // Do not silently change alpha for those formats.
            for (const auto& entry : Formats)
            {
                if (entry.Vk == format)
                {
                    return entry.Dxgi;
                }
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        uint32_t ToVk(DXGI_FORMAT format)
        {
            for (const auto& entry : Formats)
            {
                if (entry.Dxgi == format)
                {
                    return entry.Vk;
                }
            }
            return VK_FORMAT_UNDEFINED;
        }

        HRESULT Status(KTX_error_code result)
        {
            if (result == KTX_SUCCESS)
            {
                return S_OK;
            }
            if (result == KTX_OUT_OF_MEMORY)
            {
                return E_OUTOFMEMORY;
            }
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }

        bool ValidMetadata(const DirectX::TexMetadata& metadata)
        {
            if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || !metadata.width || !metadata.height ||
                metadata.width > 16384 || metadata.height > 16384 || metadata.depth != 1 || metadata.arraySize == 0 ||
                metadata.arraySize > 2048 || metadata.mipLevels == 0 || metadata.mipLevels > 15 ||
                metadata.format == DXGI_FORMAT_UNKNOWN || (metadata.miscFlags & ~DirectX::TEX_MISC_TEXTURECUBE) != 0)
            {
                return false;
            }
            size_t bytes = 0;
            for (size_t level = 0; level < metadata.mipLevels; ++level)
            {
                size_t rowPitch = 0, slicePitch = 0;
                if (FAILED(DirectX::ComputePitch(metadata.format, (std::max)(size_t(1), metadata.width >> level),
                                                 (std::max)(size_t(1), metadata.height >> level), rowPitch,
                                                 slicePitch)) ||
                    slicePitch > (MaxImageBytes - bytes) / metadata.arraySize)
                {
                    return false;
                }
                bytes += slicePitch * metadata.arraySize;
            }
            return true;
        }

        // Keep write allocations in the application's CRT; SDK WriteToMemory uses its DLL's malloc.
        struct OutputStream
        {
            std::vector<uint8_t> Bytes;
            size_t Position = 0;

            static OutputStream& Get(ktxStream* stream)
            {
                return *static_cast<OutputStream*>(stream->data.custom_ptr.address);
            }

            static KTX_error_code Write(ktxStream* stream, const void* data, size_t size, size_t count)
            {
                auto& output = Get(stream);
                if (size && count > (MaxImageBytes - output.Position) / size)
                {
                    return KTX_FILE_OVERFLOW;
                }
                const size_t length = size * count;
                try
                {
                    if (output.Bytes.size() < output.Position + length)
                    {
                        output.Bytes.resize(output.Position + length);
                    }
                    if (length)
                    {
                        std::memcpy(output.Bytes.data() + output.Position, data, length);
                    }
                    output.Position += length;
                    return KTX_SUCCESS;
                }
                catch (const std::bad_alloc&)
                {
                    return KTX_OUT_OF_MEMORY;
                }
            }

            ktxStream Make()
            {
                ktxStream stream{};
                stream.type = eStreamTypeCustom;
                stream.data.custom_ptr.address = this;
                stream.read = [](ktxStream*, void*, size_t) {
                    return KTX_INVALID_OPERATION;
                };
                stream.skip = [](ktxStream*, size_t) {
                    return KTX_INVALID_OPERATION;
                };
                stream.write = Write;
                stream.getpos = [](ktxStream* self, ktx_off_t* position) {
                    *position = Get(self).Position;
                    return KTX_SUCCESS;
                };
                stream.setpos = [](ktxStream* self, ktx_off_t position) {
                    if (position < 0 || uint64_t(position) > MaxImageBytes)
                    {
                        return KTX_FILE_OVERFLOW;
                    }
                    Get(self).Position = size_t(position);
                    return KTX_SUCCESS;
                };
                stream.getsize = [](ktxStream* self, size_t* size) {
                    *size = Get(self).Bytes.size();
                    return KTX_SUCCESS;
                };
                stream.destruct = [](ktxStream*) {
                };
                return stream;
            }
        };
#endif
    }

    bool IsKtx2(const uint8_t* bytes, size_t size)
    {
        return bytes && size >= Identifier.size() && std::memcmp(bytes, Identifier.data(), Identifier.size()) == 0;
    }

    HRESULT Load(const uint8_t* bytes, size_t size, TextureUsage usage, DirectX::TexMetadata& metadata,
                 DirectX::ScratchImage& image)
    {
#if RENDERINGPROJECT_USE_KTX
        if (!IsKtx2(bytes, size) || size < 80 || size > MaxImageBytes)
        {
            return E_INVALIDARG;
        }
        // Bound dimensions before the SDK parses optional metadata or allocates image data.
        uint32_t dimensions[6]{};
        std::memcpy(dimensions, bytes + 20, sizeof(dimensions));
        if (!dimensions[0] || !dimensions[1] || dimensions[0] > 16384 || dimensions[1] > 16384 || dimensions[2] != 0 ||
            dimensions[3] > 2048 || (dimensions[4] != 1 && dimensions[4] != 6) || dimensions[5] > 15)
        {
            return E_NOTIMPL;
        }
        try
        {
            ktxTexture2* raw = nullptr;
            auto result = ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_NO_FLAGS, &raw);
            Texture texture(raw, ktxTexture2_Destroy);
            if (result != KTX_SUCCESS)
            {
                return Status(result);
            }
            if (texture->orientation.x != KTX_ORIENT_X_RIGHT || texture->orientation.y != KTX_ORIENT_Y_DOWN)
            {
                return E_NOTIMPL;
            }
            unsigned swizzleBytes = 0;
            void* swizzle = nullptr;
            result = ktxHashList_FindValue(&texture->kvDataHead, KTX_SWIZZLE_KEY, &swizzleBytes, &swizzle);
            if (result == KTX_SUCCESS &&
                !((swizzleBytes == 4 || (swizzleBytes == 5 && static_cast<const char*>(swizzle)[4] == 0)) &&
                  std::memcmp(swizzle, "rgba", 4) == 0))
            {
                return E_NOTIMPL;
            }
            if (result != KTX_SUCCESS && result != KTX_NOT_FOUND)
            {
                return Status(result);
            }
            const bool basis = ktxTexture2_NeedsTranscoding(texture.get()) != 0;
            DXGI_FORMAT format = ToDxgi(texture->vkFormat);
            if (basis)
            {
                // Two-channel Basis may store G in alpha. Do not silently use that as a normal/material RG map.
                if (usage != TextureUsage::Color || ktxTexture2_GetNumComponents(texture.get()) < 3 ||
                    (ktxTexture2_GetColorModel_e(texture.get()) != KHR_DF_MODEL_ETC1S &&
                     ktxTexture2_GetColorModel_e(texture.get()) != KHR_DF_MODEL_UASTC))
                {
                    return E_NOTIMPL;
                }
                const auto transfer = ktxTexture2_GetTransferFunction_e(texture.get());
                if (transfer != KHR_DF_TRANSFER_LINEAR && transfer != KHR_DF_TRANSFER_SRGB)
                {
                    return E_NOTIMPL;
                }
                format = transfer == KHR_DF_TRANSFER_SRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
            }
            DirectX::TexMetadata candidateMetadata{};
            candidateMetadata.width = texture->baseWidth;
            candidateMetadata.height = texture->baseHeight;
            candidateMetadata.depth = 1;
            candidateMetadata.arraySize = size_t(texture->numLayers) * texture->numFaces;
            candidateMetadata.mipLevels = texture->numLevels;
            candidateMetadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
            candidateMetadata.format = format;
            if (texture->numFaces == 6)
            {
                candidateMetadata.miscFlags = DirectX::TEX_MISC_TEXTURECUBE;
            }
            if (ktxTexture2_GetPremultipliedAlpha(texture.get()))
            {
                candidateMetadata.miscFlags2 = DirectX::TEX_ALPHA_MODE_PREMULTIPLIED;
            }
            unsigned alphaBytes = 0;
            void* alpha = nullptr;
            result = ktxHashList_FindValue(&texture->kvDataHead, AlphaKey, &alphaBytes, &alpha);
            if (result == KTX_SUCCESS)
            {
                if (alphaBytes != sizeof(uint32_t))
                {
                    return E_INVALIDARG;
                }
                std::memcpy(&candidateMetadata.miscFlags2, alpha, sizeof(uint32_t));
                if (candidateMetadata.miscFlags2 > DirectX::TEX_ALPHA_MODE_CUSTOM)
                {
                    return E_INVALIDARG;
                }
                if ((candidateMetadata.miscFlags2 == DirectX::TEX_ALPHA_MODE_PREMULTIPLIED) !=
                    (ktxTexture2_GetPremultipliedAlpha(texture.get()) != 0))
                {
                    return E_INVALIDARG;
                }
            }
            else if (result != KTX_NOT_FOUND)
            {
                return Status(result);
            }
            if (!ValidMetadata(candidateMetadata))
            {
                return E_INVALIDARG;
            }
            if (ktxTexture_GetDataSizeUncompressed(reinterpret_cast<ktxTexture*>(texture.get())) > MaxImageBytes)
            {
                return E_INVALIDARG;
            }
            result = ktxTexture_LoadImageData(reinterpret_cast<ktxTexture*>(texture.get()), nullptr, 0);
            if (result != KTX_SUCCESS)
            {
                return Status(result);
            }
            if (basis)
            {
                result = ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_BC7_RGBA, 0);
                if (result != KTX_SUCCESS)
                {
                    return Status(result);
                }
                if (ToDxgi(texture->vkFormat) != candidateMetadata.format)
                {
                    return E_FAIL;
                }
            }
            DirectX::ScratchImage candidate;
            const HRESULT initialized = candidate.Initialize(candidateMetadata);
            if (FAILED(initialized))
            {
                return initialized;
            }
            auto* base = reinterpret_cast<ktxTexture*>(texture.get());
            for (size_t item = 0; item < candidateMetadata.arraySize; ++item)
            {
                for (size_t level = 0; level < candidateMetadata.mipLevels; ++level)
                {
                    size_t offset = 0;
                    result = ktxTexture_GetImageOffset(base, uint32_t(level), uint32_t(item / texture->numFaces),
                                                       uint32_t(item % texture->numFaces), &offset);
                    if (result != KTX_SUCCESS)
                    {
                        return Status(result);
                    }
                    const auto* destination = candidate.GetImage(level, item, 0);
                    // KTX2 images are tightly packed. This SDK's GetRowPitch truncates partial BC blocks;
                    // validate its full image size against DirectXTex's ceiling-based block layout instead.
                    const size_t imageBytes = ktxTexture_GetImageSize(base, uint32_t(level));
                    if (!texture->pData || imageBytes != destination->slicePitch || offset > texture->dataSize ||
                        imageBytes > texture->dataSize - offset)
                    {
                        return E_FAIL;
                    }
                    std::memcpy(destination->pixels, texture->pData + offset, imageBytes);
                }
            }
            metadata = candidateMetadata;
            image = std::move(candidate);
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
#else
        (void)bytes;
        (void)size;
        (void)usage;
        (void)metadata;
        (void)image;
        return E_NOTIMPL;
#endif
    }

    HRESULT LoadFile(const std::filesystem::path& path, TextureUsage usage, DirectX::TexMetadata& metadata,
                     DirectX::ScratchImage& image)
    {
#if !RENDERINGPROJECT_USE_KTX
        (void)path;
        (void)usage;
        (void)metadata;
        (void)image;
        return E_NOTIMPL;
#else
        try
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            const auto size = stream.tellg();
            if (!stream || size < 80 || size > static_cast<std::streamoff>(MaxImageBytes))
            {
                return E_INVALIDARG;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream)
            {
                return E_FAIL;
            }
            return Load(bytes.data(), bytes.size(), usage, metadata, image);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
#endif
    }

    HRESULT Encode(const DirectX::ScratchImage& image, bool zstd, std::vector<uint8_t>& bytes)
    {
#if RENDERINGPROJECT_USE_KTX
        const auto& metadata = image.GetMetadata();
        if (!ValidMetadata(metadata) || ToVk(metadata.format) == VK_FORMAT_UNDEFINED ||
            metadata.miscFlags2 > DirectX::TEX_ALPHA_MODE_CUSTOM ||
            (metadata.IsCubemap() && metadata.arraySize % 6 != 0))
        {
            return E_INVALIDARG;
        }
        ktxTextureCreateInfo info{};
        info.vkFormat = ToVk(metadata.format);
        info.baseWidth = uint32_t(metadata.width);
        info.baseHeight = uint32_t(metadata.height);
        info.baseDepth = 1;
        info.numDimensions = 2;
        info.numLevels = uint32_t(metadata.mipLevels);
        info.numFaces = metadata.IsCubemap() ? 6 : 1;
        info.numLayers = uint32_t(metadata.arraySize / info.numFaces);
        info.isArray = info.numLayers > 1;
        try
        {
            ktxTexture2* raw = nullptr;
            auto result = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw);
            Texture texture(raw, ktxTexture2_Destroy);
            if (result != KTX_SUCCESS)
            {
                return Status(result);
            }
            if (metadata.IsPMAlpha())
            {
                KHR_DFDSETVAL(texture->pDfd + 1, FLAGS, KHR_DF_FLAG_ALPHA_PREMULTIPLIED);
            }
            result = ktxHashList_AddKVPair(&texture->kvDataHead, AlphaKey, sizeof(uint32_t), &metadata.miscFlags2);
            if (result != KTX_SUCCESS)
            {
                return Status(result);
            }
            auto* base = reinterpret_cast<ktxTexture*>(texture.get());
            for (size_t item = 0; item < metadata.arraySize; ++item)
            {
                for (size_t level = 0; level < metadata.mipLevels; ++level)
                {
                    const auto* source = image.GetImage(level, item, 0);
                    if (!source)
                    {
                        return E_INVALIDARG;
                    }
                    result = ktxTexture_SetImageFromMemory(base, uint32_t(level), uint32_t(item / info.numFaces),
                                                           uint32_t(item % info.numFaces), source->pixels,
                                                           source->slicePitch);
                    if (result != KTX_SUCCESS)
                    {
                        return Status(result);
                    }
                }
            }
            if (zstd)
            {
                result = ktxTexture2_DeflateZstd(texture.get(), 3);
                if (result != KTX_SUCCESS)
                {
                    return Status(result);
                }
            }
            OutputStream output;
            auto stream = output.Make();
            result = ktxTexture2_WriteToStream(texture.get(), &stream);
            if (result != KTX_SUCCESS)
            {
                return Status(result);
            }
            bytes.swap(output.Bytes);
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
#else
        (void)image;
        (void)zstd;
        (void)bytes;
        return E_NOTIMPL;
#endif
    }
}
