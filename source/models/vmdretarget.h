#pragma once
#include "modelresourcemap.h"
#include "types.h"
#include <json/json.h>
#include <cmath>
#include <cctype>
#include <array>
#include <memory>

namespace VmdRetarget
{
inline const ModelResourceMap::Indices* RootMotionTarget(const ModelResourceMap& map)
{
    const auto* hips = map.Find(ModelResourceMap::Vmd, "下半身");
    return hips ? hips : map.Find(ModelResourceMap::Humanoid, "hips");
}
struct Transform
{
    bool Enabled = false;
    aiQuaternion GlobalBindRotation{};
    aiMatrix3x3 ParentInverse{};
    aiMatrix3x3 ParentBindLinear{};
    float TranslationScale = 1.0f;
};
inline void Apply(const Transform& transform, aiQuaternion& rotation, aiVector3D& position)
{
    if (!transform.Enabled) return;
    auto inverse = transform.GlobalBindRotation;
    inverse.Conjugate();
    rotation = inverse * rotation * transform.GlobalBindRotation;
    rotation.Normalize();
    position = transform.ParentInverse * (position * transform.TranslationScale);
}
// MMD 上半身 and 下半身 branch from the same parent. In a humanoid rig,
// spine is below hips, so cancel the lower-body delta at the torso boundary.
inline void CompensateTorso(const Transform& transform, const aiVector3D& bindPosition,
                           aiQuaternion lowerRotation, const aiVector3D& lowerPosition,
                           aiQuaternion& rotation, aiVector3D& position)
{
    if (!transform.Enabled) return;
    lowerRotation.Conjugate();
    rotation = lowerRotation * rotation;
    const aiVector3D worldOffset = transform.ParentBindLinear * bindPosition;
    const float scale = transform.TranslationScale;
    position = (lowerRotation.Rotate(worldOffset + (position - lowerPosition) * scale) - worldOffset) / scale;
}
inline std::string Normalize(std::string name)
{
    const auto prefix = name.find_last_of(":|");
    if (prefix != std::string::npos) name.erase(0, prefix + 1);
    std::string result;
    for (unsigned char c : name)
        if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
    return result;
}
struct HumanBone
{
    std::string Human;
    std::string Vmd;
    std::vector<std::string> Names;
};
inline const std::vector<HumanBone>& HumanBones()
{
    static const auto bones = [] {
        std::vector<HumanBone> result = {
            {"hips", "下半身", {"Hips", "Pelvis", "J_Bip_C_Hips"}},
            {"spine", "上半身", {"Spine", "J_Bip_C_Spine"}},
            {"chest", "上半身2", {"Chest", "Spine1", "J_Bip_C_Chest"}},
            {"upperChest", "上半身3", {"UpperChest", "Spine2", "J_Bip_C_UpperChest"}},
            {"neck", "首", {"Neck", "J_Bip_C_Neck"}},
            {"head", "頭", {"Head", "J_Bip_C_Head"}},
            {"leftEye", "左目", {"LeftEye", "eye.L", "J_Adj_L_FaceEye"}},
            {"rightEye", "右目", {"RightEye", "eye.R", "J_Adj_R_FaceEye"}},
            {"leftShoulder", "左肩", {"LeftShoulder", "J_Bip_L_Shoulder"}},
            {"rightShoulder", "右肩", {"RightShoulder", "J_Bip_R_Shoulder"}},
            {"leftUpperArm", "左腕", {"LeftUpperArm", "LeftArm", "J_Bip_L_UpperArm"}},
            {"rightUpperArm", "右腕", {"RightUpperArm", "RightArm", "J_Bip_R_UpperArm"}},
            {"leftLowerArm", "左ひじ", {"LeftLowerArm", "LeftForeArm", "J_Bip_L_LowerArm"}},
            {"rightLowerArm", "右ひじ", {"RightLowerArm", "RightForeArm", "J_Bip_R_LowerArm"}},
            {"leftHand", "左手首", {"LeftHand", "J_Bip_L_Hand"}},
            {"rightHand", "右手首", {"RightHand", "J_Bip_R_Hand"}},
            {"leftUpperLeg", "左足", {"LeftUpperLeg", "LeftUpLeg", "J_Bip_L_UpperLeg"}},
            {"rightUpperLeg", "右足", {"RightUpperLeg", "RightUpLeg", "J_Bip_R_UpperLeg"}},
            {"leftLowerLeg", "左ひざ", {"LeftLowerLeg", "LeftLeg", "J_Bip_L_LowerLeg"}},
            {"rightLowerLeg", "右ひざ", {"RightLowerLeg", "RightLeg", "J_Bip_R_LowerLeg"}},
            {"leftFoot", "左足首", {"LeftFoot", "J_Bip_L_Foot"}},
            {"rightFoot", "右足首", {"RightFoot", "J_Bip_R_Foot"}},
            {"leftToes", "左つま先", {"LeftToes", "LeftToeBase", "J_Bip_L_ToeBase"}},
            {"rightToes", "右つま先", {"RightToes", "RightToeBase", "J_Bip_R_ToeBase"}}
        };
        const char* english[] = {"Thumb", "Index", "Middle", "Ring", "Little"};
        const char* mixamo[] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
        const char* japanese[] = {"親指", "人指", "中指", "薬指", "小指"};
        const char* segments[] = {"Proximal", "Intermediate", "Distal"};
        const char* digits[] = {"１", "２", "３"};
        const char* thumbDigits[] = {"０", "１", "２"};
        for (int side = 0; side < 2; ++side)
        {
            const std::string humanSide = side ? "right" : "left";
            const std::string mixamoSide = side ? "Right" : "Left";
            const std::string vmdSide = side ? "右" : "左";
            for (int finger = 0; finger < 5; ++finger)
            {
                for (int segment = 0; segment < 3; ++segment)
                {
                    const char* humanSegment = finger == 0 ?
                        (segment == 0 ? "Metacarpal" : segment == 1 ? "Proximal" : "Distal") : segments[segment];
                    const auto human = humanSide + english[finger] + humanSegment;
                    const auto vmd = vmdSide + japanese[finger] + (finger == 0 ? thumbDigits[segment] : digits[segment]);
                    const auto mix = mixamoSide + "Hand" + mixamo[finger] + std::to_string(segment + 1);
                    result.push_back({human, vmd, {human, mix}});
                }
            }
        }
        return result;
    }();
    return bones;
}
inline void AddCommonHumanoidBones(ModelResourceMap& map)
{
    const auto category = map.Categories().find(ModelResourceMap::Bones);
    if (category == map.Categories().end()) return;
    std::unordered_map<std::string, std::vector<uint32_t>> normalized;
    for (const auto& bone : category->second)
        for (auto index : bone.second) normalized[Normalize(bone.first)].push_back(index);
    for (const auto& human : HumanBones())
    {
        if (map.Find(ModelResourceMap::Humanoid, human.Human)) continue; // VRM metadata wins.
        std::vector<uint32_t> candidates;
        auto names = human.Names;
        names.emplace_back(human.Human);
        for (const auto& name : names)
        {
            const auto it = normalized.find(Normalize(name));
            if (it != normalized.end()) candidates.insert(candidates.end(), it->second.begin(), it->second.end());
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        if (candidates.size() == 1) map.Set(ModelResourceMap::Humanoid, human.Human, candidates.front());
    }
}
inline void AddVmdAliases(ModelResourceMap& map)
{
    for (const auto& human : HumanBones())
    {
        const auto* indices = map.Find(ModelResourceMap::Humanoid, human.Human);
        if (indices && indices->size() == 1) map.Set(ModelResourceMap::Vmd, human.Vmd, indices->front());
    }
    // Alternative spellings used by MMD models are resolved by FindTrack, never inserted twice.
}
template<class TrackMap> inline const typename TrackMap::mapped_type* FindTrack(
    const TrackMap& tracks, const std::string& targetName, uint32_t targetIndex, const ModelResourceMap& map)
{
    const auto exact = tracks.find(targetName);
    if (exact != tracks.end()) return &exact->second;
    const auto* aliases = map.NamesForIndex(ModelResourceMap::Vmd, targetIndex);
    if (!aliases) return nullptr;
    const typename TrackMap::mapped_type* found = nullptr;
    for (const auto* aliasName : *aliases)
    {
        const auto& alias = *aliasName;
        const auto* targets = map.Find(ModelResourceMap::Vmd, alias);
        if (!targets || targets->size() != 1) continue;
        auto it = tracks.find(alias);
        if (it == tracks.end() && alias == "上半身2") it = tracks.find("上半身２");
        if (it == tracks.end() && alias == "上半身3") it = tracks.find("上半身３");
        if (it == tracks.end())
        {
            std::string alternative = alias;
            const char* wide[] = {"０", "１", "２", "３"};
            for (int digit = 0; digit < 4; ++digit)
            {
                const auto offset = alternative.find(wide[digit]);
                if (offset != std::string::npos) alternative.replace(offset, std::string(wide[digit]).size(), 1, char('0' + digit));
            }
            it = tracks.find(alternative);
        }
        if (it != tracks.end())
        {
            if (found) return nullptr; // Ambiguous custom aliases must be corrected by the caller.
            found = &it->second;
        }
    }
    return found;
}

// Read the standardized humanoid extension before using bone-name heuristics.
// VRM 0.x and 1.0 both reference glTF node indices, not skin palette indices.
inline bool AddVrmHumanoidBones(const std::filesystem::path& file, ModelResourceMap& map)
{
    try
    {
        std::ifstream input(file, std::ios::binary);
        auto readU32 = [&]() -> uint32_t {
            std::array<unsigned char,4> bytes{};
            input.read(reinterpret_cast<char*>(bytes.data()), 4);
            return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
        };
        const auto magic = readU32();
        const auto version = readU32();
        const auto total = readU32();
        const auto size = readU32();
        const auto type = readU32();
        if (!input || magic != 0x46546c67 || version != 2 || type != 0x4e4f534a ||
            size > 32 * 1024 * 1024 || total < 20 || size > total - 20) return false;
        std::string bytes(size, '\0');
        if (!input.read(bytes.data(), size)) return false;
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errors;
        auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
        if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &root, &errors)) return false;
        auto add = [&](const std::string& human, const Json::Value& node) {
            if (!node.isUInt() || node.asUInt() >= root["nodes"].size()) return;
            const auto name = root["nodes"][node.asUInt()]["name"].asString();
            const auto* indices = map.Find(ModelResourceMap::Bones, name);
            if (indices && indices->size() == 1) map.Set(ModelResourceMap::Humanoid, human, indices->front());
        };
        const auto& extensions = root["extensions"];
        const auto& v1 = extensions["VRMC_vrm"]["humanoid"]["humanBones"];
        if (v1.isObject())
        {
            for (const auto& human : v1.getMemberNames()) add(human, v1[human]["node"]);
            return true;
        }
        const auto& v0 = extensions["VRM"]["humanoid"]["humanBones"];
        if (!v0.isArray()) return false;
        for (const auto& human : v0)
        {
            auto bone = human["bone"].asString();
            const auto thumb = bone.find("Thumb");
            if (thumb != std::string::npos)
            {
                if (bone.ends_with("Proximal")) bone = bone.substr(0, thumb + 5) + "Metacarpal";
                else if (bone.ends_with("Intermediate")) bone = bone.substr(0, thumb + 5) + "Proximal";
            }
            add(bone, human["node"]);
        }
        return true;
    }
    catch (const std::exception&) { return false; }
}
}
