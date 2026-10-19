#include "../source/editor/configvalue.h"
#include <iostream>
#include <algorithm>

using std::clamp;
using std::max;
#include "../source/rendering/renderersettings.h"

enum class View
{
    First,
    Second
};

int main()
{
    int checks = 0;
    const auto check = [&](bool passed) {
        ++checks;
        if (!passed)
        {
            throw std::runtime_error("Editor configuration regression");
        }
    };
    unsigned int size = 192;
    std::ostringstream saved;
    EditorConfig::Value(&saved, {}, {}, "asset_thumbnail_size", size, 32, 256);
    size = 128;
    check(saved.str() == "asset_thumbnail_size=192\n");
    check(EditorConfig::Value(nullptr, "asset_thumbnail_size", "192", "asset_thumbnail_size", size, 32, 256));
    check(size == 192);
    for (const char* invalid : {"31", "257", "nan", "inf", "192 extra", "", "64.5"})
    {
        bool rejected = false;
        try
        {
            EditorConfig::Value(nullptr, "size", invalid, "size", size, 32, 256);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        check(rejected && size == 192);
    }
    check(!EditorConfig::Value(nullptr, "future_key", "invalid", "size", size, 32, 256));
    bool visible = false;
    check(EditorConfig::Value(nullptr, "visible", "1", "visible", visible) && visible);
    View view = View::First;
    check(EditorConfig::Value(nullptr, "view", "1", "view", view, 0, 1) && view == View::Second);
    float bias = 0;
    check(EditorConfig::Value(nullptr, "bias", "0.015", "bias", bias, 0, 1) && std::abs(bias - 0.015f) < 1e-6f);

    check(RendererSettings::GetIesProfilesEnabled());
    const uint64_t initialRevision = RendererSettings::GetRevision();
    RendererSettings::SetIesProfilesEnabled(false);
    check(!RendererSettings::GetIesProfilesEnabled());
    check(RendererSettings::GetRevision() == initialRevision + 1);
    RendererSettings::SetIesProfilesEnabled(false);
    check(RendererSettings::GetRevision() == initialRevision + 1);

    bool iesEnabled = RendererSettings::GetIesProfilesEnabled();
    std::ostringstream savedIes;
    EditorConfig::Value(&savedIes, {}, {}, "ies_profiles_enabled", iesEnabled);
    check(savedIes.str() == "ies_profiles_enabled=0\n");
    RendererSettings::SetIesProfilesEnabled(true);
    check(RendererSettings::GetRevision() == initialRevision + 2);
    check(EditorConfig::Value(nullptr, "ies_profiles_enabled", "0", "ies_profiles_enabled", iesEnabled));
    RendererSettings::SetIesProfilesEnabled(iesEnabled);
    check(!RendererSettings::GetIesProfilesEnabled());
    check(RendererSettings::GetRevision() == initialRevision + 3);
    bool invalidIesRejected = false;
    try
    {
        EditorConfig::Value(nullptr, "ies_profiles_enabled", "2", "ies_profiles_enabled", iesEnabled);
    }
    catch (const std::invalid_argument&)
    {
        invalidIesRejected = true;
    }
    check(invalidIesRejected && !iesEnabled);
    RendererSettings::SetIesProfilesEnabled(true);
    std::cout << "Editor configuration PASS: " << checks << " checks\n";
}
