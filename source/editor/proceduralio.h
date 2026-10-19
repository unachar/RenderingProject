#pragma once
#include "proceduralstudio.h"
#include <filesystem>
#include <string>

// プロシージャルツール用の Windows ファイルダイアログと DirectXTex による画像・ボリューム入出力。
namespace ProceduralIO
{
    // filter は '\0' 区切りの組。例: L"PNG (*.png)\0*.png\0"。キャンセル時は false。
    bool SaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, const std::wstring& defaultName,
                        std::filesystem::path& result);
    bool OpenFileDialog(const wchar_t* filter, std::filesystem::path& result);

    enum class Encoding
    {
        Color,  // 線形値を sRGB の 8 ビット値で書き込む。
        Linear, // 法線マップなどのデータはガンマ変換せず 8 ビットで書き込む。
        Height  // 単一チャンネル。格納形式が対応する場合は 16 ビット。
    };
    bool SaveImageFile(const std::filesystem::path& path, const ProcStudio::Image& image, Encoding encoding,
                       std::string& error);
    // WIC / DDS / TGA / HDR 画像に対応。3D DDS は先頭スライスを返す（ボリュームには LoadVolume を使う）。
    bool LoadImageFile(const std::filesystem::path& path, ProcStudio::Image& image, std::string& error);
    bool SaveDensityVolume(const std::filesystem::path& path, const ProcStudio::Volume& volume, std::string& error);
    bool SaveColorVolume(const std::filesystem::path& path, const std::vector<ProcStudio::Float4>& voxels, int extent,
                         std::string& error);
    // 3D テクスチャでない場合はエラーにせず false を返す。
    bool LoadVolume(const std::filesystem::path& path, ProcStudio::Volume& volume, std::string& error);
    // 折り返しと三線形補間を使って立方体サイズへ再サンプリングする。
    ProcStudio::Volume Resample(const ProcStudio::Volume& volume, int extent);
    std::string ToUtf8(const std::filesystem::path& path);
} // 名前空間 ProceduralIO の終わり。
