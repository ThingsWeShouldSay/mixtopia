#pragma once

#include <JuceHeader.h>
#include "TimelineComponent.h"

namespace te = tracktion;

// The whole mix at a glance (M7, top right): one mini row per lane, each song a block in its colour,
// numbered by start time with a short title (the full name in a tooltip); a box for the part shown
// in the lanes, the playhead, and a time scale.
//   - click: move the playhead there (the lanes scroll to it if it's off screen)
//   - drag the box: scroll the lanes
// A song pointed out by the "already in the mix" warning gets a yellow outline ("first copy").
class MixOverview : public juce::Component,
                    public juce::TooltipClient,
                    private juce::Timer
{
public:
    MixOverview();

    void setTimeline (TimelineComponent*, te::Edit*);

    // Outlines songs for a while (first copies of songs being added again).
    void highlight (const std::vector<te::EditItemID>&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    struct Block
    {
        te::EditItemID id;
        juce::String number;   // "3", or a chopped song's piece: "3c" (Chops.h)
        int lane = 0;
        double start = 0.0, end = 0.0;
        juce::String shortName, fullName;
        juce::Colour colour;
        double loopFrom = 0.0, loopTo = 0.0;   // the repeats (edit seconds), if the song has a loop
        int repeats = 0;
    };

    void timerCallback() override;
    std::vector<Block> getBlocks() const;
    juce::Rectangle<float> getRowsArea() const;
    double getTotalSeconds() const;
    float timeToX (double seconds) const;
    double xToTime (float x) const;
    juce::Rectangle<float> getVisibleBox() const;
    juce::Rectangle<float> blockBounds (const Block&, int numLanes) const;

    TimelineComponent* timeline = nullptr;
    te::Edit* edit = nullptr;

    std::vector<te::EditItemID> highlighted;
    double highlightUntilMs = 0.0;
    bool draggingBox = false;
    double dragOffsetSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixOverview)
};
