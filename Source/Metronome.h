#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// Mixtopia's metronome sounds. Tracktion's built-in clicks are thin and its beat click is ~9 dB
// weaker than its bar click, so beats 2-4 vanished under dense trance. These are short, bright
// "ticks" in the ear's most sensitive range (2-3 kHz), all equally loud; beat 1 is marked by a
// higher pitch instead of extra volume. They're written to ~/.config/Mixtopia/clicks at start-up
// and handed to Tracktion's click track.
namespace mixtopia::metronome
{
    inline juce::AudioBuffer<float> makeTick (double sampleRate, double frequency)
    {
        auto numSamples = (int) (sampleRate * 0.045);
        juce::AudioBuffer<float> buffer (1, numSamples);
        juce::Random random (1);

        for (int i = 0; i < numSamples; ++i)
        {
            auto t = i / sampleRate;
            auto tone = std::sin (juce::MathConstants<double>::twoPi * frequency * t)
                          + 0.4 * std::sin (juce::MathConstants<double>::twoPi * frequency * 2.01 * t);
            auto attack = (random.nextFloat() * 2.0f - 1.0f) * std::exp (-t / 0.0015);
            buffer.setSample (0, i, (float) (tone * std::exp (-t / 0.012) + 0.5 * attack));
        }

        // Equal loudness (RMS) for both ticks, with peaks just under full scale.
        buffer.applyGain (0.25f / buffer.getRMSLevel (0, 0, numSamples));

        if (auto peak = buffer.getMagnitude (0, numSamples); peak > 0.95f)
            buffer.applyGain (0.95f / peak);

        return buffer;
    }

    inline void writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> out (file.createOutputStream());

        if (out == nullptr)
            return;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (sampleRate)
                                                    .withNumChannels (1)
                                                    .withBitsPerSample (24));

        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // Writes the tick sounds and points Tracktion's click track at them. Call once at start-up.
    inline void install (te::Engine& engine)
    {
        constexpr double rate = 44100.0;
        auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Mixtopia/clicks");
        folder.createDirectory();

        auto beat = folder.getChildFile ("beat.wav");
        auto bar = folder.getChildFile ("bar.wav");
        writeWav (beat, makeTick (rate, 2000.0), rate);
        writeWav (bar, makeTick (rate, 3000.0), rate);

        auto& storage = engine.getPropertyStorage();
        storage.setProperty (te::SettingID::clickTrackSampleSmall, beat.getFullPathName());
        storage.setProperty (te::SettingID::clickTrackSampleBig, bar.getFullPathName());
    }
}
