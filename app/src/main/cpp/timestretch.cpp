#include "timestretch.h"

#include <algorithm>
#include <cmath>

void TimeStretch::configure(double sampleRate) {
    // ~36 ms grains moving up to ~10 ms either way: long enough for low notes, short enough that
    // drums don't smear.
    hop_ = std::max(64, static_cast<int>(std::lround(sampleRate * 0.018)));
    grain_ = hop_ * 2;
    seek_ = std::max(16, static_cast<int>(std::lround(sampleRate * 0.010)));
    window_.resize(grain_);
    // Periodic Hann: two windows half a grain apart add up to exactly 1.
    for (int i = 0; i < grain_; i++) {
        window_[i] = 0.5f - 0.5f * std::cos(6.28318531f * i / grain_);
    }
    reset();
}

void TimeStretch::reset() {
    in_.clear();
    mono_.clear();
    overlap_.assign(static_cast<size_t>(hop_) * 2, 0.0f);
    nominal_ = 0.0;
    previous_ = 0;
    started_ = false;
}

void TimeStretch::put(const int16_t* frames, size_t count) {
    in_.reserve(in_.size() + count * 2);
    mono_.reserve(mono_.size() + count);
    for (size_t i = 0; i < count; i++) {
        const float l = frames[i * 2], r = frames[i * 2 + 1];
        in_.push_back(l);
        in_.push_back(r);
        mono_.push_back(l + r);
    }
}

int TimeStretch::bestStart(int nominal, int target) const {
    // Normalised cross-correlation of each candidate's first half with what the last grain's
    // second half was taken from (every other sample is plenty to find the alignment).
    const float* t = mono_.data() + target;
    int best = std::max(0, nominal);
    float bestScore = -1e30f;
    for (int x = std::max(0, nominal - seek_); x <= nominal + seek_; x++) {
        const float* c = mono_.data() + x;
        float dot = 0.0f, energy = 1e-3f;
        for (int i = 0; i < hop_; i += 2) {
            dot += c[i] * t[i];
            energy += c[i] * c[i];
        }
        const float score = dot / std::sqrt(energy);
        if (score > bestScore) {
            bestScore = score;
            best = x;
        }
    }
    return best;
}

void TimeStretch::take(double tempo, std::vector<int16_t>& out) {
    if (grain_ == 0 || tempo <= 0.0) return;
    for (;;) {
        const int frames = static_cast<int>(mono_.size());
        const int nominal = static_cast<int>(nominal_);
        int start;
        if (!started_) {
            if (nominal + grain_ > frames) break;
            start = nominal;
        } else {
            const int target = previous_ + hop_;
            if (std::max(nominal + seek_ + grain_, target + hop_) > frames) break;
            start = bestStart(nominal, target);
        }

        // First half of this grain + second half of the last one -> hop_ output frames.
        const float* g = in_.data() + static_cast<size_t>(start) * 2;
        for (int i = 0; i < hop_; i++) {
            for (int c = 0; c < 2; c++) {
                const float v = overlap_[i * 2 + c] + g[i * 2 + c] * window_[i];
                out.push_back(static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L)));
                overlap_[i * 2 + c] = g[(hop_ + i) * 2 + c] * window_[hop_ + i];
            }
        }
        previous_ = start;
        started_ = true;
        nominal_ += hop_ * tempo;

        // Drop input no later grain can reach (in large steps; erasing shifts the rest).
        const int keep = std::min(static_cast<int>(nominal_) - seek_, previous_ + hop_);
        if (keep > 8192) {
            in_.erase(in_.begin(), in_.begin() + static_cast<ptrdiff_t>(keep) * 2);
            mono_.erase(mono_.begin(), mono_.begin() + keep);
            nominal_ -= keep;
            previous_ -= keep;
        }
    }
}
