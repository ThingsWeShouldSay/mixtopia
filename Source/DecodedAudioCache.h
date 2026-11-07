#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// MP3 and FLAC files are decoded once into 32-bit float WAV copies, which is what clips actually
// play. Tracktion reads WAVs bit-exactly from memory-mapped files; its real-time path for
// compressed files (BufferedFileReader) is unfinished and glitches at clip starts.
//
// Copies live in ~/.cache/Mixtopia/decoded, named after the source's path, size and modification
// time, so they survive between sessions and are re-made if the source changes. Only songs placed on
// the timeline get a copy (the library reads songs straight from their files). At start-up the
// copies and the waveform cache are trimmed together, least recently used first, to the size chosen
// in Options (default 10 GB); the songs of the open project are never deleted (user, 2026-10-23:
// keep the OS drive small; songs may live on an external drive).
namespace mixtopia::decodedCache
{
    // True if this file has to be decoded before it can be played.
    bool needsDecoding (const juce::File& source);

    // The cached WAV for a source (it may not exist yet).
    juce::File getCachedFile (const juce::File& source);

    // True if the file can be played now: it needs no decoding, or its decoded copy is cached.
    bool isReady (const juce::File& source);

    // A folder under ~/.cache/Mixtopia, and a short hex name for a file that changes whenever the
    // file's path, size or modification time does. Shared by the caches of decoded audio and of
    // waveform analysis.
    juce::File getCacheFolder (const juce::String& name);
    juce::String getCacheKey (const juce::File& source);

    // The key for caching what is worked out about a song (analysis, waveform): the song file's key,
    // also when given its decoded copy (whose name carries it). So the library's results from the
    // MP3 are found again once the song is decoded, and still found when the drive is unplugged.
    juce::String getSongKey (const juce::File& songOrDecodedCopy);

    // The file to read a song from without decoding it to disk: its decoded copy if there is one
    // (faster), else the file itself.
    juce::File getReadableFile (const juce::File& source);

    // Returns the file a clip should play: the source itself for WAV, otherwise its decoded copy,
    // decoding it now if needed (call from a background thread for big files). `progress` is set
    // to 0..1 as it goes, and `shouldStop` lets a caller cancel. Returns {} on failure.
    juce::File getPlayableFile (te::Engine&, const juce::File& source,
                                std::atomic<float>* progress = nullptr,
                                const std::function<bool()>& shouldStop = {});

    // Decodes files that aren't cached yet, showing a progress window with a Cancel button.
    // Returns false if cancelled.
    bool decodeWithProgressWindow (te::Engine&, const juce::Array<juce::File>& files);

    // Counts as just used (for trimming): a project that plays this copy was opened.
    void markUsed (const juce::File& decodedCopy);

    // The size limit for decoded copies + waveforms (Options > Song Cache Size), in GB.
    inline constexpr int sizeChoicesGB[] = { 5, 10, 20, 40 };
    int getSizeLimitGB();
    void setSizeLimitGB (int);

    // Bytes the decoded copies and waveforms take now.
    juce::int64 getUsedBytes();

    // Deletes least recently used decoded copies and waveforms until they're under the size limit,
    // never touching `keep` (decoded copies, with their waveforms). Returns the bytes freed.
    juce::int64 trimToSizeLimit (const juce::Array<juce::File>& keep);

    // Deletes every decoded copy and waveform except `keep` ("Clear song cache"). Returns the bytes
    // freed.
    juce::int64 clear (const juce::Array<juce::File>& keep);
}
