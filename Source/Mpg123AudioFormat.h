#pragma once

#include <JuceHeader.h>

// Reads MP3 files with libmpg123 instead of JUCE's built-in decoder. mpg123 scans the file for
// its exact length (JUCE estimates it, overshooting files without a Xing/Info header) and
// honours the LAME gapless info, so decoded audio starts at the real first sample (JUCE keeps
// the encoder delay, shifting everything ~25 ms late). Read-only.
class Mpg123AudioFormat : public juce::AudioFormat
{
public:
    Mpg123AudioFormat();

    juce::Array<int> getPossibleSampleRates() override  { return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 }; }
    juce::Array<int> getPossibleBitDepths() override    { return { 32 }; }
    bool canDoStereo() override                         { return true; }
    bool canDoMono() override                           { return true; }
    bool isCompressed() override                        { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream*, bool deleteStreamIfOpeningFails) override;

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                              const juce::AudioFormatWriterOptions&) override
    {
        return {};
    }
};
