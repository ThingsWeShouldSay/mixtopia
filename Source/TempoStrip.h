#pragma once

#include <JuceHeader.h>
#include "TempoRamps.h"

namespace te = tracktion;

// The thin strip under the bar ruler showing the mix's tempo as a line (M6; View > Tempo Strip).
// Dots are the tempo points the user placed (TempoRamps.h); a ramp is the slope between two of
// them. The first point is drawn as a ring: its BPM is also the tempo before it (user: no label
// needed for that).
//   - right-click: "Change tempo here..." (one dialog: the BPM) places a point on that bar, or
//     changes the point there; on a point also "Delete this point"
//   - drag a dot sideways: move it (whole bars; shows the ramp into it)
//   - double-click a dot: type its BPM
// Every change is one undo step and keeps the songs on their bars.
class TempoStrip : public juce::Component
{
public:
    // The timeline's view: seconds <-> x in this component's coordinates (shared with the timeline).
    TempoStrip (te::Edit&, std::function<double (double)> timeToX, std::function<double (double)> xToTime, int headerWidth);

    // Asks for a BPM and puts a tempo point on the bar line nearest `editSeconds`.
    void changeTempoAt (double editSeconds);

    std::function<void (const juce::String&)> onStatusMessage;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int preferredHeight = 34;

private:
    double beatToX (double beat) const;
    double xToBeat (double x) const;
    float bpmToY (double bpm, double low, double high) const;
    std::pair<double, double> getBpmRange (const mixtopia::tempo::TempoPlan&) const;
    std::optional<size_t> pointAt (juce::Point<int>) const;
    juce::String describePoint (const mixtopia::tempo::TempoPlan&, size_t point) const;
    bool isSteep (const mixtopia::tempo::TempoPlan&, size_t point) const;

    void editPoint (size_t point);
    void deletePoint (size_t point);
    void showMenu (juce::Point<int>);
    void status (const juce::String& text)      { if (onStatusMessage) onStatusMessage (text); }

    te::Edit& edit;
    std::function<double (double)> timeToX, xToTime;
    const int headerWidth;

    std::optional<size_t> hoverPoint, dragPoint;
    mixtopia::tempo::TempoPlan dragStartPlan;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoStrip)
};
