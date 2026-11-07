#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// Saving and opening .mixtopia project files.
//
// A project file is the Edit's state as XML: lanes, clips (position, trims, BPM, beat 1), tempo,
// mute/solo, stretch mode, metronome. Clips play decoded copies from the cache, so the file also
// records each song's original location - absolute, and relative to the project file - and
// opening a project maps the originals back to (re-)decoded copies. Songs that can't be found
// are offered for relinking before the Edit is built.
namespace mixtopia::project
{
    inline const char* fileExtension = ".mixtopia";
    inline const char* backupExtension = ".mixtopia-backup";
    inline const char* lastProjectSetting = "mixtopiaLastProject";   // (settings.xml)

    // Writes the Edit to a project file (via a temporary file, so a failed save can't destroy the
    // previous version). Returns an error message, or {} on success.
    juce::String save (te::Edit&, const juce::File& projectFile);

    // A project read from disk but not turned into an Edit yet.
    struct Contents
    {
        juce::ValueTree state;
        juce::Array<juce::File> missingFiles;     // originals that couldn't be found
        juce::Array<juce::File> missingButCopied; // of those, the ones that still play from their decoded copy
        juce::Array<juce::File> filesToDecode;    // found originals whose decoded copy isn't cached yet
        juce::String error;
    };

    // Reads a project file and resolves its songs (absolute path, then relative to the project).
    Contents read (const juce::File& projectFile);

    // Points every clip that uses `oldFile` at `newFile` and re-checks what's missing.
    void relink (Contents&, const juce::File& oldFile, const juce::File& newFile);

    // For each missing file, looks for a file with the same name in `folder`. Returns how many
    // were found.
    int relinkFromFolder (Contents&, const juce::File& folder);

    // Builds the Edit (decoding any songs not in the cache yet, which can be slow - decode
    // `filesToDecode` first on a background thread to show progress).
    std::unique_ptr<te::Edit> createEdit (te::Engine&, Contents&);

    // The decoded copies (in the song cache) an Edit's songs play, or a project file's - kept when
    // the cache is trimmed or cleared.
    juce::Array<juce::File> getDecodedCopiesUsedBy (const juce::ValueTree& editState);
    juce::Array<juce::File> getDecodedCopiesUsedBy (const juce::File& projectFile);

    // The backup file Mixtopia autosaves to for an unsaved project, or for a saved one while
    // autosave is off (a crash safety net that never touches the project itself).
    juce::File getBackupFile (const juce::File& projectFile);

    // The copy of a project as it was at the last MANUAL save, next to it: "<name> (last manual
    // save).mixtopia". Autosave overwrites the project itself, never this (user, 2026-10-27).
    juce::File getLastManualSaveFile (const juce::File& projectFile);

    // After a manual save: keeps that version as the last manual save. Returns false if it couldn't.
    bool keepManualSave (const juce::File& projectFile);

    // Autosave. intoProjectFile (a saved project, autosave on - user's option 1): saved into the
    // project's own file; a project saved by hand before this existed first gets its last-manual-save
    // copy. Otherwise into the backup file, which never touches a project. Returns an error, or empty.
    // Never clears the undo history.
    juce::String autosave (te::Edit&, const juce::File& projectFile, bool intoProjectFile);
}
