#pragma once
// ============================================================================
// SceneFile : .scene ファイル（JSON）の読み書きとシーンの変更検知用ハッシュ。ヘッダーのみ。
//
//  1つの「項目の列挙関数（Visit）」を、書き込み／読み込み／ハッシュの3通りに使う。
//  そのため、保存する項目と変更検知の対象が必ず一致する。
//  ・読み込みでキーがない・型が違う場合は既定値のまま（古いファイルや手書き編集に強い）
//  ・実行時にしか意味のない値（GPUバッファ、テクスチャID、行列、再生位置など）は保存しない
// ============================================================================
#include "componentmanager.h"
#include "transformmath.h"
#include "physicscomponent.h"
#include "timelinecomponent.h"
#include "proceduralcomponents.h"
#include "materialasset.h"
#include <json/json.h>
#include <type_traits>
#include <unordered_map>
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace SceneFile
{
    inline constexpr int kVersion = 1;
    inline constexpr const char* kExtension = ".scene";

    // ---------------------------------------------------------------- Base64（メッシュ頂点の保存用）
    inline std::string EncodeBase64(const void* data, size_t size)
    {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto* bytes = static_cast<const uint8_t*>(data);
        std::string out;
        out.reserve((size + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < size; i += 3)
        {
            const uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
            out.push_back(table[(v >> 18) & 63]);
            out.push_back(table[(v >> 12) & 63]);
            out.push_back(table[(v >> 6) & 63]);
            out.push_back(table[v & 63]);
        }
        if (i < size)
        {
            uint32_t v = bytes[i] << 16;
            if (i + 1 < size)
            {
                v |= bytes[i + 1] << 8;
            }
            out.push_back(table[(v >> 18) & 63]);
            out.push_back(table[(v >> 12) & 63]);
            out.push_back(i + 1 < size ? table[(v >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    inline bool DecodeBase64(const std::string& text, std::vector<uint8_t>& out)
    {
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z')
            {
                return c - 'A';
            }
            if (c >= 'a' && c <= 'z')
            {
                return c - 'a' + 26;
            }
            if (c >= '0' && c <= '9')
            {
                return c - '0' + 52;
            }
            if (c == '+')
            {
                return 62;
            }
            if (c == '/')
            {
                return 63;
            }
            return -1;
        };
        out.clear();
        if (text.size() % 4 != 0)
        {
            return false;
        }
        out.reserve(text.size() / 4 * 3);
        for (size_t i = 0; i < text.size(); i += 4)
        {
            int v[4];
            for (int k = 0; k < 4; ++k)
            {
                v[k] = text[i + k] == '=' ? 0 : value(text[i + k]);
                if (v[k] < 0)
                {
                    return false;
                }
            }
            const uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
            out.push_back(static_cast<uint8_t>(n >> 16));
            if (text[i + 2] != '=')
            {
                out.push_back(static_cast<uint8_t>(n >> 8));
            }
            if (text[i + 3] != '=')
            {
                out.push_back(static_cast<uint8_t>(n));
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- 3モード共通のアーカイブ
    // Stream：Json::Value を作らず、文字列へ直接 JSON を書き出す（保存の高速化。出力は Write と同じ JSON として読める）
    enum class Mode
    {
        Write,
        Read,
        Hash,
        Stream
    };

    class Archive
    {
      public:
        // Write/Read 用
        Archive(Mode mode, Json::Value& node) : m_Mode(mode), m_Node(&node) {}

        // Hash 用
        explicit Archive(uint64_t& hash) : m_Mode(Mode::Hash), m_Hash(&hash) {}

        // Stream 用：呼び出し側が '{' を書いてから渡し、終わったら '}' を書く
        explicit Archive(std::string& out) : m_Mode(Mode::Stream), m_Out(&out) {}

        Mode GetMode() const
        {
            return m_Mode;
        }

        bool Writing() const
        {
            return m_Mode == Mode::Write || m_Mode == Mode::Stream;
        }

        bool Reading() const
        {
            return m_Mode == Mode::Read;
        }

        bool Hashing() const
        {
            return m_Mode == Mode::Hash;
        }

        // エンティティ参照（EntityID）の付け替え。保存時：実ID→ファイル内番号、読み込み時：ファイル内番号→新ID
        const std::unordered_map<EntityID, EntityID>* EntityMap = nullptr;

        template <typename T> void operator()(const char* key, T& value)
        {
            if (m_Mode == Mode::Hash)
            {
                HashValue(key, value);
                return;
            }
            if (m_Mode == Mode::Stream)
            {
                StreamKey(key);
                StreamValue(value);
                return;
            }
            if (m_Mode == Mode::Write)
            {
                (*m_Node)[key] = ToJson(value);
                return;
            }
            if (m_Node->isObject() && m_Node->isMember(key))
            {
                FromJson((*m_Node)[key], value);
            }
        }

        // エンティティ参照
        void Entity(const char* key, EntityID& entity)
        {
            EntityID mapped = entity;
            if (m_Mode != Mode::Read)
            {
                mapped = Map(entity);
                if (m_Mode == Mode::Hash)
                {
                    HashBytes(&mapped, sizeof(mapped));
                    return;
                }
                if (m_Mode == Mode::Stream)
                {
                    StreamKey(key);
                    if (mapped == g_kINVALID_ENTITY)
                    {
                        m_Out->append("-1");
                    }
                    else
                    {
                        StreamInteger(static_cast<uint64_t>(mapped));
                    }
                    return;
                }
                (*m_Node)[key] =
                    mapped == g_kINVALID_ENTITY ? Json::Value(-1) : Json::Value(static_cast<Json::UInt>(mapped));
                return;
            }
            if (!m_Node->isMember(key))
            {
                return;
            }
            const Json::Value& v = (*m_Node)[key];
            entity = (v.isInt() && v.asInt() < 0) || !v.isUInt() ? g_kINVALID_ENTITY
                                                                 : Map(static_cast<EntityID>(v.asUInt()));
        }

        // 子オブジェクト
        template <typename Function> void Object(const char* key, Function&& function)
        {
            if (m_Mode == Mode::Hash)
            {
                HashString(key);
                function(*this);
                return;
            }
            if (m_Mode == Mode::Stream)
            {
                StreamKey(key);
                m_Out->push_back('{');
                Archive sub(*m_Out);
                sub.EntityMap = EntityMap;
                function(sub);
                m_Out->push_back('}');
                return;
            }
            if (m_Mode == Mode::Write)
            {
                Json::Value child(Json::objectValue);
                Archive sub(Mode::Write, child);
                sub.EntityMap = EntityMap;
                function(sub);
                (*m_Node)[key] = child;
                return;
            }
            if (!m_Node->isMember(key) || !(*m_Node)[key].isObject())
            {
                return;
            }
            Json::Value& child = (*m_Node)[key];
            Archive sub(Mode::Read, child);
            sub.EntityMap = EntityMap;
            function(sub);
        }

        // 構造体の配列（vector）
        template <typename T, typename Function> void Array(const char* key, std::vector<T>& items, Function&& function)
        {
            if (m_Mode == Mode::Hash)
            {
                uint64_t count = items.size();
                HashBytes(&count, sizeof(count));
                for (auto& item : items)
                {
                    function(*this, item);
                }
                return;
            }
            if (m_Mode == Mode::Stream)
            {
                StreamList(key, items.begin(), items.end(), function, EntityMap);
                return;
            }
            if (m_Mode == Mode::Write)
            {
                Json::Value list(Json::arrayValue);
                for (auto& item : items)
                {
                    Json::Value child(Json::objectValue);
                    Archive sub(Mode::Write, child);
                    sub.EntityMap = EntityMap;
                    function(sub, item);
                    list.append(child);
                }
                (*m_Node)[key] = list;
                return;
            }
            if (!m_Node->isMember(key) || !(*m_Node)[key].isArray())
            {
                return;
            }
            Json::Value& list = (*m_Node)[key];
            items.clear();
            items.resize(list.size());
            for (Json::ArrayIndex i = 0; i < list.size(); ++i)
            {
                if (!list[i].isObject())
                {
                    continue;
                }
                Archive sub(Mode::Read, list[i]);
                sub.EntityMap = EntityMap;
                function(sub, items[i]);
            }
        }

        // 固定長配列の構造体
        template <typename T, size_t N, typename Function>
        void FixedArray(const char* key, T (&items)[N], Function&& function)
        {
            if (m_Mode == Mode::Hash)
            {
                for (auto& item : items)
                {
                    function(*this, item);
                }
                return;
            }
            if (m_Mode == Mode::Stream)
            {
                StreamList(key, std::begin(items), std::end(items), function, nullptr);
                return;
            }
            if (m_Mode == Mode::Write)
            {
                Json::Value list(Json::arrayValue);
                for (auto& item : items)
                {
                    Json::Value child(Json::objectValue);
                    Archive sub(Mode::Write, child);
                    function(sub, item);
                    list.append(child);
                }
                (*m_Node)[key] = list;
                return;
            }
            if (!m_Node->isMember(key) || !(*m_Node)[key].isArray())
            {
                return;
            }
            Json::Value& list = (*m_Node)[key];
            for (Json::ArrayIndex i = 0; i < list.size() && i < N; ++i)
            {
                if (!list[i].isObject())
                {
                    continue;
                }
                Archive sub(Mode::Read, list[i]);
                function(sub, items[i]);
            }
        }

        void HashBytes(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            uint64_t h = *m_Hash;
            for (size_t i = 0; i < size; ++i)
            {
                h ^= bytes[i];
                h *= 1099511628211ull;
            }
            *m_Hash = h;
        }

        void HashString(const std::string& text)
        {
            uint64_t size = text.size();
            HashBytes(&size, sizeof(size));
            HashBytes(text.data(), text.size());
        }

      private:
        Mode m_Mode;
        Json::Value* m_Node = nullptr;
        uint64_t* m_Hash = nullptr;
        std::string* m_Out = nullptr;
        bool m_HasField = false; // Stream：このオブジェクトに 1 つ以上書いたか（',' の要否）

        // ---- Stream（直接書き出し）
        template <typename Iterator, typename Function>
        void StreamList(const char* key, Iterator first, Iterator last, Function& function,
                        const std::unordered_map<EntityID, EntityID>* map)
        {
            StreamKey(key);
            m_Out->push_back('[');
            bool firstItem = true;
            for (auto it = first; it != last; ++it)
            {
                if (!firstItem)
                {
                    m_Out->push_back(',');
                }
                firstItem = false;
                m_Out->push_back('{');
                Archive sub(*m_Out);
                sub.EntityMap = map;
                function(sub, *it);
                m_Out->push_back('}');
            }
            m_Out->push_back(']');
        }

        void StreamKey(const char* key)
        {
            if (m_HasField)
            {
                m_Out->push_back(',');
            }
            m_HasField = true;
            StreamString(key);
            m_Out->push_back(':');
        }

        void StreamString(const char* text, size_t length)
        {
            std::string& out = *m_Out;
            out.push_back('"');
            for (size_t i = 0; i < length; ++i)
            {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                switch (c)
                {
                    case '"':
                        out.append("\\\"");
                        break;
                    case '\\':
                        out.append("\\\\");
                        break;
                    case '\n':
                        out.append("\\n");
                        break;
                    case '\r':
                        out.append("\\r");
                        break;
                    case '\t':
                        out.append("\\t");
                        break;
                    default:
                        if (c < 0x20)
                        {
                            char buffer[8];
                            const int written = snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                            out.append(buffer, static_cast<size_t>(written));
                        }
                        else
                        {
                            out.push_back(static_cast<char>(c)); // UTF-8 はそのまま
                        }
                }
            }
            out.push_back('"');
        }

        void StreamString(const char* text)
        {
            StreamString(text, std::strlen(text));
        }

        void StreamString(const std::string& text)
        {
            StreamString(text.data(), text.size());
        }

        void StreamInteger(uint64_t value)
        {
            char buffer[32];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            m_Out->append(buffer, result.ptr);
        }

        void StreamInteger(int64_t value)
        {
            char buffer[32];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            m_Out->append(buffer, result.ptr);
        }

        void StreamFloat(double value)
        {
            if (!std::isfinite(value))
            {
                m_Out->push_back('0');
                return;
            }
            // 最短の往復表現（float は float として最短。読み込み時に同じ値に戻る）
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            m_Out->append(buffer, result.ptr);
        }

        void StreamFloat(float value)
        {
            if (!std::isfinite(value))
            {
                m_Out->push_back('0');
                return;
            }
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            m_Out->append(buffer, result.ptr);
        }

        template <typename T> void StreamValue(const T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                m_Out->append(value ? "true" : "false");
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                StreamString(value);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                StreamInteger(static_cast<int64_t>(value));
            }
            else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
            {
                StreamFloat(value);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                StreamString(FormatAssetHandle(value));
            }
            else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            {
                StreamInteger(static_cast<int64_t>(value));
            }
            else if constexpr (std::is_integral_v<T>)
            {
                StreamInteger(static_cast<uint64_t>(value));
            }
            else if constexpr (std::is_same_v<T, XMFLOAT2>)
            {
                StreamFloats({value.x, value.y});
            }
            else if constexpr (std::is_same_v<T, XMFLOAT3>)
            {
                StreamFloats({value.x, value.y, value.z});
            }
            else if constexpr (std::is_same_v<T, XMFLOAT4>)
            {
                StreamFloats({value.x, value.y, value.z, value.w});
            }
            else if constexpr (std::is_same_v<T, std::vector<AssetHandle>>)
            {
                m_Out->push_back('[');
                for (size_t i = 0; i < value.size(); ++i)
                {
                    if (i)
                    {
                        m_Out->push_back(',');
                    }
                    StreamString(FormatAssetHandle(value[i]));
                }
                m_Out->push_back(']');
            }
            else if constexpr (is_std_array<T>::value || std::is_same_v<T, std::vector<std::string>> ||
                               std::is_same_v<T, std::vector<float>> || is_enum_vector<T>::value)
            {
                m_Out->push_back('[');
                bool first = true;
                for (const auto& item : value)
                {
                    if (!first)
                    {
                        m_Out->push_back(',');
                    }
                    first = false;
                    StreamValue(item);
                }
                m_Out->push_back(']');
            }
            else
            {
                static_assert(sizeof(T) == 0, "unsupported scene field type");
            }
        }

        void StreamFloats(std::initializer_list<float> values)
        {
            m_Out->push_back('[');
            bool first = true;
            for (float v : values)
            {
                if (!first)
                {
                    m_Out->push_back(',');
                }
                first = false;
                StreamFloat(v);
            }
            m_Out->push_back(']');
        }

        EntityID Map(EntityID entity) const
        {
            if (entity == g_kINVALID_ENTITY || !EntityMap)
            {
                return entity;
            }
            const auto it = EntityMap->find(entity);
            return it == EntityMap->end() ? g_kINVALID_ENTITY : it->second;
        }

        // ---- ハッシュ
        template <typename T> void HashValue(const char*, T& value)
        {
            if constexpr (std::is_same_v<T, std::string>)
            {
                HashString(value);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::string>>)
            {
                for (auto& s : value)
                {
                    HashString(s);
                }
                uint64_t n = value.size();
                HashBytes(&n, sizeof(n));
            }
            else if constexpr (std::is_same_v<T, std::vector<float>>)
            {
                uint64_t n = value.size();
                HashBytes(&n, sizeof(n));
                if (n)
                {
                    HashBytes(value.data(), n * sizeof(float));
                }
            }
            else if constexpr (std::is_same_v<T, std::vector<AssetHandle>>)
            {
                uint64_t n = value.size();
                HashBytes(&n, sizeof(n));
                if (n)
                {
                    HashBytes(value.data(), n * sizeof(AssetHandle));
                }
            }
            else if constexpr (is_enum_vector<T>::value)
            {
                uint64_t n = value.size();
                HashBytes(&n, sizeof(n));
                for (auto& e : value)
                {
                    int i = static_cast<int>(e);
                    HashBytes(&i, sizeof(i));
                }
            }
            else if constexpr (is_std_array<T>::value)
            {
                for (auto& e : value)
                {
                    HashValue(nullptr, e);
                }
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                uint8_t b = value ? 1 : 0;
                HashBytes(&b, 1);
            }
            else
            {
                static_assert(std::is_trivially_copyable_v<T>, "unsupported scene field type");
                HashBytes(&value, sizeof(T));
            }
        }

        // ---- 型ごとの JSON 変換
        template <typename T> struct is_std_array : std::false_type
        {
        };

        template <typename U, size_t N> struct is_std_array<std::array<U, N>> : std::true_type
        {
        };

        template <typename T> struct is_enum_vector : std::false_type
        {
        };

        template <typename U> struct is_enum_vector<std::vector<U>> : std::bool_constant<std::is_enum_v<U>>
        {
        };

        static Json::Value Vec(std::initializer_list<float> values)
        {
            Json::Value list(Json::arrayValue);
            for (float v : values)
            {
                list.append(v);
            }
            return list;
        }

        template <typename T> static Json::Value ToJson(const T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                return Json::Value(value);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return Json::Value(value);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                return Json::Value(static_cast<int>(value));
            }
            else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
            {
                return Json::Value(static_cast<double>(value));
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return Json::Value(FormatAssetHandle(value));
            }
            else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            {
                return Json::Value(static_cast<Json::Int64>(value));
            }
            else if constexpr (std::is_integral_v<T>)
            {
                return Json::Value(static_cast<Json::UInt64>(value));
            }
            else if constexpr (std::is_same_v<T, XMFLOAT2>)
            {
                return Vec({value.x, value.y});
            }
            else if constexpr (std::is_same_v<T, XMFLOAT3>)
            {
                return Vec({value.x, value.y, value.z});
            }
            else if constexpr (std::is_same_v<T, XMFLOAT4>)
            {
                return Vec({value.x, value.y, value.z, value.w});
            }
            else if constexpr (std::is_same_v<T, std::vector<AssetHandle>>)
            {
                Json::Value list(Json::arrayValue);
                for (auto handle : value)
                {
                    list.append(FormatAssetHandle(handle));
                }
                return list;
            }
            else if constexpr (is_std_array<T>::value || std::is_same_v<T, std::vector<std::string>> ||
                               std::is_same_v<T, std::vector<float>> || is_enum_vector<T>::value)
            {
                Json::Value list(Json::arrayValue);
                for (const auto& item : value)
                {
                    list.append(ToJson(item));
                }
                return list;
            }
            else
            {
                static_assert(sizeof(T) == 0, "unsupported scene field type");
            }
        }

        static bool ReadFloats(const Json::Value& v, float* out, Json::ArrayIndex count)
        {
            if (!v.isArray() || v.size() != count)
            {
                return false;
            }
            for (Json::ArrayIndex i = 0; i < count; ++i)
            {
                if (!v[i].isNumeric())
                {
                    return false;
                }
            }
            for (Json::ArrayIndex i = 0; i < count; ++i)
            {
                out[i] = v[i].asFloat();
            }
            return true;
        }

        template <typename T> static void FromJson(const Json::Value& v, T& value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                if (v.isBool())
                {
                    value = v.asBool();
                }
                else if (v.isNumeric())
                {
                    value = v.asInt() != 0;
                }
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                if (v.isString())
                {
                    value = v.asString();
                }
            }
            else if constexpr (std::is_enum_v<T>)
            {
                if (v.isNumeric())
                {
                    value = static_cast<T>(v.asInt());
                }
            }
            else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
            {
                if (v.isNumeric())
                {
                    value = static_cast<T>(v.asDouble());
                }
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                if (v.isString())
                {
                    value = ParseAssetHandle(v.asString());
                }
                else if (v.isNumeric())
                {
                    value = v.asUInt64();
                }
            }
            else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            {
                if (v.isNumeric())
                {
                    value = static_cast<T>(v.asInt64());
                }
            }
            else if constexpr (std::is_integral_v<T>)
            {
                if (v.isNumeric())
                {
                    value = static_cast<T>(v.asUInt64());
                }
            }
            else if constexpr (std::is_same_v<T, XMFLOAT2>)
            {
                float f[2];
                if (ReadFloats(v, f, 2))
                {
                    value = {f[0], f[1]};
                }
            }
            else if constexpr (std::is_same_v<T, XMFLOAT3>)
            {
                float f[3];
                if (ReadFloats(v, f, 3))
                {
                    value = {f[0], f[1], f[2]};
                }
            }
            else if constexpr (std::is_same_v<T, XMFLOAT4>)
            {
                float f[4];
                if (ReadFloats(v, f, 4))
                {
                    value = {f[0], f[1], f[2], f[3]};
                }
            }
            else if constexpr (std::is_same_v<T, std::vector<AssetHandle>>)
            {
                if (!v.isArray())
                {
                    return;
                }
                value.clear();
                for (const auto& item : v)
                {
                    if (item.isString())
                    {
                        value.push_back(ParseAssetHandle(item.asString()));
                    }
                }
            }
            else if constexpr (is_std_array<T>::value)
            {
                if (!v.isArray())
                {
                    return;
                }
                for (Json::ArrayIndex i = 0; i < v.size() && i < value.size(); ++i)
                {
                    FromJson(v[i], value[i]);
                }
            }
            else if constexpr (std::is_same_v<T, std::vector<std::string>> || std::is_same_v<T, std::vector<float>> ||
                               is_enum_vector<T>::value)
            {
                if (!v.isArray())
                {
                    return;
                }
                value.clear();
                value.resize(v.size());
                for (Json::ArrayIndex i = 0; i < v.size(); ++i)
                {
                    FromJson(v[i], value[i]);
                }
            }
            else
            {
                static_assert(sizeof(T) == 0, "unsupported scene field type");
            }
        }
    };

    // ---------------------------------------------------------------- コンポーネントごとの項目
    inline void Visit(Archive& a, NameComponent& c)
    {
        a("name", c.Name);
        a("prefabAsset", c.PrefabAssetPath);
        a("prefabLocalId", c.PrefabLocalId);
        a("prefabRoot", c.IsPrefabRoot);
    }

    inline void Visit(Archive& a, TransformComponent& c)
    {
        a("position", c.Position);
        a("rotation", c.Rotation);
        a("scale", c.Scale);
        a.Entity("parent", c.Parent);
        bool affine = c.HasAffineWorld && TransformMath::Same(c.Position, c.AffinePosition) &&
                      TransformMath::Same(c.Rotation, c.AffineRotation) && TransformMath::Same(c.Scale, c.AffineScale);
        a("hasAffineWorld", affine);
        if (affine)
        {
            std::array<float, 16> matrix{};
            if (!a.Reading())
            {
                std::memcpy(matrix.data(), &c.WorldMatrix, sizeof(c.WorldMatrix));
            }
            a("affineWorld", matrix);
            if (a.Reading())
            {
                std::memcpy(&c.WorldMatrix, matrix.data(), sizeof(c.WorldMatrix));
                c.HasAffineWorld = true;
                c.AffinePosition = c.Position;
                c.AffineRotation = c.Rotation;
                c.AffineScale = c.Scale;
            }
        }
        else if (a.Reading())
        {
            c.HasAffineWorld = false;
        }
    }

    inline void Visit(Archive& a, ShaderComponent& c)
    {
        a("vs", c.VsPath);
        a("ps", c.PsPath);
    }

    inline void Visit(Archive& a, StaticModelComponent& c)
    {
        a("path", c.ModelPath);
        a("convert", c.IsConvert);
    }

    inline void Visit(Archive& a, AnimationModelComponent& c)
    {
        a("path", c.ModelPath);
        a("convert", c.IsConvert);
        a("animationPaths", c.AnimationPaths);
        a("animations", c.Animations);
        a("current", c.CurrentAnimation);
        a("next", c.NextAnimation);
        a("blendRate", c.BlendRate);
        a("speed", c.Speed);
        a("playing", c.IsPlaying);
        a.Array("layers", c.ActiveAnimationLayers, [](Archive& s, AnimationPlaybackLayer& layer) {
            s("name", layer.AnimationName);
        });
    }

    inline void Visit(Archive& a, LightComponent& c)
    {
        a("type", c.Type);
        a("renderMode", c.RenderMode);
        a("position", c.Position);
        a("color", c.Color);
        a("intensity", c.Intensity);
        a("range", c.Range);
        a("direction", c.Direction);
        a("outerAngle", c.OuterAngle);
        a("innerAngle", c.InnerAngle);
        a("volumeDensity", c.VolumeDensity);
        a("volumeShape", c.VolumeShape);
        a("areaWidth", c.AreaWidth);
        a("areaHeight", c.AreaHeight);
        a("active", c.IsActive);
        a("drawDebug", c.DrawDebug);
        a("affectsOpaque", c.AffectsOpaque);
        a("affectsForward", c.AffectsForward);
        a("affectsVolumetrics", c.AffectsVolumetrics);
        a("castShadow", c.CastShadow);
        a("allowMegaLights", c.AllowMegaLights);
        a("sourceRadius", c.SourceRadius);
        a("areaTexture", c.AreaTexturePath);
        a("iesProfile", c.IesProfilePath);
        a("iesRotation", c.IesRotationDegrees);
        a("priority", c.Priority);
    }

    inline void Visit(Archive& a, SunComponent& c)
    {
        a("target", c.Target);
        a("visualRadius", c.VisualRadius);
        a("syncDirectionalLight", c.SyncDirectionalLight);
    }

    inline void VisitBasicNoise(Archive& a, Procedural::BasicNoiseSettings& n)
    {
        a("method", n.Method);
        a("seed", n.Seed);
        a("frequency", n.Frequency);
        a("octaves", n.Octaves);
        a("persistence", n.Persistence);
        a("lacunarity", n.Lacunarity);
        a("warpStrength", n.WarpStrength);
    }

    inline void Visit(Archive& a, ProceduralTerrainComponent& c)
    {
        auto& s = c.Settings;
        a("resolution", s.Resolution);
        a("size", s.Size);
        a("height", s.Height);
        a("baseHeight", s.BaseHeight);
        a.Object("noise", [&](Archive& n) {
            VisitBasicNoise(n, s.Noise);
            n("layerCount", s.Noise.LayerCount);
            std::vector<Procedural::NoiseLayer> layers(s.Noise.Layers.begin(), s.Noise.Layers.end());
            n.Array("layers", layers, [](Archive& l, Procedural::NoiseLayer& layer) {
                VisitBasicNoise(l, layer.Noise);
                l("weight", layer.Weight);
            });
            if (n.Reading())
            {
                for (size_t i = 0; i < layers.size() && i < s.Noise.Layers.size(); ++i)
                {
                    s.Noise.Layers[i] = layers[i];
                }
            }
        });
    }

    inline void Visit(Archive& a, ProceduralMotionComponent& c)
    {
        a("target", c.Target);
        a("method", c.Method);
        a("origin", c.Origin);
        a("amplitude", c.Amplitude);
        a("speed", c.Speed);
        a("spatialFrequency", c.SpatialFrequency);
        a("seed", c.Seed);
        a("enabled", c.Enabled);
        a("previewInEditor", c.PreviewInEditor);
    }

    inline void VisitPart(Archive& a, MaterialPartParams& p)
    {
        a("metallic", p.Metallic);
        a("roughness", p.Roughness);
        a("fresnel", p.Fresnel);
        a("normalBlend", p.NormalBlend);
        a("normalBias", p.NormalBias);
        a("baseSaturation", p.BaseSaturation);
        a("baseBrightness", p.BaseBrightness);
        a("shadowThreshold", p.ShadowThreshold);
        a("shadowSoftness", p.ShadowSoftness);
        a("shadowStrength", p.ShadowStrength);
        a("midStrength", p.MidStrength);
        a("litStrength", p.LitStrength);
        a("rimStrength", p.RimStrength);
        a("rimThreshold", p.RimThreshold);
        a("rimSoftness", p.RimSoftness);
        a("rimPower", p.RimPower);
        a("rimColor", p.RimColor);
        a("rimAlbedoBlend", p.RimAlbedoBlend);
        a("rimLightBlend", p.RimLightBlend);
        a("specularStrength", p.SpecularStrength);
        a("specularThreshold", p.SpecularThreshold);
        a("kawaiiBlend", p.KawaiiBlend);
        a("skinScatterStrength", p.SkinScatterStrength);
        a("skinScatterWrap", p.SkinScatterWrap);
        a("skinBacklightStrength", p.SkinBacklightStrength);
        a("skinRimScatterStrength", p.SkinRimScatterStrength);
        a("skinOilSpecularStrength", p.SkinOilSpecularStrength);
        a("skinShadowScatter", p.SkinShadowScatter);
        a("castShadowThreshold", p.CastShadowThreshold);
        a("castShadowSoftness", p.CastShadowSoftness);
    }

    inline void Visit(Archive& a, MaterialComponent& c)
    {
        a("slots", c.MaterialSlots);
        a("sceneIdentity", c.MaterialSceneIdentity);
        a("baseColor", c.BaseColor);
        a("normalScale", c.NormalScale);
        a("aoStrength", c.AOStrength);
        a("emissiveColor", c.EmissiveColor);
        a("emissiveIntensity", c.EmissiveIntensity);
        a("assetSurface", c.AssetSurface);
        a("assetCull", c.AssetCull);
        a("assetDepthWrite", c.AssetDepthWrite);
        a("alphaCutoff", c.AlphaCutoff);
        a("texture", c.TexturePath);
        a("normalMap", c.NormalMapPath);
        a("metallic", c.Metallic);
        a("roughness", c.Roughness);
        a("fresnel", c.Fresnel);
        a("normalBlend", c.NormalBlend);
        a("normalBias", c.NormalBias);
        a("baseSaturation", c.BaseSaturation);
        a("baseBrightness", c.BaseBrightness);
        a("shadowThreshold", c.ShadowThreshold);
        a("shadowSoftness", c.ShadowSoftness);
        a("shadowStrength", c.ShadowStrength);
        a("midStrength", c.MidStrength);
        a("litStrength", c.LitStrength);
        a("rimStrength", c.RimStrength);
        a("rimThreshold", c.RimThreshold);
        a("rimSoftness", c.RimSoftness);
        a("rimPower", c.RimPower);
        a("rimColor", c.RimColor);
        a("rimAlbedoBlend", c.RimAlbedoBlend);
        a("rimLightBlend", c.RimLightBlend);
        a("specularStrength", c.SpecularStrength);
        a("specularThreshold", c.SpecularThreshold);
        a("kawaiiBlend", c.KawaiiBlend);
        a("skinScatterStrength", c.SkinScatterStrength);
        a("skinScatterWrap", c.SkinScatterWrap);
        a("skinBacklightStrength", c.SkinBacklightStrength);
        a("skinRimScatterStrength", c.SkinRimScatterStrength);
        a("skinOilSpecularStrength", c.SkinOilSpecularStrength);
        a("skinShadowScatter", c.SkinShadowScatter);
        a("castShadowThreshold", c.CastShadowThreshold);
        a("castShadowSoftness", c.CastShadowSoftness);
        a.FixedArray("parts", c.PartParams, [](Archive& s, MaterialPartParams& p) {
            VisitPart(s, p);
        });
        a("shaderClassMode", c.ShaderClassMode);
        a("shaderClass", c.ShaderClass);
        a("toonOutlineWidth", c.ToonOutlineWidth);
        a("toonOutlineScreenWidth", c.ToonOutlineScreenWidth);
        a("toonOutlineMode", c.ToonOutlineRenderMode);
        a("toonOutlineWidthMode", c.ToonOutlineWidthModeSetting);
        a("toonTeoMode", c.ToonTeoRenderMode);
        a("toonOutlineTeoWidthScale", c.ToonOutlineTeoWidthScale);
        a("meshOutlineOverrides", c.ToonMeshOutlineOverrides);
        a("meshOutlineWidthScales", c.ToonMeshOutlineWidthScales);
        a("meshShadingOverrides", c.MeshShadingOverrides);
        a("alpha", c.Alpha);
        a("shadowMode", c.ShadowMode);
        a("shadowCutoff", c.ShadowCutoff);
        a("ior", c.IOR);
        a("transmission", c.Transmission);
        a("transmissionRoughness", c.TransmissionRoughness);
        a("refractionStrength", c.RefractionStrength);
        a("thickness", c.Thickness);
        a("absorption", c.AbsorptionCoefficient);
        a("transparent", c.IsTransparent);
        a("doubleSided", c.DoubleSided);
        a("receivingPostProcess", c.ReceivingPostProcess);
        a("useTexture", c.UseTexture);
    }

    inline void Visit(Archive& a, AABBComponent& c)
    {
        a("min", c.Min);
        a("max", c.Max);
        a("center", c.Center);
        a("extents", c.Extents);
        a("drawDebug", c.DrawDebug);
    }

    inline void Visit(Archive& a, OBBComponent& c)
    {
        a("center", c.Center);
        a("extents", c.Extents);
        a("axisX", c.Axes[0]);
        a("axisY", c.Axes[1]);
        a("axisZ", c.Axes[2]);
        a("drawDebug", c.DrawDebug);
    }

    // メッシュの形状情報（頂点そのものは呼び出し側で追加する）
    inline void Visit(Archive& a, MeshComponent& c)
    {
        a("environment", c.IsEnvironment);
        a("primitive", c.PrimitiveType);
        if (c.PrimitiveType != 0 || a.Reading())
        {
            a("primitiveColor", c.PrimitiveColor);
            a("primitiveRadius", c.PrimitiveRadius);
            a("primitiveHeight", c.PrimitiveHeight);
            a("primitiveWidth", c.PrimitiveWidth);
            a("primitiveDepth", c.PrimitiveDepth);
            a("primitiveSegments", c.PrimitiveSegments);
            a("primitiveRings", c.PrimitiveRings);
            a("primitiveFaceDivisions", c.PrimitiveFaceDivisions);
        }
    }

    inline void Visit(Archive& a, SpriteComponent& c)
    {
        a("is3D", c.Is3D);
        a("useUvTransform", c.UseUvTransform);
        a("uvOffset", c.UvOffset);
        a("uvScale", c.UvScale);
        a("usePostProcess", c.UsePostProcess);
        a("shape", c.SourceShape);
        a("radius", c.SourceRadius);
        a("color", c.SourceColor);
    }

    inline void Visit(Archive& a, CameraComponent& c)
    {
        a("position", c.Position);
        a("target", c.Target);
        a("fov", c.Fov);
        a("near", c.NearClip);
        a("far", c.FarClip);
        a("postProcess", c.EnablePostProcess);
        a("gameCamera", c.IsGameCamera);
        a("mainGameCamera", c.IsMainGameCamera);
        a("userControl", c.AllowUserControl);
        a("priority", c.Priority);
        a.Entity("lockOnTarget", c.LockOnTarget);
        a("lockOnOffset", c.LockOnOffset);
    }

    inline void Visit(Archive& a, PostProcessComponent& c)
    {
        a("type", c.Type);
        a("intensity", c.Intensity);
        a("bloomThreshold", c.BloomThreshold);
        a("bloomSoftKnee", c.BloomSoftKnee);
        a("bloomRadius", c.BloomRadius);
    }

    inline void Visit(Archive& a, InputComponent& c)
    {
        a("active", c.IsActive);
    }

    inline void Visit(Archive& a, MoveComponent& c)
    {
        a("direction", c.Direction);
        a("speed", c.Speed);
        a("cameraRelative", c.UseCameraRelativeMovement);
        a("canMove", c.CanMove);
        a("rotationSpeed", c.RotationSpeed);
    }

    inline void Visit(Archive& a, PhysicsComponent& c)
    {
        a("usePhysics", c.UsePhysics);
        a("usePhysicsBone", c.UsePhysicsBone);
        a("engine", c.UsePhysicsEngine);
        a("bodyType", c.BodyType);
        a("shape", c.Shape);
        a("colliderRole", c.ColliderRole);
        a("colliderCenter", c.ColliderCenter);
        a("colliderSize", c.ColliderSize);
        a("sphereRadius", c.SphereRadius);
        a("capsuleRadius", c.CapsuleRadius);
        a("capsuleHeight", c.CapsuleHeight);
        a("velocity", c.Velocity);
        a("angularVelocity", c.AngularVelocity);
        a("mass", c.Mass);
        a("friction", c.Friction);
        a("restitution", c.Restitution);
        a("linearDamping", c.LinearDamping);
        a("angularDamping", c.AngularDamping);
        a("gravityFactor", c.GravityFactor);
        a("useGravity", c.UseGravity);
        a("continuousCollision", c.EnableContinuousCollision);
        a("allowSleeping", c.AllowSleeping);
        a("pmxMassScale", c.PmxMassScale);
        a("pmxDampingScale", c.PmxDampingScale);
        a("pmxStiffnessScale", c.PmxStiffnessScale);
        a("pmxColliderScale", c.PmxColliderScale);
        a("collisionLayer", c.CollisionLayer);
        a("collisionMask", c.CollisionMask);
    }

    inline void Visit(Archive& a, TimelineComponent& c)
    {
        a("duration", c.Duration);
        a("speed", c.Speed);
        a("playOnAwake", c.PlayOnAwake);
        a("loop", c.Loop);
        a.Array("tracks", c.Tracks, [](Archive& t, TimelineTrackData& track) {
            t("name", track.Name);
            t.Entity("target", track.Target);
            t("property", track.Property);
            t("enabled", track.Enabled);
            t("hasDefault", track.HasDefaultValue);
            t("default", track.DefaultValue);
            t.Array("clips", track.Clips, [](Archive& k, TimelineClipData& clip) {
                k("name", clip.Name);
                k("start", clip.StartTime);
                k("duration", clip.Duration);
                k("blendIn", clip.BlendIn);
                k("blendOut", clip.BlendOut);
                k("enabled", clip.Enabled);
                k.Array("keys", clip.Keys, [](Archive& f, TimelineKeyframe& key) {
                    f("time", key.Time);
                    f("value", key.Value);
                    f("interpolation", key.Interpolation);
                });
            });
        });
    }

    inline void Visit(Archive& a, LODComponent& c)
    {
        a("use", c.UseLOD);
        a("lod1", c.Lod1Distance);
        a("lod2", c.Lod2Distance);
        a("useAssetSettings", c.UseAssetSettings);
        a("count", c.ScreenLOD.Count);
        a("thresholds", c.ScreenLOD.Thresholds);
        a("hysteresis", c.ScreenLOD.Hysteresis);
        a("meshPaths", c.ScreenLOD.MeshPaths);
        a("impostor", c.UseImpostor);
        a("impostorDistance", c.ImpostorDistance);
        a("impostorFrames", c.ImpostorFrames);
        a("impostorResolution", c.ImpostorResolution);
        a("impostorHemisphere", c.ImpostorHemisphere);
    }

    inline void Visit(Archive& a, InstancingComponent& c)
    {
        a("use", c.UseInstancing);
        a("frustumCulling", c.EnableFrustumCulling);
        a("group", c.GroupId);
    }

    // コンポーネント1つを「キー名の子オブジェクト」として読み書きする。
    // present: 書き込み/ハッシュ時はコンポーネントの有無、読み込み時は「ファイルにあったか」を返す。
    template <typename T> inline void Component(Archive& a, const char* key, bool& present, T& component)
    {
        if (a.Hashing())
        {
            uint8_t flag = present ? 1 : 0;
            a.HashBytes(&flag, 1);
            if (present)
            {
                a.Object(key, [&](Archive& s) {
                    Visit(s, component);
                });
            }
            return;
        }
        if (a.Writing())
        {
            if (present)
            {
                a.Object(key, [&](Archive& s) {
                    Visit(s, component);
                });
            }
            return;
        }
        present = false;
        a.Object(key, [&](Archive& s) {
            present = true;
            Visit(s, component);
        });
    }
} // 名前空間 SceneFile の終わり。
