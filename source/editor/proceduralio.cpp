#include "pch.h"
#include "proceduralio.h"
#include <DirectXTex.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <cmath>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace ProceduralIO
{
    namespace
    {
        uint8_t ToByte(float value)
        {
            return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        float LinearToSrgb(float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
        }

        float SrgbToLinear(float value)
        {
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }

        wstring Lower(wstring text)
        {
            for (auto& c : text)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            return text;
        }

        // プロシージャル用の保存・読み込みダイアログは、常にプロジェクトルート
        // （エディターを実行する Render フォルダ）から開き、Windows が記憶した直前のフォルダに依存させない。
        filesystem::path DialogRoot()
        {
            static const filesystem::path root = [] {
                error_code error;
                auto path = filesystem::current_path(error);
                return error ? filesystem::path() : path;
            }();
            return root;
        }

        vector<COMDLG_FILTERSPEC> ParseFilter(const wchar_t* filter, vector<wstring>& storage)
        {
            storage.clear();
            for (const wchar_t* cursor = filter; cursor && *cursor; cursor += wcslen(cursor) + 1)
            {
                storage.emplace_back(cursor);
            }
            vector<COMDLG_FILTERSPEC> specs;
            for (size_t i = 0; i + 1 < storage.size(); i += 2)
            {
                specs.push_back({storage[i].c_str(), storage[i + 1].c_str()});
            }
            return specs;
        }

        // Common Item Dialog の SetFolder で開始フォルダを固定する。GetSaveFileName の lpstrInitialDir は、
        // 2 回目以降、Windows のアプリ別 MRU に上書きされる。
        // 戻り値は 1 が確定、0 がキャンセル、-1 が利用不可（呼び出し元で旧ダイアログへ切り替える）。
        int RunItemDialog(bool save, const wchar_t* filter, const wchar_t* extension, const wstring& name,
                          filesystem::path& result)
        {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const bool uninitialize = SUCCEEDED(apartment);

            struct Scope
            {
                bool Active;

                ~Scope()
                {
                    if (Active)
                    {
                        CoUninitialize();
                    }
                }
            } scope{uninitialize};

            ComPtr<IFileDialog> dialog;
            if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                return -1;
            }
            vector<wstring> storage;
            const auto specs = ParseFilter(filter, storage);
            if (!specs.empty())
            {
                dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
                dialog->SetFileTypeIndex(1);
            }
            if (extension)
            {
                dialog->SetDefaultExtension(extension);
            }
            if (!name.empty())
            {
                dialog->SetFileName(name.c_str());
            }
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            options |= FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
            options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
            dialog->SetOptions(options);
            const auto root = DialogRoot();
            ComPtr<IShellItem> folder;
            if (!root.empty() && SUCCEEDED(SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            {
                dialog->SetFolder(folder.Get());
            }
            const HRESULT shown = dialog->Show(GetActiveWindow());
            if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return 0;
            }
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (FAILED(shown) || FAILED(dialog->GetResult(&item)) ||
                FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                return FAILED(shown) ? -1 : 0;
            }
            result = path;
            CoTaskMemFree(path);
            // 保存ダイアログは、種類フィルターが一致するときだけ既定の拡張子を付ける。
            if (save && extension && result.extension().empty())
            {
                result += L".";
                result += extension;
            }
            return 1;
        }

        bool Run(bool save, const wchar_t* filter, const wchar_t* extension, const wstring& name,
                 filesystem::path& result)
        {
            const int modern = RunItemDialog(save, filter, extension, name, result);
            if (modern >= 0)
            {
                return modern == 1;
            }
            // 旧ダイアログでは、lpstrFile に絶対パスを指定する方法だけが開始フォルダを確実に選べる。
            const auto root = DialogRoot();
            wstring initial = save && !name.empty() && !root.empty() ? (root / name).wstring() : name;
            wchar_t buffer[MAX_PATH * 4] = {};
            if (!initial.empty())
            {
                wcsncpy_s(buffer, initial.c_str(), _TRUNCATE);
            }
            const wstring rootText = root.wstring();
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = GetActiveWindow();
            dialog.lpstrFilter = filter;
            dialog.lpstrFile = buffer;
            dialog.nMaxFile = static_cast<DWORD>(size(buffer));
            dialog.lpstrDefExt = extension;
            dialog.lpstrInitialDir = rootText.empty() ? nullptr : rootText.c_str();
            // NOCHANGEDIR: エンジンは作業ディレクトリを基準にアセットを解決する。
            dialog.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER |
                           (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
            const BOOL accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
            if (!accepted)
            {
                return false;
            }
            result = buffer;
            return true;
        }
    }

    bool SaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, const wstring& defaultName,
                        filesystem::path& result)
    {
        return Run(true, filter, defaultExtension, defaultName, result);
    }

    bool OpenFileDialog(const wchar_t* filter, filesystem::path& result)
    {
        return Run(false, filter, nullptr, L"", result);
    }

    string ToUtf8(const filesystem::path& path)
    {
        const auto text = path.u8string();
        return string(text.begin(), text.end());
    }

    bool SaveImageFile(const filesystem::path& path, const ProcStudio::Image& image, Encoding encoding, string& error)
    {
        const wstring extension = Lower(path.extension().wstring());
        const bool sixteen = encoding == Encoding::Height && (extension == L".png" || extension == L".dds" ||
                                                              extension == L".tif" || extension == L".tiff");
        ScratchImage scratch;
        const DXGI_FORMAT format = sixteen ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(scratch.Initialize2D(format, image.Width, image.Height, 1, 1)))
        {
            error = "画像バッファを確保できません";
            return false;
        }
        const auto* target = scratch.GetImage(0, 0, 0);
        for (int y = 0; y < image.Height; ++y)
        {
            uint8_t* row = target->pixels + static_cast<size_t>(y) * target->rowPitch;
            for (int x = 0; x < image.Width; ++x)
            {
                const auto& p = image.Pixels[static_cast<size_t>(y) * image.Width + x];
                if (sixteen)
                {
                    reinterpret_cast<uint16_t*>(row)[x] =
                        static_cast<uint16_t>(std::clamp(p.X, 0.0f, 1.0f) * 65535.0f + 0.5f);
                    continue;
                }
                const bool gamma = encoding == Encoding::Color;
                row[x * 4 + 0] = ToByte(gamma ? LinearToSrgb(p.X) : p.X);
                row[x * 4 + 1] = ToByte(gamma ? LinearToSrgb(p.Y) : p.Y);
                row[x * 4 + 2] = ToByte(gamma ? LinearToSrgb(p.Z) : p.Z);
                row[x * 4 + 3] = ToByte(p.W);
            }
        }
        HRESULT result = E_FAIL;
        if (extension == L".dds")
        {
            result = SaveToDDSFile(*target, DDS_FLAGS_NONE, path.c_str());
        }
        else if (extension == L".tga")
        {
            result = SaveToTGAFile(*target, path.c_str());
        }
        else
        {
            const auto codec = extension == L".jpg" || extension == L".jpeg"   ? WIC_CODEC_JPEG
                               : extension == L".bmp"                          ? WIC_CODEC_BMP
                               : extension == L".tif" || extension == L".tiff" ? WIC_CODEC_TIFF
                                                                               : WIC_CODEC_PNG;
            result = SaveToWICFile(*target, WIC_FLAGS_NONE, GetWICCodec(codec), path.c_str());
        }
        if (FAILED(result))
        {
            error = "画像の保存に失敗しました (HRESULT 0x" + [&] {
                char text[16];
                snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(result));
                return string(text);
            }() + ")";
            return false;
        }
        return true;
    }

    namespace
    {
        bool LoadScratch(const filesystem::path& path, ScratchImage& converted, TexMetadata& metadata, string& error)
        {
            const wstring extension = Lower(path.extension().wstring());
            ScratchImage loaded;
            HRESULT result = E_FAIL;
            if (extension == L".dds")
            {
                result = LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, &metadata, loaded);
            }
            else if (extension == L".tga")
            {
                result = LoadFromTGAFile(path.c_str(), &metadata, loaded);
            }
            else if (extension == L".hdr")
            {
                result = LoadFromHDRFile(path.c_str(), &metadata, loaded);
            }
            else
            {
                result = LoadFromWICFile(path.c_str(), WIC_FLAGS_NONE, &metadata, loaded);
            }
            if (FAILED(result))
            {
                error = "画像を読み込めません";
                return false;
            }
            const DXGI_FORMAT target = DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (IsCompressed(metadata.format))
            {
                result = Decompress(loaded.GetImages(), loaded.GetImageCount(), metadata, target, converted);
            }
            else if (metadata.format != target)
            {
                result = Convert(loaded.GetImages(), loaded.GetImageCount(), metadata, target, TEX_FILTER_DEFAULT,
                                 TEX_THRESHOLD_DEFAULT, converted);
            }
            else
            {
                converted = std::move(loaded);
                result = S_OK;
            }
            if (FAILED(result))
            {
                error = "画像フォーマットを変換できません";
                return false;
            }
            return true;
        }
    }

    bool LoadImageFile(const filesystem::path& path, ProcStudio::Image& image, string& error)
    {
        ScratchImage converted;
        TexMetadata metadata{};
        if (!LoadScratch(path, converted, metadata, error))
        {
            return false;
        }
        const bool srgb =
            IsSRGB(metadata.format) || (!IsCompressed(metadata.format) && BitsPerColor(metadata.format) <= 8 &&
                                        Lower(path.extension().wstring()) != L".hdr");
        const auto* source = converted.GetImage(0, 0, 0);
        image.Width = static_cast<int>(source->width);
        image.Height = static_cast<int>(source->height);
        image.Pixels.resize(static_cast<size_t>(image.Width) * image.Height);
        for (int y = 0; y < image.Height; ++y)
        {
            const auto* row =
                reinterpret_cast<const float*>(source->pixels + static_cast<size_t>(y) * source->rowPitch);
            for (int x = 0; x < image.Width; ++x)
            {
                const float* p = row + x * 4;
                image.Pixels[static_cast<size_t>(y) * image.Width + x] = {srgb ? SrgbToLinear(p[0]) : p[0],
                                                                          srgb ? SrgbToLinear(p[1]) : p[1],
                                                                          srgb ? SrgbToLinear(p[2]) : p[2], p[3]};
            }
        }
        return true;
    }

    bool SaveDensityVolume(const filesystem::path& path, const ProcStudio::Volume& volume, string& error)
    {
        ScratchImage scratch;
        const size_t n = static_cast<size_t>(volume.Extent);
        if (FAILED(scratch.Initialize3D(DXGI_FORMAT_R8_UNORM, n, n, n, 1)))
        {
            error = "ボリュームを確保できません";
            return false;
        }
        for (size_t z = 0; z < n; ++z)
        {
            const auto* slice = scratch.GetImage(0, 0, z);
            for (size_t y = 0; y < n; ++y)
            {
                for (size_t x = 0; x < n; ++x)
                {
                    slice->pixels[y * slice->rowPitch + x] = ToByte(volume.Density[(z * n + y) * n + x]);
                }
            }
        }
        if (FAILED(SaveToDDSFile(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(), DDS_FLAGS_NONE,
                                 path.c_str())))
        {
            error = "DDS の保存に失敗しました";
            return false;
        }
        return true;
    }

    bool SaveColorVolume(const filesystem::path& path, const vector<ProcStudio::Float4>& voxels, int extent,
                         string& error)
    {
        ScratchImage scratch;
        const size_t n = static_cast<size_t>(extent);
        if (voxels.size() != n * n * n || FAILED(scratch.Initialize3D(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, n, n, n, 1)))
        {
            error = "ボリュームを確保できません";
            return false;
        }
        for (size_t z = 0; z < n; ++z)
        {
            const auto* slice = scratch.GetImage(0, 0, z);
            for (size_t y = 0; y < n; ++y)
            {
                for (size_t x = 0; x < n; ++x)
                {
                    const auto& v = voxels[(z * n + y) * n + x];
                    uint8_t* p = slice->pixels + y * slice->rowPitch + x * 4;
                    p[0] = ToByte(LinearToSrgb(v.X));
                    p[1] = ToByte(LinearToSrgb(v.Y));
                    p[2] = ToByte(LinearToSrgb(v.Z));
                    p[3] = ToByte(v.W);
                }
            }
        }
        if (FAILED(SaveToDDSFile(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(), DDS_FLAGS_NONE,
                                 path.c_str())))
        {
            error = "DDS の保存に失敗しました";
            return false;
        }
        return true;
    }

    bool LoadVolume(const filesystem::path& path, ProcStudio::Volume& volume, string& error)
    {
        if (Lower(path.extension().wstring()) != L".dds")
        {
            return false;
        }
        TexMetadata header{};
        if (FAILED(GetMetadataFromDDSFile(path.c_str(), DDS_FLAGS_NONE, header)) ||
            header.dimension != TEX_DIMENSION_TEXTURE3D)
        {
            return false;
        }
        ScratchImage converted;
        TexMetadata metadata{};
        if (!LoadScratch(path, converted, metadata, error))
        {
            return false;
        }
        ProcStudio::Volume source;
        const size_t w = metadata.width;
        const size_t h = metadata.height;
        const size_t d = metadata.depth;
        // 立方体でないボリュームは、最長辺の立方体を中間データとして再サンプリングする。
        const size_t n = (std::max)({w, h, d});
        source.Extent = static_cast<int>(n);
        source.Density.resize(n * n * n);
        for (size_t z = 0; z < n; ++z)
        {
            const auto* slice = converted.GetImage(0, 0, (std::min)(z * d / n, d - 1));
            for (size_t y = 0; y < n; ++y)
            {
                const auto* row =
                    reinterpret_cast<const float*>(slice->pixels + (std::min)(y * h / n, h - 1) * slice->rowPitch);
                for (size_t x = 0; x < n; ++x)
                {
                    const float* p = row + (std::min)(x * w / n, w - 1) * 4;
                    // 単一チャンネルのボリュームは R を複製し、カラーの場合は輝度を使う。
                    const bool single =
                        (header.format == DXGI_FORMAT_R8_UNORM || header.format == DXGI_FORMAT_R16_UNORM ||
                         header.format == DXGI_FORMAT_R16_FLOAT || header.format == DXGI_FORMAT_R32_FLOAT ||
                         header.format == DXGI_FORMAT_BC4_UNORM);
                    source.Density[(z * n + y) * n + x] =
                        std::clamp(single ? p[0] : p[0] * 0.2126f + p[1] * 0.7152f + p[2] * 0.0722f, 0.0f, 1.0f);
                }
            }
        }
        volume = std::move(source);
        return true;
    }

    ProcStudio::Volume Resample(const ProcStudio::Volume& volume, int extent)
    {
        if (volume.Extent == extent || volume.Extent <= 0)
        {
            return volume;
        }
        ProcStudio::Volume result;
        result.Extent = extent;
        const size_t n = static_cast<size_t>(extent);
        const size_t m = static_cast<size_t>(volume.Extent);
        result.Density.resize(n * n * n);
        auto at = [&](long long x, long long y, long long z) {
            const long long e = static_cast<long long>(m);
            x = ((x % e) + e) % e;
            y = ((y % e) + e) % e;
            z = ((z % e) + e) % e;
            return volume.Density[(static_cast<size_t>(z) * m + static_cast<size_t>(y)) * m + static_cast<size_t>(x)];
        };
        for (size_t z = 0; z < n; ++z)
        {
            for (size_t y = 0; y < n; ++y)
            {
                for (size_t x = 0; x < n; ++x)
                {
                    const float fx = (x + 0.5f) * m / n - 0.5f;
                    const float fy = (y + 0.5f) * m / n - 0.5f;
                    const float fz = (z + 0.5f) * m / n - 0.5f;
                    const long long x0 = static_cast<long long>(std::floor(fx));
                    const long long y0 = static_cast<long long>(std::floor(fy));
                    const long long z0 = static_cast<long long>(std::floor(fz));
                    const float tx = fx - x0;
                    const float ty = fy - y0;
                    const float tz = fz - z0;
                    auto lerp = [](float a, float b, float t) {
                        return a + (b - a) * t;
                    };
                    const float c00 = lerp(at(x0, y0, z0), at(x0 + 1, y0, z0), tx);
                    const float c10 = lerp(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), tx);
                    const float c01 = lerp(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), tx);
                    const float c11 = lerp(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), tx);
                    result.Density[(z * n + y) * n + x] = lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
                }
            }
        }
        return result;
    }
} // 名前空間 ProceduralIO の終わり。
