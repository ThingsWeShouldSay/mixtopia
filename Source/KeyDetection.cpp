#include "KeyDetection.h"
#include <keyfinder.h>

namespace mixtopia::key
{
    namespace
    {
        // Pitch class (C = 0) of each tonic, and its Camelot number, minor and major.
        constexpr int minorCamelot[12] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };   // C minor = 5A, ...
        constexpr int majorCamelot[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };   // C major = 8B, ...
        const char* noteNames[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

        juce::String camelotOf (KeyFinder::key_t key)
        {
            if (key == KeyFinder::SILENCE)
                return {};

            // libKeyFinder's order: A major, A minor, Bb major, Bb minor, ... (from A, by semitone).
            auto pitchClass = (9 + (int) key / 2) % 12;
            auto minor = (int) key % 2 == 1;
            return juce::String (minor ? minorCamelot[pitchClass] : majorCamelot[pitchClass]) + (minor ? "A" : "B");
        }
    }

    juce::String analyse (juce::AudioFormatReader& reader, const std::function<bool()>& shouldStop)
    {
        // No audio (an empty file): no key. libKeyFinder throws on it ("Frame rate must be > 0").
        if (reader.lengthInSamples <= 0 || reader.sampleRate <= 0.0)
            return {};

        // libKeyFinder reports what it can't handle by throwing; a song it can't analyse gets no key
        // (uncaught, it ended the program, or the library's analysis thread - CLEANUP-AUDIT C5).
        try
        {
            KeyFinder::KeyFinder finder;
            KeyFinder::Workspace workspace;
            auto channels = (int) juce::jmax (1u, reader.numChannels);
            constexpr int chunk = 65536;
            juce::AudioBuffer<float> buffer (channels, chunk);

            for (juce::int64 pos = 0; pos < reader.lengthInSamples; pos += chunk)
            {
                if (shouldStop())
                    return {};

                auto num = (int) juce::jmin ((juce::int64) chunk, reader.lengthInSamples - pos);

                if (! reader.read (&buffer, 0, num, pos, true, true))
                    return {};

                KeyFinder::AudioData audio;
                audio.setFrameRate ((unsigned int) reader.sampleRate);
                audio.setChannels ((unsigned int) channels);
                audio.addToFrameCount ((unsigned int) num);

                for (int i = 0; i < num; ++i)
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        auto sample = buffer.getSample (ch, i);   // (a NaN or infinite sample counts as silence)
                        audio.setSampleByFrame ((unsigned int) i, (unsigned int) ch, std::isfinite (sample) ? sample : 0.0f);
                    }

                finder.progressiveChromagram (audio, workspace);
            }

            finder.finalChromagram (workspace);
            return camelotOf (finder.keyOfChromagram (workspace));
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    juce::StringArray allCamelotKeys()
    {
        juce::StringArray keys;

        for (int n = 1; n <= 12; ++n)
        {
            keys.add (juce::String (n) + "A");
            keys.add (juce::String (n) + "B");
        }

        return keys;
    }

    juce::String toMusicalName (const juce::String& camelot)
    {
        auto number = camelot.dropLastCharacters (1).getIntValue();
        auto letter = camelot.getLastCharacter();

        for (int pc = 0; pc < 12; ++pc)
        {
            if (letter == 'A' && minorCamelot[pc] == number)
                return juce::String (noteNames[pc]) + " minor";

            if (letter == 'B' && majorCamelot[pc] == number)
                return juce::String (noteNames[pc]) + " major";
        }

        return {};
    }
}
