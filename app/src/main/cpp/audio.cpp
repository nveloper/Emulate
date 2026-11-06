#include "audio.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <thread>

#define LOG_TAG "EmuAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

bool AudioOutput::start() {
    std::lock_guard<std::mutex> lock(streamMutex_);
    wanted_ = true;
    return openLocked();
}

void AudioOutput::stop() {
    std::lock_guard<std::mutex> lock(streamMutex_);
    wanted_ = false;
    closeLocked();
}

bool AudioOutput::openLocked() {
    if (stream_) return true;

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);

    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGE("openStream failed: %s", AAudio_convertResultToText(result));
        stream_ = nullptr;
        return false;
    }

    int32_t burst = AAudioStream_getFramesPerBurst(stream_);
    AAudioStream_setBufferSizeInFrames(stream_, burst * 2);
    double rate = AAudioStream_getSampleRate(stream_);
    outRate_ = rate;
    // Keep ~50 ms queued: enough headroom for frame-time jitter without noticeable latency.
    targetFill_ = std::max<size_t>(burst * 4, static_cast<size_t>(rate * 0.05));
    LOGI("AAudio stream: %.0f Hz, burst %d, target fill %zu", rate, burst, targetFill_.load());

    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
        LOGE("requestStart failed: %s", AAudio_convertResultToText(result));
        closeLocked();
        return false;
    }
    return true;
}

void AudioOutput::closeLocked() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
}

void AudioOutput::errorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    // Typically AAUDIO_ERROR_DISCONNECTED (headphones unplugged, BT switch). The stream must not be
    // closed from inside the callback, so reopen it from another thread.
    auto* self = static_cast<AudioOutput*>(user);
    LOGI("AAudio error %s, reopening", AAudio_convertResultToText(error));
    std::thread([self] {
        std::lock_guard<std::mutex> lock(self->streamMutex_);
        self->closeLocked();
        if (self->wanted_) self->openLocked();
    }).detach();
}

aaudio_data_callback_result_t AudioOutput::dataCallback(AAudioStream*, void* user, void* audioData, int32_t numFrames) {
    auto* self = static_cast<AudioOutput*>(user);
    auto* out = static_cast<int16_t*>(audioData);
    size_t read = self->readPos_.load(std::memory_order_relaxed);
    size_t avail = self->writePos_.load(std::memory_order_acquire) - read;
    size_t n = std::min<size_t>(avail, static_cast<size_t>(numFrames));
    for (size_t i = 0; i < n; i++) {
        size_t idx = ((read + i) & (kCapacity - 1)) * 2;
        out[i * 2] = self->ring_[idx];
        out[i * 2 + 1] = self->ring_[idx + 1];
    }
    if (n < static_cast<size_t>(numFrames)) {
        memset(out + n * 2, 0, (numFrames - n) * 2 * sizeof(int16_t));
    }
    self->readPos_.store(read + n, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

size_t AudioOutput::available() const {
    return writePos_.load(std::memory_order_acquire) - readPos_.load(std::memory_order_acquire);
}

void AudioOutput::write(int16_t l, int16_t r) {
    size_t w = writePos_.load(std::memory_order_relaxed);
    if (w - readPos_.load(std::memory_order_acquire) >= kCapacity) return;  // full: drop
    size_t idx = (w & (kCapacity - 1)) * 2;
    ring_[idx] = l;
    ring_[idx + 1] = r;
    writePos_.store(w + 1, std::memory_order_release);
}

void AudioOutput::setInputRate(double rate) {
    if (rate > 0) inRate_ = rate;
}

void AudioOutput::clear() {
    readPos_.store(writePos_.load());
    frac_ = 0.0;
    prev_[0] = prev_[1] = 0;
    stretching_ = false;
}

void AudioOutput::setTempo(double tempo) {
    if (tempo <= 0.0 || tempo == tempo_) return;
    tempo_ = tempo;
    tempoAdjust_ = 1.0;
}

double AudioOutput::fillError() const {
    const double target = static_cast<double>(targetFill_.load());
    return std::clamp((static_cast<double>(available()) - target) / target, -1.0, 1.0);
}

void AudioOutput::prime() {
    clear();
    const size_t frames = targetFill_.load();
    for (size_t i = 0; i < frames; i++) write(0, 0);
}

void AudioOutput::push(const int16_t* frames, size_t count) {
    const double error = fillError();
    if (tempo_ == 1.0) {
        stretching_ = false;
        resample(frames, count, error);
        return;
    }
    const double rate = inRate_.load();
    if (!stretching_ || rate != stretchRate_) {
        if (rate != stretchRate_) stretch_.configure(rate);
        else stretch_.reset();
        stretchRate_ = rate;
        stretching_ = true;
    }
    // The game may not quite reach the chosen speed (or overshoot it a little): steer the tempo by
    // how full the buffer is, slowly enough not to warble.
    tempoAdjust_ = std::clamp(tempoAdjust_ * (1.0 + 0.004 * error), 0.2, 1.5);
    const double tempo = tempo_ * tempoAdjust_ * (1.0 + 0.05 * error);
    stretch_.put(frames, count);
    stretched_.clear();
    stretch_.take(tempo, stretched_);
    resample(stretched_.data(), stretched_.size() / 2, error);
}

void AudioOutput::resample(const int16_t* frames, size_t count, double fillError) {
    // Input frames consumed per output frame; a fuller buffer means we consume input faster
    // (produce fewer output frames) and vice versa.
    const double step = (inRate_.load() * speed_.load() / outRate_.load()) * (1.0 + 0.005 * fillError);

    for (size_t i = 0; i < count; i++) {
        const int16_t l = frames[i * 2];
        const int16_t r = frames[i * 2 + 1];
        while (frac_ < 1.0) {
            const int ol = prev_[0] + static_cast<int>((l - prev_[0]) * frac_);
            const int orr = prev_[1] + static_cast<int>((r - prev_[1]) * frac_);
            write(static_cast<int16_t>(ol), static_cast<int16_t>(orr));
            frac_ += step;
        }
        frac_ -= 1.0;
        prev_[0] = l;
        prev_[1] = r;
    }
}
