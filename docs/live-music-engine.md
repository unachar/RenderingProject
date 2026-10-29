# Live music engine — XAudio2 foundation (phase 1)

This is an **opt-in audio foundation**, not yet an integrated editor or complete concert playback system.

## Added
- `source/audio/xaudiomusicplayer.h`: RAII XAudio2 device/master/source voice, PCM16 / IEEE float32 RIFF WAV loading, playback, pause, stop, seek and sample-derived time.
- `source/audio/livemusicclock.h`: frame-rate-independent tempo grid, tempo changes, seconds/beats conversion, bars and crossed-beat enumeration.
- `tests/live_music_clock_tests.cpp`: portable tempo-clock assertions.

Both components deliberately avoid touching the existing renderer and scene format, so existing scenes remain unchanged. XAudio2 audio functions are Windows-only. The clock module is portable C++17.

## Minimal integration example (Windows)

```cpp
#include "audio/xaudiomusicplayer.h"
#include "audio/livemusicclock.h"

LiveMusic::XAudioMusicPlayer music;
LiveMusic::TempoMap tempo(128.0);
if (music.OpenWav("asset/music/live.wav"))
    music.Play();

// In a simulation update:
double songTime = music.TimeSeconds();
const auto beat = tempo.Sample(songTime);
double positionInBars = beat.AbsoluteBeat / 4.0;
```

`TempoMap::Set()` accepts sorted tempo changes at absolute times (seconds), beginning at time zero. Beat positions are zero-based quarter notes; `BeatInBar` is zero-based for the chosen beats-per-bar value. For time signatures with denominators other than 4, introduce an explicit meter map rather than relying on beats-per-bar alone.

For beat-triggered effects use `ForEachCrossedBeat(tempo, lastTime, nowTime, callback)` to avoid skipping beats when frames are delayed. **Do not fire old callbacks on backward seeks**; instead evaluate light/camera/post FX state directly at the target time.

## Known limitations / next phase

1. WAV PCM16/float32 only; no MP3, extensible WAV, multi-buffer streaming, music loop, decoder or waveform cache yet. The WAV is fully held in RAM. XAudio2's buffer-size limit is checked.
2. `SamplesPlayed` is a voice consumption clock, **not an exact audible speaker timestamp**. Seek, queueing and audio device latency require calibration for tight AV sync.
3. Playback code is intentionally not wired into `SystemManager`, `TimelineComponent`, scene/prefab persistence or ImGui; phase 2 should add `AudioSourceComponent`, `MusicPlayerSystem`, `LiveDirector`, beat-grid UI, and audio-master transport controls.
4. XAudio2 source voice operations should be performed on the owning engine thread. Do not create/destroy voices from an XAudio2 callback. WAV parsing rejects unsupported or damaged files.
5. No Windows build or audible-output validation was run in the authoring environment. It must be tested with a real audio device before production use.

### Suggested validation

- Portable timing: `cl /std:c++17 /EHsc /I. tests\\live_music_clock_tests.cpp /Fe:live_music_clock_tests.exe` (or `c++ -std=c++17 tests/live_music_clock_tests.cpp -o live_music_clock_tests`).
- Windows audio smoke test: in a small executable, include `source/audio/xaudiomusicplayer.h`, load a short PCM16 WAV, play, pause, seek, play again and verify duration/clock.
- Check 48kHz vs 44.1kHz, EOF, rapid seek, malformed RIFF, reinitialize/shutdown, and audio-device loss.

## Planned live transport

```text
Music file -> Audio decoder / streamer -> XAudio2 -> sound device
                                    |
                            Playback sample clock
                                    |
                          Audio-master LiveDirector
                   /          |          |         \\
               Dance VMD    Camera    Light      Post FX
```

Use absolute music position for animation pose, camera and lighting curve evaluation, and cross-beat events only for one-shot effects. Event trigger and visual state must stay separate to support seek and looping.
