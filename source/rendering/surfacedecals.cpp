#include "pch.h"
#include "surfacedecals.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace SurfaceDecals
{
    namespace
    {
        std::vector<Decal> g_Decals;
    }

    void Clear()
    {
        g_Decals.clear();
    }

    uint32_t Add(const Decal& decal)
    {
        if (g_Decals.size() >= kMaxDecals)
        {
            return kNoTexture;
        }
        g_Decals.push_back(decal);
        return static_cast<uint32_t>(g_Decals.size() - 1);
    }

    uint32_t Count()
    {
        return static_cast<uint32_t>(g_Decals.size());
    }

    const Decal& Get(uint32_t index)
    {
        return g_Decals[index];
    }

    uint32_t PackAll(std::vector<GpuDecal>& out)
    {
        std::vector<uint32_t> order(g_Decals.size());
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
            return g_Decals[a].Priority < g_Decals[b].Priority;
        });
        out.clear();
        out.reserve(order.size());
        for (const uint32_t index : order)
        {
            const Decal& decal = g_Decals[index];
            GpuDecal packed{};
            std::memcpy(packed.WorldToLocal, decal.WorldToLocal, sizeof(packed.WorldToLocal));
            packed.ExtentsOpacity[0] = 1.0f;
            packed.ExtentsOpacity[1] = 1.0f;
            packed.ExtentsOpacity[2] = 1.0f;
            packed.ExtentsOpacity[3] = decal.Opacity;
            packed.Textures[0] = decal.AlbedoTexture;
            packed.Textures[1] = decal.NormalTexture;
            packed.Textures[2] = decal.RoughnessTexture;
            packed.Textures[3] = decal.LayerMask;
            packed.Fade[0] = decal.AngleFadeStartCos;
            packed.Fade[1] = decal.AngleFadeEndCos;
            packed.Fade[2] = 0.0f;
            packed.Fade[3] = 0.0f;
            out.push_back(packed);
        }
        return static_cast<uint32_t>(out.size());
    }
} // namespace SurfaceDecals
