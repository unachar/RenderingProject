#include "assets/iesprofile.h"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace
{
    int Assertions = 0;

    void Check(bool condition, const std::string& description)
    {
        ++Assertions;
        if (!condition)
        {
            throw std::runtime_error(description);
        }
    }

    void Near(float actual, float expected, const std::string& description)
    {
        Check(std::isfinite(actual) && std::abs(actual - expected) < 0.0001f, description);
    }

    std::string Document(const std::string& counts, const std::string& anglesAndCandela,
        const std::string& multiplier = "2", const std::string& type = "1")
    {
        return "IESNA:LM-63-2002\r\n[TEST] Self authored test\r\nTILT=NONE\r\n1 -1 "
            + multiplier + " " + counts + " " + type + " 2 0 0 0\r\n1 1 10\r\n" + anglesAndCandela;
    }

    IesProfiles::Profile Read(const std::string& document)
    {
        std::istringstream stream(document);
        IesProfiles::Profile profile;
        std::string error;
        Check(IesProfiles::Parse(stream, profile, error), "Parse valid profile: " + error);
        Check(error.empty(), "Successful parse clears prior error");
        return profile;
    }

    void Reject(const std::string& document, const std::string& description)
    {
        std::istringstream stream(document);
        IesProfiles::Profile profile;
        std::string error;
        Check(!IesProfiles::Parse(stream, profile, error), description);
        Check(!error.empty(), description + " has diagnostic");
    }
}

int main()
{
    try
    {
        const auto rotational = Read(Document("4 1", "0,10,40,90\r\n0\r\n100,80,20,0"));
        Near(rotational.PeakCandela, 200, "Candela multiplier is applied");
        Near(rotational.Candela[1], 160, "Candela rows retain physical values");
        Near(IesProfiles::Evaluate(rotational, 25, 278), 0.5f, "Nonuniform vertical interpolation");
        Near(IesProfiles::Evaluate(rotational, 0, -720), 1, "Rotational symmetry");
        Near(IesProfiles::Evaluate(rotational, 91, 0), 0, "Missing upper hemisphere is dark");
        Near(IesProfiles::Evaluate(rotational, -1, 0), 0, "Negative polar direction is dark");
        Near(IesProfiles::Evaluate(rotational, NAN, 0), 0, "Invalid query is safe");

        const auto quadrant = Read(Document("2 3", "0 180 0 30 90 100 100 70 70 10 10"));
        Near(IesProfiles::Evaluate(quadrant, 45, 60), 0.4f, "Nonuniform horizontal interpolation");
        Near(IesProfiles::Evaluate(quadrant, 45, 120), 0.4f, "Second quadrant reflection");
        Near(IesProfiles::Evaluate(quadrant, 45, 240), 0.4f, "Third quadrant reflection");
        Near(IesProfiles::Evaluate(quadrant, 45, -60), 0.4f, "Negative azimuth reflection");

        const auto half = Read(Document("2 3", "0 180 0 60 180 100 100 80 80 20 20"));
        Near(IesProfiles::Evaluate(half, 90, 240), 0.5f, "Half symmetry reflects around 180");
        const auto full = Read(Document("2 4", "0 180 0 90 270 360 100 100 60 60 20 20 100 100"));
        Near(IesProfiles::Evaluate(full, 90, 180), 0.4f, "Full horizontal interpolation");
        Near(IesProfiles::Evaluate(full, 90, 360), 1, "Full azimuth wraps");
        Near(IesProfiles::Evaluate(full, 90, 315), 0.6f, "Full seam interpolation");

        const auto upper = Read(Document("2 1", "90 180 0 100 0"));
        Near(IesProfiles::Evaluate(upper, 89, 0), 0, "Missing lower hemisphere is dark");
        const auto dark = Read(Document("2 1", "0 180 0 0 0"));
        Near(IesProfiles::Evaluate(dark, 90, 0), 0, "All-zero profile stays finite");
        const auto baked = IesProfiles::Bake(full, 4, 2);
        Check(baked.size() == 8, "Bake dimensions");
        Near(baked[0], 0.8f, "Bake samples azimuth texel centers");
        Near(baked[2], 0.3f, "Bake preserves azimuth ordering");
        Check(IesProfiles::Bake(full, 0, 2).empty(), "Empty bake dimensions are safe");
        Check(IesProfiles::Bake(full).size() == 256 * 128, "Default bake dimensions");

        Reject("", "Missing TILT rejected");
        Reject("TILT=INCLUDE\n1 0 0", "Embedded TILT rejected");
        Reject("TILT=external.tlt\n", "External TILT rejected");
        Reject(Document("2 1", "0 180 0 1 1", "1", "2"), "Type B rejected explicitly");
        Reject(Document("2 1", "0 180 0 1 1", "1", "3"), "Type A rejected explicitly");
        Reject(Document("0 1", ""), "Zero angle count rejected");
        Reject(Document("2.5 1", ""), "Fractional count rejected");
        Reject(Document("2 999999999", ""), "Unbounded allocation rejected");
        Reject(Document("2 1", "0 180 0 100"), "Truncated candela rejected");
        Reject(Document("3 1", "0 0 180 0 100 100 100"), "Duplicate angles rejected");
        Reject(Document("3 1", "0 90 80 0 100 100 100"), "Descending angles rejected");
        Reject(Document("2 1", "0 181 0 100 100"), "Polar limits checked");
        Reject(Document("2 2", "0 180 0 45 100 100 100 100"), "Unsupported symmetry rejected");
        Reject(Document("2 1", "0 180 10 100 100"), "Horizontal start checked");
        Reject(Document("2 1", "0 180 0 -1 100"), "Negative intensity rejected");
        Reject(Document("2 1", "0 180 0 nan 100"), "Nonfinite intensity rejected");
        Reject(Document("2 1", "0 180 0 100 100", "-1"), "Negative multiplier rejected");
        Reject(Document("2 1", "0 180 0 100 100", "1e39"), "Overflow multiplier rejected");
        Reject(Document("2 1", "0 180 0 100 100 garbage"), "Trailing malformed data rejected");

        for (const auto* name : { "NarrowBeam.ies", "Asymmetric.ies" })
        {
            IesProfiles::Profile profile;
            std::string error = "previous failure";
            Check(IesProfiles::Load(std::filesystem::path("asset/Lighting/IES") / name, profile, error),
                std::string("Load sample ") + name + ": " + error);
            Check(error.empty(), "Load clears error");
            Check(profile.PeakCandela > 0, "Sample has nonzero illumination");
        }
        IesProfiles::Profile missing;
        std::string error;
        Check(!IesProfiles::Load("missing-profile.ies", missing, error) && !error.empty(), "Missing file diagnostic");
        std::cout << "IES profile tests passed (" << Assertions << " assertions).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "IES profile test failed: " << error.what() << '\n';
        return 1;
    }
}
