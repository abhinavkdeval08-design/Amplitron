#pragma once

#include "audio/effect.h"
#include <vector>

namespace GuitarAmp {

/**
 * Pitch Shifter — shifts pitch by +/- 12 semitones using a dual-tap
 * granular overlap-add algorithm.
 *
 * Two read pointers scan through a circular buffer at a rate determined by
 * the pitch ratio. A raised-cosine (Hann) crossfade between the two taps
 * hides the grain boundary discontinuities.
 *
 * Controls: Shift (semitones), Fine (cents), Mix.
 */
class PitchShifter : public Effect {
public:
    PitchShifter();
    void process(float* buffer, int num_samples) override;
    void set_sample_rate(int sample_rate) override;
    void reset() override;
    const char* name() const override { return "Pitch Shifter"; }
    std::vector<EffectParam>& params() override { return params_; }

private:
    std::vector<EffectParam> params_;

    // Grain buffer
    std::vector<float> grain_buf_;
    int buf_size_ = 0;
    int write_pos_ = 0;

    // Two crossfading read taps (phase in [0, buf_size_))
    float read_phase_a_ = 0.0f;
    float read_phase_b_ = 0.0f;

    // Parameter smoothing
    float shift_smooth_ = 0.0f;
    float fine_smooth_ = 0.0f;
    float mix_smooth_ = 0.0f;

    float read_linear(float phase) const;
};

} // namespace GuitarAmp
