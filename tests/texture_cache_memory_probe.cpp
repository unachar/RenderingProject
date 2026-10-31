#define NOMINMAX
#include "../source/assets/texturecachestorage.h"
#include <psapi.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 5) throw std::runtime_error("usage: memory_probe <input> <stored> <raw|stored> <new-report>");
        const std::filesystem::path input = argv[1], stored = argv[2], report = argv[4];
        if (std::filesystem::exists(report)) throw std::runtime_error("memory report must be new");
        const bool compressed = std::wstring(argv[3]) == L"stored";
        if (!compressed && std::wstring(argv[3]) != L"raw") throw std::runtime_error("unknown memory mode");
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(input))
            if (entry.is_regular_file() && entry.path().extension() == ".dds") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        uint64_t bcBytes = 0;
        for (const auto& file : files)
        {
            DirectX::ScratchImage image;
            DirectX::TexMetadata metadata{};
            const bool loaded = compressed ? TextureCacheStorage::Load(stored / file.filename(), metadata, image) :
                SUCCEEDED(DirectX::LoadFromDDSFile(file.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image));
            if (!loaded) throw std::runtime_error("memory probe load failed");
            bcBytes += image.GetPixelsSize();
        }
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (files.empty() || !GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            throw std::runtime_error("memory counters unavailable");
        std::ofstream stream(report);
        stream << "{\"mode\":\"" << (compressed ? "stored" : "raw") << "\",\"files\":" << files.size()
            << ",\"bcBytes\":" << bcBytes << ",\"peakWorkingSetBytes\":" << counters.PeakWorkingSetSize
            << ",\"peakPagefileUsageBytes\":" << counters.PeakPagefileUsage << "}\n";
        stream.close();
        if (stream.fail()) throw std::runtime_error("memory report write failed");
        std::cout << "PASS fresh-process " << (compressed ? "stored" : "raw") << " load; peakWS " << counters.PeakWorkingSetSize << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
