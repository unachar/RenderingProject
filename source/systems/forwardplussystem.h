#pragma once
#include "systembase.h"
#include "megalights.h"

class ForwardPlusSystem : public SystemBase
{
public:
    struct Settings
    {
        bool Enabled = true;
        bool UseClusteredLighting = true;
        bool EnableMSAA = false;
    };

    static Settings& GetSettings() { return s_Settings; }
    void Init() override;

private:
    static Settings s_Settings;
};