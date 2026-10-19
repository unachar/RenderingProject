#pragma once

#include <iosfwd>
#include <string>

namespace SimulationConfiguration
{
    void Save(ostream& stream);
    bool TryLoad(const string& key, const string& value);
} // 名前空間 SimulationConfiguration の終わり。
