#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "Library.h"
#include "LoudnessOverview.h"

namespace te = tracktion;

// The slim preview strip under the library list (M7): a play button, the song's name and a small
// rough waveform. A quick check of a song, not part of the mix:
//   - double-click a song in the library (play()) to hear it from the start
//   - click the waveform to play from there, drag across it to scrub (you hear where you are)
//   - Left/Right arrows (jumpBars): 8 bars back/forward on the song's beat grid
//   - Esc, or a click anywhere outside the strip, stops it
// It plays the song's decoded copy at its own tempo through its own small player on the same sound
// card (JUCE mixes it with Tracktion's output), never through the mix, so it's never exported.
// Starting a preview pauses the mix (onStartedPlaying); pressing Play on the mix stops it.
class PreviewStrip : public juce::Component,
                     private juce::Timer
{
public:
    PreviewStrip (te::Engine&, mixtopia::library::Library&);
    ~PreviewStrip() override;

    void play (const juce::String& path, double fromSeconds = 0.0);
    void stop();
    bool isPlaying() const;

    // Shown only while a song is being previewed (playing or paused with its button); hidden again
    // when it's stopped (Esc, a click elsewhere, the song's end) so the list gets the room.
    bool isActive() const                       { return active; }
    std::function<void()> onActiveChanged;
    void jumpBars (int bars);

    std::function<void()> onStartedPlaying;
    std::function<void (const juce::String&)> onStatusMessage;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    static constexpr int preferredHeight = 34;

private:
    // Stops the preview on a click anywhere else in the window.
    struct ClickElsewhere : public juce::MouseListener
    {
        explicit ClickElsewhere (PreviewStrip& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent&) override;
        PreviewStrip& owner;
    };

    void timerCallback() override;
    bool shownPlaying = false, active = false;
    void setActive (bool);
    juce::Rectangle<int> getWaveformArea() const;
    double xToSeconds (int x) const;
    bool load (const juce::String& path);
    void seek (double seconds);

    te::Engine& engine;
    mixtopia::library::Library& library;
    juce::String path;                          // the song loaded ({} = none)
    juce::File playable;

    juce::TimeSliceThread readAhead { "Preview read-ahead" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> source;
    juce::AudioSourcePlayer player;

    mixtopia::LoudnessOverviews overviews;
    mixtopia::theme::IconButton playButton { mixtopia::theme::Icon::play };
    ClickElsewhere clickElsewhere { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreviewStrip)
};
