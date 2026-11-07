#include "Mpg123AudioFormat.h"
#include <mpg123.h>

namespace
{
    // mpg123 pulls bytes through these callbacks, so it can read from any juce::InputStream.
    int readCallback (void* handle, void* buffer, size_t bytes, size_t* bytesRead)
    {
        auto* stream = static_cast<juce::InputStream*> (handle);
        auto n = stream->read (buffer, (size_t) bytes);

        if (n < 0)
            return -1;

        *bytesRead = (size_t) n;
        return 0;
    }

    int64_t seekCallback (void* handle, int64_t offset, int whence)
    {
        auto* stream = static_cast<juce::InputStream*> (handle);
        juce::int64 target = offset;

        if (whence == SEEK_CUR)      target += stream->getPosition();
        else if (whence == SEEK_END) target += stream->getTotalLength();

        return stream->setPosition (target) ? stream->getPosition() : -1;
    }

    class Mpg123Reader : public juce::AudioFormatReader
    {
    public:
        Mpg123Reader (juce::InputStream* in, mpg123_handle* h)
            : AudioFormatReader (in, "MP3 file"), handle (h)
        {
            long rate = 0;
            int channels = 0, encoding = 0;
            mpg123_getformat (handle, &rate, &channels, &encoding);

            // Lock the output format so it can't change mid-file.
            mpg123_format_none (handle);
            mpg123_format (handle, rate, channels, MPG123_ENC_FLOAT_32);

            sampleRate = (double) rate;
            numChannels = (unsigned int) channels;
            bitsPerSample = 32;
            usesFloatingPointData = true;

            // Walks the whole file once: exact length, and an exact seek index.
            mpg123_scan (handle);
            lengthInSamples = juce::jmax ((juce::int64) 0, (juce::int64) mpg123_length64 (handle));

            // The scan leaves the decoder at a raw stream position; force a (gapless-aware) seek
            // before the first read.
            position = -1;
        }

        ~Mpg123Reader() override
        {
            mpg123_close (handle);
            mpg123_delete (handle);
        }

        bool readSamples (int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer,
                          juce::int64 startSampleInFile, int numSamples) override
        {
            clearSamplesBeyondAvailableLength (destSamples, numDestChannels, startOffsetInDestBuffer,
                                               startSampleInFile, numSamples, lengthInSamples);

            if (numSamples <= 0)
                return true;

            if (startSampleInFile != position)
            {
                if (mpg123_seek64 (handle, startSampleInFile, SEEK_SET) < 0)
                    return false;

                position = startSampleInFile;
            }

            auto channels = (int) numChannels;

            while (numSamples > 0)
            {
                auto framesWanted = juce::jmin (numSamples, maxFramesPerRead);
                interleaved.resize ((size_t) (framesWanted * channels));

                size_t bytesDone = 0;
                auto result = mpg123_read (handle, interleaved.data(), interleaved.size() * sizeof (float), &bytesDone);
                auto framesDone = (int) (bytesDone / (sizeof (float) * (size_t) channels));

                for (int ch = 0; ch < numDestChannels; ++ch)
                {
                    if (destSamples[ch] == nullptr)
                        continue;

                    auto* dest = reinterpret_cast<float*> (destSamples[ch]) + startOffsetInDestBuffer;
                    auto srcChannel = juce::jmin (ch, channels - 1);

                    for (int i = 0; i < framesDone; ++i)
                        dest[i] = interleaved[(size_t) (i * channels + srcChannel)];
                }

                startOffsetInDestBuffer += framesDone;
                numSamples -= framesDone;
                position += framesDone;

                if (result == MPG123_DONE || (framesDone == 0 && result != MPG123_OK && result != MPG123_NEW_FORMAT))
                    break;
            }

            // Past the real end (or a decode error): fill the rest with silence.
            if (numSamples > 0)
                for (int ch = 0; ch < numDestChannels; ++ch)
                    if (destSamples[ch] != nullptr)
                        juce::zeromem (destSamples[ch] + startOffsetInDestBuffer, (size_t) numSamples * sizeof (float));

            return true;
        }

    private:
        static constexpr int maxFramesPerRead = 4096;
        mpg123_handle* handle;
        juce::int64 position = 0;
        std::vector<float> interleaved;
    };
}

Mpg123AudioFormat::Mpg123AudioFormat()
    : AudioFormat ("MP3 file", ".mp3")
{
}

juce::AudioFormatReader* Mpg123AudioFormat::createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails)
{
    auto fail = [&]() -> juce::AudioFormatReader*
    {
        if (deleteStreamIfOpeningFails)
            delete in;

        return nullptr;
    };

    int error = MPG123_OK;
    auto* handle = mpg123_new (nullptr, &error);

    if (handle == nullptr)
        return fail();

    mpg123_param2 (handle, MPG123_FLAGS, MPG123_GAPLESS | MPG123_QUIET, 0);
    mpg123_format_none (handle);
    mpg123_format2 (handle, 0, MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);

    long rate = 0;
    int channels = 0, encoding = 0;

    if (mpg123_reader64 (handle, readCallback, seekCallback, nullptr) != MPG123_OK
         || mpg123_open_handle (handle, in) != MPG123_OK
         || mpg123_getformat (handle, &rate, &channels, &encoding) != MPG123_OK
         || rate <= 0 || channels <= 0)
    {
        mpg123_delete (handle);
        return fail();
    }

    return new Mpg123Reader (in, handle);
}
