#pragma once

#include <JuceHeader.h>
#include "ClipEnvelopes.h"
#include "LoudnessOverview.h"
#include "ClipTempo.h"
#include "TempoStrip.h"
#include "Library.h"
#include "Loops.h"
#include "Chops.h"

namespace te = tracktion;

// The multi-lane arrangement view: a bars/beats ruler on top, one lane per audio track, clips
// drawn with their waveforms and beat lines, and a playhead. Handles dropping files, moving and
// trimming clips (snapping to beats), lane and clip context menus, zooming, scrolling and seeking.
class TimelineComponent : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          public juce::DragAndDropTarget,
                          public juce::TextDragAndDropTarget,
                          private juce::ChangeListener,
                          private juce::ValueTree::Listener,
                          private juce::AsyncUpdater,
                          private juce::ScrollBar::Listener
{
public:
    explicit TimelineComponent (te::Edit&);
    ~TimelineComponent() override;

    void zoomBy (double factor);
    void zoomToFit();
    void addLane();
    void setVisibleRange (double startSeconds, double endSeconds);
    void selectFirstClipOnLane (int lane);
    void selectSongOfFirstClipOnLane (int lane);   // every piece of that song (developer option --select-song)
    void selectEveryPieceOfSelectedSong();          // Edit > Select every piece of this song
    void openClipMenuOnLane (int lane);             // developer option --clip-menu: the right-click menu over its first song

    // Selects a song, puts the playhead at its start and scrolls it into view.
    void showClip (te::EditItemID);

    // Adds files to consecutive lanes, starting at the given lane, all at the given time. They're
    // added to the song library too, and placed with its remembered corrections.
    void addFiles (const juce::StringArray& files, int firstLane, te::TimePosition start);

    // The song library (M7): songs added here go into it; BPM/beat 1/key corrections are remembered.
    void setLibrary (mixtopia::library::Library* l)     { library = l; }

    // Songs about to be added that are already in the mix (their first copies), while Mixtopia
    // asks whether to add them anyway (user's choice: a dialog).
    std::function<void (const std::vector<te::EditItemID>& firstCopies)> onDuplicatesFound;

    // Clips in start-time order (numbering, as in the mix overview).
    std::vector<te::AudioClipBase*> getClipsInStartOrder() const;

    // The time range shown, and scrolling to a time (the mix overview).
    juce::Range<double> getVisibleTimeRange() const     { return { viewStart, viewStart + getVisibleSeconds() }; }
    void scrollTo (double startSeconds)                 { setViewStart (startSeconds); }

    // Drags from the song library (library::dragDescription).
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails& d) override   { itemDragMove (d); }
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    // Envelope lines: all four are always shown. Points are added with the clip's right-click menu
    // or a key (V/B/G/T) at the mouse pointer; dragging a point moves it, dragging anywhere else
    // moves the clip. Adds a point to the clip under the mouse; false if there's none.
    bool addPointUnderMouse (mixtopia::envelopes::Param);

    // Follow: while playing, turn the page when the playhead reaches the edge. Off (the default),
    // the view stays where the user put it.
    void setFollowPlayhead (bool shouldFollow)   { followPlayhead = shouldFollow; }

    // The tempo strip under the ruler (View > Tempo Strip). The ruler's right-click "Change tempo
    // here..." asks for it to be shown (onTempoStripWanted) and then places a tempo point.
    void setTempoStripVisible (bool);
    bool isTempoStripVisible() const     { return tempoStrip.isVisible(); }
    std::function<void()> onTempoStripWanted;

    // Snap-to-beat for moving and trimming (holding Shift while dragging inverts it).
    void setSnapEnabled (bool shouldSnap)   { snapEnabled = shouldSnap; repaint(); }

    // Faint lines up through the songs at their bar starts when zoomed in (View > Bar Lines on
    // Songs, on by default): shows at a glance whether a kick sits on the song's own grid.
    void setBarLinesOnSongs (bool shouldShow)   { barLinesOnSongs = shouldShow; repaint(); }

    // The selected clip and actions on it (each is one undo step).
    te::WaveAudioClip* getSelectedClip() const;
    void deleteSelectedClip();

    // Chopping (Chops.h): C cuts the song at the playhead (the selected one if it's there, else the
    // top-most lane's), Shift+C every song there; Ctrl+D copies the selected piece.
    void cutKey (bool everySong);
    void duplicateSelected();
    void setSelectedClipBpm (double bpm);
    void clearSelectedClipBpm();
    void nudgeSelectedClipDownbeat (double seconds);
    void shiftSelectedClipBarStart (int beats);          // beat 1 a beat earlier (-1) or later (+1)
    bool resetSelectedClipToDetectedGrid();              // false if no beat grid was found
    void setSelectedClipKey (const juce::String& camelot);  // by hand
    void setSelectedClipMatching (bool shouldMatch);     // automatic level matching on/off for one song
    bool resetSelectedClipToDetectedKey();               // false if no key was found
    bool canSetDownbeatAtPlayhead() const;
    void setSelectedClipDownbeatAtPlayhead();

    std::function<void (const juce::String&)> onStatusMessage;
    std::function<void()> onSelectionChanged;

    // Repeat part of a song (right-click): gives the song a loop if it has none, then asks for the
    // loop panel. The playhead's position as the user sees it (for "Set at playhead").
    void repeatPartOf (te::WaveAudioClip&, int x);
    void repeatPartAtPlayhead (te::WaveAudioClip&);
    juce::Range<double> getLoopEditRange (te::AudioClipBase&);   // the loop and its repeats (edit seconds)
    std::function<void (te::EditItemID)> onLoopPanelWanted;

    // Keys I / O / E (step 2): act on the selected song if it's under the playhead, else on the song
    // under the playhead on the top-most lane that has one. Esc cancels a loop-in (true if one was
    // waiting). The song whose loop E opens (none: invalid).
    void loopInKey();
    void loopOutKey();
    bool cancelLoopIn();
    te::EditItemID getSongForLoopPanel();
    bool isSnapEnabled() const              { return snapEnabled; }

    // Self-tests: where a time on a lane is on screen (to drive the mouse handlers).
    juce::Point<int> pointFor (double seconds, int lane) const   { return { (int) std::round (timeToX (seconds)), getLaneBounds (lane).getCentreY() }; }

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragMove (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    // Some file managers (e.g. KDE under Wayland) offer dropped files as text first, which JUCE
    // then delivers as a text drop of file:// URIs.
    bool isInterestedInTextDrag (const juce::String&) override;
    void textDragEnter (const juce::String&, int, int y) override       { fileDragEnter ({}, 0, y); }
    void textDragMove (const juce::String&, int, int y) override        { fileDragMove ({}, 0, y); }
    void textDragExit (const juce::String&) override                    { fileDragExit ({}); }
    void textDropped (const juce::String&, int x, int y) override;

private:
    enum class DragMode { none, seek, move, trimStart, trimEnd, envelopePoint, loopStart, loopEnd, box };

    // The playhead: a line down through the lanes with a marker in the lower part of the ruler,
    // under the bar numbers (M8: it used to cover the number next to it).
    struct Playhead : public juce::Component
    {
        static constexpr int width = 13, markerHeight = 8;
        void paint (juce::Graphics&) override;
    };

    // View
    double timeToX (double seconds) const;
    double xToTime (double x) const;
    double getVisibleSeconds() const;
    double xForBeat (double beat) const;
    double getPixelsPerBeat() const;
    int getLaneHeight() const;
    int getLanesTop() const     { return rulerHeight + (tempoStrip.isVisible() ? TempoStrip::preferredHeight : 0); }
    void showRulerMenu (juce::Point<int>);
    juce::Rectangle<int> getLaneBounds (int lane) const;
    int laneAt (int y) const;
    int getNumLanes() const;
    juce::Rectangle<int> getClipBounds (te::Clip&) const;
    te::Clip* clipAt (juce::Point<int>) const;
    DragMode dragModeFor (te::Clip&, juce::Point<int>) const;
    void setViewStart (double seconds);
    void setZoom (double newPixelsPerSecond);                  // clamped to the allowed range
    void zoomAround (double anchorX, double factor);           // keeps the time at anchorX in place
    void updateScrollBar();
    void updatePlayhead();

    // Editing
    void selectClip (te::EditItemID, int piece = -1);
    double snapTime (double seconds) const;

    // How clips with a beat grid snap: Snap on = bar starts, holding Shift = beats instead; Snap
    // off = free, holding Shift = bar starts.
    mixtopia::tempo::Snap getClipSnap (bool shiftDown = false) const
    {
        using mixtopia::tempo::Snap;
        return snapEnabled ? (shiftDown ? Snap::beats : Snap::bars) : (shiftDown ? Snap::bars : Snap::none);
    }
    void showClipMenu (te::WaveAudioClip&, juce::Point<int> mousePosition, bool atThatPoint = false);

    // Chopping helpers: a cut at an edit time (snapped to the song's nearest beat tick with Snap on);
    // the clips of the song (BPM, beat 1, key and level matching are the song's: changing them on
    // one piece changes them on all - user, 2026-10-27).
    bool cutAt (te::AudioClipBase&, double editSeconds, bool snap);
    void cutAllAt (double editSeconds, bool snap);
    std::vector<te::AudioClipBase*> piecesOfSong (te::AudioClipBase&);
    void afterPiecesMoved();
    std::vector<mixtopia::chops::Labelled> pieceLabels;   // refreshed before painting the lanes

    // SEVERAL SELECTED (2026-10-28, user's 1B / 2B): Shift+click adds or removes a song or piece,
    // Shift+drag in empty lane space draws a box, right-click "Select every piece of this song".
    // Del deletes them all, Ctrl+D copies them as a group after themselves, dragging one moves them
    // all (same spacing, snapped by the same amount, lane changes shift them all). The first selected
    // stays the "selected clip" (clip bar). A piece is (clip, index); -1 = the whole clip.
    struct PieceRef
    {
        te::EditItemID clip;
        int piece = -1;
        bool operator== (const PieceRef&) const = default;
    };
    std::vector<PieceRef> extraSelection;
    std::vector<PieceRef> getSelection() const;
    bool isPieceSelected (const te::AudioClipBase&, int piece) const;
    void toggleSelected (PieceRef);
    void setSelection (const std::vector<PieceRef>&);
    void remapSelection (const std::vector<mixtopia::chops::Merge>&);

    // A selected piece pinned down by where it is (its lane and the middle of its time), so it can be
    // found again after other pieces were split off (clips and indices change, places don't).
    struct Spot { int lane = 0; double middle = 0.0; bool whole = false; };
    std::vector<Spot> selectedSpots();
    te::AudioClipBase* separateAt (const Spot&);
    void selectSongPieces (te::AudioClipBase&);

    // Dragging several: each one's place when the drag began.
    struct GroupMember { te::EditItemID clip; te::ClipPosition position; mixtopia::tempo::SourceRange keep; double contentStartBeat = 0.0, startBeat = 0.0; int lane = 0; };
    std::vector<GroupMember> dragGroup;
    bool dragIsGroup = false, dragShiftToggle = false;
    juce::Point<int> boxStart, boxEnd;
    void showLaneMenu (int lane);
    void removeLane (int lane);
    void reportMovedForPickup (te::AudioClipBase&);
    void markGridAdjusted (te::AudioClipBase&);
    bool isOnGrid (te::AudioClipBase&) const;

    // Lane headers (number, mute, solo)
    // (Quick loops, step 2 - user's idea: under M / S. They act on the song under the playhead on
    // that lane. Short lanes get one "LOOP" button with the same choices as a menu.)
    struct LaneControls
    {
        juce::TextButton mute { "M" }, solo { "S" };
        juce::TextButton bars[4] { juce::TextButton ("1"), juce::TextButton ("2"), juce::TextButton ("4"), juce::TextButton ("8") };
        juce::TextButton loopIn { "In" }, loopOut { "Out" }, loopMenu { "LOOP" };
    };
    // 16 dropped (user, 2026-10-27) for a tidy 2 x 2 grid; 16 bars = an 8-bar loop + its end handle.
    static constexpr int quickLoopBars[4] { 1, 2, 4, 8 };
    static constexpr int numQuickLoops = (int) std::size (quickLoopBars);
    static constexpr int fullLoopToolsHeight = 150, loopMenuHeight = 82;   // lane heights that fit them

    // Quick loops and Loop in / out (step 2).
    te::WaveAudioClip* clipUnderPlayhead (int lane);
    void quickLoop (int lane, int bars);
    void loopIn (int lane);
    void loopOut (int lane);
    void showLoopMenu (int lane);
    void reportLoopSet (te::WaveAudioClip&, const mixtopia::loops::Result&, bool replaced);
    struct PendingLoopIn { te::EditItemID clip; int lane = 0; double song = 0.0; };
    std::optional<PendingLoopIn> pendingLoopIn;

    // The loop's edge handles (step 2): which one is at a point (true = start), and the readout.
    std::optional<bool> loopHandleAt (te::WaveAudioClip&, juce::Point<int>) const;
    te::EditItemID hoverHandleClip;
    std::optional<bool> hoverHandleIsStart;

    void updateLaneControls();
    void paintLaneHeaders (juce::Graphics&);

    // Painting
    void paintRuler (juce::Graphics&);
    void paintGrid (juce::Graphics&);
    void paintLanes (juce::Graphics&);
    void paintClip (juce::Graphics&, te::WaveAudioClip&, juce::Rectangle<int>, juce::Colour);
    void paintLoudnessShape (juce::Graphics&, const mixtopia::LoudnessOverview&, juce::Rectangle<int> area,
                             const std::function<double (int)>& toSourceTime, juce::Colour, const juce::File& songFile);
    void paintClipBeats (juce::Graphics&, te::WaveAudioClip&, juce::Rectangle<int>);
    void paintEnvelopes (juce::Graphics&, te::WaveAudioClip&, juce::Rectangle<int>);

    // Envelope lines: drawn in the clip below its title strip. The vertical scale is
    // sqrt(amplitude) so fades look natural: top = +6 dB, ~30% down = 0 dB, bottom = kill.
    static juce::Rectangle<int> getEnvelopeArea (juce::Rectangle<int> clipBounds);
    // A line's value <-> height: dB on a sqrt(amplitude) scale (0 dB near the top), the filter's
    // position straight with "off" in the middle (high-pass up, low-pass down).
    static float valueToY (mixtopia::envelopes::Param, float value, juce::Rectangle<int> area);
    static float yToValue (mixtopia::envelopes::Param, float y, juce::Rectangle<int> area);
    struct PointHit
    {
        mixtopia::envelopes::Param param;
        int index;
        bool operator== (const PointHit&) const = default;
    };

    static bool isHiddenByLoop (te::AudioClipBase&, double playedTime);
    void paintLoop (juce::Graphics&, te::WaveAudioClip&, juce::Rectangle<int> bounds, juce::Colour);

    // The point nearest the mouse (within a few pixels), on any line.
    std::optional<PointHit> pointAt (te::WaveAudioClip&, juce::Point<int>) const;
    void addPoint (te::WaveAudioClip&, mixtopia::envelopes::Param, juce::Point<int>);
    void setHover (te::EditItemID, std::optional<PointHit>);
    void clearLines (te::WaveAudioClip&, std::optional<mixtopia::envelopes::Param> onlyThis);

    // After a trim (or a move that trims at 0:00): points left in a trimmed-off part are pushed to
    // the new edge, keeping their level (user's choice "a": a dot always stays on the song). Of
    // several past the same edge, the one nearest the song is kept.
    void keepPointsInside (te::AudioClipBase&);
    juce::AudioThumbnail& getThumbnail (const juce::File& sourceFile);
    juce::AudioFormatReader* getSampleReader (const juce::File& songFile);   // the decoded WAV, memory-mapped (close zoom)
    void changeListenerCallback (juce::ChangeBroadcaster*) override     { repaint(); }

    // The transport updates its state while playing; that doesn't change the arrangement.
    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier&) override { if (! v.hasType (te::IDs::TRANSPORT)) triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    // Runs on every screen refresh: moves the playhead (and turns the page when following).
    void onFrame();

    // Where the playhead should be drawn: what's being heard now. While playing, the audio
    // engine's position only moves once per audio chunk (10-40 ms), so between those updates the
    // clock carries it on, gently corrected towards the engine; seeks and starts jump at once.
    double getPlayheadSeconds();
    double getOutputLatencySeconds() const;

    te::Edit& edit;
    mixtopia::library::Library* library = nullptr;
    void rememberGrid (te::AudioClipBase&);
    juce::String getSourcePath (te::AudioClipBase&) const;

    double viewStart = 0.0;         // seconds at the left edge
    double pixelsPerSecond = 10.0;
    bool snapEnabled = true;
    bool followPlayhead = false;
    bool barLinesOnSongs = true;

    // Playhead clock (see getPlayheadSeconds)
    bool playheadClockRunning = false;
    double shownPlayheadSeconds = 0.0, lastReportedSeconds = -1.0, lastReportMs = 0.0, lastFrameMs = 0.0;

    juce::ScrollBar scrollBar { false };
    TempoStrip tempoStrip { edit, [this] (double s) { return timeToX (s); }, [this] (double x) { return xToTime (x); }, headerWidth };
    std::vector<std::unique_ptr<LaneControls>> laneControls;
    Playhead playhead;

    // Waveforms are drawn from the original files (one per file, shared by its clips) with
    // juce::AudioThumbnail, which keeps them steady while clips are edited.
    // MixMeister-style loudness shapes (LoudnessOverview.h); the thumbnail shows until it's ready.
    mixtopia::LoudnessOverviews overviews { edit.engine.getAudioFileFormatManager().readFormatManager };
    juce::AudioThumbnailCache thumbnailCache { 64 };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails;
    std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> sampleReaders;
    te::EditItemID selectedClipID;
    int selectedPiece = -1;

    // Current mouse gesture
    DragMode dragMode = DragMode::none;
    te::EditItemID dragClipID;
    te::ClipPosition dragStartPosition;
    mixtopia::tempo::SourceRange dragKeep;          // the part of the song the clip played when the drag began
    double dragContentStartBeat = 0.0;    // tempo-matched clips: where the song's start sat (beats)
    bool dragSnaps = true;
    int dragPointIndex = -1;
    bool dragReportedPickup = false;
    double dragMayCutUntil = 0.0;      // source seconds: silence that may be cut to keep beat 1 on bar 1
    mixtopia::envelopes::Param dragPointParam = mixtopia::envelopes::Param::volume;
    float dragPointStartDb = 0.0f;
    double dragPointStartSource = 0.0;
    int dragStartX = 0, dragStartY = 0;
    int dragPiece = -1;           // chopping: the piece grabbed (separated, or copied with Ctrl, once the drag starts)
    bool dragCopies = false, dragPrepared = false;

    // The point under the mouse (highlighted, with its value shown).
    te::EditItemID hoverClipID;
    std::optional<PointHit> hoverPoint;

    int dropHighlightLane = -1;

    juce::VBlankAttachment frameUpdates { this, [this] { onFrame(); } };

    static constexpr int headerWidth = 76;
    static constexpr int rulerHeight = 28;
    static constexpr int clipTitleHeight = 20;
    static constexpr int beatRailHeight = 10;     // the strip along a song's bottom with its beat ticks
    // The faint bar lines on songs: tuned on screenshots of all six song colours (2026-10-22) -
    // 0.3/0.22 vanished on yellow and lime, 0.75/0.5 was heavy; 0.6/0.4 shows on bright and dark parts.
    static constexpr float barLineLightAlpha = 0.4f, barLineDarkAlpha = 0.6f;
    static constexpr int scrollBarHeight = 14;
    static constexpr int edgeGrabWidth = 8;
    static constexpr int pointDeadzone = 8;       // pixels a point drag ignores on each axis
    static constexpr int zeroDbStickiness = 5;    // pixels around 0 dB that snap a point to it
    static constexpr double minClipSeconds = 0.1;
    static constexpr int beatsPerBar = 4;
    static constexpr double minPixelsPerSecond = 0.2, maxPixelsPerSecond = 20000.0;   // (10x closer since M11: ~2 samples per pixel)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineComponent)
};
