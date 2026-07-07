#include "pch.h"
#include "atmosphere.h"

namespace
{
	AtmosphereParameters g_AtmosphereParameters{};
}

const AtmosphereParameters& Atmosphere::GetParameters()
{
	return g_AtmosphereParameters;
}

AtmosphereParameters& Atmosphere::GetMutableParameters()
{
	return g_AtmosphereParameters;
}

void Atmosphere::Reset()
{
	g_AtmosphereParameters = AtmosphereParameters{};
}
