#include "models/lookatsolver.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace Gaze;
static Pose MakePose()
{
    Pose p; p.Nodes.resize(5);
    p.Nodes[0].Parent=-1;
    for (int i=1;i<5;++i) p.Nodes[i].Parent=(i==1 ? 0 : i==2 ? 1 : 2);
    p.Nodes[1].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(),aiVector3D(0,1,0));
    p.Nodes[2].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(),aiVector3D(0,0.2f,0));
    p.Nodes[3].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(),aiVector3D(-0.04f,0.06f,0.08f));
    p.Nodes[4].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(),aiVector3D(0.04f,0.06f,0.08f));
    assert(p.CaptureBindPose()); return p;
}
static Rig MakeRig() { Rig r; r.Bones={1,2,3,4}; return r; }
static aiVector3D Forward(const Pose& p,int bone)
{ return HumanoidIk::Rotation(p.Nodes[bone].Global).Rotate(aiVector3D(0,0,1)); }
static float Angle(aiVector3D a,aiVector3D b)
{ return std::acos(std::clamp(HumanoidIk::Unit(a)*HumanoidIk::Unit(b),-1.0f,1.0f)); }
static void Advance(Pose& result, State& state, Rig rig, const aiVector3D& target, Settings settings, int frames=240,float dt=1.0f/60)
{
    auto base=MakePose();
    for(int i=0;i<frames;++i) { result=base; Solve(result,rig,state,target,settings,true,dt); }
}
int main()
{
    const auto rig=MakeRig(); Settings settings; State state; Pose pose;
    const aiVector3D front(0,1.26f,5);
    Advance(pose,state,rig,front,settings);
    for (int eye : {3,4}) assert(Angle(Forward(pose,eye),front-pose.Position(eye))<0.002f);
    // Small target offsets are handled by the eyes, without head/neck motion.
    state={}; Advance(pose,state,rig,{0.3f,1.26f,5},settings);
    assert(std::abs(state.Angles[0].Yaw)<0.0001f && std::abs(state.Angles[1].Yaw)<0.0001f);
    // Side target: neck and head share rotation; eyes solve only residual.
    const aiVector3D side(3,1.26f,3);
    state={}; Advance(pose,state,rig,side,settings);
    assert(state.Angles[0].Yaw>0.01f && state.Angles[1].Yaw>0.01f);
    for(int eye : {3,4}) assert(Angle(Forward(pose,eye),side-pose.Position(eye))<0.015f);
    for(int i=0;i<4;++i) assert(std::abs(state.Angles[i].Yaw)<=Radians(Joint(settings,i).MaxYawDegrees)+1e-5f);
    // Clearing eases corrections to zero while preserving the fresh animation pose.
    for(int i=0;i<240;++i) { pose=MakePose(); Solve(pose,rig,state,side,settings,false,1.0f/60); }
    assert(AtRest(state));
    for(int i : {1,2,3,4}) assert(Angle(Forward(pose,i),{0,0,1})<0.001f);
    // Behind targets should ease out instead of forcing a 180-degree eye rotation.
    state={}; Advance(pose,state,rig,{0,1,-5},settings);
    assert(AtRest(state));
    // Zero distance, nonfinite target, missing bones and degenerate world scales are safe.
    state={}; Advance(pose,state,rig,{0,1.2f,0},settings);
    for(auto a:state.Angles) assert(std::isfinite(a.Yaw) && std::isfinite(a.Pitch));
    Rig missing; state={}; pose=MakePose(); assert(!Solve(pose,missing,state,front,settings,true,1.0f/60));
    pose=MakePose(); auto before=pose.Nodes[2].Local;
    pose.Nodes[1].Local.a1=0; pose.UpdateGlobals();
    assert(!Solve(pose,rig,state,front,settings,true,1.0f/60));
    (void)before;
    // Response and rate limits are stable at different frame rates.
    State slow,fast; Pose a,b;
    Advance(a,slow,rig,side,settings,30,1.0f/30);
    Advance(b,fast,rig,side,settings,120,1.0f/120);
    assert(std::abs(slow.Angles[0].Yaw-fast.Angles[0].Yaw)<0.025f);
    state={}; pose=MakePose(); Solve(pose,rig,state,side,settings,true,1.0f/60);
    assert(std::abs(state.Angles[0].Yaw)<=Radians(settings.Neck.MaxSpeedDegrees)/60+1e-6f);
    // Vertical tracking, with separate pitch clamps on neck, head and eyes.
    state={}; const aiVector3D elevated(1.5f,2.7f,4);
    Advance(pose,state,rig,elevated,settings);
    for(int eye : {3,4}) assert(Angle(Forward(pose,eye),elevated-pose.Position(eye))<0.02f);
    for(int i=0;i<4;++i) assert(std::abs(state.Angles[i].Pitch)<=Radians(Joint(settings,i).MaxPitchDegrees)+1e-5f);
    // Calibration must survive rotated eye bind axes AND an animated body turn.
    auto rotated=MakePose();
    rotated.Nodes[3].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(aiVector3D(0,1,0),Radians(90)),aiVector3D(-.04f,.06f,.08f));
    rotated.Nodes[4].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(aiVector3D(0,1,0),Radians(-90)),aiVector3D(.04f,.06f,.08f));
    assert(rotated.CaptureBindPose());
    rotated.Nodes[0].Local=aiMatrix4x4(aiVector3D(1),aiQuaternion(aiVector3D(0,1,0),Radians(90)),aiVector3D());
    assert(rotated.UpdateGlobals());
    state={}; const aiVector3D turnedTarget(5,1.3f,-0.5f);
    for(int i=0;i<240;++i) { pose=rotated; Solve(pose,rig,state,turnedTarget,settings,true,1.0f/60); }
    for(int eye : {3,4}) assert(Angle(AnimationFrame(pose,eye).Rotate(settings.ModelForward),turnedTarget-pose.Position(eye))<0.01f);
    // Cached/paused animation: restore the actual captured locals before each solve.
    pose=MakePose(); SavedLocals saved; saved.Capture(pose,rig); state={};
    for(int i=0;i<1000;++i) { saved.Restore(pose); Solve(pose,rig,state,side,settings,true,1.0f/60); }
    for(int eye : {3,4}) assert(Angle(Forward(pose,eye),side-pose.Position(eye))<0.015f);
    saved.Restore(pose);
    for(int bone : rig.Bones) assert(Angle(Forward(pose,bone),{0,0,1})<0.0001f);
    // Disabled parts and a partial rig remain useful.
    auto eyeOnly=settings; eyeOnly.Neck.Weight=eyeOnly.Head.Weight=0;
    state={}; Advance(pose,state,rig,front,eyeOnly);
    assert(state.Angles[0].Yaw==0 && state.Angles[1].Yaw==0);
    Rig partial; partial.Bones={-1,2,-1,-1}; state={};
    Advance(pose,state,partial,side,settings);
    assert(state.Angles[1].Yaw>0.01f && state.Angles[0].Yaw==0);
    // No movement on paused/invalid time, and invalid targets relax safely.
    const auto held=state; pose=MakePose();
    assert(!Solve(pose,rig,state,side,settings,true,0));
    assert(state.Angles[1].Yaw==held.Angles[1].Yaw);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(int i=0;i<240;++i) { pose=MakePose(); Solve(pose,rig,state,{nan,0,0},settings,true,1.0f/60); }
    assert(AtRest(state));
    // Rigid globals can conceal a nonuniform ancestor compensated by child local scale.
    pose=MakePose();
    pose.Nodes[0].Local=aiMatrix4x4(aiVector3D(2,1,1),aiQuaternion(),aiVector3D());
    pose.Nodes[1].Local=aiMatrix4x4(aiVector3D(.5f,1,1),aiQuaternion(),aiVector3D(0,1,0));
    assert(pose.CaptureBindPose());
    assert(HumanoidIk::RigidScale(pose.Nodes[2].Global));
    state={}; assert(!Solve(pose,rig,state,side,settings,true,1.0f/60));
    // Deletion is invalidated synchronously, before an ECS can reuse the target ID.
    LookAtRuntime runtime; runtime.Active=true; runtime.Tracking=true; runtime.Target=42;
    runtime.State.Angles[1].Yaw=0.1f;
    InvalidateLookAtTarget(runtime,7);
    assert(runtime.Tracking && runtime.Target==42);
    InvalidateLookAtTarget(runtime,42);
    assert(!runtime.Tracking && runtime.Target==UINT32_MAX && runtime.Active);
    assert(runtime.State.Angles[1].Yaw==0.1f); // preserve the smooth return, not a snap
    puts("LookAt solver: passed");
}
