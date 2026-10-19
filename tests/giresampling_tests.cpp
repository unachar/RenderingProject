#include "rendering/giresampling.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

void Check(bool valid, const char* message)
{
    if (!valid)
    {
        throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        constexpr float pi = 3.14159265f;
        Check(std::abs(GiResampling::GiGuideAcceptance(.5f, pi / .5f) - 1) < 1e-6f,
              "cosine-distributed guides must be accepted");
        Check(std::abs(GiResampling::GiGuideAcceptance(.5f, pi / 5) - .1f) < 1e-6f,
              "overrepresented radiance guides need BRDF probability correction");
        Check(GiResampling::GiGuideAcceptance(.5f, 100) == 1 && GiResampling::GiGuideAcceptance(.5f, 0) == 0,
              "guide acceptance must remain a bounded probability");
        // 立体角が2倍になる写像では、積分の重みも2倍になる。
        const auto expanded = GiResampling::GiSpatialReuseWeights(3, 2, 4, 2);
        Check(expanded.Selection == 48, "solid-angle expansion must multiply the sample weight");
        Check(expanded.Target == 3, "normalization must use the receiver target without the Jacobian");
        // ほぼ接線方向の再接続では、異常な輝点を発生させず、重みを小さくする。
        const auto grazing = GiResampling::GiSpatialReuseWeights(3, 2, 4, .001f);
        Check(std::abs(grazing.Selection - .024f) < 1e-6f, "grazing reconnection must approach zero weight");
        Check(grazing.Target == expanded.Target, "Jacobian must not change selected target");
        const auto identity = GiResampling::GiSpatialReuseWeights(3, 2, 4, 1);
        Check(identity.Selection == 24 && identity.Target == 3, "identity mapping preserves weight");
        const auto dark = GiResampling::GiSpatialReuseWeights(0, 2, 4, 2);
        Check(dark.Selection == 0 && dark.Target == 0, "zero radiance stays dark");
        std::cout << "GI resampling tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
