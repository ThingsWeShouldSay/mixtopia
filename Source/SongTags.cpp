#include "SongTags.h"
#include <mpg123.h>

namespace mixtopia::tags
{
    namespace
    {
        juce::String fromMpg123 (const mpg123_string* s)
        {
            return s != nullptr && s->p != nullptr && s->fill > 0 ? juce::String::fromUTF8 (s->p, (int) s->fill).trim() : juce::String();
        }

        // ID3v1 fields: Latin-1, padded with zeros or spaces.
        juce::String fromId3v1 (const char* field, int size)
        {
            juce::String text;

            for (int i = 0; i < size && field[i] != 0; ++i)
                text += (juce::juce_wchar) (juce::uint8) field[i];

            return text.trim();
        }

        Tags readMp3 (const juce::File& file)
        {
            Tags tags;
            int error = 0;
            auto* handle = mpg123_new (nullptr, &error);

            if (handle == nullptr)
                return tags;

            mpg123_param2 (handle, MPG123_FLAGS, MPG123_QUIET, 0);

            // The ID3v2 tag is at the start; scanning the frames finds one later in the file too
            // (and an ID3v1 tag at the end).
            if (mpg123_open (handle, file.getFullPathName().toRawUTF8()) == MPG123_OK)
            {
                mpg123_scan (handle);
                mpg123_id3v1* v1 = nullptr;
                mpg123_id3v2* v2 = nullptr;

                if (mpg123_id3 (handle, &v1, &v2) == MPG123_OK)
                {
                    if (v2 != nullptr)
                    {
                        tags.title = fromMpg123 (v2->title);
                        tags.artist = fromMpg123 (v2->artist);
                    }

                    if (v1 != nullptr)
                    {
                        if (tags.title.isEmpty())   tags.title = fromId3v1 (v1->title, 30);
                        if (tags.artist.isEmpty())  tags.artist = fromId3v1 (v1->artist, 30);
                    }
                }

                mpg123_close (handle);
            }

            mpg123_delete (handle);
            return tags;
        }

        // FLAC: "fLaC", then metadata blocks (1-byte flags/type, 3-byte big-endian length); type 4
        // holds the Vorbis comments: little-endian lengths, "KEY=value" in UTF-8.
        Tags readFlac (const juce::File& file)
        {
            Tags tags;
            juce::FileInputStream in (file);
            char magic[4] = {};

            if (! in.openedOk() || in.read (magic, 4) != 4 || juce::String (magic, 4) != "fLaC")
                return tags;

            for (bool last = false; ! last && ! in.isExhausted();)
            {
                auto header = (juce::uint8) in.readByte();
                last = (header & 0x80) != 0;
                auto length = ((int) (juce::uint8) in.readByte() << 16) | ((int) (juce::uint8) in.readByte() << 8) | (int) (juce::uint8) in.readByte();
                auto next = in.getPosition() + length;

                if ((header & 0x7f) == 4)
                {
                    auto vendorLength = in.readInt();
                    in.skipNextBytes (vendorLength);
                    auto count = in.readInt();

                    for (int i = 0; i < count && in.getPosition() < next; ++i)
                    {
                        auto size = in.readInt();

                        if (size <= 0 || in.getPosition() + size > next)
                            break;

                        juce::MemoryBlock block;
                        in.readIntoMemoryBlock (block, size);
                        auto comment = juce::String::fromUTF8 ((const char*) block.getData(), (int) block.getSize());
                        auto key = comment.upToFirstOccurrenceOf ("=", false, false).toUpperCase();
                        auto value = comment.fromFirstOccurrenceOf ("=", false, false).trim();

                        if (key == "TITLE" && tags.title.isEmpty())    tags.title = value;
                        if (key == "ARTIST" && tags.artist.isEmpty())  tags.artist = value;
                    }

                    break;
                }

                in.setPosition (next);
            }

            return tags;
        }
    }

    Tags fromFileName (const juce::String& nameWithoutExtension)
    {
        auto name = nameWithoutExtension.trim();

        // A leading track number: "01 ", "02. ", "2. ".
        auto i = 0;

        while (i < name.length() && juce::CharacterFunctions::isDigit (name[i]))
            ++i;

        if (i > 0 && i < name.length() && (name[i] == ' ' || name[i] == '.'))
            name = name.substring (i).trimCharactersAtStart (". ");

        Tags tags;

        if (name.contains (" - "))
        {
            tags.artist = name.upToFirstOccurrenceOf (" - ", false, false).trim();
            tags.title = name.fromFirstOccurrenceOf (" - ", false, false).trim();
        }
        else
        {
            tags.title = name;
        }

        return tags;
    }

    Tags read (const juce::File& file)
    {
        Tags tags;

        if (file.hasFileExtension ("mp3"))
            tags = readMp3 (file);
        else if (file.hasFileExtension ("flac"))
            tags = readFlac (file);

        auto fromName = fromFileName (file.getFileNameWithoutExtension());

        if (tags.title.isEmpty())
            tags.title = fromName.title;

        if (tags.artist.isEmpty())
            tags.artist = fromName.artist;

        return tags;
    }
}
