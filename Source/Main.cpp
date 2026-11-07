#include <JuceHeader.h>
#include <unistd.h>
#include "AppBehaviour.h"
#include "AppLookAndFeel.h"
#include "EditHelpers.h"
#include "Mpg123AudioFormat.h"
#include "Loops.h"
#include "Settings.h"
#include "Metronome.h"
#include "Project.h"
#include "MasterLimiter.h"
#include "ClipMixer.h"
#include "DevSpecs.h"
#include "DecodedAudioCache.h"
#include "MainComponent.h"
#include "TestHooks.h"

// Mixtopia's start-up: the engine (formats, plugins, caches), then either a developer test hook
// (TestHooks.cpp; self-tests in SelfTests.cpp) or the main window.
class MixtopiaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Mixtopia"; }
    const juce::String getApplicationVersion() override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        lookAndFeel = std::make_unique<mixtopia::AppLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        // Diagnostic log at ~/.config/Mixtopia/mixtopia.log (trimmed to its last 1 MB on start).
        logger.reset (new juce::FileLogger (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                                .getChildFile ("Mixtopia/mixtopia.log"),
                                            "Mixtopia " + getApplicationVersion() + " started", 1024 * 1024));
        juce::Logger::setCurrentLogger (logger.get());

        auto uiBehaviour = std::make_unique<mixtopia::UIBehaviour>();
        uiBehaviour->headless = mixtopia::testhooks::isHeadless (commandLine);
        engine = std::make_unique<te::Engine> ("Mixtopia", std::move (uiBehaviour), std::make_unique<mixtopia::EngineBehaviour>());
        engine->getPluginManager().createBuiltInType<MasterLimiterPlugin>();
        engine->getPluginManager().createBuiltInType<ClipMixerPlugin>();
        mixtopia::deleteStaleTempDirectories (*engine);
        engine->getAudioFileFormatManager().addFormat ([] { return new Mpg123AudioFormat(); }, false, false);
        mixtopia::loops::install (*engine);   // songs with a repeated part (.mixloop)
        mixtopia::metronome::install (*engine);

        auto args = juce::StringArray::fromTokens (commandLine, true);

        // The song cache (decoded copies + waveforms) is trimmed to its size (Options) once per
        // start, keeping every song of the project about to be worked on: the last one opened or
        // saved (and its backup), one given on the command line, the unsaved project's backup.
        // Not in test sessions or test hooks (they leave the user's cache alone).
        if (! mixtopia::settings::isTestSession() && ! args[0].startsWith ("--"))
        {
            juce::Array<juce::File> keep;
            juce::File last (mixtopia::settings::getFile().getValue (mixtopia::project::lastProjectSetting));
            auto opened = juce::File::getCurrentWorkingDirectory().getChildFile (args[0].unquoted());

            for (auto& projectFile : { last, mixtopia::project::getBackupFile (last), opened, mixtopia::project::getBackupFile ({}) })
                if (projectFile != juce::File())
                    keep.addArray (mixtopia::project::getDecodedCopiesUsedBy (projectFile));

            mixtopia::decodedCache::trimToSizeLimit (keep);
        }

        // Developer hooks (TestHooks.cpp): `Mixtopia --self-test ...`, `--render-test ...` etc. run
        // without a window and quit with their result.
        if (auto result = mixtopia::testhooks::run (*engine, args))
        {
            setApplicationReturnValue (*result);
            quit();
            return;
        }

        // Developer hook: `Mixtopia --open <specs as for --render-test> [--view <from> <to>]
        // [--select <lane>]` opens the window with that arrangement, for screenshots.
        juce::StringArray devSpecs;

        if (args[0] == "--open")
        {
            args.remove (0);
            devSpecs = args;
        }

        // Interface size (View > Interface Size): everything scaled together. Test sessions use
        // 100% unless `--scale <percent>` is given.
        auto scale = mixtopia::settings::isTestSession() ? 100 : MainComponent::getSavedInterfaceScale();

        if (auto value = mixtopia::dev::getOptionValues (devSpecs, "--scale", 1); value.size() == 1)
            scale = value[0].getIntValue();

        juce::Desktop::getInstance().setGlobalScaleFactor ((float) juce::jlimit (50, 300, scale) / 100.0f);

        mainWindow = std::make_unique<MainWindow> (*engine, *this, devSpecs);

        // A project file given on the command line (e.g. double-clicked) is opened.
        if (auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args[0].unquoted());
            devSpecs.isEmpty() && file.hasFileExtension (mixtopia::project::fileExtension) && file.existsAsFile())
            if (auto* content = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                content->openAtStartup (file);
    }

    // Closing the window or quitting asks about unsaved changes first. During an export or while songs are being prepared
    // or measured, that work is stopped first (as its Cancel button does) and the window closes once it has (CLEANUP-AUDIT
    // C19 / C20: quitting while a progress window ran froze the program).
    void systemRequestedQuit() override
    {
        if (! waitingForWorkToStop)   // (a close already waiting for the work to stop is enough: more clicks would each
            requestQuit();            // ask about unsaved changes again, the questions inside each other - audit review)
    }

    void requestQuit()
    {
        auto* content = mainWindow != nullptr ? dynamic_cast<MainComponent*> (mainWindow->getContentComponent()) : nullptr;
        auto exporting = content != nullptr && content->isExporting();

        if (exporting || mixtopia::runningProgressWindow() != nullptr)
        {
            if (exporting)
                content->stopExportForQuit();
            else
                mixtopia::runningProgressWindow()->signalThreadShouldExit();

            waitingForWorkToStop = true;
            juce::Timer::callAfterDelay (100, [this]   // (checked again shortly; no dialog may open inside a timer callback)
            {
                juce::MessageManager::callAsync ([this]
                {
                    waitingForWorkToStop = false;
                    requestQuit();
                });
            });
            return;
        }

        if (mainWindow == nullptr || mainWindow->confirmClose())
        {
            quit();
            wakeEventLoop();
        }
    }

    // JUCE's Linux event loop can miss a quit (CLEANUP-AUDIT C20): when a modal loop ran inside the same round of events
    // (a progress window that was just stopped, a dialog), that round can look as if nothing happened, and the loop sleeps
    // until the next event - but once quitting, nothing is posted any more, so it slept for good (seen when closing
    // while a project's songs were being prepared at start-up; any mouse move woke it). A pipe the loop watches is
    // written to, so it wakes and sees the quit; it is never read, so it keeps waking it until the loop has ended (then
    // shutdown() closes it, so nothing after the loop spins on it).
    void wakeEventLoop()
    {
        if (wakePipe[0] < 0 && pipe (wakePipe) == 0)
            juce::LinuxEventLoop::registerFdCallback (wakePipe[0], [] (int) {});

        if (wakePipe[1] >= 0)
        {
            const char byte = 0;
            [[maybe_unused]] auto written = write (wakePipe[1], &byte, 1);
        }
    }

    void shutdown() override
    {
        if (wakePipe[0] >= 0)
        {
            juce::LinuxEventLoop::unregisterFdCallback (wakePipe[0]);
            close (wakePipe[0]);
            close (wakePipe[1]);
            wakePipe[0] = wakePipe[1] = -1;
        }

        // The window owns the Edit, which must be destroyed before the Engine.
        mainWindow = nullptr;
        engine = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
        juce::Logger::setCurrentLogger (nullptr);
    }

private:

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (te::Engine& engine, juce::JUCEApplication& a, const juce::StringArray& devSpecs)
            : DocumentWindow ("Mixtopia",
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons),
              app (a)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (engine, devSpecs), true);
            setResizable (true, false);
            setResizeLimits (960, 600, 10000, 10000);

            // Where it was last time; at first (and in test sessions) as big as the screen allows.
            auto saved = mixtopia::settings::getFile().getValue (windowSetting);

            // Test sessions: a fixed size, so screenshot comparisons don't depend on how much of the
            // screen the desktop allows on that run (it varied: 1080, 1043, 999 px).
            // Test sessions also open on a side screen, not the user's main one (user, 2026-10-23: the
            // test windows popped up where they were working): the left-most screen, or the right-most
            // with MIXTOPIA_TEST_SCREEN=right; the main screen only if there's no other.
            if (mixtopia::settings::isTestSession())
            {
                auto& displays = juce::Desktop::getInstance().getDisplays();
                auto* primary = displays.getPrimaryDisplay();
                auto wantRight = juce::SystemStats::getEnvironmentVariable ("MIXTOPIA_TEST_SCREEN", {}) == "right";
                const juce::Displays::Display* chosen = nullptr;

                for (auto& d : displays.displays)
                    if (&d != primary && (chosen == nullptr || (wantRight ? d.logicalBounds.getX() > chosen->logicalBounds.getX()
                                                                          : d.logicalBounds.getX() < chosen->logicalBounds.getX())))
                        chosen = &d;

                if (chosen == nullptr)
                    chosen = primary;

                auto area = chosen != nullptr ? chosen->userBounds.toNearestInt() : juce::Rectangle<int> (1920, 990);
                setBounds (juce::Rectangle<int> (juce::jmin (1920, area.getWidth()), juce::jmin (990, area.getHeight())).withCentre (area.getCentre()));
            }
            else if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
            {
                if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                    setBounds (display->userBounds.toNearestInt());
                else
                    centreWithSize (getWidth(), getHeight());
            }

            setVisible (true);
        }

        ~MainWindow() override
        {
            if (! mixtopia::settings::isTestSession())
            {
                mixtopia::settings::getFile().setValue (windowSetting, getWindowStateAsString());
                mixtopia::settings::getFile().saveIfNeeded();
            }
        }

        void closeButtonPressed() override { app.systemRequestedQuit(); }

        bool confirmClose()
        {
            auto* content = dynamic_cast<MainComponent*> (getContentComponent());
            return content == nullptr || content->confirmCloseProject();
        }

    private:
        static constexpr const char* windowSetting = "mixtopiaWindow";
        juce::JUCEApplication& app;
    };

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<mixtopia::AppLookAndFeel> lookAndFeel;   // outlives the window
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    bool waitingForWorkToStop = false;   // (see systemRequestedQuit)
    int wakePipe[2] { -1, -1 };          // (see wakeEventLoop)
};

START_JUCE_APPLICATION (MixtopiaApplication)
