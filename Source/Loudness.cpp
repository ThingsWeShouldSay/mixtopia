#include "Loudness.h"

namespace mixtopia::loudness
{
    // The two K-weighting stages for a sample rate (the formulas libebur128 uses; at 48 kHz they
    // give the coefficients printed in ITU-R BS.1770-4).
    void KWeighting::prepare (double sampleRate)
    {
        const auto pi = juce::MathConstants<double>::pi;

        // Stage 1: high shelf, +4 dB above ~1.7 kHz.
        auto f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        auto k = std::tan (pi * f0 / sampleRate);
        auto vh = std::pow (10.0, gain / 20.0);
        auto vb = std::pow (vh, 0.4996667741545416);
        auto a0 = 1.0 + k / q + k * k;
        shelf = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                  2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };

        // Stage 2: high-pass at 38 Hz.
        f0 = 38.13547087602444;
        q = 0.5003270373238773;
        k = std::tan (pi * f0 / sampleRate);
        a0 = 1.0 + k / q + k * k;
        highPass = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
    }

    double toLufs (double meanSquare)
    {
        return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : -std::numeric_limits<double>::infinity();
    }

    void Meter::addStep (double meanSquare)
    {
        steps.push_back (meanSquare);

        // 400 ms blocks with 75% overlap.
        auto n = steps.size();

        if (n >= 4)
            blocks.push_back ((steps[n - 4] + steps[n - 3] + steps[n - 2] + steps[n - 1]) / 4.0);

        // Only the last 3 s of steps are needed (short-term); blocks are kept for the gating.
        constexpr size_t shortTermSteps = 30;

        if (steps.size() > shortTermSteps)
            steps.erase (steps.begin(), steps.end() - (long) shortTermSteps);
    }

    double Meter::getShortTerm() const
    {
        if (steps.empty())
            return -std::numeric_limits<double>::infinity();

        double sum = 0.0;

        for (auto s : steps)
            sum += s;

        return toLufs (sum / (double) steps.size());
    }

    double Meter::getIntegrated() const
    {
        // Absolute gate (-70 LUFS), then relative gate (10 LU below the gated average).
        auto gatedMean = [this] (double thresholdLufs)
        {
            double sum = 0.0;
            int count = 0;

            for (auto b : blocks)
                if (toLufs (b) > thresholdLufs)
                {
                    sum += b;
                    ++count;
                }

            return count > 0 ? sum / count : 0.0;
        };

        auto absolute = gatedMean (-70.0);

        if (absolute <= 0.0)
            return -std::numeric_limits<double>::infinity();

        return toLufs (gatedMean (toLufs (absolute) - 10.0));
    }

    double measure (juce::AudioFormatReader& reader, const std::function<bool()>& shouldStop)
    {
        auto channels = (int) juce::jmax (1u, reader.numChannels);
        auto rate = reader.sampleRate;
        std::vector<KWeighting> filters ((size_t) channels);

        for (auto& f : filters)
            f.prepare (rate);

        // Mean square (summed over channels) of each 100 ms step.
        auto stepLength = (int) std::round (rate * 0.1);
        Meter meter;
        double stepSum = 0.0;
        int inStep = 0;

        // A mono file plays on both speakers, so it counts twice (as identical left and right).
        auto channelWeight = channels == 1 ? 2.0 : 1.0;

        constexpr int chunk = 65536;
        juce::AudioBuffer<float> buffer (channels, chunk);

        for (juce::int64 pos = 0; pos < reader.lengthInSamples; pos += chunk)
        {
            if (shouldStop())
                return -std::numeric_limits<double>::infinity();

            auto num = (int) juce::jmin ((juce::int64) chunk, reader.lengthInSamples - pos);

            if (! reader.read (&buffer, 0, num, pos, true, true))
                break;

            for (int i = 0; i < num; ++i)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto y = filters[(size_t) ch].process (buffer.getSample (ch, i));
                    stepSum += channelWeight * y * y;
                }

                if (++inStep == stepLength)
                {
                    meter.addStep (stepSum / stepLength);
                    stepSum = 0.0;
                    inStep = 0;
                }
            }
        }

        return meter.getIntegrated();
    }

    double matchGainDb (double songLufs)
    {
        if (! std::isfinite (songLufs))
            return 0.0;

        return juce::jlimit (0.0, maxRaiseDb, referenceLufs - songLufs);
    }
}
