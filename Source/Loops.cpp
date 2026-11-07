#include "Loops.h"
#include "Chops.h"
#include "EditHelpers.h"
#include "DecodedAudioCache.h"
#include "ClipColours.h"
#include "TempoRamps.h"
#include "SongNames.h"

namespace mixtopia::loops
{
    //==============================================================================
    std::optional<Loop> getLoop (const juce::ValueTree& clipState)
    {
        auto list = clipState.getChildWithName (listType);

        if (! list.isValid())
            return {};

        auto v = list.getChildWithName (loopType);

        if (! v.isValid())
            return {};

        Loop loop;
        loop.start = v[startProperty];
        loop.end = v[endProperty];
        loop.repeats = v.getProperty (repeatsProperty, 1);
        loop.crossfadeMs = v.getProperty (crossfadeProperty, defaultCrossfadeMs);
        loop.copyLines = v[linesProperty].toString() == "copy";

        if (loop.end <= loop.start || loop.repeats < 1)
            return {};

        return loop;
    }

    void setLoop (juce::ValueTree clipState, const std::optional<Loop>& loop, juce::UndoManager* um)
    {
        auto list = clipState.getChildWithName (listType);

        if (list.isValid())
            clipState.removeChild (list, um);

        if (! loop)
            return;

        juce::ValueTree v (loopType, { { startProperty, loop->start }, { endProperty, loop->end }, { repeatsProperty, loop->repeats },
                                       { crossfadeProperty, loop->crossfadeMs }, { linesProperty, loop->copyLines ? "copy" : "once" } });
        list = juce::ValueTree (listType);
        list.appendChild (v, nullptr);
        clipState.appendChild (list, um);
    }

    //==============================================================================
    Place playedToPlace (const std::optional<Loop>& loop, double t)
    {
        if (! loop || t < loop->end)
            return { 0, t };

        auto length = loop->length();

        if (t < loop->end + loop->added())
        {
            auto pass = juce::jlimit (1, loop->repeats, (int) std::floor ((t - loop->end) / length) + 1);
            return { pass, loop->start + (t - loop->end - (pass - 1) * length) };
        }

        return { 0, t - loop->added() };
    }

    double songToPlayed (const std::optional<Loop>& loop, double song)
    {
        return ! loop || song < loop->end ? song : song + loop->added();
    }

    double placeToPlayed (const std::optional<Loop>& loop, Place place)
    {
        if (place.pass == 0 || ! loop)
            return songToPlayed (loop, place.song);

        auto pass = juce::jmin (place.pass, loop->repeats);
        return loop->end + (pass - 1) * loop->length() + juce::jlimit (0.0, loop->length(), place.song - loop->start);
    }

    double transform (const std::optional<Loop>& from, const std::optional<Loop>& to, double played)
    {
        return placeToPlayed (to, playedToPlace (from, played));
    }

    //==============================================================================
    float newPointValue (const std::vector<envelopes::Point>& stored, const std::optional<Loop>& loop, double t)
    {
        return envelopes::evaluate (effectivePoints (stored, loop), t);   // (0 = 0 dB, or filter off, on an empty line)
    }

    std::vector<envelopes::Point> effectivePoints (const std::vector<envelopes::Point>& stored, const std::optional<Loop>& loop)
    {
        if (! loop || ! loop->copyLines || stored.empty())
            return stored;

        // The line as the song had it without repeats (points stored in the repeats are ignored).
        std::vector<envelopes::Point> song;

        for (auto& p : stored)
        {
            auto place = playedToPlace (loop, p.sourceTime);

            if (place.pass == 0)
                song.push_back ({ place.song, p.db });
        }

        if (song.empty())
            return stored;

        std::stable_sort (song.begin(), song.end(), [] (auto& a, auto& b) { return a.sourceTime < b.sourceTime; });
        auto lineAt = [&] (double s) { return envelopes::evaluate (song, s); };

        constexpr double epsilon = 1.0e-6;
        auto atStart = lineAt (loop->start), atEnd = lineAt (loop->end);
        std::vector<envelopes::Point> result;

        for (auto& p : song)
            if (p.sourceTime < loop->end)
                result.push_back (p);

        result.push_back ({ loop->end - epsilon, atEnd });

        for (int pass = 1; pass <= loop->repeats; ++pass)
        {
            auto passStart = loop->end + (pass - 1) * loop->length();
            result.push_back ({ passStart, atStart });

            for (auto& p : song)
                if (p.sourceTime > loop->start && p.sourceTime < loop->end)
                    result.push_back ({ passStart + (p.sourceTime - loop->start), p.db });

            result.push_back ({ passStart + loop->length() - epsilon, atEnd });
        }

        result.push_back ({ loop->end + loop->added(), atEnd });

        for (auto& p : song)
            if (p.sourceTime >= loop->end)
                result.push_back ({ p.sourceTime + loop->added(), p.db });

        return result;
    }

    std::vector<envelopes::Point> getEffectivePoints (const juce::ValueTree& clipState, envelopes::Param param)
    {
        return effectivePoints (envelopes::getPoints (clipState, param), getLoop (clipState));
    }

    //==============================================================================
    bool Spec::parse (const juce::File& loopFile)
    {
        return parseText (loopFile.loadFileAsString());
    }

    bool Spec::parseText (const juce::String& text)
    {
        auto lines = juce::StringArray::fromLines (text);

        if (lines.size() < 5)
            return false;

        wav = juce::File (lines[0]);
        start = lines[1].getLargeIntValue();
        end = lines[2].getLargeIntValue();
        repeats = lines[3].getIntValue();
        crossfade = lines[4].getIntValue();
        return start >= 0 && end > start && repeats >= 0 && crossfade >= 0;
    }

    bool isLoopFile (const juce::File& f)
    {
        return f.hasFileExtension (fileExtension);
    }

    Loop snappedToSamples (Loop loop, double sampleRate)
    {
        if (sampleRate > 0.0)
        {
            loop.start = (double) std::llround (loop.start * sampleRate) / sampleRate;
            loop.end = (double) std::llround (loop.end * sampleRate) / sampleRate;
        }

        return loop;
    }

    namespace
    {
        double wavSampleRate (const juce::File& wav)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (wav.createInputStream().release(), true));
            return reader != nullptr ? reader->sampleRate : 44100.0;
        }
    }

    juce::File getLoopFile (const juce::File& wav, const Loop& loop)
    {
        auto sampleRate = wavSampleRate (wav);
        auto text = wav.getFullPathName() + "\n" + juce::String (std::llround (loop.start * sampleRate)) + "\n"
                    + juce::String (std::llround (loop.end * sampleRate)) + "\n" + juce::String (loop.repeats) + "\n"
                    + juce::String (std::llround (loop.crossfadeMs * sampleRate / 1000.0)) + "\n";
        auto folder = decodedCache::getCacheFolder ("loops");
        folder.createDirectory();
        auto file = folder.getChildFile (juce::String::toHexString (text.hashCode64()) + fileExtension);

        if (! file.existsAsFile() || file.loadFileAsString() != text)
            file.replaceWithText (text);

        return file;
    }

    //==============================================================================
    namespace
    {
        constexpr int scratchSize = 2048;   // > 20 ms at 96 kHz

        struct Scratch
        {
            explicit Scratch (int channels) : buffer (juce::jmax (1, channels), scratchSize) {}
            juce::AudioBuffer<float> buffer;
            float* const* get()     { return buffer.getArrayOfWritePointers(); }
        };

        // The AudioFormatReader side shared by both readers: float pointers at the offset, extra
        // destination channels cleared, then readLooped with the reader's way of fetching the WAV.
        template <typename Fetch>
        bool readFor (const Spec& spec, juce::int64 sourceLength, int sourceChannels, Scratch& scratch,
                      int* const* dest, int numDestChannels, int offset, juce::int64 start, int num, Fetch&& fetch)
        {
            float* d[16] = {};

            for (int c = 0; c < juce::jmin (numDestChannels, 16); ++c)
                d[c] = dest[c] != nullptr ? reinterpret_cast<float*> (dest[c]) + offset : nullptr;

            auto channels = juce::jmin (numDestChannels, sourceChannels, 16);

            for (int c = channels; c < juce::jmin (numDestChannels, 16); ++c)
                if (d[c] != nullptr)
                    juce::FloatVectorOperations::clear (d[c], num);

            return readLooped (spec, sourceLength, d, channels, start, num, scratch.get(), scratchSize, fetch);
        }

        // Through the format manager (song analysis, file info): reads the memory-mapped WAV.
        struct FormatReader : public juce::AudioFormatReader
        {
            FormatReader (const Spec& s) : juce::AudioFormatReader (nullptr, "Mixtopia loop"), spec (s)
            {
                source.reset (juce::WavAudioFormat().createMemoryMappedReader (spec.wav));
                ok = source != nullptr && source->mapEntireFile() && spec.end <= source->lengthInSamples;

                if (! ok)
                    return;

                sampleRate = source->sampleRate;
                bitsPerSample = 32;
                usesFloatingPointData = true;
                numChannels = source->numChannels;
                lengthInSamples = source->lengthInSamples + spec.length() * spec.repeats;
                scratch = std::make_unique<Scratch> ((int) numChannels);
            }

            bool readSamples (int* const* dest, int numDestChannels, int offset, juce::int64 start, int num) override
            {
                return readFor (spec, source->lengthInSamples, (int) numChannels, *scratch, dest, numDestChannels, offset, start, num,
                                [this] (float* const* to, int ch, juce::int64 from, int count)
                                {
                                    return source->readSamples (reinterpret_cast<int* const*> (to), ch, 0, from, count);
                                });
            }

            Spec spec;
            std::unique_ptr<juce::MemoryMappedAudioFormatReader> source;
            std::unique_ptr<Scratch> scratch;
            bool ok = false;
        };

        struct LoopAudioFormat : public juce::AudioFormat
        {
            LoopAudioFormat() : juce::AudioFormat ("Mixtopia loop", fileExtension) {}

            juce::Array<int> getPossibleSampleRates() override   { return { 44100, 48000, 88200, 96000 }; }
            juce::Array<int> getPossibleBitDepths() override     { return { 32 }; }
            bool canDoStereo() override                          { return true; }
            bool canDoMono() override                            { return true; }

            juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails) override
            {
                // (The reader keeps nothing of the stream: it reads the WAV the text names.)
                auto text = in->readEntireStreamAsString();
                Spec spec;
                std::unique_ptr<FormatReader> reader;

                if (spec.parseText (text))
                    reader = std::make_unique<FormatReader> (spec);

                if (reader != nullptr && reader->ok)
                {
                    delete in;
                    return reader.release();
                }

                if (deleteStreamIfOpeningFails)
                    delete in;

                return nullptr;
            }

            std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&) override
            {
                return {};
            }
        };

        // Keeps the parts of decoded songs that looped songs will read soon in memory, so the audio
        // thread never waits for the disk (one background thread for all of them).
        struct Prefetcher : public juce::TimeSliceThread
        {
            Prefetcher() : juce::TimeSliceThread ("Mixtopia loop prefetch")     { startThread (juce::Thread::Priority::normal); }
            ~Prefetcher() override                                              { stopThread (2000); }
        };

        Prefetcher& getPrefetcher()
        {
            static Prefetcher prefetcher;
            return prefetcher;
        }

        // Playback and export (through the direct-reader hook): reads the memory-mapped WAV itself,
        // without Tracktion's file cache (which returns nothing for a few ms when a spot is read
        // for the first time - at the song's start and at every jump back to the loop start). The
        // prefetcher touches the next seconds ahead (through the loop), and the loop start.
        struct DirectReader : public te::AudioFormatReaderWithTimeout,
                              private juce::TimeSliceClient
        {
            DirectReader (const Spec& s) : spec (s)
            {
                source.reset (juce::WavAudioFormat().createMemoryMappedReader (spec.wav));
                ok = source != nullptr && source->mapEntireFile() && spec.end <= source->lengthInSamples;

                if (! ok)
                    return;

                sampleRate = source->sampleRate;
                bitsPerSample = 32;
                usesFloatingPointData = true;
                numChannels = source->numChannels;
                lengthInSamples = source->lengthInSamples + spec.length() * spec.repeats;
                scratch = std::make_unique<Scratch> ((int) numChannels);
                getPrefetcher().addTimeSliceClient (this);
            }

            ~DirectReader() override
            {
                if (ok)
                    getPrefetcher().removeTimeSliceClient (this);
            }

            void setReadTimeout (int) override {}

            bool readSamples (int* const* dest, int numDestChannels, int offset, juce::int64 start, int num) override
            {
                lastRead.store (start, std::memory_order_relaxed);
                return readFor (spec, source->lengthInSamples, (int) numChannels, *scratch, dest, numDestChannels, offset, start, num,
                                [this] (float* const* to, int ch, juce::int64 from, int count)
                                {
                                    return source->readSamples (reinterpret_cast<int* const*> (to), ch, 0, from, count);
                                });
            }

            int useTimeSlice() override
            {
                // From a little before the last read to 4 s after it, along the looped song; and
                // the second before and after the loop start (every join jumps there).
                auto rate = (juce::int64) sampleRate;
                auto touchPlayed = [this] (juce::int64 from, juce::int64 to)
                {
                    auto loopLength = spec.length();

                    for (auto t = juce::jmax ((juce::int64) 0, from); t < juce::jmin (to, lengthInSamples); t += 1024)
                    {
                        auto sourcePos = t < spec.end ? t
                                       : t < spec.end + loopLength * spec.repeats ? spec.start + (t - spec.end) % loopLength
                                                                                  : t - loopLength * spec.repeats;
                        source->touchSample (sourcePos);
                    }
                };

                auto now = lastRead.load (std::memory_order_relaxed);
                touchPlayed (now - rate / 2, now + 4 * rate);

                for (auto t = juce::jmax ((juce::int64) 0, spec.start - rate); t < juce::jmin (spec.start + rate, source->lengthInSamples); t += 1024)
                    source->touchSample (t);

                return 100;
            }

            Spec spec;
            std::unique_ptr<juce::MemoryMappedAudioFormatReader> source;
            std::unique_ptr<Scratch> scratch;
            std::atomic<juce::int64> lastRead { 0 };
            bool ok = false;
        };
    }

    void install (te::Engine& engine)
    {
        engine.getAudioFileFormatManager().addFormat ([] { return new LoopAudioFormat(); }, false, false);
        // Looped songs, and (chopping, 2026-10-28) every decoded copy: a piece starting while the mix
        // plays read nothing from Tracktion's file cache for its first ~10 ms (a dropout at the join).
        // A decoded copy is read as a "loop" with no repeats: the 32-bit float WAV as it is.
        auto isDecodedCopy = [] (const juce::File& file)
        {
            return file.hasFileExtension ("wav") && file.isAChildOf (decodedCache::getCacheFolder ("decoded"));
        };

        engine.getAudioFileManager().isReadDirectly = [isDecodedCopy] (const juce::File& file) { return isLoopFile (file) || isDecodedCopy (file); };
        engine.getAudioFileManager().createDirectReader = [isDecodedCopy] (const juce::File& file) -> std::unique_ptr<te::AudioFormatReaderWithTimeout>
        {
            Spec spec;

            if (isDecodedCopy (file))
                spec.wav = file;
            else if (! isLoopFile (file) || ! spec.parse (file))
                return {};

            auto reader = std::make_unique<DirectReader> (spec);
            return reader->ok ? std::move (reader) : nullptr;
        };
    }

    //==============================================================================
    std::optional<Loop> getLoop (const te::AudioClipBase& clip)
    {
        return getLoop (clip.state);
    }

    juce::File getSongFile (const te::AudioClipBase& clip)
    {
        auto played = clip.getOriginalFile();

        if (! isLoopFile (played))
            return played;

        // The .mixloop names the decoded copy; worked out from the song file only if it can't be
        // read (that fails while the song file is missing, e.g. its drive unplugged).
        if (Spec spec; spec.parse (played))
            return spec.wav;

        auto source = getSourceFile (clip);
        return decodedCache::needsDecoding (source) ? decodedCache::getCachedFile (source) : source;
    }

    double getSampleRate (const te::AudioClipBase& clip)
    {
        auto rate = te::AudioFile (clip.edit.engine, getSongFile (clip)).getInfo().sampleRate;
        return rate > 0.0 ? rate : 44100.0;
    }

    namespace
    {
        double songLength (const te::AudioClipBase& clip)
        {
            return te::AudioFile (clip.edit.engine, getSongFile (clip)).getLength();
        }

        // Moves every song starting at or after `fromSeconds` (except `keep`) and the tempo points
        // there later by `beats` (earlier if negative), keeping songs on their bars.
        void moveLaterBy (te::Edit& edit, const te::Clip& keep, double fromSeconds, double beats)
        {
            if (std::abs (beats) < 1.0e-9)
                return;

            auto fromBeat = tempo::toBeats (edit, te::TimePosition::fromSeconds (fromSeconds)) - 1.0e-6;
            auto plan = tempo::getTempoPlan (edit);
            auto changed = false;

            for (auto& p : plan.points)
                if (p.beat >= fromBeat)
                {
                    p.beat += beats;
                    changed = true;
                }

            if (changed)
                tempo::setTempoPlan (edit, plan);

            for (auto* track : te::getAudioTracks (edit))
                for (auto* c : track->getClips())
                    if (auto* other = dynamic_cast<te::AudioClipBase*> (c); other != nullptr && other != &keep)
                        if (tempo::toBeats (edit, other->getPosition().getStart()) >= fromBeat)
                        {
                            if (tempo::isTempoMatched (*other))
                                tempo::place (*other, tempo::getContentStartBeat (*other) + beats, tempo::getSourceRange (*other));
                            else
                                other->setStart (tempo::toTime (edit, tempo::toBeats (edit, other->getPosition().getStart()) + beats), false, true);
                        }
        }

        // Edit beats taken by `playedSeconds` of the song starting at played time `from`.
        double editBeatsFor (te::AudioClipBase& clip, double from, double playedSeconds)
        {
            if (tempo::isTempoMatched (clip))
                return playedSeconds * tempo::getClipBpm (clip) / 60.0;

            auto start = tempo::sourceToEditSeconds (clip, from);
            return tempo::toBeats (clip.edit, te::TimePosition::fromSeconds (start + playedSeconds))
                     - tempo::toBeats (clip.edit, te::TimePosition::fromSeconds (start));
        }

        juce::String formatBars (double bars)
        {
            auto whole = std::abs (bars - std::round (bars)) < 0.01;
            return whole ? juce::String ((int) std::round (bars)) : juce::String (bars, 1);
        }
    }

    Result apply (te::AudioClipBase& clip, const std::optional<Loop>& requested, bool moveLater)
    {
        auto& edit = clip.edit;
        auto* um = &edit.getUndoManager();
        auto sampleRate = getSampleRate (clip);
        auto old = getLoop (clip);
        auto loop = requested ? std::optional (snappedToSamples (*requested, sampleRate)) : std::nullopt;
        Result result;

        if (old == loop)
            return result;

        auto wav = getSongFile (clip);
        auto range = tempo::getSourceRange (clip);
        auto matched = tempo::isTempoMatched (clip);
        auto bpm = tempo::getClipBpm (clip);
        auto contentStartBeat = matched ? tempo::getContentStartBeat (clip) : 0.0;
        auto clipStart = clip.getPosition().getStart();

        // Where the loop ends in the mix (songs after it move with "move later").
        auto loopEnd = (loop ? loop->end : old->end);
        auto loopEndEdit = tempo::sourceToEditSeconds (clip, songToPlayed (old, loopEnd));
        auto addedBefore = old ? old->added() : 0.0, addedAfter = loop ? loop->added() : 0.0;
        auto deltaBeats = editBeatsFor (clip, loopEnd, addedAfter) - editBeatsFor (clip, loopEnd, addedBefore);

        auto map = [&] (double t) { return transform (old, loop, t); };

        // The lines, beat 1 and the sound start move with the music.
        for (int p = 0; p < envelopes::numParams; ++p)
        {
            auto points = envelopes::getPoints (clip.state, (envelopes::Param) p);

            if (points.empty())
                continue;

            for (auto& point : points)
                point.sourceTime = map (point.sourceTime);

            envelopes::setPoints (clip.state, (envelopes::Param) p, points, um);
        }

        if (clip.state.hasProperty (tempo::downbeatProperty))
            clip.state.setProperty (tempo::downbeatProperty, map (tempo::getDownbeat (clip)), um);

        // Cuts (chopping) move with the music too; set once the clip is in its new place.
        auto cuts = chops::getCuts (clip);

        for (auto& t : cuts)
            t = map (t);

        setLoop (clip.state, loop, um);

        // The file it plays, then its length in beats (auto-tempo), then its place.
        auto file = loop ? getLoopFile (wav, *loop) : wav;
        clip.state.setProperty (te::IDs::source, file.getFullPathName(), um);
        auto playedLength = te::AudioFile (edit.engine, wav).getLength() + addedAfter;

        if (bpm > 0.0)
            clip.getLoopInfo().setNumBeats (playedLength * bpm / 60.0);

        tempo::SourceRange keep { map (range.start), map (range.end) };
        keep.end = juce::jlimit (keep.start + 0.01, playedLength, keep.end);

        if (matched)
        {
            tempo::place (clip, contentStartBeat - (keep.start - range.start) * bpm / 60.0, keep);
        }
        else
        {
            clip.setPosition ({ { clipStart, clipStart + te::TimeDuration::fromSeconds (keep.end - keep.start) },
                                te::TimeDuration::fromSeconds (keep.start) });
        }

        chops::setCuts (clip, cuts, um);

        // Songs never overlap on one lane (user, 2026-10-23): if the longer song now runs into the
        // next song on its lane, everything after the loop moves later to make room - songs on every
        // lane and the tempo points (their lines move with them). Also when asked ("move later").
        auto runsIntoNext = false;

        if (auto* track = dynamic_cast<te::ClipTrack*> (clip.getTrack()))
            for (auto* other : track->getClips())
                if (other != &clip && other->getPosition().getStart() >= clipStart && other->getPosition().getStart() < clip.getPosition().getEnd())
                    runsIntoNext = true;

        if (moveLater || (runsIntoNext && deltaBeats > 0.0))
        {
            moveLaterBy (edit, clip, loopEndEdit, deltaBeats);

            if (! moveLater)
                result.madeRoom = "Made room: the songs after the loop moved " + juce::String (deltaBeats >= 0.0 ? "later" : "earlier") + " by "
                                    + formatBars (std::abs (deltaBeats) / tempo::beatsPerBar) + " bars.";
        }

        result.recoloured = clipcolours::keepApartFromNeighbours (clip);
        return result;
    }

    double snapped (te::AudioClipBase& clip, double songSeconds, tempo::Snap snap)
    {
        auto bpm = tempo::getClipBpm (clip);

        if (bpm <= 0.0 || snap == tempo::Snap::none)
            return songSeconds;

        auto step = (snap == tempo::Snap::bars ? tempo::beatsPerBar : 1) * 60.0 / bpm;
        auto downbeat = playedToSong (getLoop (clip), tempo::getDownbeat (clip));
        return juce::jmax (0.0, downbeat + std::round ((songSeconds - downbeat) / step) * step);
    }

    std::optional<Loop> makeQuick (te::AudioClipBase& clip, double songStart, int bars, juce::String& problem)
    {
        // `bars` bars of the song from `songStart`: exact for the song's tempo (the project's tempo
        // at the playhead for a song without a beat grid). Keeps an existing loop's crossfade and
        // lines setting; 1 repeat.
        auto bpm = tempo::getClipBpm (clip);

        if (bpm <= 0.0)
            bpm = tempo::getProjectBpm (clip.edit, te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (clip, songToPlayed (getLoop (clip), songStart))));

        Loop loop;

        if (auto old = getLoop (clip))
        {
            loop.crossfadeMs = old->crossfadeMs;
            loop.copyLines = old->copyLines;
        }

        loop.start = songStart;
        loop.end = songStart + bars * tempo::beatsPerBar * 60.0 / bpm;
        loop.repeats = 1;

        if (loop.end > songLength (clip))
        {
            problem = "Not enough of the song left after the playhead for " + juce::String (bars) + (bars == 1 ? " bar." : " bars.");
            return {};
        }

        return snappedToSamples (loop, getSampleRate (clip));
    }

    Loop makeDefault (te::AudioClipBase& clip, double songSeconds, bool snap)
    {
        auto bpm = tempo::getClipBpm (clip);
        auto length = songLength (clip);
        Loop loop;

        if (bpm > 0.0)
        {
            auto bar = tempo::beatsPerBar * 60.0 / bpm;
            auto downbeat = tempo::getDownbeat (clip);
            loop.start = snap ? downbeat + std::floor ((songSeconds - downbeat) / bar + 1.0e-6) * bar : songSeconds;
            loop.end = loop.start + 8 * bar;
        }
        else
        {
            loop.start = songSeconds;
            loop.end = songSeconds + 15.0;
        }

        if (loop.end > length)
        {
            auto shift = loop.end - length;
            loop.start -= shift;
            loop.end -= shift;
        }

        loop.start = juce::jmax (0.0, loop.start);
        return snappedToSamples (loop, getSampleRate (clip));
    }

    juce::String shortTitleOf (const te::AudioClipBase& clip)
    {
        return shortTitle (clip.getName());
    }

    double lengthInBeats (te::AudioClipBase& clip, const Loop& loop)
    {
        auto bpm = tempo::getClipBpm (clip);
        return bpm > 0.0 ? loop.length() * bpm / 60.0 : 0.0;
    }

    juce::String formatSongTime (double seconds)
    {
        auto ms = (juce::int64) std::llround (juce::jmax (0.0, seconds) * 1000.0);
        return juce::String::formatted ("%d:%02d.%03d", (int) (ms / 60000), (int) (ms / 1000 % 60), (int) (ms % 1000));
    }

    juce::String songBarText (te::AudioClipBase& clip, double songSeconds)
    {
        auto bpm = tempo::getClipBpm (clip);

        if (bpm <= 0.0)
            return {};

        // The nearest beat, and how far off it the point is (ms) when it isn't on it.
        auto beats = (songSeconds - tempo::getDownbeat (clip)) * bpm / 60.0;
        auto nearest = std::round (beats);
        auto bar = (int) std::floor (nearest / tempo::beatsPerBar) + 1;
        auto beat = (int) (nearest - (bar - 1) * tempo::beatsPerBar) + 1;
        auto offMs = (beats - nearest) * 60.0 / bpm * 1000.0;
        auto off = std::abs (offMs) < 0.5 ? juce::String()
                                          : (offMs < 0 ? juce::String (juce::CharPointer_UTF8 (" \xe2\x88\x92")) : juce::String (" +"))
                                              + juce::String (std::abs (offMs), 1) + " ms";
        return "song bar " + juce::String (bar) + "." + juce::String (beat) + off;
    }

    juce::String describe (te::AudioClipBase& clip, const Loop& loop)
    {
        auto bpm = tempo::getClipBpm (clip);
        auto times = juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (loop.repeats) + "  ";

        if (bpm <= 0.0)
            return formatSongTime (loop.start) + "-" + formatSongTime (loop.end) + "  " + times + "+" + juce::String (loop.added(), 1) + " s";

        auto bar = tempo::beatsPerBar * 60.0 / bpm;
        auto barOf = [&] (double s) { return (s - tempo::getDownbeat (clip)) / bar + 1.0; };
        auto onBars = std::abs (barOf (loop.start) - std::round (barOf (loop.start))) < 0.002
                       && std::abs (barOf (loop.end) - std::round (barOf (loop.end))) < 0.002;
        auto range = onBars ? "bar " + juce::String ((int) std::round (barOf (loop.start))) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"))
                                + juce::String ((int) std::round (barOf (loop.end)))
                            : formatSongTime (loop.start) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) + formatSongTime (loop.end);
        return range + "  " + times + "+" + formatBars (lengthInBeats (clip, loop) * loop.repeats / tempo::beatsPerBar) + " bars";
    }

    juce::String describeAdded (te::AudioClipBase& clip, const Loop& loop)
    {
        auto dot = juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
        auto beats = editBeatsFor (clip, loop.end, loop.added());
        auto from = tempo::sourceToEditSeconds (clip, loop.end);
        auto seconds = tempo::toTime (clip.edit, tempo::toBeats (clip.edit, te::TimePosition::fromSeconds (from)) + beats).inSeconds() - from;
        auto secondsText = "+" + juce::String (seconds, 1) + " s";

        if (tempo::getClipBpm (clip) <= 0.0)
            return secondsText;

        return "+" + formatBars (beats / tempo::beatsPerBar) + " bars" + dot + secondsText;
    }

    std::optional<double> parseSongTime (te::AudioClipBase& clip, const juce::String& input)
    {
        auto text = input.trim().toLowerCase();

        if (text.startsWith ("bar") || text.startsWith ("song bar"))
        {
            auto bpm = tempo::getClipBpm (clip);

            if (bpm <= 0.0)
                return {};

            auto numbers = text.fromFirstOccurrenceOf ("bar", false, false).trim();
            auto bar = numbers.upToFirstOccurrenceOf (".", false, false).getIntValue();
            auto beat = numbers.contains (".") ? numbers.fromFirstOccurrenceOf (".", false, false).getIntValue() : 1;
            return tempo::getDownbeat (clip) + ((bar - 1) * tempo::beatsPerBar + (beat - 1)) * 60.0 / bpm;
        }

        if (text.containsChar (':'))
        {
            auto minutes = text.upToFirstOccurrenceOf (":", false, false).getIntValue();
            auto seconds = text.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
            return minutes * 60.0 + seconds;
        }

        if (text.containsOnly ("0123456789.") && text.isNotEmpty())
            return text.getDoubleValue();

        return {};
    }
}
