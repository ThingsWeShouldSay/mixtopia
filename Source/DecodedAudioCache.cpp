#include "DecodedAudioCache.h"
#include "Settings.h"
#include "AppBehaviour.h"
#include <map>
#include <set>
#include <mutex>
#include <unistd.h>

namespace mixtopia::decodedCache
{
    namespace
    {
        constexpr int defaultSizeGB = 10;   // ~8 hours of audio
        const char* sizeSetting = "mixtopiaSongCacheGB";

        juce::File getDecodedFolder()
        {
            return getCacheFolder ("decoded");
        }

        // One lock per song being decoded: when two parts of Mixtopia ask for the same song at once
        // (a timeline drop prepares it while the library's helper does too), the second waits and
        // uses the first's copy. They used to write the same temporary file together, and one of
        // them failed (the library then marked "Light At The End" as failed - user, 2026-10-23).
        std::shared_ptr<std::mutex> lockFor (const juce::File& cached)
        {
            static std::mutex mapLock;
            static std::map<juce::String, std::shared_ptr<std::mutex>> locks;
            const std::lock_guard<std::mutex> guard (mapLock);
            auto& lock = locks[cached.getFullPathName()];

            if (lock == nullptr)
                lock = std::make_shared<std::mutex>();

            return lock;
        }
    }

    juce::File getCacheFolder (const juce::String& name)
    {
        // (MIXTOPIA_CACHE_DIR: the self-tests use a folder of their own, never the user's cache)
        if (auto testFolder = juce::SystemStats::getEnvironmentVariable ("MIXTOPIA_CACHE_DIR", {}); testFolder.isNotEmpty())
            return juce::File (testFolder).getChildFile (name);

        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile (".cache/Mixtopia").getChildFile (name);
    }

    juce::String getCacheKey (const juce::File& source)
    {
        auto key = source.getFullPathName() + "|" + juce::String (source.getSize())
                     + "|" + juce::String (source.getLastModificationTime().toMilliseconds());
        return juce::String::toHexString (key.hashCode64());
    }

    juce::String getSongKey (const juce::File& file)
    {
        if (file.getParentDirectory() == getDecodedFolder() && file.hasFileExtension ("wav"))
        {
            auto name = file.getFileNameWithoutExtension();
            auto key = name.fromLastOccurrenceOf ("_", false, false);

            if (key.isNotEmpty() && key != name && key.containsOnly ("0123456789abcdef"))
                return key;
        }

        return getCacheKey (file);
    }

    juce::File getReadableFile (const juce::File& source)
    {
        if (needsDecoding (source))
            if (auto cached = getCachedFile (source); cached.existsAsFile())
                return cached;

        return source;
    }

    bool needsDecoding (const juce::File& source)
    {
        if (source.hasFileExtension ("mp3;flac"))
            return true;

        // A WAV plays straight from its file - unless it isn't 32-bit float: the reader for looped songs and decoded
        // copies (Loops.cpp) reads 32-bit float only, so a looped 16- or 24-bit WAV played as noise or silence
        // (CLEANUP-AUDIT C1). Such WAVs get a float copy like MP3/FLAC (which also keeps them off the music drive).
        if (source.hasFileExtension ("wav"))
            if (auto in = source.createInputStream())
                if (std::unique_ptr<juce::AudioFormatReader> reader { juce::WavAudioFormat().createReaderFor (in.release(), true) })
                    return ! (reader->usesFloatingPointData && reader->bitsPerSample == 32);

        return false;
    }

    juce::File getCachedFile (const juce::File& source)
    {
        return getDecodedFolder().getChildFile (source.getFileNameWithoutExtension().substring (0, 40)
                                                  .replaceCharacters (" /\\:", "____")
                                                + "_" + getCacheKey (source) + ".wav");
    }

    bool isReady (const juce::File& source)
    {
        return ! needsDecoding (source) || getCachedFile (source).existsAsFile();
    }

    juce::File getPlayableFile (te::Engine& engine, const juce::File& source,
                                std::atomic<float>* progress, const std::function<bool()>& shouldStop)
    {
        if (! needsDecoding (source))
            return source;

        auto cached = getCachedFile (source);

        if (cached.existsAsFile())
        {
            // Mark as recently used, for trimming.
            cached.setLastAccessTime (juce::Time::getCurrentTime());
            return cached;
        }

        auto lock = lockFor (cached);
        const std::lock_guard<std::mutex> decoding (*lock);

        if (cached.existsAsFile())   // (decoded by someone else meanwhile)
            return cached;

        std::unique_ptr<juce::AudioFormatReader> reader (
            engine.getAudioFileFormatManager().readFormatManager.createReaderFor (source));

        if (reader == nullptr || reader->lengthInSamples <= 0)
            return {};

        getDecodedFolder().createDirectory();

        // Write to a temporary name and rename at the end, so a half-written file is never used.
        auto partial = cached.withFileExtension ("." + juce::String ((int) getpid()) + ".partial");   // (one per process, too)
        partial.deleteFile();

        {
            std::unique_ptr<juce::OutputStream> out (partial.createOutputStream());

            if (out == nullptr)
                return {};

            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                        .withSampleRate (reader->sampleRate)
                                                        .withNumChannels ((int) reader->numChannels)
                                                        .withBitsPerSample (32)
                                                        .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

            if (writer == nullptr)
                return {};

            constexpr int blockSize = 65536;
            juce::AudioBuffer<float> buffer ((int) reader->numChannels, blockSize);

            for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += blockSize)
            {
                if (shouldStop && shouldStop())
                {
                    writer = nullptr;
                    partial.deleteFile();
                    return {};
                }

                auto n = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - pos);

                if (! reader->read (&buffer, 0, n, pos, true, true) || ! writer->writeFromAudioSampleBuffer (buffer, 0, n))
                {
                    writer = nullptr;
                    partial.deleteFile();
                    return {};
                }

                if (progress != nullptr)
                    *progress = (float) ((double) (pos + n) / (double) reader->lengthInSamples);
            }
        }

        if (! partial.moveFileTo (cached))
            return {};

        return cached;
    }

    bool decodeWithProgressWindow (te::Engine& engine, const juce::Array<juce::File>& files)
    {
        struct Window : public juce::ThreadWithProgressWindow
        {
            Window (te::Engine& e, const juce::Array<juce::File>& f)
                : ThreadWithProgressWindow ("Preparing songs", true, true), engine (e), files (f) {}

            void run() override
            {
                for (int i = 0; i < files.size() && ! threadShouldExit(); ++i)
                {
                    setStatusMessage ("Decoding " + files[i].getFileName() + "  (" + juce::String (i + 1)
                                        + " of " + juce::String (files.size()) + ")");
                    std::atomic<float> fileProgress { 0.0f };
                    getPlayableFile (engine, files[i], &fileProgress, [&]
                    {
                        setProgress ((i + fileProgress.load()) / files.size());
                        return threadShouldExit();
                    });
                }
            }

            te::Engine& engine;
            const juce::Array<juce::File>& files;
        };

        if (files.isEmpty())
            return true;

        Window window (engine, files);
        const juce::ScopedValueSetter<juce::ThreadWithProgressWindow*> running (runningProgressWindow(), &window);   // (C20: see there)
        return window.runThread();
    }

    void markUsed (const juce::File& decodedCopy)
    {
        if (decodedCopy.getParentDirectory() == getDecodedFolder() && decodedCopy.existsAsFile())
            decodedCopy.setLastAccessTime (juce::Time::getCurrentTime());
    }

    int getSizeLimitGB()
    {
        auto gb = settings::getFile().getIntValue (sizeSetting, defaultSizeGB);

        for (auto choice : sizeChoicesGB)
            if (gb == choice)
                return gb;

        return defaultSizeGB;
    }

    void setSizeLimitGB (int gb)
    {
        settings::getFile().setValue (sizeSetting, gb);
        settings::getFile().saveIfNeeded();
    }

    namespace
    {
        // The files that count: decoded copies and waveforms, plus leftovers of interrupted writes
        // (a decode another running Mixtopia is doing right now is left alone).
        juce::Array<juce::File> getCacheFiles()
        {
            auto files = getDecodedFolder().findChildFiles (juce::File::findFiles, false, "*.wav;*.partial");
            files.addArray (getCacheFolder ("overviews").findChildFiles (juce::File::findFiles, false, "*.bin;*.tmp"));

            files.removeIf ([] (const juce::File& f)
            {
                auto pid = f.getFileNameWithoutExtension().fromLastOccurrenceOf (".", false, false).getIntValue();
                return f.hasFileExtension ("partial") && pid > 0 && pid != (int) getpid()
                         && juce::File ("/proc/" + juce::String (pid)).isDirectory();
            });

            return files;
        }

        // Kept files: the copies themselves and their waveforms (named by the song's key).
        std::function<bool (const juce::File&)> isKept (const juce::Array<juce::File>& keep)
        {
            std::set<juce::String> paths, overviewNames;

            for (auto& f : keep)
            {
                paths.insert (f.getFullPathName());
                overviewNames.insert (getSongKey (f) + ".bin");
            }

            return [paths, overviewNames] (const juce::File& f)
            {
                return paths.count (f.getFullPathName()) > 0 || overviewNames.count (f.getFileName()) > 0;
            };
        }
    }

    juce::int64 getUsedBytes()
    {
        juce::int64 total = 0;

        for (auto& f : getCacheFiles())
            total += f.getSize();

        return total;
    }

    juce::int64 trimToSizeLimit (const juce::Array<juce::File>& keep)
    {
        auto files = getCacheFiles();
        auto kept = isKept (keep);
        auto limit = (juce::int64) getSizeLimitGB() * 1024 * 1024 * 1024;

        // Oldest use first.
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   {
                       return a.getLastAccessTime() < b.getLastAccessTime();
                   });

        juce::int64 total = 0, freed = 0;

        for (auto& f : files)
            total += f.getSize();

        for (auto& f : files)
        {
            if (total <= limit)
                break;

            if (kept (f))
                continue;

            auto size = f.getSize();

            if (f.deleteFile())
            {
                total -= size;
                freed += size;
            }
        }

        return freed;
    }

    juce::int64 clear (const juce::Array<juce::File>& keep)
    {
        auto kept = isKept (keep);
        juce::int64 freed = 0;

        for (auto& f : getCacheFiles())
            if (auto size = f.getSize(); ! kept (f) && f.deleteFile())
                freed += size;

        return freed;
    }
}
