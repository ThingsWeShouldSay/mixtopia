#pragma once

#include <JuceHeader.h>

namespace mixtopia
{
    // A second-order (12 dB/octave) Butterworth low- or high-pass filter for up to two channels.
    // Two in series make a 24 dB/octave Linkwitz-Riley filter. Used by the clip EQ (ClipMixer) and
    // the waveform analysis (LoudnessOverview).
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] {}, z2[2] {};

        void setLowPass (double sampleRate, double frequency)
        {
            auto w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            auto cosW = std::cos (w0), alpha = std::sin (w0) / (2.0 * butterworthQ), a0 = 1.0 + alpha;
            b0 = (1.0 - cosW) / 2.0 / a0;
            b1 = (1.0 - cosW) / a0;
            b2 = b0;
            a1 = -2.0 * cosW / a0;
            a2 = (1.0 - alpha) / a0;
        }

        void setHighPass (double sampleRate, double frequency)
        {
            auto w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            auto cosW = std::cos (w0), alpha = std::sin (w0) / (2.0 * butterworthQ), a0 = 1.0 + alpha;
            b0 = (1.0 + cosW) / 2.0 / a0;
            b1 = -(1.0 + cosW) / a0;
            b2 = b0;
            a1 = -2.0 * cosW / a0;
            a2 = (1.0 - alpha) / a0;
        }

        float process (float x, int channel = 0)
        {
            auto y = b0 * x + z1[channel];
            z1[channel] = b1 * x - a1 * y + z2[channel];
            z2[channel] = b2 * x - a2 * y;
            return (float) y;
        }

        void clear()
        {
            z1[0] = z1[1] = z2[0] = z2[1] = 0.0;
        }

        static constexpr double butterworthQ = 0.70710678118654752;
    };
}
