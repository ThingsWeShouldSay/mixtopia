#include "LoopPanel.h"
#include "TimelineComponent.h"
#include "ClipColours.h"

namespace theme = mixtopia::theme;
namespace loops = mixtopia::loops;
namespace tempo = mixtopia::tempo;

namespace
{
    constexpr int pad = 18, rowGap = 12, nudgeHeight = 26, boxHeight = 30;
    const juce::String leftArrow (juce::CharPointer_UTF8 ("\xe2\x80\xb9")), rightArrow (juce::CharPointer_UTF8 ("\xe2\x80\xba"));

    void setUpBox (juce::TextEditor& box)
    {
        box.setFont (theme::font (14.0f, true));
        box.setJustification (juce::Justification::centredLeft);
        box.setIndents (10, 0);
        box.setSelectAllWhenFocused (true);
        box.setTooltip ("Type a time in the song (3:21.418 or 201.4) or a bar (bar 97.1) and press Enter");
    }
}

LoopPanel::LoopPanel (te::Edit& e, TimelineComponent& t)
    : edit (e), timeline (t)
{
    setUpBox (startBox);
    setUpBox (endBox);
    startBox.onReturnKey = [this] { setFromText (true); };
    endBox.onReturnKey = [this] { setFromText (false); };
    startBox.onFocusLost = [this] { setFromText (true); };
    endBox.onFocusLost = [this] { setFromText (false); };
    startBox.onEscapeKey = endBox.onEscapeKey = [this] { shown.reset(); refresh(); };

    startAtPlayhead.onClick = [this] { setFromPlayhead (true); };
    endAtPlayhead.onClick = [this] { setFromPlayhead (false); };

    auto nudge = [this] (bool start, bool end)
    {
        return [this, start, end] (double amount, bool isBeat)
        {
            auto* clip = getClip();
            auto loop = clip != nullptr ? loops::getLoop (*clip) : std::nullopt;

            if (! loop)
                return;

            if (isBeat)
                amount *= 60.0 / tempo::getClipBpm (*clip);

            if (start)  loop->start += amount;
            if (end)    loop->end += amount;

            commit (*loop, start && end ? "Move the loop" : start ? "Move the loop start" : "Move the loop end");
        };
    };

    setUpNudges (startNudges, nudge (true, false));
    setUpNudges (endNudges, nudge (false, true));
    setUpNudges (wholeNudges, nudge (true, true));

    auto change = [this] (std::function<void (loops::Loop&)> f, const juce::String& name)
    {
        if (auto* clip = getClip())
            if (auto loop = loops::getLoop (*clip))
            {
                f (*loop);
                commit (*loop, name);
            }
    };

    repeatsDown.onClick = [change] { change ([] (auto& l) { l.repeats -= 1; }, "Fewer repeats"); };
    repeatsUp.onClick = [change] { change ([] (auto& l) { l.repeats += 1; }, "More repeats"); };
    fadeDown.onClick = [change] { change ([] (auto& l) { l.crossfadeMs = std::round (l.crossfadeMs) - 1.0; }, "Shorter crossfade"); };
    fadeUp.onClick = [change] { change ([] (auto& l) { l.crossfadeMs = std::round (l.crossfadeMs) + 1.0; }, "Longer crossfade"); };

    snapBeatsButton.onClick = [this, change]
    {
        if (auto* clip = getClip())
        {
            auto bpm = tempo::getClipBpm (*clip);
            change ([clip, bpm] (auto& l) { l.end = l.start + juce::jmax (1.0, std::round (loops::lengthInBeats (*clip, l))) * 60.0 / bpm; },
                    "Loop to whole beats");
        }
    };

    listenButton.setTooltip ("Plays 2 s before the join to 2 s after it, round and round, so you can hear whether it's seamless while you "
                             "change the loop. Stop with this button, Esc or Space.");
    listenButton.onClick = [this] { isListening() ? stopListening() : startListening(); };
    songOnlyToggle.setTooltip ("Listen to this song alone (its lane soloed while listening); off = the real mix, with the other songs");
    songOnlyToggle.onClick = [this] { if (isListening()) applySongOnly(); };

    moveLaterToggle.setButtonText ("Move everything after the loop later");
    moveLaterToggle.setTooltip ("While this is ticked, every change to the loop also moves the songs and tempo points after it (all lanes). "
                                "Off: nothing else moves - unless the song would run into the next song on its lane, then they move anyway.");

    linesOnce.setRadioGroupId (1);
    linesCopy.setRadioGroupId (1);
    linesOnce.onClick = [change] { change ([] (auto& l) { l.copyLines = false; }, "Lines only the first time"); };
    linesCopy.onClick = [change] { change ([] (auto& l) { l.copyLines = true; }, "Lines copied into every repeat"); };

    removeButton.onClick = [this]
    {
        if (auto* clip = getClip())
        {
            edit.getUndoManager().beginNewTransaction ("Remove the loop");
            loops::apply (*clip, std::nullopt, false);
            status (clip->getName() + ": loop removed.");
        }

        setVisible (false);
    };

    doneButton.onClick = closeButton.onClick = [this] { setVisible (false); };

    for (auto* c : std::initializer_list<juce::Component*> { &closeButton, &startBox, &endBox, &startAtPlayhead, &endAtPlayhead,
                                                             &repeatsDown, &repeatsUp, &fadeDown, &fadeUp, &snapBeatsButton, &listenButton,
                                                             &songOnlyToggle, &moveLaterToggle, &linesOnce, &linesCopy, &removeButton, &doneButton })
        addAndMakeVisible (c);

    for (auto* b : std::initializer_list<juce::Button*> { &closeButton, &startAtPlayhead, &endAtPlayhead, &repeatsDown, &repeatsUp, &fadeDown,
                                                          &fadeUp, &snapBeatsButton, &songOnlyToggle, &moveLaterToggle, &linesOnce, &linesCopy,
                                                          &removeButton, &doneButton })
        b->setWantsKeyboardFocus (false);

    setSize (panelWidth, 600);
    startTimerHz (8);
}

void LoopPanel::setUpNudges (Nudges& n, std::function<void (double, bool)> action)
{
    struct Step { const char* text; double amount; bool beat; };
    const Step steps[] { { "beat", -1.0, true }, { "10 ms", -0.010, false }, { "1 ms", -0.001, false },
                         { "1 ms", 0.001, false }, { "10 ms", 0.010, false }, { "beat", 1.0, true } };

    for (auto& step : steps)
    {
        auto* b = n.buttons.add (new juce::TextButton (step.amount < 0 ? leftArrow + " " + step.text : juce::String (step.text) + " " + rightArrow));
        b->setWantsKeyboardFocus (false);
        b->onClick = [action, step] { action (step.amount, step.beat); };
        addAndMakeVisible (b);
    }
}

te::AudioClipBase* LoopPanel::getClip() const
{
    return dynamic_cast<te::AudioClipBase*> (te::findClipForID (edit, clipID));
}

void LoopPanel::showFor (te::EditItemID id, juce::Rectangle<int> avoid, juce::Rectangle<int> within, int preferredTop)
{
    if (isListening() && id != clipID)
        stopListening();

    clipID = id;
    shown.reset();
    refresh();

    // Beside the loop, not over it: on the side of the timeline with more room.
    auto height = getIdealHeight();
    auto x = avoid.getCentreX() < within.getCentreX() ? within.getRight() - panelWidth - 12 : within.getX() + 12;
    auto y = juce::jmax (within.getY(), juce::jmin (preferredTop, within.getBottom() - height));   // (up over the library if it's tall)
    setBounds (juce::Rectangle<int> (x, y, panelWidth, height).constrainedWithin (within));
    setVisible (true);
    toFront (false);
}

bool LoopPanel::commit (loops::Loop loop, const juce::String& undoName)
{
    auto* clip = getClip();

    if (clip == nullptr)
        return false;

    auto length = te::AudioFile (edit.engine, loops::getSongFile (*clip)).getLength();
    loop.repeats = juce::jlimit (1, loops::maxRepeats, loop.repeats);
    loop.crossfadeMs = juce::jlimit (0.0, loops::maxCrossfadeMs, loop.crossfadeMs);

    juce::String problem;

    if (loop.start < 0.0)                     problem = "The loop can't start before the song does.";
    else if (loop.end > length + 1.0e-6)      problem = "The loop can't end after the song does.";
    else if (loop.length() < 0.05)            problem = "The loop's end must be after its start.";

    if (problem.isNotEmpty())
    {
        status (problem);
        shown.reset();
        refresh();
        return false;
    }

    edit.getUndoManager().beginNewTransaction (undoName);
    auto result = loops::apply (*clip, loop, moveLaterToggle.getToggleState());

    if (result.madeRoom.isNotEmpty() || result.recoloured.isNotEmpty())
        status ((result.madeRoom + " " + result.recoloured).trim());

    shown.reset();
    refresh();
    return true;
}

void LoopPanel::setFromPlayhead (bool isStart)
{
    auto* clip = getClip();
    auto loop = clip != nullptr ? loops::getLoop (*clip) : std::nullopt;

    if (! loop)
        return;

    auto now = edit.getTransport().getPosition();

    if (! clip->getPosition().time.contains (now))
    {
        status ("Put the playhead on this song first.");
        return;
    }

    // Snaps like dragging a song.
    auto song = loops::snapped (*clip, loops::playedToSong (loop, tempo::editToSourceSeconds (*clip, now.inSeconds())),
                                loops::snapFor (timeline.isSnapEnabled(), juce::ModifierKeys::currentModifiers.isShiftDown()));

    (isStart ? loop->start : loop->end) = song;
    commit (*loop, isStart ? "Loop start at the playhead" : "Loop end at the playhead");
}

void LoopPanel::setFromText (bool isStart)
{
    auto* clip = getClip();
    auto loop = clip != nullptr ? loops::getLoop (*clip) : std::nullopt;
    auto& box = isStart ? startBox : endBox;

    if (! loop || box.getText() == loops::formatSongTime (isStart ? loop->start : loop->end))
        return;

    if (auto seconds = loops::parseSongTime (*clip, box.getText()))
    {
        (isStart ? loop->start : loop->end) = *seconds;
        commit (*loop, isStart ? "Type the loop start" : "Type the loop end");
    }
    else
    {
        status ("Type a time like 3:21.418 or 201.4, or a bar like bar 97.1.");
        shown.reset();
        refresh();
    }
}

void LoopPanel::timerCallback()
{
    // Listening stopped from elsewhere (Stop, Play, a jump): put things back.
    if (isListening() && ! edit.getTransport().isPlaying())
        stopListening();

    if (isVisible())
        refresh();
}

void LoopPanel::visibilityChanged()
{
    if (! isVisible() && isListening())
        stopListening();
}

juce::Range<double> LoopPanel::getListenRange() const
{
    // 2 s either side of the first join (every join sounds the same).
    auto* clip = getClip();
    auto loop = clip != nullptr ? loops::getLoop (*clip) : std::nullopt;

    if (! loop)
        return {};

    auto join = tempo::sourceToEditSeconds (*clip, loop->end);
    return { juce::jmax (0.0, join - listenSeconds), join + listenSeconds };
}

void LoopPanel::startListening()
{
    auto range = getListenRange();

    if (range.isEmpty())
        return;

    auto& transport = edit.getTransport();
    saved.loopRange = transport.getLoopRange();
    saved.looping = transport.looping.get();
    saved.position = transport.getPosition();
    saved.solo.clear();

    for (auto* track : te::getAudioTracks (edit))
        saved.solo.push_back (track->isSolo (false));

    listening = true;
    applySongOnly();
    transport.setLoopRange ({ te::TimePosition::fromSeconds (range.getStart()), te::TimePosition::fromSeconds (range.getEnd()) });
    transport.looping = true;
    transport.setPosition (te::TimePosition::fromSeconds (range.getStart()));
    transport.play (false);
    shownRange = range;
    refreshListenButton();
}

void LoopPanel::stopListening()
{
    if (! listening)
        return;

    listening = false;
    auto& transport = edit.getTransport();
    transport.stop (false, false);
    transport.looping = saved.looping;
    transport.setLoopRange (saved.loopRange);
    transport.setPosition (saved.position);

    auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < tracks.size() && i < (int) saved.solo.size(); ++i)
        tracks[i]->setSolo (saved.solo[(size_t) i]);

    edit.updateMuteSoloStatuses();
    refreshListenButton();
}

void LoopPanel::applySongOnly()
{
    // "This song only": its lane soloed (songs never overlap on a lane, so that's this song).
    auto* clip = getClip();
    auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < tracks.size() && i < (int) saved.solo.size(); ++i)
        tracks[i]->setSolo (songOnlyToggle.getToggleState() ? (clip != nullptr && clip->getTrack() == tracks[i]) : saved.solo[(size_t) i]);

    edit.updateMuteSoloStatuses();
}

void LoopPanel::refreshListenButton()
{
    listenButton.setButtonText (listening ? "Stop listening" : "Listen to the join");
    listenButton.setIcon (listening ? theme::Icon::pause : theme::Icon::play);
    listenButton.setToggleState (listening, juce::dontSendNotification);
}

void LoopPanel::refresh()
{
    auto* clip = getClip();
    auto loop = clip != nullptr ? loops::getLoop (*clip) : std::nullopt;

    // The song or its loop is gone (deleted, undone): nothing to edit.
    if (! loop)
    {
        setVisible (false);
        return;
    }

    if (shown == loop)
        return;

    shown = loop;
    auto bpm = tempo::getClipBpm (*clip);

    if (! startBox.hasKeyboardFocus (true))
        startBox.setText (loops::formatSongTime (loop->start), false);

    if (! endBox.hasKeyboardFocus (true))
        endBox.setText (loops::formatSongTime (loop->end), false);

    for (auto* nudges : { &startNudges, &endNudges, &wholeNudges })
    {
        nudges->buttons[0]->setEnabled (bpm > 0.0);
        nudges->buttons[5]->setEnabled (bpm > 0.0);
    }

    linesOnce.setToggleState (! loop->copyLines, juce::dontSendNotification);
    linesCopy.setToggleState (loop->copyLines, juce::dontSendNotification);
    repeatsDown.setEnabled (loop->repeats > 1);
    repeatsUp.setEnabled (loop->repeats < loops::maxRepeats);
    fadeDown.setEnabled (loop->crossfadeMs > 0.0);
    fadeUp.setEnabled (loop->crossfadeMs < loops::maxCrossfadeMs);

    auto beats = loops::lengthInBeats (*clip, *loop);
    auto whole = std::round (beats);
    auto fractional = bpm > 0.0 && std::abs (beats - whole) > 0.001;
    snapBeatsButton.setButtonText ("Snap to " + juce::String ((int) juce::jmax (1.0, whole)) + (whole == 1.0 ? " beat" : " beats"));
    snapBeatsButton.setVisible (fractional);

    // While listening, the range follows the loop's end (nudges are heard on the next time round).
    if (listening)
    {
        auto range = getListenRange();

        if (range != shownRange && ! range.isEmpty())
        {
            edit.getTransport().setLoopRange ({ te::TimePosition::fromSeconds (range.getStart()), te::TimePosition::fromSeconds (range.getEnd()) });
            shownRange = range;
        }
    }

    auto height = getIdealHeight();

    if (getHeight() != height)
        setSize (getWidth(), height);

    repaint();
}

int LoopPanel::getIdealHeight()
{
    // The layout works out the height it needs.
    resized();
    return contentHeight;
}

void LoopPanel::resized()
{
    auto w = getWidth();
    closeButton.setBounds (w - 44, 14, 26, 26);

    auto y = 64;
    auto nudgeRow = [&] (Nudges& n)
    {
        auto x = pad;
        auto width = (w - 2 * pad - 5 * 4) / 6;

        for (auto* b : n.buttons)
        {
            b->setBounds (x, y, width, nudgeHeight);
            x += width + 4;
        }

        y += nudgeHeight + rowGap;
    };

    auto pointRows = [&] (juce::Rectangle<int>& labelRow, juce::TextEditor& box, juce::TextButton& atPlayhead, Nudges& n)
    {
        labelRow = { pad, y, w - 2 * pad, 16 };
        y += 18;
        box.setBounds (pad, y, 126, boxHeight);
        atPlayhead.setBounds (w - pad - 170, y, 170, boxHeight);
        y += boxHeight + 6;
        nudgeRow (n);
    };

    pointRows (startLabelRow, startBox, startAtPlayhead, startNudges);
    pointRows (endLabelRow, endBox, endAtPlayhead, endNudges);
    wholeLabelRow = { pad, y, w - 2 * pad, 16 };
    y += 18;
    nudgeRow (wholeNudges);

    repeatsRow = { pad, y, 220, 46 };
    fadeRow = { pad + 240, y, 220, 46 };
    repeatsDown.setBounds (repeatsRow.getX(), y + 18, 28, 28);
    repeatsUp.setBounds (repeatsRow.getX() + 82, y + 18, 28, 28);
    fadeDown.setBounds (fadeRow.getX(), y + 18, 28, 28);
    fadeUp.setBounds (fadeRow.getX() + 82, y + 18, 28, 28);
    y += 46 + 18;

    readoutRow = { pad, y, w - 2 * pad, 48 };
    y += 1 + 12 + 48;

    fractionRow = {};

    if (snapBeatsButton.isVisible())
    {
        fractionRow = { pad, y, w - 2 * pad, 54 };
        snapBeatsButton.setBounds (fractionRow.getRight() - 132, y + 13, 120, 28);
        y += 64;
    }

    listenButton.setBounds (pad, y, 196, 34);
    songOnlyToggle.setBounds (pad + 212, y + 4, 160, 26);
    y += 34 + 16;

    moveLaterRow = { pad, y, w - 2 * pad, 30 };
    moveLaterToggle.setBounds (pad, y, w - 2 * pad, 22);
    y += 30;

    linesRow = { pad, y, w - 2 * pad, 46 };
    linesOnce.setBounds (pad, y + 18, 190, 26);
    linesCopy.setBounds (pad + 210, y + 18, 230, 26);
    y += 46 + 1 + 12;

    bottomRow = { pad, y, w - 2 * pad, 34 };
    removeButton.setBounds (pad, y, 130, 34);
    doneButton.setBounds (w - pad - 100, y, 100, 34);
    contentHeight = y + 34 + pad;
}

void LoopPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (theme::line.brighter (0.25f));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.2f);

    auto* clip = getClip();
    auto loop = clip != nullptr ? loops::getLoop (*clip) : std::nullopt;

    if (! loop)
        return;

    g.setColour (theme::text1);
    g.setFont (theme::font (16.0f, true));
    g.drawText ("Repeat part of this song", pad, 12, getWidth() - 80, 22, juce::Justification::centredLeft);
    g.setColour (mixtopia::clipcolours::get (*clip));
    g.setFont (theme::font (theme::textSmall, true));
    g.drawText (clip->getName(), pad, 36, getWidth() - 80, 16, juce::Justification::centredLeft, true);

    theme::drawLabel (g, "LOOP START", startLabelRow);
    theme::drawLabel (g, "LOOP END", endLabelRow);
    theme::drawLabel (g, "MOVE THE WHOLE LOOP (same length)", wholeLabelRow);

    g.setColour (theme::text2);
    g.setFont (theme::font (theme::textSmall));
    g.drawText (loops::songBarText (*clip, loop->start), startBox.getRight() + 10, startBox.getY(), startAtPlayhead.getX() - startBox.getRight() - 14,
                boxHeight, juce::Justification::centredLeft, true);
    g.drawText (loops::songBarText (*clip, loop->end), endBox.getRight() + 10, endBox.getY(), endAtPlayhead.getX() - endBox.getRight() - 14,
                boxHeight, juce::Justification::centredLeft, true);

    // Steppers.
    theme::drawLabel (g, "REPEATS", repeatsRow.withHeight (16));
    theme::drawLabel (g, "CROSSFADE AT EACH JOIN", fadeRow.withHeight (16));
    g.setColour (theme::inset);
    g.fillRoundedRectangle (repeatsDown.getBounds().getTopRight().toFloat().x + 2.0f, (float) repeatsDown.getY(), 48.0f, 28.0f, 4.0f);
    g.fillRoundedRectangle (fadeDown.getBounds().getTopRight().toFloat().x + 2.0f, (float) fadeDown.getY(), 48.0f, 28.0f, 4.0f);
    g.setColour (theme::text1);
    g.setFont (theme::font (14.0f, true));
    g.drawText (juce::String (loop->repeats), repeatsDown.getRight() + 2, repeatsDown.getY(), 48, 28, juce::Justification::centred);
    g.drawText (juce::String (juce::roundToInt (loop->crossfadeMs)) + " ms", fadeDown.getRight() + 2, fadeDown.getY(), 48, 28, juce::Justification::centred);
    g.setColour (theme::text2);
    g.setFont (theme::font (theme::textSmall));
    g.drawText ("plays " + juce::String (loop->repeats + 1) + " times", repeatsUp.getRight() + 10, repeatsUp.getY(), 100, 28, juce::Justification::centredLeft);

    // Readouts.
    g.setColour (theme::line);
    g.fillRect (pad, readoutRow.getY() - 6, getWidth() - 2 * pad, 1);
    auto bpm = tempo::getClipBpm (*clip);
    auto beats = loops::lengthInBeats (*clip, *loop);
    auto dot = juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
    auto lengthText = bpm > 0.0 ? (std::abs (beats - std::round (beats)) <= 0.001 && std::fmod (std::round (beats), (double) tempo::beatsPerBar) == 0.0
                                     ? juce::String ((int) std::round (beats) / tempo::beatsPerBar) + " bars  (" + juce::String ((int) std::round (beats)) + " beats)"
                                     : juce::String (beats, std::abs (beats - std::round (beats)) <= 0.001 ? 0 : 2) + " beats") + dot
                                : juce::String();
    lengthText += juce::String (loop->length(), 1) + " s";

    g.setColour (theme::text2);
    g.setFont (theme::font (theme::textSmall));
    g.drawText ("Loop length", readoutRow.getX(), readoutRow.getY() + 4, 140, 18, juce::Justification::centredLeft);
    g.drawText ("Song gets longer by", readoutRow.getX(), readoutRow.getY() + 28, 140, 18, juce::Justification::centredLeft);
    g.setColour (theme::text1);
    g.setFont (theme::font (theme::textNormal, true));
    g.drawText (lengthText, readoutRow.getX() + 150, readoutRow.getY() + 4, readoutRow.getWidth() - 150, 18, juce::Justification::centredLeft, true);
    g.setColour (theme::accent);
    g.drawText (loops::describeAdded (*clip, *loop), readoutRow.getX() + 150, readoutRow.getY() + 28, readoutRow.getWidth() - 150, 18,
                juce::Justification::centredLeft, true);

    auto warning = [&] (juce::Rectangle<int> row, const juce::String& first, const juce::String& second, int buttonWidth)
    {
        g.setColour (theme::warn.withAlpha (0.12f));
        g.fillRoundedRectangle (row.toFloat(), 5.0f);
        g.setColour (theme::warn);
        g.fillRect (row.withWidth (3));
        auto text = row.withTrimmedLeft (12).withTrimmedRight (buttonWidth + 16);
        g.setColour (theme::text1);
        g.setFont (theme::font (theme::textSmall, true));
        g.drawText (first, text.removeFromTop (row.getHeight() / 2).withTrimmedTop (6), juce::Justification::centredLeft, true);
        g.setColour (theme::text2);
        g.setFont (theme::font (theme::textSmall));
        g.drawText (second, text.withTrimmedBottom (6), juce::Justification::centredLeft, true);
    };

    if (! fractionRow.isEmpty())
    {
        auto off = beats - std::floor (beats);
        warning (fractionRow, "Loop = " + juce::String (beats, 2) + " beats - not a whole number of beats.",
                 "After the loop the groove sits " + juce::String (off <= 0.5 ? off : 1.0 - off, 2) + " beat off the grid.", snapBeatsButton.getWidth());
    }


    theme::drawLabel (g, "LINES INSIDE THE LOOP", linesRow.withHeight (16));
    g.setColour (theme::line);
    g.fillRect (pad, bottomRow.getY() - 14, getWidth() - 2 * pad, 1);
}

void LoopPanel::mouseDown (const juce::MouseEvent& e)
{
    // Dragged by its title.
    if (e.y < 60)
        dragger.startDraggingComponent (this, e);
}

void LoopPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mouseDownPosition.y < 60)
    {
        constrainer.setMinimumOnscreenAmounts (getHeight(), getWidth(), getHeight(), getWidth());
        dragger.dragComponent (this, e, &constrainer);
    }
}
