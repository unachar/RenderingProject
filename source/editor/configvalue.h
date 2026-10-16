#pragma once
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace EditorConfig
{
template<class T>
bool Value(std::ostream* output, const std::string& key, const std::string& text,
    const char* name, T& setting,
    long double minimum = std::numeric_limits<T>::lowest(),
    long double maximum = std::numeric_limits<T>::max())
{
    if (output)
    {
        *output << name << '=';
        if constexpr (std::is_enum_v<T>) *output << static_cast<int>(setting);
        else *output << setting;
        *output << '\n';
        return false;
    }
    if (key != name) return false;
    std::istringstream input(text);
    long double number;
    std::string extra;
    if (!(input >> number) || !std::isfinite(number) || (input >> extra) ||
        number < minimum || number > maximum)
        throw std::invalid_argument("Invalid editor configuration value");
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
        if (std::trunc(number) != number) throw std::invalid_argument("Expected integer setting");
    setting = static_cast<T>(number);
    return true;
}
}

