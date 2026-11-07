#pragma once

#include <JuceHeader.h>
#include <unistd.h>
#include "DecodedAudioCache.h"
#include "MasterLimiter.h"
#include "ClipMixer.h"
#include "ClipTempo.h"
#include "LevelMatch.h"
#include "ClipColours.h"

namespace te = tracktion;

namespace mixtopia
{
    constexpr int defaultNumLanes = 3;   // user: songs on lanes 1 and 4 are too far apart to mix; taller lanes

    // Each Edit keeps decoded MP3/FLAC copies (~21 MB per minute of audio) in its own temp folder
    // under ~/.config/Mixtopia/Temporary. A file in that folder names the owning process, so
    // startup can delete folders left behind by sessions that have ended without touching ones
    // another running Mixtopia still uses.
    inline const char* tempOwnerFileName = "owner.pid";

    inline void claimTempDirectory (te::Edit& edit)
    {
        edit.getTempDirectory (true).getChildFile (tempOwnerFileName)
            .replaceWithText (juce::String ((int) getpid()));
    }

    inline void deleteStaleTempDirectories (te::Engine& engine)
    {
        auto tempRoot = engine.getTemporaryFileManager().getTempDirectory();

        for (auto& dir : tempRoot.findChildFiles (juce::File::findDirectories, false, "edit_*"))
        {
            auto pid = dir.getChildFile (tempOwnerFileName).loadFileAsString().getIntValue();

            if (pid <= 0 || ! juce::File ("/proc/" + juce::String (pid)).isDirectory())
                dir.deleteRecursively();
        }
    }

    // Destroys an Edit created by createEdit() and deletes its temp folder.
    inline void destroyEdit (std::unique_ptr<te::Edit>& edit)
    {
        if (edit == nullptr)
            return;

        auto tempDir = edit->getTempDirectory (false);
        edit = nullptr;
        tempDir.deleteRecursively();
    }

    // Setup every audio clip needs, whether just added or loaded from a project.
    inline void prepareClip (te::AudioClipBase& clip)
    {
        // Time-stretch in real time rather than via pre-rendered proxy files, so tempo and beat
        // grid edits apply instantly.
        clip.setUsesProxy (false);

        // Volume / EQ envelope lines.
        ClipMixerPlugin::ensureOnClip (clip);
    }

    // The last steps for every Edit, whether new or loaded from a project.
    inline void finishEdit (te::Edit& edit)
    {
        // Also adds one to projects from before it existed, and puts old three-knob settings on
        // the one-knob scheme.
        if (auto* limiter = MasterLimiterPlugin::ensureOnMaster (edit))
            limiter->applyOneKnobScheme();
        clipcolours::assignMissing (edit);            // projects from before song colours
        edit.getUndoManager().clearUndoHistory();
        claimTempDirectory (edit);
    }

    // Every Mixtopia Edit must be created through here so playback and export share the same
    // settings. New projects from the menu pass the user's default tempo (Options).
    inline std::unique_ptr<te::Edit> createEdit (te::Engine& engine, double bpm = tempo::defaultProjectBpm)
    {
        auto edit = te::Edit::createSingleTrackEdit (engine);

        // Tracktion's default master volume is -3 dB; a mixing tool must play files at their
        // real level.
        edit->setMasterVolumeSliderPos (te::decibelsToVolumeFaderPosition (0.0f));
        edit->ensureNumberOfAudioTracks (defaultNumLanes);

        if (auto* tempo = edit->tempoSequence.getTempo (0))
            tempo->setBpm (bpm);

        // Automatic level matching: on for new projects (LevelMatch.h).
        edit->state.setProperty (levelmatch::projectMatchProperty, 1, nullptr);
        edit->clickTrackGain = 0.8f;
        edit->clickTrackEmphasiseBars = true;
        edit->clickTrackRecordingOnly = false;
        finishEdit (*edit);
        return edit;
    }

    inline bool isSupportedAudioFile (const juce::File& f)
    {
        return f.hasFileExtension ("mp3;flac;wav");
    }

    // Turns dropped text (one file:// URI or path per line) into local file paths.
    inline juce::StringArray filesFromDroppedText (const juce::String& text)
    {
        juce::StringArray files;

        for (auto line : juce::StringArray::fromLines (text))
        {
            line = line.trim();

            if (line.startsWith ("file://"))
                files.add (juce::URL (line).getLocalFile().getFullPathName());
            else if (juce::File::isAbsolutePath (line))
                files.add (line);
        }

        return files;
    }

    // Whether a lane is heard, given mute and solo (any soloed lane silences the unsoloed ones).
    inline bool isLaneAudible (te::AudioTrack& track)
    {
        auto anySolo = false;

        for (auto* t : te::getAudioTracks (track.edit))
            anySolo = anySolo || t->isSolo (false);

        return ! track.isMuted (false) && (! anySolo || track.isSolo (false));
    }

    // The tracks a render/export should include: what you hear. (Tracktion's Renderer un-mutes
    // whatever tracks it's given, so muted/unsoloed lanes must be left out here.)
    inline juce::BigInteger getAudibleTracksForRender (te::Edit& edit)
    {
        juce::BigInteger tracks;
        auto allTracks = te::getAllTracks (edit);

        for (auto* track : te::getAudioTracks (edit))
            if (isLaneAudible (*track))
                tracks.setBit (allTracks.indexOf (track));

        return tracks;
    }

    // Clips remember the file the user added (MP3/FLAC/WAV); what they play may be a decoded copy.
    inline const juce::Identifier sourceFileProperty { "mixtopiaSourceFile" };

    inline juce::File getSourceFile (const te::AudioClipBase& clip)
    {
        auto path = clip.state.getProperty (sourceFileProperty).toString();
        return path.isNotEmpty() ? juce::File (path) : clip.getOriginalFile();
    }

    // Adds a file as a clip on the given track, starting at the given time. MP3/FLAC are decoded
    // first if they aren't in the cache yet (slow for big files: pre-decode on a background thread
    // where it matters). Returns nullptr if the file can't be read.
    inline te::WaveAudioClip::Ptr insertAudioFile (te::AudioTrack& track, const juce::File& file,
                                                   te::TimePosition start)
    {
        auto playable = decodedCache::getPlayableFile (track.edit.engine, file);
        te::AudioFile audioFile (track.edit.engine, playable);

        if (playable == juce::File() || ! audioFile.isValid())
            return {};

        auto length = te::TimeDuration::fromSeconds (audioFile.getLength());
        auto clip = track.insertWaveClip (file.getFileNameWithoutExtension(), playable,
                                          { { start, start + length }, te::TimeDuration() }, false);

        if (clip != nullptr)
        {
            clip->state.setProperty (sourceFileProperty, file.getFullPathName(), &track.edit.getUndoManager());
            prepareClip (*clip);
            clipcolours::assignNew (*clip);
        }

        return clip;
    }
}
