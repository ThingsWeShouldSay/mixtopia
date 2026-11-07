#pragma once

#include <JuceHeader.h>

namespace mixtopia
{
    // A short song title for tight spaces (meter bars, the mix overview):
    // "02. NØRTH SHORE - Bright Lights (Example Extended Remix)" -> "Bright Lights".
    inline juce::String shortTitle (const juce::String& clipName)
    {
        auto name = clipName.trim();

        while (name.isNotEmpty() && (juce::CharacterFunctions::isDigit (name[0]) || name[0] == '.' || name[0] == ' '))
            name = name.substring (1);

        if (name.contains (" - "))
            name = name.fromFirstOccurrenceOf (" - ", false, false);

        for (auto cut : { " - ", " (", " [" })
            name = name.upToFirstOccurrenceOf (cut, false, false);

        name = name.trim();

        while (name.isNotEmpty() && ! juce::CharacterFunctions::isLetterOrDigit (name.getLastCharacter()))
            name = name.dropLastCharacters (1);

        return name.isNotEmpty() ? name : clipName;
    }
}
