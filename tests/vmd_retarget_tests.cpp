#include "models/vmdretarget.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <chrono>

static bool Near(float a, float b) { return std::abs(a-b) < 0.0001f; }
int main()
{
    ModelResourceMap map;
    map.Add(ModelResourceMap::Bones, "mixamorig:Hips", 0);
    map.Add(ModelResourceMap::Bones, "mixamorig:LeftArm", 1);
    map.Add(ModelResourceMap::Bones, "mixamorig:RightLeg", 2);
    map.Add(ModelResourceMap::Bones, "mixamorig:LeftHandIndex1", 3);
    VmdRetarget::AddCommonHumanoidBones(map);
    VmdRetarget::AddVmdAliases(map);
    assert(map.Find(ModelResourceMap::Vmd, "下半身")->front() == 0);
    assert(map.Find(ModelResourceMap::Vmd, "左腕")->front() == 1);
    assert(map.Find(ModelResourceMap::Vmd, "右ひざ")->front() == 2);
    assert(!map.Find(ModelResourceMap::Vmd, "左足ＩＫ"));
    assert(map.Find(ModelResourceMap::Vmd, "左人指１")->front() == 3);
    ModelResourceMap custom;
    custom.Set(ModelResourceMap::Vmd, "下半身", 99);
    assert(VmdRetarget::RootMotionTarget(custom)->front() == 99);
    custom.Set(ModelResourceMap::Humanoid, "hips", 1);
    assert(VmdRetarget::RootMotionTarget(custom)->front() == 99);
    std::unordered_map<std::string, int> tracks{{"左腕", 42}, {"mixamorig:LeftArm", 9}};
    assert(*VmdRetarget::FindTrack(tracks, "mixamorig:LeftArm", 1, map) == 9);
    tracks.erase("mixamorig:LeftArm");
    assert(*VmdRetarget::FindTrack(tracks, "mixamorig:LeftArm", 1, map) == 42);
    assert(!VmdRetarget::FindTrack(tracks, "unknown", 99, map));
    ModelResourceMap ambiguous;
    ambiguous.Add(ModelResourceMap::Bones, "rigA:LeftArm", 1);
    ambiguous.Add(ModelResourceMap::Bones, "rigB:LeftArm", 2);
    VmdRetarget::AddCommonHumanoidBones(ambiguous);
    assert(!ambiguous.Find(ModelResourceMap::Humanoid, "leftUpperArm"));

    VmdRetarget::Transform transform;
    transform.Enabled = true;
    transform.GlobalBindRotation = aiQuaternion(aiVector3D(0,0,1), float(3.141592653589793/2));
    transform.ParentInverse = aiMatrix3x3();
    transform.TranslationScale = 0.1f;
    aiQuaternion rotation(aiVector3D(1,0,0), 0.5f);
    aiVector3D position(0,20,0);
    VmdRetarget::Apply(transform, rotation, position);
    assert(Near(rotation.x, 0) && Near(std::abs(rotation.y), std::sin(0.25f)));
    assert(Near(position.y,2));
    aiQuaternion identity;
    aiVector3D zero;
    VmdRetarget::Apply(transform, identity, zero);
    assert(Near(identity.w,1) && Near(identity.x,0) && Near(identity.y,0) && Near(identity.z,0));
    // MMD torso and lower body are siblings; humanoid spine is a child of hips.
    VmdRetarget::Transform torso;
    torso.Enabled = true;
    aiQuaternion lower(aiVector3D(0,0,1), float(3.141592653589793/2));
    aiQuaternion upper;
    aiVector3D offset;
    const aiVector3D bindOffset(0,1,0);
    VmdRetarget::CompensateTorso(torso, bindOffset, lower, aiVector3D(), upper, offset);
    VmdRetarget::Apply(torso, upper, offset);
    const auto globalUpper = lower * upper;
    const auto globalOffset = lower.Rotate(bindOffset + offset);
    assert(Near(globalUpper.w,1));
    assert(Near(globalOffset.x,0) && Near(globalOffset.y,1));
    auto unchanged = rotation;
    VmdRetarget::Transform pmx;
    VmdRetarget::Apply(pmx, rotation, position);
    assert(Near(rotation.y, unchanged.y) && Near(position.y, 2));

    ModelResourceMap vrm;
    vrm.Add(ModelResourceMap::Bones, "head", 11);
    vrm.Add(ModelResourceMap::Bones, "hips", 8);
    assert(VmdRetarget::AddVrmHumanoidBones("asset/model/moca.vrm", vrm));
    assert(vrm.Find(ModelResourceMap::Humanoid, "head")->front() == 11);
    assert(vrm.Find(ModelResourceMap::Humanoid, "hips")->front() == 8);
    // VRM 0 uses a different thumb-segment naming convention.
    const auto temp = std::filesystem::temp_directory_path() /
        ("vrm0-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".vrm");
    std::string json = R"({"nodes":[{"name":"thumb0"},{"name":"hips0"}],"extensions":{"VRM":{"humanoid":{"humanBones":[{"bone":"leftThumbProximal","node":0},{"bone":"hips","node":1}]}}}})";
    while (json.size() % 4) json += ' ';
    {
        std::ofstream output(temp, std::ios::binary);
        auto u32 = [&](uint32_t value) {
            for (unsigned byte=0; byte<4; ++byte) output.put(char((value >> (byte*8)) & 255));
        };
        u32(0x46546c67); u32(2); u32(static_cast<uint32_t>(20 + json.size()));
        u32(static_cast<uint32_t>(json.size())); u32(0x4e4f534a); output << json;
    }
    ModelResourceMap vrm0;
    vrm0.Add(ModelResourceMap::Bones, "thumb0", 3);
    vrm0.Add(ModelResourceMap::Bones, "hips0", 4);
    assert(VmdRetarget::AddVrmHumanoidBones(temp, vrm0));
    assert(vrm0.Find(ModelResourceMap::Humanoid, "leftThumbMetacarpal")->front() == 3);
    assert(vrm0.Find(ModelResourceMap::Humanoid, "hips")->front() == 4);
    std::filesystem::remove(temp);
    std::cout << "VMD retarget: PASS (aliases, ambiguity, exact precedence, bind axes, scale, VRM1)\n";
}
