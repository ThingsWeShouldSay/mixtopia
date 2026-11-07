#pragma once

#include <JuceHeader.h>
#include "ClipEnvelopes.h"
#include "ClipTempo.h"

namespace te = tracktion;

// Repeat part of a song (user, 2026-10-22, before M9): a range of the song plays again a number of
// times, then the song carries on from the end of the range exactly where it would have without the
// loop. The song gets longer; nothing else moves unless asked.
//
// How it plays: the clip plays a ".mixloop" file - a few lines of text in ~/.cache/Mixtopia/loops
// naming the song's decoded WAV, the range (in samples), the repeats and the crossfade. Reading it
// gives the song up to the range's end, the range again for each repeat, then the rest, straight
// from the decoded WAV (no copy is written). To Tracktion it's just a longer song, so the stretcher
// runs straight through the joins, tempo changes and ramps behave as for any song, and playback and
// export read the same samples. Tracktion reads it through Mixtopia's direct reader
// (patches/tracktion-direct-reader.patch), not its background-buffered path, which can return
// silence for a moment after Play or a jump.
//
// Each join gets a short crossfade (default 4 ms, 0-20 ms) placed just BEFORE the join: the end of
// the pass fades out while the audio leading up to the loop start fades in, so the first hit at the
// loop start is never softened. After the last repeat there is no join: the song simply continues.
//
// Times: the loop's start and end are SONG times (seconds into the song, whole samples). Everything
// else stored on the clip - its offset and end, beat 1, the lines' points - stays in PLAYED time
// (seconds into the longer, looped song), as Tracktion counts it. Adding, changing or removing a loop
// re-maps those (transform): before the loop nothing changes, after it everything moves by the
// change in added length, so a fade at the song's end still ends it.
//
// Lines inside the loop (per loop): "only the first time" (default) - the lines are what's stored,
// so between a point inside the loop and the next point after it the line runs across all the
// repeats; "copy into every repeat" - every pass reproduces the first pass's line exactly (computed
// when read, so editing the first pass changes every repeat). Points stored inside the repeats are
// ignored while copying (and come back when switched back), never deleted.
//
// Stored as a LIST (MIXTOPIA_LOOPS > LOOP) so two loops per song can come later without changing
// the project format; one loop per song for now (user).
namespace mixtopia::loops
{
    inline const juce::Identifier listType { "MIXTOPIA_LOOPS" }, loopType { "LOOP" },
                                  startProperty { "start" }, endProperty { "end" }, repeatsProperty { "repeats" },
                                  crossfadeProperty { "crossfadeMs" }, linesProperty { "lines" };
    inline const char* fileExtension = ".mixloop";

    constexpr double defaultCrossfadeMs = 4.0, maxCrossfadeMs = 20.0;
    constexpr int maxRepeats = 64;

    struct Loop
    {
        double start = 0.0, end = 0.0;    // song seconds
        int repeats = 1;
        double crossfadeMs = defaultCrossfadeMs;
        bool copyLines = false;           // "copy into every repeat"

        double length() const   { return end - start; }
        double added() const    { return repeats * length(); }
        bool operator== (const Loop&) const = default;
    };

    std::optional<Loop> getLoop (const juce::ValueTree& clipState);
    void setLoop (juce::ValueTree clipState, const std::optional<Loop>&, juce::UndoManager*);

    // Played time -> where in the song it is: pass 0 = the song itself (before and after the
    // repeats, and the loop's first pass), pass 1..repeats = a repeat.
    struct Place { int pass = 0; double song = 0.0; };
    Place playedToPlace (const std::optional<Loop>&, double played);
    inline double playedToSong (const std::optional<Loop>& loop, double played)   { return playedToPlace (loop, played).song; }

    // Song time -> played time (pass 0: after the repeats for times past the loop's end).
    double songToPlayed (const std::optional<Loop>&, double song);
    double placeToPlayed (const std::optional<Loop>&, Place);

    // A played time under one loop setting -> the same moment of the music under another.
    double transform (const std::optional<Loop>& from, const std::optional<Loop>& to, double played);

    // The points a line really follows (with "copy into every repeat" the repeats' copies).
    std::vector<envelopes::Point> effectivePoints (const std::vector<envelopes::Point>& stored, const std::optional<Loop>&);
    std::vector<envelopes::Point> getEffectivePoints (const juce::ValueTree& clipState, envelopes::Param);

    // The value a new point added at played time t starts with: what its own line already has
    // there (user, 2026-10-27), so adding a point never changes the sound.
    float newPointValue (const std::vector<envelopes::Point>& stored, const std::optional<Loop>&, double t);

    //==============================================================================
    // The .mixloop file for a decoded WAV + loop (created if needed; named after its contents).
    juce::File getLoopFile (const juce::File& wav, const Loop&);
    bool isLoopFile (const juce::File&);

    // Puts the song's sample rate on the loop's start and end (whole samples).
    Loop snappedToSamples (Loop, double sampleRate);

    // Registers the .mixloop format and the direct reader with the engine (once, at start-up).
    void install (te::Engine&);

    //==============================================================================
    // On a clip.
    std::optional<Loop> getLoop (const te::AudioClipBase&);

    // The song's own audio file (the decoded WAV), even while the clip plays a .mixloop.
    juce::File getSongFile (const te::AudioClipBase&);
    double getSampleRate (const te::AudioClipBase&);

    struct Result
    {
        juce::String madeRoom;           // "Made room: the songs after the loop moved later by 8 bars."
        juce::String recoloured;         // the neighbour colour rule's message, if the song changed colour
    };

    // Sets, changes or removes (nullopt) the clip's loop as part of the current undo transaction:
    // re-maps the clip's offset/end, beat 1 and line points, switches the file it plays, and moves
    // every song that starts after the loop's end, and the tempo points there, by the change in
    // added length - with `moveLater`, or by itself when the song would run into the next song on
    // its lane (songs never overlap on a lane: user, 2026-10-23).
    Result apply (te::AudioClipBase&, const std::optional<Loop>&, bool moveLater);

    // A quick loop (step 2): `bars` bars from `songStart`, exact for the song's tempo, 1 repeat;
    // nullopt (and `problem` says why) if the song ends before that.
    std::optional<Loop> makeQuick (te::AudioClipBase&, double songStart, int bars, juce::String& problem);

    // A loop point snapped to the song's own grid: the nearest bar or beat line (unchanged with
    // Snap::none or without a beat grid). snapFor = how loop points snap, like dragging a song:
    // Snap on = bars, Shift = beats; Snap off = free, Shift = bars.
    double snapped (te::AudioClipBase&, double songSeconds, tempo::Snap);
    inline tempo::Snap snapFor (bool snapOn, bool shiftDown)
    {
        return snapOn ? (shiftDown ? tempo::Snap::beats : tempo::Snap::bars) : (shiftDown ? tempo::Snap::bars : tempo::Snap::none);
    }
    inline double nearestBeat (te::AudioClipBase& clip, double songSeconds)    { return snapped (clip, songSeconds, tempo::Snap::beats); }

    // A loop to start from where the user right-clicked (song seconds): 8 bars (with a beat grid;
    // 15 s without), starting on the song's bar at or before it when `snap`.
    Loop makeDefault (te::AudioClipBase&, double songSeconds, bool snap);

    // Text for the clip bar and title: "bar 97–105  ×3  +24 bars" (seconds without a beat grid).
    juce::String describe (te::AudioClipBase&, const Loop&);
    juce::String describeAdded (te::AudioClipBase&, const Loop&);   // "+24 bars  ·  +41.7 s"
    juce::String songBarText (te::AudioClipBase&, double songSeconds);   // "song bar 97.1" or ""
    juce::String shortTitleOf (const te::AudioClipBase&);                // "Bright Lights"
    double lengthInBeats (te::AudioClipBase&, const Loop&);   // 0 without a beat grid
    juce::String formatSongTime (double seconds);              // "3:21.418"
    std::optional<double> parseSongTime (te::AudioClipBase&, const juce::String&);   // "3:21.418", "201.4", "bar 97.1"

    //==============================================================================
    // The sample-level reading, shared by the .mixloop format (analysis, info) and the direct
    // reader (playback and export). Positions in samples.
    struct Spec
    {
        juce::File wav;
        juce::int64 start = 0, end = 0;
        int repeats = 0;
        int crossfade = 0;     // samples

        juce::int64 length() const      { return end - start; }
        bool parse (const juce::File& loopFile);
        bool parseText (const juce::String&);
    };

    // Reads played samples [t0, t0 + n) into `dest` (float channels; null ones skipped). `fetch`
    // (float* const* dest, int numChannels, juce::int64 sourceStart, int count) reads the WAV;
    // `scratch` holds `scratchSize` samples per channel for the crossfades.
    template <typename Fetch>
    bool readLooped (const Spec& spec, juce::int64 sourceLength, float* const* dest, int numChannels, juce::int64 t0, int n,
                     float* const* scratch, int scratchSize, Fetch&& fetch)
    {
        auto loopLength = spec.length();
        auto total = sourceLength + loopLength * spec.repeats;
        bool ok = true;

        float* d[16] = {};
        auto channels = juce::jmin (numChannels, 16);

        // 1. Each output sample from where it plays in the song.
        for (int done = 0; done < n;)
        {
            auto t = t0 + done;
            juce::int64 sourcePos = 0, run = 0;

            if (t < 0 || t >= total)
            {
                run = t < 0 ? -t : (juce::int64) (n - done);
                run = juce::jmin (run, (juce::int64) (n - done));

                for (int c = 0; c < channels; ++c)
                    if (dest[c] != nullptr)
                        juce::FloatVectorOperations::clear (dest[c] + done, (int) run);

                done += (int) run;
                continue;
            }

            if (t < spec.end)                                   { sourcePos = t; run = spec.end - t; }
            else if (t < spec.end + loopLength * spec.repeats)  { auto into = (t - spec.end) % loopLength; sourcePos = spec.start + into; run = loopLength - into; }
            else                                                { sourcePos = t - loopLength * spec.repeats; run = total - t; }

            auto count = (int) juce::jmin (run, (juce::int64) (n - done));

            for (int c = 0; c < channels; ++c)
                d[c] = dest[c] != nullptr ? dest[c] + done : nullptr;

            ok = fetch (d, channels, sourcePos, count) && ok;
            done += count;
        }

        // 2. The crossfades: in the last `fade` samples before each join, the pass fades out while
        //    the audio leading up to the loop start fades in.
        auto fade = (int) juce::jmin ((juce::int64) spec.crossfade, loopLength, spec.start, (juce::int64) scratchSize);

        if (fade <= 0 || spec.repeats <= 0 || n <= 0)
            return ok;

        auto firstJoin = juce::jmax ((juce::int64) 0, (t0 - spec.end) / loopLength);
        auto lastJoin = juce::jmin ((juce::int64) spec.repeats - 1, (t0 + n + fade - spec.end) / loopLength + 1);

        for (auto k = firstJoin; k <= lastJoin; ++k)
        {
            auto join = spec.end + k * loopLength;
            auto from = juce::jmax (t0, join - fade), to = juce::jmin (t0 + n, join);

            if (from >= to)
                continue;

            auto count = (int) (to - from);
            ok = fetch (scratch, channels, spec.start - (join - from), count) && ok;

            for (int c = 0; c < channels; ++c)
            {
                if (dest[c] == nullptr)
                    continue;

                auto* out = dest[c] + (from - t0);

                for (int i = 0; i < count; ++i)
                {
                    auto step = (int) (from + i - (join - fade));
                    auto g = (float) (step + 1) / (float) (fade + 1);
                    out[i] = out[i] * (1.0f - g) + scratch[c][i] * g;
                }
            }
        }

        return ok;
    }
}
