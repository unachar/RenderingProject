#pragma once
#include <cfileio.h>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include <string>

// Debug と Release では STL の配置が異なるため、DLL 境界は C のデータだけで渡す。
class ThumbnailMeshIO
{
  public:
    explicit ThumbnailMeshIO(std::vector<std::filesystem::path>& files) : m_Files(files) {}

    aiFileIO Callbacks()
    {
        aiFileIO callbacks{};
        callbacks.UserData = reinterpret_cast<char*>(this);
        callbacks.OpenProc = [](aiFileIO* io, const char* path, const char* mode) -> aiFile* {
            if (!path || !mode || mode[0] != 'r')
            {
                return nullptr;
            }
            try
            {
                return reinterpret_cast<ThumbnailMeshIO*>(io->UserData)->Open(path);
            }
            catch (...)
            {
                return nullptr;
            }
        };
        callbacks.CloseProc = [](aiFileIO*, aiFile* file) {
            if (!file)
            {
                return;
            }
            std::fclose(reinterpret_cast<FILE*>(file->UserData));
            delete file;
        };
        return callbacks;
    }

  private:
    aiFile* Open(const char* path)
    {
        const std::string bytes(path);
        const std::u8string utf8(bytes.begin(), bytes.end());
        const auto name = std::filesystem::absolute(std::filesystem::path(utf8)).lexically_normal();
        if (std::find(m_Files.begin(), m_Files.end(), name) == m_Files.end())
        {
            m_Files.push_back(name);
        }
        auto file = std::make_unique<aiFile>();
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, name.c_str(), L"rb") != 0)
        {
            return nullptr;
        }
        file->UserData = reinterpret_cast<char*>(stream);
        file->ReadProc = [](aiFile* file, char* output, size_t size, size_t count) {
            return std::fread(output, size, count, reinterpret_cast<FILE*>(file->UserData));
        };
        file->WriteProc = [](aiFile*, const char*, size_t, size_t) -> size_t {
            return 0;
        };
        file->TellProc = [](aiFile* file) -> size_t {
            return static_cast<size_t>(_ftelli64(reinterpret_cast<FILE*>(file->UserData)));
        };
        file->FileSizeProc = [](aiFile* file) -> size_t {
            auto* stream = reinterpret_cast<FILE*>(file->UserData);
            const auto position = _ftelli64(stream);
            _fseeki64(stream, 0, SEEK_END);
            const auto bytes = _ftelli64(stream);
            _fseeki64(stream, position, SEEK_SET);
            return bytes >= 0 ? static_cast<size_t>(bytes) : 0;
        };
        file->SeekProc = [](aiFile* file, size_t offset, aiOrigin origin) {
            if (offset > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
            {
                return aiReturn_FAILURE;
            }
            int anchor = SEEK_END;
            if (origin == aiOrigin_SET)
            {
                anchor = SEEK_SET;
            }
            else if (origin == aiOrigin_CUR)
            {
                anchor = SEEK_CUR;
            }
            const auto displacement =
                origin == aiOrigin_END ? -static_cast<int64_t>(offset) : static_cast<int64_t>(offset);
            return _fseeki64(reinterpret_cast<FILE*>(file->UserData), displacement, anchor) == 0 ? aiReturn_SUCCESS
                                                                                                 : aiReturn_FAILURE;
        };
        file->FlushProc = [](aiFile*) {
        };
        return file.release();
    }

    std::vector<std::filesystem::path>& m_Files;
};
