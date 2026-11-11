#include "pch.h"
#include "forwardplussystem.h"

ForwardPlusSystem::Settings ForwardPlusSystem::s_Settings;

void ForwardPlusSystem::Init()
{
	auto& megaLights = MegaLights::GetSettings();
	megaLights.Enabled = megaLights.Enabled || s_Settings.Enabled;
	megaLights.Translucency = s_Settings.Enabled && s_Settings.UseClusteredLighting;
}
