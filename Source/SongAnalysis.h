#pragma once

#include <JuceHeader.h>
#include "BeatGrid.h"

namespace te = tracktion;

// What Mixtopia works out about a song by itself: its beat grid and how much silence it starts
// with (later also its key and loudness). Worked out once per song and cached in
// ~/.cache/Mixtopia/analysis (a few lines of text), keyed like the decoded copies by the played
// file's path, size and modification time.
namespace mixtopia::analysis
{
    struct SongInfo
    {
        beatgrid::Result grid;

        // Seconds into the file where sound starts (louder than -60 dBFS), and how far the start
        // may be trimmed: 20 ms before that, but never past the beat line where the sound begins
        // (so a fade-in that starts quieter than -60 dB keeps its beginning).
        double soundStart = 0.0, silentUntil = 0.0;

        juce::String key;   // Camelot ("8A"), {} if none found (KeyDetection.h)
        double loudness = -std::numeric_limits<double>::infinity();   // integrated LUFS (Loudness.h)
    };

    // Auto-trim: new songs start where their sound starts (Options menu; on by default).
    bool isAutoTrimEnabled();
    void setAutoTrimEnabled (bool);

    // The cached analysis of a playable file (the decoded copy for MP3/FLAC), if there is one.
    std::optional<SongInfo> getCached (const juce::File& playable);

    // Analyses a song (no cache): beat grid, sound start, how far it may be trimmed.
    SongInfo analyseReader (juce::AudioFormatReader&, const std::function<bool()>& shouldStop = [] { return false; });

    // Reads the cache, or analyses now and caches the result (slow: background thread).
    std::optional<SongInfo> analyse (te::Engine&, const juce::File& playable, const std::function<bool()>& shouldStop = {});

    // Makes songs ready to add: decodes MP3/FLAC that aren't cached yet and analyses songs that
    // haven't been, with a progress window and Cancel button. Returns false if cancelled.
    bool prepareWithProgressWindow (te::Engine&, const juce::Array<juce::File>& songs);

    // Gives a clip the detected BPM and beat 1 (one undoable step with whatever the caller started),
    // placing it so its bar starts land on the project's bars (snap) or keeping it where it is.
    void applyDetectedGrid (te::AudioClipBase&, const SongInfo&, bool snapToBars);

    // Sets up a song just added to the timeline at `dropTime`: detected grid, beat 1 on the bar the
    // drop point is in (with snap; without, the song starts at the drop point), the silence at its
    // start trimmed (if auto-trim is on), kept on the grid at the start of the mix. Returns true if
    // it had to move a bar later to keep a sound before beat 1.
    bool prepareNewClip (te::AudioClipBase&, const SongInfo&, te::TimePosition dropTime, bool snapToBars, bool autoTrim);

    // Clip properties (source seconds): where the song's sound starts, and how much the auto-trim
    // cut (for "Restore the start").
    inline const juce::Identifier soundStartProperty { "mixtopiaSoundStart" };
    inline const juce::Identifier autoTrimProperty { "mixtopiaAutoTrim" };

    // The song's key (Camelot) and whether it was "detected" or "set by hand".
    inline const juce::Identifier keyProperty { "mixtopiaKey" };
    inline const juce::Identifier keySourceProperty { "mixtopiaKeySource" };
    void setKey (te::AudioClipBase&, const juce::String& camelot, bool byHand);

    // Stores the song's measured loudness on the clip (for level matching), if it has one.
    void setLoudness (te::AudioClipBase&, const SongInfo&);

    // Measures (with a progress window) the songs in an Edit that have no loudness yet - clips
    // from projects saved before level matching existed - and stores it on them.
    void measureMissingLoudness (te::Edit&);

    // How far a clip's start may be cut to keep beat 1 on bar 1 at 0:00: up to where its sound
    // starts (5 ms of tolerance: the -60 dB crossing and the kick's start differ by a few ms).
    // Clips from before this was stored: from the cached analysis, else by reading the file's start
    // (call once per gesture, not per mouse move).
    double getMayCutUntil (const te::AudioClipBase&);

    // "Use the detected BPM and beat 1" for a clip already on the timeline: like applyDetectedGrid,
    // but keeping its trim and never cutting sound at 0:00.
    void reapplyDetectedGrid (te::AudioClipBase&, const SongInfo&, bool snapToBars);

    // Undoes the auto-trim of a clip (as far as 0:00 allows). False if there's nothing to restore.
    bool restoreTrimmedStart (te::AudioClipBase&);
}
