#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// The developer test hooks: `Mixtopia --self-test <audio file>`, `--render-test`, `--export-test`,
// `--save-test`, `--beatgrid-test`, `--project-check`, `--placement-check`, `--library-test`,
// `--tags-test`, `--loudness-test`, `--key-test`, `--loop-dump` (see CLAUDE.md "How to build").
namespace mixtopia::testhooks
{
    // True if the command line runs a hook that needs no window (no progress dialogs either).
    bool isHeadless (const juce::String& commandLine);

    // Runs the hook named by args[0]; its exit code, or nothing if args isn't a hook.
    std::optional<int> run (te::Engine&, juce::StringArray args);
}
