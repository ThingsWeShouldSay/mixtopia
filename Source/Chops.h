#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// Chopping (M11, 2026-10-27; plan approved by the user, mockup 24): cut a song into pieces and
// rearrange them.
//
// A CUT doesn't split anything: it's a mark on the song (MIXTOPIA_CUTS > CUT t=<played seconds>, the
// clip's own file time - a looped song's repeats included). Pieces that still touch and follow on in
// the song are ONE clip underneath, so they play bit-for-bit like the uncut song. A piece becomes a
// clip of its own when it's moved, deleted, copied or dragged to another lane (separate()): a copy of
// the song's clip showing only its part. Put back exactly where it was, it joins up again
// (joinTouching()). Edges that came from a cut and no longer follow on get a 4 ms fade on each side
// (mixtopiaFadeIn / mixtopiaFadeOut, applied by the ClipMixer) - the song's own start and end never do.
//
// Every piece keeps a copy of the song's lines (in song time), so pulling a piece's edge out later
// plays the song's own lines there; the BPM, beat 1, key and loudness are the song's.
//
// Labels: songs are numbered in time order (by their first piece); a chopped song's pieces get a
// letter by their place in the ORIGINAL song (so a label stays with its piece when it's moved), and
// copies of the same part are told apart by a count in time order: 3c, 3c·2, 3c·3.
namespace mixtopia::chops
{
    inline const juce::Identifier cutsType { "MIXTOPIA_CUTS" };
    inline const juce::Identifier cutType { "CUT" };
    inline const juce::Identifier timeProperty { "t" };
    inline const juce::Identifier fadeInProperty { "mixtopiaFadeIn" };
    inline const juce::Identifier fadeOutProperty { "mixtopiaFadeOut" };
    inline const juce::Identifier leadInProperty { "mixtopiaLeadIn" };

    constexpr double fadeSeconds = 0.004;        // each side of an edge that no longer follows on
    constexpr double minPieceSeconds = 0.05;     // a cut this close to an edge or another cut is refused

    // A piece: a part of the clip's file, in played seconds (the clip's own file time).
    struct Piece
    {
        double start = 0.0, end = 0.0;
    };

    // The cuts inside the part of the song the clip plays, in order.
    std::vector<double> getCuts (const te::AudioClipBase&);
    void setCuts (te::AudioClipBase&, std::vector<double> cuts, juce::UndoManager*);   // keeps only those inside

    std::vector<Piece> getPieces (te::AudioClipBase&);
    int pieceAt (te::AudioClipBase&, double editSeconds);                    // -1 outside the clip
    juce::Range<double> getPieceEditRange (te::AudioClipBase&, int piece);  // edit seconds

    inline bool hasFadeIn (const te::AudioClipBase& c)   { return c.state.getProperty (fadeInProperty, false); }
    inline bool hasFadeOut (const te::AudioClipBase& c)  { return c.state.getProperty (fadeOutProperty, false); }

    // CROSSFADES (2026-10-28: a fade out to silence and back in left a near-silent 8 ms hole at a join -
    // the user heard a click). A piece that starts exactly where another clip ends (on any lane; on its
    // own lane pieces that follow on have joined instead) starts fadeSeconds EARLIER, with the song's own audio from just before
    // its cut, fading in while the other fades out - as a loop's joins do. Its clip really starts there
    // (a 4 ms overlap; the grid isn't affected: pieces are placed by their content start), flagged
    // mixtopiaLeadIn; where it's seen to start = visibleStart().
    inline bool hasLeadIn (const te::AudioClipBase& c)   { return c.state.getProperty (leadInProperty, false); }
    double visibleStart (const te::AudioClipBase&);
    void updateLeadIns (te::Edit&);

    // A piece of a chopped song (has cuts, or edges that came from cuts).
    bool isPiece (const te::AudioClipBase&);

    // Which song a clip plays (its original file's path): pieces and copies of one song share it.
    juce::String songKey (const te::AudioClipBase&);
    std::vector<te::AudioClipBase*> clipsOfSong (te::Edit&, const juce::String& key);

    // Adds a cut at a played time of the clip; false (nothing changed) if too close to an edge or a cut.
    bool cut (te::AudioClipBase&, double playedSeconds);

    // Makes the piece a clip of its own (the parts before and after stay where they are, as their own
    // clips). Returns it (the clip itself when it's already the only piece).
    te::AudioClipBase* separate (te::AudioClipBase&, int piece);

    // A copy of a piece, starting at `editStart` (snapped to keep it on the song's grid when the song
    // is tempo-matched: moved by whole beats from where it is) on `track`.
    te::AudioClipBase* copyPiece (te::AudioClipBase&, int piece, te::AudioTrack& track, double beatsLater);

    // Ctrl+D: a copy of the piece right after it - on the same lane if there's room, else on the next
    // lane with room at that time (a new lane if none). Nothing else moves. Returns the copy.
    te::AudioClipBase* duplicate (te::AudioClipBase&, int piece, juce::String& status);

    // Pieces of one song next to each other on a lane that follow on in the song and are alike in
    // everything else (lines, loop, settings) become one clip again (bit-for-bit as before they were
    // separated). Each merge: the clip that went, the one it joined (the one playing at the playhead
    // stays), where the removed one's pieces start in it, and how many pieces now come before the kept
    // clip's own.
    struct Merge { te::EditItemID removed, into; int piece = 0, prepended = 0; };
    std::vector<Merge> joinTouching (te::Edit&);

    // After any chopping edit: join what follows on, then set the crossfades.
    std::vector<Merge> tidy (te::Edit&);

    // Right-click "Join with the next piece": the next clip on the lane follows on in the song, but
    // differs (e.g. its lines were changed) - joined anyway, each part keeping its own lines.
    te::AudioClipBase* nextJoinable (te::AudioClipBase&);
    Merge joinWithNext (te::AudioClipBase&);

    // Labels for the timeline, the mix overview and the meter, in time order (one per piece).
    struct Labelled
    {
        te::AudioClipBase* clip = nullptr;
        int piece = 0;
        juce::Range<double> time;    // edit seconds
        int songNumber = 0;
        juce::String label;          // "3", or "3c", "3c·2" for a chopped song
    };
    std::vector<Labelled> listPieces (te::Edit&);
    juce::String labelFor (const std::vector<Labelled>&, const te::AudioClipBase&, int piece);
}
