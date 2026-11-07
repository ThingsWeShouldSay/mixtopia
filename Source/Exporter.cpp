#include "Exporter.h"
#include "EditHelpers.h"
#include "Loudness.h"
#include "Mpg123AudioFormat.h"

namespace mixtopia::exporter
{
    constexpr double exportSampleRate = 44100.0;

    // A file stream that remembers how far it was written. JUCE's file stream reports no error when a full disk takes
    // only part of a write, and a FLAC's header holds the length the encoder was GIVEN - so a file the disk cut short
    // looked whole (audit review of C3). The file's size is compared with this afterwards.
    struct MeasuredStream : public juce::OutputStream
    {
        MeasuredStream (std::unique_ptr<juce::OutputStream> s, juce::int64& end) : stream (std::move (s)), furthest (end) {}

        void flush() override                         { stream->flush(); }
        bool setPosition (juce::int64 position) override  { return stream->setPosition (position); }
        juce::int64 getPosition() override            { return stream->getPosition(); }

        bool write (const void* data, size_t numBytes) override
        {
            auto ok = stream->write (data, numBytes);
            furthest = std::max (furthest, stream->getPosition());
            return ok;
        }

        std::unique_ptr<juce::OutputStream> stream;
        juce::int64& furthest;
    };

    // A finished copy is complete: no error, and the file on disk as long as what was written to it.
    bool isComplete (juce::FileOutputStream& out, const juce::File& file)
    {
        out.flush();
        return ! out.getStatus().failed() && file.getSize() == out.getPosition();
    }

    juce::String getExtension (Format format)
    {
        return format == Format::mp3 ? ".mp3" : ".flac";
    }

    float getTargetPeakDb (Format format)
    {
        return format == Format::mp3 ? -1.0f : -0.3f;
    }

    float getNormalisingGain (Format format, float peak)
    {
        if (peak <= 0.0f)
            return 1.0f;

        return std::max (1.0f, juce::Decibels::decibelsToGain (getTargetPeakDb (format)) / peak);
    }

    juce::File renderMix (te::Edit& edit, juce::String& error)
    {
        if (edit.getLength() <= te::TimeDuration())
        {
            error = "There's nothing on the timeline to export.";
            return {};
        }

        auto tracks = getAudibleTracksForRender (edit);

        if (tracks.isZero())
        {
            error = "Every lane is muted, so there's nothing to export.";
            return {};
        }

        auto wav = edit.getTempDirectory (true).getChildFile ("export-mix.wav");
        wav.deleteFile();

        te::Renderer::Parameters params (edit);
        params.destFile = wav;
        params.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
        params.bitDepth = 32;
        params.sampleRateForAudio = exportSampleRate;
        params.blockSizeForAudio = 512;
        params.time = { te::TimePosition(), te::TimePosition() + edit.getLength() };
        params.tracksToDo = tracks;
        params.canRenderInMono = false;
        params.useMasterPlugins = true;   // the master limiter: export must match playback

        if (te::Renderer::renderToFile ("Rendering the mix", params) == juce::File())
        {
            wav.deleteFile();
            error = "Rendering was cancelled or failed.";
            return {};
        }

        // The whole mix must be there: a full disk cut the render short without an error, and the export then
        // "succeeded" with only the start of the mix (CLEANUP-AUDIT C3). Checked against the header AND the file's size
        // (a header can claim more than was written).
        auto expected = te::toSamples (params.time.getLength(), exportSampleRate);
        std::unique_ptr<juce::AudioFormatReader> check (juce::WavAudioFormat().createReaderFor (wav.createInputStream().release(), true));

        if (check == nullptr || check->lengthInSamples != expected
             || wav.getSize() < expected * (juce::int64) check->numChannels * (juce::int64) (check->bitsPerSample / 8))
        {
            check = nullptr;
            wav.deleteFile();
            error = "Rendering stopped before the end of the mix (is the disk full?). Nothing was exported.";
            return {};
        }

        return wav;
    }

    float measurePeak (const juce::File& wav)
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (wav.createInputStream().release(), true));

        if (reader == nullptr)
            return 0.0f;

        constexpr int blockSize = 65536;
        juce::AudioBuffer<float> buffer ((int) reader->numChannels, blockSize);
        float peak = 0.0f;

        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += blockSize)
        {
            auto n = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - pos);
            reader->read (&buffer, 0, n, pos, true, true);
            peak = std::max (peak, buffer.getMagnitude (0, n));
        }

        return peak;
    }

    double measureLoudness (const juce::File& wav, const std::function<bool()>& shouldStop)
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (wav.createInputStream().release(), true));
        return reader != nullptr ? loudness::measure (*reader, shouldStop) : -std::numeric_limits<double>::infinity();
    }

    // The tags an export carries (user, 2026-10-27): the title = the export's file name, track
    // number 0, and the artist (also as album artist), album, genre and year from the Export window
    // when filled in. Never an original release date (TDOR / ORIGINALDATE).
    juce::String getTitleTag (const juce::File& destination)
    {
        return destination.getFileNameWithoutExtension();
    }

    // The cover image, read once per export.
    struct Artwork
    {
        juce::MemoryBlock data;
        juce::String mimeType;
        int width = 0, height = 0, bitsPerPixel = 24;
    };

    constexpr size_t maxArtworkBytes = (1u << 24) - 64;   // a FLAC metadata block holds under 16 MB

    std::optional<Artwork> loadArtwork (const juce::File& file, juce::String& error)
    {
        Artwork art;

        if (! file.existsAsFile() || ! file.loadFileAsData (art.data))
        {
            error = file.getFileName() + " isn't there any more.";
            return {};
        }

        auto* bytes = static_cast<const juce::uint8*> (art.data.getData());
        auto size = art.data.getSize();

        if (size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
            art.mimeType = "image/jpeg";
        else if (size >= 8 && std::memcmp (bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
            art.mimeType = "image/png";
        else
        {
            error = file.getFileName() + " isn't a JPG or PNG image.";
            return {};
        }

        if (size > maxArtworkBytes)
        {
            error = file.getFileName() + " is too big for a cover (" + juce::File::descriptionOfSizeInBytes ((juce::int64) size) + "; up to 16 MB).";
            return {};
        }

        auto image = juce::ImageFileFormat::loadFrom (art.data.getData(), size);

        if (! image.isValid())
        {
            error = file.getFileName() + " couldn't be read as an image.";
            return {};
        }

        art.width = image.getWidth();
        art.height = image.getHeight();
        art.bitsPerPixel = image.hasAlphaChannel() ? 32 : 24;
        return art;
    }

    juce::String checkArtwork (const juce::File& file)
    {
        juce::String error;
        loadArtwork (file, error);
        return error;
    }

    // (field, value) pairs in writing order, empty ones left out; `names` = the format's field names
    // for title, artist, album artist, album, genre, year, track number.
    std::vector<std::pair<juce::String, juce::String>> listTags (const juce::String& title, const Tags& tags,
                                                                  const std::array<const char*, 7>& names)
    {
        std::vector<std::pair<juce::String, juce::String>> list;
        const juce::String values[] { title, tags.artist, tags.artist, tags.album, tags.genre, tags.year, "0" };

        for (size_t i = 0; i < names.size(); ++i)
            if (values[i].trim().isNotEmpty())
                list.emplace_back (names[i], values[i].trim());

        return list;
    }

    // JUCE's FLAC writer ignores tags; libFLAC leaves an empty Vorbis comment block. Copies the file
    // with that block filled in (TITLE, TRACKNUMBER=0); everything else, the audio frames
    // included, is copied unchanged.
    juce::String copyFlacWithTags (const juce::File& source, const juce::File& destination, const juce::String& title, const Tags& tags,
                                   const std::optional<Artwork>& artwork)
    {
        juce::FileInputStream in (source);
        char magic[4] {};

        if (! in.openedOk() || in.read (magic, 4) != 4 || std::memcmp (magic, "fLaC", 4) != 0)
            return "Couldn't read the FLAC file back to tag it.";

        struct Block { int type; juce::MemoryBlock data; };
        std::vector<Block> blocks;
        juce::String vendor ("Mixtopia");

        for (bool last = false; ! last;)
        {
            juce::uint8 header[4] {};

            if (in.read (header, 4) != 4)
                return "The FLAC file ended inside its header.";

            last = (header[0] & 0x80) != 0;
            auto size = (size_t) ((header[1] << 16) | (header[2] << 8) | header[3]);
            Block block { header[0] & 0x7f, juce::MemoryBlock (size) };

            if (size > 0 && in.read (block.data.getData(), (int) size) != (int) size)
                return "The FLAC file ended inside its header.";

            if (block.type == 4)   // VORBIS_COMMENT: keep the encoder's vendor string, drop the rest
            {
                juce::MemoryInputStream old (block.data, false);
                auto vendorSize = (size_t) old.readInt();

                if (vendorSize <= size - 4)
                {
                    juce::MemoryBlock text (vendorSize);
                    old.read (text.getData(), (int) vendorSize);
                    vendor = text.toString();
                }
            }
            else
            {
                blocks.push_back (std::move (block));
            }
        }

        // Vorbis comments: little-endian lengths, UTF-8 "FIELD=value" entries.
        juce::MemoryOutputStream comment;
        auto writeString = [&comment] (const juce::String& s)
        {
            comment.writeInt ((int) s.getNumBytesAsUTF8());
            comment.write (s.toRawUTF8(), s.getNumBytesAsUTF8());
        };

        auto fields = listTags (title, tags, { "TITLE", "ARTIST", "ALBUMARTIST", "ALBUM", "GENRE", "DATE", "TRACKNUMBER" });
        writeString (vendor);
        comment.writeInt ((int) fields.size());

        for (auto& [field, value] : fields)
            writeString (field + "=" + value);
        blocks.insert (blocks.begin() + std::min<size_t> (1, blocks.size()), Block { 4, comment.getMemoryBlock() });   // after STREAMINFO

        // PICTURE (front cover), big-endian: type, MIME, description, size, colour depth, colours, data.
        if (artwork)
        {
            juce::MemoryOutputStream picture;
            picture.writeIntBigEndian (3);   // front cover
            picture.writeIntBigEndian ((int) artwork->mimeType.length());
            picture << artwork->mimeType;
            picture.writeIntBigEndian (0);   // no description
            picture.writeIntBigEndian (artwork->width);
            picture.writeIntBigEndian (artwork->height);
            picture.writeIntBigEndian (artwork->bitsPerPixel);
            picture.writeIntBigEndian (0);   // not an indexed-colour image
            picture.writeIntBigEndian ((int) artwork->data.getSize());
            picture << artwork->data;
            blocks.insert (blocks.begin() + std::min<size_t> (2, blocks.size()), Block { 6, picture.getMemoryBlock() });
        }

        juce::FileOutputStream out (destination);

        if (! out.openedOk())
            return "Couldn't write to " + destination.getParentDirectory().getFullPathName();

        out.write ("fLaC", 4);

        for (size_t i = 0; i < blocks.size(); ++i)
        {
            auto size = blocks[i].data.getSize();
            juce::uint8 header[4] { (juce::uint8) (blocks[i].type | (i + 1 == blocks.size() ? 0x80 : 0)),
                                    (juce::uint8) (size >> 16), (juce::uint8) (size >> 8), (juce::uint8) size };
            out.write (header, 4);
            out.write (blocks[i].data.getData(), size);
        }

        out.writeFromInputStream (in, -1);   // the audio frames
        return isComplete (out, destination) ? juce::String() : "Writing the export failed (is the disk full?).";
    }

    // Replaces a file with a copy made by `copy` (from the file, to a temporary one next to it).
    juce::String rewriteFile (const juce::File& file, const std::function<juce::String (const juce::File&, const juce::File&)>& copy)
    {
        juce::TemporaryFile rewritten (file);

        if (auto error = copy (file, rewritten.getFile()); error.isNotEmpty())
            return error;

        return rewritten.overwriteTargetFileWithTemporary() ? juce::String() : "Couldn't tag " + file.getFileName();
    }

    juce::String writeFlacTags (const juce::File& flacFile, const juce::String& title, const Tags& tags, const std::optional<Artwork>& artwork)
    {
        return rewriteFile (flacFile, [&] (auto& from, auto& to) { return copyFlacWithTags (from, to, title, tags, artwork); });
    }

    // An ID3v2.3 tag: TIT2 (title), TPE1/TPE2 (artist, album artist), TALB, TCON, TYER and TRCK "0".
    // Mixtopia writes it itself: LAME's own tagging adds an encoder frame (TSSE) the user doesn't want.
    juce::MemoryBlock makeId3Tag (const juce::String& title, const Tags& tags, const std::optional<Artwork>& artwork)
    {
        juce::MemoryOutputStream frames;
        auto addTextFrame = [&frames] (const char* id, const juce::String& text)
        {
            juce::MemoryOutputStream body;
            auto latin1 = true;

            for (auto c : text)
                latin1 = latin1 && c < 256;

            if (latin1)
            {
                body.writeByte (0);   // ISO-8859-1

                for (auto c : text)
                    body.writeByte ((char) c);
            }
            else
            {
                body.writeByte (1);   // UTF-16 with a byte-order mark
                body.writeShort ((short) 0xfeff);

                for (auto* unit = text.toUTF16().getAddress(); *unit != 0; ++unit)   // (surrogate pairs as they are)
                    body.writeShort ((short) *unit);
            }

            frames.write (id, 4);
            frames.writeIntBigEndian ((int) body.getDataSize());
            frames.writeShort (0);   // no frame flags
            frames << body;
        };

        for (auto& [id, value] : listTags (title, tags, { "TIT2", "TPE1", "TPE2", "TALB", "TCON", "TYER", "TRCK" }))
            addTextFrame (id.toRawUTF8(), value);

        // APIC (front cover): text encoding, MIME type, picture type, empty description, the image.
        if (artwork)
        {
            juce::MemoryOutputStream body;
            body.writeByte (0);
            body << artwork->mimeType;
            body.writeByte (0);
            body.writeByte (3);   // front cover
            body.writeByte (0);   // no description
            body << artwork->data;
            frames.write ("APIC", 4);
            frames.writeIntBigEndian ((int) body.getDataSize());
            frames.writeShort (0);
            frames << body;
        }

        juce::MemoryOutputStream tag;
        auto size = (juce::uint32) frames.getDataSize();
        const juce::uint8 header[10] { 'I', 'D', '3', 3, 0, 0,   // ID3v2.3, no flags; size in 7-bit bytes
                                       (juce::uint8) ((size >> 21) & 0x7f), (juce::uint8) ((size >> 14) & 0x7f),
                                       (juce::uint8) ((size >> 7) & 0x7f), (juce::uint8) (size & 0x7f) };
        tag.write (header, sizeof (header));
        tag << frames;
        return tag.getMemoryBlock();
    }

    juce::String writeMp3Tags (const juce::File& mp3File, const juce::String& title, const Tags& tags, const std::optional<Artwork>& artwork)
    {
        return rewriteFile (mp3File, [&] (const juce::File& from, const juce::File& to) -> juce::String
        {
            juce::FileInputStream in (from);
            juce::FileOutputStream out (to);

            if (! in.openedOk() || ! out.openedOk())
                return "Couldn't tag " + from.getFileName();

            out << makeId3Tag (title, tags, artwork);
            out.writeFromInputStream (in, -1);   // the MP3 frames, unchanged
            return isComplete (out, to) ? juce::String() : "Writing the export failed (is the disk full?).";
        });
    }

    juce::String encode (const juce::File& wav, Format format, float gain, const juce::File& destination, const Tags& tags,
                         std::atomic<float>* progress, const std::function<bool()>& shouldStop)
    {
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatReader> reader (wavFormat.createReaderFor (wav.createInputStream().release(), true));

        if (reader == nullptr)
            return "Couldn't read the rendered mix.";

        juce::TemporaryFile temp (destination);
        juce::int64 encodedBytes = 0;
        std::unique_ptr<juce::OutputStream> out;

        if (auto file = temp.getFile().createOutputStream())
            out = std::make_unique<MeasuredStream> (std::move (file), encodedBytes);
        else
            return "Couldn't write to " + destination.getParentDirectory().getFullPathName();

        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate (exportSampleRate)
                           .withNumChannels ((int) reader->numChannels)
                           .withBitsPerSample (16);

        std::unique_ptr<juce::AudioFormatWriter> writer;
        juce::FlacAudioFormat flac;
        std::unique_ptr<te::LibLameEncoderAudioFormat> lame;

        if (format == Format::mp3)
        {
            lame = std::make_unique<te::LibLameEncoderAudioFormat> (te::LibLameEncoderAudioFormat::getDefaultLibraryName());

            if (! lame->isLibraryLoaded())
                return "The MP3 encoder (libmp3lame) couldn't be loaded.";

            writer = lame->createWriterFor (out, options.withQualityOptionIndex (lame->getQualityOptions().indexOf ("320 Kb/s CBR")));
        }
        else
        {
            writer = flac.createWriterFor (out, options);
        }

        if (writer == nullptr)
            return "Couldn't start the " + getExtension (format).substring (1).toUpperCase() + " encoder.";

        constexpr int blockSize = 32768;
        juce::AudioBuffer<float> buffer ((int) reader->numChannels, blockSize);
        juce::Random random;

        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += blockSize)
        {
            if (shouldStop && shouldStop())
                return "Export cancelled.";

            auto n = (int) std::min<juce::int64> (blockSize, reader->lengthInSamples - pos);
            reader->read (&buffer, 0, n, pos, true, true);
            buffer.applyGain (0, n, gain);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                {
                    // FLAC is 16-bit: add triangular dither (+-1 LSB) so quiet fades don't turn
                    // into grainy distortion. LAME does its own conversion.
                    if (format == Format::flac)
                        data[i] += (random.nextFloat() - random.nextFloat()) / 32768.0f;

                    data[i] = juce::jlimit (-1.0f, 1.0f, data[i]);
                }
            }

            if (! writer->writeFromAudioSampleBuffer (buffer, 0, n))
                return "Writing the export failed (is the disk full?).";

            if (progress != nullptr)
                *progress = (float) ((double) (pos + n) / (double) reader->lengthInSamples);
        }

        writer = nullptr;   // flushes and finalises the file (e.g. the MP3's length tag)

        if (temp.getFile().getSize() != encodedBytes)
            return "Writing the export stopped before the end (is the disk full?). " + destination.getFileName() + " was not saved.";

        std::optional<Artwork> artwork;

        if (tags.artwork != juce::File())
        {
            juce::String artworkError;

            if (artwork = loadArtwork (tags.artwork, artworkError); ! artwork)
                return "The cover image can't be used: " + artworkError;
        }

        if (auto error = format == Format::mp3 ? writeMp3Tags (temp.getFile(), getTitleTag (destination), tags, artwork)
                                               : writeFlacTags (temp.getFile(), getTitleTag (destination), tags, artwork);
            error.isNotEmpty())
            return error;

        // The finished file (after its tags were written) must decode to the whole mix (CLEANUP-AUDIT C3).
        {
            std::unique_ptr<juce::AudioFormatReader> written (format == Format::mp3 ? Mpg123AudioFormat().createReaderFor (temp.getFile().createInputStream().release(), true)
                                                                                    : flac.createReaderFor (temp.getFile().createInputStream().release(), true));

            if (written == nullptr || written->lengthInSamples != reader->lengthInSamples)
                return "Writing the export stopped before the end (is the disk full?). " + destination.getFileName() + " was not saved.";
        }

        if (! temp.overwriteTargetFileWithTemporary())
            return "Couldn't save " + destination.getFullPathName();

        return {};
    }

    juce::String writeTags (const juce::File& file, Format format, const juce::String& title, const Tags& tags)
    {
        return format == Format::mp3 ? writeMp3Tags (file, title, tags, std::nullopt) : writeFlacTags (file, title, tags, std::nullopt);
    }
}
