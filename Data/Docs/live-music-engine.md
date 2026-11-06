# Live music engine — XAudio2 foundation

The original phase-1 foundation is now connected to the editor Timeline. See [LiveDirector](live-director.md) for transport controls, scene/prefab persistence and stage synchronization. The following describes the underlying WAV player and tempo primitives.

## Added
- `source/audio/xaudiomusicplayer.h`: RAII XAudio2 device/master/source voice, PCM16 / IEEE float32 RIFF WAV loading, playback, pause, stop, seek and sample-derived time.
- `source/audio/livemusicclock.h`: frame-rate-independent tempo grid, tempo changes, seconds/beats conversion, bars and crossed-beat enumeration.
- `tests/live_music_clock_tests.cpp`: portable tempo-clock assertions.

The player and tempo primitives remain independent of the renderer. The Timeline integration adds optional scene/prefab fields while preserving existing property values and defaults. XAudio2 audio functions are Windows-only. The clock module is portable C++17.

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

1. WAV PCM16/float32 only; no MP3, extensible WAV, multi-buffer streaming, decoder or waveform cache yet. The WAV is fully held in RAM. XAudio2's buffer-size limit is checked. LiveDirector implements Timeline/song-boundary looping.
2. `SamplesPlayed` is a voice consumption clock, **not an exact audible speaker timestamp**. Seek, queueing and audio device latency require calibration for tight AV sync.
3. Playback is owned by each live Timeline and uses the sample clock as its master. Timeline contains the persisted WAV path, intro offset, BPM and beat offset. Standalone audio sources, multiple tempo-change editing and waveform UI remain outside this integration.
4. XAudio2 source voice operations should be performed on the owning engine thread. Do not create/destroy voices from an XAudio2 callback. WAV parsing rejects unsupported or damaged files.
5. The integrated Windows Release application compiles and links successfully. Audible speaker output and device-specific latency require separate confirmation; cursor progress alone does not establish audible timing.

Seeks now recreate the source voice on its owning thread to establish a new sample-zero origin. EOF is derived from the drained buffer queue because `XAUDIO2_END_OF_STREAM` resets `SamplesPlayed`; this avoids a backward clock jump at the end. The WAV path is opened as UTF-8 through `std::filesystem::u8path`.

The clock handling follows Microsoft's [voice-state documentation](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/ns-xaudio2-xaudio2_voice_state) and [flush/reset contract](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-flushsourcebuffers).

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
