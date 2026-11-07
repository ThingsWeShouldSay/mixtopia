#pragma once

#include <JuceHeader.h>

// A song's musical key, shown in Camelot notation (8A = A minor, 8B = C major, ...): numbers go
// round the circle of fifths, A = minor, B = major. Display only - no key matching.
//
// Detected with libKeyFinder (Mixxx's detector, GPLv3): on the user's 26 songs it agreed with Mixed
// In Key 10 on 25 (bake-off 2026-10-20, build/keybench: madmom's CNN also 25/26, Essentia edma/bgate
// 23/26). The song is fed in chunks, so it never has to be held in memory whole.
namespace mixtopia::key
{
    // "8A" etc., or {} if no key could be found (silence, or cancelled).
    juce::String analyse (juce::AudioFormatReader&, const std::function<bool()>& shouldStop = [] { return false; });

    // All 24 Camelot keys in order: 1A, 1B, 2A, 2B, ... 12B.
    juce::StringArray allCamelotKeys();

    // "8A" -> "A minor", "8B" -> "C major"; {} for anything else.
    juce::String toMusicalName (const juce::String& camelot);
}
