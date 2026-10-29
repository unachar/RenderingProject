#pragma once
#include "lookatsettings.h"
#include "humanoidik.h"

namespace Gaze
{
using Pose = HumanoidIk::Pose;
using Settings = LookAtSettings;
struct Rig { std::array<int, 4> Bones{-1, -1, -1, -1}; };
// Capture only controlled local transforms; restoring them never rewinds unrelated physics bones.
struct SavedLocals
{
    Rig Bindings;
    std::array<aiMatrix4x4,4> Local{};
    void Capture(const Pose& pose,const Rig& rig)
    {
        Bindings=rig;
        for(int i=0;i<4;++i) if(pose.Contains(rig.Bones[i])) Local[i]=pose.Nodes[rig.Bones[i]].Local;
    }
    void Restore(Pose& pose) const
    {
        for(int i=0;i<4;++i) if(pose.Contains(Bindings.Bones[i])) pose.Nodes[Bindings.Bones[i]].Local=Local[i];
        pose.UpdateGlobals();
    }
};
inline float Radians(float degrees) { return degrees * (3.14159265358979323846f / 180.0f); }
inline float Safe(float value, float fallback) { return std::isfinite(value) ? value : fallback; }
inline const LookAtJointSettings& Joint(const Settings& settings, int index)
{ return index == 0 ? settings.Neck : index == 1 ? settings.Head : settings.Eyes; }
inline bool AtRest(const State& state)
{
    for(const auto& a : state.Angles) if(std::abs(a.Yaw)>0.00001f || std::abs(a.Pitch)>0.00001f) return false;
    return true;
}
inline float Approach(float current, float target, const LookAtJointSettings& settings, float dt)
{
    const float response = std::max(0.0f, Safe(settings.ResponseSpeed, 12));
    const float limit = Radians(std::max(0.0f, Safe(settings.MaxSpeedDegrees, 180))) * dt;
    const float step = (target - current) * (-std::expm1(-response * dt));
    const float result = current + std::clamp(step, -limit, limit);
    return std::abs(result)<0.000001f && std::abs(target)<0.000001f ? 0.0f : result;
}
inline float OutsideComfort(float angle, float comfort)
{ return std::copysign(std::max(0.0f, std::abs(angle)-comfort), angle); }
inline aiQuaternion AnimationFrame(const Pose& pose, int bone)
{
    auto inverseBind = HumanoidIk::Rotation(pose.Nodes[static_cast<size_t>(bone)].BindGlobal);
    inverseBind.Conjugate();
    return HumanoidIk::Rotation(pose.Nodes[static_cast<size_t>(bone)].Global) * inverseBind;
}
inline bool Solve(Pose& pose, const Rig& rig, State& state, aiVector3D target,
                  const Settings& settings, bool tracking, float deltaTime)
{
    if (!pose.UpdateGlobals()) return false;
    int anchor = -1;
    for(int bone : rig.Bones)
    {
        if (!pose.Contains(bone)) continue;
        // RotateWorld decomposes local and parent transforms. A rigid global can
        // hide a nonuniform parent compensated by a child: reject the whole chain.
        int ancestor=bone;
        for(size_t step=0;pose.Contains(ancestor) && step<pose.Nodes.size();++step)
        {
            const auto& node=pose.Nodes[static_cast<size_t>(ancestor)];
            if(!HumanoidIk::RigidScale(node.Local) || !HumanoidIk::RigidScale(node.Global) ||
               !HumanoidIk::RigidScale(node.BindGlobal)) return false;
            ancestor=node.Parent;
        }
        if(ancestor!=-1) return false;
        if (anchor < 0) anchor = bone;
    }
    if (anchor < 0) return false;
    const float dt = std::clamp(Safe(deltaTime,0),0.0f,0.25f);
    if (dt <= 0) return false;
    const auto forward = HumanoidIk::Unit(settings.ModelForward);
    const auto upInput = HumanoidIk::Unit(settings.ModelUp,{0,1,0});
    const auto right = HumanoidIk::Unit(upInput ^ forward,HumanoidIk::Perpendicular(forward));
    const auto up = HumanoidIk::Unit(forward ^ right,{0,1,0});
    tracking = tracking && HumanoidIk::Finite(target);
    // Keep extremely close targets outside an interocular-distance-based comfort sphere.
    const int centerBone = pose.Contains(rig.Bones[1]) ? rig.Bones[1] : anchor;
    const auto center = pose.Position(centerBone);
    auto direction = target-center;
    float minDistance=0;
    if(pose.Contains(rig.Bones[2]) && pose.Contains(rig.Bones[3]))
        minDistance=(pose.Position(rig.Bones[2])-pose.Position(rig.Bones[3])).Length()*3.0f;
    if(direction.SquareLength()<1e-12f) tracking=false;
    else if(tracking && direction.Length()<minDistance) target=center+HumanoidIk::Unit(direction)*minDistance;
    // Use the incoming animated chain, before any LookAt rotations, for a stable rear fade.
    auto inverseFrame=AnimationFrame(pose,anchor); inverseFrame.Conjugate();
    const auto localDirection=inverseFrame.Rotate(HumanoidIk::Unit(direction));
    const float yaw=std::abs(std::atan2(localDirection*right,localDirection*forward));
    const float fadeStart=Radians(std::clamp(Safe(settings.RearFadeStartDegrees,100),0.0f,179.0f));
    const float fadeEnd=std::max(fadeStart+0.0001f,Radians(std::clamp(Safe(settings.RearFadeEndDegrees,140),0.0f,180.0f)));
    const float fadeT=std::clamp((yaw-fadeStart)/(fadeEnd-fadeStart),0.0f,1.0f);
    const float fade=1.0f-fadeT*fadeT*(3.0f-2.0f*fadeT);
    const float weight=tracking ? std::clamp(Safe(settings.Weight,0),0.0f,1.0f)*fade : 0;
    bool changed=false;
    for(int part=0;part<4;++part)
    {
        const int bone=rig.Bones[part];
        if(!pose.Contains(bone)) { state.Angles[part]={}; continue; }
        // Never rotate the same mapped node twice when a custom mapping aliases joints.
        bool duplicate=false;
        for(int earlier=0;earlier<part;++earlier) duplicate |= rig.Bones[earlier]==bone;
        if(duplicate) { state.Angles[part]={}; continue; }
        const auto frame=AnimationFrame(pose,bone);
        auto inverse=frame; inverse.Conjugate();
        const auto relative=inverse.Rotate(target-pose.Position(bone));
        const float x=relative*right,y=relative*up,z=relative*forward;
        Angles desired;
        if(weight>0 && relative.SquareLength()>1e-12f)
        {
            desired.Yaw=std::atan2(x,z);
            desired.Pitch=std::atan2(y,std::sqrt(x*x+z*z));
            if(part<2)
            {
                const float comfort=Radians(std::clamp(Safe(settings.EyeComfortDegrees,10),0.0f,90.0f));
                desired.Yaw=OutsideComfort(desired.Yaw,comfort);
                desired.Pitch=OutsideComfort(desired.Pitch,comfort);
            }
        }
        const auto& joint=Joint(settings,part);
        const float jointWeight=weight*std::clamp(Safe(joint.Weight,0),0.0f,1.0f);
        const float yawLimit=Radians(std::clamp(Safe(joint.MaxYawDegrees,0),0.0f,89.0f));
        const float pitchLimit=Radians(std::clamp(Safe(joint.MaxPitchDegrees,0),0.0f,89.0f));
        desired.Yaw=std::clamp(desired.Yaw*jointWeight,-yawLimit,yawLimit);
        desired.Pitch=std::clamp(desired.Pitch*jointWeight,-pitchLimit,pitchLimit);
        auto& current=state.Angles[part];
        current.Yaw=Approach(Safe(current.Yaw,0),desired.Yaw,joint,dt);
        current.Pitch=Approach(Safe(current.Pitch,0),desired.Pitch,joint,dt);
        // Hard limits remain hard even when settings shrink during tracking.
        current.Yaw=std::clamp(current.Yaw,-yawLimit,yawLimit);
        current.Pitch=std::clamp(current.Pitch,-pitchLimit,pitchLimit);
        const aiQuaternion yawRotation(frame.Rotate(up),current.Yaw);
        const aiQuaternion pitchRotation(yawRotation.Rotate(frame.Rotate(right)),-current.Pitch);
        if(std::abs(current.Yaw)>0 || std::abs(current.Pitch)>0)
        {
            pose.RotateWorld(bone,pitchRotation*yawRotation);
            changed=true;
        }
    }
    return changed;
}
}
