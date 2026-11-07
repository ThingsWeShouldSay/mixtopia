#include "SongAnalysis.h"
#include "DecodedAudioCache.h"
#include "ClipTempo.h"
#include "Settings.h"
#include "AppBehaviour.h"
#include "KeyDetection.h"
#include "Loudness.h"
#include "LevelMatch.h"
#include "Loops.h"

namespace mixtopia::analysis
{
    namespace
    {
        // Bump when the analysis changes, so old cached results are worked out again.
        constexpr int analysisVersion = 4;
        constexpr float soundThreshold = 0.001f;        // -60 dBFS
        constexpr double trimMarginSeconds = 0.020;
        const char* autoTrimSetting = "mixtopiaAutoTrim";

        // Seconds into the file of the first sample louder than -60 dBFS (any channel).
        double findSoundStart (juce::AudioFormatReader& reader)
        {
            constexpr int chunk = 65536;
            juce::AudioBuffer<float> buffer ((int) reader.numChannels, chunk);
            auto limit = juce::jmin (reader.lengthInSamples, (juce::int64) (120.0 * reader.sampleRate));

            for (juce::int64 pos = 0; pos < limit; pos += chunk)
            {
                auto num = (int) juce::jmin ((juce::int64) chunk, limit - pos);

                if (! reader.read (&buffer, 0, num, pos, true, true))
                    return 0.0;

                for (int i = 0; i < num; ++i)
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        if (std::abs (buffer.getSample (ch, i)) > soundThreshold)
                            return (double) (pos + i) / reader.sampleRate;
            }

            return 0.0;   // silent (or no sound in the first two minutes): don't trim
        }

        double findSilentUntil (const SongInfo& info)
        {
            auto limit = juce::jmax (0.0, info.soundStart - trimMarginSeconds);

            // Not past the beat line where the sound begins.
            if (info.grid.valid && info.grid.bpm > 0.0)
            {
                auto beat = 60.0 / info.grid.bpm;
                auto line = info.grid.firstBeat + std::floor ((info.soundStart - info.grid.firstBeat + 0.005) / beat) * beat;

                if (line >= 0.0)
                    limit = juce::jmin (limit, line);
            }

            return limit;
        }

        // Named by the song's key: the same for the song file and its decoded copy (the library
        // analyses the file, the timeline plays the copy). Files named the old way (by the decoded
        // copy itself) are renamed.
        juce::File getCacheFile (const juce::File& playable)
        {
            auto folder = decodedCache::getCacheFolder ("analysis");
            auto file = folder.getChildFile (decodedCache::getSongKey (playable) + ".xml");

            if (auto old = folder.getChildFile (decodedCache::getCacheKey (playable) + ".xml"); old != file && old.existsAsFile())
            {
                if (file.existsAsFile())
                    old.deleteFile();
                else
                    old.moveFileTo (file);
            }

            return file;
        }

        void store (const juce::File& playable, const SongInfo& info)
        {
            juce::XmlElement xml ("MIXTOPIA_ANALYSIS");
            xml.setAttribute ("version", analysisVersion);
            auto* grid = xml.createNewChildElement ("BEATGRID");
            grid->setAttribute ("valid", info.grid.valid ? 1 : 0);
            grid->setAttribute ("bpm", info.grid.bpm);
            grid->setAttribute ("measuredBpm", info.grid.measuredBpm);
            grid->setAttribute ("firstBeat", info.grid.firstBeat);
            grid->setAttribute ("downbeat", info.grid.downbeat);
            grid->setAttribute ("driftMs", info.grid.driftMs);
            xml.setAttribute ("soundStart", info.soundStart);
            xml.setAttribute ("silentUntil", info.silentUntil);
            xml.setAttribute ("key", info.key);
            xml.setAttribute ("loudness", std::isfinite (info.loudness) ? juce::String (info.loudness, 2) : juce::String ("-inf"));

            auto file = getCacheFile (playable);
            file.getParentDirectory().createDirectory();
            xml.writeTo (file);
        }
    }

    std::optional<SongInfo> getCached (const juce::File& playable)
    {
        auto xml = juce::parseXML (getCacheFile (playable));

        if (xml == nullptr || xml->getIntAttribute ("version") != analysisVersion)
            return {};

        SongInfo info;
        info.soundStart = xml->getDoubleAttribute ("soundStart");
        info.silentUntil = xml->getDoubleAttribute ("silentUntil");
        info.key = xml->getStringAttribute ("key");
        info.loudness = xml->getStringAttribute ("loudness") == "-inf" ? -std::numeric_limits<double>::infinity()
                                                                       : xml->getDoubleAttribute ("loudness");

        if (auto* grid = xml->getChildByName ("BEATGRID"))
        {
            info.grid.valid = grid->getIntAttribute ("valid") != 0;
            info.grid.bpm = grid->getDoubleAttribute ("bpm");
            info.grid.measuredBpm = grid->getDoubleAttribute ("measuredBpm");
            info.grid.firstBeat = grid->getDoubleAttribute ("firstBeat");
            info.grid.downbeat = grid->getDoubleAttribute ("downbeat");
            info.grid.driftMs = grid->getDoubleAttribute ("driftMs");
        }

        return info;
    }

    SongInfo analyseReader (juce::AudioFormatReader& reader, const std::function<bool()>& shouldStop)
    {
        SongInfo info;
        info.grid = beatgrid::analyse (reader, shouldStop);
        info.soundStart = findSoundStart (reader);
        info.silentUntil = findSilentUntil (info);
        info.key = key::analyse (reader, shouldStop);
        info.loudness = loudness::measure (reader, shouldStop);
        return info;
    }

    std::optional<SongInfo> analyse (te::Engine& engine, const juce::File& playable, const std::function<bool()>& shouldStop)
    {
        if (auto cached = getCached (playable))
            return cached;

        std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (engine, playable));

        if (reader == nullptr)
            return {};

        auto stop = shouldStop ? shouldStop : [] { return false; };
        auto info = analyseReader (*reader, stop);

        if (stop())
            return {};

        store (playable, info);
        return info;
    }

    bool prepareWithProgressWindow (te::Engine& engine, const juce::Array<juce::File>& songs)
    {
        juce::Array<juce::File> toPrepare;

        for (auto& song : songs)
            if (! decodedCache::isReady (song) || ! getCached (decodedCache::getPlayableFile (engine, song)))
                toPrepare.add (song);

        if (toPrepare.isEmpty())
            return true;

        struct Window : public juce::ThreadWithProgressWindow
        {
            Window (te::Engine& e, const juce::Array<juce::File>& f)
                : ThreadWithProgressWindow ("Preparing songs", true, true), engine (e), files (f) {}

            void run() override
            {
                for (int i = 0; i < files.size() && ! threadShouldExit(); ++i)
                {
                    auto counter = "  (" + juce::String (i + 1) + " of " + juce::String (files.size()) + ")";
                    std::atomic<float> fileProgress { 0.0f };

                    if (! decodedCache::isReady (files[i]))
                        setStatusMessage ("Decoding " + files[i].getFileName() + counter);

                    // Decoding is most of the work: the first 80% of each song's share.
                    auto playable = decodedCache::getPlayableFile (engine, files[i], &fileProgress, [&]
                    {
                        setProgress ((i + 0.8 * fileProgress.load()) / files.size());
                        return threadShouldExit();
                    });

                    if (playable == juce::File() || threadShouldExit())
                        continue;

                    setStatusMessage ("Finding the beat, key and loudness of " + files[i].getFileName() + counter);
                    analyse (engine, playable, [this] { return threadShouldExit(); });
                    setProgress ((i + 1.0) / files.size());
                }
            }

            te::Engine& engine;
            const juce::Array<juce::File>& files;
        };

        Window window (engine, toPrepare);
        const juce::ScopedValueSetter<juce::ThreadWithProgressWindow*> running (runningProgressWindow(), &window);   // (C20: see there)
        return window.runThread();
    }

    bool isAutoTrimEnabled()
    {
        return settings::getBool (autoTrimSetting, true);
    }

    void setAutoTrimEnabled (bool enabled)
    {
        settings::setBool (autoTrimSetting, enabled);
    }

    bool prepareNewClip (te::AudioClipBase& clip, const SongInfo& info, te::TimePosition dropTime, bool snapToBars, bool autoTrim)
    {
        auto* um = &clip.edit.getUndoManager();
        clip.state.setProperty (soundStartProperty, info.soundStart, um);
        setKey (clip, info.key, false);
        setLoudness (clip, info);
        applyDetectedGrid (clip, info, false);

        if (! info.grid.valid)
            return false;

        // Beat 1 on the bar the drop point is in; then the start trimmed and kept on the grid at
        // 0:00. (Placing it again also undoes any cut tempo::apply made at 0:00.)
        auto contentStart = snapToBars ? tempo::contentStartFor (clip.edit, info.grid.bpm, info.grid.downbeat, tempo::barAtOrBefore (clip.edit, dropTime))
                                       : dropTime;
        auto trim = autoTrim ? info.silentUntil : 0.0;
        auto moved = tempo::placeKeepingTheGrid (clip, contentStart, { trim, tempo::getSourceLength (clip) }, getMayCutUntil (clip));

        auto trimmed = tempo::getSourceRange (clip).start;
        clip.state.setProperty (autoTrimProperty, trimmed, um);
        return moved;
    }

    void setKey (te::AudioClipBase& clip, const juce::String& camelot, bool byHand)
    {
        auto* um = &clip.edit.getUndoManager();

        if (camelot.isEmpty())
        {
            clip.state.removeProperty (keyProperty, um);
            clip.state.removeProperty (keySourceProperty, um);
            return;
        }

        clip.state.setProperty (keyProperty, camelot, um);
        clip.state.setProperty (keySourceProperty, byHand ? "set by hand" : "detected", um);
    }

    void setLoudness (te::AudioClipBase& clip, const SongInfo& info)
    {
        if (std::isfinite (info.loudness))
            clip.state.setProperty (levelmatch::loudnessProperty, std::round (info.loudness * 100.0) / 100.0, &clip.edit.getUndoManager());
    }

    void measureMissingLoudness (te::Edit& edit)
    {
        juce::Array<te::AudioClipBase*> clips;
        juce::Array<juce::File> files;

        for (auto* track : te::getAudioTracks (edit))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && ! levelmatch::getLoudness (*clip))
                    if (mixtopia::loops::getSongFile (*clip).existsAsFile())
                    {
                        clips.add (clip);
                        files.addIfNotAlreadyThere (mixtopia::loops::getSongFile (*clip));
                    }

        if (files.isEmpty())
            return;

        struct Window : public juce::ThreadWithProgressWindow
        {
            Window (te::Engine& e, const juce::Array<juce::File>& f)
                : ThreadWithProgressWindow ("Measuring song loudness", true, true), engine (e), files (f) {}

            void run() override
            {
                for (int i = 0; i < files.size() && ! threadShouldExit(); ++i)
                {
                    setStatusMessage ("Measuring " + files[i].getFileNameWithoutExtension() + "  (" + juce::String (i + 1)
                                        + " of " + juce::String (files.size()) + ")");
                    analyse (engine, files[i], [this] { return threadShouldExit(); });
                    setProgress ((i + 1.0) / files.size());
                }
            }

            te::Engine& engine;
            const juce::Array<juce::File>& files;
        };

        Window window (edit.engine, files);
        const juce::ScopedValueSetter<juce::ThreadWithProgressWindow*> running (runningProgressWindow(), &window);   // (C20: see there)
        window.runThread();

        // Derived data, not an edit: no undo step.
        for (auto* clip : clips)
            if (auto info = getCached (mixtopia::loops::getSongFile (*clip)); info && std::isfinite (info->loudness))
                clip->state.setProperty (levelmatch::loudnessProperty, std::round (info->loudness * 100.0) / 100.0, nullptr);
    }

    double getMayCutUntil (const te::AudioClipBase& clip)
    {
        if (clip.state.hasProperty (soundStartProperty))
            return (double) clip.state[soundStartProperty] + 0.005;

        // Clips added before this was stored: the song's cached analysis, or a quick look at the
        // start of the file.
        if (auto info = getCached (mixtopia::loops::getSongFile (clip)))
            return info->soundStart + 0.005;

        std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (clip.edit.engine, mixtopia::loops::getSongFile (clip)));
        return reader != nullptr ? findSoundStart (*reader) + 0.005 : 0.0;
    }

    void reapplyDetectedGrid (te::AudioClipBase& clip, const SongInfo& info, bool snapToBars)
    {
        // Keep the clip's current trim; at 0:00 trim silence or move a bar later rather than cut sound.
        auto keep = tempo::getSourceRange (clip);
        clip.state.setProperty (soundStartProperty, info.soundStart, &clip.edit.getUndoManager());
        applyDetectedGrid (clip, info, snapToBars);
        keep.end = juce::jmax (keep.end, tempo::getSourceRange (clip).end);
        tempo::placeKeepingTheGrid (clip, tempo::getContentStart (clip), keep, getMayCutUntil (clip), keep.start);
    }

    bool restoreTrimmedStart (te::AudioClipBase& clip)
    {
        if ((double) clip.state.getProperty (autoTrimProperty, 0.0) <= 0.0 || clip.getPosition().getOffset() <= te::TimeDuration())
            return false;

        auto range = tempo::getSourceRange (clip);
        auto contentStart = tempo::getContentStart (clip);

        // As far back as the song's start, or 0:00.
        range.start = juce::jmax (0.0, tempo::editToSourceSeconds (clip, 0.0));
        tempo::placeKeepingTheGrid (clip, contentStart, range, range.start);
        clip.state.setProperty (autoTrimProperty, 0.0, &clip.edit.getUndoManager());
        return true;
    }

    void applyDetectedGrid (te::AudioClipBase& clip, const SongInfo& info, bool snapToBars)
    {
        if (! info.grid.valid)
            return;

        tempo::setBpmAndDownbeat (clip, info.grid.bpm, info.grid.downbeat, snapToBars ? tempo::Snap::bars : tempo::Snap::none);
        clip.state.setProperty (tempo::gridSourceProperty, "detected", &clip.edit.getUndoManager());
    }
}
