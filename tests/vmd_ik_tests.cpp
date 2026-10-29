#include "models/humanoidik.h"
#include "models/vmdikbinding.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace HumanoidIk;
static bool Near(const aiVector3D& a, const aiVector3D& b, float epsilon=0.0002f) { return (a-b).Length()<epsilon; }
static Pose Rig(float scale=1)
{
    Pose pose;
    pose.Nodes.resize(5);
    pose.Nodes[0].Parent=-1;
    pose.Nodes[0].Local=aiMatrix4x4(aiVector3D(scale,scale,scale),aiQuaternion(),aiVector3D());
    pose.Nodes[1].Parent=0;
    pose.Nodes[1].Local=aiMatrix4x4(aiVector3D(1,1,1),aiQuaternion(),aiVector3D(0,2,0));
    pose.Nodes[2].Parent=1;
    pose.Nodes[2].Local=aiMatrix4x4(aiVector3D(1,1,1),aiQuaternion(),aiVector3D(0,-1,0));
    pose.Nodes[3].Parent=2;
    pose.Nodes[3].Local=aiMatrix4x4(aiVector3D(1,1,1),aiQuaternion(),aiVector3D(0,-1,0));
    pose.Nodes[4].Parent=3;
    pose.Nodes[4].Local=aiMatrix4x4(aiVector3D(1,1,1),aiQuaternion(),aiVector3D(0,0,0.3f));
    assert(pose.CaptureBindPose());
    return pose;
}
int main()
{
    Leg leg{1,2,3,4,aiVector3D(0,0,1)};
    auto pose=Rig();
    Target goal;
    goal.FootEnabled=true;
    goal.FootPosition={0,0.6f,0.3f};
    goal.FootRotation={};
    assert(Apply(pose,leg,goal));
    assert(Near(pose.Position(3),goal.FootPosition));
    assert(pose.Position(2).z>0);
    assert(std::abs((pose.Position(2)-pose.Position(1)).Length()-1)<0.0001f);
    assert(std::abs((pose.Position(3)-pose.Position(2)).Length()-1)<0.0001f);
    assert(Near(Translation(pose.Nodes[2].Local),{0,-1,0}));
    assert(Near(Translation(pose.Nodes[3].Local),{0,-1,0}));
    // Center/pelvis move while the source foot IK target remains planted.
    pose=Rig();
    pose.Nodes[1].Local.a4=0.5f;
    pose.Nodes[1].Local.b4=1.8f;
    pose.UpdateGlobals();
    goal.FootPosition={0,0,0};
    assert(Apply(pose,leg,goal));
    assert(Near(pose.Position(3),{0,0,0}));
    // Reach limits and coincident hip/goal never stretch or produce NaNs.
    pose=Rig(); goal.FootPosition={0,-100,0};
    assert(Apply(pose,leg,goal));
    assert(std::abs((pose.Position(3)-pose.Position(1)).Length()-2)<0.0002f);
    goal.FootPosition=pose.Position(1);
    assert(Apply(pose,leg,goal));
    assert(Finite(pose.Position(2)) && Finite(pose.Position(3)));
    pose=Rig(100); goal.FootPosition={0,60,30};
    assert(Apply(pose,leg,goal));
    assert(Near(pose.Position(3),goal.FootPosition,0.02f));
    pose=Rig(); goal.FootEnabled=false;
    const auto before=pose.Position(3);
    assert(!Apply(pose,leg,goal));
    assert(Near(pose.Position(3),before));
    // Toe-only IK rotates the foot, leaving ankle position and leg FK unchanged.
    goal.ToeEnabled=true; goal.ToePosition={0,0.2f,0.2f};
    assert(Apply(pose,leg,goal));
    assert(Near(pose.Position(3),before));
    const auto toeDirection=(pose.Position(4)-pose.Position(3)).Normalize();
    assert(Near(toeDirection,aiVector3D(0,1,1).Normalize()));
    pose=Rig(); pose.Nodes[2].Local.b4=0; pose.UpdateGlobals();
    goal.FootEnabled=true; goal.ToeEnabled=false;
    assert(!Apply(pose,leg,goal));
    Leg invalid=leg; invalid.Knee=4;
    assert(!Apply(pose,invalid,goal));
    // Globally rigid joints can hide a compensating nonuniform/sheared parent.
    pose=Rig();
    pose.Nodes[0].Local=aiMatrix4x4(aiVector3D(2,1,1),aiQuaternion(aiVector3D(0,0,1),0.5f),aiVector3D());
    auto inverseParent=pose.Nodes[0].Local; inverseParent.Inverse();
    pose.Nodes[1].Local=inverseParent*aiMatrix4x4(aiVector3D(1,1,1),aiQuaternion(),aiVector3D(0,2,0));
    assert(pose.UpdateGlobals());
    assert(RigidScale(pose.Nodes[1].Global));
    goal.FootPosition={0.5f,0.5f,0.3f};
    const auto saved=pose.Nodes[1].Local;
    assert(!Apply(pose,leg,goal));
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) assert(saved[r][c]==pose.Nodes[1].Local[r][c]);
    Target fk; fk.FootPosition={0,0,0}; fk.FootRotation={};
    Target active=fk; active.FootEnabled=true; active.FootPosition={1,0,0};
    const auto half=Blend(active,Target{},0.5f,fk);
    assert(half.FootEnabled && Near(half.FootPosition,{0.5f,0,0}));
    assert(!Blend(Target{},Target{},0.3f,fk).FootEnabled);
    Offsets offset; offset.FootEnabled=true; offset.FootPosition={0,10,0}; offset.RootPosition={10,0,0};
    const auto converted=FromOffsets({0,0,0},aiQuaternion(),{0,0,0.3f},offset,0.1f);
    assert(Near(converted.FootPosition,{1,1,0}));
    std::unordered_map<std::string,int> tracks{{"左足ＩＫ",1},{"left",0}};
    assert(*VmdIkBinding::Find(tracks,VmdIkBinding::FootNames(0))==1);
    tracks.erase("左足ＩＫ");tracks["左足IK"]=2;
    assert(*VmdIkBinding::Find(tracks,VmdIkBinding::FootNames(0))==2);
    assert(!VmdIkBinding::Find(tracks,VmdIkBinding::FootNames(1)));
    struct MotionRef { const std::vector<int>* Track=nullptr; size_t LayerIndex=0; int Cursor=123; };
    struct SwitchRef { const std::vector<bool>* Track=nullptr; size_t LayerIndex=0; int Cursor=123; };
    struct Channel { MotionRef Foot,Toe,Root,Parent; SwitchRef FootSwitch,ToeSwitch; } channel;
    std::unordered_map<std::string,std::vector<int>> baseTracks{{"左足ＩＫ",{1}},{"全ての親",{2}}};
    std::unordered_map<std::string,std::vector<bool>> baseSwitches{{"左足ＩＫ",{true}}};
    VmdIkBinding::UpdateChannel(baseTracks,baseSwitches,0,0,channel);
    assert(channel.Foot.Track==&baseTracks.at("左足ＩＫ") && channel.Foot.Cursor==0);
    std::unordered_map<std::string,std::vector<int>> faceTracks{{"face",{9}}};
    std::unordered_map<std::string,std::vector<bool>> offSwitch{{"左足IK",{false}}};
    VmdIkBinding::UpdateChannel(faceTracks,offSwitch,1,0,channel);
    assert(channel.Foot.Track==&baseTracks.at("左足ＩＫ") && channel.Foot.LayerIndex==0);
    assert(channel.FootSwitch.Track==&offSwitch.at("左足IK") && channel.FootSwitch.LayerIndex==1);
    std::unordered_map<std::string,std::vector<int>> newTrack{{"左足IK",{3}}};
    VmdIkBinding::UpdateChannel(newTrack,baseSwitches,2,0,channel);
    assert(channel.Foot.Track==&newTrack.at("左足IK") && channel.Foot.LayerIndex==2);
    assert(channel.Root.Track==&baseTracks.at("全ての親") && channel.Root.LayerIndex==0);
    std::cout<<"VMD humanoid IK: PASS\n";
}
