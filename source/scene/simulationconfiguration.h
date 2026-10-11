#pragma once

#include <iosfwd>
#include <string>

namespace SimulationConfiguration
{
void Save(std::ostream& stream);
bool TryLoad(const std::string& key, const std::string& value);
}
