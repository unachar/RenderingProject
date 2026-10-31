#include "pch.h"
#include "colormanagement.h"

#if RENDERINGPROJECT_USE_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

#include <algorithm>
#include <cmath>

namespace ColorManagement
{

ColorManager& ColorManager::Get()
{
    static ColorManager instance;
    return instance;
}

ColorManager::~ColorManager()
{
    Shutdown();
}

bool ColorManager::Initialize(const ColorSpaceConfig& config)
{
    m_enabled = false;

    // Initialize built-in color spaces
    m_builtinSpaces = {
        {"ACES - ACEScg", "scene_linear", true},
        {"ACES2065-1", "scene_linear", true},
        {"ACEScg", "scene_linear", true},
        {"ACEScc", "scene_linear", false},
        {"ACEScct", "scene_linear", false},
        {"ARRI LogC3", "scene_linear", false},
        {"ARRI LogC4", "scene_linear", false},
        {"Blackmagic Design Film", "scene_linear", false},
        {"Canon Log", "scene_linear", false},
        {"Canon Log 2", "scene_linear", false},
        {"Canon Log 3", "scene_linear", false},
        {"DCI-P3", "display_linear", false},
        {"Display P3", "display_linear", false},
        {"ERIMM RGB", "scene_linear", true},
        {"FilmLight E-Gamut", "scene_linear", false},
        {"ITU-R BT.2020", "display_linear", false},
        {"ITU-R BT.709", "display_linear", false},
        {"Linear sRGB", "display_linear", false},
        {"Log3G10", "scene_linear", false},
        {"Log3G12", "scene_linear", false},
        {"Panasonic V-Log", "scene_linear", false},
        {"REDLog3G10", "scene_linear", false},
        {"REDLogFilm", "scene_linear", false},
        {"Rec.2020", "display_linear", false},
        {"Rec.709", "display_linear", false},
        {"sRGB", "display_linear", false},
        {"Sony S-Log", "scene_linear", false},
        {"Sony S-Log2", "scene_linear", false},
        {"Sony S-Log3", "scene_linear", false},
        {"Utility - Linear - sRGB", "display_linear", false},
        {"Utility - Raw", "scene_linear", true},
        {"Utility - sRGB - Texture", "display_linear", false},
    };

#if RENDERINGPROJECT_USE_OCIO
    try
    {
        OCIO::ConstConfigRcPtr loadedConfig;

        if (!config.configPath.empty())
        {
            loadedConfig = OCIO::Config::CreateFromFile(config.configPath.c_str());
        }
        else
        {
            // Try to find OCIO config from environment
            const char* ocioEnv = std::getenv("OCIO");
            if (ocioEnv && *ocioEnv)
            {
                loadedConfig = OCIO::Config::CreateFromFile(ocioEnv);
            }
            else
            {
                // Create a minimal built-in config
                loadedConfig = OCIO::Config::Create();
            }
        }

        if (!loadedConfig)
        {
            Debug::Log("WARNING: Failed to load OCIO config, using built-in transforms\n");
        }
        else
        {
            m_config = loadedConfig;

            // Validate working space
            if (!config.workingSpace.empty())
            {
                OCIO::ConstColorSpaceRcPtr workingSpace = m_config->getColorSpace(config.workingSpace.c_str());
                if (!workingSpace)
                {
                    Debug::Log("WARNING: Working space '");
                    Debug::Log(config.workingSpace.c_str());
                    Debug::Log("' not found in OCIO config\n");
                }
            }

            // Build display processor
            if (!config.displayDevice.empty() && !config.viewTransform.empty())
            {
                // v2.3 API: Use DisplayViewHelpers::GetProcessor for display/view
                OCIO::ConstProcessorRcPtr processor = OCIO::DisplayViewHelpers::GetProcessor(
                    m_config,
                    OCIO::ROLE_SCENE_LINEAR,
                    config.displayDevice.c_str(),
                    config.viewTransform.c_str(),
                    OCIO::ConstMatrixTransformRcPtr(), // no channel view
                    OCIO::TRANSFORM_DIR_FORWARD);

                // Apply look transform if specified
                if (!config.lookTransform.empty())
                {
                    OCIO::LookTransformRcPtr lookTransform = OCIO::LookTransform::Create();
                    lookTransform->setLooks(config.lookTransform.c_str());
                    lookTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                    
                    // Chain with display processor using GroupTransform
                    OCIO::GroupTransformRcPtr groupTransform = OCIO::GroupTransform::Create();
                    groupTransform->appendTransform(processor->createGroupTransform());
                    groupTransform->appendTransform(lookTransform);
                    
                    m_displayProcessor = m_config->getProcessor(groupTransform);
                }
                else
                {
                    m_displayProcessor = processor;
                }

                // Apply exposure/gamma if needed (v2.3: use ExposureContrastTransform)
                if (config.exposure != 0.0f || config.gamma != 2.2f)
                {
                    // Create exposure/gamma transform
                    OCIO::ExposureContrastTransformRcPtr ecTransform = OCIO::ExposureContrastTransform::Create();
                    ecTransform->setExposure(config.exposure);
                    ecTransform->setGamma(config.gamma);
                    ecTransform->setStyle(OCIO::EXPOSURE_CONTRAST_VIDEO);
                    
                    // Chain with display processor using GroupTransform
                    OCIO::GroupTransformRcPtr groupTransform = OCIO::GroupTransform::Create();
                    if (config.lookTransform.empty())
                    {
                        groupTransform->appendTransform(m_displayProcessor->createGroupTransform());
                    }
                    else
                    {
                        groupTransform->appendTransform(m_displayProcessor->createGroupTransform());
                    }
                    groupTransform->appendTransform(ecTransform);
                    
                    m_displayProcessor = m_config->getProcessor(groupTransform);
                }

                // Working to display processor
                m_workingToDisplayProcessor = OCIO::DisplayViewHelpers::GetProcessor(
                    m_config,
                    OCIO::ROLE_SCENE_LINEAR,
                    config.displayDevice.c_str(),
                    config.viewTransform.c_str(),
                    OCIO::ConstMatrixTransformRcPtr(),
                    OCIO::TRANSFORM_DIR_FORWARD);
            }

            m_enabled = true;
            Debug::Log("OpenColorIO initialized successfully\n");
            return true;
        }
    }
    catch (const OCIO::Exception& e)
    {
        Debug::Log("OCIO initialization error: ");
        Debug::Log(e.what());
        Debug::Log("\n");
    }
    catch (const std::exception& e)
    {
        Debug::Log("OCIO initialization error: ");
        Debug::Log(e.what());
        Debug::Log("\n");
    }
#endif

    // Fallback: enable built-in transforms even without OCIO
    if (config.enableOCIO)
    {
        m_enabled = true;
        Debug::Log("Color management enabled with built-in transforms (OCIO not available)\n");
        return true;
    }

    return false;
}

void ColorManager::Shutdown()
{
#if RENDERINGPROJECT_USE_OCIO
    m_displayProcessor.reset();
    m_workingToDisplayProcessor.reset();
    m_config.reset();
#endif
    m_enabled = false;
}

bool ColorManager::IsOCIOAvailable() const
{
#if RENDERINGPROJECT_USE_OCIO
    return m_config != nullptr;
#else
    return false;
#endif
}

bool ColorManager::ConvertColorSpace(
    const float* src, float* dst,
    int width, int height, int channels,
    const std::string& fromSpace, const std::string& toSpace) const
{
    if (!src || !dst || width <= 0 || height <= 0 || channels <= 0)
        return false;

    if (fromSpace == toSpace)
    {
        if (src != dst)
        {
            std::memcpy(dst, src, size_t(width) * height * channels * sizeof(float));
        }
        return true;
    }

    const int pixelCount = width * height;
    const int stride = channels;

#if RENDERINGPROJECT_USE_OCIO
    if (m_config && m_workingToDisplayProcessor)
    {
        try
        {
            OCIO::ConstProcessorRcPtr processor = m_config->getProcessor(
                fromSpace.c_str(), toSpace.c_str());

            if (processor)
            {
                // v2.3 API: Use CPUProcessor for apply with PackedImageDesc
                OCIO::ConstCPUProcessorRcPtr cpuProcessor = processor->getDefaultCPUProcessor();
                if (cpuProcessor)
                {
                    OCIO::PackedImageDesc srcImgDesc(const_cast<float*>(src), width, height, channels);
                    OCIO::PackedImageDesc dstImgDesc(dst, width, height, channels);

                    cpuProcessor->apply(srcImgDesc, dstImgDesc);
                    return true;
                }
            }
        }
        catch (const OCIO::Exception&)
        {
            // Fall through to built-in
        }
    }
#endif

    // Built-in conversions
    if (fromSpace == "Linear sRGB" || fromSpace == "sRGB" || fromSpace == "Rec.709")
    {
        if (toSpace == "Linear sRGB" || toSpace == "Linear" || toSpace == "scene_linear")
        {
            // sRGB to Linear
            for (int i = 0; i < pixelCount; ++i)
            {
                const float* s = src + i * stride;
                float* d = dst + i * stride;
                SRGBToLinear(s, d, std::min(channels, 3));
                if (channels == 4) d[3] = s[3];
            }
            return true;
        }
    }
    else if (fromSpace == "Linear" || fromSpace == "scene_linear" || fromSpace == "Linear sRGB")
    {
        if (toSpace == "sRGB" || toSpace == "Rec.709" || toSpace == "Display")
        {
            // Linear to sRGB
            for (int i = 0; i < pixelCount; ++i)
            {
                const float* s = src + i * stride;
                float* d = dst + i * stride;
                LinearToSRGB(s, d, std::min(channels, 3));
                if (channels == 4) d[3] = s[3];
            }
            return true;
        }
    }

    // Default: copy
    if (src != dst)
    {
        std::memcpy(dst, src, size_t(pixelCount) * stride * sizeof(float));
    }
    return true;
}

bool ColorManager::ApplyDisplayTransform(
    const float* src, float* dst,
    int width, int height,
    const DisplayConfig& displayConfig) const
{
    if (!src || !dst || width <= 0 || height <= 0)
        return false;

    const int pixelCount = width * height;
    const int channels = 4;

#if RENDERINGPROJECT_USE_OCIO
    if (m_displayProcessor)
    {
        try
        {
            // v2.3 API: Use CPUProcessor for apply with PackedImageDesc
            OCIO::ConstCPUProcessorRcPtr cpuProcessor = m_displayProcessor->getDefaultCPUProcessor();
            if (cpuProcessor)
            {
                OCIO::PackedImageDesc srcImgDesc(const_cast<float*>(src), width, height, channels);
                OCIO::PackedImageDesc dstImgDesc(dst, width, height, channels);
                cpuProcessor->apply(srcImgDesc, dstImgDesc);
                return true;
            }
        }
        catch (const OCIO::Exception&)
        {
            // Fall through to built-in
        }
    }
#endif

    // Built-in display transform: Linear -> sRGB with optional exposure/gamma
    DisplayConfig config = displayConfig;
    if (config.gamma <= 0.0f) config.gamma = 2.2f;

    for (int i = 0; i < pixelCount; ++i)
    {
        const float* s = src + i * channels;
        float* d = dst + i * channels;

        // Apply exposure
        float r = s[0], g = s[1], b = s[2];
        if (config.exposure != 0.0f)
        {
            float expScale = std::exp2(config.exposure);
            r *= expScale;
            g *= expScale;
            b *= expScale;
        }

        // Linear to sRGB (approximate)
        auto toSRGB = [](float x) -> float
        {
            x = std::max(0.0f, x);
            return (x <= 0.0031308f) ? (12.92f * x) : (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f);
        };

        d[0] = toSRGB(r);
        d[1] = toSRGB(g);
        d[2] = toSRGB(b);
        d[3] = s[3];
    }

    return true;
}

std::vector<std::string> ColorManager::GetColorSpaces() const
{
    std::vector<std::string> spaces;

#if RENDERINGPROJECT_USE_OCIO
    if (m_config)
    {
        int numSpaces = m_config->getNumColorSpaces();
        for (int i = 0; i < numSpaces; ++i)
        {
            spaces.push_back(m_config->getColorSpaceNameByIndex(i));
        }
        return spaces;
    }
#endif

    // Built-in spaces
    for (const auto& space : m_builtinSpaces)
    {
        spaces.push_back(space.name);
    }
    return spaces;
}

std::vector<std::string> ColorManager::GetDisplays() const
{
    std::vector<std::string> displays;

#if RENDERINGPROJECT_USE_OCIO
    if (m_config)
    {
        int numDisplays = m_config->getNumDisplays();
        for (int i = 0; i < numDisplays; ++i)
        {
            displays.push_back(m_config->getDisplay(i));
        }
        return displays;
    }
#endif

    // Default displays
    displays = {"sRGB", "Rec.709", "DCI-P3", "Display P3", "Rec.2020"};
    return displays;
}

std::vector<std::string> ColorManager::GetViews(const std::string& display) const
{
    std::vector<std::string> views;

#if RENDERINGPROJECT_USE_OCIO
    if (m_config)
    {
        int numViews = m_config->getNumViews(display.c_str());
        for (int i = 0; i < numViews; ++i)
        {
            views.push_back(m_config->getView(display.c_str(), i));
        }
        return views;
    }
#endif

    // Default views
    views = {"sRGB", "Rec.709", "DCI-P3", "Display P3", "Rec.2020", "Raw"};
    return views;
}

std::vector<std::string> ColorManager::GetLooks(const std::string& display) const
{
    std::vector<std::string> looks;

#if RENDERINGPROJECT_USE_OCIO
    if (m_config)
    {
        int numLooks = m_config->getNumLooks();
        for (int i = 0; i < numLooks; ++i)
        {
            looks.push_back(m_config->getLookNameByIndex(i));
        }
        return looks;
    }
#endif

    // Default looks
    looks = {"None", "Filmic", "ACES", "Filmic Log", "Kodak2383", "Fuji3510"};
    return looks;
}

bool ColorManager::BuildGPUProcessor(
    const std::string& fromSpace, const std::string& toSpace,
    GPUProcessor& outProcessor) const
{
    outProcessor = GPUProcessor();

#if RENDERINGPROJECT_USE_OCIO
    if (m_config)
    {
        try
        {
            OCIO::ConstProcessorRcPtr processor = m_config->getProcessor(
                fromSpace.c_str(), toSpace.c_str());

            if (processor)
            {
                // v2.3 API: Use GPUProcessor and GpuShaderDesc to extract LUTs
                OCIO::ConstGPUProcessorRcPtr gpuProcessor = processor->getDefaultGPUProcessor();
                if (gpuProcessor)
                {
                    OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
                    gpuProcessor->extractGpuShaderInfo(shaderDesc);

                    // Extract 3D LUT
                    unsigned num3DTextures = shaderDesc->getNum3DTextures();
                    if (num3DTextures > 0)
                    {
                        const char* textureName;
                        const char* samplerName;
                        unsigned edgelen;
                        OCIO::Interpolation interpolation;
                        shaderDesc->get3DTexture(0, textureName, samplerName, edgelen, interpolation);
                        const float* values;
                        shaderDesc->get3DTextureValues(0, values);
                        outProcessor.lutSize = edgelen;
                        int numValues = edgelen * edgelen * edgelen * 4; // RGBA
                        outProcessor.lut3D.assign(values, values + numValues);
                        outProcessor.hasLut3D = true;
                    }

                    // Extract 1D LUTs
                    unsigned numTextures = shaderDesc->getNumTextures();
                    if (numTextures >= 3)
                    {
                        for (unsigned i = 0; i < 3 && i < numTextures; ++i)
                        {
                            const char* textureName;
                            const char* samplerName;
                            unsigned width, height;
                            OCIO::GpuShaderDesc::TextureType channel;
                            OCIO::GpuShaderDesc::TextureDimensions dimensions;
                            OCIO::Interpolation interpolation;
                            shaderDesc->getTexture(i, textureName, samplerName, width, height, channel, dimensions, interpolation);
                            const float* values;
                            shaderDesc->getTextureValues(i, values);
                            if (i == 0)
                            {
                                outProcessor.lut1DSize = width;
                                outProcessor.lut1D_R.assign(values, values + width);
                            }
                            else if (i == 1)
                            {
                                outProcessor.lut1D_G.assign(values, values + width);
                            }
                            else if (i == 2)
                            {
                                outProcessor.lut1D_B.assign(values, values + width);
                            }
                        }
                    }

                    return outProcessor.hasLut3D || outProcessor.lut1DSize > 0;
                }
            }
        }
        catch (const OCIO::Exception&)
        {
        }
    }
#endif

    return false;
}

bool ColorManager::BuildDisplayGPUProcessor(const DisplayConfig& displayConfig, GPUProcessor& outProcessor) const
{
    outProcessor = GPUProcessor();

#if RENDERINGPROJECT_USE_OCIO
    if (m_displayProcessor)
    {
        try
        {
            // v2.3 API: Use GPUProcessor and GpuShaderDesc to extract LUTs
            OCIO::ConstGPUProcessorRcPtr gpuProcessor = m_displayProcessor->getDefaultGPUProcessor();
            if (gpuProcessor)
            {
                OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
                gpuProcessor->extractGpuShaderInfo(shaderDesc);

                // Extract 3D LUT
                unsigned num3DTextures = shaderDesc->getNum3DTextures();
                if (num3DTextures > 0)
                {
                    const char* textureName;
                    const char* samplerName;
                    unsigned edgelen;
                    OCIO::Interpolation interpolation;
                    shaderDesc->get3DTexture(0, textureName, samplerName, edgelen, interpolation);
                    const float* values;
                    shaderDesc->get3DTextureValues(0, values);
                    outProcessor.lutSize = edgelen;
                    int numValues = edgelen * edgelen * edgelen * 4; // RGBA
                    outProcessor.lut3D.assign(values, values + numValues);
                    outProcessor.hasLut3D = true;
                }

                // Extract 1D LUTs
                unsigned numTextures = shaderDesc->getNumTextures();
                if (numTextures >= 3)
                {
                    for (unsigned i = 0; i < 3 && i < numTextures; ++i)
                    {
                        const char* textureName;
                        const char* samplerName;
                        unsigned width, height;
                        OCIO::GpuShaderDesc::TextureType channel;
                        OCIO::GpuShaderDesc::TextureDimensions dimensions;
                        OCIO::Interpolation interpolation;
                        shaderDesc->getTexture(i, textureName, samplerName, width, height, channel, dimensions, interpolation);
                        const float* values;
                        shaderDesc->getTextureValues(i, values);
                        if (i == 0)
                        {
                            outProcessor.lut1DSize = width;
                            outProcessor.lut1D_R.assign(values, values + width);
                        }
                        else if (i == 1)
                        {
                            outProcessor.lut1D_G.assign(values, values + width);
                        }
                        else if (i == 2)
                        {
                            outProcessor.lut1D_B.assign(values, values + width);
                        }
                    }
                }

                return outProcessor.hasLut3D || outProcessor.lut1DSize > 0;
            }
        }
        catch (const OCIO::Exception&)
        {
        }
    }
#endif

    return false;
}

// Static built-in transform implementations

void ColorManager::LinearToSRGB(const float* src, float* dst, int count)
{
    for (int i = 0; i < count; ++i)
    {
        float x = std::max(0.0f, src[i]);
        dst[i] = (x <= 0.0031308f) ? (12.92f * x) : (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f);
    }
}

void ColorManager::SRGBToLinear(const float* src, float* dst, int count)
{
    for (int i = 0; i < count; ++i)
    {
        float x = std::max(0.0f, src[i]);
        dst[i] = (x <= 0.04045f) ? (x / 12.92f) : std::pow((x + 0.055f) / 1.055f, 2.4f);
    }
}

void ColorManager::LinearToRec709(const float* src, float* dst, int count)
{
    // Rec.709 is very similar to sRGB
    LinearToSRGB(src, dst, count);
}

void ColorManager::ApplyExposure(const float* src, float* dst, int count, float exposure)
{
    float scale = std::exp2(exposure);
    for (int i = 0; i < count; ++i)
    {
        dst[i] = src[i] * scale;
    }
}

void ColorManager::ApplyGamma(const float* src, float* dst, int count, float gamma)
{
    float invGamma = 1.0f / gamma;
    for (int i = 0; i < count; ++i)
    {
        float x = std::max(0.0f, src[i]);
        dst[i] = std::pow(x, invGamma);
    }
}

void ColorManager::ApplyACESFilm(const float* src, float* dst, int count, float exposure)
{
    // Simplified ACES filmic tone mapping
    // Reference: https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
    float expScale = std::exp2(exposure);

    // ACES input matrix (approx)
    const float ACESInputMat[3][3] = {
        { 0.59719f, 0.35458f, 0.04823f },
        { 0.07600f, 0.90834f, 0.01566f },
        { 0.02840f, 0.13383f, 0.83777f }
    };

    // ACES output matrix (approx)
    const float ACESOutputMat[3][3] = {
        {  1.60475f, -0.53108f, -0.07367f },
        { -0.10208f,  1.10813f, -0.00605f },
        { -0.00327f, -0.07276f,  1.07602f }
    };

    // RRT+ODT simplified
    auto RRTAndODTFit = [](float v) -> float
    {
        float a = v * (v + 0.0245786f) - 0.000090537f;
        float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
        return std::max(0.0f, a / b);
    };

    for (int i = 0; i < count; i += 3)
    {
        float r = src[i + 0] * expScale;
        float g = src[i + 1] * expScale;
        float b = src[i + 2] * expScale;

        // ACES input transform
        float r2 = ACESInputMat[0][0] * r + ACESInputMat[0][1] * g + ACESInputMat[0][2] * b;
        float g2 = ACESInputMat[1][0] * r + ACESInputMat[1][1] * g + ACESInputMat[1][2] * b;
        float b2 = ACESInputMat[2][0] * r + ACESInputMat[2][1] * g + ACESInputMat[2][2] * b;

        // RRT+ODT
        r2 = RRTAndODTFit(r2);
        g2 = RRTAndODTFit(g2);
        b2 = RRTAndODTFit(b2);

        // ACES output transform
        dst[i + 0] = ACESOutputMat[0][0] * r2 + ACESOutputMat[0][1] * g2 + ACESOutputMat[0][2] * b2;
        dst[i + 1] = ACESOutputMat[1][0] * r2 + ACESOutputMat[1][1] * g2 + ACESOutputMat[1][2] * b2;
        dst[i + 2] = ACESOutputMat[2][0] * r2 + ACESOutputMat[2][1] * g2 + ACESOutputMat[2][2] * b2;
    }
}

} // namespace ColorManagement