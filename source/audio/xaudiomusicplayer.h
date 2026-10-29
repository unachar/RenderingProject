#pragma once
// Windows-only XAudio2 WAV playback foundation for the live music editor.
// PCM16 and IEEE 32-bit float WAV; no compressed codecs or streaming yet.
#include <xaudio2.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#pragma comment(lib, "xaudio2.lib")

namespace LiveMusic
{
class XAudioMusicPlayer
{
  public:
    XAudioMusicPlayer() = default;
    ~XAudioMusicPlayer() { Shutdown(); }
    XAudioMusicPlayer(const XAudioMusicPlayer&) = delete;
    XAudioMusicPlayer& operator=(const XAudioMusicPlayer&) = delete;

    bool Initialize()
    {
        if (m_Engine) return true;
        if (FAILED(XAudio2Create(&m_Engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(m_Engine->CreateMasteringVoice(&m_Master))) { Shutdown(); return false; }
        return true;
    }

    void Shutdown()
    {
        Close();
        if (m_Master) { m_Master->DestroyVoice(); m_Master = nullptr; }
        if (m_Engine) { m_Engine->Release(); m_Engine = nullptr; }
    }

    bool OpenWav(const std::string& path)
    {
        Close();
        if (!Initialize()) return false;
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        char riff[4]{}, wave[4]{};
        uint32_t fileBytes = 0;
        if (!Read(input, riff, 4) || std::memcmp(riff, "RIFF", 4) != 0 ||
            !Read(input, &fileBytes, 4) || !Read(input, wave, 4) ||
            std::memcmp(wave, "WAVE", 4) != 0) return false;

        uint64_t riffRemaining = fileBytes >= 4 ? static_cast<uint64_t>(fileBytes - 4) : 0;
        bool gotFmt = false, gotData = false;
        WAVEFORMATEX format{};
        std::vector<uint8_t> pcm;
        while (riffRemaining >= 8 && input)
        {
            char id[4]{};
            uint32_t length = 0;
            if (!Read(input, id, 4) || !Read(input, &length, 4)) return false;
            riffRemaining -= 8;
            const uint64_t padded = static_cast<uint64_t>(length) + (length & 1u);
            if (padded > riffRemaining || length > 512u * 1024u * 1024u) return false;
            if (std::memcmp(id, "fmt ", 4) == 0)
            {
                if (length < 16 || length > 4096) return false;
                std::vector<uint8_t> bytes(length);
                if (!Read(input, bytes.data(), length)) return false;
                std::memcpy(&format, bytes.data(), 16);
                format.cbSize = 0;
                gotFmt = true;
            }
            else if (std::memcmp(id, "data", 4) == 0)
            {
                pcm.resize(length);
                if (!Read(input, pcm.data(), length)) return false;
                gotData = true;
            }
            else
            {
                input.seekg(length, std::ios::cur);
                if (!input) return false;
            }
            if (length & 1u) { input.ignore(1); if (!input) return false; }
            riffRemaining -= padded;
        }
        if (!gotFmt || !gotData || pcm.empty()) return false;
        if (!((format.wFormatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) ||
              (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32)))
            return false;
        const uint32_t align = static_cast<uint32_t>(format.nChannels) * format.wBitsPerSample / 8;
        if (!format.nChannels || !format.nSamplesPerSec || format.nBlockAlign != align ||
            format.nAvgBytesPerSec != align * format.nSamplesPerSec || !align ||
            pcm.size() % align || pcm.size() > XAUDIO2_MAX_BUFFER_BYTES) return false;
        if (FAILED(m_Engine->CreateSourceVoice(&m_Voice, &format))) return false;
        m_Format = format;
        m_Pcm = std::move(pcm);
        m_TotalFrames = m_Pcm.size() / m_Format.nBlockAlign;
        m_OffsetFrames = 0;
        return QueueFrom(0);
    }

    bool Play()
    {
        if (!m_Voice) return false;
        XAUDIO2_VOICE_STATE state{};
        m_Voice->GetState(&state);
        if (!state.BuffersQueued && (m_OffsetFrames >= m_TotalFrames || !QueueFrom(m_OffsetFrames))) return false;
        if (FAILED(m_Voice->Start())) return false;
        m_Playing = true;
        return true;
    }

    void Pause()
    {
        if (m_Voice) m_Voice->Stop();
        m_Playing = false;
    }

    void Stop()
    {
        Pause();
        Seek(0.0);
    }

    bool Seek(double seconds)
    {
        if (!m_Voice || !std::isfinite(seconds)) return false;
        const bool resume = m_Playing;
        Pause();
        const double clamped = std::clamp(seconds, 0.0, DurationSeconds());
        const auto frame = static_cast<uint64_t>(clamped * m_Format.nSamplesPerSec);
        if (!QueueFrom(std::min(frame, m_TotalFrames))) return false;
        if (resume) return Play();
        return true;
    }

    double TimeSeconds() const
    {
        if (!m_Voice || !m_Format.nSamplesPerSec) return 0.0;
        XAUDIO2_VOICE_STATE state{};
        m_Voice->GetState(&state);
        const uint64_t played = state.SamplesPlayed >= m_StartSamples ?
            state.SamplesPlayed - m_StartSamples : 0;
        return static_cast<double>(std::min(m_TotalFrames, m_OffsetFrames + played)) /
               m_Format.nSamplesPerSec;
    }

    double DurationSeconds() const
    {
        return m_Format.nSamplesPerSec ?
            static_cast<double>(m_TotalFrames) / m_Format.nSamplesPerSec : 0.0;
    }
    bool IsPlaying() const
    {
        if (!m_Playing || !m_Voice) return false;
        XAUDIO2_VOICE_STATE state{};
        m_Voice->GetState(&state);
        return state.BuffersQueued != 0;
    }
    bool Loaded() const { return m_Voice != nullptr; }

  private:
    static bool Read(std::ifstream& f, void* bytes, size_t size)
    {
        return size <= static_cast<size_t>((std::numeric_limits<std::streamsize>::max)()) &&
            static_cast<bool>(f.read(static_cast<char*>(bytes), static_cast<std::streamsize>(size)));
    }

    bool QueueFrom(uint64_t frame)
    {
        if (!m_Voice) return false;
        m_Voice->Stop();
        m_Voice->FlushSourceBuffers();
        m_OffsetFrames = std::min(frame, m_TotalFrames);
        XAUDIO2_VOICE_STATE state{};
        m_Voice->GetState(&state);
        m_StartSamples = state.SamplesPlayed;
        if (m_OffsetFrames == m_TotalFrames) return true;
        const auto startByte = m_OffsetFrames * m_Format.nBlockAlign;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(m_Pcm.size() - startByte);
        buffer.pAudioData = m_Pcm.data() + startByte;
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        return SUCCEEDED(m_Voice->SubmitSourceBuffer(&buffer));
    }

    void Close()
    {
        m_Playing = false;
        if (m_Voice) { m_Voice->Stop(); m_Voice->DestroyVoice(); m_Voice = nullptr; }
        m_Pcm.clear();
        m_Format = {};
        m_TotalFrames = m_OffsetFrames = m_StartSamples = 0;
    }

    IXAudio2* m_Engine = nullptr;
    IXAudio2MasteringVoice* m_Master = nullptr;
    IXAudio2SourceVoice* m_Voice = nullptr;
    WAVEFORMATEX m_Format{};
    std::vector<uint8_t> m_Pcm;
    uint64_t m_TotalFrames = 0, m_OffsetFrames = 0, m_StartSamples = 0;
    bool m_Playing = false;
};
} // namespace LiveMusic
