#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// Exporting the mix: render what you hear (mute/solo respected, master limiter included, no
// metronome) to a 32-bit float WAV at 44.1 kHz, measure its peak, then encode to MP3 (320 kbps
// CBR) or FLAC (16-bit, dithered) with an optional gain. Normalising only ever raises a mix: the
// limiter already keeps peaks under its ceiling, and turning a whole mix down to fit one peak is
// what made the first exports far too quiet.
namespace mixtopia::exporter
{
    enum class Format { mp3, flac };

    juce::String getExtension (Format);

    // Where normalising puts the loudest sample. MP3 decoding overshoots peaks slightly, so it
    // gets more room than FLAC.
    float getTargetPeakDb (Format);

    // The gain to apply for a measured peak: raises a quiet mix to the target, never lowers.
    float getNormalisingGain (Format, float peak);

    // Renders the audible mix to a temporary float WAV. Shows Tracktion's progress (via the
    // UIBehaviour) and can be cancelled. Returns {} on failure/cancel and sets `error`.
    juce::File renderMix (te::Edit&, juce::String& error);

    // The highest absolute sample value in a WAV file (1.0 = full scale).
    float measurePeak (const juce::File& wav);

    // The integrated loudness (LUFS, as for songs: Loudness.h) of a WAV file.
    double measureLoudness (const juce::File& wav, const std::function<bool()>& shouldStop = [] { return false; });

    // The tags typed in the Export window (remembered between exports). The title is always the
    // export's file name and the track number always 0 (user, 2026-10-27: a tag editor adding the
    // album dropped the 0, so mixes sorted to the top in a music player). Empty fields aren't written.
    struct Tags
    {
        juce::String artist, album, genre, year;   // the artist is also written as the album artist
        juce::File artwork;                        // front cover (JPG or PNG); none if empty
    };

    // Why an image can't be the cover (missing, not a JPG/PNG, over 16 MB - FLAC's limit), or {}.
    juce::String checkArtwork (const juce::File&);

    // Encodes the rendered WAV to `destination` with the given linear gain and tags. Writes via a
    // temporary file, so cancelling or failing leaves no half-written export. Returns an error or {}.
    juce::String encode (const juce::File& wav, Format, float gain, const juce::File& destination, const Tags& = {},
                         std::atomic<float>* progress = nullptr, const std::function<bool()>& shouldStop = {});

    // (Re)writes the tags of a finished MP3 / FLAC file as encode does (no cover image) - for the self-tests.
    juce::String writeTags (const juce::File& file, Format, const juce::String& title, const Tags& = {});
}
