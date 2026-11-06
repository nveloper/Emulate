#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Changes the tempo of stereo audio without changing its pitch, so fast-forward and slow motion
// sound like the game rather than chipmunks or a tape running slow.
//
// WSOLA (waveform-similarity overlap-add): the output is built from short overlapping grains of
// the input, each cross-faded into the last. Grains are taken from the input [tempo] times further
// apart than they are laid down in the output; each one is nudged to where its waveform best lines
// up with the previous grain's continuation, so the joins don't beat or click.
class TimeStretch {
public:
    // Sets the grain size for [sampleRate] and clears all state.
    void configure(double sampleRate);
    void reset();

    // Queues interleaved stereo input frames.
    void put(const int16_t* frames, size_t count);
    // Appends as much output as the queued input allows (interleaved stereo). [tempo] is input
    // frames consumed per output frame: 2 = twice as fast, 0.5 = half speed.
    void take(double tempo, std::vector<int16_t>& out);

private:
    int bestStart(int nominal, int target) const;

    int grain_ = 0;  // grain length in frames
    int hop_ = 0;    // output distance between grains (half a grain)
    int seek_ = 0;   // how far a grain may move to line up
    std::vector<float> window_;   // Hann, grain_ long
    std::vector<float> in_;       // queued input, interleaved stereo
    std::vector<float> mono_;     // the same, mid channel (for lining grains up)
    std::vector<float> overlap_;  // second half of the last grain, already windowed (stereo)
    double nominal_ = 0.0;        // where the next grain should start, in frames into in_
    int previous_ = 0;            // where the last grain started (may be < 0 after trimming in_)
    bool started_ = false;        // a grain has been laid down (there is something to line up with)
};
