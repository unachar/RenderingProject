#pragma once
#include "proceduralstudio.h"
#include <filesystem>
#include <string>

// Windows file dialogs and DirectXTex based image / volume I/O for the procedural tools.
namespace ProceduralIO
{
// filter: pairs separated by '\0', e.g. L"PNG (*.png)\0*.png\0". Returns false when cancelled.
bool SaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, const std::wstring& defaultName,
                    std::filesystem::path& result);
bool OpenFileDialog(const wchar_t* filter, std::filesystem::path& result);

enum class Encoding
{
    Color,  // linear values written as sRGB 8 bit
    Linear, // data (normal maps) written as 8 bit without gamma
    Height  // single channel, 16 bit where the container supports it
};
bool SaveImageFile(const std::filesystem::path& path, const ProcStudio::Image& image, Encoding encoding, std::string& error);
// Any WIC / DDS / TGA / HDR image; 3D DDS returns the first slice (use LoadVolume for volumes).
bool LoadImageFile(const std::filesystem::path& path, ProcStudio::Image& image, std::string& error);
bool SaveDensityVolume(const std::filesystem::path& path, const ProcStudio::Volume& volume, std::string& error);
bool SaveColorVolume(const std::filesystem::path& path, const std::vector<ProcStudio::Float4>& voxels, int extent,
                     std::string& error);
// Returns false (no error) when the file is not a 3D texture.
bool LoadVolume(const std::filesystem::path& path, ProcStudio::Volume& volume, std::string& error);
// Trilinear, wrapping resample to a cubic extent.
ProcStudio::Volume Resample(const ProcStudio::Volume& volume, int extent);
std::string ToUtf8(const std::filesystem::path& path);
} // namespace ProceduralIO
