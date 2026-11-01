#define NOMINMAX
#include <DirectXTex.h>
#include "../source/rendering/ktxbasis.h"
#include "../source/assets/texturecompression.h"
#include <ktx.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <objbase.h>
using namespace std;
using namespace DirectX;

struct ThumbnailSourceImage
{
    bool Borrowed = false;
    filesystem::path Path;
    vector<uint8_t> Encoded;
    unique_ptr<DirectX::ScratchImage> Decoded;
};

class ImGuiManager
{
  public:
    static bool IsTextureFile(const filesystem::path& path);
};

// Complete production methods, extracted by the runner to avoid a second routing implementation.
#include "texture-image-loaders.inc"
#include "texture-thumbnail-decode.inc"
#include "texture-file-classification.inc"

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value)
        {
            throw std::runtime_error(message);
        }
    }

    void Equal(const DirectX::ScratchImage& source, const DirectX::ScratchImage& decoded)
    {
        const auto& a = source.GetMetadata();
        const auto& b = decoded.GetMetadata();
        Require(a.width == b.width && a.height == b.height && a.depth == b.depth && a.arraySize == b.arraySize &&
                    a.mipLevels == b.mipLevels && a.format == b.format && a.dimension == b.dimension &&
                    a.miscFlags == b.miscFlags && a.miscFlags2 == b.miscFlags2,
                "KTX metadata preserved");
        Require(source.GetImageCount() == decoded.GetImageCount() &&
                    source.GetPixelsSize() == decoded.GetPixelsSize() &&
                    std::memcmp(source.GetPixels(), decoded.GetPixels(), source.GetPixelsSize()) == 0,
                "all native bytes preserved");
    }

    void BasisColor()
    {
        DirectX::ScratchImage source;
        Require(SUCCEEDED(source.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 16, 16, 1, 1)), "Basis fixture");
        for (size_t pixel = 0; pixel < 16 * 16; ++pixel)
        {
            const uint8_t rgba[] = {128, 64, 32, 200};
            std::memcpy(source.GetPixels() + pixel * 4, rgba, 4);
        }
        std::vector<uint8_t> native;
        Require(SUCCEEDED(KtxBasis::Encode(source, false, native)), "Basis source KTX2");
        ktxTexture2* raw = nullptr;
        Require(ktxTexture2_CreateFromMemory(native.data(), native.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                             &raw) == KTX_SUCCESS,
                "Basis encoder source");
        std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)> texture(raw, ktxTexture2_Destroy);
        Require(ktxTexture2_CompressBasis(texture.get(), 128) == KTX_SUCCESS, "real Basis encoding");
        const auto path = std::filesystem::path("bin/validation/ktx/color-basis.ktx2");
        Require(ktxTexture2_WriteToNamedFile(texture.get(), path.string().c_str()) == KTX_SUCCESS,
                "Basis fixture file");
        DirectX::ScratchImage decoded;
        DirectX::TexMetadata metadata{};
        Require(SUCCEEDED(KtxBasis::LoadFile(path, TextureUsage::Color, metadata, decoded)), "real Basis to BC7");
        Require(metadata.format == DXGI_FORMAT_BC7_UNORM_SRGB, "Basis sRGB retained");
        DirectX::ScratchImage rgba;
        Require(SUCCEEDED(DirectX::Decompress(decoded.GetImages(), decoded.GetImageCount(), metadata,
                                              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, rgba)),
                "Basis BC7 quality readback in source encoding");
        double difference = 0;
        for (size_t index = 0; index < source.GetPixelsSize(); ++index)
        {
            difference += std::abs(int(source.GetPixels()[index]) - int(rgba.GetPixels()[index]));
        }
        if (difference / source.GetPixelsSize() >= 12)
        {
            std::cerr << "Basis RGBA=" << int(rgba.GetPixels()[0]) << ',' << int(rgba.GetPixels()[1]) << ','
                      << int(rgba.GetPixels()[2]) << ',' << int(rgba.GetPixels()[3]) << '\n';
        }
        Require(difference / source.GetPixelsSize() < 12, "Basis constant-color and alpha error bounded");
        Require(FAILED(KtxBasis::LoadFile(path, TextureUsage::Normal, metadata, decoded)),
                "unverified Basis normal channel mapping must not be assumed");
    }

    void StandardPremultipliedAlpha()
    {
        DirectX::ScratchImage image;
        DirectX::TexMetadata metadata{};
        metadata.width = metadata.height = 4;
        metadata.depth = metadata.arraySize = metadata.mipLevels = 1;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
        metadata.miscFlags2 = DirectX::TEX_ALPHA_MODE_PREMULTIPLIED;
        Require(SUCCEEDED(image.Initialize(metadata)), "premultiplied fixture");
        std::memset(image.GetPixels(), 128, image.GetPixelsSize());
        std::vector<uint8_t> encoded;
        Require(SUCCEEDED(KtxBasis::Encode(image, false, encoded)), "premultiplied encoding");
        ktxTexture2* raw = nullptr;
        Require(ktxTexture2_CreateFromMemory(encoded.data(), encoded.size(), KTX_TEXTURE_CREATE_NO_FLAGS, &raw) ==
                    KTX_SUCCESS,
                "standard reader metadata");
        std::unique_ptr<ktxTexture2, decltype(&ktxTexture2_Destroy)> texture(raw, ktxTexture2_Destroy);
        Require(ktxTexture2_GetPremultipliedAlpha(texture.get()) != 0, "standard DFD must declare premultiplied alpha");
    }
}

int main()
{
    try
    {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "thumbnail WIC COM environment");

        struct ComScope
        {
            ~ComScope()
            {
                CoUninitialize();
            }
        } com;

        Require(ImGuiManager::IsTextureFile("image.ktx2") && ImGuiManager::IsTextureFile("IMAGE.KTX2") &&
                    ImGuiManager::IsTextureFile("image.png") && !ImGuiManager::IsTextureFile("image.ktx2.meta"),
                "editor texture assignment recognizes KTX2");
        for (DXGI_FORMAT format :
             {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC4_UNORM,
              DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16, DXGI_FORMAT_BC7_UNORM,
              DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB})
        {
            for (bool cube : {false, true})
            {
                DirectX::ScratchImage source;
                const HRESULT initialized =
                    cube ? source.InitializeCube(format, 16, 16, 2, 4) : source.Initialize2D(format, 13, 9, 2, 4);
                Require(SUCCEEDED(initialized), "image fixture");
                auto sourceMetadata = source.GetMetadata();
                sourceMetadata.miscFlags2 = DirectX::TEX_ALPHA_MODE_STRAIGHT;
                Require(SUCCEEDED(source.Initialize(sourceMetadata)), "alpha metadata fixture");
                for (size_t index = 0; index < source.GetPixelsSize(); ++index)
                {
                    source.GetPixels()[index] = static_cast<uint8_t>(index * 17 + index / 13);
                }
                for (bool compressed : {false, true})
                {
                    std::vector<uint8_t> encoded;
                    const auto encodedStatus = KtxBasis::Encode(source, compressed, encoded);
                    if (FAILED(encodedStatus))
                    {
                        std::cerr << "encode format=" << format << " cube=" << cube << " zstd=" << compressed
                                  << " hr=" << std::hex << encodedStatus << '\n';
                    }
                    Require(SUCCEEDED(encodedStatus), "real KTX2 encoding");
                    Require(KtxBasis::IsKtx2(encoded.data(), encoded.size()), "KTX2 signature");
                    DirectX::ScratchImage decoded;
                    DirectX::TexMetadata metadata{};
                    const auto decodedStatus =
                        KtxBasis::Load(encoded.data(), encoded.size(), TextureUsage::Color, metadata, decoded);
                    if (FAILED(decodedStatus))
                    {
                        std::cerr << "decode format=" << format << " cube=" << cube << " zstd=" << compressed
                                  << " hr=" << std::hex << decodedStatus << '\n';
                    }
                    Require(SUCCEEDED(decodedStatus), "real KTX2 loading");
                    Equal(source, decoded);
                    Require(FAILED(LoadImageFromMemory(encoded.data(), encoded.size(), metadata, decoded)),
                            "material Texture2D import rejects array and cube shapes");
                    const auto before = decoded.GetPixels()[0];
                    encoded[0] = 0;
                    Require(
                        FAILED(KtxBasis::Load(encoded.data(), encoded.size(), TextureUsage::Color, metadata, decoded)),
                        "invalid input rejected");
                    Require(decoded.GetPixels()[0] == before, "failed load preserves caller output");
                    encoded.assign(encoded.size(), 0);
                    Equal(source, decoded);
                }
            }
        }
        StandardPremultipliedAlpha();
        BasisColor();
        DirectX::ScratchImage image;
        Require(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1)), "invalid-input fixture");
        std::memset(image.GetPixels(), 128, image.GetPixelsSize());
        std::vector<uint8_t> valid;
        Require(SUCCEEDED(KtxBasis::Encode(image, true, valid)), "invalid-input source");
        DirectX::ScratchImage decoded;
        DirectX::TexMetadata metadata{};
        for (size_t length : {size_t(0), size_t(12), size_t(79), size_t(80), valid.size() - 1})
        {
            Require(FAILED(KtxBasis::Load(valid.data(), length, TextureUsage::Color, metadata, decoded)),
                    "truncated KTX rejected");
        }
        auto oversized = valid;
        const uint32_t width = UINT32_MAX;
        std::memcpy(oversized.data() + 20, &width, sizeof(width));
        Require(FAILED(KtxBasis::Load(oversized.data(), oversized.size(), TextureUsage::Color, metadata, decoded)),
                "oversized dimensions rejected before image allocation");
        const auto unicodePath = std::filesystem::path(L"bin/validation/ktx/\u65e5\u672c\u8a9e.ktx2");
        {
            std::ofstream file(unicodePath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(valid.data()), valid.size());
            Require(bool(file), "Unicode fixture write");
        }
        Require(SUCCEEDED(KtxBasis::LoadFile(unicodePath, TextureUsage::Color, metadata, decoded)),
                "Unicode file import");
        Equal(image, decoded);
        Require(SUCCEEDED(LoadImageFromFile(unicodePath.c_str(), metadata, decoded, TextureUsage::Normal)),
                "production file loader supports native KTX2 data maps");
        Require(SUCCEEDED(LoadImageFromMemory(valid.data(), valid.size(), metadata, decoded)),
                "production memory import");
        Equal(image, decoded);
        ThumbnailSourceImage thumbnail;
        thumbnail.Encoded = valid;
        string error;
        const bool thumbnailLoaded = Decode(thumbnail, 64, error);
        if (!thumbnailLoaded)
        {
            std::cerr << "thumbnail: " << error << '\n';
        }
        Require(thumbnailLoaded && thumbnail.Decoded && thumbnail.Decoded->GetMetadata().width == 16,
                "production thumbnail KTX2 memory decoding");
        ThumbnailSourceImage fileThumbnail;
        fileThumbnail.Path = unicodePath;
        Require(Decode(fileThumbnail, 64, error) && fileThumbnail.Decoded, "production thumbnail KTX2 file decoding");
        std::cout << "PASS native KTX2 BC/RGBA mip/array/cube and sRGB roundtrips\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
