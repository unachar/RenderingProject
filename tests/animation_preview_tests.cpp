#include "../source/editor/animationpreviewstate.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace AnimationPreview;

int main()
{
    std::vector<Clip> clips{{"body", "body.vmd", true, 4.0f}, {"face", "face.vmd", true, 2.0f}};
    assert(ValidateClips(clips));
    assert(Duration(clips) == 4.0f);
    clips[0].Enabled = false;
    assert(Duration(clips) == 2.0f);
    clips[1].Enabled = false;
    assert(Duration(clips) == 0.0f);
    clips[0].Enabled = clips[1].Enabled = true;
    clips[1].Name = "body";
    assert(!ValidateClips(clips));
    clips[1].Name = "face";
    clips[1].Path.clear();
    assert(!ValidateClips(clips));
    assert(ValidateClips({}));

    clips = {{"body", "body.vmd", true, 4.0f}, {"face", "face.VMD", true, 2.0f}, {"idle", "idle.fbx", false, 3.0f}};
    SetClipEnabled(clips, 2, true);
    assert(!clips[0].Enabled && !clips[1].Enabled && clips[2].Enabled);
    SetClipEnabled(clips, 0, true);
    SetClipEnabled(clips, 1, true);
    assert(clips[0].Enabled && clips[1].Enabled && !clips[2].Enabled);
    const auto selection = BuildSelection(clips);
    assert(selection.Names.size() == 3 && selection.Paths.size() == 3);
    assert(selection.PlayingNames == std::vector<std::string>({"body", "face"}));
    SetClipEnabled(clips, 0, false);
    assert(!clips[0].Enabled && clips[1].Enabled);

    Playback playback;
    playback.Playing = true;
    playback.Time = 3.5f;
    playback.Speed = 2.0f;
    playback.Advance(0.5f, 4.0f);
    assert(std::abs(playback.Time - 0.5f) < 0.0001f);
    playback.Playing = false;
    playback.Seek(3.0f, 4.0f);
    playback.Advance(1.0f, 4.0f);
    assert(playback.Time == 3.0f);
    playback.Seek(100.0f, 4.0f);
    assert(playback.Time == 4.0f);
    assert(SampleTime(playback.Time, 4.0f) < 4.0f);
    assert(SampleTime(3.0f, 2.0f) < 2.0f);
    assert(SampleTime(3.0f, 2.0f) > 1.9f);
    assert(LayerSampleTime(3.0f, 2.0f, 4.0f, true) == 3.0f);
    assert(LayerSampleTime(3.0f, 2.0f, 4.0f, false) < 2.0f);
    playback.Seek(-3.0f, 4.0f);
    assert(playback.Time == 0.0f);
    playback.Playing = true;
    playback.Advance(std::numeric_limits<float>::quiet_NaN(), 4.0f);
    playback.Advance(-1.0f, 4.0f);
    assert(playback.Time == 0.0f);
    playback.Loop = false;
    playback.Advance(10.0f, 4.0f);
    assert(playback.Time == 4.0f && !playback.Playing);
    playback.Playing = true;
    playback.Advance(1.0f, 0.0f);
    assert(playback.Time == 0.0f && !playback.Playing);
    std::cout << "Animation preview state tests passed\n";
}
