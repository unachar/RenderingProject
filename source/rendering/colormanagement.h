#pragma once

#include "rendererstate.h"
#include <string>
#include <vector>
#include <memory>

// Forward declare OCIO types for when OCIO is not available
#if RENDERINGPROJECT_USE_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#else
namespace OCIO
{
    class ConstConfigRcPtr;
    class ConstProcessorRcPtr;
}
#endif

namespace ColorManagement
{
    struct ColorSpaceConfig
    {
        std::string configPath;
        std::string workingSpace = "ACES - ACEScg";
        std::string displayDevice = "sRGB";
        std::string viewTransform = "sRGB";
        std::string lookTransform = "";
        float exposure = 0.0f;
        float gamma = 2.2f;
        bool enableOCIO = true;
    };

    struct DisplayConfig
    {
        std::string displayDevice;
        std::string viewTransform;
        std::string lookTransform;
        float exposure = 0.0f;
        float gamma = 2.2f;
    };

    class ColorManager
    {
    public:
        static ColorManager& Get();

        bool Initialize(const ColorSpaceConfig& config);
        void Shutdown();

        bool IsEnabled() const { return m_enabled; }
        bool IsOCIOAvailable() const;

        // Color space conversions
        bool ConvertColorSpace(
            const float* src, float* dst,
            int width, int height, int channels,
            const std::string& fromSpace, const std::string& toSpace) const;

        // Display transform for final output
        bool ApplyDisplayTransform(
            const float* src, float* dst,
            int width, int height,
            const DisplayConfig& displayConfig = {}) const;

        // Get available color spaces
        std::vector<std::string> GetColorSpaces() const;
        std::vector<std::string> GetDisplays() const;
        std::vector<std::string> GetViews(const std::string& display) const;
        std::vector<std::string> GetLooks(const std::string& display) const;

        // Built-in transforms (no OCIO required)
        static void LinearToSRGB(const float* src, float* dst, int count);
        static void SRGBToLinear(const float* src, float* dst, int count);
        static void LinearToRec709(const float* src, float* dst, int count);
        static void ApplyExposure(const float* src, float* dst, int count, float exposure);
        static void ApplyGamma(const float* src, float* dst, int count, float gamma);
        static void ApplyACESFilm(const float* src, float* dst, int count, float exposure = 0.0f);

        // Shader processor for GPU transforms
        struct GPUProcessor
        {
            std::vector<float> lut3D;  // 3D LUT data (R,G,B,R,G,B...)
            uint32_t lutSize = 0;      // LUT dimension (e.g., 32, 64)
            bool hasLut3D = false;

            // 1D LUTs for each channel
            std::vector<float> lut1D_R;
            std::vector<float> lut1D_G;
            std::vector<float> lut1D_B;
            uint32_t lut1DSize = 0;
        };

        bool BuildGPUProcessor(const std::string& fromSpace, const std::string& toSpace, GPUProcessor& outProcessor) const;
        bool BuildDisplayGPUProcessor(const DisplayConfig& displayConfig, GPUProcessor& outProcessor) const;

    private:
        ColorManager() = default;
        ~ColorManager();

        bool m_enabled = false;

#if RENDERINGPROJECT_USE_OCIO
        OCIO::ConstConfigRcPtr m_config;
        OCIO::ConstProcessorRcPtr m_displayProcessor;
        OCIO::ConstProcessorRcPtr m_workingToDisplayProcessor;
#else
        void* m_config = nullptr;
        void* m_displayProcessor = nullptr;
        void* m_workingToDisplayProcessor = nullptr;
#endif

        // Built-in color space data
        struct BuiltinColorSpace
        {
            std::string name;
            std::string referenceSpace; // "scene_linear", "display_linear", etc.
            bool isSceneLinear = false;
        };
        std::vector<BuiltinColorSpace> m_builtinSpaces;
    };

    // Global accessor
    inline ColorManager& GetColorManager() { return ColorManager::Get(); }

    // Convenience functions
    inline bool IsColorManagementEnabled() { return ColorManager::Get().IsEnabled(); }
    inline bool ConvertToDisplaySpace(const float* src, float* dst, int width, int height, int channels = 4)
    {
        return ColorManager::Get().ApplyDisplayTransform(src, dst, width, height);
    }
}