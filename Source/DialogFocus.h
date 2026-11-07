#pragma once

#include <JuceHeader.h>

// Puts the cursor in a dialog's text box (its text selected) so the user can type straight away.
// On Linux the window manager hands the keyboard to a new dialog a moment after it appears, and a
// grab before that is lost (user, 2026-10-27: still had to click the box). So this keeps trying
// every 50 ms until the box has kept the keyboard for half a second while its window is focused (up
// to 2 s). Checked with `--open --tempo-dialog`: the box had it 50 ms after the dialog appeared.
namespace mixtopia
{
    class FocusWhenShown : private juce::Timer
    {
    public:
        static void start (juce::TextEditor& box)   { new FocusWhenShown (box); }   // deletes itself

    private:
        explicit FocusWhenShown (juce::TextEditor& box) : target (&box)   { startTimer (50); }

        void timerCallback() override
        {
            auto* peer = target != nullptr ? target->getPeer() : nullptr;

            if (target == nullptr || ++ticks > 40)
            {
                delete this;
                return;
            }

            if (peer == nullptr || ! target->isShowing())
                return;

            if (peer->isFocused() && target->hasKeyboardFocus (false))
            {
                if (++settled >= 10)   // kept it for half a second: done
                    delete this;

                return;
            }

            settled = 0;
            target->grabKeyboardFocus();

            if (! selected)
            {
                target->selectAll();
                selected = true;
            }
        }

        juce::Component::SafePointer<juce::TextEditor> target;
        int ticks = 0, settled = 0;
        bool selected = false;
    };
}
