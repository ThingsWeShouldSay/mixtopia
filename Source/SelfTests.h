#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

namespace mixtopia
{
    // `Mixtopia --self-test <audio file>` (use build/test-audio/test.mp3): prints a PASS / FAIL line
    // per check; true if all passed.
    bool selfTest (te::Engine&, const juce::File& audioFile);
}
