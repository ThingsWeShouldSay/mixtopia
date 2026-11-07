#pragma once

#include <JuceHeader.h>

// A song's title and artist for the library: from its tags (MP3: ID3v2 or ID3v1, read with
// libmpg123; FLAC: Vorbis comments), else from the file name ("01. Artist - Title").
namespace mixtopia::tags
{
    struct Tags
    {
        juce::String title, artist;
    };

    // Reads the file's tags; missing parts come from the file name. (Reads the whole MP3 to find
    // its tags: call on a background thread.)
    Tags read (const juce::File&);

    // "02. NØRTH SHORE - Bright Lights (Remix)" -> artist "NØRTH SHORE", title "Bright
    // Lights (Remix)"; without " - " the whole name (minus a leading track number) is the title.
    Tags fromFileName (const juce::String& nameWithoutExtension);
}
