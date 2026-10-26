#include "pch.h"
#include "animator.h"
#include "componentmanager.h"
#include "vmdanimationimpoter.h"
#include "scenefile.h"
#include "scenedepth.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>

// Component headers register their types at startup. These tests need only the
// registration storage, not a running World, GPU renderer or entity lifecycle.
unordered_map<type_index, ComponentTypeID>& ComponentTypeRegistry::TypeIds()
{
    static unordered_map<type_index, ComponentTypeID> values;
    return values;
}

vector<void (*)(EntityID)>& ComponentTypeRegistry::CreateCallbacks()
{
    static vector<void (*)(EntityID)> values;
    return values;
}

vector<void (*)(EntityID)>& ComponentTypeRegistry::ClearCallbacks()
{
    static vector<void (*)(EntityID)> values;
    return values;
}

vector<void (*)()>& ComponentTypeRegistry::ResetCallbacks()
{
    static vector<void (*)()> values;
    return values;
}

ComponentTypeID& ComponentTypeRegistry::NextTypeId()
{
    static ComponentTypeID value = 0;
    return value;
}

namespace
{
    template <typename T> void Write(std::ofstream& stream, const T& value)
    {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void WriteCamera(std::ofstream& stream, uint32_t frame, float x, float roll = 0.0f, bool perspective = true)
    {
        Write(stream, frame);
        Write(stream, -10.0f);
        for (float value : {x, 2.0f, 3.0f, 0.0f, 0.0f, roll})
        {
            Write(stream, value);
        }
        for (int channel = 0; channel < 6; ++channel)
        {
            const unsigned char curve[] = {0, 127, 0, 127};
            stream.write(reinterpret_cast<const char*>(curve), sizeof(curve));
        }
        Write(stream, uint32_t{60});
        Write(stream, static_cast<uint8_t>(perspective ? 0 : 1));
    }

    void CreateFixture(const char* path, uint32_t count = 2, bool mixedProjection = false)
    {
        std::ofstream stream(path, std::ios::binary);
        char header[30] = "Vocaloid Motion Data 0002";
        char model[20] = "camera";
        stream.write(header, sizeof(header));
        stream.write(model, sizeof(model));
        Write(stream, uint32_t{0}); // Bone and morph sections may be empty.
        Write(stream, uint32_t{0});
        Write(stream, mixedProjection ? uint32_t{4} : count);
        if (mixedProjection)
        {
            WriteCamera(stream, 0, 0.0f);
            WriteCamera(stream, 2, 2.0f, 0.0f, false);
            WriteCamera(stream, 6, 4.0f);
            WriteCamera(stream, 10, 6.0f, 0.0f, false);
        }
        else
        {
            WriteCamera(stream, 30, 10.0f, DirectX::XM_PIDIV2);
            WriteCamera(stream, 0, 0.0f);
        }
    }

    bool Near(float value, float expected)
    {
        return std::abs(value - expected) < 0.001f;
    }
}

int main(int argc, char** argv)
{
    const char* path = "bin/validation/vmd-camera-fixture.vmd";
    CreateFixture(path);
    VmdAnimation animation;
    assert(LoadVmdAnimationFile(path, animation));
    assert(animation.CameraTrack.size() == 2);
    assert(animation.CameraTrack.front().Frame == 0);
    assert(animation.MaxFrame == 30);
    auto sample = SampleVmdCameraTrack(animation.CameraTrack, 15.0f);
    assert(Near(sample.Center.x, 5.0f));
    assert(Near(sample.Distance, -10.0f));
    assert(Near(sample.Fov, DirectX::XM_PI / 3.0f));
    assert(Near(SampleVmdCameraTrack(animation.CameraTrack, -1.0f).Center.x, 0.0f));
    assert(Near(SampleVmdCameraTrack(animation.CameraTrack, 100.0f).Center.x, 10.0f));

    // Adjacent camera keys are cuts, including when sampled between MMD frames.
    auto cutTrack = animation.CameraTrack;
    cutTrack[1].Frame = 1;
    assert(Near(SampleVmdCameraTrack(cutTrack, 0.5f).Center.x, 0.0f));
    assert(Near(SampleVmdCameraTrack(cutTrack, 1.0f).Center.x, 10.0f));

    auto curvedTrack = animation.CameraTrack;
    curvedTrack[1].P1[0] = {0.0f, 1.0f};
    curvedTrack[1].P2[0] = {0.0f, 1.0f};
    curvedTrack[1].Center.y = 12.0f;
    curvedTrack[1].Distance = -20.0f;
    curvedTrack[1].Fov = DirectX::XM_PIDIV2;
    sample = SampleVmdCameraTrack(curvedTrack, 15.0f);
    assert(sample.Center.x > 9.0f); // Uses the destination key's camera Bezier.
    assert(Near(sample.Center.y, 7.0f)); // Channels retain independent curves.
    assert(Near(sample.Distance, -15.0f));
    assert(Near(sample.Fov, DirectX::XM_PI * 5.0f / 12.0f));

    CameraComponent camera;
    TransformComponent transform;
    camera.Animation.Scale = 0.15f;
    camera.Animation.Offset = {1.0f, 0.0f, 0.0f};
    assert(Animator::Play(camera, path));
    Animator::Update(camera, transform, 0.0f);
    assert(Near(transform.Position.x, 1.0f));
    assert(Near(transform.Position.y, 0.3f));
    assert(Near(transform.Position.z, -1.05f));
    assert(camera.Animation.IsActive && camera.Animation.IsPlaying);
    Animator::Update(camera, transform, 0.5f);
    assert(Near(camera.Animation.CurrentTime, 0.5f));
    assert(Near(transform.Position.x, 1.75f));
    Animator::Pause(camera);
    Animator::Update(camera, transform, 1.0f);
    assert(Near(camera.Animation.CurrentTime, 0.5f));
    assert(camera.Animation.IsActive);
    assert(Animator::Play(camera, false));
    Animator::SetSpeed(camera, 2.0f);
    Animator::Update(camera, transform, 0.25f);
    assert(Near(camera.Animation.CurrentTime, 1.0f));
    assert(std::abs(camera.Up.x) > 0.99f); // Roll reaches the view's up direction.
    camera.Animation.Loop = false;
    Animator::Update(camera, transform, 1.0f);
    assert(!camera.Animation.IsPlaying && camera.Animation.IsActive);
    assert(Near(camera.Animation.CurrentTime, 1.0f));
    Animator::Stop(camera);
    assert(!camera.Animation.IsActive && !camera.Animation.IsPlaying);

    camera.Animation.Loop = true;
    assert(Animator::Play(camera));
    Animator::Update(camera, transform, 0.0f);
    assert(camera.Animation.CutThisFrame);
    Animator::Update(camera, transform, 0.0f);
    assert(camera.Animation.CutThisFrame); // Remains pending until rendering.
    camera.Animation.CutThisFrame = false;
    Animator::Update(camera, transform, 0.5f);
    assert(!camera.Animation.CutThisFrame);
    Animator::Update(camera, transform, 0.1f); // Speed 2: crosses the 31-frame loop.
    assert(camera.Animation.CutThisFrame);
    assert(Near(transform.Position.x, 1.25f));
    camera.Animation.CutThisFrame = false;
    Animator::Update(camera, transform, 31.0f / 30.0f);
    assert(camera.Animation.CutThisFrame); // A multi-loop step also resets history.

    // A combined pitch + roll pose verifies orientation beyond a single axis.
    auto mixedClip = std::make_shared<VmdAnimation>();
    VmdCameraKeyframe mixedKey;
    mixedKey.Center = {0, 2, 3};
    mixedKey.Distance = -10;
    mixedKey.Rotation = {DirectX::XM_PIDIV2, 0, DirectX::XM_PIDIV2};
    mixedClip->CameraTrack.push_back(mixedKey);
    CameraComponent mixedCamera;
    mixedCamera.Animation.Clip = mixedClip;
    mixedCamera.Animation.IsActive = true;
    Animator::Update(mixedCamera, transform, 0.0f);
    assert(Near(transform.Position.y, -8.0f));
    assert(Near(transform.Position.z, 3.0f));
    assert(Near(mixedCamera.Up.x, 1.0f));
    assert(Near(mixedCamera.Target.y, -7.0f));

    Json::Value saved;
    SceneFile::Archive writer(SceneFile::Mode::Write, saved);
    SceneFile::Visit(writer, camera);
    CameraComponent restored;
    SceneFile::Archive reader(SceneFile::Mode::Read, saved);
    SceneFile::Visit(reader, restored);
    assert(restored.Animation.Path == path);
    assert(Near(restored.Animation.Scale, 0.15f));
    assert(Near(restored.Animation.Offset.x, 1.0f));
    assert(Near(restored.Animation.Speed, 2.0f));
    assert(!restored.Animation.Clip && !restored.Animation.IsActive);
    Json::Value oldScene(Json::objectValue);
    SceneFile::Archive oldReader(SceneFile::Mode::Read, oldScene);
    CameraComponent oldCamera;
    SceneFile::Visit(oldReader, oldCamera);
    assert(Near(oldCamera.Up.y, 1.0f) && oldCamera.Animation.Path.empty());

    // Failed loads leave the existing pose and playback untouched.
    assert(!Animator::Play(camera, "bin/validation/missing-camera.vmd"));
    assert(camera.Animation.IsActive && camera.Animation.Path == path);
    CreateFixture(path);
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(122); // First camera key's orthographic flag.
        const uint8_t orthographic = 1;
        stream.write(reinterpret_cast<const char*>(&orthographic), sizeof(orthographic));
    }
    assert(Animator::Play(camera, path));
    camera.Animation.Loop = false;
    Animator::Update(camera, transform, 0.0f);
    assert(!camera.IsOrthographic);
    camera.Animation.CutThisFrame = false;
    Animator::Update(camera, transform, 1.0f);
    assert(camera.IsOrthographic && camera.Animation.CutThisFrame);
    assert(Near(camera.OrthographicHeight, 10.0f * 0.15f * 25.0f / 45.0f));

    const auto orthographic = SceneDepth::ReverseZOrthographicLH(10.0f, 2.0f, 0.1f, 100.0f);
    const auto project = [&](float x, float y, float z) {
        return DirectX::XMVector4Transform(DirectX::XMVectorSet(x, y, z, 1), orthographic);
    };
    assert(Near(DirectX::XMVectorGetZ(project(0, 0, 0.1f)), 1.0f));
    assert(Near(DirectX::XMVectorGetZ(project(0, 0, 100.0f)), 0.0f));
    assert(Near(DirectX::XMVectorGetX(project(10, 5, 1)), 1.0f));
    assert(Near(DirectX::XMVectorGetY(project(10, 5, 50)), 1.0f));
    assert(Near(DirectX::XMVectorGetW(project(0, 0, 50)), 1.0f));
    DirectX::XMFLOAT4X4 orthoValues;
    DirectX::XMStoreFloat4x4(&orthoValues, orthographic);
    SceneDepth::SetProjectionJitter(orthoValues, {0.01f, -0.02f});
    assert(Near(orthoValues._41, 0.01f) && Near(orthoValues._42, -0.02f));
    assert(Near(SceneDepth::ProjectionJitter(orthoValues).x, 0.01f));
    assert(Near(orthoValues._31, 0.0f));
    Json::Value orthoScene;
    camera.OrthographicHeight = 12.5f;
    SceneFile::Archive orthoWriter(SceneFile::Mode::Write, orthoScene);
    SceneFile::Visit(orthoWriter, camera);
    CameraComponent restoredOrtho;
    SceneFile::Archive orthoReader(SceneFile::Mode::Read, orthoScene);
    SceneFile::Visit(orthoReader, restoredOrtho);
    assert(restoredOrtho.IsOrthographic && Near(restoredOrtho.OrthographicHeight, 12.5f));
    CreateFixture(path, 3); // Count exceeds the available 61-byte camera records.
    assert(!LoadVmdAnimationFile(path, animation));
    std::filesystem::remove(path);
    if (argc == 2 && std::string(argv[1]) == "--runtime-fixture")
    {
        CreateFixture("bin/validation/vmd-camera-runtime.vmd", 4, true);
    }
    std::puts("VMD camera parser, interpolation and Animator tests: PASS");
}
