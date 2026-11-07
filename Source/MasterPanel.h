#pragma once

#include <JuceHeader.h>
#include "Loudness.h"

namespace te = tracktion;
class MasterLimiterPlugin;

// The strip right of the timeline: loudness meters, the limiter's gain reduction, and the limiter
// control (one "Limiter push" knob at the bottom, shown via View > Limiter Control).
//
// Meters (LUFS over a steady 3 s window, scale -20..0): one bar per song under the playhead, in
// start-time order (outgoing left, incoming right), in the song's own colour; always at least two
// slots so the layout doesn't jump; a third or fourth song squeezes the bars (animated). Each bar
// shows what the song adds to the mix (after its volume and EQ lines), with a line at the song's
// normal loudness (measured + level matching). The "Mix" bar is everything added together BEFORE
// the limiter (after it, the limiter would squash any bump away); it turns orange more than 2 dB
// above the loudest song's line (a transition bump). No number readouts (user, 2026-10-21: the
// bars and the timeline show it all; the export reports its loudness instead).
class MasterPanel : public juce::Component,
                    public juce::TooltipClient,
                    private juce::Timer
{
public:
    MasterPanel();

    void setEdit (te::Edit*);
    void setLimiterControlVisible (bool);

    void paint (juce::Graphics&) override;
    void resized() override;
    juce::String getTooltip() override;

    static constexpr int preferredWidth = 214;

private:
    void timerCallback() override;
    MasterLimiterPlugin* getLimiter() const;
    void refreshControls();
    void updateSongs (double dt);
    void resetSongMeters();

    te::Edit* edit = nullptr;

    // One song under the playhead.
    struct Song
    {
        juce::String id;   // the song: a clip, or all the pieces of a chopped song together (familyOf)
        juce::String name;
        juce::Colour colour;
        std::optional<double> normalLufs;           // its line: measured loudness + level matching
        mixtopia::loudness::Meter afterLines;
        float position = 0.0f;                      // slot, animated
    };

    std::vector<Song> songs;                        // in start-time order
    float slots = 2.0f;                             // animated
    double lastPosition = 0.0;

    float reductionDb = 0.0f;
    mixtopia::loudness::Meter mixMeter;             // before the limiter (the Mix bar)
    bool wasPlaying = false;
    double mixLufs = -100.0;

    juce::Slider push;

    juce::Rectangle<int> meterArea, limiterArea, reductionArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterPanel)
};
