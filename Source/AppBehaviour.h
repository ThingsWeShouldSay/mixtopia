#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

namespace mixtopia
{
    // The progress window that is running now, if any (message thread only): the export's, "Preparing songs" or
    // "Measuring song loudness". Closing Mixtopia stops it first: JUCE's ThreadWithProgressWindow::runThread never
    // returns once the app is quitting (its message loop stops, so the timer that notices the finished job never fires) -
    // the program froze at 100% CPU (CLEANUP-AUDIT C19 for the export, C20 for the others).
    inline juce::ThreadWithProgressWindow*& runningProgressWindow()
    {
        static juce::ThreadWithProgressWindow* window = nullptr;
        return window;
    }

    // Mixtopia only plays back, so never open an audio input (microphone).
    struct EngineBehaviour : public te::EngineBehaviour
    {
        bool shouldOpenAudioInputByDefault() override { return false; }
    };

    // Runs long Tracktion jobs (rendering an export) with a progress window and Cancel button.
    // Tracktion's default does nothing. In headless test runs the job just runs to completion.
    struct UIBehaviour : public te::UIBehaviour
    {
        bool headless = false;

        void runTaskWithProgressBar (te::ThreadPoolJobWithProgress& task) override
        {
            if (headless)
            {
                while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
                {}

                return;
            }

            struct Window : public juce::ThreadWithProgressWindow
            {
                explicit Window (te::ThreadPoolJobWithProgress& t)
                    : ThreadWithProgressWindow (t.getJobName(), true, t.canCancel()), task (t) {}

                void run() override
                {
                    for (;;)
                    {
                        if (threadShouldExit())
                            task.signalJobShouldExit();

                        if (task.runJob() != juce::ThreadPoolJob::jobNeedsRunningAgain)
                            break;

                        setProgress (task.getCurrentTaskProgress());
                    }
                }

                te::ThreadPoolJobWithProgress& task;
            };

            Window window (task);
            const juce::ScopedValueSetter<juce::ThreadWithProgressWindow*> running (runningProgressWindow(), &window);
            window.runThread();
        }
    };
}
