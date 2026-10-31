#include "pch.h"
#include "ktxbasis.h"

#if RENDERINGPROJECT_USE_KTX
#include <ktx.h>
#include <ktxvulkan.h>
#endif

#if RENDERINGPROJECT_USE_BASIS
#include "basisu_transcoder.h"
#include "basisu.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace KtxBasis
{

// ============================================================================
// BasisTranscoder Implementation
// ============================================================================

BasisTranscoder::BasisTranscoder()
{
}

BasisTranscoder::~BasisTranscoder()
{
#if RENDERINGPROJECT_USE_BASIS
    if (m_transcoder)
    {
        delete m_transcoder;
        m_transcoder = nullptr;
    }
#endif
}

bool BasisTranscoder::Initialize()
{
#if RENDERINGPROJECT_USE_BASIS
    if (m_transcoder)
    {
        return true;
    }

    m_transcoder = new basisu::Transcoder();
    if (!m_transcoder)
    {
        return false;
    }

    // Initialize the transcoder
    if (!m_transcoder->init())
    {
        delete m_transcoder;
        m_transcoder = nullptr;
        return false;
    }

    m_initialized = true;
    return true;
#else
    m_initialized = false;
    return false;
#endif
}

bool BasisTranscoder::Transcode(const uint8_t* sourceData, size_t sourceSize,
                               const TranscodeSettings& settings,
                               std::vector<uint8_t>& outData,
                               KtxTextureInfo& outInfo)
{
    if (!m_initialized || !sourceData || sourceSize == 0)
    {
        return false;
    }

#if RENDERINGPROJECT_USE_BASIS
    // Create a basisu texture from the source data
    basisu::basisu_file_info fileInfo;
    if (!m_transcoder->get_file_info(sourceData, sourceSize, fileInfo))
    {
        return false;
    }

    // Get total image size
    size_t totalSize = 0;
    for (uint32_t i = 0; i < fileInfo.m_num_levels; ++i)
    {
        uint32_t width = std::max<uint32_t>(1, fileInfo.m_width >> i);
        uint32_t height = std::max<uint32_t>(1, fileInfo.m_height >> i);
        totalSize += m_transcoder->get_transcoded_image_size(
            basisu::basis_format(settings.targetFormat), width, height);
    }

    outData.resize(totalSize);
    
    // Transcode each mip level
    size_t offset = 0;
    for (uint32_t level = 0; level < fileInfo.m_num_levels; ++level)
    {
        uint32_t width = std::max<uint32_t>(1, fileInfo.m_width >> level);
        uint32_t height = std::max<uint32_t>(1, fileInfo.m_height >> level);
        
        size_t levelSize = m_transcoder->get_transcoded_image_size(
            basisu::basis_format(settings.targetFormat), width, height);
        
        if (!m_transcoder->transcode_image(
            sourceData, sourceSize,
            outData.data() + offset,
            level, 0, 0,
            basisu::basis_format(settings.targetFormat),
            0))
        {
            return false;
        }
        
        offset += levelSize;
    }

    // Fill output info
    outInfo.width = fileInfo.m_width;
    outInfo.height = fileInfo.m_height;
    outInfo.depth = 1;
    outInfo.mipLevels = fileInfo.m_num_levels;
    outInfo.layers = 1;
    outInfo.faces = 1;
    outInfo.isArray = false;
    outInfo.isCubemap = false;
    outInfo.isCompressed = true;
    outInfo.srgb = settings.srgb;
    outInfo.format = settings.targetFormat;
    
    return true;
#else
    return false;
#endif
}

bool BasisTranscoder::TranscodeFromFile(const std::string& filePath,
                                       const TranscodeSettings& settings,
                                       std::vector<uint8_t>& outData,
                                       KtxTextureInfo& outInfo)
{
    // Read file into memory
    FILE* file = fopen(filePath.c_str(), "rb");
    if (!file)
    {
        return false;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    std::vector<uint8_t> fileData(fileSize);
    size_t readSize = fread(fileData.data(), 1, fileSize, file);
    fclose(file);

    if (readSize != static_cast<size_t>(fileSize))
    {
        return false;
    }

    return Transcode(fileData.data(), fileData.size(), settings, outData, outInfo);
}

bool BasisTranscoder::TranscodeBasisFromFile(const std::string& filePath,
                                            const TranscodeSettings& settings,
                                            std::vector<uint8_t>& outData,
                                            KtxTextureInfo& outInfo)
{
    // Basis files are transcoded the same way as KTX2
    return TranscodeFromFile(filePath, settings, outData, outInfo);
}

std::vector<TextureFormat> BasisTranscoder::GetSupportedFormats()
{
    return {
        TextureFormat::BC1_RGB,
        TextureFormat::BC1_RGBA,
        TextureFormat::BC3,
        TextureFormat::BC4,
        TextureFormat::BC5,
        TextureFormat::BC6H_UF16,
        TextureFormat::BC6H_SF16,
        TextureFormat::BC7,
        TextureFormat::ETC1,
        TextureFormat::ETC2_RGB,
        TextureFormat::ETC2_RGBA,
        TextureFormat::ASTC_4x4,
        TextureFormat::ASTC_6x6,
        TextureFormat::ASTC_8x8,
        TextureFormat::ASTC_10x10,
        TextureFormat::ASTC_12x12
    };
}

const char* BasisTranscoder::GetFormatName(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BC1_RGB: return "BC1_RGB";
        case TextureFormat::BC1_RGBA: return "BC1_RGBA";
        case TextureFormat::BC3: return "BC3";
        case TextureFormat::BC4: return "BC4";
        case TextureFormat::BC5: return "BC5";
        case TextureFormat::BC6H_UF16: return "BC6H_UF16";
        case TextureFormat::BC6H_SF16: return "BC6H_SF16";
        case TextureFormat::BC7: return "BC7";
        case TextureFormat::ETC1: return "ETC1";
        case TextureFormat::ETC2_RGB: return "ETC2_RGB";
        case TextureFormat::ETC2_RGBA: return "ETC2_RGBA";
        case TextureFormat::ASTC_4x4: return "ASTC_4x4";
        case TextureFormat::ASTC_6x6: return "ASTC_6x6";
        case TextureFormat::ASTC_8x8: return "ASTC_8x8";
        case TextureFormat::ASTC_10x10: return "ASTC_10x10";
        case TextureFormat::ASTC_12x12: return "ASTC_12x12";
        case TextureFormat::RGBA8: return "RGBA8";
        case TextureFormat::RG16F: return "RG16F";
        case TextureFormat::RGBA16F: return "RGBA16F";
        case TextureFormat::R32F: return "R32F";
        case TextureFormat::RG32F: return "RG32F";
        case TextureFormat::RGBA32F: return "RGBA32F";
        default: return "Unknown";
    }
}

bool BasisTranscoder::IsFormatSupported(TextureFormat format, bool isVulkan)
{
    (void)isVulkan;
    // All formats in GetSupportedFormats are supported
    auto formats = GetSupportedFormats();
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}

// ============================================================================
// KtxTextureLoader Implementation
// ============================================================================

KtxTextureLoader::KtxTextureLoader()
{
}

KtxTextureLoader::~KtxTextureLoader()
{
#if RENDERINGPROJECT_USE_KTX
    if (m_texture)
    {
        ktxTexture_Destroy(m_texture);
        m_texture = nullptr;
    }
#endif
}

bool KtxTextureLoader::LoadFromMemory(const uint8_t* data, size_t size, KtxTextureInfo& outInfo)
{
#if RENDERINGPROJECT_USE_KTX
    if (!data || size == 0)
    {
        return false;
    }

    // Destroy existing texture
    if (m_texture)
    {
        ktxTexture_Destroy(m_texture);
        m_texture = nullptr;
    }

    ktx_result result = ktxTexture2_CreateFromMemory(data, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &m_texture);
    if (result != KTX_SUCCESS)
    {
        return false;
    }

    // Fill output info
    outInfo.width = m_texture->baseWidth;
    outInfo.height = m_texture->baseHeight;
    outInfo.depth = m_texture->baseDepth;
    outInfo.mipLevels = m_texture->numLevels;
    outInfo.layers = m_texture->numLayers;
    outInfo.faces = m_texture->numFaces;
    outInfo.isArray = (m_texture->numLayers > 1);
    outInfo.isCubemap = (m_texture->numFaces == 6);
    outInfo.isCompressed = ktxTexture2_IsCompressed(m_texture);
    outInfo.srgb = false; // TODO: Check sRGB flag
    
    // Map KTX format to our format enum
    // This is simplified - in practice you'd need a full format mapping
    outInfo.format = TextureFormat::BC7; // Default
    
    return true;
#else
    return false;
#endif
}

bool KtxTextureLoader::LoadFromFile(const std::string& filePath, KtxTextureInfo& outInfo)
{
#if RENDERINGPROJECT_USE_KTX
    // Destroy existing texture
    if (m_texture)
    {
        ktxTexture_Destroy(m_texture);
        m_texture = nullptr;
    }

    ktx_result result = ktxTexture2_CreateFromFile(filePath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &m_texture);
    if (result != KTX_SUCCESS)
    {
        return false;
    }

    // Fill output info
    outInfo.width = m_texture->baseWidth;
    outInfo.height = m_texture->baseHeight;
    outInfo.depth = m_texture->baseDepth;
    outInfo.mipLevels = m_texture->numLevels;
    outInfo.layers = m_texture->numLayers;
    outInfo.faces = m_texture->numFaces;
    outInfo.isArray = (m_texture->numLayers > 1);
    outInfo.isCubemap = (m_texture->numFaces == 6);
    outInfo.isCompressed = ktxTexture2_IsCompressed(m_texture);
    outInfo.srgb = false; // TODO: Check sRGB flag
    outInfo.format = TextureFormat::BC7; // Default
    
    return true;
#else
    return false;
#endif
}

bool KtxTextureLoader::GetTextureData(const KtxTextureInfo& info, uint32_t mipLevel, uint32_t layer,
                                     std::vector<uint8_t>& outData) const
{
#if RENDERINGPROJECT_USE_KTX
    if (!m_texture)
    {
        return false;
    }

    if (mipLevel >= info.mipLevels || layer >= info.layers)
    {
        return false;
    }

    size_t offset = 0;
    ktx_size_t stride = 0;
    const void* data = ktxTexture_GetImageOffset(m_texture, mipLevel, layer, 0, &offset);
    if (!data)
    {
        return false;
    }

    // Calculate size for this mip level
    uint32_t width = std::max<uint32_t>(1, info.width >> mipLevel);
    uint32_t height = std::max<uint32_t>(1, info.height >> mipLevel);
    uint32_t depth = std::max<uint32_t>(1, info.depth >> mipLevel);

    ktx_size_t imageSize = 0;
    ktxTexture_GetImageSize(m_texture, mipLevel, &imageSize);

    outData.resize(imageSize);
    memcpy(outData.data(), static_cast<const uint8_t*>(data) + offset, imageSize);
    return true;
#else
    return false;
#endif
}

bool KtxTextureLoader::GetSubresources(const KtxTextureInfo& info,
                                      std::vector<D3D12_SUBRESOURCE_DATA>& outSubresources) const
{
#if RENDERINGPROJECT_USE_KTX
    if (!m_texture)
    {
        return false;
    }

    outSubresources.clear();
    outSubresources.reserve(info.mipLevels * info.layers * info.faces);

    for (uint32_t face = 0; face < info.faces; ++face)
    {
        for (uint32_t layer = 0; layer < info.layers; ++layer)
        {
            for (uint32_t level = 0; level < info.mipLevels; ++level)
            {
                size_t offset = 0;
                const void* data = ktxTexture_GetImageOffset(m_texture, level, layer, face, &offset);
                if (!data)
                {
                    return false;
                }

                ktx_size_t imageSize = 0;
                ktxTexture_GetImageSize(m_texture, level, &imageSize);

                uint32_t width = std::max<uint32_t>(1, info.width >> level);
                uint32_t height = std::max<uint32_t>(1, info.height >> level);
                uint32_t depth = std::max<uint32_t>(1, info.depth >> level);

                D3D12_SUBRESOURCE_DATA subresource = {};
                subresource.pData = static_cast<const uint8_t*>(data) + offset;
                subresource.RowPitch = imageSize; // For compressed, row pitch = total size
                subresource.SlicePitch = imageSize;

                outSubresources.push_back(subresource);
            }
        }
    }

    return true;
#else
    return false;
#endif
}

bool KtxTextureLoader::CreateD3D12Texture(ID3D12Device* device, const KtxTextureInfo& info,
                                         ID3D12Resource** outTexture,
                                         std::vector<D3D12_SUBRESOURCE_DATA>* outSubresources)
{
#if RENDERINGPROJECT_USE_KTX
    if (!device || !outTexture || !m_texture)
    {
        return false;
    }

    // Determine DXGI format
    DXGI_FORMAT dxgiFormat = FormatUtils::ToDxgiFormat(info.format, info.srgb);

    // Create texture resource
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = info.width;
    desc.Height = info.height;
    desc.DepthOrArraySize = info.layers * info.faces;
    desc.MipLevels = info.mipLevels;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Determine heap type based on usage
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(outTexture));

    if (FAILED(hr))
    {
        return false;
    }

    // Get subresources
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    if (!GetSubresources(info, subresources))
    {
        return false;
    }

    // Upload texture data
    if (outSubresources)
    {
        *outSubresources = std::move(subresources);
    }

    return true;
#else
    return false;
#endif
}

bool KtxTextureLoader::IsSupported()
{
#if RENDERINGPROJECT_USE_KTX
    return true;
#else
    return false;
#endif
}

// ============================================================================
// FormatUtils Implementation
// ============================================================================

namespace FormatUtils
{

DXGI_FORMAT ToDxgiFormat(TextureFormat format, bool srgb)
{
    switch (format)
    {
        case TextureFormat::BC1_RGB:
            return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC1_RGBA:
            return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC3:
            return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC4:
            return DXGI_FORMAT_BC4_UNORM;
        case TextureFormat::BC5:
            return DXGI_FORMAT_BC5_UNORM;
        case TextureFormat::BC6H_UF16:
            return DXGI_FORMAT_BC6H_UF16;
        case TextureFormat::BC6H_SF16:
            return DXGI_FORMAT_BC6H_SF16;
        case TextureFormat::BC7:
            return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        case TextureFormat::ETC1:
        case TextureFormat::ETC2_RGB:
        case TextureFormat::ETC2_RGBA:
        case TextureFormat::ASTC_4x4:
        case TextureFormat::ASTC_6x6:
        case TextureFormat::ASTC_8x8:
        case TextureFormat::ASTC_10x10:
        case TextureFormat::ASTC_12x12:
            // These are not natively supported on D3D12, would need transcoding
            return DXGI_FORMAT_UNKNOWN;
        case TextureFormat::RGBA8:
            return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RG16F:
            return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::RGBA16F:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::R32F:
            return DXGI_FORMAT_R32_FLOAT;
        case TextureFormat::RG32F:
            return DXGI_FORMAT_R32G32_FLOAT;
        case TextureFormat::RGBA32F:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

uint32_t GetBlockSize(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BC1_RGB:
        case TextureFormat::BC1_RGBA:
        case TextureFormat::ETC1:
        case TextureFormat::ETC2_RGB:
            return 8;
        case TextureFormat::BC3:
        case TextureFormat::BC4:
        case TextureFormat::BC5:
        case TextureFormat::BC6H_UF16:
        case TextureFormat::BC6H_SF16:
        case TextureFormat::BC7:
        case TextureFormat::ETC2_RGB:
        case TextureFormat::ETC2_RGBA:
            return 16;
        case TextureFormat::ASTC_4x4:
            return 16;
        case TextureFormat::ASTC_6x6:
            return 16;
        case TextureFormat::ASTC_8x8:
            return 16;
        case TextureFormat::ASTC_10x10:
            return 16;
        case TextureFormat::ASTC_12x12:
            return 16;
        case TextureFormat::RGBA8:
            return 4;
        case TextureFormat::RG16F:
            return 4;
        case TextureFormat::RGBA16F:
            return 8;
        case TextureFormat::R32F:
            return 4;
        case TextureFormat::RG32F:
            return 8;
        case TextureFormat::RGBA32F:
            return 16;
        default:
            return 0;
    }
}

uint32_t GetBlockWidth(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BC1_RGB:
        case TextureFormat::BC1_RGBA:
        case TextureFormat::BC3:
        case TextureFormat::BC4:
        case TextureFormat::BC5:
        case TextureFormat::BC6H_UF16:
        case TextureFormat::BC6H_SF16:
        case TextureFormat::BC7:
        case TextureFormat::ETC1:
        case TextureFormat::ETC2_RGB:
        case TextureFormat::ETC2_RGBA:
            return 4;
        case TextureFormat::ASTC_4x4:
            return 4;
        case TextureFormat::ASTC_6x6:
            return 6;
        case TextureFormat::ASTC_8x8:
            return 8;
        case TextureFormat::ASTC_10x10:
            return 10;
        case TextureFormat::ASTC_12x12:
            return 12;
        default:
            return 1;
    }
}

uint32_t GetBlockHeight(TextureFormat format)
{
    return GetBlockWidth(format); // Most formats have square blocks
}

bool IsCompressed(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::BC1_RGB:
        case TextureFormat::BC1_RGBA:
        case TextureFormat::BC3:
        case TextureFormat::BC4:
        case TextureFormat::BC5:
        case TextureFormat::BC6H_UF16:
        case TextureFormat::BC6H_SF16:
        case TextureFormat::BC7:
        case TextureFormat::ETC1:
        case TextureFormat::ETC2_RGB:
        case TextureFormat::ETC2_RGBA:
        case TextureFormat::ASTC_4x4:
        case TextureFormat::ASTC_6x6:
        case TextureFormat::ASTC_8x8:
        case TextureFormat::ASTC_10x10:
        case TextureFormat::ASTC_12x12:
            return true;
        default:
            return false;
    }
}

bool IsSrgb(TextureFormat format)
{
    // Formats that have sRGB variants
    switch (format)
    {
        case TextureFormat::BC1_RGB:
        case TextureFormat::BC1_RGBA:
        case TextureFormat::BC3:
        case TextureFormat::BC7:
        case TextureFormat::RGBA8:
            return true;
        default:
            return false;
    }
}

size_t CalculateImageSize(TextureFormat format, uint32_t width, uint32_t height, uint32_t depth)
{
    if (!IsCompressed(format))
    {
        uint32_t blockSize = GetBlockSize(format);
        return size_t(width) * height * depth * blockSize;
    }

    uint32_t blockWidth = GetBlockWidth(format);
    uint32_t blockHeight = GetBlockHeight(format);
    uint32_t blockSize = GetBlockSize(format);

    uint32_t blocksX = (width + blockWidth - 1) / blockWidth;
    uint32_t blocksY = (height + blockHeight - 1) / blockHeight;

    return size_t(blocksX) * blocksY * depth * blockSize;
}

size_t CalculateMipSize(TextureFormat format, uint32_t width, uint32_t height, uint32_t mipLevel)
{
    uint32_t mipWidth = std::max<uint32_t>(1, width >> mipLevel);
    uint32_t mipHeight = std::max<uint32_t>(1, height >> mipLevel);
    return CalculateImageSize(format, mipWidth, mipHeight, 1);
}

} // namespace FormatUtils

} // namespace KtxBasis