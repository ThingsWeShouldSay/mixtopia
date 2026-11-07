#include "Project.h"
#include "EditHelpers.h"
#include "Loops.h"
#include "Settings.h"

namespace mixtopia::project
{
    namespace
    {
        const juce::Identifier relativeSourceProperty { "mixtopiaSourceRelative" };
        const juce::Identifier versionProperty { "mixtopiaVersion" };

        void forEachAudioClip (juce::ValueTree tree, const std::function<void (juce::ValueTree)>& fn)
        {
            for (auto child : tree)
            {
                if (child.hasType (te::IDs::AUDIOCLIP))
                    fn (child);

                forEachAudioClip (child, fn);
            }
        }

        juce::File getOriginal (const juce::ValueTree& clip)
        {
            auto path = clip.getProperty (sourceFileProperty).toString();
            return path.isNotEmpty() ? juce::File (path) : juce::File (clip.getProperty (te::IDs::source).toString());
        }

        // The decoded copy a clip plays (through its .mixloop for a song with a loop), {} if none.
        juce::File getDecodedCopy (const juce::ValueTree& clip)
        {
            juce::File played (clip.getProperty (te::IDs::source).toString());

            if (loops::isLoopFile (played))
            {
                loops::Spec spec;
                played = spec.parse (played) ? spec.wav : juce::File();
            }

            if (played != juce::File() && played != getOriginal (clip) && played.hasFileExtension ("wav")
                 && played.getParentDirectory() == decodedCache::getCachedFile (played).getParentDirectory())
                return played;

            return {};
        }

        // Re-works out which songs are missing and which still need decoding.
        void refresh (Contents& contents)
        {
            contents.missingFiles.clear();
            contents.missingButCopied.clear();
            contents.filesToDecode.clear();

            forEachAudioClip (contents.state, [&] (juce::ValueTree clip)
            {
                auto original = getOriginal (clip);

                if (! original.existsAsFile())
                {
                    contents.missingFiles.addIfNotAlreadyThere (original);

                    // (e.g. the drive holding the songs isn't plugged in: the song still plays from its copy)
                    if (getDecodedCopy (clip).existsAsFile())
                        contents.missingButCopied.addIfNotAlreadyThere (original);
                }
                else if (! decodedCache::isReady (original))
                    contents.filesToDecode.addIfNotAlreadyThere (original);
            });
        }
    }

    juce::String save (te::Edit& edit, const juce::File& projectFile)
    {
        edit.flushState();
        auto state = edit.state.createCopy();
        state.setProperty (versionProperty, 1, nullptr);

        forEachAudioClip (state, [&] (juce::ValueTree clip)
        {
            clip.setProperty (relativeSourceProperty,
                              getOriginal (clip).getRelativePathFrom (projectFile.getParentDirectory()), nullptr);
        });

        auto xml = state.createXml();

        // (The same text XmlElement::writeTo writes; checked complete on disk before it replaces the project.)
        if (xml == nullptr || ! settings::replaceFileContents (projectFile, xml->toString()))
            return "Couldn't write " + projectFile.getFullPathName() + " (is the disk full?). The file on disk was left as it was.";

        return {};
    }

    Contents read (const juce::File& projectFile)
    {
        Contents contents;

        if (auto xml = juce::parseXML (projectFile))
            contents.state = juce::ValueTree::fromXml (*xml);

        if (! contents.state.hasType (te::IDs::EDIT))
        {
            contents.error = projectFile.getFileName() + " isn't a Mixtopia project, or is damaged.";
            return contents;
        }

        // Songs that moved together with the project are found relative to it.
        forEachAudioClip (contents.state, [&] (juce::ValueTree clip)
        {
            auto relative = clip.getProperty (relativeSourceProperty).toString();
            auto besideProject = projectFile.getParentDirectory().getChildFile (relative);

            if (! getOriginal (clip).existsAsFile() && relative.isNotEmpty() && besideProject.existsAsFile())
                clip.setProperty (sourceFileProperty, besideProject.getFullPathName(), nullptr);
        });

        refresh (contents);
        return contents;
    }

    void relink (Contents& contents, const juce::File& oldFile, const juce::File& newFile)
    {
        forEachAudioClip (contents.state, [&] (juce::ValueTree clip)
        {
            if (getOriginal (clip) == oldFile)
                clip.setProperty (sourceFileProperty, newFile.getFullPathName(), nullptr);
        });

        refresh (contents);
    }

    int relinkFromFolder (Contents& contents, const juce::File& folder)
    {
        int found = 0;

        for (auto& missing : juce::Array<juce::File> (contents.missingFiles))
        {
            auto candidate = folder.getChildFile (missing.getFileName());

            if (candidate.existsAsFile())
            {
                relink (contents, missing, candidate);
                ++found;
            }
        }

        return found;
    }

    std::unique_ptr<te::Edit> createEdit (te::Engine& engine, Contents& contents)
    {
        forEachAudioClip (contents.state, [&] (juce::ValueTree clip)
        {
            auto original = getOriginal (clip);
            clip.setProperty (sourceFileProperty, original.getFullPathName(), nullptr);

            // A missing song keeps pointing at its old decoded copy, which may still be cached.
            decodedCache::markUsed (getDecodedCopy (clip));

            if (original.existsAsFile())
                if (auto playable = decodedCache::getPlayableFile (engine, original); playable != juce::File())
                {
                    // A song with a loop plays its .mixloop (re-made from the loop if the cache lost it).
                    auto loop = loops::getLoop (clip);
                    clip.setProperty (te::IDs::source, (loop ? loops::getLoopFile (playable, *loop) : playable).getFullPathName(), nullptr);
                }
        });

        auto edit = te::loadEditFromState (engine, contents.state);

        if (edit == nullptr)
            return {};

        for (auto* track : te::getAudioTracks (*edit))
            for (auto* clip : track->getClips())
                if (auto* audioClip = dynamic_cast<te::AudioClipBase*> (clip))
                    prepareClip (*audioClip);   // also adds clip mixers to projects from before envelopes

        finishEdit (*edit);
        edit->resetChangedStatus();
        return edit;
    }

    juce::Array<juce::File> getDecodedCopiesUsedBy (const juce::ValueTree& editState)
    {
        juce::Array<juce::File> copies;

        forEachAudioClip (editState, [&] (juce::ValueTree clip)
        {
            if (auto copy = getDecodedCopy (clip); copy != juce::File())
                copies.addIfNotAlreadyThere (copy);

            if (auto original = getOriginal (clip); original.existsAsFile() && decodedCache::needsDecoding (original))
                copies.addIfNotAlreadyThere (decodedCache::getCachedFile (original));
        });

        return copies;
    }

    juce::Array<juce::File> getDecodedCopiesUsedBy (const juce::File& projectFile)
    {
        if (projectFile.existsAsFile())
            if (auto xml = juce::parseXML (projectFile))
                return getDecodedCopiesUsedBy (juce::ValueTree::fromXml (*xml));

        return {};
    }

    juce::File getLastManualSaveFile (const juce::File& projectFile)
    {
        return projectFile.getSiblingFile (projectFile.getFileNameWithoutExtension() + " (last manual save)" + fileExtension);
    }

    bool keepManualSave (const juce::File& projectFile)
    {
        juce::MemoryBlock data;   // (a byte-for-byte copy, checked complete before it replaces the old copy)
        return projectFile.loadFileAsData (data) && settings::replaceFileContents (getLastManualSaveFile (projectFile), data);
    }

    juce::String autosave (te::Edit& edit, const juce::File& projectFile, bool intoProjectFile)
    {
        if (! intoProjectFile || projectFile == juce::File())
            return save (edit, getBackupFile (projectFile));

        if (! getLastManualSaveFile (projectFile).existsAsFile() && projectFile.existsAsFile() && ! keepManualSave (projectFile))
            return "Couldn't keep a copy of the last manual save next to " + projectFile.getFileName();

        if (auto error = save (edit, projectFile); error.isNotEmpty())
            return error;

        getBackupFile (projectFile).deleteFile();   // (a leftover from when autosave was off)
        return {};
    }

    juce::File getBackupFile (const juce::File& projectFile)
    {
        if (projectFile != juce::File())
            return projectFile.withFileExtension (backupExtension);

        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Mixtopia/untitled").withFileExtension (backupExtension);
    }
}
