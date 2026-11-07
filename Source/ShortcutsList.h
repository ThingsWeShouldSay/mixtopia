#pragma once

#include <JuceHeader.h>
#include "Theme.h"

// Help > Keyboard Shortcuts... (Options until 2026-11-03): a read-only list of every shortcut, grouped. Keep it in step with
// MainComponent::keyPressed and the timeline's mouse handling. (Re-assigning keys: IDEAS.md.)
namespace mixtopia
{
    class ShortcutsList : public juce::Component
    {
    public:
        ShortcutsList()
        {
            setSize (2 * columnWidth + 3 * padding, juce::jmax (heightOf (left()), heightOf (right())) + padding);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::panel);
            paintColumn (g, left(), padding);
            paintColumn (g, right(), 2 * padding + columnWidth);
        }

    private:
        struct Group { juce::String title; std::vector<std::pair<juce::String, juce::String>> keys; };

        static std::vector<Group> left()
        {
            return {
                { "Playback", { { "Space", "Play / pause" },
                                { "Home", "Back to the start" },
                                { "F", "Follow the playhead (on / off)" },
                                { "Click the ruler or empty space", "Move the playhead there" },
                                { "Double-click a song", "Move the playhead there" } } },
                { "Lines", { { "V", "Add a volume point at the mouse" },
                             { "B", "Add a bass point at the mouse" },
                             { "G", "Add a mid point at the mouse" },
                             { "T", "Add a treble point at the mouse" },
                             { "R", "Add a filter point at the mouse" },
                             { "Shift + drag a point", "Move it finely (no sticky 0 dB)" },
                             { "Double-click a point", "Back to 0 dB" },
                             { "Right-click a point", "Delete it" } } },
                { "Loops", { { "I", "Loop in at the playhead" },
                             { "O", "Loop out at the playhead (makes the loop)" },
                             { "E", "Open the loop panel" },
                             { "Esc", "Cancel a loop in / stop listening to a join" } } },
            };
        }

        static std::vector<Group> right()
        {
            return {
                { "Songs", { { "Delete / Backspace", "Delete the selected song or piece" },
                             { "Shift + drag a song", "Snap to beats (to bars when Snap is off)" } } },
                { "Chopping", { { "C", "Cut the song at the playhead" },
                                { "Shift+C", "Cut every song at the playhead" },
                                { "Ctrl+D", "Copy the selected piece (right after it)" },
                                { "Ctrl + drag a piece", "Drag a copy of it" } } },
                { "View", { { "L", "Show / hide the library and mix overview" },
                            { "Mouse wheel", "Zoom in / out around the mouse" },
                            { "+  /  -", "Zoom in / out" },
                            { "Ctrl + mouse wheel", "Scroll along the timeline" } } },
                { "Song preview (while playing)", { { "Left / Right", "Jump 8 bars back / on" },
                                                    { "Esc", "Stop the preview" } } },
                { "Files", { { "Ctrl+N", "New project" },
                             { "Ctrl+O", "Open a project" },
                             { "Ctrl+S", "Save" },
                             { "Ctrl+Shift+S", "Save as" },
                             { "Ctrl+E", "Export the mix" } } },
                { "Edit", { { "Ctrl+Z", "Undo" },
                            { "Ctrl+Shift+Z  /  Ctrl+Y", "Redo" } } },
            };
        }

        static constexpr int columnWidth = 560, keyWidth = 220, padding = 28, rowHeight = 28, titleHeight = 38;

        static constexpr float textSize = 16.0f;   // user: the 13 px list was too small

        static int heightOf (const std::vector<Group>& groups)
        {
            int h = padding;

            for (auto& group : groups)
                h += titleHeight + (int) group.keys.size() * rowHeight + 10;

            return h;
        }

        static void paintColumn (juce::Graphics& g, const std::vector<Group>& groups, int x)
        {
            int y = padding;

            for (auto& group : groups)
            {
                g.setFont (theme::font (theme::textLarge, true));   // (bigger than the usual 11 px labels: user)
                g.setColour (theme::text3);
                g.drawText (group.title.toUpperCase(), x, y + 10, columnWidth, 20, juce::Justification::centredLeft);
                y += titleHeight;

                for (auto& [key, action] : group.keys)
                {
                    g.setFont (theme::font (textSize, true));
                    g.setColour (theme::text1);
                    g.drawFittedText (key, x, y, keyWidth - 8, rowHeight, juce::Justification::centredLeft, 1);
                    g.setFont (theme::font (textSize));
                    g.setColour (theme::text2);
                    g.drawFittedText (action, x + keyWidth, y, columnWidth - keyWidth, rowHeight, juce::Justification::centredLeft, 1);
                    y += rowHeight;
                }

                y += 10;
            }
        }
    };
}
