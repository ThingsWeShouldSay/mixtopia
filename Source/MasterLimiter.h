#pragma once

#include <JuceHeader.h>
#include "Loudness.h"

namespace te = tracktion;

// A transparent look-ahead peak limiter on the Edit's master, used for playback and export alike.
// Overlapping songs (and time-stretching songs mastered up to 0 dBFS) push peaks over full
// scale; peak-normalising the whole mix to fit made exports ~8 dB quieter than playback, and
// playback itself clipped at the sound card. The limiter catches just the peaks.
//
// Stereo-linked. Gain = moving average (over the look-ahead) of a release-smoothed sliding minimum
// of the gain each sample needs, which guarantees no sample exceeds the ceiling while attacks
// stay smooth. Parameters (ceiling, drive, release) are stored in the plugin state and set from
// the master panel (MasterPanel.h).
class MasterLimiterPlugin : public te::Plugin
{
public:
    explicit MasterLimiterPlugin (te::PluginCreationInfo);
    ~MasterLimiterPlugin() override;

    static const char* getPluginName()      { return "Mixtopia Limiter"; }
    static const char* xmlTypeName;
    static juce::ValueTree create();

    juce::String getName() const override                   { return getPluginName(); }
    juce::String getPluginType() override                   { return xmlTypeName; }
    juce::String getSelectableDescription() override        { return getName(); }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    double getLatencySeconds() override                     { return lookAheadSeconds; }

    // Metering for the UI. Each "take" returns what happened since the previous call.
    float takePeakGainReductionDb()                         { return peakReductionDb.exchange (0.0f); }

    // Loudness in 100 ms steps (K-weighted mean squares, see loudness::Meter) of what comes in: the
    // songs added together, before drive and limiting (the master panel's Mix bar - after the
    // limiter it would squash any transition bump away). Copies the steps finished since the
    // previous call into dest, returns how many. (Until 2026-10-23 it also measured the output's
    // loudness and peaks, for meters that were replaced on 2026-10-21.)
    int takeLoudnessSteps (double* dest, int maxSteps);

    juce::CachedValue<float> ceilingDb, driveDb, releaseMs;

    // The one control shown ("Limiter push", user 2026-10-21: three knobs took too much room):
    // push = drive, 0..maxPushDb; the release lengthens with it from 60 to 150 ms (less distortion
    // and pumping when limiting hard); the ceiling stays at -1 dB (safe for MP3).
    static constexpr float maxPushDb = 9.0f, fixedCeilingDb = -1.0f;
    static float releaseForPush (float pushDb)  { return 60.0f + 90.0f * juce::jlimit (0.0f, maxPushDb, pushDb) / maxPushDb; }
    void setPush (float pushDb);

    // Puts saved settings (projects from the three-knob days) onto that scheme, keeping the push.
    void applyOneKnobScheme()                               { setPush (driveDb.get()); }

    static constexpr double lookAheadSeconds = 0.003;

    // Makes sure an Edit has exactly one limiter at the end of its master plugins; returns it.
    static MasterLimiterPlugin* ensureOnMaster (te::Edit&);

private:
    int lookAhead = 0;                  // samples
    std::vector<float> delayL, delayR;  // look-ahead delay lines (lookAhead samples)
    std::vector<float> required;        // ring of required gains, for the sliding minimum
    std::vector<long long> minQueue;    // monotonic queue of sample indices (values in `required`)
    std::vector<float> averageRing;     // ring of smoothed gains, for the moving average
    int minHead = 0, minTail = 0;
    long long sampleIndex = 0;
    double averageSum = 0.0;
    float envelope = 1.0f;
    std::atomic<float> peakReductionDb { 0.0f };
    std::array<mixtopia::loudness::KWeighting, 2> kWeighting;   // left, right (input)
    double inputStepSum = 0.0;
    int inStep = 0, stepLength = 4410;
    static constexpr int maxPendingSteps = 256;     // 25 s; the UI collects them 30 times a second
    juce::AbstractFifo stepFifo { maxPendingSteps };
    std::array<double, maxPendingSteps> pendingSteps {};

    static void storeMax (std::atomic<float>& target, float value)
    {
        auto previous = target.load();

        while (value > previous && ! target.compare_exchange_weak (previous, value))
        {}
    }

    float processSample (float& left, float& right, float ceiling, float drive, float releaseCoeff);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterLimiterPlugin)
};
