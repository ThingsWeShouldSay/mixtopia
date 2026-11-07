#pragma once

#include <JuceHeader.h>
#include "Loudness.h"

namespace te = tracktion;

// Automatic level matching (M5c): songs quieter than loudness::referenceLufs are raised to it (at
// most loudness::maxRaiseDb), never lowered. Applied as a fixed gain by the clip mixer, on top of
// the song's volume line. Can be switched off per song and per project.
//
// New projects have it on; projects saved before it existed have it off, so they sound exactly as
// they were saved until it's switched on (Options menu).
namespace mixtopia::levelmatch
{
    // Clip: the song's integrated loudness (LUFS; missing = not measured yet) and whether matching
    // is switched off for it.
    inline const juce::Identifier loudnessProperty { "mixtopiaLoudness" };
    inline const juce::Identifier matchOffProperty { "mixtopiaMatchOff" };

    // Edit: matching on (1) or off (0) for the whole project.
    inline const juce::Identifier projectMatchProperty { "mixtopiaLevelMatch" };

    inline bool isOnForProject (const te::Edit& edit)    { return (int) edit.state.getProperty (projectMatchProperty, 0) != 0; }
    inline bool isOnForClip (const te::AudioClipBase& clip)   { return ! (bool) clip.state.getProperty (matchOffProperty, false); }

    inline std::optional<double> getLoudness (const te::AudioClipBase& clip)
    {
        if (! clip.state.hasProperty (loudnessProperty))
            return {};

        return (double) clip.state[loudnessProperty];
    }

    // What the song would be raised by if matching applies to it (whatever the switches say).
    inline double getPotentialGainDb (const te::AudioClipBase& clip)
    {
        auto lufs = getLoudness (clip);
        return lufs ? loudness::matchGainDb (*lufs) : 0.0;
    }

    // The gain actually applied now (0 when switched off or not measured).
    inline double getAppliedGainDb (const te::AudioClipBase& clip)
    {
        return isOnForProject (clip.edit) && isOnForClip (clip) ? getPotentialGainDb (clip) : 0.0;
    }
}
