#pragma once

#include <JuceHeader.h>
#include "TimelineComponent.h"
#include "MasterPanel.h"
#include "Library.h"
#include "LibraryPanel.h"
#include "PreviewStrip.h"
#include "MixOverview.h"
#include "LoopPanel.h"
#include "Theme.h"

namespace te = tracktion;

// The main window contents: a menu bar, a toolbar (import, undo, tempo, snap, follow, metronome; the
// transport and time in the middle; save status, export, lanes, zoom and view toggles on the right),
// a clip bar for the selected song's BPM, beat grid, key and level, the library and mix overview,
// the timeline with the master panel, and a status bar (with CPU and disk use). Owns the current
// project (Edit) and handles new/open/save/export and autosaved backups.
//
// Layout (M8, Mockups/16-18): everything sits on cards (theme::drawCard) with an 8 px outer margin
// and 6 px gaps; the toolbar is grouped into cards. A narrow window (or a big interface size)
// first un-centres the transport, then drops some button labels.
class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      public juce::DragAndDropContainer,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    // devSpecs: optional developer arrangement (see DevSpecs.h) to open with.
    MainComponent (te::Engine&, const juce::StringArray& devSpecs = {});
    ~MainComponent() override;

    // Asks to save unsaved changes. Returns false if the user cancelled.
    bool confirmCloseProject();

    // While an export runs, closing Mixtopia first stops it (CLEANUP-AUDIT C19); Main.cpp quits once isExporting() is false.
    bool isExporting() const    { return exporting; }
    void stopExportForQuit();

    // Opens a project once the window is up (e.g. one given on the command line).
    void openAtStartup (const juce::File& projectFile);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int, int) override;

private:
    // Projects
    void setEdit (std::unique_ptr<te::Edit>);
    void newProject();
    void openProject();
    void loadProject (const juce::File& projectFile, const juce::File& readFrom, bool fromBackup);
    bool saveProject();
    bool saveProjectAs();
    void exportMix (const juce::File& testDestination = {});   // (testDestination: developer option --export-start)
    void autosaveIfNeeded();
    static int getAutosaveMinutes();   // Options > Autosave Every (0 = off; default 2)
    void setAutosaveMinutes (int);
    bool offerToRestoreUntitledBackup();   // true if the backup was restored
    void openLastProjectIfWanted();        // Options > Open the Last Project at Start-up
    bool hasUnsavedChanges() const;
    juce::String getProjectName() const;
    void updateTitle();

    void chooseFiles();
    void undo();
    void togglePlay();
    void stop();
    void updateControls();
    void updateClipBar();
    void showBeatGridMenu();
    void showUseDetectedMenu();
    void applyTempoText();
    void applyClipBpmText();
    void setStatus (const juce::String&);
    void toggleLevelMatching();
    void setInterfaceScale (int percent);
    void setSongCacheSize (int gb);
    void clearSongCache();
    void askForDefaultTempo();
    static double getNewProjectBpm();   // Options > Default Tempo for New Projects (140 until changed)
    void rememberLastProject();
    juce::StringArray getRecentProjects() const;   // File > Open Recent, newest first
    void openRecentProject (const juce::File&);
    void showShortcuts();
    juce::String describeAudioDevice (bool full = false) const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // The top area (M7): song library + preview strip on the left, mix overview on the right; L or
    // View hides it; the divider under it sets its height (remembered).
    void setTopAreaShown (bool);
    std::set<juce::String> getPathsInMix() const;
    void showSongOnTimeline (const juce::String& path);
    void timerCallback() override;

    te::Engine& engine;
    std::unique_ptr<mixtopia::library::Library> library;   // before everything that uses it
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<TimelineComponent> timeline;
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::File projectFile;                 // empty until first saved
    juce::File startupProject;              // to open once the window is up
    bool restoredFromBackup = false;        // counts as unsaved until saved
    bool exporting = false, stoppingExportForQuit = false;   // (C19: see stopExportForQuit)
    juce::int64 lastAutosavedStateHash = 0;
    juce::Time lastAutosaveCheck;
    int exportFormatChoice = 1;             // 1 = MP3, 2 = FLAC
    bool exportNormalise = true;

    juce::MenuBarComponent menuBar { this };

    // Toolbar
    using IconButton = mixtopia::theme::IconButton;
    using Icon = mixtopia::theme::Icon;
    IconButton importButton { Icon::importFile, "Import" };
    IconButton playButton { Icon::play };
    IconButton stopButton { Icon::stop };
    // Current time (big), then time left, total time and the bar - each a value over a small label.
    struct TimeDisplay : public juce::Component
    {
        juce::String now, left, total, bar;
        bool compact = false;
        void paint (juce::Graphics&) override;
    } timeDisplay;
    IconButton undoButton { Icon::undo };
    IconButton redoButton { Icon::redo };
    juce::Label tempoLabel { {}, "Tempo here" };
    juce::TextEditor tempoEditor;
    IconButton snapButton { Icon::snap };
    IconButton followButton { Icon::follow };
    IconButton clickButton { Icon::click };
    juce::Slider clickVolume;

    // Save status ("Unsaved changes" / "Saved"; a click saves) and Export.
    struct SaveStatus : public juce::Button
    {
        SaveStatus() : juce::Button ("Save status")     { setWantsKeyboardFocus (false); }
        void paintButton (juce::Graphics&, bool over, bool down) override;
        enum class State { newProject, unsaved, saved } state = State::newProject;
        bool compact = false;
    } saveStatus;
    IconButton exportButton { Icon::exportFile, "Export" };

    IconButton addLaneButton { Icon::lane, "Lane" };
    IconButton zoomOutButton { Icon::zoomOut };
    IconButton zoomInButton { Icon::zoomIn };
    IconButton zoomFitButton { Icon::fit };
    IconButton topAreaButton { Icon::topArea };
    IconButton tempoStripButton { Icon::tempoStrip };

    // Clip bar (acts on the selected clip)
    juce::Label clipNameLabel;
    juce::Label clipBpmLabel { {}, "BPM" };
    juce::TextEditor clipBpmEditor;
    juce::TextButton doubleBpmButton { "x2" };
    juce::TextButton halveBpmButton { "/2" };
    juce::TextButton clearBpmButton { "Clear" };
    mixtopia::theme::DropDownButton beatGridButton { "Beat grid" };       // Set beat 1 at the playhead + the grid nudges
    juce::Label keyLabel { {}, "KEY" };
    juce::ComboBox keyBox;
    mixtopia::theme::DropDownButton useDetectedButton { "Use detected" };  // the detected BPM and beat 1 / key
    juce::Label loudnessLabel;
    juce::TextButton matchButton { "Match" };
    juce::Label clipHintLabel;
    juce::Label loopLabel;              // "LOOP  bar 97–105  ×3  +24 bars"
    juce::TextButton loopButton;        // "Edit loop" / "Repeat part..."
    std::unique_ptr<LoopPanel> loopPanel;
    void openLoopPanel (te::EditItemID);

    juce::Label statusLabel, deviceLabel;

    // CPU and disk use, bottom right. CPU: the share of each audio block's time the mix takes
    // (Tracktion's measure); disk: how fast Mixtopia reads from the disk (/proc/self/io).
    struct PerfMeter : public juce::Component, public juce::TooltipClient
    {
        void update (float cpu);
        void paint (juce::Graphics&) override;
        juce::String getTooltip() override;
        float cpu = 0.0f;
        double diskMBps = 0.0;
        juce::int64 lastReadBytes = -1;
        double lastReadTime = 0.0;
    } perfMeter;

    MasterPanel masterPanel;

    std::unique_ptr<LibraryPanel> libraryPanel;
    std::unique_ptr<PreviewStrip> preview;
    MixOverview mixOverview;

    struct TopDivider : public juce::Component
    {
        explicit TopDivider (MainComponent& o) : owner (o)  { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override   { startHeight = owner.topAreaHeight; }
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        MainComponent& owner;
        int startHeight = 0;
    } topDivider { *this };

    bool topAreaShown = true;
    int topAreaHeight = 0;
    static constexpr const char* showTopAreaSetting = "mixtopiaShowLibrary";
    static constexpr const char* topAreaHeightSetting = "mixtopiaTopAreaHeight";
    static constexpr int dividerHeight = mixtopia::theme::gap;
    int getMinTopAreaHeight() const;
    int getDefaultTopAreaHeight() const;
    static constexpr const char* showLimiterSetting = "mixtopiaShowLimiterControl";
    static constexpr const char* showTempoStripSetting = "mixtopiaShowTempoStrip";
    static constexpr const char* barLinesSetting = "mixtopiaBarLinesOnSongs";
    static constexpr const char* interfaceScaleSetting = "mixtopiaInterfaceScale";
    static constexpr const char* defaultTempoSetting = "mixtopiaDefaultProjectBpm";
    static constexpr const char* openLastProjectSetting = "mixtopiaOpenLastProject";
    static constexpr const char* recentProjectsSetting = "mixtopiaRecentProjects";
    static constexpr int maxRecentProjects = 10;
    static constexpr const char* autosaveMinutesSetting = "mixtopiaAutosaveMinutes";
    static constexpr int autosaveChoices[] { 0, 1, 2, 5, 10 };   // minutes; 0 = off
    void setTempoStripShown (bool);
    juce::TooltipWindow tooltipWindow { this, 700 };   // shows the tooltips of the knobs and the meters

    // The cards: painted behind (toolbar groups, clip bar) or around (library, overview, timeline,
    // master: their corners are rounded off over the child) the controls.
    std::vector<juce::Rectangle<int>> cards, framedCards;
    juce::Rectangle<int> clipSwatch, clipBarSeparators[3], statusBar;
    bool clipBarShowsClip = true;       // false: the controls are hidden and the bar says how to pick a song

public:
    static constexpr int menuBarHeight = 28;
    static constexpr int toolbarHeight = 48;
    static constexpr int clipBarHeight = 38;
    static constexpr int statusBarHeight = 26;

    // Interface sizes offered (View > Interface Size), in percent.
    static constexpr int interfaceScales[] { 90, 100, 110, 125, 150 };
    static int getSavedInterfaceScale();

private:

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
