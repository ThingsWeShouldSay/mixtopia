#include "TimelineComponent.h"
#include "SongNames.h"
#include "ClipTempo.h"
#include "EditHelpers.h"
#include "ClipEnvelopes.h"
#include "SongAnalysis.h"
#include "LevelMatch.h"
#include "ClipColours.h"
#include "Settings.h"
#include "SongTags.h"
#include "Loops.h"
#include "Theme.h"

namespace tempo = mixtopia::tempo;
namespace envelopes = mixtopia::envelopes;
namespace theme = mixtopia::theme;

namespace
{
    // The right-click menu's top row (mockup 26): five buttons in the lines' colours; a click adds that
    // point where the menu was opened. The chosen line is handed back through `choice`.
    struct PointButtonsRow : public juce::PopupMenu::CustomComponent
    {
        explicit PointButtonsRow (std::shared_ptr<int> c) : juce::PopupMenu::CustomComponent (false), choice (std::move (c)) {}

        void getIdealSize (int& w, int& h) override     { w = 2 * pad + numButtons * buttonWidth + (numButtons - 1) * gap; h = 54; }

        juce::Rectangle<int> buttonArea (int i) const   { return { pad + i * (buttonWidth + gap), 24, buttonWidth, 26 }; }

        int buttonAt (juce::Point<int> p) const
        {
            for (int i = 0; i < numButtons; ++i)
                if (buttonArea (i).contains (p))
                    return i;

            return -1;
        }

        void paint (juce::Graphics& g) override
        {
            theme::drawLabel (g, "ADD A POINT", { pad + 4, 4, 120, 16 });
            g.setColour (theme::text3);
            g.setFont (theme::font (theme::textSmall));
            g.drawText ("keys V B G T R", juce::Rectangle<int> (0, 4, getWidth() - pad - 4, 16), juce::Justification::centredRight);

            for (int i = 0; i < numButtons; ++i)
            {
                auto r = buttonArea (i).toFloat();
                g.setColour (i == hovered ? theme::raised.brighter (0.25f) : theme::raised);
                g.fillRoundedRectangle (r, theme::controlRadius);
                g.setColour (theme::line.brighter (0.15f));
                g.drawRoundedRectangle (r.reduced (0.5f), theme::controlRadius, 1.0f);
                auto param = (envelopes::Param) i;
                g.setColour (envelopes::getColour (param));
                g.setFont (theme::font (theme::textSmall, true));
                g.drawText (envelopes::getName (param), r, juce::Justification::centred);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override     { setHovered (buttonAt (e.getPosition())); }
        void mouseExit (const juce::MouseEvent&) override        { setHovered (-1); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (auto i = buttonAt (e.getPosition()); i >= 0)
            {
                *choice = i;
                triggerMenuItem();
            }
        }

        void setHovered (int i)
        {
            if (hovered != i)
            {
                hovered = i;
                repaint();
            }
        }

        static constexpr int numButtons = mixtopia::envelopes::numParams, buttonWidth = 54, gap = 4, pad = 10;
        std::shared_ptr<int> choice;
        int hovered = -1;
    };
}

void TimelineComponent::Playhead::paint (juce::Graphics& g)
{
    // Marker at the top (in the ruler, under the bar numbers), then the line.
    auto centre = (float) getWidth() / 2.0f;
    juce::Path marker;
    marker.addTriangle (centre - 6.0f, 0.0f, centre + 6.0f, 0.0f, centre, (float) markerHeight);
    g.setColour (theme::text1);
    g.fillPath (marker);
    g.fillRect (centre - 0.75f, (float) markerHeight - 1.0f, 1.5f, (float) getHeight());
}

TimelineComponent::TimelineComponent (te::Edit& e)
    : edit (e)
{
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    addAndMakeVisible (scrollBar);

    tempoStrip.onStatusMessage = [this] (const juce::String& text) { if (onStatusMessage) onStatusMessage (text); };
    addChildComponent (tempoStrip);

    playhead.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (playhead);

    overviews.onReady = [this] { repaint(); };
    edit.state.addListener (this);
}

TimelineComponent::~TimelineComponent()
{
    edit.state.removeListener (this);
}

//==============================================================================
double TimelineComponent::timeToX (double seconds) const   { return headerWidth + (seconds - viewStart) * pixelsPerSecond; }
double TimelineComponent::xToTime (double x) const         { return viewStart + (x - headerWidth) / pixelsPerSecond; }
double TimelineComponent::getVisibleSeconds() const        { return juce::jmax (1, getWidth() - headerWidth) / pixelsPerSecond; }

double TimelineComponent::xForBeat (double beat) const
{
    return timeToX (edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat)).inSeconds());
}

double TimelineComponent::getPixelsPerBeat() const
{
    return pixelsPerSecond * 60.0 / tempo::getProjectBpm (edit, te::TimePosition::fromSeconds (viewStart));
}

int TimelineComponent::getNumLanes() const
{
    return te::getAudioTracks (edit).size();
}

int TimelineComponent::getLaneHeight() const
{
    auto available = getHeight() - getLanesTop() - scrollBarHeight;
    return juce::jmax (40, available / juce::jmax (1, getNumLanes()));
}

juce::Rectangle<int> TimelineComponent::getLaneBounds (int lane) const
{
    return { 0, getLanesTop() + lane * getLaneHeight(), getWidth(), getLaneHeight() };
}

int TimelineComponent::laneAt (int y) const
{
    return juce::jlimit (0, getNumLanes() - 1, (y - getLanesTop()) / getLaneHeight());
}

juce::Rectangle<int> TimelineComponent::getClipBounds (te::Clip& clip) const
{
    auto lane = te::getAudioTracks (edit).indexOf (dynamic_cast<te::AudioTrack*> (clip.getTrack()));
    auto pos = clip.getPosition();
    auto x1 = timeToX (pos.getStart().inSeconds());
    auto x2 = timeToX (pos.getEnd().inSeconds());
    auto laneBounds = getLaneBounds (lane).reduced (0, 3);

    // Clamp so huge zoom levels don't overflow int coordinates.
    x1 = juce::jlimit (-10000.0, getWidth() + 10000.0, x1);
    x2 = juce::jlimit (-10000.0, getWidth() + 10000.0, x2);
    // (Outwards to whole pixels, so a point right at the clip's end is inside it and can be clicked.)
    auto left = (int) std::floor (x1), right = (int) std::ceil (x2);
    return { left, laneBounds.getY(), juce::jmax (2, right - left), laneBounds.getHeight() };
}

te::Clip* TimelineComponent::clipAt (juce::Point<int> p) const
{
    if (p.y < getLanesTop() || p.x < headerWidth)
        return nullptr;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (getClipBounds (*clip).contains (p))
                return clip;

    return nullptr;
}

TimelineComponent::DragMode TimelineComponent::dragModeFor (te::Clip& clip, juce::Point<int> p) const
{
    auto bounds = getClipBounds (clip);
    auto grab = juce::jmin (edgeGrabWidth, bounds.getWidth() / 4);

    if (p.x < bounds.getX() + grab)       return DragMode::trimStart;
    if (p.x >= bounds.getRight() - grab)  return DragMode::trimEnd;
    return DragMode::move;
}

void TimelineComponent::setViewStart (double seconds)
{
    viewStart = juce::jmax (0.0, seconds);
    updateScrollBar();
    updatePlayhead();
    repaint();
}

void TimelineComponent::updateScrollBar()
{
    auto visible = getVisibleSeconds();
    auto total = juce::jmax (edit.getLength().inSeconds(), viewStart + visible) + 60.0;
    scrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    scrollBar.setCurrentRange (viewStart, visible, juce::dontSendNotification);
}

void TimelineComponent::updatePlayhead()
{
    auto seconds = getPlayheadSeconds();
    auto x = juce::roundToInt (timeToX (seconds));
    playhead.setVisible (x >= headerWidth && x < getWidth());
    auto top = rulerHeight - Playhead::markerHeight;
    playhead.setBounds (x - Playhead::width / 2, top, Playhead::width, getHeight() - scrollBarHeight - top);

    // Developer measurement (MIXTOPIA_LOG_PLAYHEAD=1, see scripts/playhead/): logs the playhead on
    // every frame while playing.
    static const bool logPlayhead = juce::SystemStats::getEnvironmentVariable ("MIXTOPIA_LOG_PLAYHEAD", {}).isNotEmpty();

    if (logPlayhead && edit.getTransport().isPlaying())
    {
        auto* device = edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice();
        juce::Logger::writeToLog ("PLAYHEAD " + juce::String (juce::Time::getMillisecondCounterHiRes(), 2)
                                  + " shown " + juce::String (seconds, 5)
                                  + " engine " + juce::String (edit.getTransport().getPosition().inSeconds(), 5)
                                  + " x " + juce::String (x) + " pps " + juce::String (pixelsPerSecond, 1)
                                  + " outLatency " + juce::String (device != nullptr ? device->getOutputLatencyInSamples() : -1)
                                  + " buffer " + juce::String (device != nullptr ? device->getCurrentBufferSizeSamples() : -1));
    }
}

double TimelineComponent::getOutputLatencySeconds() const
{
    if (auto* device = edit.engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
        if (auto rate = device->getCurrentSampleRate(); rate > 0.0)
            return device->getOutputLatencyInSamples() / rate;

    return 0.0;
}

double TimelineComponent::getPlayheadSeconds()
{
    auto& transport = edit.getTransport();
    auto now = juce::Time::getMillisecondCounterHiRes();
    auto frameSeconds = (now - lastFrameMs) / 1000.0;
    lastFrameMs = now;

    if (! transport.isPlaying())
    {
        playheadClockRunning = false;
        return transport.getPosition().inSeconds();
    }

    // What's being heard: the engine's audible position (allows for the delay inside the mix,
    // e.g. the limiter's look-ahead) minus the sound card's output delay.
    auto* context = transport.getCurrentPlaybackContext();
    auto reported = (context != nullptr ? context->getAudibleTimelineTime() : transport.getPosition()).inSeconds()
                      - getOutputLatencySeconds();

    if (reported != lastReportedSeconds)
    {
        lastReportedSeconds = reported;
        lastReportMs = now;
    }

    auto estimate = reported + juce::jmin (0.1, (now - lastReportMs) / 1000.0);
    auto advanced = shownPlayheadSeconds + frameSeconds;

    if (! playheadClockRunning || std::abs (estimate - advanced) > 0.15)
        shownPlayheadSeconds = estimate;   // just started, or jumped (seek)
    else
        shownPlayheadSeconds = juce::jmax (shownPlayheadSeconds, advanced + 0.1 * (estimate - advanced));

    playheadClockRunning = true;
    return shownPlayheadSeconds;
}

void TimelineComponent::setZoom (double newPixelsPerSecond)
{
    pixelsPerSecond = juce::jlimit (minPixelsPerSecond, maxPixelsPerSecond, newPixelsPerSecond);
}

void TimelineComponent::zoomAround (double anchorX, double factor)
{
    auto anchorTime = xToTime (anchorX);
    setZoom (pixelsPerSecond * factor);
    setViewStart (anchorTime - (anchorX - headerWidth) / pixelsPerSecond);
}

void TimelineComponent::zoomBy (double factor)
{
    // Zoom around the playhead if it's on screen, otherwise around the centre.
    auto anchorX = timeToX (edit.getTransport().getPosition().inSeconds());

    if (anchorX < headerWidth || anchorX > getWidth())
        anchorX = (getWidth() + headerWidth) / 2.0;

    zoomAround (anchorX, factor);
}

void TimelineComponent::zoomToFit()
{
    auto length = juce::jmax (30.0, edit.getLength().inSeconds());
    setZoom ((getWidth() - headerWidth) / (length * 1.05));
    setViewStart (0.0);
}

void TimelineComponent::setVisibleRange (double startSeconds, double endSeconds)
{
    setZoom (juce::jmax (1, getWidth() - headerWidth) / juce::jmax (0.01, endSeconds - startSeconds));
    setViewStart (startSeconds);
}

void TimelineComponent::addLane()
{
    edit.getUndoManager().beginNewTransaction ("Add lane");
    edit.ensureNumberOfAudioTracks (getNumLanes() + 1);
}

void TimelineComponent::selectFirstClipOnLane (int lane)
{
    if (auto* track = te::getAudioTracks (edit)[lane]; track != nullptr && ! track->getClips().isEmpty())
        selectClip (track->getClips().getFirst()->itemID);
}

void TimelineComponent::selectSongOfFirstClipOnLane (int lane)
{
    if (auto* track = te::getAudioTracks (edit)[lane]; track != nullptr && ! track->getClips().isEmpty())
        if (auto* clip = dynamic_cast<te::AudioClipBase*> (track->getClips().getFirst()))
            selectSongPieces (*clip);
}

void TimelineComponent::selectEveryPieceOfSelectedSong()
{
    if (auto* clip = getSelectedClip())
        selectSongPieces (*clip);
}

void TimelineComponent::showClip (te::EditItemID id)
{
    if (auto* clip = te::findClipForID (edit, id))
    {
        selectClip (id);
        auto start = clip->getPosition().getStart().inSeconds();
        edit.getTransport().setPosition (clip->getPosition().getStart());

        if (start < viewStart || start > viewStart + getVisibleSeconds() * 0.9)
            setViewStart (juce::jmax (0.0, start - getVisibleSeconds() * 0.1));
    }
}

//==============================================================================
void TimelineComponent::selectClip (te::EditItemID id, int piece)
{
    if (selectedClipID == id && selectedPiece == piece && extraSelection.empty())
        return;

    extraSelection.clear();
    selectedClipID = id;
    selectedPiece = piece;
    repaint();

    if (onSelectionChanged)
        onSelectionChanged();
}

te::WaveAudioClip* TimelineComponent::getSelectedClip() const
{
    return dynamic_cast<te::WaveAudioClip*> (te::findClipForID (edit, selectedClipID));
}

void TimelineComponent::deleteSelectedClip()
{
    // Everything selected; a piece of a chopped song that still touches the others: only that piece goes.
    auto spots = selectedSpots();

    if (! spots.empty())
    {
        edit.getUndoManager().beginNewTransaction (spots.size() > 1 ? "Delete pieces" : selectedPiece >= 0 ? "Delete piece" : "Delete clip");

        for (auto& spot : spots)
            if (auto* target = separateAt (spot))
                target->removeFromParent();

        mixtopia::chops::tidy (edit);
    }

    selectClip ({});
}

std::vector<TimelineComponent::PieceRef> TimelineComponent::getSelection() const
{
    std::vector<PieceRef> all;

    if (selectedClipID.isValid())
        all.push_back ({ selectedClipID, selectedPiece });

    for (auto& r : extraSelection)
        if (std::find (all.begin(), all.end(), r) == all.end())
            all.push_back (r);

    return all;
}

bool TimelineComponent::isPieceSelected (const te::AudioClipBase& clip, int piece) const
{
    for (auto& r : getSelection())
        if (r.clip == clip.itemID && (r.piece < 0 || piece < 0 || r.piece == piece))
            return true;

    return false;
}

void TimelineComponent::setSelection (const std::vector<PieceRef>& refs)
{
    extraSelection.clear();
    selectedClipID = refs.empty() ? te::EditItemID() : refs.front().clip;
    selectedPiece = refs.empty() ? -1 : refs.front().piece;

    for (size_t i = 1; i < refs.size(); ++i)
        if (std::find (extraSelection.begin(), extraSelection.end(), refs[i]) == extraSelection.end() && ! (refs[i] == refs.front()))
            extraSelection.push_back (refs[i]);

    repaint();

    if (onSelectionChanged)
        onSelectionChanged();

    if (refs.size() > 1 && onStatusMessage)
        onStatusMessage (juce::String ((int) refs.size()) + " selected: Del deletes them, Ctrl+D copies them, dragging one moves them all.");
}

void TimelineComponent::toggleSelected (PieceRef ref)
{
    auto all = getSelection();

    if (auto found = std::find (all.begin(), all.end(), ref); found != all.end())
        all.erase (found);
    else
        all.push_back (ref);

    setSelection (all);
}

void TimelineComponent::remapSelection (const std::vector<mixtopia::chops::Merge>& merges)
{
    if (merges.empty())
        return;

    auto all = getSelection();

    for (auto& m : merges)
        for (auto& r : all)
        {
            if (r.clip == m.removed)
                r = { m.into, m.piece + juce::jmax (0, r.piece) };
            else if (r.clip == m.into)
                r.piece = r.piece < 0 ? m.prepended : r.piece + m.prepended;
        }

    setSelection (all);
}

std::vector<TimelineComponent::Spot> TimelineComponent::selectedSpots()
{
    std::vector<Spot> spots;
    auto tracks = te::getAudioTracks (edit);

    for (auto& r : getSelection())
        if (auto* clip = dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, r.clip)))
        {
            auto range = r.piece < 0 ? juce::Range<double> (clip->getPosition().getStart().inSeconds(), clip->getPosition().getEnd().inSeconds())
                                     : mixtopia::chops::getPieceEditRange (*clip, r.piece);
            spots.push_back ({ tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip->getTrack())), range.getStart() + range.getLength() * 0.5, r.piece < 0 });
        }

    return spots;
}

te::AudioClipBase* TimelineComponent::separateAt (const Spot& spot)
{
    if (auto* track = te::getAudioTracks (edit)[spot.lane])
        for (auto* c : track->getClips())
            if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && clip->getPosition().time.contains (te::TimePosition::fromSeconds (spot.middle)))
            {
                if (spot.whole || mixtopia::chops::getCuts (*clip).empty())
                    return clip;

                return mixtopia::chops::separate (*clip, mixtopia::chops::pieceAt (*clip, spot.middle));
            }

    return nullptr;
}

void TimelineComponent::selectSongPieces (te::AudioClipBase& clip)
{
    std::vector<PieceRef> refs;

    for (auto* c : piecesOfSong (clip))
    {
        auto count = (int) mixtopia::chops::getPieces (*c).size();

        for (int i = 0; i < count; ++i)
            refs.push_back ({ c->itemID, count > 1 ? i : -1 });
    }

    setSelection (refs);
}

std::vector<te::AudioClipBase*> TimelineComponent::piecesOfSong (te::AudioClipBase& clip)
{
    if (! mixtopia::chops::isPiece (clip))
        return { &clip };

    std::vector<te::AudioClipBase*> pieces;

    for (auto* c : mixtopia::chops::clipsOfSong (edit, mixtopia::chops::songKey (clip)))
        if (mixtopia::chops::isPiece (*c))
            pieces.push_back (c);

    return pieces;
}

bool TimelineComponent::cutAt (te::AudioClipBase& clip, double editSeconds, bool snap)
{
    // With Snap on, the cut lands on the song's nearest beat tick (as quick loops do).
    auto loop = mixtopia::loops::getLoop (clip);
    auto played = tempo::editToSourceSeconds (clip, editSeconds);

    if (snap && tempo::getClipBpm (clip) > 0.0)
        played = mixtopia::loops::songToPlayed (loop, mixtopia::loops::nearestBeat (clip, mixtopia::loops::playedToSong (loop, played)));

    return mixtopia::chops::cut (clip, played);
}

void TimelineComponent::cutAllAt (double editSeconds, bool snap)
{
    edit.getUndoManager().beginNewTransaction ("Cut");
    auto any = false;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* c : track->getClips())
            if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && clip->getPosition().time.contains (te::TimePosition::fromSeconds (editSeconds)))
                any = cutAt (*clip, editSeconds, snap) || any;

    if (! any && onStatusMessage)
        onStatusMessage ("Nothing to cut there (no song, or too close to a song's edge or another cut).");
}

void TimelineComponent::cutKey (bool everySong)
{
    auto now = getPlayheadSeconds();

    if (everySong)
    {
        cutAllAt (now, snapEnabled);
        return;
    }

    // The selected song if it's under the playhead, else the top-most lane's song there.
    te::AudioClipBase* target = getSelectedClip();

    if (target != nullptr && ! target->getPosition().time.contains (te::TimePosition::fromSeconds (now)))
        target = nullptr;

    for (int lane = 0; target == nullptr && lane < getNumLanes(); ++lane)
        if (auto* track = te::getAudioTracks (edit)[lane])
            for (auto* c : track->getClips())
                if (c->getPosition().time.contains (te::TimePosition::fromSeconds (now)))
                    target = dynamic_cast<te::AudioClipBase*> (c);

    if (target == nullptr)
    {
        if (onStatusMessage)
            onStatusMessage ("No song under the playhead to cut.");

        return;
    }

    edit.getUndoManager().beginNewTransaction ("Cut");

    if (! cutAt (*target, now, snapEnabled) && onStatusMessage)
        onStatusMessage ("Can't cut there: too close to the song's edge or another cut.");
}

void TimelineComponent::duplicateSelected()
{
    auto* clip = getSelectedClip();

    if (clip == nullptr)
    {
        if (onStatusMessage)
            onStatusMessage ("Select a song or piece to copy first.");

        return;
    }

    auto spots = selectedSpots();

    if (spots.size() > 1)
    {
        // Several: copied as a group right after the group (same lanes, same spacing); if a copy
        // wouldn't fit there, the whole group goes as many lanes down as needed (new lanes if none).
        struct Item { te::AudioClipBase* clip; int piece; int lane; double startBeat, endBeat; };
        std::vector<Item> items;

        for (auto& spot : spots)
            if (auto* track = te::getAudioTracks (edit)[spot.lane])
                for (auto* c : track->getClips())
                    if (auto* found = dynamic_cast<te::AudioClipBase*> (c); found != nullptr && found->getPosition().time.contains (te::TimePosition::fromSeconds (spot.middle)))
                    {
                        auto count = (int) mixtopia::chops::getPieces (*found).size();
                        auto first = spot.whole ? 0 : mixtopia::chops::pieceAt (*found, spot.middle);
                        auto last = spot.whole ? count - 1 : first;

                        for (int i = first; i <= last; ++i)
                        {
                            auto range = mixtopia::chops::getPieceEditRange (*found, i);
                            items.push_back ({ found, i, spot.lane, tempo::toBeats (edit, te::TimePosition::fromSeconds (range.getStart())),
                                               tempo::toBeats (edit, te::TimePosition::fromSeconds (range.getEnd())) });
                        }
                    }

        if (items.empty())
            return;

        auto from = items.front().startBeat, to = items.front().endBeat;

        for (auto& item : items)
        {
            from = juce::jmin (from, item.startBeat);
            to = juce::jmax (to, item.endBeat);
        }

        auto span = to - from;
        auto fits = [&] (int laneShift)
        {
            auto tracks = te::getAudioTracks (edit);

            for (auto& item : items)
                if (auto* track = tracks[item.lane + laneShift])
                    for (auto* c : track->getClips())
                    {
                        auto t = c->getPosition().time;
                        auto a = tempo::toTime (edit, item.startBeat + span).inSeconds(), b = tempo::toTime (edit, item.endBeat + span).inSeconds();

                        if (t.getStart().inSeconds() < b - 1.0e-6 && t.getEnd().inSeconds() > a + mixtopia::chops::fadeSeconds + 1.0e-6)
                            return false;
                    }

            return true;
        };

        auto shift = 0;

        while (! fits (shift))
            ++shift;

        edit.getUndoManager().beginNewTransaction ("Copy pieces");
        auto highest = 0;

        for (auto& item : items)
            highest = juce::jmax (highest, item.lane + shift);

        edit.ensureNumberOfAudioTracks (highest + 1);
        std::vector<PieceRef> copies;

        for (auto& item : items)
            if (auto* copy = mixtopia::chops::copyPiece (*item.clip, item.piece, *te::getAudioTracks (edit)[item.lane + shift], span))
                copies.push_back ({ copy->itemID, -1 });

        mixtopia::chops::tidy (edit);
        setSelection (copies);

        if (shift > 0 && onStatusMessage)
            onStatusMessage ("No room after them on their lanes: the copies went " + juce::String (shift) + (shift == 1 ? " lane" : " lanes") + " down.");

        return;
    }

    edit.getUndoManager().beginNewTransaction ("Copy piece");
    juce::String status;
    auto* copy = mixtopia::chops::duplicate (*clip, juce::jmax (0, selectedPiece), status);
    auto copyID = copy != nullptr ? copy->itemID : te::EditItemID();
    mixtopia::chops::tidy (edit);

    if (copyID.isValid())
        selectClip (copyID);

    if (status.isNotEmpty() && onStatusMessage)
        onStatusMessage (status);
}

void TimelineComponent::afterPiecesMoved()
{
    // Pieces put back exactly where they followed on join up again (bit-for-bit as before); pieces
    // that start where another ends get their crossfade.
    remapSelection (mixtopia::chops::tidy (edit));
}

void TimelineComponent::setSelectedClipBpm (double bpm)
{
    if (auto clip = getSelectedClip())
    {
        edit.getUndoManager().beginNewTransaction ("Set BPM");

        for (auto* c : piecesOfSong (*clip))
        {
            tempo::setBpmAndDownbeat (*c, bpm, tempo::getDownbeat (*c), getClipSnap());
            markGridAdjusted (*c);
        }
    }
}

void TimelineComponent::clearSelectedClipBpm()
{
    if (auto clip = getSelectedClip())
    {
        edit.getUndoManager().beginNewTransaction ("Clear BPM");

        for (auto* c : piecesOfSong (*clip))
        {
            tempo::setBpmAndDownbeat (*c, 0.0, 0.0, tempo::Snap::none);
            c->state.removeProperty (tempo::gridSourceProperty, &edit.getUndoManager());
        }

        rememberGrid (*clip);
    }
}

void TimelineComponent::nudgeSelectedClipDownbeat (double seconds)
{
    if (auto clip = getSelectedClip(); clip != nullptr && tempo::getClipBpm (*clip) > 0.0)
    {
        edit.getUndoManager().beginNewTransaction ("Move beat grid");
        auto bpm = tempo::getClipBpm (*clip), downbeat = tempo::getDownbeat (*clip) + seconds;

        for (auto* c : piecesOfSong (*clip))
        {
            tempo::setBpmAndDownbeat (*c, bpm, downbeat, getClipSnap());
            markGridAdjusted (*c);
        }
    }
}

void TimelineComponent::shiftSelectedClipBarStart (int beats)
{
    if (auto clip = getSelectedClip(); clip != nullptr && tempo::getClipBpm (*clip) > 0.0)
    {
        // Same beat lines, but beat 1 a beat earlier or later. With snap the song then moves by a
        // beat so its new beat 1 sits on a bar start.
        edit.getUndoManager().beginNewTransaction ("Move beat 1 by a beat");
        auto bpm = tempo::getClipBpm (*clip);
        auto downbeat = tempo::getDownbeat (*clip) + beats * 60.0 / bpm;

        for (auto* c : piecesOfSong (*clip))
        {
            tempo::setBpmAndDownbeat (*c, bpm, downbeat, getClipSnap());
            markGridAdjusted (*c);
        }
    }
}

void TimelineComponent::setSelectedClipMatching (bool shouldMatch)
{
    if (auto clip = getSelectedClip())
    {
        edit.getUndoManager().beginNewTransaction (shouldMatch ? "Match song level" : "Don't match song level");

        for (auto* c : piecesOfSong (*clip))
        {
            if (shouldMatch)
                c->state.removeProperty (mixtopia::levelmatch::matchOffProperty, &edit.getUndoManager());
            else
                c->state.setProperty (mixtopia::levelmatch::matchOffProperty, true, &edit.getUndoManager());
        }
    }
}

void TimelineComponent::setSelectedClipKey (const juce::String& camelot)
{
    if (auto clip = getSelectedClip())
    {
        edit.getUndoManager().beginNewTransaction ("Set key");

        for (auto* c : piecesOfSong (*clip))
            mixtopia::analysis::setKey (*c, camelot, true);

        if (library != nullptr)
            library->rememberKey (getSourcePath (*clip), camelot);
    }
}

bool TimelineComponent::resetSelectedClipToDetectedKey()
{
    auto clip = getSelectedClip();

    if (clip == nullptr)
        return false;

    auto info = mixtopia::analysis::analyse (edit.engine, mixtopia::loops::getSongFile (*clip));

    if (! info || info->key.isEmpty())
        return false;

    edit.getUndoManager().beginNewTransaction ("Detected key");

    for (auto* c : piecesOfSong (*clip))
        mixtopia::analysis::setKey (*c, info->key, false);

    if (library != nullptr)
        library->forgetKey (getSourcePath (*clip));

    return true;
}

bool TimelineComponent::resetSelectedClipToDetectedGrid()
{
    auto clip = getSelectedClip();

    if (clip == nullptr)
        return false;

    auto info = mixtopia::analysis::analyse (edit.engine, mixtopia::loops::getSongFile (*clip));

    if (! info || ! info->grid.valid)
        return false;

    edit.getUndoManager().beginNewTransaction ("Detected beat grid");

    for (auto* c : piecesOfSong (*clip))
        mixtopia::analysis::reapplyDetectedGrid (*c, *info, snapEnabled);

    if (library != nullptr)
        library->forgetGrid (getSourcePath (*clip));

    return true;
}

bool TimelineComponent::canSetDownbeatAtPlayhead() const
{
    auto clip = getSelectedClip();
    return clip != nullptr && tempo::getClipBpm (*clip) > 0.0
            && clip->getPosition().time.contains (edit.getTransport().getPosition());
}

void TimelineComponent::setSelectedClipDownbeatAtPlayhead()
{
    if (! canSetDownbeatAtPlayhead())
        return;

    auto clip = getSelectedClip();
    auto source = tempo::editToSourceSeconds (*clip, edit.getTransport().getPosition().inSeconds());
    edit.getUndoManager().beginNewTransaction ("Set beat 1");
    auto bpm = tempo::getClipBpm (*clip);

    for (auto* c : piecesOfSong (*clip))
    {
        tempo::setBpmAndDownbeat (*c, bpm, source, getClipSnap());
        markGridAdjusted (*c);
    }
}

double TimelineComponent::snapTime (double seconds) const
{
    return tempo::snapToBeat (edit, te::TimePosition::fromSeconds (seconds)).inSeconds();
}

void TimelineComponent::addFiles (const juce::StringArray& filesToAdd, int firstLane, te::TimePosition start)
{
    auto files = filesToAdd;

    // Songs already in the mix: ask before adding them again (test sessions: add them anyway).
    {
        auto order = getClipsInStartOrder();
        std::vector<te::EditItemID> firstCopies;
        juce::StringArray duplicates;
        juce::String list;

        for (auto& path : files)
            for (size_t i = 0; i < order.size(); ++i)
                if (getSourcePath (*order[i]) == juce::File (path).getFullPathName())
                {
                    auto* clip = order[i];
                    auto* song = library != nullptr ? library->find (getSourcePath (*clip)) : nullptr;
                    auto name = song != nullptr ? mixtopia::tags::Tags { song->title, song->artist }
                                                : mixtopia::tags::fromFileName (clip->getName());   // (not in the library)
                    list << "\n" << (name.artist.isNotEmpty() ? name.artist + " - " : juce::String()) + name.title;
                    duplicates.add (path);
                    firstCopies.push_back (clip->itemID);
                    break;
                }

        if (! duplicates.isEmpty())
        {
            if (onDuplicatesFound)
                onDuplicatesFound (firstCopies);

            auto one = duplicates.size() == 1;
            static const bool askInTests = juce::SystemStats::getEnvironmentVariable ("MIXTOPIA_TEST_DIALOGS", {}).isNotEmpty();
            auto addAnyway = (mixtopia::settings::isTestSession() && ! askInTests)
                               || juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::NoIcon, "Already in the mix", list.trim(),
                                                                      "Add anyway", one ? "Don't add it" : "Skip them", this);

            if (! addAnyway)
                for (auto& d : duplicates)
                    files.removeString (d);

            if (files.isEmpty())
                return;
        }
    }

    // Decode and find the beat of songs that haven't been yet (progress window).
    juce::Array<juce::File> songs;

    for (auto& path : files)
        if (juce::File f (path); mixtopia::isSupportedAudioFile (f))
            songs.add (f);

    if (library != nullptr)
        library->add (songs);

    if (! mixtopia::analysis::prepareWithProgressWindow (edit.engine, songs) && onStatusMessage)
        onStatusMessage ("Cancelled - songs that weren't prepared yet weren't added.");

    edit.getUndoManager().beginNewTransaction ("Add files");
    auto lane = firstLane;
    auto dropTime = start;

    if (snapEnabled)
        start = tempo::barAtOrBefore (edit, start);

    for (auto& path : files)
    {
        juce::File file (path);

        if (! mixtopia::isSupportedAudioFile (file) || ! mixtopia::decodedCache::isReady (file))
            continue;

        edit.ensureNumberOfAudioTracks (lane + 1);

        if (auto clip = mixtopia::insertAudioFile (*te::getAudioTracks (edit)[lane], file, start))
        {
            // Tempo-match it with its detected beat grid (or the one corrected by hand, remembered by
            // the library; beat 1 on a bar start), trim the silence at its start, and keep it on the
            // grid at the start of the mix.
            auto info = mixtopia::analysis::getCached (mixtopia::loops::getSongFile (*clip));

            if (info && library != nullptr)
                library->applyCorrections (file.getFullPathName(), *info);

            if (info && info->grid.valid)
            {
                if (mixtopia::analysis::prepareNewClip (*clip, *info, dropTime, snapEnabled, mixtopia::analysis::isAutoTrimEnabled()))
                    reportMovedForPickup (*clip);
            }
            else if (onStatusMessage)
            {
                onStatusMessage ("No steady beat found in " + file.getFileName() + " - type its BPM to tempo-match it.");
            }

            // Remembered corrections show as such ("grid adjusted", key "set by hand").
            if (auto* song = library != nullptr ? library->find (file.getFullPathName()) : nullptr)
            {
                if (song->keyByHand)
                    mixtopia::analysis::setKey (*clip, song->key, true);

                if (song->gridByHand && tempo::getClipBpm (*clip) > 0.0)
                    clip->state.setProperty (tempo::gridSourceProperty, "adjusted", &edit.getUndoManager());
            }

            selectClip (clip->itemID);
            ++lane;
        }
        else if (onStatusMessage)
        {
            onStatusMessage ("Couldn't read " + file.getFileName());
        }
    }
}

void TimelineComponent::markGridAdjusted (te::AudioClipBase& clip)
{
    clip.state.setProperty (tempo::gridSourceProperty, "adjusted", &edit.getUndoManager());
    rememberGrid (clip);
}

juce::String TimelineComponent::getSourcePath (te::AudioClipBase& clip) const
{
    return mixtopia::getSourceFile (clip).getFullPathName();
}

void TimelineComponent::rememberGrid (te::AudioClipBase& clip)
{
    // The library remembers the corrected grid for the next time the song is placed.
    if (library != nullptr)
        library->rememberGrid (getSourcePath (clip), tempo::getClipBpm (clip), tempo::getDownbeat (clip));
}

std::vector<te::AudioClipBase*> TimelineComponent::getClipsInStartOrder() const
{
    std::vector<te::AudioClipBase*> clips;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* c : track->getClips())
            if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                clips.push_back (clip);

    std::stable_sort (clips.begin(), clips.end(), [] (auto* a, auto* b) { return a->getPosition().getStart() < b->getPosition().getStart(); });
    return clips;
}

bool TimelineComponent::isInterestedInDragSource (const SourceDetails& d)
{
    return ! mixtopia::library::pathsFromDragDescription (d.description).isEmpty();
}

void TimelineComponent::itemDragMove (const SourceDetails& d)
{
    dropHighlightLane = laneAt (d.localPosition.y);
    repaint();
}

void TimelineComponent::itemDragExit (const SourceDetails&)
{
    dropHighlightLane = -1;
    repaint();
}

void TimelineComponent::itemDropped (const SourceDetails& d)
{
    filesDropped (mixtopia::library::pathsFromDragDescription (d.description), d.localPosition.x, d.localPosition.y);
}

bool TimelineComponent::isOnGrid (te::AudioClipBase& clip) const
{
    // Beat 1 (any of the song's bar starts) within 2 ms of a project bar line.
    auto firstBarStart = te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (clip, tempo::barPhase (tempo::getClipBpm (clip),
                                                                                                         tempo::getDownbeat (clip))));
    auto snapped = tempo::snapToBar (edit, firstBarStart);
    return std::abs ((firstBarStart - snapped).inSeconds()) < 0.002;
}

void TimelineComponent::reportMovedForPickup (te::AudioClipBase& clip)
{
    if (! onStatusMessage)
        return;

    // The bar the song's beat 1 is on now.
    auto beatOne = te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (clip, tempo::getDownbeat (clip)));
    auto bar = (int) std::round (edit.tempoSequence.toBeats (beatOne).inBeats() / tempo::beatsPerBar) + 1;
    onStatusMessage (clip.getName() + ": beat 1 is on bar " + juce::String (bar) + " so the sound before it isn't cut.");
}

void TimelineComponent::removeLane (int lane)
{
    auto tracks = te::getAudioTracks (edit);

    if (tracks.size() <= 1 || ! juce::isPositiveAndBelow (lane, tracks.size()))
        return;

    edit.getUndoManager().beginNewTransaction ("Remove lane");
    edit.deleteTrack (tracks[lane]);
}

void TimelineComponent::openClipMenuOnLane (int lane)
{
    if (auto* track = te::getAudioTracks (edit)[lane]; track != nullptr && ! track->getClips().isEmpty())
        if (auto* clip = dynamic_cast<te::WaveAudioClip*> (track->getClips().getFirst()))
        {
            auto b = getClipBounds (*clip).getIntersection (getLocalBounds());
            selectClip (clip->itemID, mixtopia::chops::getCuts (*clip).empty() ? -1 : 0);
            showClipMenu (*clip, { b.getX() + 40, b.getCentreY() }, true);
        }
}

void TimelineComponent::showClipMenu (te::WaveAudioClip& clip, juce::Point<int> mousePosition, bool atThatPoint)
{
    // Mockup 26 (user-approved 2026-11-02, with two changes): a row of point buttons on top; only rows that
    // apply (no greyed-out ones); the BPM / beat 1 / key fix-ups are on the clip bar (Clear, Beat grid,
    // Use detected). Item IDs: clear one line = 11 + param.
    enum { pointButtons = 1, clearLineBase = 11, clearAllLines = 20, restoreStart, deleteClip,
           repeatPart, removeLoop, cutHere, cutEverySong, copyPiece, joinNext, selectSong };
    auto several = getSelection().size() > 1;
    auto chopped = ! mixtopia::chops::getCuts (clip).empty();
    auto isPiece = chopped || mixtopia::chops::isPiece (clip);
    auto hasLoop = mixtopia::loops::getLoop (clip).has_value();
    auto hasLines = false;
    auto chosenLine = std::make_shared<int> (-1);

    juce::PopupMenu menu, clearMenu;
    menu.addCustomItem (pointButtons, std::make_unique<PointButtonsRow> (chosenLine));

    for (int p = 0; p < envelopes::numParams; ++p)
    {
        auto param = (envelopes::Param) p;
        auto hasPoints = ! envelopes::getPoints (clip.state, param).empty();
        hasLines = hasLines || hasPoints;

        if (hasPoints)
            clearMenu.addItem (clearLineBase + p, envelopes::getName (param));
    }

    if (hasLines)
    {
        clearMenu.addSeparator();
        clearMenu.addItem (clearAllLines, "All lines");
        menu.addSubMenu ("Clear lines", clearMenu);
    }

    auto withKey = [&menu] (int id, const juce::String& text, const juce::String& key)
    {
        juce::PopupMenu::Item item (text);
        item.shortcutKeyDescription = key;
        menu.addItem (item.setID (id));
    };

    auto count = juce::String ((int) getSelection().size());
    menu.addSeparator();
    withKey (cutHere, "Cut here", "C");
    withKey (cutEverySong, "Cut every song here", "Shift+C");
    withKey (copyPiece, several ? "Duplicate the selected (" + count + ")" : isPiece ? "Duplicate this piece" : "Duplicate this song", "Ctrl+D");

    // (Join only when the next piece follows on in the song but differs - alike ones join by themselves.)
    auto lastPiece = chopped ? (int) mixtopia::chops::getPieces (clip).size() - 1 : 0;

    if (! several && (! chopped || selectedPiece == lastPiece) && mixtopia::chops::nextJoinable (clip) != nullptr)
        menu.addItem (joinNext, "Join with the next piece");

    if (isPiece)
        menu.addItem (selectSong, "Select every piece of this song");

    menu.addSeparator();

    if (hasLoop)
        withKey (repeatPart, "Edit loop...", "E");
    else
        menu.addItem (repeatPart, "Repeat part of this song...");

    if (hasLoop)
        menu.addItem (removeLoop, "Remove the loop");

    if ((double) clip.state.getProperty (mixtopia::analysis::autoTrimProperty, 0.0) > 0.0 && clip.getPosition().getOffset() > te::TimeDuration())
        menu.addItem (restoreStart, "Restore the start (undo auto-trim)");

    menu.addSeparator();
    withKey (deleteClip, several ? "Delete the selected (" + count + ")" : isPiece ? "Delete this piece" : "Delete clip", "Del");

    auto options = atThatPoint ? juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (juce::Rectangle<int> (mousePosition, mousePosition).expanded (1)))
                               : juce::PopupMenu::Options().withMousePosition();

    menu.showMenuAsync (options,
                        [this, safeThis = juce::Component::SafePointer<TimelineComponent> (this),
                         clipID = clip.itemID, mousePosition, chosenLine] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            auto* target = dynamic_cast<te::WaveAudioClip*> (te::findClipForID (edit, clipID));

                            if (target == nullptr)
                                return;

                            if (result == pointButtons && juce::isPositiveAndBelow (*chosenLine, envelopes::numParams))
                                addPoint (*target, (envelopes::Param) *chosenLine, mousePosition);
                            else if (result >= clearLineBase && result < clearLineBase + envelopes::numParams)
                                clearLines (*target, (envelopes::Param) (result - clearLineBase));
                            else if (result == clearAllLines)
                                clearLines (*target, {});
                            else if (result == restoreStart)
                            {
                                edit.getUndoManager().beginNewTransaction ("Restore the start");
                                mixtopia::analysis::restoreTrimmedStart (*target);
                            }
                            else if (result == deleteClip)
                                deleteSelectedClip();
                            else if (result == cutHere)
                            {
                                edit.getUndoManager().beginNewTransaction ("Cut");

                                if (! cutAt (*target, xToTime (mousePosition.x), snapEnabled) && onStatusMessage)
                                    onStatusMessage ("Can't cut there: too close to the song's edge or another cut.");
                            }
                            else if (result == cutEverySong)
                                cutAllAt (xToTime (mousePosition.x), snapEnabled);
                            else if (result == copyPiece)
                                duplicateSelected();
                            else if (result == selectSong)
                                selectSongPieces (*target);
                            else if (result == joinNext)
                            {
                                edit.getUndoManager().beginNewTransaction ("Join pieces");
                                auto merged = mixtopia::chops::joinWithNext (*target);

                                if (merged.into.isValid())
                                    selectClip (merged.into, merged.prepended > 0 ? merged.prepended : merged.piece);

                                afterPiecesMoved();
                            }
                            else if (result == repeatPart)
                                repeatPartOf (*target, mousePosition.x);
                            else if (result == removeLoop)
                            {
                                edit.getUndoManager().beginNewTransaction ("Remove the loop");
                                mixtopia::loops::apply (*target, std::nullopt, false);

                                if (onStatusMessage)
                                    onStatusMessage (target->getName() + ": loop removed.");
                            }
                        });
}

void TimelineComponent::clearLines (te::WaveAudioClip& clip, std::optional<envelopes::Param> onlyThis)
{
    edit.getUndoManager().beginNewTransaction ("Clear lines");

    for (int p = 0; p < envelopes::numParams; ++p)
        if (! onlyThis || *onlyThis == (envelopes::Param) p)
            envelopes::setPoints (clip.state, (envelopes::Param) p, {}, &edit.getUndoManager());
}

void TimelineComponent::showLaneMenu (int lane)
{
    enum { addLaneItem = 1, removeItem };
    auto numClips = te::getAudioTracks (edit)[lane]->getClips().size();

    juce::PopupMenu menu;
    menu.addItem (addLaneItem, "Add lane");
    menu.addItem (removeItem, numClips > 0 ? "Remove lane " + juce::String (lane + 1) + " (and its "
                                           + juce::String (numClips) + (numClips == 1 ? " clip)" : " clips)")
                                       : "Remove lane " + juce::String (lane + 1),
                  getNumLanes() > 1);

    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [this, lane, safeThis = juce::Component::SafePointer<TimelineComponent> (this)] (int result)
                        {
                            if (safeThis == nullptr)
                                return;

                            if (result == addLaneItem)
                                safeThis->addLane();

                            if (result == removeItem)
                                removeLane (lane);
                        });
}

//==============================================================================
void TimelineComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::laneA);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (getLocalBounds().withTrimmedLeft (headerWidth));
        paintLanes (g);
        paintRuler (g);

        if (dragMode == DragMode::box)
        {
            auto box = juce::Rectangle<int> (boxStart, boxEnd).toFloat();
            g.setColour (theme::accent.withAlpha (0.12f));
            g.fillRect (box);
            g.setColour (theme::accent);
            g.drawRect (box, 1.0f);
        }
    }

    paintLaneHeaders (g);
}

void TimelineComponent::paintLaneHeaders (juce::Graphics& g)
{
    auto bottom = getHeight() - scrollBarHeight;
    g.setColour (theme::panel);
    g.fillRect (0, 0, headerWidth, getHeight());

    // The ruler's corner.
    g.setColour (theme::raised);
    g.fillRect (0, 0, headerWidth, rulerHeight);
    g.setColour (theme::line);
    g.fillRect (0, rulerHeight - 1, headerWidth, 1);
    theme::drawLabel (g, "BARS", { 12, 0, headerWidth - 12, rulerHeight });

    g.setColour (theme::line);
    g.fillRect (headerWidth - 1, 0, 1, bottom);

    auto tracks = te::getAudioTracks (edit);

    for (int lane = 0; lane < tracks.size(); ++lane)
    {
        auto bounds = getLaneBounds (lane).withWidth (headerWidth - 1);
        g.setColour (theme::line.withAlpha (0.6f));
        g.fillRect (0, bounds.getBottom() - 1, headerWidth - 1, 1);
        g.setColour (mixtopia::isLaneAudible (*tracks[lane]) ? theme::text2 : theme::text3.withAlpha (0.6f));
        g.setFont (theme::font (theme::textNormal, true));
        g.drawText (juce::String (lane + 1), bounds.getX() + 13, bounds.getY() + 6, 40, 18, juce::Justification::centredLeft);

        if (bounds.getHeight() >= fullLoopToolsHeight)
            theme::drawLabel (g, "LOOP", { 13, bounds.getY() + 58, 60, 16 });
    }
}

void TimelineComponent::updateLaneControls()
{
    auto tracks = te::getAudioTracks (edit);

    while (laneControls.size() > (size_t) tracks.size())
        laneControls.pop_back();

    while (laneControls.size() < (size_t) tracks.size())
    {
        auto controls = std::make_unique<LaneControls>();

        for (auto* b : { &controls->mute, &controls->solo })
        {
            b->setClickingTogglesState (true);
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::textColourOffId, theme::text2);
            b->setColour (juce::TextButton::textColourOnId, theme::darkText);
            addAndMakeVisible (*b);
        }

        controls->mute.setColour (juce::TextButton::buttonOnColourId, theme::mute);
        controls->solo.setColour (juce::TextButton::buttonOnColourId, theme::solo);
        controls->mute.setTooltip ("Mute this lane");
        controls->solo.setTooltip ("Solo: hear only soloed lanes");

        auto laneIndex = (int) laneControls.size();

        for (int i = 0; i < numQuickLoops; ++i)
        {
            auto& b = controls->bars[i];
            b.setTooltip ("Quick loop: " + juce::String (quickLoopBars[i]) + (quickLoopBars[i] == 1 ? " bar" : " bars")
                          + " of the song under the playhead on this lane, from the playhead (1 repeat)");
            b.onClick = [this, laneIndex, bars = quickLoopBars[i]] { quickLoop (laneIndex, bars); };
        }

        controls->loopIn.setTooltip ("Loop in (I): the loop starts here - then press Out (O) where it should end. "
                                     "Press In again (or Esc, or Ctrl+Z) to cancel it.");
        controls->loopOut.setTooltip ("Loop out (O): the loop ends here (after Loop in on this lane)");
        controls->loopMenu.setTooltip ("Quick loops for the song under the playhead on this lane");
        controls->loopIn.onClick = [this, laneIndex] { loopIn (laneIndex); };
        controls->loopOut.onClick = [this, laneIndex] { loopOut (laneIndex); };
        controls->loopMenu.onClick = [this, laneIndex] { showLoopMenu (laneIndex); };

        for (auto* b : std::initializer_list<juce::TextButton*> { &controls->bars[0], &controls->bars[1], &controls->bars[2], &controls->bars[3],
                                                                  &controls->loopIn, &controls->loopOut, &controls->loopMenu })
        {
            b->setWantsKeyboardFocus (false);
            b->setColour (juce::TextButton::textColourOffId, theme::text1);
            addChildComponent (*b);
        }

        laneControls.push_back (std::move (controls));
    }

    for (int lane = 0; lane < tracks.size(); ++lane)
    {
        auto& controls = *laneControls[(size_t) lane];
        auto* track = tracks[lane];

        controls.mute.onClick = [track, b = &controls.mute] { track->setMute (b->getToggleState()); };
        controls.solo.onClick = [track, b = &controls.solo] { track->setSolo (b->getToggleState()); };
        controls.mute.setToggleState (track->isMuted (false), juce::dontSendNotification);
        controls.solo.setToggleState (track->isSolo (false), juce::dontSendNotification);

        auto lane0 = getLaneBounds (lane);
        auto y = lane0.getY() + juce::jmin (30, lane0.getHeight() - 24);
        controls.mute.setBounds (6, y, 30, 20);    // the same two columns as the loop buttons below
        controls.solo.setBounds (40, y, 30, 20);

        // Quick loops: all the buttons when there's room, else one LOOP button with them as a menu.
        auto full = lane0.getHeight() >= fullLoopToolsHeight, menu = ! full && lane0.getHeight() >= loopMenuHeight;

        // Two even columns: 1 2 / 4 8 / In Out, all buttons the same width.
        for (int i = 0; i < numQuickLoops; ++i)
        {
            controls.bars[i].setVisible (full);
            controls.bars[i].setBounds (6 + (i % 2) * 34, lane0.getY() + 76 + (i / 2) * 22, 30, 20);
        }

        controls.loopIn.setVisible (full);
        controls.loopOut.setVisible (full);
        controls.loopIn.setBounds (6, lane0.getY() + 120, 30, 20);
        controls.loopOut.setBounds (40, lane0.getY() + 120, 30, 20);
        controls.loopIn.setToggleState (pendingLoopIn && pendingLoopIn->lane == lane, juce::dontSendNotification);
        controls.loopMenu.setVisible (menu);
        controls.loopMenu.setBounds (6, lane0.getY() + 56, 64, 20);
        controls.loopMenu.setToggleState (pendingLoopIn && pendingLoopIn->lane == lane, juce::dontSendNotification);
    }
}

void TimelineComponent::paintRuler (juce::Graphics& g)
{
    g.setColour (theme::raised);
    g.fillRect (0, 0, getWidth(), rulerHeight);
    g.setColour (theme::line);
    g.fillRect (0, rulerHeight - 1, getWidth(), 1);

    // Label every Nth bar, with N chosen so labels don't collide.
    auto pixelsPerBar = getPixelsPerBeat() * beatsPerBar;
    int barStep = 1;

    while (barStep * pixelsPerBar < 48.0 && barStep < 4096)
        barStep *= 2;

    auto firstBeat = edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (viewStart)).inBeats();
    auto lastBeat = edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (xToTime (getWidth()))).inBeats();
    auto firstBar = (int) std::floor (firstBeat / beatsPerBar / barStep) * barStep;

    // Beat ticks when there's room for them.
    if (getPixelsPerBeat() >= 6.0)
    {
        g.setColour (theme::text3.withAlpha (0.5f));

        for (auto beat = std::floor (firstBeat); beat <= lastBeat; beat += 1.0)
            g.fillRect ((float) (int) xForBeat (beat), (float) rulerHeight - 5.0f, 1.0f, 4.0f);
    }

    // Numbers in the upper part; the playhead's marker sits in the lower part, so it never covers one.
    g.setFont (theme::font (theme::textSmall));

    for (int bar = firstBar; bar * beatsPerBar <= lastBeat; bar += barStep)
    {
        auto x = (int) xForBeat (bar * beatsPerBar);
        g.setColour (theme::text3);
        g.fillRect ((float) x, (float) rulerHeight - 10.0f, 1.0f, 9.0f);
        g.setColour (theme::text2);
        g.drawText (juce::String (bar + 1), x + 4, 1, 60, 16, juce::Justification::left);
    }

    // Zoomed in so far that a bar is wider than the view and its number is off to the left: show
    // where the view is (bar.beat) at the left edge, unless the next bar's number is about to show.
    if (pixelsPerBar > getWidth() - headerWidth && xForBeat (std::floor (firstBeat / beatsPerBar) * beatsPerBar) < headerWidth
         && xForBeat ((std::floor (firstBeat / beatsPerBar) + 1.0) * beatsPerBar) > headerWidth + 70)
    {
        auto bar = (int) std::floor (firstBeat / beatsPerBar);
        auto beat = (int) std::floor (firstBeat - bar * beatsPerBar);
        g.setColour (theme::text2);
        g.drawText (juce::String (bar + 1) + "." + juce::String (beat + 1), headerWidth + 4, 1, 60, 16, juce::Justification::left);
    }
}

void TimelineComponent::paintGrid (juce::Graphics& g)
{
    // Bar lines (and beat lines when zoomed in) behind the clips.
    auto top = (float) getLanesTop(), bottom = (float) (getHeight() - scrollBarHeight);
    auto pixelsPerBeat = getPixelsPerBeat();
    auto firstBeat = std::floor (edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (viewStart)).inBeats());
    auto lastBeat = edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (xToTime (getWidth()))).inBeats();

    int beatStep = pixelsPerBeat >= 12.0 ? 1 : beatsPerBar;

    while (beatStep * pixelsPerBeat < 12.0 && beatStep < 65536)
        beatStep *= 2;

    for (auto beat = std::floor (firstBeat / beatStep) * beatStep; beat <= lastBeat; beat += beatStep)
    {
        auto isBar = std::fmod (beat, (double) beatsPerBar) == 0.0;
        g.setColour (theme::grid.withAlpha (isBar ? 1.0f : 0.5f));
        g.drawVerticalLine ((int) xForBeat (beat), top, bottom);
    }
}

void TimelineComponent::paintLanes (juce::Graphics& g)
{
    auto tracks = te::getAudioTracks (edit);

    for (int lane = 0; lane < tracks.size(); ++lane)
    {
        auto bounds = getLaneBounds (lane);
        g.setColour (lane % 2 == 0 ? theme::laneA : theme::laneB);
        g.fillRect (bounds);

        if (lane == dropHighlightLane)
        {
            g.setColour (theme::accent.withAlpha (0.1f));
            g.fillRect (bounds);
        }

        g.setColour (theme::line.withAlpha (0.6f));
        g.fillRect (bounds.getX(), bounds.getBottom() - 1, bounds.getWidth(), 1);
    }

    paintGrid (g);
    pieceLabels = mixtopia::chops::listPieces (edit);

    for (int lane = 0; lane < tracks.size(); ++lane)
    {
        for (auto* clip : tracks[lane]->getClips())
        {
            auto colour = mixtopia::clipcolours::get (*clip);
            auto bounds = getClipBounds (*clip);

            if (! bounds.intersects (g.getClipBounds()))
                continue;

            if (auto* waveClip = dynamic_cast<te::WaveAudioClip*> (clip))
                paintClip (g, *waveClip, bounds, mixtopia::isLaneAudible (*tracks[lane]) ? colour : colour.withSaturation (0.1f).withAlpha (0.45f));
        }
    }
}

void TimelineComponent::paintClip (juce::Graphics& g, te::WaveAudioClip& clip,
                                   juce::Rectangle<int> bounds, juce::Colour colour)
{
    // A song: its dark tint, a title bar (in the song's colour when selected - no white border),
    // the loudness shape, the beat rail along the bottom, the lines, and a thin outline.
    auto isSelected = isPieceSelected (clip, -1);
    juce::Path outline;
    outline.addRoundedRectangle (bounds.toFloat(), 4.0f);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (outline);

        g.setColour (theme::tint (colour));
        g.fillRect (bounds);

        // Only draw the part of the waveform that's on screen. The MixMeister-style shape once the
        // song is analysed; until then the plain peak waveform.
        // (Mapped to the song itself: a looped song's waveform repeats with it.)
        auto loop = mixtopia::loops::getLoop (clip);
        auto editToSong = [&] (double editSeconds) { return mixtopia::loops::playedToSong (loop, tempo::editToSourceSeconds (clip, editSeconds)); };
        auto toSourceTime = [&] (int x) { return editToSong (xToTime (x)); };

        // The shape is drawn from slices one pixel wide, counted from where the song's audio begins (its content
        // start), so they belong to the song: scrolling, moving, trimming or cutting it only changes which column shows
        // which slice (user, 2026-11-10: waveforms "wiggled" while scrolling - each column took a slightly different
        // part of the song whenever the view moved by part of a pixel; the same when a song was moved). The slice in
        // the first column is worked out once, so rounding can't make neighbouring columns disagree. Zooming makes new
        // slices, so it redraws the shape.
        auto contentStart = tempo::getContentStart (clip).inSeconds();
        auto firstSlice = (viewStart - contentStart) * pixelsPerSecond - headerWidth;   // (slice under column x: floor (firstSlice + x + 0.5))
        auto shapeArea = bounds.withTrimmedTop (clipTitleHeight + 2).withTrimmedBottom (beatRailHeight)
                             .getIntersection (getLocalBounds().withTrimmedLeft (headerWidth));

        if (auto* overview = overviews.get (mixtopia::loops::getSongFile (clip)); overview != nullptr && ! shapeArea.isEmpty())
        {
            paintLoudnessShape (g, *overview, shapeArea, firstSlice, contentStart, editToSong, colour, mixtopia::loops::getSongFile (clip));
        }
        else
        {
            auto visible = shapeArea;
            auto& thumb = getThumbnail (mixtopia::loops::getSongFile (clip));   // the decoded copy for MP3/FLAC: quick to scan

            if (thumb.getNumChannels() == 0)
            {
                g.setColour (theme::text2);
                g.setFont (theme::font (theme::textSmall));
                g.drawText ("Loading waveform...", visible, juce::Justification::centred);
            }
            else if (! visible.isEmpty())
            {
                g.setColour (colour);
                thumb.drawChannels (g, visible, toSourceTime (visible.getX()), toSourceTime (visible.getRight()), 1.0f);
            }
        }

        paintClipBeats (g, clip, bounds);
        paintLoop (g, clip, bounds, colour);

        // A loop-in waiting for its loop-out: a teal flag where it starts.
        if (pendingLoopIn && pendingLoopIn->clip == clip.itemID)
        {
            auto x = (float) timeToX (tempo::sourceToEditSeconds (clip, mixtopia::loops::songToPlayed (mixtopia::loops::getLoop (clip), pendingLoopIn->song)));
            auto top = (float) bounds.getY() + (float) clipTitleHeight;
            g.setColour (theme::accent);
            g.fillRect (x - 1.0f, top, 2.0f, (float) bounds.getBottom() - top);
            juce::Path flag;
            flag.startNewSubPath (x, top);
            flag.lineTo (x + 14.0f, top);
            flag.lineTo (x + 9.0f, top + 6.0f);
            flag.lineTo (x + 14.0f, top + 12.0f);
            flag.lineTo (x, top + 12.0f);
            flag.closeSubPath();
            g.fillPath (flag);
            g.setFont (theme::font (theme::textTiny, true));
            g.drawText ("in " + mixtopia::loops::formatSongTime (pendingLoopIn->song), (int) x + 18, (int) top, 120, 13, juce::Justification::centredLeft);
        }

        // Title bar. A chopped song whose pieces still touch: each piece has its own title, the
        // selected one in the song's colour, a dashed line and a notch at each cut.
        auto title = bounds.withHeight (clipTitleHeight);
        auto cuts = mixtopia::chops::getCuts (clip);
        auto selection = getSelection();
        auto wholeSelected = std::any_of (selection.begin(), selection.end(), [&] (const PieceRef& r) { return r.clip == clip.itemID && r.piece < 0; });
        auto pieceSelected = isSelected && ! cuts.empty() && ! wholeSelected;   // (some of its pieces)
        auto titleRight = cuts.empty() ? title.getRight() : (int) timeToX (tempo::sourceToEditSeconds (clip, cuts.front()));

        g.setColour (isSelected && ! pieceSelected ? colour : juce::Colours::black.withAlpha (0.28f));
        g.fillRect (title);

        if (pieceSelected)
        {
            g.setColour (colour);

            for (int i = 0; i <= (int) cuts.size(); ++i)
                if (isPieceSelected (clip, i))
                {
                    auto range = mixtopia::chops::getPieceEditRange (clip, i);
                    g.fillRect (title.withLeft ((int) timeToX (range.getStart())).withRight ((int) timeToX (range.getEnd())));
                }

            isSelected = isPieceSelected (clip, 0);   // (the first piece's title and tags below)
        }

        for (size_t i = 0; i < cuts.size(); ++i)
        {
            auto x = (float) timeToX (tempo::sourceToEditSeconds (clip, cuts[i]));
            auto piece = (int) i + 1;
            auto selectedHere = (pieceSelected && isPieceSelected (clip, piece)) || (wholeSelected);
            auto pieceEnd = i + 1 < cuts.size() ? (int) timeToX (tempo::sourceToEditSeconds (clip, cuts[i + 1])) : title.getRight();

            g.setColour (juce::Colours::white.withAlpha (0.8f));
            juce::Path line;
            line.startNewSubPath (x, (float) title.getBottom());
            line.lineTo (x, (float) (bounds.getBottom() - beatRailHeight));
            juce::Path dashed;
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, line, dashes, 2);
            g.fillPath (dashed);

            juce::Path notch;
            notch.addTriangle (x - 5.0f, (float) title.getY(), x + 5.0f, (float) title.getY(), x, (float) title.getY() + 7.0f);
            g.setColour (juce::Colours::white);
            g.fillPath (notch);

            auto text = mixtopia::chops::labelFor (pieceLabels, clip, piece) + "  " + mixtopia::shortTitle (clip.getName());
            g.setColour (selectedHere ? theme::darkText : theme::text1);
            g.setFont (theme::font (theme::textSmall, true));
            g.drawText (text, juce::Rectangle<int> ((int) x + 9, title.getY(), pieceEnd - (int) x - 12, title.getHeight()),
                        juce::Justification::centredLeft, true);
        }

        // Edges that came from a cut fade in / out over a few ms: small wedges at the top.
        auto wedge = [&] (float x, bool atStart)
        {
            juce::Path p;
            auto top = (float) title.getBottom() + 1.0f;
            p.addTriangle (x, top, x + (atStart ? 7.0f : -7.0f), top, x, top + 10.0f);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.fillPath (p);
        };

        if (mixtopia::chops::hasFadeIn (clip))
            wedge ((float) bounds.getX() + 1.0f, true);

        if (mixtopia::chops::hasFadeOut (clip))
            wedge ((float) bounds.getRight() - 1.0f, false);

        auto bpm = tempo::getClipBpm (clip);
        auto key = clip.state[mixtopia::analysis::keyProperty].toString();
        auto dot = juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
        auto pieceLabel = mixtopia::chops::isPiece (clip) ? mixtopia::chops::labelFor (pieceLabels, clip, 0) : juce::String();
        auto label = (pieceLabel.isNotEmpty() ? pieceLabel + "  " : juce::String()) + clip.getName()
                       + (bpm > 0.0 ? dot + tempo::formatBpm (bpm) + " BPM" : dot + "no BPM set")
                       + (key.isNotEmpty() ? dot + key : juce::String());

        // Keep the name readable when the clip starts off-screen.
        auto labelArea = title.withRight (titleRight).getIntersection (getLocalBounds().withTrimmedLeft (headerWidth)).withTrimmedLeft (9);
        auto font = theme::font (theme::textSmall, true);
        auto textColour = isSelected ? theme::darkText : theme::text1;
        g.setFont (font);
        g.setColour (textColour);
        g.drawText (label, labelArea, juce::Justification::centredLeft, true);

        // Tags (grid corrected by hand, beat 1 off the bar lines, level raise, big stretch) as small pills.
        auto tagX = (float) labelArea.getX() + juce::GlyphArrangement::getStringWidth (font, label) + 10.0f;
        auto tagFont = theme::font (theme::textTiny, true);

        auto drawTag = [&] (const juce::String& text, juce::Colour tagColour, bool filled)
        {
            auto c = isSelected ? theme::darkText : tagColour;
            auto pill = juce::Rectangle<float> (tagX, (float) title.getY() + 2.0f, juce::GlyphArrangement::getStringWidth (tagFont, text) + 14.0f, 16.0f);

            if (pill.getRight() > (float) titleRight - 4.0f)
                return;

            if (filled && ! isSelected)
            {
                g.setColour (c.withAlpha (0.18f));
                g.fillRoundedRectangle (pill, 3.0f);
            }
            else
            {
                g.setColour (c.withAlpha (0.65f));
                g.drawRoundedRectangle (pill.reduced (0.5f), 3.0f, 1.0f);
            }

            g.setColour (c);
            g.setFont (tagFont);
            g.drawText (text, pill, juce::Justification::centred);
            tagX = pill.getRight() + 6.0f;
        };

        if (auto loop = mixtopia::loops::getLoop (clip))
            drawTag ("loop  " + mixtopia::loops::describe (clip, *loop).fromFirstOccurrenceOf ("  ", false, false).trim(), theme::accent, false);

        if (bpm > 0.0 && clip.state[tempo::gridSourceProperty].toString() == "adjusted")
            drawTag ("grid adjusted", theme::solo, false);

        // (Only while Snap is on - with Snap off a song off the bar lines is what's wanted; user's option C.)
        if (bpm > 0.0 && snapEnabled && ! isOnGrid (clip))
            drawTag ("off grid", theme::warn, false);

        if (auto raise = mixtopia::levelmatch::getAppliedGainDb (clip); raise > 0.05)
            drawTag ("+" + juce::String (raise, 1) + " dB", theme::good, true);

        if (auto stretch = tempo::getMaxStretchPercent (clip); stretch > tempo::noticeableStretchPercent)
            drawTag ("stretched " + juce::String (juce::roundToInt (stretch)) + "%", juce::Colour (0xffff9f43), false);

    }

    auto selected = isPieceSelected (clip, -1);
    auto inside = selected ? 1.0f : 0.5f;
    g.setColour (colour.withAlpha (selected ? 1.0f : 0.55f));
    g.drawRoundedRectangle (bounds.toFloat().reduced (inside), 4.0f - inside, inside * 2.0f);

    // Selected pieces of a chopped song that still touches the others: each outlined on its own.
    auto cutsHere = mixtopia::chops::getCuts (clip);
    auto selection = getSelection();

    if (selected && ! cutsHere.empty()
         && ! std::any_of (selection.begin(), selection.end(), [&] (const PieceRef& r) { return r.clip == clip.itemID && r.piece < 0; }))
        for (int i = 0; i <= (int) cutsHere.size(); ++i)
            if (isPieceSelected (clip, i))
            {
                auto range = mixtopia::chops::getPieceEditRange (clip, i);
                auto r = bounds.toFloat().withLeft ((float) timeToX (range.getStart())).withRight ((float) timeToX (range.getEnd()));
                g.setColour (colour);
                g.drawRect (r.reduced (1.0f), 2.0f);
            }

    // The lines and their points on top, not clipped by the rounded corners (a point on an edge).
    paintEnvelopes (g, clip, bounds);
}

void TimelineComponent::paintLoudnessShape (juce::Graphics& g, const mixtopia::LoudnessOverview& overview,
                                            juce::Rectangle<int> shapeArea, double firstSlice, double contentStart,
                                            const std::function<double (double)>& editToSong, juce::Colour colour, const juce::File& songFile)
{
    // One shape growing up from the bottom: the song's loudest moments reach 82% of the height,
    // 18 dB quieter is the bottom. Zoomed out it shows the average loudness (the song's flow); at
    // medium zoom it fades over to the low end's peaks (a block per kick or bass note); at maximum
    // zoom to the whole sound's peaks on a straight scale, where a kick's click is a spike on the
    // hit (the low end only builds up 40-50 ms later).
    constexpr float rangeDb = 18.0f, topFraction = 0.82f;
    auto secondsPerPixel = 1.0 / pixelsPerSecond;
    auto fadeIn = [secondsPerPixel] (double from, double to)   // 0 at `from`, 1 at `to` seconds per pixel
    {
        return (float) juce::jlimit (0.0, 1.0, (from - secondsPerPixel) / (from - to));
    };
    auto peakWeight = fadeIn (0.015, 0.005);
    auto fullWeight = fadeIn (0.0025, 0.001);

    // Closer than the stored peaks (one per 64 samples, ~1.5 ms): the same peaks read exactly from
    // the song's audio, down to a couple of samples per pixel. (Stretched songs: this is the song's
    // own audio placed in time - the stretcher rebuilds the sound, so fine detail can differ.)
    auto* samples = fullWeight >= 1.0f ? getSampleReader (songFile) : nullptr;

    if (samples != nullptr && secondsPerPixel * samples->sampleRate >= mixtopia::LoudnessOverview::fullPeakBlockSize)
        samples = nullptr;
    auto bottom = (float) shapeArea.getBottom();
    auto height = (float) shapeArea.getHeight() * topFraction;
    auto toFraction = [&] (float level)
    {
        auto db = level > 0.0f ? juce::Decibels::gainToDecibels (level) : -100.0f;
        return juce::jlimit (0.0f, 1.0f, (db + rangeDb) / rangeDb);
    };

    // Only the columns being repainted (plus a little either side so the outline joins up the same
    // as in a full repaint): the moving playhead repaints a narrow strip on every frame.
    auto columns = shapeArea.getIntersection (g.getClipBounds().expanded (2, 0));

    if (columns.isEmpty())
        return;

    juce::Path shape, outline;
    shape.startNewSubPath ((float) columns.getX(), bottom);

    for (int x = columns.getX(); x < columns.getRight(); ++x)
    {
        // The slice under the column's centre (slices belong to the song - see paintClip).
        auto slice = std::floor (firstSlice + x + 0.5);
        auto start = editToSong (contentStart + slice * secondsPerPixel), end = editToSong (contentStart + (slice + 1.0) * secondsPerPixel);
        auto fraction = peakWeight < 1.0f ? toFraction (overview.getRelativeLevel (start, end)) : 0.0f;

        if (peakWeight > 0.0f)
            fraction += peakWeight * (toFraction (overview.getRelativeBassPeak (start, end)) - fraction);

        if (fullWeight > 0.0f)
        {
            auto peak = samples != nullptr ? overview.getRelativeFullPeak (*samples, juce::jmin (start, end), juce::jmax (start, end))
                                           : overview.getRelativeFullPeak (start, end);
            fraction += fullWeight * (juce::jlimit (0.0f, 1.0f, peak) - fraction);
        }

        auto y = bottom - fraction * height;
        shape.lineTo ((float) x, y);
        shape.lineTo ((float) x + 1.0f, y);

        if (x == columns.getX())
            outline.startNewSubPath ((float) x, y);
        else
            outline.lineTo ((float) x, y);

        outline.lineTo ((float) x + 1.0f, y);
    }

    shape.lineTo ((float) columns.getRight(), bottom);
    shape.closeSubPath();
    g.setColour (colour);
    g.fillPath (shape);
    g.setColour (colour.brighter (0.5f));
    g.strokePath (outline, juce::PathStrokeType (1.0f));
}

void TimelineComponent::paintClipBeats (juce::Graphics& g, te::WaveAudioClip& clip, juce::Rectangle<int> bounds)
{
    // The beat rail (M8, user-approved): a dark strip along the bottom of the song with ticks at the
    // song's own beats - so they show the same on any song colour and in loud and quiet parts. Bar
    // starts: white, full height, 2 px; beats: half height in a light shade of the song's colour
    // (hidden when they'd crowd together; bar ticks stay longer). The song's beat 1 gets a small
    // marker. Zoomed in, faint lines go up through the waveform at the bar starts (View > Bar Lines
    // on Songs), so you can see whether a kick sits on them.
    auto rail = bounds.removeFromBottom (beatRailHeight);
    auto colour = mixtopia::clipcolours::get (clip);
    g.setColour (theme::bg.interpolatedWith (colour, 0.1f));
    g.fillRect (rail);

    auto bpm = tempo::getClipBpm (clip);

    if (bpm <= 0.0)
        return;

    auto beatLength = 60.0 / bpm;
    auto beatPixels = beatLength * tempo::getStretchRatio (clip) * pixelsPerSecond;
    auto showBeats = beatPixels >= 6.0;
    auto showBarLines = barLinesOnSongs && beatPixels >= 40.0;

    if (beatPixels * beatsPerBar < 6.0)
        return;

    auto visible = rail.getIntersection (getLocalBounds().withTrimmedLeft (headerWidth));
    auto loop = mixtopia::loops::getLoop (clip);
    auto songDownbeat = mixtopia::loops::playedToSong (loop, tempo::getDownbeat (clip));
    auto phase = tempo::beatPhase (bpm, songDownbeat);
    auto downbeatIndex = std::llround ((songDownbeat - phase) / beatLength);
    auto firstSource = tempo::editToSourceSeconds (clip, xToTime (visible.getX() - 4));
    auto lastSource = tempo::editToSourceSeconds (clip, xToTime (visible.getRight() + 4));
    auto waveTop = (float) bounds.getY() + (float) clipTitleHeight, waveBottom = (float) rail.getY();
    auto beatColour = colour.interpolatedWith (juce::Colours::white, 0.45f);

    // The song's own beats, wherever the song plays them: with a loop, the loop's beats again in each
    // repeat and the rest after it - so after a loop that isn't a whole number of beats the ticks
    // sit where the kicks really are, not on the grid continued.
    struct Segment { double songFrom, songTo, offset; bool first; };
    std::vector<Segment> segments;

    if (loop)
    {
        segments.push_back ({ -1.0e9, loop->end, 0.0, true });

        for (int pass = 1; pass <= loop->repeats; ++pass)
            segments.push_back ({ loop->start, loop->end, loop->end + (pass - 1) * loop->length() - loop->start, false });

        segments.push_back ({ loop->end, 1.0e9, loop->added(), true });
    }
    else
    {
        segments.push_back ({ -1.0e9, 1.0e9, 0.0, true });
    }

    if (loop)
    {
        // Under the loop: a teal line along the rail.
        auto x0 = (float) timeToX (tempo::sourceToEditSeconds (clip, loop->start));
        auto x1 = (float) timeToX (tempo::sourceToEditSeconds (clip, loop->end + loop->added()));
        g.setColour (theme::accent);
        g.fillRect (x0, (float) rail.getY(), x1 - x0, 2.0f);
    }

    for (auto& segment : segments)
    {
        auto from = juce::jmax (segment.songFrom, firstSource - segment.offset), to = juce::jmin (segment.songTo, lastSource - segment.offset);

        for (auto k = (juce::int64) std::ceil ((from - phase) / beatLength - 1.0e-9); phase + k * beatLength < to; ++k)
        {
            auto x = (float) std::round (timeToX (tempo::sourceToEditSeconds (clip, phase + k * beatLength + segment.offset)));
            auto isBar = ((k - downbeatIndex) % beatsPerBar + beatsPerBar) % beatsPerBar == 0;

            if (isBar)
            {
                if (showBarLines)
                {
                    // Two tones side by side, so the line shows on bright and on dark parts of the shape.
                    g.setColour (juce::Colours::black.withAlpha (barLineDarkAlpha));
                    g.fillRect (x + 1.0f, waveTop, 1.0f, waveBottom - waveTop);
                    g.setColour (juce::Colours::white.withAlpha (barLineLightAlpha));
                    g.fillRect (x, waveTop, 1.0f, waveBottom - waveTop);
                }

                // (Zoomed out, where bars crowd together, thinner and softer so the rail isn't a comb.)
                auto crowded = beatPixels * beatsPerBar < 24.0;
                g.setColour (juce::Colours::white.withAlpha (crowded ? 0.55f : 1.0f));
                g.fillRect (crowded ? x : x - 0.5f, (float) rail.getY(), crowded ? 1.0f : 2.0f, (float) rail.getHeight());

                if (k == downbeatIndex && segment.first)
                {
                    // Beat 1: a small marker standing on the rail.
                    juce::Path marker;
                    marker.addTriangle (x - 4.5f, waveBottom - 6.0f, x + 5.5f, waveBottom - 6.0f, x + 0.5f, waveBottom);
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    g.strokePath (marker, juce::PathStrokeType (1.5f));
                    g.setColour (juce::Colours::white);
                    g.fillPath (marker);
                }
            }
            else if (showBeats)
            {
                g.setColour (beatColour);
                g.fillRect (x, (float) rail.getBottom() - (float) rail.getHeight() * 0.55f, 1.0f, (float) rail.getHeight() * 0.55f);
            }
        }
    }
}

juce::Rectangle<int> TimelineComponent::getEnvelopeArea (juce::Rectangle<int> clipBounds)
{
    return clipBounds.withTrimmedTop (clipTitleHeight).withTrimmedBottom (beatRailHeight).reduced (0, 3);
}

float TimelineComponent::valueToY (envelopes::Param param, float value, juce::Rectangle<int> area)
{
    if (param == envelopes::Param::filter)
        return area.toFloat().getCentreY() - value / envelopes::filterMax * (float) area.getHeight() * 0.5f;

    auto fraction = std::sqrt (envelopes::toGain (value) / envelopes::toGain (envelopes::maxDb));
    return (float) area.getBottom() - fraction * (float) area.getHeight();
}

float TimelineComponent::yToValue (envelopes::Param param, float y, juce::Rectangle<int> area)
{
    if (param == envelopes::Param::filter)
        return juce::jlimit (envelopes::filterMin, envelopes::filterMax,
                             (area.toFloat().getCentreY() - y) / ((float) juce::jmax (1, area.getHeight()) * 0.5f) * envelopes::filterMax);

    auto fraction = juce::jlimit (0.0f, 1.0f, ((float) area.getBottom() - y) / (float) juce::jmax (1, area.getHeight()));
    auto gain = fraction * fraction * envelopes::toGain (envelopes::maxDb);
    return gain <= 0.0f ? envelopes::killDb : juce::jlimit (envelopes::killDb, envelopes::maxDb, juce::Decibels::gainToDecibels (gain));
}

std::optional<TimelineComponent::PointHit> TimelineComponent::pointAt (te::WaveAudioClip& clip, juce::Point<int> p) const
{
    auto area = getEnvelopeArea (getClipBounds (clip));
    std::optional<PointHit> best;
    auto bestDistance = 7.0f;

    // Later lines are drawn on top, so on a tie they win.
    for (int param = 0; param < envelopes::numParams; ++param)
    {
        auto points = envelopes::getPoints (clip.state, (envelopes::Param) param);

        for (int i = 0; i < (int) points.size(); ++i)
        {
            if (isHiddenByLoop (clip, points[(size_t) i].sourceTime))
                continue;

            auto x = (float) timeToX (tempo::sourceToEditSeconds (clip, points[(size_t) i].sourceTime));
            auto y = valueToY ((envelopes::Param) param, points[(size_t) i].db, area);
            auto distance = juce::Point<float> (x, y).getDistanceFrom (p.toFloat());

            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = PointHit { (envelopes::Param) param, i };
            }
        }
    }

    return best;
}

void TimelineComponent::addPoint (te::WaveAudioClip& clip, envelopes::Param param, juce::Point<int> p)
{
    // Exactly where the mouse is in time (points ignore snap), kept inside the clip.
    auto pos = clip.getPosition();
    auto editTime = juce::jlimit (pos.getStart().inSeconds(), pos.getEnd().inSeconds(), xToTime (p.x));
    auto shown = tempo::getSourceRange (clip);   // the part of the song the clip plays (not trimmed away)
    auto source = juce::jlimit (shown.start, shown.end, tempo::editToSourceSeconds (clip, editTime));
    auto loop = mixtopia::loops::getLoop (clip);
    auto points = envelopes::getPoints (clip.state, param);

    // Its value is what its own line already has there (user, 2026-10-27: nothing may change until
    // the point is dragged), not the mouse height: 0 dB on an empty line, the held value before the
    // first / after the last point, and on the straight line between two points.
    auto db = mixtopia::loops::newPointValue (points, loop, source);

    // In a repeat whose lines copy the first pass: the point goes into the first pass (and so into
    // every repeat).
    if (loop && loop->copyLines)
    {
        if (auto place = mixtopia::loops::playedToPlace (loop, source); place.pass > 0)
        {
            source = place.song;

            if (onStatusMessage)
                onStatusMessage ("The lines copy the loop's first pass into every repeat, so the point went into the first pass.");
        }
    }

    // A point already at that moment stays as it is (it's just selected below).
    auto existing = std::find_if (points.begin(), points.end(), [&] (auto& point) { return std::abs (point.sourceTime - source) < 0.001; });

    if (existing == points.end())
    {
        points.push_back ({ source, db });
        edit.getUndoManager().beginNewTransaction ("Add " + envelopes::getName (param).toLowerCase() + " point");
        envelopes::setPoints (clip.state, param, points, &edit.getUndoManager());
    }

    // Highlight the new point so its value shows straight away.
    points = envelopes::getPoints (clip.state, param);

    for (int i = 0; i < (int) points.size(); ++i)
        if (std::abs (points[(size_t) i].sourceTime - source) < 0.001)
            setHover (clip.itemID, PointHit { param, i });

    selectClip (clip.itemID);
}

bool TimelineComponent::addPointUnderMouse (envelopes::Param param)
{
    auto p = getMouseXYRelative();
    auto* clip = getLocalBounds().contains (p) ? dynamic_cast<te::WaveAudioClip*> (clipAt (p)) : nullptr;

    if (clip == nullptr)
        return false;

    addPoint (*clip, param, p);
    return true;
}

void TimelineComponent::setHover (te::EditItemID clipID, std::optional<PointHit> point)
{
    if (! point)
        clipID = {};

    if (clipID == hoverClipID && point == hoverPoint)
        return;

    hoverClipID = clipID;
    hoverPoint = point;
    repaint();
}

void TimelineComponent::paintEnvelopes (juce::Graphics& g, te::WaveAudioClip& clip, juce::Rectangle<int> bounds)
{
    auto area = getEnvelopeArea (bounds);
    auto visible = bounds.getIntersection (getLocalBounds().withTrimmedLeft (headerWidth));

    if (visible.isEmpty())
        return;

    auto leftSource = tempo::editToSourceSeconds (clip, xToTime (visible.getX()));
    auto rightSource = tempo::editToSourceSeconds (clip, xToTime (visible.getRight()));
    auto edgeLeeway = 3.0 / pixelsPerSecond;
    auto highlighted = clip.itemID == hoverClipID ? hoverPoint : std::nullopt;
    auto loop = mixtopia::loops::getLoop (clip);

    if (dragMode == DragMode::envelopePoint && clip.itemID == dragClipID)
        highlighted = PointHit { dragPointParam, dragPointIndex };

    // Every line that has points, each in its own colour; then the points on top; then the
    // highlighted point's name and value.
    std::array<std::vector<envelopes::Point>, envelopes::numParams> lines;

    for (int p = 0; p < envelopes::numParams; ++p)
    {
        auto param = (envelopes::Param) p;
        lines[(size_t) p] = envelopes::getPoints (clip.state, param);

        // The line as it plays (with a loop that copies its first pass, the repeats' copies too).
        auto points = mixtopia::loops::effectivePoints (lines[(size_t) p], loop);

        if (points.empty())
            continue;

        juce::Path path;
        path.startNewSubPath ((float) visible.getX(), valueToY (param, envelopes::evaluate (points, leftSource), area));

        for (auto& point : points)
            if (point.sourceTime > leftSource && point.sourceTime < rightSource)
                path.lineTo ((float) timeToX (tempo::sourceToEditSeconds (clip, point.sourceTime)), valueToY (param, point.db, area));

        path.lineTo ((float) visible.getRight(), valueToY (param, envelopes::evaluate (points, rightSource), area));

        if (param == envelopes::Param::filter)
        {
            // The filter line is the dashed one (mockup 23): it reads apart from the others on any
            // song colour. "Off" is the middle: a faint dotted guide, with HP / LP at the left edge.
            auto midY = area.toFloat().getCentreY();
            juce::Path guide, dashedGuide;
            guide.startNewSubPath ((float) visible.getX(), midY);
            guide.lineTo ((float) visible.getRight(), midY);
            const float dots[] { 2.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashedGuide, guide, dots, 2);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.fillPath (dashedGuide);

            g.setFont (theme::font (theme::textTiny, true));

            for (auto [label, top] : { std::pair<const char*, bool> { "HP", true }, { "LP", false } })
            {
                auto mark = juce::Rectangle<float> ((float) visible.getX() + 4.0f, top ? (float) area.getY() + 1.0f : (float) area.getBottom() - 15.0f, 22.0f, 14.0f);
                g.setColour (juce::Colours::black.withAlpha (0.65f));
                g.fillRoundedRectangle (mark, 3.0f);
                g.setColour (envelopes::getColour (param));
                g.drawText (label, mark, juce::Justification::centred);
            }

            const float dashes[] { 9.0f, 5.0f };
            juce::Path edge, line;
            juce::PathStrokeType (3.75f).createDashedStroke (edge, path, dashes, 2);
            juce::PathStrokeType (1.75f).createDashedStroke (line, path, dashes, 2);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillPath (edge);
            g.setColour (envelopes::getColour (param));
            g.fillPath (line);
            continue;
        }

        g.setColour (juce::Colours::black.withAlpha (0.6f));   // dark edge: readable on any lane colour
        g.strokePath (path, juce::PathStrokeType (3.75f));
        g.setColour (envelopes::getColour (param));
        g.strokePath (path, juce::PathStrokeType (1.75f));
    }

    std::optional<juce::Point<float>> labelAt;

    // The repeats' copies of the first pass's points: small hollow dots (edit them in the first pass).
    if (loop && loop->copyLines)
        for (int p = 0; p < envelopes::numParams; ++p)
            for (auto& point : mixtopia::loops::effectivePoints (lines[(size_t) p], loop))
                if (point.sourceTime >= leftSource && point.sourceTime <= rightSource
                     && mixtopia::loops::playedToPlace (loop, point.sourceTime).pass > 0)
                {
                    auto x = (float) timeToX (tempo::sourceToEditSeconds (clip, point.sourceTime));
                    auto y = valueToY ((envelopes::Param) p, point.db, area);
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    g.fillEllipse (x - 3.0f, y - 3.0f, 6.0f, 6.0f);
                    g.setColour (envelopes::getColour ((envelopes::Param) p));
                    g.drawEllipse (x - 3.0f, y - 3.0f, 6.0f, 6.0f, 1.2f);
                }

    for (int p = 0; p < envelopes::numParams; ++p)
    {
        auto& points = lines[(size_t) p];

        for (int i = 0; i < (int) points.size(); ++i)
        {
            auto& point = points[(size_t) i];

            // (A few pixels of leeway: points pushed to a trimmed edge sit right on it, and the
            // clip's on-screen edges are rounded to whole pixels.)
            if (point.sourceTime < leftSource - edgeLeeway || point.sourceTime > rightSource + edgeLeeway
                 || isHiddenByLoop (clip, point.sourceTime))
                continue;

            auto isHighlighted = highlighted == PointHit { (envelopes::Param) p, i };
            auto radius = isHighlighted ? 6.0f : 4.0f;
            auto x = (float) timeToX (tempo::sourceToEditSeconds (clip, point.sourceTime));
            auto y = valueToY ((envelopes::Param) p, point.db, area);
            g.setColour (envelopes::getColour ((envelopes::Param) p));
            g.fillEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f);
            g.setColour (isHighlighted ? juce::Colours::white : juce::Colours::black);
            g.drawEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f, isHighlighted ? 2.0f : 1.0f);

            if (isHighlighted)
                labelAt = juce::Point<float> (x, y);
        }
    }

    if (labelAt && highlighted && juce::isPositiveAndBelow (highlighted->index, (int) lines[(size_t) highlighted->param].size()))
    {
        auto db = lines[(size_t) highlighted->param][(size_t) highlighted->index].db;
        auto text = highlighted->param == envelopes::Param::filter ? "Filter: " + envelopes::describeFilter (db)
                  : envelopes::getName (highlighted->param) + " " + (db <= envelopes::killDb + 0.05f ? juce::String ("cut") : juce::String (db, 1) + " dB");
        auto font = theme::font (theme::textNormal);
        auto box = juce::Rectangle<float> (labelAt->x + 12.0f, labelAt->y - 34.0f, juce::GlyphArrangement::getStringWidth (font, text) + 34.0f, 26.0f);

        if (box.getRight() > (float) getWidth())
            box.setX (labelAt->x - 12.0f - box.getWidth());

        if (box.getY() < (float) getLanesTop())
            box.setY (labelAt->y + 10.0f);

        g.setColour (theme::hover);
        g.fillRoundedRectangle (box, theme::controlRadius);
        g.setColour (theme::line.brighter (0.2f));
        g.drawRoundedRectangle (box.reduced (0.5f), theme::controlRadius, 1.0f);
        g.setColour (envelopes::getColour (highlighted->param));
        g.fillEllipse (box.getX() + 10.0f, box.getCentreY() - 4.0f, 8.0f, 8.0f);
        g.setColour (theme::text1);
        g.setFont (font);
        g.drawText (text, box.withTrimmedLeft (24.0f), juce::Justification::centredLeft);
    }
}

juce::AudioFormatReader* TimelineComponent::getSampleReader (const juce::File& songFile)
{
    // Opened once per song (a failure is remembered too, so it isn't retried on every frame).
    auto [it, isNew] = sampleReaders.try_emplace (songFile.getFullPathName());

    if (isNew && songFile.hasFileExtension ("wav"))
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::MemoryMappedAudioFormatReader> reader (wav.createMemoryMappedReader (songFile));

        if (reader != nullptr && reader->mapEntireFile())
            it->second = std::move (reader);
    }

    return it->second.get();
}

juce::AudioThumbnail& TimelineComponent::getThumbnail (const juce::File& sourceFile)
{
    auto& thumb = thumbnails[sourceFile.getFullPathName()];

    if (thumb == nullptr)
    {
        thumb = std::make_unique<juce::AudioThumbnail> (512, edit.engine.getAudioFileFormatManager().readFormatManager,
                                                        thumbnailCache);
        thumb->addChangeListener (this);
        thumb->setSource (new juce::FileInputSource (sourceFile));
    }

    return *thumb;
}

void TimelineComponent::setTempoStripVisible (bool shouldShow)
{
    tempoStrip.setVisible (shouldShow);
    resized();
    repaint();
}

void TimelineComponent::showRulerMenu (juce::Point<int> p)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Change tempo here...");
    auto seconds = juce::jmax (0.0, xToTime (p.x));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ localPointToGlobal (p), localPointToGlobal (p) }),
                        [safeThis = juce::Component::SafePointer<TimelineComponent> (this), seconds] (int result)
    {
        if (result != 1)
            return;

        // Show the strip first, so the new point appears where it's made; the dialog after the menu.
        juce::MessageManager::callAsync ([safeThis, seconds]
        {
            if (safeThis == nullptr)
                return;

            if (safeThis->onTempoStripWanted)
                safeThis->onTempoStripWanted();
            else
                safeThis->setTempoStripVisible (true);

            safeThis->tempoStrip.changeTempoAt (seconds);
        });
    });
}

void TimelineComponent::resized()
{
    tempoStrip.setBounds (0, rulerHeight, getWidth(), TempoStrip::preferredHeight);
    updateLaneControls();
    scrollBar.setBounds (getLocalBounds().removeFromBottom (scrollBarHeight).withTrimmedLeft (headerWidth));
    updateScrollBar();
    updatePlayhead();
}

//==============================================================================
void TimelineComponent::mouseMove (const juce::MouseEvent& e)
{
    auto cursor = juce::MouseCursor::NormalCursor;
    auto* clip = dynamic_cast<te::WaveAudioClip*> (clipAt (e.getPosition()));
    auto point = clip != nullptr ? pointAt (*clip, e.getPosition()) : std::nullopt;
    setHover (clip != nullptr ? clip->itemID : te::EditItemID(), point);

    auto handle = clip != nullptr && ! point ? loopHandleAt (*clip, e.getPosition()) : std::nullopt;

    if (hoverHandleIsStart != handle || hoverHandleClip != (handle ? clip->itemID : te::EditItemID()))
    {
        hoverHandleIsStart = handle;
        hoverHandleClip = handle ? clip->itemID : te::EditItemID();
        repaint();
    }

    if (point)
    {
        cursor = juce::MouseCursor::PointingHandCursor;
    }
    else if (handle)
    {
        cursor = juce::MouseCursor::LeftRightResizeCursor;
    }
    else if (clip != nullptr)
    {
        switch (dragModeFor (*clip, e.getPosition()))
        {
            case DragMode::trimStart:
            case DragMode::trimEnd:     cursor = juce::MouseCursor::LeftRightResizeCursor; break;
            default:                    cursor = juce::MouseCursor::DraggingHandCursor; break;
        }
    }

    setMouseCursor (cursor);
}

void TimelineComponent::mouseExit (const juce::MouseEvent&)
{
    if (dragMode != DragMode::envelopePoint)
        setHover ({}, std::nullopt);
}

void TimelineComponent::mouseDown (const juce::MouseEvent& e)
{
    dragMode = DragMode::none;

    // Keep keyboard shortcuts working after typing in the toolbar.
    if (auto* parent = getParentComponent())
        parent->grabKeyboardFocus();

    auto* clip = dynamic_cast<te::WaveAudioClip*> (clipAt (e.getPosition()));
    auto point = clip != nullptr ? pointAt (*clip, e.getPosition()) : std::nullopt;

    if (e.x < headerWidth)
    {
        if (e.mods.isPopupMenu() && e.y >= getLanesTop())
            showLaneMenu (laneAt (e.y));

        return;
    }

    if (e.mods.isPopupMenu())
    {
        if (point)
        {
            // Right-click a point: delete it.
            auto points = envelopes::getPoints (clip->state, point->param);
            points.erase (points.begin() + point->index);
            edit.getUndoManager().beginNewTransaction ("Delete " + envelopes::getName (point->param).toLowerCase() + " point");
            envelopes::setPoints (clip->state, point->param, points, &edit.getUndoManager());
            setHover ({}, std::nullopt);
        }
        else if (clip != nullptr)
        {
            auto piece = mixtopia::chops::getCuts (*clip).empty() ? -1 : mixtopia::chops::pieceAt (*clip, xToTime (e.x));

            if (! (extraSelection.size() > 0 && isPieceSelected (*clip, piece)))   // (inside a selection of several: keep it)
                selectClip (clip->itemID, piece);

            showClipMenu (*clip, e.getPosition());
        }
        else if (e.y >= getLanesTop())
        {
            showLaneMenu (laneAt (e.y));
        }
        else if (e.y < rulerHeight)
        {
            showRulerMenu (e.getPosition());
        }

        return;
    }

    if (point)
    {
        // Grab a point (any line).
        selectClip (clip->itemID);
        dragClipID = clip->itemID;
        dragSnaps = snapEnabled != e.mods.isShiftDown();
        dragStartX = e.x;
        dragStartY = e.y;
        dragPointParam = point->param;
        dragPointStartSource = envelopes::getPoints (clip->state, point->param)[(size_t) point->index].sourceTime;
        dragPointIndex = point->index;
        dragPointStartDb = envelopes::getPoints (clip->state, point->param)[(size_t) point->index].db;
        dragMode = DragMode::envelopePoint;
        edit.getUndoManager().beginNewTransaction ("Move " + envelopes::getName (point->param).toLowerCase() + " point");
        repaint();
    }
    else if (auto handle = clip != nullptr ? loopHandleAt (*clip, e.getPosition()) : std::nullopt)
    {
        // Grab the loop's start or end.
        selectClip (clip->itemID);
        dragClipID = clip->itemID;
        dragMode = *handle ? DragMode::loopStart : DragMode::loopEnd;
        dragSnaps = snapEnabled != e.mods.isShiftDown();
        edit.getUndoManager().beginNewTransaction (*handle ? "Move the loop start" : "Move the loop end");
    }
    else if (clip != nullptr)
    {
        // A chopped song's piece: selected on its own; it becomes a clip of its own (or, with Ctrl,
        // a copy is made and dragged) once the drag really starts.
        dragPiece = mixtopia::chops::getCuts (*clip).empty() ? -1 : mixtopia::chops::pieceAt (*clip, xToTime (e.x));
        dragCopies = e.mods.isCtrlDown();
        dragShiftToggle = e.mods.isShiftDown() && ! dragCopies;   // (Shift+click without a drag: add / remove it)
        auto inGroup = ! extraSelection.empty() && isPieceSelected (*clip, dragPiece);
        dragIsGroup = inGroup && ! dragCopies;

        if (! dragShiftToggle && ! inGroup)
            selectClip (clip->itemID, dragPiece);

        dragClipID = clip->itemID;
        dragStartX = e.x;
        dragStartY = e.y;
        dragStartPosition = clip->getPosition();
        dragKeep = tempo::getSourceRange (*clip);
        dragContentStartBeat = tempo::isTempoMatched (*clip) ? tempo::getContentStartBeat (*clip) : 0.0;
        dragMode = dragModeFor (*clip, e.getPosition());
        dragReportedPickup = false;
        dragMayCutUntil = dragMode == DragMode::move && tempo::getClipBpm (*clip) > 0.0 ? mixtopia::analysis::getMayCutUntil (*clip) : 0.0;
        dragSnaps = snapEnabled != e.mods.isShiftDown();
        dragCopies = dragMode == DragMode::move && dragCopies;
        dragIsGroup = dragIsGroup && dragMode == DragMode::move;
        dragPrepared = false;
        edit.getUndoManager().beginNewTransaction (dragCopies ? "Copy piece" : dragMode == DragMode::move ? "Move clip" : "Trim clip");
    }
    else if (e.mods.isShiftDown() && e.y >= getLanesTop())
    {
        // Shift+drag in empty lane space: a box selecting the pieces it touches (added to the selection).
        dragMode = DragMode::box;
        boxStart = boxEnd = e.getPosition();
    }
    else
    {
        // Clicking the ruler or empty space moves the playhead there.
        selectClip ({});
        dragMode = DragMode::seek;
        edit.getTransport().setPosition (te::TimePosition::fromSeconds (juce::jmax (0.0, xToTime (e.x))));
        updatePlayhead();
    }
}

void TimelineComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::seek)
    {
        edit.getTransport().setPosition (te::TimePosition::fromSeconds (juce::jmax (0.0, xToTime (e.x))));
        updatePlayhead();
        return;
    }

    if (dragMode == DragMode::box)
    {
        boxEnd = e.getPosition();
        repaint();
        return;
    }

    auto* clip = dynamic_cast<te::WaveAudioClip*> (te::findClipForID (edit, dragClipID));

    if (clip == nullptr)
        return;

    // Songs and loop handles only move once the mouse has really been dragged (JUCE's few-pixel
    // threshold), so a double-click never nudges a song. Points have their own deadzone.
    if (dragMode != DragMode::envelopePoint && ! e.mouseWasDraggedSinceMouseDown())
        return;

    // Chopping: the grabbed piece becomes a clip of its own, or Ctrl drags a copy of it.
    if (dragMode == DragMode::move && ! dragPrepared)
    {
        dragPrepared = true;
        te::AudioClipBase* moving = clip;

        auto changed = false;

        if (dragIsGroup)
        {
            // Every selected piece becomes a clip of its own; the one under the mouse leads.
            auto tracks = te::getAudioTracks (edit);
            Spot lead { tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip->getTrack())), xToTime (dragStartX), false };
            dragGroup.clear();
            std::vector<PieceRef> refs;

            for (auto& spot : selectedSpots())
                if (auto* member = separateAt (spot))
                    refs.push_back ({ member->itemID, -1 });

            moving = separateAt (lead);

            for (auto& r : refs)
                if (auto* member = dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, r.clip)))
                    dragGroup.push_back ({ member->itemID, member->getPosition(), tempo::getSourceRange (*member),
                                           tempo::isTempoMatched (*member) ? tempo::getContentStartBeat (*member) : 0.0,
                                           tempo::toBeats (edit, member->getPosition().getStart()),
                                           tracks.indexOf (dynamic_cast<te::AudioTrack*> (member->getTrack())) });

            if (moving != nullptr)
            {
                std::stable_partition (refs.begin(), refs.end(), [&] (const PieceRef& r) { return r.clip == moving->itemID; });
                setSelection (refs);
            }

            changed = true;
        }
        else if (dragCopies)
        {
            if (auto* track = dynamic_cast<te::AudioTrack*> (clip->getTrack()))
                moving = mixtopia::chops::copyPiece (*clip, juce::jmax (0, dragPiece), *track, 0.0);

            changed = true;
        }
        else if (dragPiece >= 0 && ! mixtopia::chops::getCuts (*clip).empty())
        {
            moving = mixtopia::chops::separate (*clip, dragPiece);
            changed = true;   // (also when the piece stays on this clip - the first piece: it's shorter now)
        }

        if (auto* waveClip = dynamic_cast<te::WaveAudioClip*> (moving); waveClip != nullptr && changed)
        {
            clip = waveClip;
            dragClipID = clip->itemID;

            if (! dragIsGroup)
                selectClip (clip->itemID);
            dragStartPosition = clip->getPosition();
            dragKeep = tempo::getSourceRange (*clip);
            dragContentStartBeat = tempo::isTempoMatched (*clip) ? tempo::getContentStartBeat (*clip) : 0.0;
            dragMayCutUntil = tempo::getClipBpm (*clip) > 0.0 ? mixtopia::analysis::getMayCutUntil (*clip) : 0.0;
        }
    }

    dragSnaps = snapEnabled != e.mods.isShiftDown();
    auto delta = e.getDistanceFromDragStartX() / pixelsPerSecond;
    auto start = dragStartPosition.getStart().inSeconds();
    auto end = dragStartPosition.getEnd().inSeconds();
    auto offset = dragStartPosition.getOffset().inSeconds();
    auto contentStart = start - offset;

    if (dragMode == DragMode::envelopePoint)
    {
        auto points = envelopes::getPoints (clip->state, dragPointParam);

        if (! juce::isPositiveAndBelow (dragPointIndex, (int) points.size()))
            return;

        // Points ignore snap. A small deadzone on each axis keeps a sideways drag at the same
        // level and an up/down drag at the same time; past it the point follows without a jump.
        auto pastDeadzone = [] (int distance)
        {
            return std::abs (distance) <= pointDeadzone ? 0 : distance - (distance > 0 ? pointDeadzone : -pointDeadzone);
        };

        // Time: kept between its neighbours.
        auto editTime = tempo::sourceToEditSeconds (*clip, dragPointStartSource) + pastDeadzone (e.x - dragStartX) / pixelsPerSecond;
        auto source = tempo::editToSourceSeconds (*clip, editTime);
        // Between its neighbours, and within the part of the song that's showing (user: a point
        // dragged past a trimmed start disappeared).
        auto shown = tempo::getSourceRange (*clip);
        auto low = juce::jmax (shown.start, dragPointIndex > 0 ? points[(size_t) dragPointIndex - 1].sourceTime + 0.001 : 0.0);
        auto high = juce::jmin (shown.end, dragPointIndex + 1 < (int) points.size() ? points[(size_t) dragPointIndex + 1].sourceTime - 0.001
                                                                                     : tempo::getSourceLength (*clip));
        auto& point = points[(size_t) dragPointIndex];
        point.sourceTime = juce::jlimit (low, juce::jmax (low, high), source);

        // Value: Shift moves it 4x more finely. Without Shift, 0 dB (the filter: off) is sticky.
        auto area = getEnvelopeArea (getClipBounds (*clip));
        auto dy = (float) pastDeadzone (e.y - dragStartY) * (e.mods.isShiftDown() ? 0.25f : 1.0f);
        auto y = valueToY (dragPointParam, dragPointStartDb, area) + dy;
        point.db = yToValue (dragPointParam, y, area);

        if (! e.mods.isShiftDown() && std::abs (y - valueToY (dragPointParam, 0.0f, area)) <= (float) zeroDbStickiness)
            point.db = 0.0f;

        envelopes::setPoints (clip->state, dragPointParam, points, &edit.getUndoManager());
        return;
    }

    switch (dragMode)
    {
        case DragMode::move:
        {
            auto newStart = start + delta;

            if (dragIsGroup && ! dragGroup.empty())
            {
                // Several: the one under the mouse snaps as a song would; the others move by the same
                // number of beats (none before 0:00) and the same number of lanes.
                auto lead = std::find_if (dragGroup.begin(), dragGroup.end(), [&] (const GroupMember& m) { return m.clip == clip->itemID; });

                if (lead == dragGroup.end())
                    break;

                auto snap = getClipSnap (e.mods.isShiftDown());
                double deltaBeats;

                if (tempo::isTempoMatched (*clip))
                {
                    auto contentStart = tempo::contentStartFor (edit, tempo::getClipBpm (*clip), lead->keep.start, te::TimePosition::fromSeconds (newStart));
                    contentStart = tempo::snapContentStart (*clip, contentStart, snap);
                    deltaBeats = tempo::toBeats (edit, contentStart) - lead->contentStartBeat;
                }
                else
                {
                    if (snap != tempo::Snap::none)
                        newStart = snapTime (newStart);

                    deltaBeats = tempo::toBeats (edit, te::TimePosition::fromSeconds (newStart)) - lead->startBeat;
                }

                auto earliest = lead->startBeat;
                auto lowLane = lead->lane, highLane = lead->lane;

                for (auto& m : dragGroup)
                {
                    earliest = juce::jmin (earliest, m.startBeat);
                    lowLane = juce::jmin (lowLane, m.lane);
                    highLane = juce::jmax (highLane, m.lane);
                }

                deltaBeats = juce::jmax (deltaBeats, -earliest);
                auto laneShift = juce::jlimit (-lowLane, getNumLanes() - 1 - highLane, laneAt (e.y) - lead->lane);
                auto tracks = te::getAudioTracks (edit);

                for (auto& m : dragGroup)
                    if (auto* member = dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, m.clip)))
                    {
                        if (tempo::isTempoMatched (*member))
                            tempo::place (*member, m.contentStartBeat + deltaBeats, m.keep);
                        else
                            member->setStart (tempo::toTime (edit, m.startBeat + deltaBeats), false, true);

                        if (auto* target = tracks[m.lane + laneShift]; target != nullptr && target != member->getTrack())
                            member->moveTo (*target);
                    }

                break;
            }

            // Tempo-matched clips snap so their beat 1 sits on a bar start (Shift: beat lines on
            // beats); others snap their start to a beat.
            auto snap = getClipSnap (e.mods.isShiftDown());

            // Tempo-matched clips keep playing the same part of the song (placed by beats, so it
            // stays right wherever the tempo changes).
            auto contentStartHere = tempo::contentStartFor (edit, tempo::getClipBpm (*clip), dragKeep.start,
                                                            te::TimePosition::fromSeconds (newStart));

            if (tempo::isTempoMatched (*clip) && snap != tempo::Snap::none)
            {
                // Snapped: at the start of the mix, trim silence or move a bar later rather than
                // stopping off the grid at 0:00.
                auto contentStart = tempo::snapContentStart (*clip, contentStartHere, snap);
                auto mayCutUntil = juce::jmax (dragMayCutUntil, dragKeep.start);
                // (Counted from the first bar line in the part that plays: a song whose start was trimmed
                // off - or a piece - must be able to reach 0:00; user, 2026-10-28.)
                auto moved = tempo::placeKeepingTheGrid (*clip, contentStart, dragKeep, mayCutUntil, dragKeep.start);

                if (moved && ! dragReportedPickup)
                    reportMovedForPickup (*clip);

                dragReportedPickup = dragReportedPickup || moved;
            }
            else if (tempo::isTempoMatched (*clip))
            {
                // Free: stops at 0:00 rather than cutting what would go past it.
                tempo::place (*clip, tempo::contentStartNotBeforeZero (*clip, tempo::toBeats (edit, contentStartHere), dragKeep), dragKeep);
            }
            else
            {
                if (snap != tempo::Snap::none)
                    newStart = snapTime (newStart);

                clip->setStart (te::TimePosition::fromSeconds (juce::jmax (0.0, newStart)), false, true);
            }

            auto targetTrack = te::getAudioTracks (edit)[laneAt (e.y)];

            if (targetTrack != nullptr && targetTrack != clip->getTrack())
                clip->moveTo (*targetTrack);

            break;
        }

        case DragMode::trimStart:
        {
            // Can't trim to before the start of the audio file, or past the clip's end.
            auto newStart = dragSnaps ? snapTime (start + delta) : start + delta;

            if (tempo::isTempoMatched (*clip))
            {
                auto songStart = tempo::toTime (edit, dragContentStartBeat).inSeconds();
                newStart = juce::jlimit (juce::jmax (0.0, songStart), end - minClipSeconds, newStart);
                auto source = (tempo::toBeats (edit, te::TimePosition::fromSeconds (newStart)) - dragContentStartBeat) * 60.0 / tempo::getClipBpm (*clip);
                tempo::place (*clip, dragContentStartBeat, { source, dragKeep.end });
                break;
            }

            newStart = juce::jlimit (juce::jmax (0.0, contentStart), end - minClipSeconds, newStart);
            clip->setPosition ({ { te::TimePosition::fromSeconds (newStart), te::TimePosition::fromSeconds (end) },
                                 te::TimeDuration::fromSeconds (newStart - contentStart) });
            break;
        }

        case DragMode::trimEnd:
        {
            auto maxEnd = tempo::isTempoMatched (*clip)
                            ? tempo::toTime (edit, dragContentStartBeat + tempo::getSourceLength (*clip) * tempo::getClipBpm (*clip) / 60.0).inSeconds()
                            : contentStart + tempo::getSourceLength (*clip);
            auto newEnd = dragSnaps ? snapTime (end + delta) : end + delta;
            newEnd = juce::jlimit (start + minClipSeconds, maxEnd, newEnd);
            clip->setLength (te::TimeDuration::fromSeconds (newEnd - start), true);
            break;
        }

        case DragMode::loopStart:
        case DragMode::loopEnd:
        {
            auto* waveClip = dynamic_cast<te::WaveAudioClip*> (clip);
            auto loop = waveClip != nullptr ? mixtopia::loops::getLoop (*waveClip) : std::nullopt;

            if (! loop)
                break;

            // The first pass plays the song unchanged, so the time under the mouse there is song
            // time. Snaps like dragging a song.
            auto song = mixtopia::loops::snapped (*waveClip, tempo::editToSourceSeconds (*waveClip, juce::jmax (0.0, xToTime (e.x))),
                                                  mixtopia::loops::snapFor (snapEnabled, e.mods.isShiftDown()));

            auto length = te::AudioFile (edit.engine, mixtopia::loops::getSongFile (*waveClip)).getLength();
            auto changed = *loop;

            if (dragMode == DragMode::loopStart)
                changed.start = juce::jlimit (0.0, loop->end - 0.05, song);
            else
                changed.end = juce::jlimit (loop->start + 0.05, length, song);

            changed = mixtopia::loops::snappedToSamples (changed, mixtopia::loops::getSampleRate (*waveClip));

            if (! (changed == *loop))
            {
                auto result = mixtopia::loops::apply (*waveClip, changed, false);

                if (result.madeRoom.isNotEmpty() && onStatusMessage)
                    onStatusMessage (result.madeRoom);
            }

            break;
        }

        case DragMode::seek:
        case DragMode::none:
        case DragMode::envelopePoint:
        case DragMode::box:
            break;
    }
}

void TimelineComponent::mouseUp (const juce::MouseEvent& e)
{
    auto dragged = e.mouseWasDraggedSinceMouseDown();

    if (dragMode == DragMode::box)
    {
        // The pieces the box touches join the selection.
        auto box = juce::Rectangle<int> (boxStart, boxEnd);

        if (box.getWidth() > 2)
        {
            auto from = xToTime (box.getX()), to = xToTime (box.getRight());
            auto all = getSelection();
            auto tracks = te::getAudioTracks (edit);

            for (auto lane = laneAt (box.getY()); lane <= laneAt (box.getBottom()); ++lane)
                for (auto* c : tracks[lane]->getClips())
                    if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                    {
                        auto count = (int) mixtopia::chops::getPieces (*clip).size();

                        for (int i = 0; i < count; ++i)
                        {
                            auto range = mixtopia::chops::getPieceEditRange (*clip, i);
                            PieceRef ref { clip->itemID, count > 1 ? i : -1 };

                            if (range.getStart() < to && range.getEnd() > from && std::find (all.begin(), all.end(), ref) == all.end())
                                all.push_back (ref);
                        }
                    }

            setSelection (all);
        }
    }

    // A click without a drag: Shift adds / removes the song or piece; inside a selection of several,
    // a plain click selects just that one.
    if (dragMode == DragMode::move && ! dragged)
    {
        if (dragShiftToggle)
            toggleSelected ({ dragClipID, dragPiece });
        else if (dragIsGroup)
            selectClip (dragClipID, dragPiece);
    }

    if ((dragMode == DragMode::move || dragMode == DragMode::trimStart || dragMode == DragMode::trimEnd) && dragged)
    {
        std::vector<te::EditItemID> moved { dragClipID };

        if (dragIsGroup)
            for (auto& m : dragGroup)
                moved.push_back (m.clip);

        for (auto id : moved)
            if (auto* clip = dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, id)))
            {
                mixtopia::chops::setCuts (*clip, mixtopia::chops::getCuts (*clip), &edit.getUndoManager());   // (cuts trimmed off go)
                keepPointsInside (*clip);   // (part of the drag's undo step)

                // Songs next to each other never share a colour (also part of the drag's undo step).
                if (auto recoloured = mixtopia::clipcolours::keepApartFromNeighbours (*clip); recoloured.isNotEmpty() && onStatusMessage)
                    onStatusMessage (recoloured);
            }

        afterPiecesMoved();
    }

    dragGroup.clear();
    dragIsGroup = dragShiftToggle = false;

    dragMode = DragMode::none;
    dragClipID = {};
    dragPointIndex = -1;
    mouseMove (e);
    repaint();
}

void TimelineComponent::keepPointsInside (te::AudioClipBase& clip)
{
    // (Not at a piece's edges that came from a cut: the song's lines stay there, so pulling the edge
    // out again plays them - user, 2026-10-27.)
    auto shown = tempo::getSourceRange (clip);

    if (mixtopia::chops::hasFadeIn (clip))
        shown.start = -1.0e9;

    if (mixtopia::chops::hasFadeOut (clip))
        shown.end = 1.0e9;

    for (int p = 0; p < envelopes::numParams; ++p)
    {
        auto param = (envelopes::Param) p;
        auto points = envelopes::getPoints (clip.state, param);
        auto inside = envelopes::pushedInside (points, shown.start, shown.end);

        if (inside.size() != points.size() || ! std::equal (inside.begin(), inside.end(), points.begin(),
                                                             [] (auto& a, auto& b) { return a.sourceTime == b.sourceTime && a.db == b.db; }))
            envelopes::setPoints (clip.state, param, inside, &edit.getUndoManager());
    }
}

void TimelineComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click a point: back to 0 dB. Anywhere else on a song: the playhead goes exactly there
    // (no snap; while playing, playback jumps there).
    auto* clip = dynamic_cast<te::WaveAudioClip*> (clipAt (e.getPosition()));

    if (clip == nullptr || ! e.mods.isLeftButtonDown())
        return;

    if (auto point = pointAt (*clip, e.getPosition()))
    {
        auto points = envelopes::getPoints (clip->state, point->param);
        points[(size_t) point->index].db = 0.0f;
        edit.getUndoManager().beginNewTransaction ("Reset point");
        envelopes::setPoints (clip->state, point->param, points, &edit.getUndoManager());
        return;
    }

    edit.getTransport().setPosition (te::TimePosition::fromSeconds (juce::jmax (0.0, xToTime (e.x))));
    updatePlayhead();
}

void TimelineComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Ctrl + wheel (or a sideways trackpad swipe) scrolls along the timeline.
    if (e.mods.isCtrlDown() || wheel.deltaX != 0.0f)
    {
        auto delta = wheel.deltaX != 0.0f ? -wheel.deltaX : -wheel.deltaY;
        setViewStart (viewStart + delta * getVisibleSeconds() * 0.5);
        return;
    }

    // The plain wheel zooms around the mouse pointer.
    if (wheel.deltaY != 0.0f)
        zoomAround (juce::jmax (headerWidth, e.x), wheel.deltaY > 0 ? 1.25 : 0.8);
}

//==============================================================================
bool TimelineComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (mixtopia::isSupportedAudioFile (juce::File (f)))
            return true;

    return false;
}

void TimelineComponent::fileDragEnter (const juce::StringArray&, int, int y)  { dropHighlightLane = laneAt (y); repaint(); }
void TimelineComponent::fileDragMove (const juce::StringArray&, int, int y)   { dropHighlightLane = laneAt (y); repaint(); }
void TimelineComponent::fileDragExit (const juce::StringArray&)               { dropHighlightLane = -1; repaint(); }

void TimelineComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropHighlightLane = -1;
    juce::Logger::writeToLog ("Dropped on lane " + juce::String (laneAt (y) + 1) + ": " + files.joinIntoString (" | "));

    // After the drop has finished: adding may ask a question or show a progress window.
    juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<TimelineComponent> (this), files,
                                      lane = laneAt (y), time = juce::jmax (0.0, xToTime (x))]
    {
        if (safeThis != nullptr)
            safeThis->addFiles (files, lane, te::TimePosition::fromSeconds (time));
    });
}

bool TimelineComponent::isInterestedInTextDrag (const juce::String& text)
{
    return isInterestedInFileDrag (mixtopia::filesFromDroppedText (text));
}

void TimelineComponent::textDropped (const juce::String& text, int x, int y)
{
    filesDropped (mixtopia::filesFromDroppedText (text), x, y);
}

//==============================================================================
void TimelineComponent::handleAsyncUpdate()
{
    // Forget waveforms of files no clip uses any more.
    std::set<juce::String> filesInUse;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (auto* audioClip = dynamic_cast<te::AudioClipBase*> (clip))
                filesInUse.insert (mixtopia::loops::getSongFile (*audioClip).getFullPathName());

    for (auto it = thumbnails.begin(); it != thumbnails.end();)
        it = filesInUse.count (it->first) == 0 ? thumbnails.erase (it) : std::next (it);

    for (auto it = sampleReaders.begin(); it != sampleReaders.end();)
        it = filesInUse.count (it->first) == 0 ? sampleReaders.erase (it) : std::next (it);

    overviews.retainOnly (filesInUse);

    if (selectedClipID.isValid() && te::findClipForID (edit, selectedClipID) == nullptr)
        selectClip ({});
    else if (onSelectionChanged)
        onSelectionChanged();   // the selected clip's BPM etc. may have changed

    updateLaneControls();
    updateScrollBar();
    repaint();
}

void TimelineComponent::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    setViewStart (newRangeStart);
}

void TimelineComponent::onFrame()
{
    updatePlayhead();

    // Follow: keep the playhead on screen while playing (page-turn style).
    if (followPlayhead && edit.getTransport().isPlaying() && dragMode == DragMode::none)
    {
        auto pos = shownPlayheadSeconds;
        auto visible = getVisibleSeconds();

        if (pos < viewStart || pos > viewStart + visible)
            setViewStart (pos - visible * 0.1);
    }
}

bool TimelineComponent::isHiddenByLoop (te::AudioClipBase& clip, double playedTime)
{
    // While a loop copies its first pass's lines into the repeats, points stored inside the repeats
    // are ignored (and not shown); they come back if the loop goes back to "only the first time".
    auto loop = mixtopia::loops::getLoop (clip);
    return loop && loop->copyLines && mixtopia::loops::playedToPlace (loop, playedTime).pass > 0;
}

void TimelineComponent::paintLoop (juce::Graphics& g, te::WaveAudioClip& clip, juce::Rectangle<int> bounds, juce::Colour)
{
    // A looped song: a band under the title naming the loop's first pass and each repeat, the
    // repeats lightly hatched, and a dashed line at every join (all in the controls' teal).
    auto loop = mixtopia::loops::getLoop (clip);

    if (! loop)
        return;

    auto xOf = [&] (double played) { return (float) timeToX (tempo::sourceToEditSeconds (clip, played)); };
    auto top = (float) bounds.getY() + (float) clipTitleHeight, bottom = (float) (bounds.getBottom() - beatRailHeight);
    auto band = juce::Rectangle<float> (xOf (loop->start), top, xOf (loop->end + loop->added()) - xOf (loop->start), 16.0f);

    // Hatching over the repeats.
    {
        juce::Graphics::ScopedSaveState save (g);
        auto repeats = juce::Rectangle<float> (xOf (loop->end), top, xOf (loop->end + loop->added()) - xOf (loop->end), bottom - top);
        g.reduceClipRegion (repeats.toNearestInt());
        g.setColour (theme::accent.withAlpha (0.16f));
        auto first = std::floor ((juce::jmax (repeats.getX(), (float) g.getClipBounds().getX()) - (bottom - top)) / 8.0f) * 8.0f;

        for (auto x = first; x < juce::jmin (repeats.getRight(), (float) g.getClipBounds().getRight()); x += 8.0f)
            g.drawLine (x, bottom, x + (bottom - top), top, 2.0f);
    }

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRect (band);
    g.setColour (theme::accent.withAlpha (0.22f));
    g.fillRect (band.withRight (xOf (loop->end)));
    g.setFont (theme::font (theme::textTiny, true));

    auto label = [&] (double played, double until, const juce::String& text, juce::Colour colour)
    {
        auto area = juce::Rectangle<float> (xOf (played) + 6.0f, top, xOf (until) - xOf (played) - 8.0f, 16.0f);
        g.setColour (colour);
        g.drawText (text, area, juce::Justification::centredLeft, true);
    };

    // Zoomed out, "repeat 1 of 3" no longer fits a repeat: one "LOOP ×3" label instead (drawn after the joins).
    auto longest = "repeat " + juce::String (loop->repeats) + " of " + juce::String (loop->repeats);
    auto narrow = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), longest) > xOf (loop->end + loop->length()) - xOf (loop->end) - 8.0f;

    if (! narrow)
        label (loop->start, loop->end, "LOOP", theme::accent);

    for (int pass = 1; pass <= loop->repeats; ++pass)
    {
        auto from = loop->end + (pass - 1) * loop->length();

        if (! narrow)
            label (from, from + loop->length(), "repeat " + juce::String (pass) + " of " + juce::String (loop->repeats), theme::text1);

        // The join.
        juce::Path line, dashed;
        line.startNewSubPath (xOf (from), top);
        line.lineTo (xOf (from), bottom);
        const float pattern[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, line, pattern, 2);
        g.setColour (theme::accent.withAlpha (0.9f));
        g.fillPath (dashed);
    }

    if (narrow)
    {
        auto text = "LOOP " + juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (loop->repeats);
        auto box = juce::Rectangle<float> (xOf (loop->end) + 10.0f, top + 1.0f,
                                           juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 10.0f, 14.0f);
        g.setColour (theme::tint (theme::accent, 0.25f));
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (theme::accent);
        g.drawText (text, box, juce::Justification::centred, false);
    }

    // The first pass's edges, with grips to drag them (step 2).
    for (auto isStart : { true, false })
    {
        auto x = xOf (isStart ? loop->start : loop->end);
        g.setColour (theme::accent);
        g.fillRect (x - 1.5f, top, 3.0f, bottom - top);
        g.fillRoundedRectangle (x - 6.0f, top, 12.0f, 16.0f, 2.0f);
        g.setColour (theme::bg);

        for (auto dx : { -2.5f, 0.5f, 3.5f })
            g.fillRect (x + dx - 0.5f, top + 4.0f, 1.2f, 8.0f);

        // The readout while the mouse is on it or it's being dragged.
        auto active = (clip.itemID == hoverHandleClip && hoverHandleIsStart == isStart)
                       || (clip.itemID == dragClipID && dragMode == (isStart ? DragMode::loopStart : DragMode::loopEnd));

        if (active)
        {
            auto time = isStart ? loop->start : loop->end;
            auto bars = mixtopia::loops::lengthInBeats (clip, *loop) / beatsPerBar;
            auto second = mixtopia::loops::songBarText (clip, time);
            second += (second.isNotEmpty() ? juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")) : juce::String())
                        + (bars > 0.0 ? juce::String (bars, std::abs (bars - std::round (bars)) < 0.01 ? 0 : 2) + " bars" : juce::String (loop->length(), 1) + " s");
            auto box = juce::Rectangle<float> (x + 12.0f, top + 22.0f, 230.0f, 44.0f);

            if (box.getRight() > (float) getWidth())
                box.setX (x - 12.0f - box.getWidth());

            g.setColour (theme::hover);
            g.fillRoundedRectangle (box, theme::controlRadius);
            g.setColour (theme::line.brighter (0.2f));
            g.drawRoundedRectangle (box.reduced (0.5f), theme::controlRadius, 1.0f);
            g.setColour (theme::text1);
            g.setFont (theme::font (theme::textNormal, true));
            g.drawText ((isStart ? "Loop start  " : "Loop end  ") + mixtopia::loops::formatSongTime (time), box.reduced (10.0f, 4.0f).removeFromTop (18.0f),
                        juce::Justification::centredLeft);
            g.setColour (theme::text2);
            g.setFont (theme::font (theme::textSmall));
            g.drawText (second, box.reduced (10.0f, 4.0f).removeFromBottom (16.0f), juce::Justification::centredLeft, true);
        }
    }
}

std::optional<bool> TimelineComponent::loopHandleAt (te::WaveAudioClip& clip, juce::Point<int> p) const
{
    auto loop = mixtopia::loops::getLoop (clip);

    if (! loop)
        return {};

    auto bounds = getClipBounds (clip);

    if (p.y < bounds.getY() + clipTitleHeight || p.y > bounds.getBottom() - beatRailHeight)
        return {};

    auto xOf = [&] (double played) { return timeToX (tempo::sourceToEditSeconds (clip, played)); };

    if (std::abs (p.x - xOf (loop->end)) <= 6.0)    return false;   // (the end wins where they touch)
    if (std::abs (p.x - xOf (loop->start)) <= 6.0)  return true;
    return {};
}

void TimelineComponent::repeatPartOf (te::WaveAudioClip& clip, int x)
{
    // A song without a loop gets one (8 bars where it was clicked, snapped like a drag), as one
    // undo step; then the loop panel opens to fine-tune it.
    selectClip (clip.itemID);

    if (! mixtopia::loops::getLoop (clip))
    {
        auto played = tempo::editToSourceSeconds (clip, juce::jlimit (clip.getPosition().getStart().inSeconds(),
                                                                       clip.getPosition().getEnd().inSeconds(), xToTime (x)));
        auto loop = mixtopia::loops::makeDefault (clip, mixtopia::loops::playedToSong ({}, played), snapEnabled);
        edit.getUndoManager().beginNewTransaction ("Repeat part of a song");
        auto result = mixtopia::loops::apply (clip, loop, false);

        if (onStatusMessage)
            onStatusMessage ((result.madeRoom + " " + result.recoloured).trim());
    }

    if (onLoopPanelWanted)
        onLoopPanelWanted (clip.itemID);
}

void TimelineComponent::repeatPartAtPlayhead (te::WaveAudioClip& clip)
{
    auto pos = clip.getPosition();
    auto now = juce::jlimit (pos.getStart().inSeconds(), pos.getEnd().inSeconds(), edit.getTransport().getPosition().inSeconds());
    repeatPartOf (clip, (int) std::round (timeToX (now)));
}

juce::Range<double> TimelineComponent::getLoopEditRange (te::AudioClipBase& clip)
{
    auto loop = mixtopia::loops::getLoop (clip);

    if (! loop)
        return {};

    return { tempo::sourceToEditSeconds (clip, loop->start), tempo::sourceToEditSeconds (clip, loop->end + loop->added()) };
}

//==============================================================================
// Quick loops and Loop in / out (looping step 2).
te::WaveAudioClip* TimelineComponent::clipUnderPlayhead (int lane)
{
    auto tracks = te::getAudioTracks (edit);

    if (! juce::isPositiveAndBelow (lane, tracks.size()))
        return nullptr;

    auto now = te::TimePosition::fromSeconds (getPlayheadSeconds());

    // (Songs never overlap on a lane, so there's at most one.)
    for (auto* clip : tracks[lane]->getClips())
        if (clip->getPosition().time.contains (now))
            return dynamic_cast<te::WaveAudioClip*> (clip);

    return nullptr;
}

void TimelineComponent::reportLoopSet (te::WaveAudioClip& clip, const mixtopia::loops::Result& result, bool replaced)
{
    if (! onStatusMessage)
        return;

    auto loop = mixtopia::loops::getLoop (clip);
    onStatusMessage ("Loop set" + juce::String (replaced ? " (replaced the old one)" : "") + ": " + mixtopia::loops::shortTitleOf (clip) + "  "
                     + (loop ? mixtopia::loops::describe (clip, *loop) : juce::String())
                     + " - press E or Edit loop to fine-tune. " + result.madeRoom + " " + result.recoloured);
}

void TimelineComponent::quickLoop (int lane, int bars)
{
    auto* clip = clipUnderPlayhead (lane);

    if (clip == nullptr)
    {
        if (onStatusMessage)
            onStatusMessage ("No song under the playhead on lane " + juce::String (lane + 1) + ".");

        return;
    }

    auto old = mixtopia::loops::getLoop (*clip);
    auto song = mixtopia::loops::playedToSong (old, tempo::editToSourceSeconds (*clip, getPlayheadSeconds()));

    // With Snap on, the loop starts on the song's nearest beat tick (user, 2026-10-23); its end is
    // then exactly that many bars later.
    if (snapEnabled)
        song = mixtopia::loops::nearestBeat (*clip, song);

    juce::String problem;
    auto loop = mixtopia::loops::makeQuick (*clip, song, bars, problem);

    if (! loop)
    {
        if (onStatusMessage)
            onStatusMessage (problem);

        return;
    }

    cancelLoopIn();
    selectClip (clip->itemID);
    edit.getUndoManager().beginNewTransaction ("Quick loop");
    auto result = mixtopia::loops::apply (*clip, *loop, false);
    reportLoopSet (*clip, result, old.has_value());
}

void TimelineComponent::loopIn (int lane)
{
    // Pressing In again (or I) takes the waiting loop in away.
    if (pendingLoopIn && pendingLoopIn->lane == lane)
    {
        cancelLoopIn();

        if (onStatusMessage)
            onStatusMessage ("Loop in cancelled.");

        return;
    }

    auto* clip = clipUnderPlayhead (lane);

    if (clip == nullptr)
    {
        if (onStatusMessage)
            onStatusMessage ("No song under the playhead on lane " + juce::String (lane + 1) + ".");

        return;
    }

    auto song = mixtopia::loops::playedToSong (mixtopia::loops::getLoop (*clip), tempo::editToSourceSeconds (*clip, getPlayheadSeconds()));
    pendingLoopIn = PendingLoopIn { clip->itemID, lane, song };
    selectClip (clip->itemID);
    updateLaneControls();
    repaint();

    if (onStatusMessage)
        onStatusMessage ("Loop in at " + mixtopia::loops::formatSongTime (song) + " in " + mixtopia::loops::shortTitleOf (*clip)
                         + " - press Out (O) where the loop should end. Esc cancels.");
}

void TimelineComponent::loopOut (int lane)
{
    if (! pendingLoopIn)
    {
        if (onStatusMessage)
            onStatusMessage ("Press In (I) where the loop should start first.");

        return;
    }

    auto in = *pendingLoopIn;
    auto* clip = clipUnderPlayhead (in.lane);
    auto* inClip = dynamic_cast<te::WaveAudioClip*> (te::findClipForID (edit, in.clip));

    if (inClip == nullptr || lane != in.lane || clip != inClip)
    {
        cancelLoopIn();

        if (onStatusMessage)
            onStatusMessage ("Loop out must be inside the same song as loop in"
                             + (inClip != nullptr ? " (" + mixtopia::loops::shortTitleOf (*inClip) + ")" : juce::String()) + " - loop in cancelled.");

        return;
    }

    auto old = mixtopia::loops::getLoop (*clip);
    auto out = mixtopia::loops::playedToSong (old, tempo::editToSourceSeconds (*clip, getPlayheadSeconds()));

    if (out < in.song + 0.05)
    {
        if (onStatusMessage)
            onStatusMessage ("Loop out must come after loop in (" + mixtopia::loops::formatSongTime (in.song) + ").");

        return;
    }

    mixtopia::loops::Loop loop;

    if (old)
    {
        loop.crossfadeMs = old->crossfadeMs;
        loop.copyLines = old->copyLines;
    }

    loop.start = in.song;
    loop.end = out;
    loop.repeats = 1;
    pendingLoopIn.reset();
    updateLaneControls();
    edit.getUndoManager().beginNewTransaction ("Loop in / out");
    auto result = mixtopia::loops::apply (*clip, loop, false);
    reportLoopSet (*clip, result, old.has_value());
    repaint();
}

bool TimelineComponent::cancelLoopIn()
{
    if (! pendingLoopIn)
        return false;

    pendingLoopIn.reset();
    updateLaneControls();
    repaint();
    return true;
}

void TimelineComponent::showLoopMenu (int lane)
{
    juce::PopupMenu menu;

    for (int i = 0; i < numQuickLoops; ++i)
        menu.addItem (1 + i, "Loop " + juce::String (quickLoopBars[i]) + (quickLoopBars[i] == 1 ? " bar" : " bars") + " from the playhead");

    menu.addSeparator();
    auto waiting = pendingLoopIn && pendingLoopIn->lane == lane;
    menu.addItem (10, waiting ? "Cancel loop in" : "Loop in here");
    menu.addItem (11, "Loop out here", waiting);
    menu.addSeparator();
    auto* clip = clipUnderPlayhead (lane);
    menu.addItem (12, "Edit loop...", clip != nullptr && mixtopia::loops::getLoop (*clip).has_value());

    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [safeThis = juce::Component::SafePointer<TimelineComponent> (this), lane] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            if (result <= numQuickLoops) safeThis->quickLoop (lane, quickLoopBars[result - 1]);
                            else if (result == 10)      safeThis->loopIn (lane);
                            else if (result == 11)      safeThis->loopOut (lane);
                            else if (auto* c = safeThis->clipUnderPlayhead (lane); c != nullptr && safeThis->onLoopPanelWanted)
                                safeThis->onLoopPanelWanted (c->itemID);
                        });
}

void TimelineComponent::loopInKey()
{
    if (pendingLoopIn)
        return loopIn (pendingLoopIn->lane);   // (cancels it)

    // The selected song if it's under the playhead, else the top-most lane with a song there.
    auto tracks = te::getAudioTracks (edit);

    for (int lane = 0; lane < tracks.size(); ++lane)
        if (auto* clip = clipUnderPlayhead (lane); clip != nullptr && clip->itemID == selectedClipID)
            return loopIn (lane);

    for (int lane = 0; lane < tracks.size(); ++lane)
        if (clipUnderPlayhead (lane) != nullptr)
            return loopIn (lane);

    if (onStatusMessage)
        onStatusMessage ("No song under the playhead.");
}

void TimelineComponent::loopOutKey()
{
    loopOut (pendingLoopIn ? pendingLoopIn->lane : -1);
}

te::EditItemID TimelineComponent::getSongForLoopPanel()
{
    if (auto* selected = getSelectedClip(); selected != nullptr && mixtopia::loops::getLoop (*selected))
        return selected->itemID;

    for (int lane = 0; lane < getNumLanes(); ++lane)
        if (auto* clip = clipUnderPlayhead (lane); clip != nullptr && mixtopia::loops::getLoop (*clip))
            return clip->itemID;

    return {};
}
