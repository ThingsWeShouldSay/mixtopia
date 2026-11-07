#pragma once

#include <JuceHeader.h>
#include "ClipEnvelopes.h"
#include "Biquad.h"
#include "Loudness.h"

namespace te = tracktion;

// The effect every clip carries: volume, a 3-band DJ EQ and a filter, driven by the clip's envelope
// lines (see ClipEnvelopes.h). Lives in the clip's own plugin list, so it processes only that clip's
// audio (after time-stretching) and moves with it.
//
// EQ: a 3-way Linkwitz-Riley (24 dB/oct) crossover at 300 Hz and 3 kHz, like a DJ mixer's
// isolator: bass = LR4 low-pass(300), the rest is split at 3 kHz into mids and treble, and the bass
// band goes through the 3 kHz crossover too so all three stay phase-aligned and sum flat. A band at
// killDb is removed. The crossover shifts phase (inaudibly), so it only runs on clips that have EQ
// points; clips without any pass bit-exactly (and a flat volume line leaves them untouched too).
//
// FILTER (M11): a low-pass then a high-pass, each 24 dB/oct = two state-variable filter stages (Simper's
// trapezoidal SVF, which stays clean while its cutoff moves) with a moderate resonance. All stages
// always run on a clip with filter points - the line only moves their cutoffs, the unused one sits
// fully open (no resonance) - so crossing "off" in the middle switches nothing and can't click; in
// the dead zone the output blends back to the untouched song (exactly it at the middle). The cutoff
// follows the line per sample. Clips without filter points never run it (bit-exact).
//
// The envelope points are converted to edit time on the message thread whenever the Edit changes
// and handed to the audio thread as an immutable snapshot (try-lock, never blocks). Values ramp
// per sample from block start to block end, so there's no zipper noise.
class ClipMixerPlugin : public te::Plugin
{
public:
    explicit ClipMixerPlugin (te::PluginCreationInfo);
    ~ClipMixerPlugin() override;

    static const char* getPluginName()      { return "Mixtopia Clip Mixer"; }
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

    // Re-reads the clip's envelopes now (normally done asynchronously after any Edit change).
    void updateSnapshot();

    // Makes sure a clip has exactly one clip mixer; returns it.
    static ClipMixerPlugin* ensureOnClip (te::AudioClipBase&);

    // What the song adds to the mix (after its volume and EQ lines) in 100 ms loudness steps
    // (K-weighted mean squares, see loudness::Meter) while it plays. Collected by the master panel;
    // exports aren't measured.
    int takeLoudnessSteps (double* dest, int maxSteps);

    static constexpr double bassCrossoverHz = 300.0;
    static constexpr double trebleCrossoverHz = 3000.0;

private:
    struct Snapshot
    {
        // Per parameter: (edit seconds, dB), sorted by time.
        std::array<std::vector<std::pair<double, float>>, mixtopia::envelopes::numParams> points;
        bool usesEq = false;   // any bass/mids/treble points
        bool usesFilter = false;   // any filter points
        float matchGain = 1.0f;   // automatic level matching (LevelMatch.h), on top of the volume line
        bool fadeIn = false, fadeOut = false;   // chopping: edges that came from a cut (Chops.h)
        double clipStart = 0.0, clipEnd = 0.0;   // edit seconds
    };

    struct EditWatcher : public juce::ValueTree::Listener, public juce::AsyncUpdater
    {
        explicit EditWatcher (ClipMixerPlugin& o) : owner (o) {}
        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier&) override { if (! v.hasType (te::IDs::TRANSPORT)) triggerAsyncUpdate(); }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override                { triggerAsyncUpdate(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override         { triggerAsyncUpdate(); }
        void handleAsyncUpdate() override                                                     { owner.updateSnapshot(); }
        ClipMixerPlugin& owner;
    };

    using Gains = std::array<float, mixtopia::envelopes::numParams>;
    Gains valuesAt (const Snapshot&, double editSeconds) const;

    // Applies the EQ and volume, ramping each value from start to end across the block.
    void process (juce::AudioBuffer<float>&, int startSample, int numSamples, const Gains& start, const Gains& end, bool usesEq);

    // Chopping: a short fade at a piece's edges that came from a cut (Chops.h fadeSeconds).
    void applyEdgeFades (juce::AudioBuffer<float>&, int startSample, int numSamples, te::TimeRange editTime, const Snapshot&);

    // Applies the filter, its position (Gains[filter]) ramping from start to end across the block.
    void applyFilter (juce::AudioBuffer<float>&, int startSample, int numSamples, float startPosition, float endPosition);

    // One 12 dB/oct state-variable filter stage, stereo.
    struct Svf
    {
        float ic1[2] {}, ic2[2] {};
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.4142f;
        void setCutoff (double hz, double q, double sampleRate);
        void clear()   { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }
    };

    // [0]: the resonant stage, [1]: the fixed Q 0.541 one (24 dB/oct together).
    std::array<Svf, 2> lowPassStages, highPassStages;
    bool filterRunning = false;
    Snapshot fades;   // the edges' fades (copied with the snapshot; only its fade fields are used)
    float wetNow = -1.0f;   // the dry/filtered blend, following filterWet() over ~20 ms (-1 = start at it)
    float wetSmoothing = 0.999f;
    static constexpr double wetSeconds = 0.02;
    double sampleRate = 44100.0;

    EditWatcher watcher { *this };
    juce::SpinLock snapshotLock;
    std::unique_ptr<Snapshot> snapshot;

    // LR4 = two Butterworth sections. [0..1]: bass low-pass, [2..3]: high-pass at 300 Hz,
    // [4..5]: mids low-pass and [6..7]: treble high-pass at 3 kHz, [8..11]: the bass band through
    // the 3 kHz low-pass + high-pass (phase alignment).
    std::array<mixtopia::Biquad, 12> filters;
    bool eqRunning = false;
    std::array<float, mixtopia::envelopes::numParams> lastGains { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };   // (filter: off)
    bool hasLastGains = false;

    // Loudness metering of the output.
    std::array<mixtopia::loudness::KWeighting, 2> kWeighting;
    double stepSum = 0.0;
    int inStep = 0, stepLength = 4410;
    static constexpr int maxPendingSteps = 64;
    juce::AbstractFifo stepFifo { maxPendingSteps };
    std::array<double, maxPendingSteps> pendingSteps {};

    void meter (const juce::AudioBuffer<float>&, int start, int num);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipMixerPlugin)
};
