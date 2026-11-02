#pragma once
// ============================================================================
// SequencerWaveform : Sequencer の音声クリップに表示する波形のピーク（min / max）キャッシュ。
//
//  ・WAV（PCM16 / float32、XAudioMusicPlayer と同じ対応形式）をワーカースレッドで 1 回だけ解析し、
//    kBucketsPerSecond 単位のピークに縮約して保持する。UI スレッドは完成済みの配列を参照するだけ。
//  ・ファイルの更新時刻・サイズが変わったら作り直す。
// ============================================================================
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SequencerWaveform
{
    inline constexpr int kBucketsPerSecond = 400;

    struct Peaks
    {
        float Duration = 0.0f;
        int SampleRate = 0;
        int Channels = 0;
        std::vector<float> Min; // kBucketsPerSecond / 秒、モノラルに縮約済み（-1..1）
        std::vector<float> Max;
    };

    enum class State
    {
        None,
        Loading,
        Ready,
        Failed,
    };

    // 未解析なら解析を開始し、現在の状態を返す。Ready のとき peaks に結果が入る（所有権は共有）。
    State Request(const std::string& utf8Path, std::shared_ptr<const Peaks>& peaks);

    // 同期解析（テスト・ワーカー用）。
    bool Analyze(const std::string& utf8Path, Peaks& peaks, std::string* error = nullptr);

    // 画面の 1 区間 [t0, t1) のピーク。データなしは false。
    bool Range(const Peaks& peaks, float t0, float t1, float& minimum, float& maximum);

    void Clear();
}
