#pragma once
#include "modelresourcemap.h"
#include <json/json.h>
#include <charconv>

// Embed the same bounded binary snapshot in derived-geometry JSON manifests.
namespace ModelMapJson
{
inline Json::Value Write(const ModelResourceMap& map)
{
    Json::Value result;
    const auto fingerprint = map.Fingerprint("static-metadata-v1");
    std::ostringstream binary(std::ios::binary);
    if (!map.Write(binary, fingerprint)) return result;
    const char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : binary.str())
    {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    result["fingerprint"] = std::to_string(fingerprint);
    result["data"] = hex;
    return result;
}
inline bool Read(const Json::Value& value, ModelResourceMap& map)
{
    if (!value["data"].isString() || !value["fingerprint"].isString()) return false;
    const auto hex = value["data"].asString();
    if (hex.size() % 2 || hex.size() > 2 * (16 * 1024 * 1024 + 32)) return false;
    const auto fingerprintText = value["fingerprint"].asString();
    uint64_t fingerprint = 0;
    const auto parsed = std::from_chars(fingerprintText.data(), fingerprintText.data() + fingerprintText.size(), fingerprint);
    if (parsed.ec != std::errc{} || parsed.ptr != fingerprintText.data() + fingerprintText.size()) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const int high = digit(hex[i]), low = digit(hex[i+1]);
        if (high < 0 || low < 0) return false;
        bytes += static_cast<char>(high * 16 + low);
    }
    std::istringstream binary(bytes, std::ios::binary);
    return map.Read(binary, fingerprint);
}
}
