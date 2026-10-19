#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace IesProfiles
{
    struct Profile
    {
        int PhotometricType = 1;
        std::vector<float> VerticalAngles;
        std::vector<float> HorizontalAngles;
        std::vector<float> Candela;
        float PeakCandela = 0;
    };

    namespace Detail
    {
        inline std::string Trim(const std::string& text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return {};
            }
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        inline bool ReadNumber(std::istream& stream, double& value)
        {
            return static_cast<bool>(stream >> value) && std::isfinite(value);
        }

        inline bool ReadAngles(std::istream& stream, std::vector<float>& angles, float maximum)
        {
            double previous = -1;
            for (float& angle : angles)
            {
                double value = 0;
                if (!ReadNumber(stream, value) || value < 0 || value > maximum || value <= previous)
                {
                    return false;
                }
                angle = static_cast<float>(value);
                if (previous >= 0 && angle <= static_cast<float>(previous))
                {
                    return false;
                }
                previous = value;
            }
            return true;
        }

        struct Interval
        {
            size_t First = 0;
            size_t Second = 0;
            float Weight = 0;
        };

        inline Interval FindInterval(const std::vector<float>& angles, float degrees)
        {
            const auto upper = std::upper_bound(angles.begin(), angles.end(), degrees);
            if (upper == angles.end())
            {
                return { angles.size() - 1, angles.size() - 1, 0 };
            }
            if (upper == angles.begin())
            {
                return {};
            }
            const size_t second = static_cast<size_t>(upper - angles.begin());
            const size_t first = second - 1;
            return { first, second, (degrees - angles[first]) / (angles[second] - angles[first]) };
        }
    }

    // Type C LM-63 data only. Photometric angles are in degrees; candela rows
    // are horizontal-major. Metadata, ballast factors and lumens do not change
    // the normalized angular mask used by this renderer.
    inline bool Parse(std::istream& input, Profile& output, std::string& error)
    {
        error.clear();
        const auto fail = [&error](const std::string& message)
        {
            error = message;
            return false;
        };

        constexpr size_t maximumTextBytes = 32 * 1024 * 1024;
        size_t textBytes = 0;
        bool foundTilt = false;
        std::string line;
        while (std::getline(input, line))
        {
            textBytes += line.size();
            if (textBytes > maximumTextBytes)
            {
                return fail("IES file exceeds the 32 MiB limit.");
            }
            const std::string trimmed = Detail::Trim(line);
            if (trimmed.rfind("TILT=", 0) == 0)
            {
                if (trimmed != "TILT=NONE")
                {
                    return fail("Unsupported IES TILT data. Only TILT=NONE is supported.");
                }
                foundTilt = true;
                break;
            }
        }
        if (!foundTilt)
        {
            return fail("IES file is missing TILT=NONE.");
        }

        std::string numericText;
        while (std::getline(input, line))
        {
            textBytes += line.size() + 1;
            if (textBytes > maximumTextBytes)
            {
                return fail("IES file exceeds the 32 MiB limit.");
            }
            numericText += line;
            numericText += '\n';
        }
        if (input.bad())
        {
            return fail("Unable to read IES file.");
        }
        std::replace(numericText.begin(), numericText.end(), ',', ' ');
        std::istringstream numbers(numericText);
        numbers.imbue(std::locale::classic());

        double header[13] = {};
        for (double& field : header)
        {
            if (!Detail::ReadNumber(numbers, field))
            {
                return fail("IES numeric header is truncated or contains invalid numbers.");
            }
        }
        const double verticalCount = header[3];
        const double horizontalCount = header[4];
        const auto validCount = [](double count)
        {
            return count >= 1 && count <= 65536 && std::floor(count) == count;
        };
        if (!validCount(verticalCount) || !validCount(horizontalCount)
            || verticalCount < 2 || verticalCount * horizontalCount > 4 * 1024 * 1024)
        {
            return fail("IES angle counts are invalid or exceed the supported size.");
        }
        if (header[5] != 1)
        {
            return fail("Unsupported IES photometric type. Only Type C (1) is supported; A/B are not supported.");
        }
        if (header[0] < 1 || std::floor(header[0]) != header[0]
            || (header[1] < 0 && header[1] != -1) || header[2] < 0
            || header[2] > std::numeric_limits<float>::max()
            || (header[6] != 1 && header[6] != 2)
            || header[10] < 0 || header[11] < 0 || header[12] < 0)
        {
            return fail("IES header has an invalid lamp count, multiplier, units or electrical value.");
        }

        Profile profile;
        profile.VerticalAngles.resize(static_cast<size_t>(verticalCount));
        profile.HorizontalAngles.resize(static_cast<size_t>(horizontalCount));
        if (!Detail::ReadAngles(numbers, profile.VerticalAngles, 180)
            || !Detail::ReadAngles(numbers, profile.HorizontalAngles, 360))
        {
            return fail("IES angles must be finite, strictly increasing and within their angular limits.");
        }
        const float horizontalEnd = profile.HorizontalAngles.back();
        const bool rotational = profile.HorizontalAngles.size() == 1 && horizontalEnd == 0;
        const bool supportedSymmetry = horizontalEnd == 90 || horizontalEnd == 180 || horizontalEnd == 360;
        if (profile.HorizontalAngles.front() != 0 || (!rotational && !supportedSymmetry))
        {
            return fail("Unsupported Type C horizontal range. Expected 0 alone, 0..90, 0..180 or 0..360 degrees.");
        }

        profile.Candela.resize(profile.VerticalAngles.size() * profile.HorizontalAngles.size());
        for (float& candela : profile.Candela)
        {
            double value = 0;
            if (!Detail::ReadNumber(numbers, value) || value < 0
                || value * header[2] > std::numeric_limits<float>::max())
            {
                return fail("IES candela table is truncated or has a negative, nonfinite or overflowing value.");
            }
            candela = static_cast<float>(value * header[2]);
            profile.PeakCandela = (std::max)(profile.PeakCandela, candela);
        }
        numbers >> std::ws;
        if (!numbers.eof())
        {
            return fail("IES file contains unexpected trailing data after the candela table.");
        }
        output = std::move(profile);
        return true;
    }

    inline bool Load(const std::filesystem::path& path, Profile& profile, std::string& error)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            error = "Unable to open IES profile file.";
            return false;
        }
        return Parse(stream, profile, error);
    }

    inline float Evaluate(const Profile& profile, float verticalDegrees, float horizontalDegrees)
    {
        if (profile.PhotometricType != 1 || profile.PeakCandela <= 0
            || !std::isfinite(profile.PeakCandela) || !std::isfinite(verticalDegrees)
            || !std::isfinite(horizontalDegrees) || profile.VerticalAngles.empty()
            || profile.HorizontalAngles.empty()
            || profile.Candela.size() / profile.VerticalAngles.size() != profile.HorizontalAngles.size()
            || profile.Candela.size() % profile.VerticalAngles.size() != 0)
        {
            return 0;
        }
        if (verticalDegrees < profile.VerticalAngles.front() || verticalDegrees > profile.VerticalAngles.back())
        {
            return 0;
        }

        float azimuth = std::fmod(horizontalDegrees, 360.0f);
        if (azimuth < 0)
        {
            azimuth += 360;
        }
        const float horizontalEnd = profile.HorizontalAngles.back();
        if (horizontalEnd == 0)
        {
            azimuth = 0;
        }
        else if (horizontalEnd <= 180)
        {
            if (azimuth > 180)
            {
                azimuth = 360 - azimuth;
            }
            if (horizontalEnd == 90 && azimuth > 90)
            {
                azimuth = 180 - azimuth;
            }
        }

        const auto vertical = Detail::FindInterval(profile.VerticalAngles, verticalDegrees);
        const auto horizontal = Detail::FindInterval(profile.HorizontalAngles, azimuth);
        const size_t rowSize = profile.VerticalAngles.size();
        const auto sampleRow = [&](size_t row)
        {
            const float first = profile.Candela[row * rowSize + vertical.First];
            const float second = profile.Candela[row * rowSize + vertical.Second];
            return first + (second - first) * vertical.Weight;
        };
        const float first = sampleRow(horizontal.First);
        const float second = sampleRow(horizontal.Second);
        const float normalized = (first + (second - first) * horizontal.Weight) / profile.PeakCandela;
        if (!std::isfinite(normalized))
        {
            return 0;
        }
        return (std::clamp)(normalized, 0.0f, 1.0f);
    }

    // Equirectangular mask: x is azimuth [0,360), y is polar [0,180].
    // Use horizontal wrap and vertical clamp when sampling the GPU texture.
    inline std::vector<float> Bake(const Profile& profile, unsigned width = 256, unsigned height = 128)
    {
        if (width == 0 || height == 0 || static_cast<uint64_t>(width) * height > 4 * 1024 * 1024)
        {
            return {};
        }
        std::vector<float> pixels(static_cast<size_t>(width) * height);
        for (unsigned row = 0; row < height; ++row)
        {
            const float vertical = (static_cast<float>(row) + 0.5f) * 180.0f / height;
            for (unsigned column = 0; column < width; ++column)
            {
                const float horizontal = (static_cast<float>(column) + 0.5f) * 360.0f / width;
                pixels[static_cast<size_t>(row) * width + column] = Evaluate(profile, vertical, horizontal);
            }
        }
        return pixels;
    }
}
