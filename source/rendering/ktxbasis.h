#pragma once

#include "rendererstate.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#if RENDERINGPROJECT_USE_KTX
#include <ktx.h>
#include <ktxvulkan.h>
#endif

#if RENDERINGPROJECT_USE_BASIS
#include "basisu_transcoder.h"
#endif

namespace KtxBasis
{
    // Supported texture formats
    enum class TextureFormat
    {
        Unknown = 0,
        BC1_RGB,
        BC1_RGBA,
        BC3,
        BC4,
        BC5,
        BC6H_UF16,
        BC6H_SF16,
        BC7,
        ETC1,
        ETC2_RGB,
        ETC2_RGBA,
        ASTC_4x4,
        ASTC_6x6,
        ASTC_8x8,
        ASTC_10x10,
        ASTC_12x12,
        RGBA8,
        RG16F,
        RGBA16F,
        R32F,
        RG32F,
        RGBA32F
    };

    // Transcode settings
    struct TranscodeSettings
    {
        TextureFormat targetFormat = TextureFormat::BC7;
        bool generateMipmaps = true;
        uint32_t mipLevels = 0; // 0 = auto
        bool srgb = true;
        bool normalMap = false;
        float quality = 0.75f; // 0.0 - 1.0
    };

    // KTX2 texture info
    struct KtxTextureInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t mipLevels = 0;
        uint32_t layers = 0;
        uint32_t faces = 0;
        bool isArray = false;
        bool isCubemap = false;
        bool isCompressed = false;
        bool srgb = false;
        TextureFormat format = TextureFormat::Unknown;
        ktxTexture2* ktxTexture = nullptr; // Opaque pointer
    };

    // Basis Universal transcoder
    class BasisTranscoder
    {
    public:
        BasisTranscoder();
        ~BasisTranscoder();

        // Initialize the transcoder
        bool Initialize();

        // Transcode KTX2/Basis data to target format
        bool Transcode(const uint8_t* sourceData, size_t sourceSize, 
                       const TranscodeSettings& settings,
                       std::vector<uint8_t>& outData,
                       KtxTextureInfo& outInfo);

        // Transcode from KTX2 file
        bool TranscodeFromFile(const std::string& filePath,
                              const TranscodeSettings& settings,
                              std::vector<uint8_t>& outData,
                              KtxTextureInfo& outInfo);

        // Transcode from Basis Universal file
        bool TranscodeBasisFromFile(const std::string& filePath,
                                   const TranscodeSettings& settings,
                                   std::vector<uint8_t>& outData,
                                   KtxTextureInfo& outInfo);

        // Check if transcoder is initialized
        bool IsInitialized() const { return m_initialized; }

        // Get supported formats
        static std::vector<TextureFormat> GetSupportedFormats();
        
        // Get format name
        static const char* GetFormatName(TextureFormat format);

        // Check if format is supported for given API
        static bool IsFormatSupported(TextureFormat format, bool isVulkan = false);

    private:
        bool m_initialized = false;
        
#if RENDERINGPROJECT_USE_BASIS
        basisu::Transcoder* m_transcoder = nullptr;
#endif
    };

    // KTX2 texture loader
    class KtxTextureLoader
    {
    public:
        KtxTextureLoader();
        ~KtxTextureLoader();

        // Load KTX2 texture from memory
        bool LoadFromMemory(const uint8_t* data, size_t size, KtxTextureInfo& outInfo);

        // Load KTX2 texture from file
        bool LoadFromFile(const std::string& filePath, KtxTextureInfo& outInfo);

        // Get texture data (raw)
        bool GetTextureData(const KtxTextureInfo& info, uint32_t mipLevel, uint32_t layer, 
                           std::vector<uint8_t>& outData) const;

        // Get texture data as Direct3D12 subresources
        bool GetSubresources(const KtxTextureInfo& info, 
                            std::vector<D3D12_SUBRESOURCE_DATA>& outSubresources) const;

        // Create D3D12 texture from KTX2
        bool CreateD3D12Texture(ID3D12Device* device, const KtxTextureInfo& info,
                               ID3D12Resource** outTexture,
                               std::vector<D3D12_SUBRESOURCE_DATA>* outSubresources = nullptr);

        // Check if KTX2 is supported
        static bool IsSupported();

    private:
#if RENDERINGPROJECT_USE_KTX
        ktxTexture2* m_texture = nullptr;
#endif
    };

    // Texture format utilities
    namespace FormatUtils
    {
        DXGI_FORMAT ToDxgiFormat(TextureFormat format, bool srgb = false);
        uint32_t GetBlockSize(TextureFormat format);
        uint32_t GetBlockWidth(TextureFormat format);
        uint32_t GetBlockHeight(TextureFormat format);
        bool IsCompressed(TextureFormat format);
        bool IsSrgb(TextureFormat format);
        size_t CalculateImageSize(TextureFormat format, uint32_t width, uint32_t height, uint32_t depth = 1);
        size_t CalculateMipSize(TextureFormat format, uint32_t width, uint32_t height, uint32_t mipLevel);
    };

} // namespace KtxBasis