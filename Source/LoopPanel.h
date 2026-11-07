#pragma once

#include <JuceHeader.h>
#include "Loops.h"
#include "Theme.h"

class TimelineComponent;

// The loop panel (right-click a song > "Repeat part of this song...", or the clip bar's "Edit loop"):
// the precise tool for a song's loop. It floats over the window - drag it by its title - and stays
// open while you move the playhead or play. Every change is one undo step and applies at once.
//   - loop start / end: type a time ("3:21.418", "201.4" or "bar 97.1"), "Set at playhead" (snaps
//     like dragging a song: Snap on = bars, Shift = beats; Snap off = free, Shift = bars), nudges
//     (beat, 10 ms, 1 ms each way)
//   - "move the whole loop": the same nudges for both ends together (the length stays)
//   - repeats (1-64) and the crossfade at each join (0-20 ms, default 4)
//   - readouts: the loop's length and how much longer the song gets; a warning (with "Snap to N
//     beats") when the loop isn't a whole number of beats
//   - Listen to the join (step 2): 2 s before to 2 s after the first join, round and round, through
//     the real mix ("this song only": its lane soloed meanwhile); the range follows the loop as it
//     changes; stops with the button, Esc, Space or Stop, and puts the playhead, the transport's
//     loop setting and the solos back
//   - "Move everything after the loop later" (off by default): each change also moves the songs and
//     tempo points after the loop. (When the song would run into the next song on its lane, they
//     move anyway - songs never overlap on a lane.)
//   - lines inside the loop: only the first time / copy into every repeat
//   - Remove loop, Done
class LoopPanel : public juce::Component,
                  private juce::Timer
{
public:
    LoopPanel (te::Edit&, TimelineComponent&);

    // Shows the panel for a song (which must have a loop) at a spot that keeps the loop in view.
    // `within`: where it may go; it starts at `preferredTop` and moves up if it would run off the bottom.
    void showFor (te::EditItemID clip, juce::Rectangle<int> avoid, juce::Rectangle<int> within, int preferredTop);
    bool isListening() const            { return listening; }
    void startListening();
    void stopListening();

    std::function<void (const juce::String&)> onStatusMessage;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void visibilityChanged() override;

    static constexpr int panelWidth = 488;

private:
    struct Nudges
    {
        juce::OwnedArray<juce::TextButton> buttons;
    };

    te::AudioClipBase* getClip() const;
    void timerCallback() override;
    void refresh();
    int getIdealHeight();
    int contentHeight = 600;
    void status (const juce::String& s)     { if (onStatusMessage) onStatusMessage (s); }

    // One undo step that changes the loop (validated first; false if it was refused).
    bool commit (mixtopia::loops::Loop, const juce::String& undoName);
    void setFromPlayhead (bool isStart);
    void setFromText (bool isStart);
    void setUpNudges (Nudges&, std::function<void (double seconds, bool isBeat)>);

    te::Edit& edit;
    TimelineComponent& timeline;
    te::EditItemID clipID;
    std::optional<mixtopia::loops::Loop> shown;

    // Listen to the join.
    static constexpr double listenSeconds = 2.0;   // either side of the join (user)
    juce::Range<double> getListenRange() const;
    void applySongOnly();
    void refreshListenButton();
    bool listening = false;
    juce::Range<double> shownRange;

    struct Saved
    {
        te::TimeRange loopRange;
        bool looping = false;
        te::TimePosition position;
        std::vector<bool> solo;
    } saved;

    juce::TextButton closeButton { juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) };
    juce::TextEditor startBox, endBox;
    juce::TextButton startAtPlayhead { "Set at playhead" }, endAtPlayhead { "Set at playhead" };
    Nudges startNudges, endNudges, wholeNudges;
    juce::TextButton repeatsDown { juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) }, repeatsUp { "+" },
                     fadeDown { juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) }, fadeUp { "+" };
    juce::TextButton snapBeatsButton;
    mixtopia::theme::IconButton listenButton { mixtopia::theme::Icon::play, "Listen to the join" };
    juce::ToggleButton songOnlyToggle { "this song only" };
    juce::ToggleButton moveLaterToggle, linesOnce { "only the first time" }, linesCopy { "copy into every repeat" };
    juce::TextButton removeButton { "Remove loop" }, doneButton { "Done" };

    juce::ComponentDragger dragger;
    juce::ComponentBoundsConstrainer constrainer;

    // Layout rows (for the labels painted in paint()).
    juce::Rectangle<int> startLabelRow, endLabelRow, wholeLabelRow, repeatsRow, fadeRow, readoutRow, fractionRow,
                         moveLaterRow, linesRow, bottomRow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopPanel)
};
