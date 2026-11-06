#pragma once

#include "timestretch.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Stereo int16 audio output on AAudio.
//
// The core produces samples at its own rate (e.g. 32768 Hz for GBA). We resample them to the
// device rate with a linear resampler whose ratio is nudged by ±0.5% depending on how full the
// ring buffer is ("dynamic rate control"), so the video-paced emulation loop never drifts into
// buffer underruns or overruns.
//
// At other game speeds (fast-forward, slow motion) the sound is time-stretched to real time first,
// keeping its pitch.
class AudioOutput {
public:
    bool start();
    void stop();
    void setInputRate(double rate);
    // Interleaved stereo frames at the core's rate. Called from the emulation thread only.
    void push(const int16_t* frames, size_t count);
    void clear();
    // Drops queued audio and queues silence up to the target latency, so playback (re)starts with
    // headroom instead of hovering at empty and crackling until rate control fills the buffer.
    void prime();
    // The small difference between the emulation's actual and nominal rate when the loop is locked
    // to the display refresh (e.g. 60 / 59.73). Samples are played back that much faster/slower.
    void setSpeed(double ratio) { if (ratio > 0) speed_ = ratio; }
    // Game speed (fast-forward / slow motion). Emulation thread only.
    void setTempo(double tempo);

private:
    static aaudio_data_callback_result_t dataCallback(AAudioStream* stream, void* user, void* audioData, int32_t numFrames);
    static void errorCallback(AAudioStream* stream, void* user, aaudio_result_t error);
    bool openLocked();
    void closeLocked();
    size_t available() const;
    void write(int16_t l, int16_t r);
    void resample(const int16_t* frames, size_t count, double fillError);
    double fillError() const;

    static constexpr size_t kCapacity = 16384;  // stereo frames, power of two
    std::vector<int16_t> ring_ = std::vector<int16_t>(kCapacity * 2);
    std::atomic<size_t> readPos_{0};
    std::atomic<size_t> writePos_{0};

    std::atomic<double> inRate_{32768.0};
    std::atomic<double> outRate_{48000.0};
    std::atomic<double> speed_{1.0};
    std::atomic<size_t> targetFill_{2400};
    double frac_ = 0.0;
    int16_t prev_[2] = {0, 0};

    // Emulation thread.
    TimeStretch stretch_;
    std::vector<int16_t> stretched_;
    double tempo_ = 1.0;
    double stretchRate_ = 0.0;  // input rate stretch_ is configured for
    bool stretching_ = false;
    // Corrects the tempo for how fast the game really runs (a slow phone may not reach 8x).
    double tempoAdjust_ = 1.0;

    std::mutex streamMutex_;
    AAudioStream* stream_ = nullptr;
    std::atomic<bool> wanted_{false};
};
