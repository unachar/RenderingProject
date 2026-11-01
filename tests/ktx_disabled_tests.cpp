#define NOMINMAX
#include "../source/rendering/ktxbasis.h"
#include "../source/assets/texturecompression.h"
#include <iostream>
#include <stdexcept>

int main()
{
    DirectX::ScratchImage image;
    if (FAILED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1)))
    {
        return 1;
    }
    image.GetPixels()[0] = 123;
    DirectX::TexMetadata metadata{};
    metadata.width = 7;
    std::vector<uint8_t> encoded{1, 2, 3};
    if (KtxBasis::Encode(image, false, encoded) != E_NOTIMPL || encoded != std::vector<uint8_t>{1, 2, 3} ||
        KtxBasis::Load(nullptr, 0, TextureUsage::Color, metadata, image) != E_NOTIMPL ||
        KtxBasis::LoadFile("missing.ktx2", TextureUsage::Color, metadata, image) != E_NOTIMPL ||
        image.GetPixels()[0] != 123 || metadata.width != 7)
    {
        throw std::runtime_error("disabled SDK must preserve output and report unavailable");
    }
    std::cout << "PASS KTX-disabled module without KTX headers or library\n";
}
