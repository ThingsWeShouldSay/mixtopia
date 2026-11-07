#include "MainComponent.h"
#include "ClipTempo.h"
#include "EditHelpers.h"
#include "DevSpecs.h"
#include "Project.h"
#include "Exporter.h"
#include "SongAnalysis.h"
#include "KeyDetection.h"
#include "LevelMatch.h"
#include "Settings.h"
#include "TempoRamps.h"
#include "ClipColours.h"
#include "Loops.h"
#include "ShortcutsList.h"
#include "DialogFocus.h"
#include "ExportDialog.h"
#include "AppBehaviour.h"

namespace tempo = mixtopia::tempo;
namespace project = mixtopia::project;
namespace exporter = mixtopia::exporter;
namespace theme = mixtopia::theme;

namespace
{
    // The Beat grid menu's "MOVE THE GRID" row: < beat, < 10 ms, 10 ms >, beat >. A click moves the selected
    // song's grid and leaves the menu open, so it can be clicked again while listening.
    struct GridNudgeRow : public juce::PopupMenu::CustomComponent
    {
        explicit GridNudgeRow (std::function<void (int)> nudge) : juce::PopupMenu::CustomComponent (false)
        {
            const char* names[] = { "< beat", "< 10 ms", "10 ms >", "beat >" };
            const char* tips[] = { "Beat 1 is one beat earlier: the beat lines stay, the bar starts move",
                                   "Move this song's beat grid 10 ms earlier", "Move this song's beat grid 10 ms later",
                                   "Beat 1 is one beat later: the beat lines stay, the bar starts move" };

            for (int i = 0; i < 4; ++i)
            {
                auto* b = buttons.add (new juce::TextButton (names[i]));
                b->setTooltip (tips[i]);
                b->setWantsKeyboardFocus (false);
                b->onClick = [nudge, i] { nudge (i); };
                addAndMakeVisible (b);
            }
        }

        void getIdealSize (int& w, int& h) override     { w = 2 * pad + 4 * buttonWidth + 3 * gap; h = 50; }

        void resized() override
        {
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setBounds (pad + i * (buttonWidth + gap), 20, buttonWidth, 24);
        }

        void paint (juce::Graphics& g) override     { theme::drawLabel (g, "MOVE THE GRID", { pad + 2, 2, 160, 16 }); }

        static constexpr int buttonWidth = 62, gap = 4, pad = 12;
        juce::OwnedArray<juce::TextButton> buttons;
    };

    // "12:34.5" (with tenths) or "45:44"; hours when needed ("1:02:03").
    juce::String formatTime (double seconds, bool withTenths)
    {
        auto tenths = (juce::int64) std::floor (juce::jmax (0.0, seconds) * 10.0);
        auto whole = tenths / 10;
        auto text = whole >= 3600 ? juce::String::formatted ("%d:%02d:%02d", (int) (whole / 3600), (int) (whole / 60 % 60), (int) (whole % 60))
                                  : juce::String::formatted ("%d:%02d", (int) (whole / 60), (int) (whole % 60));
        return withTenths ? text + "." + juce::String ((int) (tenths % 10)) : text;
    }

    juce::String formatBar (te::Edit& edit, te::TimePosition position)
    {
        auto beats = edit.tempoSequence.toBeats (position).inBeats();
        auto bar = (int) std::floor (beats / 4.0) + 1;
        auto beat = (int) std::floor (std::fmod (beats, 4.0)) + 1;
        return juce::String (bar) + "." + juce::String (beat);
    }

    // Accepts a typed BPM, or returns 0 if it isn't a sensible one.
    double parseBpm (const juce::String& text)
    {
        auto bpm = text.trim().getDoubleValue();
        return bpm >= tempo::minBpm && bpm <= tempo::maxBpm ? bpm : 0.0;
    }

    // Export window: the cover image (Choose... opens the file browser; Remove clears it).
    enum MenuItems { newItem = 1, openItem, saveItem, saveAsItem, exportItem, quitItem, autoTrimItem, levelMatchItem, limiterControlItem, tempoStripItem, libraryItem,
                     stretchSignalsmithItem, stretchRubberBandItem, barLinesItem, clearSongCacheItem, defaultTempoItem,
                     openLastProjectItem, shortcutsItem, clearRecentItem, noRecentItem,
                     undoItem, redoItem, cutItem, cutEverySongItem, duplicateItem, deleteItem, selectSongPiecesItem,
                     interfaceScaleItemBase = 100, songCacheSizeItemBase = 200, recentProjectItemBase = 300, autosaveItemBase = 400 };

    constexpr double backupIntervalSeconds = 120.0;   // the crash backup while autosave is off

    juce::File getDefaultFolder (const juce::File& projectFile)
    {
        return projectFile != juce::File() ? projectFile.getParentDirectory()
                                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    }

    void setUpNumberEditor (juce::TextEditor& editor, float textSize)
    {
        editor.setInputRestrictions (7, "0123456789.");
        editor.setFont (theme::font (textSize, true));
        editor.setJustification (juce::Justification::centred);
        editor.setSelectAllWhenFocused (true);
        editor.setIndents (2, 0);
    }

    void setUpLabel (juce::Label& label, float size, juce::Colour colour, bool bold = false)
    {
        label.setFont (theme::font (size, bold));
        label.setColour (juce::Label::textColourId, colour);
        label.setBorderSize ({});
    }
}

MainComponent::MainComponent (te::Engine& e, const juce::StringArray& devSpecs)
    : engine (e)
{
    addAndMakeVisible (menuBar);

    // Song library (one for all projects). A test session (Claude's GUI scripts) uses its own file
    // so the user's library is never touched.
    library = std::make_unique<mixtopia::library::Library> (engine, mixtopia::settings::isTestSession()
                                                                      ? mixtopia::library::Library::getDefaultFile().getSiblingFile ("library-test-session.xml")
                                                                      : juce::File());
    libraryPanel = std::make_unique<LibraryPanel> (*library);
    libraryPanel->getPathsInMix = [this] { return getPathsInMix(); };
    libraryPanel->onPreview = [this] (const juce::String& path) { preview->play (path); };
    libraryPanel->onShowOnTimeline = [this] (const juce::String& path) { showSongOnTimeline (path); };
    libraryPanel->onStatusMessage = [this] (const juce::String& s) { setStatus (s); };
    library->onSaveFailed = [this] (const juce::String& s) { setStatus (s); };

    preview = std::make_unique<PreviewStrip> (engine, *library);
    preview->onStatusMessage = [this] (const juce::String& s) { setStatus (s); };
    preview->onActiveChanged = [this] { resized(); };
    preview->onStartedPlaying = [this]
    {
        // One output: the mix pauses while a song is previewed.
        if (edit != nullptr && edit->getTransport().isPlaying())
            edit->getTransport().stop (false, false);
    };

    topAreaShown = mixtopia::settings::getBool (showTopAreaSetting, true);
    topAreaHeight = mixtopia::settings::getFile().getIntValue (topAreaHeightSetting, getDefaultTopAreaHeight());

    for (auto* c : std::initializer_list<juce::Component*> { libraryPanel.get(), preview.get(), &mixOverview, &topDivider })
        addChildComponent (c);

    // Toolbar
    importButton.onClick  = [this] { chooseFiles(); };
    playButton.onClick    = [this] { togglePlay(); };
    stopButton.onClick    = [this] { stop(); };
    undoButton.onClick    = [this] { undo(); };
    redoButton.onClick    = [this] { edit->getUndoManager().redo(); };
    addLaneButton.onClick = [this] { timeline->addLane(); };
    zoomOutButton.onClick = [this] { timeline->zoomBy (0.5); };
    zoomInButton.onClick  = [this] { timeline->zoomBy (2.0); };
    zoomFitButton.onClick = [this] { timeline->zoomToFit(); };
    exportButton.onClick  = [this] { exportMix(); };
    saveStatus.onClick    = [this] { saveProject(); };
    topAreaButton.onClick = [this] { setTopAreaShown (! topAreaShown); };
    tempoStripButton.onClick = [this] { setTempoStripShown (! timeline->isTempoStripVisible()); };

    importButton.setTooltip ("Add song files to the mix at the playhead");
    playButton.setTooltip ("Play / pause (Space)");
    stopButton.setTooltip ("Stop and go back to the start");
    undoButton.setTooltip ("Undo (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Shift+Z)");
    exportButton.setTooltip ("Export the mix as MP3 or FLAC (Ctrl+E)");
    saveStatus.setTooltip ("Save the project (Ctrl+S)");
    addLaneButton.setTooltip ("Add a lane");
    zoomOutButton.setTooltip ("Zoom out (-)");
    zoomInButton.setTooltip ("Zoom in (+)");
    zoomFitButton.setTooltip ("Fit the whole mix in the window");
    topAreaButton.setTooltip ("Show or hide the library and mix overview (L)");
    tempoStripButton.setTooltip ("Show or hide the tempo strip");
    topAreaButton.setToggleState (topAreaShown, juce::dontSendNotification);

    setUpNumberEditor (tempoEditor, 15.0f);
    setUpLabel (tempoLabel, theme::textSmall, theme::text2);
    tempoEditor.onReturnKey = [this] { applyTempoText(); grabKeyboardFocus(); };
    tempoEditor.onFocusLost = [this] { applyTempoText(); };
    tempoEditor.onEscapeKey = [this] { updateControls(); grabKeyboardFocus(); };

    snapButton.setClickingTogglesState (true);
    snapButton.setToggleState (true, juce::dontSendNotification);
    snapButton.setTooltip ("Snap clips to beats. Hold Shift while dragging to do the opposite.");
    snapButton.onClick = [this] { timeline->setSnapEnabled (snapButton.getToggleState()); };

    // Follow: off by default, so a zoomed-in view stays put while playing (user's choice).
    followButton.setClickingTogglesState (true);
    followButton.setToggleState (false, juce::dontSendNotification);
    followButton.setTooltip ("Follow the playhead: turn the page when it reaches the edge while playing (F)");
    followButton.onClick = [this] { timeline->setFollowPlayhead (followButton.getToggleState()); };

    // Metronome: a click on every beat (accented on bar starts). Heard while playing only - it is
    // not part of an export.
    clickButton.setClickingTogglesState (true);
    clickButton.setTooltip ("Metronome: a click on every beat, louder on beat 1 of each bar");
    clickButton.onClick = [this] { edit->clickTrackEnabled = clickButton.getToggleState(); };

    // Tracktion limits the click gain to 0.2-1.0; the slider covers that range.
    clickVolume.setSliderStyle (juce::Slider::LinearHorizontal);
    clickVolume.setRange (0.2, 1.0);
    clickVolume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    clickVolume.setPopupDisplayEnabled (false, false, nullptr);
    clickVolume.setTooltip ("Metronome volume");
    clickVolume.setWantsKeyboardFocus (false);
    clickVolume.onValueChange = [this] { edit->clickTrackGain = (float) clickVolume.getValue(); };


    // Clip bar
    setUpLabel (clipNameLabel, theme::textLarge, theme::text1, true);
    clipNameLabel.setMinimumHorizontalScale (1.0f);

    for (auto* label : { &clipBpmLabel, &keyLabel })
        setUpLabel (*label, theme::textTiny, theme::text3, true);

    setUpNumberEditor (clipBpmEditor, theme::textNormal);
    clipBpmEditor.onReturnKey = [this] { applyClipBpmText(); grabKeyboardFocus(); };
    clipBpmEditor.onFocusLost = [this] { applyClipBpmText(); };
    clipBpmEditor.onEscapeKey = [this] { updateClipBar(); grabKeyboardFocus(); };

    auto scaleBpm = [this] (double factor)
    {
        if (auto clip = timeline->getSelectedClip(); clip != nullptr && tempo::getClipBpm (*clip) > 0.0)
            timeline->setSelectedClipBpm (juce::jlimit (tempo::minBpm, tempo::maxBpm, tempo::getClipBpm (*clip) * factor));
    };

    doubleBpmButton.onClick    = [scaleBpm] { scaleBpm (2.0); };
    halveBpmButton.onClick     = [scaleBpm] { scaleBpm (0.5); };
    clearBpmButton.onClick     = [this] { timeline->clearSelectedClipBpm(); };
    beatGridButton.onClick     = [this] { showBeatGridMenu(); };
    useDetectedButton.onClick  = [this] { showUseDetectedMenu(); };

    // Key (Camelot), detected on import; pick another to correct it.
    auto camelotKeys = mixtopia::key::allCamelotKeys();

    for (int i = 0; i < camelotKeys.size(); ++i)
        keyBox.addItem (camelotKeys[i] + "   (" + mixtopia::key::toMusicalName (camelotKeys[i]) + ")", i + 1);

    keyBox.setTooltip ("The song's key (Camelot). Detected automatically; pick another if it's wrong "
                       "(Use detected brings the detected one back).");
    keyBox.setWantsKeyboardFocus (false);

    // Loudness and automatic level matching (raise-only, towards loudness::referenceLufs).
    setUpLabel (loudnessLabel, theme::textSmall, theme::text2);
    loudnessLabel.setTooltip ("How loud the song sounds (integrated loudness, LUFS; higher = louder)");
    matchButton.setClickingTogglesState (true);
    matchButton.setTooltip ("Level matching: raise this song to the loudness of the others (never lowers). "
                            "Options > Match song levels switches it for the whole project.");
    matchButton.onClick = [this] { timeline->setSelectedClipMatching (matchButton.getToggleState()); };
    keyBox.onChange = [this]
    {
        if (auto id = keyBox.getSelectedId(); id > 0)
            timeline->setSelectedClipKey (mixtopia::key::allCamelotKeys()[id - 1]);

        grabKeyboardFocus();
    };

    doubleBpmButton.setTooltip ("The detected/typed BPM is half the real tempo");
    halveBpmButton.setTooltip ("The detected/typed BPM is double the real tempo");
    beatGridButton.setTooltip ("Set beat 1 at the playhead, or move the song's beat grid");
    useDetectedButton.setTooltip ("Go back to the BPM and beat 1, or the key, that Mixtopia detected (every piece of a chopped song)");
    setUpLabel (clipHintLabel, theme::textSmall, theme::text3);
    setUpLabel (loopLabel, theme::textSmall, theme::text1, true);
    loopLabel.setJustificationType (juce::Justification::centredRight);
    loopButton.setTooltip ("Repeat part of this song to make it longer (also: right-click the song)");
    loopButton.onClick = [this]
    {
        if (auto* clip = timeline->getSelectedClip())
        {
            if (mixtopia::loops::getLoop (*clip))
                openLoopPanel (clip->itemID);
            else
                timeline->repeatPartAtPlayhead (*clip);
        }
    };

    setUpLabel (deviceLabel, theme::textSmall, theme::text3);
    deviceLabel.setJustificationType (juce::Justification::centredRight);
    setUpLabel (statusLabel, theme::textSmall, theme::text2);
    setStatus ("Drop MP3, FLAC or WAV files onto a lane, or click Import. Right-click clips and lanes for more.");

    // Settings or library files that couldn't be read were set aside at start-up (CLEANUP-AUDIT C4/C10): say so, once.
    if (auto aside = mixtopia::settings::getSetAsideFiles(); ! aside.isEmpty())
    {
        juce::StringArray originals, kept;

        for (auto& path : aside)
        {
            kept.add (juce::File (path).getFileName());
            originals.add (kept.strings.getLast().upToFirstOccurrenceOf (".damaged-", false, false));
        }

        setStatus ("Couldn't read " + originals.joinIntoString (" or ") + " - kept as " + kept.joinIntoString (", ")
                     + " (in " + juce::File (aside[0]).getParentDirectory().getFullPathName() + ").");

        if (! mixtopia::settings::isTestSession())
            juce::MessageManager::callAsync ([aside]
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "A file couldn't be read",
                                                        "Mixtopia couldn't read the file below (it may have been cut off when the computer "
                                                        "crashed or the disk was full), so it started without it. Nothing in it was "
                                                        "deleted - it was kept as:\n\n" + aside.joinIntoString ("\n"));
            });
    }

    for (auto* b : std::initializer_list<juce::Button*> { &importButton, &playButton, &stopButton, &undoButton, &redoButton, &snapButton,
                                                          &followButton, &clickButton, &saveStatus, &exportButton, &addLaneButton, &zoomOutButton,
                                                          &zoomInButton, &zoomFitButton, &topAreaButton, &tempoStripButton, &doubleBpmButton,
                                                          &halveBpmButton, &clearBpmButton, &beatGridButton, &useDetectedButton, &matchButton, &loopButton })
    {
        // Keep keyboard focus on the main component so the shortcuts always work.
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &timeDisplay, &tempoLabel, &tempoEditor, &clickVolume,
                                                             &clipNameLabel, &clipBpmLabel, &clipBpmEditor,
                                                             &keyLabel, &keyBox, &loudnessLabel,
                                                             &clipHintLabel, &loopLabel, &statusLabel, &deviceLabel, &perfMeter,
                                                             &masterPanel })
        addAndMakeVisible (c);

    masterPanel.setLimiterControlVisible (mixtopia::settings::getBool (showLimiterSetting, false));
    setWantsKeyboardFocus (true);

    auto newEdit = mixtopia::createEdit (engine, getNewProjectBpm());

    if (! devSpecs.isEmpty())
        mixtopia::dev::addClipsFromSpecs (*newEdit, devSpecs);

    setEdit (std::move (newEdit));
    setSize (1280, 704);
    startTimerHz (10);

    // Once the window is up: open a project given on the command line, or after a crash offer the
    // unsaved work that was backed up (not in developer runs).
    // The dialogs run a modal loop, so they must not run inside the timer callback: JUCE's
    // callAfterDelay timer keeps firing until its callback returns, which stacked up endless
    // copies of the dialog and then crashed. The timer only posts a one-off message.
    if (devSpecs.isEmpty())
        juce::Timer::callAfterDelay (600, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::MessageManager::callAsync ([safeThis]
            {
                if (safeThis == nullptr)
                    return;

                if (safeThis->startupProject != juce::File())
                    safeThis->loadProject (safeThis->startupProject, safeThis->startupProject, false);
                else if (! safeThis->offerToRestoreUntitledBackup())
                    safeThis->openLastProjectIfWanted();
            });
        });

    if (auto view = mixtopia::dev::getOptionValues (devSpecs, "--view", 2); view.size() == 2)
        timeline->setVisibleRange (view[0].getDoubleValue(), view[1].getDoubleValue());

    if (auto lane = mixtopia::dev::getOptionValues (devSpecs, "--select", 1); lane.size() == 1)
        timeline->selectFirstClipOnLane (lane[0].getIntValue() - 1);

    if (auto lane = mixtopia::dev::getOptionValues (devSpecs, "--select-song", 1); lane.size() == 1)
        timeline->selectSongOfFirstClipOnLane (lane[0].getIntValue() - 1);

    // Developer options: songs into the (test session's) library, a song in the preview strip.
    if (auto at = devSpecs.indexOf ("--library-add"); at >= 0)
    {
        juce::Array<juce::File> files;

        for (int i = at + 1; i < devSpecs.size() && ! devSpecs[i].startsWith ("--"); ++i)
            files.add (juce::File (devSpecs[i].unquoted()));

        library->add (files);
    }

    if (auto song = mixtopia::dev::getOptionValues (devSpecs, "--preview", 1); song.size() == 1)
        juce::Timer::callAfterDelay (3000, [this, path = juce::File (song[0].unquoted()).getFullPathName()]
        {
            juce::MessageManager::callAsync ([this, path] { preview->play (path, 60.0); });
        });

    // Developer option: `--loop-panel <lane>` opens the loop panel for that lane's song (screenshots).
    if (auto lane = mixtopia::dev::getOptionValues (devSpecs, "--loop-panel", 1); lane.size() == 1)
        juce::Timer::callAfterDelay (1500, [safeThis = juce::Component::SafePointer<MainComponent> (this), index = lane[0].getIntValue() - 1]
        {
            if (safeThis != nullptr)
                if (auto* track = te::getAudioTracks (*safeThis->edit)[index])
                    if (auto* clip = track->getClips().getFirst())
                        safeThis->openLoopPanel (clip->itemID);
        });

    // Developer options (live check): `--listen-join <lane>` opens that lane's loop panel and starts
    // "Listen to the join" 5 s after opening; `--transport-loop <from> <to>` plays that range round
    // and round instead (the comparison without a loop).
    if (auto lane = mixtopia::dev::getOptionValues (devSpecs, "--listen-join", 1); lane.size() == 1)
        juce::Timer::callAfterDelay (5000, [safeThis = juce::Component::SafePointer<MainComponent> (this), index = lane[0].getIntValue() - 1]
        {
            if (safeThis != nullptr)
                if (auto* track = te::getAudioTracks (*safeThis->edit)[index])
                    if (auto* clip = track->getClips().getFirst())
                    {
                        safeThis->openLoopPanel (clip->itemID);
                        safeThis->loopPanel->startListening();
                    }
        });

    if (auto range = mixtopia::dev::getOptionValues (devSpecs, "--transport-loop", 2); range.size() == 2)
        juce::Timer::callAfterDelay (5000, [safeThis = juce::Component::SafePointer<MainComponent> (this),
                                            from = te::TimePosition::fromSeconds (range[0].getDoubleValue()),
                                            to = te::TimePosition::fromSeconds (range[1].getDoubleValue())]
        {
            if (safeThis != nullptr)
            {
                auto& transport = safeThis->edit->getTransport();
                transport.setLoopRange ({ from, to });
                transport.looping = true;
                transport.setPosition (from);
                transport.play (false);
            }
        });

    if (devSpecs.contains ("--follow"))
        followButton.setToggleState (true, juce::sendNotificationSync);

    if (devSpecs.contains ("--limiter-control"))   // shown for this session only (the setting stays)
        masterPanel.setLimiterControlVisible (true);

    // Developer option (C19 test): `--export-start <file.mp3|flac>` starts File > Export straight to that file, 3 s after
    // opening (remembered tags and Normalize; no Export window, no file browser) - to close the window mid-export.
    if (auto file = mixtopia::dev::getOptionValues (devSpecs, "--export-start", 1); file.size() == 1)
        juce::Timer::callAfterDelay (3000, [safeThis = juce::Component::SafePointer<MainComponent> (this), destination = juce::File (file[0])]
        {
            juce::MessageManager::callAsync ([safeThis, destination]   // (a modal loop must not run inside the timer)
            {
                if (safeThis != nullptr)
                    safeThis->exportMix (destination);
            });
        });

    if (devSpecs.contains ("--export-window"))   // File > Export... window (screenshots)
        juce::Timer::callAfterDelay (1500, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::MessageManager::callAsync ([safeThis]   // (a modal loop must not run inside the timer)
            {
                if (safeThis != nullptr)
                    safeThis->exportMix();
            });
        });

    if (devSpecs.contains ("--tempo-dialog"))   // Options > Default Tempo... (focus check, screenshots)
        juce::Timer::callAfterDelay (1500, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::MessageManager::callAsync ([safeThis]   // (a modal loop must not run inside the timer)
            {
                if (safeThis != nullptr)
                    safeThis->askForDefaultTempo();
            });
        });

    // Developer option (screenshots): `--clip-menu <lane>` opens the right-click menu over that lane's first song.
    if (auto lane = mixtopia::dev::getOptionValues (devSpecs, "--clip-menu", 1); lane.size() == 1)
        juce::Timer::callAfterDelay (2000, [safeThis = juce::Component::SafePointer<MainComponent> (this), lane = lane[0].getIntValue() - 1]
        {
            if (safeThis != nullptr)
                safeThis->timeline->openClipMenuOnLane (lane);
        });

    // Developer option (screenshots): `--show-menu <File|Edit|Options|View|Help>` opens that menu of the menu bar.
    if (auto name = mixtopia::dev::getOptionValues (devSpecs, "--show-menu", 1); name.size() == 1)
        juce::Timer::callAfterDelay (2000, [safeThis = juce::Component::SafePointer<MainComponent> (this), menu = name[0]]
        {
            if (safeThis != nullptr)
                safeThis->menuBar.showMenu (safeThis->getMenuBarNames().indexOf (menu));
        });

    // Developer option (screenshots): `--clip-bar-menu <grid|detected>` opens the clip bar's Beat grid or Use detected menu.
    if (auto name = mixtopia::dev::getOptionValues (devSpecs, "--clip-bar-menu", 1); name.size() == 1)
        juce::Timer::callAfterDelay (2500, [safeThis = juce::Component::SafePointer<MainComponent> (this), menu = name[0]]
        {
            if (safeThis != nullptr)
                (menu == "grid" ? safeThis->beatGridButton : safeThis->useDetectedButton).triggerClick();
        });

    if (devSpecs.contains ("--shortcuts"))   // Help > Keyboard Shortcuts... (screenshots)
        juce::Timer::callAfterDelay (1500, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safeThis != nullptr)
                safeThis->showShortcuts();
        });

    // Developer option (live check, scripts/regression/live-check.sh): `--jumps <every s> <t1> <t2> ...`
    // jumps the playhead to each time in turn while playing, the first one <every> s after --play.
    if (auto at = devSpecs.indexOf ("--jumps"); at >= 0 && at + 2 < devSpecs.size())
    {
        auto every = devSpecs[at + 1].getDoubleValue();

        for (int i = at + 2, n = 1; i < devSpecs.size() && ! devSpecs[i].startsWith ("--"); ++i, ++n)
            juce::Timer::callAfterDelay ((int) (5000 + n * every * 1000.0), [safeThis = juce::Component::SafePointer<MainComponent> (this),
                                                                             position = te::TimePosition::fromSeconds (devSpecs[i].getDoubleValue())]
            {
                if (safeThis != nullptr)
                    safeThis->edit->getTransport().setPosition (position);
            });
    }

    if (auto from = mixtopia::dev::getOptionValues (devSpecs, "--play", 1); from.size() == 1)
    {
        // Give the audio devices time to settle: Tracktion rescans them shortly after start-up and
        // that stops the transport.
        auto position = te::TimePosition::fromSeconds (from[0].getDoubleValue());
        juce::Timer::callAfterDelay (5000, [safeThis = juce::Component::SafePointer<MainComponent> (this), position]
        {
            if (safeThis != nullptr)
            {
                safeThis->edit->getTransport().setPosition (position);
                safeThis->edit->getTransport().play (false);
            }
        });
    }
}

MainComponent::~MainComponent()
{
    library->onSaveFailed = nullptr;   // (the library saves a last time when it goes, after this component)
    setEdit (nullptr);
}

//==============================================================================
void MainComponent::setEdit (std::unique_ptr<te::Edit> newEdit)
{
    loopPanel = nullptr;
    masterPanel.setEdit (nullptr);
    mixOverview.setTimeline (nullptr, nullptr);

    if (edit != nullptr)
    {
        edit->getTransport().removeChangeListener (this);
        edit->getTransport().stop (false, true);
        timeline = nullptr;
        mixtopia::destroyEdit (edit);
    }

    edit = std::move (newEdit);
    lastAutosavedStateHash = 0;

    if (edit == nullptr)
        return;

    edit->getTransport().addChangeListener (this);

    timeline = std::make_unique<TimelineComponent> (*edit);
    timeline->onStatusMessage = [this] (const juce::String& s) { setStatus (s); };
    timeline->onSelectionChanged = [this] { updateClipBar(); };
    timeline->setSnapEnabled (snapButton.getToggleState());
    timeline->setFollowPlayhead (followButton.getToggleState());
    timeline->onTempoStripWanted = [this] { setTempoStripShown (true); };
    timeline->onLoopPanelWanted = [this] (te::EditItemID id) { openLoopPanel (id); };
    loopPanel = std::make_unique<LoopPanel> (*edit, *timeline);
    loopPanel->onStatusMessage = [this] (const juce::String& s) { setStatus (s); };
    addChildComponent (*loopPanel);
    timeline->setLibrary (library.get());
    timeline->onDuplicatesFound = [this] (const std::vector<te::EditItemID>& firstCopies)
    {
        mixOverview.highlight (firstCopies);
        mixOverview.repaint();
    };
    mixOverview.setTimeline (timeline.get(), edit.get());

    // A project with tempo changes shows them (the setting is remembered).
    if (tempo::hasTempoChanges (*edit) && ! mixtopia::settings::isTestSession())
        mixtopia::settings::setBool (showTempoStripSetting, true);

    timeline->setTempoStripVisible (mixtopia::settings::getBool (showTempoStripSetting, false) || tempo::hasTempoChanges (*edit));
    timeline->setBarLinesOnSongs (mixtopia::settings::getBool (barLinesSetting, true));
    tempoStripButton.setToggleState (timeline->isTempoStripVisible(), juce::dontSendNotification);
    addAndMakeVisible (*timeline);
    loopPanel->toFront (false);

    clickButton.setToggleState (edit->clickTrackEnabled.get(), juce::dontSendNotification);
    clickVolume.setValue (edit->getClickTrackVolume(), juce::dontSendNotification);
    masterPanel.setEdit (edit.get());

    resized();
    updateControls();
    updateClipBar();
    updateTitle();
}

void MainComponent::openAtStartup (const juce::File& file)
{
    startupProject = file;
}

bool MainComponent::hasUnsavedChanges() const
{
    return edit != nullptr && (edit->hasChangedSinceSaved() || restoredFromBackup);
}

juce::String MainComponent::getProjectName() const
{
    return projectFile != juce::File() ? projectFile.getFileNameWithoutExtension() : "Untitled";
}

void MainComponent::updateTitle()
{
    if (auto* window = getTopLevelComponent(); window != nullptr && window != this)
    {
        auto title = getProjectName() + (hasUnsavedChanges() ? " *" : "") + " - Mixtopia";

        if (window->getName() != title)
            window->setName (title);
    }

    auto state = hasUnsavedChanges() ? SaveStatus::State::unsaved
               : projectFile != juce::File() ? SaveStatus::State::saved : SaveStatus::State::newProject;

    if (state != saveStatus.state)
    {
        saveStatus.state = state;
        saveStatus.repaint();
    }
}

bool MainComponent::confirmCloseProject()
{
    if (! hasUnsavedChanges() || mixtopia::settings::isTestSession())
        return true;

    auto choice = juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "Unsaved changes",
                                                          "Save the changes to \"" + getProjectName() + "\" first?",
                                                          "Save", "Don't save", "Cancel", this);

    if (choice == 0)
        return false;

    if (choice == 1)
        return saveProject();

    project::getBackupFile (projectFile).deleteFile();
    return true;
}

void MainComponent::newProject()
{
    if (! confirmCloseProject())
        return;

    setEdit (mixtopia::createEdit (engine, getNewProjectBpm()));
    projectFile = juce::File();
    restoredFromBackup = false;
    updateTitle();
    setStatus ("New project.");
}

void MainComponent::openProject()
{
    if (! confirmCloseProject())
        return;

    juce::FileChooser chooser ("Open a Mixtopia project", getDefaultFolder (projectFile), "*" + juce::String (project::fileExtension));

    if (chooser.browseForFileToOpen())
        loadProject (chooser.getResult(), chooser.getResult(), false);
}

void MainComponent::loadProject (const juce::File& file, const juce::File& readFrom, bool fromBackup)
{
    auto source = readFrom;

    // A backup newer than the project means Mixtopia didn't close normally after changes.
    if (! fromBackup && ! mixtopia::settings::isTestSession())
    {
        auto backup = project::getBackupFile (file);

        if (backup.existsAsFile() && backup.getLastModificationTime() > file.getLastModificationTime()
             && juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Newer backup found",
                                                    "Mixtopia has an automatic backup of this project from "
                                                      + backup.getLastModificationTime().toString (true, true)
                                                      + ", newer than the saved project. Open the backup instead?",
                                                    "Open backup", "Open saved project", this))
        {
            source = backup;
            fromBackup = true;
        }
    }

    auto contents = project::read (source);

    if (contents.error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBox (juce::MessageBoxIconType::WarningIcon, "Couldn't open the project", contents.error, "OK", this);
        return;
    }

    // Songs that have moved: let the user point at them. Locating one also finds any other
    // missing songs sitting in the same folder.
    while (! contents.missingFiles.isEmpty() && ! mixtopia::settings::isTestSession())
    {
        juce::String list;

        for (auto& f : contents.missingFiles)
            list << "  " << f.getFileName() << "    (was in " << f.getParentDirectory().getFullPathName() << ")\n";

        auto first = contents.missingFiles.getFirst();
        auto choice = juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::WarningIcon,
                                                              juce::String (contents.missingFiles.size())
                                                                + (contents.missingFiles.size() == 1 ? " song can't be found" : " songs can't be found"),
                                                              list + (contents.missingButCopied.isEmpty() ? juce::String()
                                                                      : contents.missingButCopied.size() == contents.missingFiles.size()
                                                                          ? "\nThey still play: Mixtopia has its own copies (e.g. the drive isn't plugged in).\n"
                                                                          : "\n" + juce::String (contents.missingButCopied.size()) + " of them still play: Mixtopia has its own copies.\n")
                                                                + "\nLocate \"" + first.getFileName() + "\"? Other missing songs in the "
                                                                "same folder will be found too.",
                                                              "Locate...", "Open without them", "Cancel", this);
        if (choice == 0)
            return;

        if (choice == 2)
            break;

        juce::FileChooser chooser ("Where is \"" + first.getFileName() + "\"?", getDefaultFolder (file), "*" + first.getFileExtension());

        if (chooser.browseForFileToOpen())
        {
            project::relink (contents, first, chooser.getResult());
            project::relinkFromFolder (contents, chooser.getResult().getParentDirectory());
        }
    }

    if (! mixtopia::decodedCache::decodeWithProgressWindow (engine, contents.filesToDecode))
    {
        setStatus ("Opening cancelled.");
        return;
    }

    auto newEdit = project::createEdit (engine, contents);

    if (newEdit == nullptr)
    {
        juce::AlertWindow::showMessageBox (juce::MessageBoxIconType::WarningIcon, "Couldn't open the project",
                                           "The project file couldn't be loaded.", "OK", this);
        return;
    }

    // Songs saved before level matching existed get their loudness measured (once; cached). That
    // alone changes nothing audible, so the project still counts as unchanged.
    auto wasChanged = newEdit->hasChangedSinceSaved();
    mixtopia::analysis::measureMissingLoudness (*newEdit);

    if (! wasChanged)
        newEdit->resetChangedStatus();

    setEdit (std::move (newEdit));
    projectFile = file;
    restoredFromBackup = fromBackup;
    rememberLastProject();
    timeline->zoomToFit();
    updateTitle();

    auto silent = contents.missingFiles.size() - contents.missingButCopied.size();
    setStatus ("Opened " + file.getFileName() + (fromBackup ? " (from the automatic backup - save to keep it)" : "")
               + (contents.missingFiles.isEmpty() ? juce::String()
                  : silent == 0 ? " - missing songs play from Mixtopia's own copies."
                  : " - " + juce::String (silent) + (silent == 1 ? " missing song has" : " missing songs have")
                      + " no copy in Mixtopia's cache and won't play."));
}

bool MainComponent::saveProject()
{
    if (projectFile == juce::File())
        return saveProjectAs();

    if (auto error = project::save (*edit, projectFile); error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBox (juce::MessageBoxIconType::WarningIcon, "Couldn't save", error, "OK", this);
        return false;
    }

    edit->resetChangedStatus();
    restoredFromBackup = false;
    rememberLastProject();
    project::getBackupFile (projectFile).deleteFile();
    project::getBackupFile ({}).deleteFile();

    updateTitle();
    setStatus ("Saved " + projectFile.getFullPathName());

    // The version from this manual save, kept apart: autosave never overwrites it. If that copy can't be written, say
    // so - the old copy (if any) is now out of date (CLEANUP-AUDIT C7).
    if (! project::keepManualSave (projectFile))
    {
        auto copy = project::getLastManualSaveFile (projectFile).getFullPathName();
        juce::Logger::writeToLog ("Couldn't keep a copy of the manual save: " + copy);
        setStatus ("Saved " + projectFile.getFullPathName() + " - but couldn't write the \"(last manual save)\" copy.");
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "The last-manual-save copy couldn't be written",
                                                "The project was saved, but its safety copy couldn't be written (is the disk full?):\n\n"
                                                  + copy + "\n\nIf an older copy is there, it is from an earlier manual save.", "OK", this);
    }

    return true;
}

// The last project opened or saved: its songs' decoded copies are kept when the cache is trimmed at
// the next start-up (Main.cpp).
void MainComponent::rememberLastProject()
{
    if (mixtopia::settings::isTestSession())
        return;

    mixtopia::settings::getFile().setValue (project::lastProjectSetting, projectFile.getFullPathName());

    // File > Open Recent: newest first, each project once.
    if (projectFile != juce::File())
    {
        auto recent = getRecentProjects();
        recent.removeString (projectFile.getFullPathName());
        recent.insert (0, projectFile.getFullPathName());
        recent.removeRange (maxRecentProjects, recent.size());
        mixtopia::settings::getFile().setValue (recentProjectsSetting, recent.joinIntoString ("\n"));
    }

    mixtopia::settings::getFile().saveIfNeeded();
}

juce::StringArray MainComponent::getRecentProjects() const
{
    auto recent = juce::StringArray::fromLines (mixtopia::settings::getFile().getValue (recentProjectsSetting));
    recent.removeEmptyStrings();
    return recent;
}

void MainComponent::openRecentProject (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        setStatus (file.getFullPathName() + " isn't there any more (moved, renamed or deleted).");
        return;
    }

    if (confirmCloseProject())
        loadProject (file, file, false);
}

void MainComponent::showShortcuts()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new mixtopia::ShortcutsList());
    options.dialogTitle = "Keyboard shortcuts";
    options.dialogBackgroundColour = mixtopia::theme::panel;
    options.componentToCentreAround = this;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::setSongCacheSize (int gb)
{
    mixtopia::decodedCache::setSizeLimitGB (gb);
    auto freed = mixtopia::decodedCache::trimToSizeLimit (project::getDecodedCopiesUsedBy (edit->state));
    setStatus ("Song cache size: " + juce::String (gb) + " GB"
               + (freed > 0 ? " - freed " + juce::File::descriptionOfSizeInBytes (freed) + " (this project's songs kept)." : "."));
}

void MainComponent::clearSongCache()
{
    auto keep = project::getDecodedCopiesUsedBy (edit->state);
    auto used = mixtopia::decodedCache::getUsedBytes();

    if (! mixtopia::settings::isTestSession()
         && ! juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::NoIcon, "Clear song cache",
                                                  "Mixtopia's copies of songs and their waveforms take "
                                                    + juce::File::descriptionOfSizeInBytes (used) + ".\n\n"
                                                    "Delete them? The songs in this project are kept. Others are unpacked "
                                                    "again from their files when you put them on the timeline (a few "
                                                    "seconds each). Your song files aren't touched.",
                                                  "Clear", "Cancel", this))
        return;

    auto freed = mixtopia::decodedCache::clear (keep);
    setStatus ("Song cache cleared: freed " + juce::File::descriptionOfSizeInBytes (freed)
               + (keep.isEmpty() ? "." : " (this project's songs kept)."));
}

double MainComponent::getNewProjectBpm()
{
    // Test sessions keep 140 so their screenshots and checks don't depend on the user's choice.
    if (mixtopia::settings::isTestSession())
        return tempo::defaultProjectBpm;

    auto bpm = parseBpm (mixtopia::settings::getFile().getValue (defaultTempoSetting));
    return bpm > 0.0 ? bpm : tempo::defaultProjectBpm;
}

void MainComponent::askForDefaultTempo()
{
    juce::AlertWindow window ("Default tempo for new projects",
                              "New projects start at this tempo. Projects you already have keep their own.",
                              juce::MessageBoxIconType::NoIcon, this);
    window.addTextEditor ("bpm", tempo::formatBpm (getNewProjectBpm()), "BPM:");
    window.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window.getTextEditor ("bpm")->setInputRestrictions (7, "0123456789.");
    mixtopia::FocusWhenShown::start (*window.getTextEditor ("bpm"));

    if (window.runModalLoop() != 1)
        return;

    auto bpm = parseBpm (window.getTextEditorContents ("bpm"));

    if (bpm <= 0.0)
    {
        setStatus ("Default tempo not changed: type a BPM from " + tempo::formatBpm (tempo::minBpm) + " to "
                   + tempo::formatBpm (tempo::maxBpm) + ".");
        return;
    }

    mixtopia::settings::getFile().setValue (defaultTempoSetting, bpm);
    mixtopia::settings::getFile().saveIfNeeded();
    setStatus ("New projects will start at " + tempo::formatBpm (bpm) + " BPM.");
}

bool MainComponent::saveProjectAs()
{
    juce::FileChooser chooser ("Save the project as", getDefaultFolder (projectFile).getChildFile (getProjectName() + project::fileExtension),
                               "*" + juce::String (project::fileExtension));

    if (! chooser.browseForFileToSave (true))
        return false;

    projectFile = chooser.getResult().withFileExtension (project::fileExtension);
    return saveProject();
}

int MainComponent::getAutosaveMinutes()
{
    auto minutes = mixtopia::settings::getFile().getIntValue (autosaveMinutesSetting, 2);
    return std::find (std::begin (autosaveChoices), std::end (autosaveChoices), minutes) != std::end (autosaveChoices) ? minutes : 2;
}

void MainComponent::setAutosaveMinutes (int minutes)
{
    mixtopia::settings::getFile().setValue (autosaveMinutesSetting, minutes);
    mixtopia::settings::getFile().saveIfNeeded();
    lastAutosaveCheck = juce::Time::getCurrentTime();
    setStatus (minutes == 0 ? "Autosave is off: save with Ctrl+S. (A hidden backup is still kept every 2 minutes in case Mixtopia closes unexpectedly.)"
                            : "Autosave every " + juce::String (minutes) + (minutes == 1 ? " minute" : " minutes")
                                + ": saved projects are saved into their own file; the last manual save is kept as \"... (last manual save)\".");
}

void MainComponent::autosaveIfNeeded()
{
    if (! hasUnsavedChanges() || edit->getLength() <= te::TimeDuration() || mixtopia::settings::isTestSession()
         || juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0)   // not while a dialog is open
        return;

    // A saved project with autosave on: saved into its own file (user's option 1, 2026-10-27). Undo
    // keeps working: saving never clears the undo history.
    if (projectFile != juce::File() && getAutosaveMinutes() > 0)
    {
        if (auto error = project::autosave (*edit, projectFile, true); error.isNotEmpty())
        {
            setStatus ("Autosave failed: " + error);
            return;
        }

        edit->resetChangedStatus();
        restoredFromBackup = false;
        updateTitle();
        setStatus ("Autosaved at " + juce::Time::getCurrentTime().toString (false, true, false, true) + ".");
        return;
    }

    // An unsaved project (or autosave off): the backup copy, which never touches a project file.
    edit->flushState();
    auto hash = edit->state.toXmlString().hashCode64();

    if (hash == lastAutosavedStateHash)
        return;

    if (auto error = project::autosave (*edit, projectFile, false); error.isEmpty())
        lastAutosavedStateHash = hash;
    else
        setStatus ("Couldn't write the automatic backup copy: " + error);   // (it was silent - CLEANUP-AUDIT C11)
}

bool MainComponent::offerToRestoreUntitledBackup()
{
    auto backup = project::getBackupFile ({});

    if (! backup.existsAsFile() || mixtopia::settings::isTestSession())
        return false;

    if (juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Restore unsaved work?",
                                            "Mixtopia closed without saving a new project. Its automatic backup is from "
                                              + backup.getLastModificationTime().toString (true, true) + ". Restore it?",
                                            "Restore", "Discard", this))
    {
        loadProject ({}, backup, true);
        return true;
    }

    backup.deleteFile();
    return false;
}

void MainComponent::openLastProjectIfWanted()
{
    if (mixtopia::settings::isTestSession() || ! mixtopia::settings::getBool (openLastProjectSetting, true))
        return;

    juce::File last (mixtopia::settings::getFile().getValue (project::lastProjectSetting));

    if (last == juce::File())
        return;

    if (last.existsAsFile())
        loadProject (last, last, false);
    else
        setStatus ("The last project (" + last.getFullPathName() + ") wasn't found, so this is a new project.");
}

void MainComponent::stopExportForQuit()
{
    // Like the progress window's Cancel button; the export then returns without a message (CLEANUP-AUDIT C19).
    stoppingExportForQuit = true;

    if (auto* window = mixtopia::runningProgressWindow())
        window->signalThreadShouldExit();
}

void MainComponent::exportMix (const juce::File& testDestination)
{
    if (edit->getLength() <= te::TimeDuration())
    {
        setStatus ("There's nothing on the timeline to export.");
        return;
    }

    // Settings (the tags are remembered between exports; empty ones aren't written). The title is the file name and
    // the track number 0, always - so a mix never needs a tag editor afterwards (user, 2026-10-27).
    auto& settingsFile = mixtopia::settings::getFile();
    auto tag = [&settingsFile] (const char* key) { return settingsFile.getValue (juce::String ("mixtopiaExportTag_") + key); };
    mixtopia::ExportDialog::Choices choices { exportFormatChoice, exportNormalise, tag ("artist"), tag ("album"), tag ("genre"), tag ("year"),
                                              juce::File (tag ("artwork")) };

    if (testDestination != juce::File())   // (developer option --export-start: no window, the format from the file name)
        choices.format = testDestination.hasFileExtension ("flac") ? 2 : 1;
    else if (! mixtopia::ExportDialog::run (this, choices))
        return;

    exportFormatChoice = choices.format;
    exportNormalise = choices.normalise;

    for (auto [key, value] : { std::pair { "artist", choices.artist }, { "album", choices.album }, { "genre", choices.genre }, { "year", choices.year } })
        settingsFile.setValue (juce::String ("mixtopiaExportTag_") + key, value);

    settingsFile.setValue ("mixtopiaExportTag_artwork", choices.artwork.getFullPathName());   // (kept even if missing now)
    settingsFile.saveIfNeeded();
    exporter::Tags tags { choices.artist, choices.album, choices.genre, choices.year, mixtopia::ExportDialog::getUsableArtwork (choices.artwork) };
    auto format = exportFormatChoice == 1 ? exporter::Format::mp3 : exporter::Format::flac;

    // Destination.
    auto destination = testDestination;

    if (destination == juce::File())
    {
        juce::FileChooser chooser ("Export the mix as", getDefaultFolder (projectFile).getChildFile (getProjectName() + exporter::getExtension (format)),
                                   "*" + exporter::getExtension (format));

        if (! chooser.browseForFileToSave (true))
            return;

        destination = chooser.getResult().withFileExtension (exporter::getExtension (format));
    }

    edit->getTransport().stop (false, false);
    const juce::ScopedValueSetter<bool> busy (exporting, true);
    stoppingExportForQuit = false;

    // Render (with its own progress window), then decide the gain.
    juce::String error;
    auto wav = exporter::renderMix (*edit, error);

    // Mixtopia is closing: no "cancelled" message box - only the status line, seen if the close is then cancelled
    // (e.g. Cancel in the unsaved-changes question; audit review of C19).
    const auto stoppedForQuit = "The export was stopped because Mixtopia was being closed - nothing was exported.";

    if (stoppingExportForQuit)
    {
        wav.deleteFile();
        setStatus (stoppedForQuit);
        return;
    }

    if (wav == juce::File())
    {
        juce::AlertWindow::showMessageBox (juce::MessageBoxIconType::WarningIcon, "Export", error, "OK", this);
        return;
    }

    auto peak = exporter::measurePeak (wav);
    auto gain = exportNormalise ? exporter::getNormalisingGain (format, peak) : 1.0f;

    // Encode.
    struct EncodeWindow : public juce::ThreadWithProgressWindow
    {
        EncodeWindow (const juce::File& w, exporter::Format f, float g, const juce::File& d, const exporter::Tags& t)
            : ThreadWithProgressWindow ("Writing " + d.getFileName(), true, true), wav (w), format (f), gain (g), destination (d), tags (t) {}

        void run() override
        {
            std::atomic<float> progress { 0.0f };
            error = exporter::encode (wav, format, gain, destination, tags, &progress, [&]
            {
                setProgress (progress.load());
                return threadShouldExit();
            });

            if (error.isEmpty())
            {
                setStatusMessage ("Measuring the loudness");
                setProgress (-1.0);
                loudness = exporter::measureLoudness (wav, [this] { return threadShouldExit(); })
                             + juce::Decibels::gainToDecibels (gain);
            }
        }

        juce::File wav;
        exporter::Format format;
        float gain;
        juce::File destination;
        exporter::Tags tags;
        juce::String error;
        double loudness = -std::numeric_limits<double>::infinity();   // of the export (LUFS)
    };

    EncodeWindow encoding (wav, format, gain, destination, tags);

    {
        const juce::ScopedValueSetter<juce::ThreadWithProgressWindow*> running (mixtopia::runningProgressWindow(), &encoding);
        encoding.runThread();
    }

    wav.deleteFile();

    if (stoppingExportForQuit)
    {
        // (a close during the loudness measuring comes after the file was saved: then it WAS exported)
        setStatus (encoding.error.isEmpty() ? "Exported " + destination.getFullPathName() + " (the loudness wasn't measured: Mixtopia was being closed)."
                                            : juce::String (stoppedForQuit));
        return;
    }

    if (encoding.error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBox (juce::MessageBoxIconType::WarningIcon, "Export", encoding.error, "OK", this);
        return;
    }

    setStatus ("Exported " + destination.getFullPathName() + (tags.artwork != juce::File() ? " with its cover" : "") + "  (loudness "
               + (std::isfinite (encoding.loudness) ? juce::String (encoding.loudness, 1) + " LUFS" : juce::String ("-"))
               + ", loudest point " + juce::String (juce::Decibels::gainToDecibels (std::min (1.0f, peak * gain)), 1) + " dB)");
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Options", "View", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int, const juce::String& menuName)
{
    juce::PopupMenu menu;

    // Shortcuts in the menu's right-hand column (File, Edit, and the right-click menus alike).
    auto withKey = [&menu] (int id, const juce::String& text, const juce::String& key, bool enabled = true)
    {
        juce::PopupMenu::Item item (text);
        item.shortcutKeyDescription = key;
        menu.addItem (item.setID (id).setEnabled (enabled));
    };

    if (menuName == "Edit")
    {
        // The editing keys, where people look for them (design review, mockup 27). They act like the keys.
        auto& um = edit->getUndoManager();
        auto* selected = timeline->getSelectedClip();
        withKey (undoItem, "Undo", "Ctrl+Z", um.canUndo());
        withKey (redoItem, "Redo", "Ctrl+Shift+Z", um.canRedo());
        menu.addSeparator();
        withKey (cutItem, "Cut at the playhead", "C");
        withKey (cutEverySongItem, "Cut every song at the playhead", "Shift+C");
        withKey (duplicateItem, "Duplicate", "Ctrl+D", selected != nullptr);
        withKey (deleteItem, "Delete", "Del", selected != nullptr);
        menu.addSeparator();
        menu.addItem (selectSongPiecesItem, "Select every piece of this song", selected != nullptr && mixtopia::chops::isPiece (*selected));
        return menu;
    }

    if (menuName == "Help")
    {
        menu.addItem (shortcutsItem, "Keyboard Shortcuts...");
        return menu;
    }

    if (menuName == "Options")
    {
        menu.addItem (autoTrimItem, "Trim silence at song starts (new songs)", true, mixtopia::analysis::isAutoTrimEnabled());
        menu.addItem (levelMatchItem, "Match song levels (this project)", true, mixtopia::levelmatch::isOnForProject (*edit));
        menu.addItem (defaultTempoItem, "Default Tempo for New Projects...  (" + tempo::formatBpm (getNewProjectBpm()) + " BPM)");
        menu.addItem (openLastProjectItem, "Open the Last Project at Start-up", true, mixtopia::settings::getBool (openLastProjectSetting, true));

        juce::PopupMenu autosave;

        for (int i = 0; i < (int) std::size (autosaveChoices); ++i)
        {
            auto minutes = autosaveChoices[i];
            autosave.addItem (autosaveItemBase + i, minutes == 0 ? juce::String ("Off")
                                                                 : juce::String (minutes) + (minutes == 1 ? " minute" : " minutes") + (minutes == 2 ? "  (default)" : ""),
                              true, minutes == getAutosaveMinutes());
        }

        menu.addSubMenu ("Autosave Every", autosave);

        // Chosen by blind listening test (2026-10-19): Signalsmith kept drums strongest with good
        // vocals; Rubber Band R3 has equally good vocals but muddier drums. Saved per project.
        juce::PopupMenu stretch;
        auto mode = tempo::getStretchMode (*edit);
        stretch.addItem (stretchSignalsmithItem, "Signalsmith (best overall)", true, mode == te::TimeStretcher::signalsmithDefault);
        stretch.addItem (stretchRubberBandItem, "Rubber Band Finer", true, mode == te::TimeStretcher::rubberbandMelodic);
        menu.addSubMenu ("Stretch (this project)", stretch);
        menu.addSeparator();

        // Decoded copies of the songs on the timeline + their waveforms, on the OS drive.
        juce::PopupMenu cacheSizes;

        for (int i = 0; i < (int) std::size (mixtopia::decodedCache::sizeChoicesGB); ++i)
        {
            auto gb = mixtopia::decodedCache::sizeChoicesGB[i];
            cacheSizes.addItem (songCacheSizeItemBase + i, juce::String (gb) + " GB" + (gb == 10 ? "  (default)" : ""),
                                true, gb == mixtopia::decodedCache::getSizeLimitGB());
        }

        menu.addSubMenu ("Song Cache Size", cacheSizes);
        menu.addItem (clearSongCacheItem, "Clear Song Cache...");
        return menu;
    }

    if (menuName == "View")
    {
        menu.addItem (libraryItem, "Library and Mix Overview  (L)", true, topAreaShown);
        menu.addItem (tempoStripItem, "Tempo Strip", true, timeline != nullptr && timeline->isTempoStripVisible());
        menu.addItem (limiterControlItem, "Limiter Control", true, mixtopia::settings::getBool (showLimiterSetting, false));
        menu.addItem (barLinesItem, "Bar Lines on Songs (zoomed in)", true, mixtopia::settings::getBool (barLinesSetting, true));
        menu.addSeparator();

        juce::PopupMenu sizes;
        auto current = juce::roundToInt (juce::Desktop::getInstance().getGlobalScaleFactor() * 100.0f);

        for (int i = 0; i < (int) std::size (interfaceScales); ++i)
            sizes.addItem (interfaceScaleItemBase + i, juce::String (interfaceScales[i]) + "%" + (interfaceScales[i] == 100 ? "  (default)" : ""),
                           true, interfaceScales[i] == current);

        menu.addSubMenu ("Interface Size", sizes);
        return menu;
    }

    withKey (newItem, "New", "Ctrl+N");
    withKey (openItem, "Open...", "Ctrl+O");

    juce::PopupMenu recentMenu;
    auto recent = getRecentProjects();

    for (int i = 0; i < recent.size(); ++i)
    {
        juce::File f (recent[i]);
        recentMenu.addItem (recentProjectItemBase + i, f.getFileNameWithoutExtension() + "   " + f.getParentDirectory().getFullPathName(),
                            f.existsAsFile());   // missing ones greyed out
    }

    if (recent.isEmpty())
        recentMenu.addItem (noRecentItem, "(no recent projects)", false);
    else
        recentMenu.addSeparator();

    recentMenu.addItem (clearRecentItem, "Clear List", ! recent.isEmpty());
    menu.addSubMenu ("Open Recent", recentMenu);
    withKey (saveItem, "Save", "Ctrl+S");
    withKey (saveAsItem, "Save As...", "Ctrl+Shift+S");
    menu.addSeparator();
    withKey (exportItem, "Export...", "Ctrl+E");
    menu.addSeparator();
    menu.addItem (quitItem, "Quit");
    return menu;
}

void MainComponent::menuItemSelected (int itemID, int)
{
    // Run after the menu has closed.
    juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<MainComponent> (this), itemID]
    {
        if (safeThis == nullptr)
            return;

        switch (itemID)
        {
            case newItem:       safeThis->newProject(); break;
            case openItem:      safeThis->openProject(); break;
            case saveItem:      safeThis->saveProject(); break;
            case saveAsItem:    safeThis->saveProjectAs(); break;
            case exportItem:    safeThis->exportMix(); break;
            case quitItem:      juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            case levelMatchItem:    safeThis->toggleLevelMatching(); break;
            case stretchSignalsmithItem:
            case stretchRubberBandItem:
                safeThis->edit->getUndoManager().beginNewTransaction ("Change stretch quality");
                tempo::setStretchMode (*safeThis->edit, itemID == stretchSignalsmithItem ? te::TimeStretcher::signalsmithDefault
                                                                                         : te::TimeStretcher::rubberbandMelodic);
                safeThis->setStatus (itemID == stretchSignalsmithItem ? "Stretching with Signalsmith (best overall)."
                                                                      : "Stretching with Rubber Band Finer.");
                break;
            case libraryItem:
                safeThis->setTopAreaShown (! safeThis->topAreaShown);
                break;
            case tempoStripItem:
                safeThis->setTempoStripShown (! safeThis->timeline->isTempoStripVisible());
                break;
            case limiterControlItem:
            {
                auto show = ! mixtopia::settings::getBool (showLimiterSetting, false);
                mixtopia::settings::setBool (showLimiterSetting, show);
                safeThis->masterPanel.setLimiterControlVisible (show);
                break;
            }
            case barLinesItem:
            {
                auto show = ! mixtopia::settings::getBool (barLinesSetting, true);
                mixtopia::settings::setBool (barLinesSetting, show);
                safeThis->timeline->setBarLinesOnSongs (show);
                break;
            }
            case clearSongCacheItem:
                safeThis->clearSongCache();
                break;
            case defaultTempoItem:
                safeThis->askForDefaultTempo();
                break;
            case openLastProjectItem:
            {
                auto on = ! mixtopia::settings::getBool (openLastProjectSetting, true);
                mixtopia::settings::setBool (openLastProjectSetting, on);
                safeThis->setStatus (on ? "Mixtopia will open the last project when it starts."
                                        : "Mixtopia will start with a new project.");
                break;
            }
            case shortcutsItem:
                safeThis->showShortcuts();
                break;
            case undoItem:              safeThis->undo(); break;
            case redoItem:              safeThis->edit->getUndoManager().redo(); break;
            case cutItem:               safeThis->timeline->cutKey (false); break;
            case cutEverySongItem:      safeThis->timeline->cutKey (true); break;
            case duplicateItem:         safeThis->timeline->duplicateSelected(); break;
            case deleteItem:            safeThis->timeline->deleteSelectedClip(); break;
            case selectSongPiecesItem:  safeThis->timeline->selectEveryPieceOfSelectedSong(); break;
            case clearRecentItem:
                mixtopia::settings::getFile().removeValue (recentProjectsSetting);
                mixtopia::settings::getFile().saveIfNeeded();
                safeThis->setStatus ("Recent projects list cleared.");
                break;
            case autoTrimItem:
                mixtopia::analysis::setAutoTrimEnabled (! mixtopia::analysis::isAutoTrimEnabled());
                safeThis->setStatus (mixtopia::analysis::isAutoTrimEnabled()
                                       ? "New songs will start where their sound starts (silence trimmed)."
                                       : "New songs will keep the silence at their start.");
                break;
            default:
                if (itemID >= interfaceScaleItemBase && itemID < interfaceScaleItemBase + (int) std::size (interfaceScales))
                    safeThis->setInterfaceScale (interfaceScales[itemID - interfaceScaleItemBase]);
                else if (itemID >= songCacheSizeItemBase && itemID < songCacheSizeItemBase + (int) std::size (mixtopia::decodedCache::sizeChoicesGB))
                    safeThis->setSongCacheSize (mixtopia::decodedCache::sizeChoicesGB[itemID - songCacheSizeItemBase]);
                else if (itemID >= autosaveItemBase && itemID < autosaveItemBase + (int) std::size (autosaveChoices))
                    safeThis->setAutosaveMinutes (autosaveChoices[itemID - autosaveItemBase]);
                else if (itemID >= recentProjectItemBase && itemID < recentProjectItemBase + maxRecentProjects)
                    safeThis->openRecentProject (juce::File (safeThis->getRecentProjects()[itemID - recentProjectItemBase]));
                break;
        }
    });
}

//==============================================================================
int MainComponent::getMinTopAreaHeight() const
{
    return LibraryPanel::controlsHeight + LibraryPanel::headerHeight + 2 * LibraryPanel::rowHeight + PreviewStrip::preferredHeight + 2;
}

int MainComponent::getDefaultTopAreaHeight() const
{
    return LibraryPanel::controlsHeight + LibraryPanel::headerHeight + 5 * LibraryPanel::rowHeight + PreviewStrip::preferredHeight + 2;
}

void MainComponent::setTopAreaShown (bool shouldShow)
{
    topAreaShown = shouldShow;
    topAreaButton.setToggleState (shouldShow, juce::dontSendNotification);

    if (! shouldShow)
        preview->stop();

    if (! mixtopia::settings::isTestSession())
        mixtopia::settings::setBool (showTopAreaSetting, shouldShow);

    resized();
}

void MainComponent::TopDivider::paint (juce::Graphics& g)
{
    // Just the gap between the cards, with a small grip in the middle (brighter under the mouse).
    g.setColour (isMouseOverOrDragging() ? theme::text2 : theme::line.brighter (0.2f));
    g.fillRoundedRectangle ((float) getWidth() / 2.0f - 20.0f, (float) getHeight() / 2.0f - 1.0f, 40.0f, 2.0f, 1.0f);
}

void MainComponent::TopDivider::mouseDrag (const juce::MouseEvent& e)
{
    owner.topAreaHeight = juce::jmax (owner.getMinTopAreaHeight(), startHeight + e.getDistanceFromDragStartY());
    owner.resized();
}

void MainComponent::TopDivider::mouseUp (const juce::MouseEvent&)
{
    if (! mixtopia::settings::isTestSession())
    {
        mixtopia::settings::getFile().setValue (topAreaHeightSetting, owner.topAreaHeight);
        mixtopia::settings::getFile().saveIfNeeded();
    }
}

std::set<juce::String> MainComponent::getPathsInMix() const
{
    std::set<juce::String> paths;

    if (edit != nullptr)
        for (auto* track : te::getAudioTracks (*edit))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                    paths.insert (mixtopia::getSourceFile (*clip).getFullPathName());

    return paths;
}

void MainComponent::showSongOnTimeline (const juce::String& path)
{
    if (timeline == nullptr)
        return;

    for (auto* clip : timeline->getClipsInStartOrder())
        if (mixtopia::getSourceFile (*clip).getFullPathName() == path)
        {
            timeline->showClip (clip->itemID);
            return;
        }
}

void MainComponent::TimeDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (theme::inset);
    g.fillRoundedRectangle (r.toFloat(), theme::controlRadius);
    r.reduce (12, 0);

    auto nowFont = theme::font (theme::textTime, true);
    g.setColour (theme::text1);
    g.setFont (nowFont);
    g.drawText (now, r.removeFromLeft (compact ? 82 : 92), juce::Justification::centredLeft);
    r.removeFromLeft (compact ? 6 : 10);

    // Each: the value over its label.
    auto column = [&] (const juce::String& value, const juce::String& label, int width)
    {
        auto c = r.removeFromLeft (width);
        g.setColour (theme::text2);
        g.setFont (theme::font (theme::textNormal, true));
        g.drawText (value, c.getX(), c.getCentreY() - 15, c.getWidth(), 16, juce::Justification::left);
        g.setColour (theme::text3);
        g.setFont (theme::font (theme::textTiny));
        g.drawText (label, c.getX(), c.getCentreY() + 1, c.getWidth(), 14, juce::Justification::left);
    };

    column (left, "left", compact ? 46 : 52);
    column (total, "total", compact ? 46 : 54);
    column (bar, "bar", r.getWidth());
}

void MainComponent::SaveStatus::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat();

    if (over && state != State::newProject)
    {
        g.setColour (theme::hover);
        g.fillRoundedRectangle (r, theme::controlRadius);
    }

    auto mark = r.removeFromLeft (26.0f);

    if (state == State::unsaved)
    {
        g.setColour (theme::warn);
        g.fillEllipse (mark.withSizeKeepingCentre (8.0f, 8.0f));
    }
    else if (state == State::saved)
    {
        theme::drawIcon (g, theme::Icon::check, mark.withSizeKeepingCentre (16.0f, 16.0f), theme::good);
    }

    if (! compact)
    {
        g.setColour (state == State::newProject ? theme::text3 : theme::text2);
        g.setFont (theme::font (theme::textSmall));
        g.drawText (state == State::unsaved ? "Unsaved changes" : state == State::saved ? "Saved" : "New project", r,
                    juce::Justification::centredLeft, true);
    }
}

void MainComponent::PerfMeter::update (float newCpu)
{
    // Test sessions show a steady 0: a live reading would make every screenshot comparison differ.
    if (mixtopia::settings::isTestSession())
        return;

    cpu = newCpu;

    // Bytes Mixtopia really read from the disk (not from the system's cache), per second.
    auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;

    if (now - lastReadTime >= 1.0)
    {
        auto io = juce::File ("/proc/self/io").loadFileAsString();
        auto at = io.indexOf ("read_bytes:");
        auto bytes = at >= 0 ? io.substring (at + 11).trim().getLargeIntValue() : (juce::int64) -1;

        if (bytes >= 0 && lastReadBytes >= 0)
            diskMBps = (double) (bytes - lastReadBytes) / (now - lastReadTime) / 1.0e6;

        lastReadBytes = bytes;
        lastReadTime = now;
    }

    repaint();
}

void MainComponent::PerfMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    auto colourFor = [] (float load) { return load > 0.9f ? theme::mute : load > 0.7f ? theme::warn : theme::accent; };

    theme::drawLabel (g, "CPU", r.removeFromLeft (30));
    auto bar = r.removeFromLeft (50).withSizeKeepingCentre (50, 6).toFloat();
    g.setColour (theme::inset);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (colourFor (cpu));
    g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, cpu))), 3.0f);
    r.removeFromLeft (6);
    g.setColour (theme::text2);
    g.setFont (theme::font (theme::textSmall));
    g.drawText (juce::String (juce::roundToInt (cpu * 100.0f)) + "%", r.removeFromLeft (40), juce::Justification::centredLeft);

    theme::drawLabel (g, "DISK", r.removeFromLeft (36));
    g.setColour (theme::text2);
    g.setFont (theme::font (theme::textSmall));
    g.drawText (juce::String (diskMBps, 1) + " MB/s", r, juce::Justification::centredLeft);
}

juce::String MainComponent::PerfMeter::getTooltip()
{
    return "CPU: how much of the time the sound card gives for each block of audio the mix uses (amber above 70%, "
           "red above 90% - near 100% the sound breaks up). Disk: how fast Mixtopia is reading songs from the disk.";
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);

    for (auto& card : cards)
        theme::drawCard (g, card.toFloat());

    for (auto& card : framedCards)
        theme::drawCard (g, card.toFloat());

    // The click volume's speaker; the selected song's colour; the clip bar's separators.
    if (clickVolume.isVisible())
        theme::drawIcon (g, theme::Icon::speaker, clickVolume.getBounds().toFloat().withX ((float) clickVolume.getX() - 20.0f).withWidth (15.0f)
                                                   .withSizeKeepingCentre (15.0f, 15.0f), theme::text2);

    if (auto clip = timeline != nullptr ? timeline->getSelectedClip() : nullptr; clip != nullptr && ! clipSwatch.isEmpty())
    {
        g.setColour (mixtopia::clipcolours::get (*clip));
        g.fillRoundedRectangle (clipSwatch.toFloat(), 2.0f);
    }

    g.setColour (theme::line);
    g.fillRect ((zoomFitButton.getRight() + topAreaButton.getX()) / 2, zoomFitButton.getY() + 6, 1, zoomFitButton.getHeight() - 12);

    for (auto& separator : clipBarSeparators)
        if (! separator.isEmpty())
            g.fillRect (separator);

    // Status bar.
    g.setColour (theme::panel);
    g.fillRect (statusBar);
    g.setColour (theme::line);
    g.fillRect (statusBar.withHeight (1));
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // Round off the corners of the cards that a component fills, and draw their border over it.
    for (auto& card : framedCards)
    {
        juce::Path corners;
        corners.addRectangle (card.toFloat());
        corners.addRoundedRectangle (card.toFloat(), theme::cardRadius);
        corners.setUsingNonZeroWinding (false);
        g.setColour (theme::bg);
        g.fillPath (corners);
        g.setColour (theme::line);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), theme::cardRadius, 1.0f);
    }
}

void MainComponent::resized()
{
    using theme::margin, theme::gap;
    cards.clear();
    framedCards.clear();

    auto r = getLocalBounds();
    menuBar.setBounds (r.removeFromTop (menuBarHeight));
    statusBar = r.removeFromBottom (statusBarHeight);
    r.reduce (margin, 0);
    r.removeFromTop (margin);
    r.removeFromBottom (margin);

    //==========================================================================
    // Toolbar: groups on the left, the transport in the middle, save/export and the view on the
    // right. If it doesn't fit, the transport moves off the centre, then some labels go.
    auto toolbar = r.removeFromTop (toolbarHeight);
    constexpr int pad = 6, inner = 4, buttonHeight = 34, square = 34;
    auto buttonY = toolbar.getY() + (toolbarHeight - buttonHeight) / 2;

    struct Item { juce::Component* c; int width; int gapAfter = inner; };
    using Group = std::vector<Item>;

    auto groupWidth = [&] (const Group& group)
    {
        auto w = 2 * pad;

        for (size_t i = 0; i < group.size(); ++i)
            w += group[i].width + (i + 1 < group.size() ? group[i].gapAfter : 0);

        return w;
    };

    auto placeGroup = [&] (const Group& group, int x)
    {
        cards.push_back ({ x, toolbar.getY(), groupWidth (group), toolbarHeight });
        x += pad;

        for (auto& item : group)
        {
            if (item.c != nullptr)
                item.c->setBounds (x, buttonY, item.width, buttonHeight);

            x += item.width + item.gapAfter;
        }
    };

    std::vector<Group> left, right;
    Group transport;

    for (int compact = 0; compact <= 2; ++compact)
    {
        importButton.setButtonText (compact >= 1 ? "" : "Import");
        addLaneButton.setButtonText (compact >= 2 ? "" : "Lane");
        exportButton.setButtonText (compact >= 2 ? "" : "Export");
        tempoLabel.setText (compact >= 1 ? "Tempo" : "Tempo here", juce::dontSendNotification);
        saveStatus.compact = compact >= 1;
        timeDisplay.compact = compact >= 2;

        auto tempoLabelWidth = juce::GlyphArrangement::getStringWidthInt (tempoLabel.getFont(), tempoLabel.getText()) + 10;

        left = { { { &importButton, importButton.getIdealWidth (buttonHeight) } },
                 { { &undoButton, 38 }, { &redoButton, 38 } },
                 { { &tempoLabel, tempoLabelWidth, 0 }, { &tempoEditor, 62, 6 }, { &snapButton, square }, { &followButton, square } },
                 { { &clickButton, square, 26 }, { &clickVolume, compact >= 2 ? 72 : 100 } } };
        transport = { { &playButton, 44 }, { &stopButton, 44, 8 }, { &timeDisplay, compact >= 2 ? 262 : 300 } };
        right = { { { &saveStatus, saveStatus.compact ? 26 : 128 }, { &exportButton, exportButton.getIdealWidth (buttonHeight) } },
                  { { &addLaneButton, addLaneButton.getIdealWidth (buttonHeight) }, { &zoomOutButton, square }, { &zoomInButton, square },
                    { &zoomFitButton, square, 13 }, { &topAreaButton, 36 }, { &tempoStripButton, 36 } } };

        auto total = groupWidth (transport) + 2 * gap;

        for (auto* side : { &left, &right })
            for (auto& group : *side)
                total += groupWidth (group) + gap;

        if (total <= toolbar.getWidth() + gap || compact == 2)
            break;
    }

    auto x = toolbar.getX();

    for (auto& group : left)
    {
        placeGroup (group, x);
        x += groupWidth (group) + gap;
    }

    auto leftEnd = x;
    auto rightX = toolbar.getRight();

    for (auto it = right.rbegin(); it != right.rend(); ++it)
    {
        rightX -= groupWidth (*it);
        placeGroup (*it, rightX);
        rightX -= gap;
    }

    auto transportWidth = groupWidth (transport);
    auto centred = toolbar.getCentreX() - transportWidth / 2;
    placeGroup (transport, juce::jlimit (leftEnd, juce::jmax (leftEnd, rightX + gap - transportWidth), centred));

    r.removeFromTop (gap);

    //==========================================================================
    // Clip bar: the selected song's colour and name, then its BPM, beat 1, key and level.
    auto clipBar = r.removeFromTop (clipBarHeight);
    cards.push_back (clipBar);
    auto bar = clipBar.reduced (10, 6);
    clipSwatch = bar.removeFromLeft (4).withSizeKeepingCentre (4, 20);
    bar.removeFromLeft (8);

    if (! clipBarShowsClip)
    {
        // Nothing selected: "No song selected" and what to do, instead of a row of greyed-out controls.
        for (auto& separator : clipBarSeparators)
            separator = {};

        auto nameWidth = juce::GlyphArrangement::getStringWidthInt (clipNameLabel.getFont(), clipNameLabel.getText()) + 12;
        clipNameLabel.setBounds (bar.removeFromLeft (nameWidth));
        bar.removeFromLeft (12);
        clipHintLabel.setBounds (bar);
        clipHintLabel.setVisible (true);
    }
    else
    {
        const int controlsWidth = 900 + 112;   // (+ the loop button)
        auto nameWidth = juce::jlimit (120, 330, bar.getWidth() - controlsWidth);
        clipNameLabel.setBounds (bar.removeFromLeft (nameWidth));
        bar.removeFromLeft (12);

        auto place = [&bar] (juce::Component& c, int width, int gapAfter = 4)
        {
            c.setBounds (bar.removeFromLeft (width));
            bar.removeFromLeft (gapAfter);
        };

        auto separatorIndex = 0;
        auto separator = [&]
        {
            bar.removeFromLeft (6);
            clipBarSeparators[separatorIndex++] = bar.removeFromLeft (1).reduced (0, 3);
            bar.removeFromLeft (11);
        };

        place (clipBpmLabel, 32, 2);
        place (clipBpmEditor, 56, 6);
        place (doubleBpmButton, 34);
        place (halveBpmButton, 34);
        place (clearBpmButton, 50, 0);
        separator();
        place (beatGridButton, 104, 0);
        separator();
        place (keyLabel, 30, 2);
        place (keyBox, 140, 8);
        place (useDetectedButton, 116, 0);
        separator();
        place (loudnessLabel, 84, 4);
        place (matchButton, 112, 16);

        // The loop (or the button to make one) at the right end; the hint in what's left.
        loopButton.setBounds (bar.removeFromRight (104));
        bar.removeFromRight (8);
        loopLabel.setBounds (bar.removeFromRight (juce::jmin (250, juce::jmax (0, bar.getWidth()))));
        clipHintLabel.setBounds (bar);
        clipHintLabel.setVisible (bar.getWidth() > 120);
    }

    r.removeFromTop (gap);

    //==========================================================================
    // Status bar: messages on the left; CPU / disk and the audio device on the right.
    {
        auto status = statusBar.reduced (14, 0);
        deviceLabel.setBounds (status.removeFromRight (juce::jmin (240, status.getWidth() / 4)));
        status.removeFromRight (16);
        perfMeter.setBounds (status.removeFromRight (240));
        status.removeFromRight (16);
        statusLabel.setBounds (status);
    }

    //==========================================================================
    // Top area: library + preview strip | mix overview.
    for (auto* c : std::initializer_list<juce::Component*> { libraryPanel.get(), &mixOverview, &topDivider })
        c->setVisible (topAreaShown);

    preview->setVisible (topAreaShown && preview->isActive());

    if (topAreaShown)
    {
        auto height = juce::jlimit (getMinTopAreaHeight(), juce::jmax (getMinTopAreaHeight(), r.getHeight() * 6 / 10), topAreaHeight);
        auto top = r.removeFromTop (height);
        auto libraryCard = top.removeFromLeft (juce::jmin (780, (top.getWidth() - gap) * 6 / 10));
        top.removeFromLeft (gap);
        framedCards.push_back (libraryCard);
        framedCards.push_back (top);

        auto libraryArea = libraryCard.reduced (1);

        if (preview->isActive())
            preview->setBounds (libraryArea.removeFromBottom (PreviewStrip::preferredHeight));

        libraryPanel->setBounds (libraryArea);
        mixOverview.setBounds (top.reduced (1));
        topDivider.setBounds (r.removeFromTop (dividerHeight));
    }

    //==========================================================================
    // Timeline and master panel.
    auto master = r.removeFromRight (MasterPanel::preferredWidth);
    r.removeFromRight (gap);
    framedCards.push_back (master);
    framedCards.push_back (r);
    masterPanel.setBounds (master.reduced (1));

    if (timeline != nullptr)
        timeline->setBounds (r.reduced (1));

    repaint();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // Compare with KeyPress objects: JUCE matches letters case-insensitively that way (on Linux
    // Ctrl+Z arrives as 'z').
    const juce::KeyPress undoKey ('z', juce::ModifierKeys::ctrlModifier, 0);
    const juce::KeyPress redoKey ('z', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0);
    const juce::KeyPress redoKeyAlt ('y', juce::ModifierKeys::ctrlModifier, 0);

    const auto ctrl = juce::ModifierKeys::ctrlModifier, shift = juce::ModifierKeys::shiftModifier;

    if (key == juce::KeyPress ('o', ctrl, 0))           { openProject(); return true; }
    if (key == juce::KeyPress ('s', ctrl, 0))           { saveProject(); return true; }
    if (key == juce::KeyPress ('s', ctrl | shift, 0))   { saveProjectAs(); return true; }
    if (key == juce::KeyPress ('e', ctrl, 0))           { exportMix(); return true; }
    if (key == juce::KeyPress ('n', ctrl, 0))           { newProject(); return true; }
    // Listening to a loop's join: Space and Esc stop that (and put things back).
    if ((key == juce::KeyPress::spaceKey || key == juce::KeyPress::escapeKey) && loopPanel != nullptr && loopPanel->isListening())
    {
        loopPanel->stopListening();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)                                     { togglePlay(); return true; }
    if (key == juce::KeyPress::homeKey)                                      { edit->getTransport().setPosition (te::TimePosition()); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { timeline->deleteSelectedClip(); return true; }
    if (key == undoKey)                                                      { undo(); return true; }
    if (key == redoKey || key == redoKeyAlt)                                 { edit->getUndoManager().redo(); return true; }

    // V/B/G/T/R: add a volume/bass/mid/treble/filter point on the song under the mouse.
    const std::pair<juce::KeyPress, mixtopia::envelopes::Param> pointKeys[] {
        { juce::KeyPress ('v'), mixtopia::envelopes::Param::volume }, { juce::KeyPress ('b'), mixtopia::envelopes::Param::bass },
        { juce::KeyPress ('g'), mixtopia::envelopes::Param::mids },   { juce::KeyPress ('t'), mixtopia::envelopes::Param::treble },
        { juce::KeyPress ('r'), mixtopia::envelopes::Param::filter }
    };

    for (auto& [pointKey, param] : pointKeys)
    {
        if (key == pointKey)
        {
            if (! timeline->addPointUnderMouse (param))
                setStatus ("Point the mouse at a song, then press the key to add a " + mixtopia::envelopes::getName (param).toLowerCase() + " point there.");

            return true;
        }
    }

    if (key == juce::KeyPress ('f'))                                         { followButton.triggerClick(); return true; }

    // Chopping: C cuts at the playhead, Shift+C every song there; Ctrl+D copies the selected piece.
    if (key == juce::KeyPress ('c'))                                         { timeline->cutKey (false); return true; }
    if (key == juce::KeyPress ('c', shift, 0))                               { timeline->cutKey (true); return true; }
    if (key == juce::KeyPress ('d', ctrl, 0))                                { timeline->duplicateSelected(); return true; }

    // Loops: I = loop in, O = loop out (at the playhead), E = the loop panel; Esc cancels a loop in.
    if (key == juce::KeyPress ('i'))                                         { timeline->loopInKey(); return true; }
    if (key == juce::KeyPress ('o'))                                         { timeline->loopOutKey(); return true; }
    if (key == juce::KeyPress::escapeKey && timeline->cancelLoopIn())
    {
        setStatus ("Loop in cancelled.");
        return true;
    }

    if (key == juce::KeyPress ('e'))
    {
        if (auto id = timeline->getSongForLoopPanel(); id.isValid())
            openLoopPanel (id);
        else
            setStatus ("No loop here: select a song with a loop, or use a quick loop on its lane (or right-click > Repeat part of this song...).");

        return true;
    }
    if (key == juce::KeyPress ('l'))                                         { setTopAreaShown (! topAreaShown); return true; }

    // The preview strip: Esc stops it; Left/Right jump 8 bars while it plays.
    if (preview->isPlaying())
    {
        if (key == juce::KeyPress::escapeKey)       { preview->stop(); return true; }
        if (key == juce::KeyPress::leftKey)         { preview->jumpBars (-8); return true; }
        if (key == juce::KeyPress::rightKey)        { preview->jumpBars (8); return true; }
    }
    if (key.getTextCharacter() == '+' || key.getTextCharacter() == '=')      { timeline->zoomBy (2.0); return true; }
    if (key.getTextCharacter() == '-')                                       { timeline->zoomBy (0.5); return true; }

    return false;
}

void MainComponent::chooseFiles()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import audio files",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.mp3;*.flac;*.wav");

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
               | juce::FileBrowserComponent::canSelectMultipleItems;

    fileChooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        juce::StringArray paths;

        for (auto& f : fc.getResults())
            paths.add (f.getFullPathName());

        // Imported files go on consecutive lanes, starting at the playhead.
        timeline->addFiles (paths, 0, edit->getTransport().getPosition());
    });
}

void MainComponent::togglePlay()
{
    auto& transport = edit->getTransport();
    preview->stop();   // one output: the preview stops when the mix plays

    if (transport.isPlaying())
        transport.stop (false, false);
    else if (edit->getLength() > te::TimeDuration())
        transport.play (false);
}

void MainComponent::stop()
{
    edit->getTransport().stop (false, false);
    edit->getTransport().setPosition (te::TimePosition());
}

void MainComponent::setTempoStripShown (bool shouldShow)
{
    if (! mixtopia::settings::isTestSession())
        mixtopia::settings::setBool (showTempoStripSetting, shouldShow);

    if (timeline != nullptr)
        timeline->setTempoStripVisible (shouldShow);

    tempoStripButton.setToggleState (shouldShow, juce::dontSendNotification);
}

int MainComponent::getSavedInterfaceScale()
{
    auto percent = mixtopia::settings::getFile().getIntValue (interfaceScaleSetting, 100);
    return std::find (std::begin (interfaceScales), std::end (interfaceScales), percent) != std::end (interfaceScales) ? percent : 100;
}

void MainComponent::setInterfaceScale (int percent)
{
    // Everything - text, buttons, panels - grows or shrinks together (JUCE's global scale). The
    // window keeps its place on the screen and is fitted inside it.
    if (! mixtopia::settings::isTestSession())
    {
        mixtopia::settings::getFile().setValue (interfaceScaleSetting, percent);
        mixtopia::settings::getFile().saveIfNeeded();
    }

    juce::Desktop::getInstance().setGlobalScaleFactor ((float) percent / 100.0f);

    if (auto* window = getTopLevelComponent())
        if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window->getScreenBounds()))
            window->setBounds (window->getBounds().constrainedWithin (display->userBounds.toNearestInt()).getIntersection (display->userBounds.toNearestInt()));

    setStatus ("Interface size " + juce::String (percent) + "%.");
}

void MainComponent::applyTempoText()
{
    // The box shows (and sets) the tempo at the playhead: the steady stretch it's in (the point(s)
    // setting it). Inside a ramp it's greyed out.
    auto bpm = parseBpm (tempoEditor.getText());
    auto plan = tempo::getTempoPlan (*edit);
    auto steady = tempo::steadyPointsAt (plan, tempo::toBeats (*edit, edit->getTransport().getPosition()));
    auto inRamp = ! plan.points.empty() && steady.empty();
    auto current = plan.points.empty() ? plan.projectBpm : inRamp ? 0.0 : plan.points[steady.front()].bpm;

    if (bpm > 0.0 && ! inRamp && std::abs (bpm - current) > 0.0001)
    {
        edit->getUndoManager().beginNewTransaction ("Change tempo");

        if (plan.points.empty())
        {
            tempo::setProjectBpm (*edit, bpm);
        }
        else
        {
            for (auto i : steady)
                plan.points[i].bpm = bpm;

            tempo::setTempoPlan (*edit, plan);
        }
    }
    else if (bpm == 0.0 && tempoEditor.getText().isNotEmpty())
    {
        setStatus ("Tempo must be between " + juce::String ((int) tempo::minBpm) + " and " + juce::String ((int) tempo::maxBpm) + " BPM.");
    }

    updateControls();
}

void MainComponent::applyClipBpmText()
{
    auto clip = timeline->getSelectedClip();

    if (clip == nullptr)
        return;

    auto bpm = parseBpm (clipBpmEditor.getText());

    if (bpm > 0.0 && std::abs (bpm - tempo::getClipBpm (*clip)) > 0.0001)
        timeline->setSelectedClipBpm (bpm);
    else if (bpm == 0.0 && clipBpmEditor.getText().isNotEmpty())
        setStatus ("Song BPM must be between " + juce::String ((int) tempo::minBpm) + " and " + juce::String ((int) tempo::maxBpm) + ".");

    updateClipBar();
}

void MainComponent::updateControls()
{
    auto playing = edit->getTransport().isPlaying();
    playButton.setIcon (playing ? theme::Icon::pause : theme::Icon::play);
    playButton.setToggleState (playing, juce::dontSendNotification);
    undoButton.setEnabled (edit->getUndoManager().canUndo());
    redoButton.setEnabled (edit->getUndoManager().canRedo());

    if (! tempoEditor.hasKeyboardFocus (true))
    {
        auto position = edit->getTransport().getPosition();
        auto inRamp = tempo::rampAt (tempo::getTempoPlan (*edit), tempo::toBeats (*edit, position)) >= 0;
        auto bpm = tempo::getProjectBpm (*edit, position);
        tempoEditor.setText (inRamp ? juce::String (bpm, 1) : tempo::formatBpm (bpm), false);
        tempoEditor.setEnabled (! inRamp);
        tempoEditor.setTooltip (inRamp ? "The playhead is inside a tempo ramp: change it on the tempo strip (View > Tempo Strip)."
                                       : "The tempo at the playhead. Typing changes the steady stretch it's in.");
    }


}

void MainComponent::updateClipBar()
{
    auto clip = timeline->getSelectedClip();
    auto hasClip = clip != nullptr;
    auto bpm = hasClip ? tempo::getClipBpm (*clip) : 0.0;

    clipNameLabel.setText (hasClip ? clip->getName() : "No song selected", juce::dontSendNotification);

    if (hasClip != clipBarShowsClip)
    {
        clipBarShowsClip = hasClip;

        for (auto* c : std::initializer_list<juce::Component*> { &clipBpmLabel, &clipBpmEditor, &doubleBpmButton, &halveBpmButton,
                                                                 &clearBpmButton, &beatGridButton, &keyLabel, &keyBox, &useDetectedButton,
                                                                 &loudnessLabel, &matchButton, &loopLabel, &loopButton })
            c->setVisible (hasClip);

        resized();
        repaint();   // the separators
    }

    if (! clipBpmEditor.hasKeyboardFocus (true))
        clipBpmEditor.setText (bpm > 0.0 ? tempo::formatBpm (bpm) : juce::String(), false);

    for (auto* c : std::initializer_list<juce::Component*> { &clipBpmLabel, &clipBpmEditor })
        c->setEnabled (hasClip);

    for (auto* c : std::initializer_list<juce::Component*> { &doubleBpmButton, &halveBpmButton, &clearBpmButton, &beatGridButton })
        c->setEnabled (bpm > 0.0);

    useDetectedButton.setEnabled (hasClip);

    auto key = hasClip ? clip->state[mixtopia::analysis::keyProperty].toString() : juce::String();
    keyBox.setSelectedId (mixtopia::key::allCamelotKeys().indexOf (key) + 1, juce::dontSendNotification);
    keyBox.setTextWhenNothingSelected (hasClip ? "not found" : "");
    keyBox.setEnabled (hasClip);
    keyLabel.setEnabled (hasClip);

    auto lufs = hasClip ? mixtopia::levelmatch::getLoudness (*clip) : std::optional<double>();
    loudnessLabel.setText (! hasClip ? juce::String() : lufs ? juce::String (*lufs, 1) + " LUFS" : juce::String ("not measured"),
                           juce::dontSendNotification);
    auto potential = hasClip ? mixtopia::levelmatch::getPotentialGainDb (*clip) : 0.0;
    auto projectOn = mixtopia::levelmatch::isOnForProject (*edit);
    matchButton.setButtonText (! hasClip || ! lufs ? "Match" : potential <= 0.05 ? "Loud enough"
                                                                               : "Match +" + juce::String (potential, 1) + " dB");
    matchButton.setToggleState (hasClip && projectOn && potential > 0.05 && mixtopia::levelmatch::isOnForClip (*clip),
                                juce::dontSendNotification);
    matchButton.setEnabled (hasClip && projectOn && potential > 0.05);

    auto loop = hasClip ? mixtopia::loops::getLoop (*clip) : std::nullopt;
    loopLabel.setText (loop ? "LOOP  " + mixtopia::loops::describe (*clip, *loop) : juce::String(), juce::dontSendNotification);
    loopButton.setButtonText (loop ? "Edit loop..." : "Repeat part...");
    loopButton.setEnabled (hasClip);

    clipHintLabel.setText (! hasClip   ? "Click a song on the timeline to see and correct its BPM, beat 1 and key here."
                           : bpm <= 0.0 ? "No beat grid: type the song's BPM and press Enter to tempo-match it."
                                        : "Lines: right-click the song, or point and press V / B / G / T / R. Drag a point to move it, right-click it to delete.",
                           juce::dontSendNotification);
}

void MainComponent::showBeatGridMenu()
{
    enum { setBeatOne = 1 };
    juce::PopupMenu menu;
    menu.addItem (juce::PopupMenu::Item ("Set beat 1 at the playhead").setID (setBeatOne)
                      .setEnabled (timeline->canSetDownbeatAtPlayhead()));
    menu.addSeparator();
    menu.addCustomItem (-1, std::make_unique<GridNudgeRow> ([safe = juce::Component::SafePointer<MainComponent> (this)] (int which)
    {
        if (safe == nullptr)
            return;

        switch (which)
        {
            case 0:  safe->timeline->shiftSelectedClipBarStart (-1); break;
            case 1:  safe->timeline->nudgeSelectedClipDownbeat (-0.010); break;
            case 2:  safe->timeline->nudgeSelectedClipDownbeat (0.010); break;
            default: safe->timeline->shiftSelectedClipBarStart (1); break;
        }
    }), nullptr, "Move the grid");

    beatGridButton.setMenuOpen (true);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&beatGridButton),
                        [safe = juce::Component::SafePointer<MainComponent> (this)] (int result)
                        {
                            if (safe == nullptr)
                                return;

                            safe->beatGridButton.setMenuOpen (false);

                            if (result == setBeatOne)
                                safe->timeline->setSelectedClipDownbeatAtPlayhead();

                            safe->grabKeyboardFocus();
                        });
}

void MainComponent::showUseDetectedMenu()
{
    enum { detectedGrid = 1, detectedKey };
    juce::PopupMenu menu;
    menu.addItem (detectedGrid, "Use the detected BPM and beat 1");
    menu.addItem (detectedKey, "Use the detected key");

    useDetectedButton.setMenuOpen (true);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&useDetectedButton),
                        [safe = juce::Component::SafePointer<MainComponent> (this)] (int result)
                        {
                            if (safe == nullptr)
                                return;

                            safe->useDetectedButton.setMenuOpen (false);

                            if (result == detectedGrid && ! safe->timeline->resetSelectedClipToDetectedGrid())
                                safe->setStatus ("No steady beat was found in this song - type its BPM instead.");
                            else if (result == detectedKey && ! safe->timeline->resetSelectedClipToDetectedKey())
                                safe->setStatus ("No key could be found for this song.");

                            safe->grabKeyboardFocus();
                        });
}

void MainComponent::toggleLevelMatching()
{
    auto turnOn = ! mixtopia::levelmatch::isOnForProject (*edit);

    if (turnOn)
        mixtopia::analysis::measureMissingLoudness (*edit);

    edit->getUndoManager().beginNewTransaction (turnOn ? "Match song levels" : "Stop matching song levels");
    edit->state.setProperty (mixtopia::levelmatch::projectMatchProperty, turnOn ? 1 : 0, &edit->getUndoManager());
    setStatus (turnOn ? "Song levels are matched: quieter songs are raised to " + juce::String (mixtopia::loudness::referenceLufs, 0)
                          + " LUFS (never lowered)."
                      : "Song levels are no longer matched.");
    updateClipBar();
}

void MainComponent::setStatus (const juce::String& message)
{
    statusLabel.setText (message, juce::dontSendNotification);
}

juce::String MainComponent::describeAudioDevice (bool full) const
{
    if (auto* device = engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
    {
        if (full)
            return "Audio output: " + device->getTypeName() + " / " + device->getName()
                     + " @ " + juce::String ((int) device->getCurrentSampleRate()) + " Hz";

        // "Default ALSA Output (currently PipeWire Media Server)" -> "PipeWire Media Server".
        auto name = device->getName();
        name = name.contains ("(currently ") ? name.fromFirstOccurrenceOf ("(currently ", false, false).upToFirstOccurrenceOf (")", false, false)
                                             : name.upToFirstOccurrenceOf (" (", false, false);
        return name + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "))
                 + juce::String (device->getCurrentSampleRate() / 1000.0, 1) + " kHz";
    }

    return "No audio output device!";
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateControls();
}

void MainComponent::timerCallback()
{
    auto& transport = edit->getTransport();

    // Stop at the end of the mix rather than playing silence forever.
    if (transport.isPlaying() && transport.getPosition() >= te::TimePosition() + edit->getLength())
    {
        juce::Logger::writeToLog ("Stopped at the end of the mix: position " + juce::String (transport.getPosition().inSeconds(), 2)
                                    + " s, mix length " + juce::String (edit->getLength().inSeconds(), 2) + " s");
        transport.stop (false, false);
    }

    if (auto now = juce::Time::getCurrentTime(); (now - lastAutosaveCheck).inSeconds() >= (getAutosaveMinutes() > 0 ? getAutosaveMinutes() * 60.0 : backupIntervalSeconds))
    {
        lastAutosaveCheck = now;
        autosaveIfNeeded();
    }

    updateTitle();
    auto now = transport.getPosition().inSeconds(), total = edit->getLength().inSeconds();
    auto nowText = formatTime (now, true), leftText = formatTime (total - now, false), totalText = formatTime (total, false);
    auto barText = formatBar (*edit, transport.getPosition());

    if (nowText != timeDisplay.now || leftText != timeDisplay.left || totalText != timeDisplay.total || barText != timeDisplay.bar)
    {
        timeDisplay.now = nowText;
        timeDisplay.left = leftText;
        timeDisplay.total = totalText;
        timeDisplay.bar = barText;
        timeDisplay.repaint();
    }

    deviceLabel.setText (describeAudioDevice(), juce::dontSendNotification);
    deviceLabel.setTooltip (describeAudioDevice (true));
    perfMeter.update (engine.getDeviceManager().getCpuUsage());

    updateControls();
}

void MainComponent::openLoopPanel (te::EditItemID id)
{
    auto* clip = dynamic_cast<te::AudioClipBase*> (te::findClipForID (*edit, id));

    if (clip == nullptr || loopPanel == nullptr || timeline == nullptr)
        return;

    // Beside the loop, on the side of the timeline with more room.
    auto range = timeline->getLoopEditRange (*clip);
    auto view = timeline->getVisibleTimeRange();
    auto area = timeline->getBounds();
    auto middle = view.getLength() > 0.0 ? (range.getStart() + range.getEnd()) / 2.0 : 0.0;
    auto x = area.getX() + (int) ((middle - view.getStart()) / juce::jmax (0.001, view.getLength()) * area.getWidth());
    auto within = area.withTop (menuBarHeight + theme::margin + toolbarHeight + theme::gap + clipBarHeight + theme::gap);
    loopPanel->showFor (id, { x, area.getY(), 1, area.getHeight() }, within, area.getY() + 12);
}

void MainComponent::undo()
{
    // A loop in waiting for its loop out goes first (it isn't an edit yet, so undo wouldn't reach it).
    if (timeline != nullptr && timeline->cancelLoopIn())
    {
        setStatus ("Loop in cancelled.");
        return;
    }

    edit->getUndoManager().undo();
}
