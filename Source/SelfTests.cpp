#include "SelfTests.h"
#include <JuceHeader.h>
#include <thread>
#include "AppBehaviour.h"
#include "EditHelpers.h"
#include "Mpg123AudioFormat.h"
#include "Loops.h"
#include "Chops.h"
#include "ClipTempo.h"
#include "DevSpecs.h"
#include "Settings.h"
#include "ClipColours.h"
#include "Metronome.h"
#include "Project.h"
#include "Exporter.h"
#include "MasterLimiter.h"
#include "ClipMixer.h"
#include "ClipEnvelopes.h"
#include "LoudnessOverview.h"
#include "BeatGrid.h"
#include "KeyDetection.h"
#include "Loudness.h"
#include "LevelMatch.h"
#include "SongAnalysis.h"
#include "Library.h"
#include "SongTags.h"
#include "LibraryPanel.h"
#include "MainComponent.h"
#include "TimelineComponent.h"
#include "AppLookAndFeel.h"
#include <sys/resource.h>
#include <csignal>

// The self-tests (`Mixtopia --self-test <audio file>`; moved out of Main.cpp in the 2026-10-23
// code-health review): non-audio checks of every part, one PASS / FAIL line each.
namespace mixtopia
{
    bool selfTest (te::Engine& engine, const juce::File& audioFile)
    {
        bool allPassed = true;
        auto check = [&] (bool ok, const juce::String& what)
        {
            std::cout << (ok ? "PASS  " : "FAIL  ") << what << std::endl;
            allPassed = allPassed && ok;
        };

        // Dropped-text parsing (KDE/Wayland delivers file drops as text/uri-list text).
        auto dropped = mixtopia::filesFromDroppedText ("file:///home/me/My%20Music/a%2Bb.mp3\r\nfile:///tmp/x.flac\r\n");
        check (dropped == juce::StringArray { "/home/me/My Music/a+b.mp3", "/tmp/x.flac" },
               "text drop -> file paths: " + dropped.joinIntoString (" | "));

        // MP3s go through libmpg123 with gapless trimming. The test files are 10.000 s at 44.1 kHz
        // (encoded by LAME, which adds 1105 samples of delay + padding that must be removed).
        if (audioFile.hasFileExtension ("mp3"))
        {
            std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (engine, audioFile));
            check (reader != nullptr && reader->getFormatName() == "MP3 file" && reader->lengthInSamples == 441000,
                   "MP3 read by " + (reader ? reader->getFormatName() : juce::String ("nothing"))
                     + ", length " + juce::String (reader ? reader->lengthInSamples : 0) + " samples (want 441000)");

            auto playable = mixtopia::decodedCache::getPlayableFile (engine, audioFile);
            auto info = te::AudioFile (engine, playable).getInfo();
            check (playable.hasFileExtension ("wav") && info.lengthInSamples == 441000,
                   "decoded copy " + playable.getFileName() + ": " + juce::String (info.lengthInSamples)
                     + " samples (want 441000)");
        }

        // Trim then undo restores the clip; redo re-applies the trim.
        auto edit = mixtopia::createEdit (engine);
        check (std::abs (mixtopia::tempo::getProjectBpm (*edit) - 140.0) < 1.0e-9, "new project starts at 140 BPM");
        check (te::getAudioTracks (*edit).size() == 3, "new project starts with 3 lanes");
        auto clip = mixtopia::insertAudioFile (*te::getAudioTracks (*edit)[0], audioFile, te::TimePosition());
        check (clip != nullptr, "insert " + audioFile.getFileName());

        if (clip != nullptr)
        {
            auto& um = edit->getUndoManager();
            auto original = clip->getPosition();
            auto trimmed = original;
            trimmed.time = trimmed.time.withStart (trimmed.time.getStart() + te::TimeDuration::fromSeconds (2.0));
            trimmed.offset = trimmed.offset + te::TimeDuration::fromSeconds (2.0);

            um.beginNewTransaction ("Trim clip");
            clip->setPosition (trimmed);
            um.beginNewTransaction ("Move clip");
            clip->moveTo (*te::getAudioTracks (*edit)[1]);

            um.undo();
            check (clip->getTrack() == te::getAudioTracks (*edit)[0], "undo move -> back on lane 1");
            um.undo();
            check (clip->getPosition() == original, "undo trim -> original start/length/offset");
            um.redo();
            check (clip->getPosition() == trimmed, "redo trim -> trimmed again");
        }

        // Tempo matching: a 120 BPM song with beat 1 at 0.25 s, in a 128 BPM project.
        if (clip != nullptr)
        {
            namespace tempo = mixtopia::tempo;
            auto& um = edit->getUndoManager();
            auto isOnBeat = [&] (te::TimePosition t)
            {
                auto beats = edit->tempoSequence.toBeats (t).inBeats();
                return std::abs (beats - std::round (beats)) < 1.0e-6;
            };

            um.beginNewTransaction ("Tempo");
            tempo::setProjectBpm (*edit, 128.0);
            clip->setStart (te::TimePosition::fromSeconds (3.3), false, true);
            auto before = clip->getPosition();

            um.beginNewTransaction ("Set BPM");
            tempo::setBpmAndDownbeat (*clip, 120.0, 0.25, tempo::Snap::beats);
            check (clip->getAutoTempo() && std::abs (tempo::getStretchRatio (*clip) - 120.0 / 128.0) < 1.0e-9,
                   "set BPM -> stretched by 120/128");
            check (isOnBeat (tempo::getGridAnchor (*clip)), "set BPM with snap -> song's beats on project beats");
            check (std::abs (tempo::sourceToEditSeconds (*clip, 0.25 + 0.5) - tempo::sourceToEditSeconds (*clip, 0.25)
                              - 0.5 * 120.0 / 128.0) < 1.0e-9,
                   "one song beat (0.5 s) lasts one project beat (0.46875 s)");

            // Snapping to bars puts beat 1 (the downbeat) on a bar start.
            um.beginNewTransaction ("Set BPM");
            tempo::setBpmAndDownbeat (*clip, 120.0, 0.25 + 3 * 0.5, tempo::Snap::bars);
            auto downbeatBeats = edit->tempoSequence.toBeats (te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (*clip, 0.25 + 3 * 0.5))).inBeats();
            check (std::abs (downbeatBeats / 4.0 - std::round (downbeatBeats / 4.0)) < 1.0e-6, "set BPM with bar snap -> beat 1 on a bar start");
            um.undo();

            auto anchorBeat = edit->tempoSequence.toBeats (tempo::getGridAnchor (*clip)).inBeats();
            um.beginNewTransaction ("Tempo");
            tempo::setProjectBpm (*edit, 140.0);
            check (std::abs (edit->tempoSequence.toBeats (tempo::getGridAnchor (*clip)).inBeats() - anchorBeat) < 1.0e-6,
                   "project tempo 128 -> 140 keeps the song on the same beat");
            check (std::abs (tempo::getStretchRatio (*clip) - 120.0 / 140.0) < 1.0e-9, "... and re-stretches it");

            um.undo();
            um.undo();
            check (! clip->getAutoTempo() && tempo::getClipBpm (*clip) == 0.0 && clip->getPosition() == before,
                   "undo tempo change and BPM -> back to original speed and place");

            // Tempo ramps (M6): a ramp added before a song keeps it on its bars (with its lines);
            // linear per beat; undo/redo; tidying; save and reopen.
            {
                auto e = mixtopia::createEdit (engine);
                auto c = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], audioFile, te::TimePosition::fromSeconds (40.0));
                tempo::setBpmAndDownbeat (*c, 120.0, 0.25, tempo::Snap::bars);
                mixtopia::envelopes::setPoints (c->state, mixtopia::envelopes::Param::volume, { { 1.0, -6.0f } }, nullptr);
                e->getUndoManager().clearUndoHistory();

                auto before = c->getPosition();
                auto contentStartBeat = tempo::getContentStartBeat (*c);
                auto keep = tempo::getSourceRange (*c);
                auto beatOne = tempo::toBeats (*e, te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (*c, 0.25)));
                auto pointBeat = tempo::toBeats (*e, te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (*c, 1.0)));

                // Two points placed as in the app: bar 5 at the project tempo, bar 21 at 135.
                auto plan = tempo::getTempoPlan (*e);
                plan = tempo::withTempoAt (plan, 16.0, 140.0);
                plan = tempo::withTempoAt (plan, 80.0, 135.0);
                e->getUndoManager().beginNewTransaction ("Change tempo");
                tempo::setTempoPlan (*e, plan);

                auto back = tempo::getTempoPlan (*e);
                check (back.projectBpm == 140.0 && back.points.size() == 2 && back.points[0].beat == 16.0 && back.points[0].bpm == 140.0
                         && back.points[1].beat == 80.0 && back.points[1].bpm == 135.0 && tempo::hasTempoChanges (*e),
                       "tempo points: bar 5 at 140, bar 21 at 135 - placed and read back");
                // (Tracktion evaluates ramps in small steps: 137.55 at the halfway point.)
                check (std::abs (tempo::getProjectBpm (*e, tempo::toTime (*e, 8.0)) - 140.0) < 1.0e-9
                         && std::abs (tempo::getProjectBpm (*e, tempo::toTime (*e, 48.0)) - 137.5) < 0.1
                         && std::abs (tempo::getProjectBpm (*e, tempo::toTime (*e, 100.0)) - 135.0) < 1.0e-9
                         && tempo::rampAt (back, 48.0) == 0 && tempo::rampAt (back, 8.0) < 0 && tempo::rampAt (back, 100.0) < 0,
                       "tempo points: steady before the first, a straight ramp between the two (halfway "
                         + juce::String (tempo::getProjectBpm (*e, tempo::toTime (*e, 48.0)), 3) + " BPM, want 137.5), holds after");

                auto keepAfter = tempo::getSourceRange (*c);
                check (std::abs (tempo::getContentStartBeat (*c) - contentStartBeat) < 1.0e-6
                         && std::abs (keepAfter.start - keep.start) < 1.0e-6 && std::abs (keepAfter.end - keep.end) < 1.0e-6
                         && std::abs (tempo::toBeats (*e, te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (*c, 0.25))) - beatOne) < 1.0e-6
                         && std::abs (tempo::toBeats (*e, te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (*c, 1.0))) - pointBeat) < 1.0e-6
                         && c->getPosition().getStart() != before.getStart(),
                       "tempo ramp before a song: it moves in time but keeps its beats, the part it plays and its volume point");

                auto rampedPosition = c->getPosition();
                e->getUndoManager().undo();
                check (! tempo::hasTempoChanges (*e) && c->getPosition() == before, "undo the tempo points -> tempo and song back exactly");
                e->getUndoManager().redo();
                check (tempo::getTempoPlan (*e).points.size() == 2 && c->getPosition() == rampedPosition, "redo the tempo points");

                e->getUndoManager().beginNewTransaction ("Tempo");
                tempo::setProjectBpm (*e, 142.0);
                auto changedStart = tempo::getTempoPlan (*e);
                check (changedStart.projectBpm == 142.0 && changedStart.points[0].bpm == 142.0 && changedStart.points[1].bpm == 135.0
                         && std::abs (tempo::toBeats (*e, te::TimePosition::fromSeconds (tempo::sourceToEditSeconds (*c, 0.25))) - beatOne) < 1.0e-6,
                       "project tempo typed -> the first point gets it too (it sets the tempo before it); the song stays on its bar");
                e->getUndoManager().undo();

                check (std::abs (tempo::getMaxStretchPercent (*c) - 100.0 * (135.0 / 120.0 - 1.0)) < 0.01,
                       "stretch warning: a 120 BPM song after a ramp to 135 is sped up " + juce::String (tempo::getMaxStretchPercent (*c), 2) + "% (want 12.50)");

                // A single point never makes a ramp: it's steady everywhere (and is the project tempo).
                {
                    tempo::TempoPlan single;
                    single.points = { { 80.0, 135.0 } };
                    e->getUndoManager().beginNewTransaction ("Change tempo");
                    tempo::setTempoPlan (*e, single);
                    check (std::abs (tempo::getProjectBpm (*e, tempo::toTime (*e, 8.0)) - 135.0) < 1.0e-9
                             && std::abs (tempo::getProjectBpm (*e, tempo::toTime (*e, 60.0)) - 135.0) < 1.0e-9
                             && tempo::getTempoPlan (*e).projectBpm == 135.0,
                           "a single tempo point never ramps: its tempo holds before and after it");
                    e->getUndoManager().undo();
                }

                // Projects from the first M6 build (ramps stored as start/end pairs with hold curves) play
                // the same, and still do after an edit rewrites them as points.
                {
                    auto old = mixtopia::createEdit (engine);
                    auto& ts = old->tempoSequence;
                    ts.getTempo (0)->setBpm (140.0);
                    ts.getTempo (0)->setCurve (1.0f);
                    ts.insertTempo (te::BeatPosition::fromBeats (16.0), 140.0, 0.0f);
                    ts.insertTempo (te::BeatPosition::fromBeats (80.0), 135.0, 1.0f);
                    ts.insertTempo (te::BeatPosition::fromBeats (120.0), 135.0, 0.0f);
                    ts.insertTempo (te::BeatPosition::fromBeats (124.0), 128.0, 1.0f);

                    std::vector<double> times;

                    for (double b = 0.0; b <= 200.0; b += 0.5)
                        times.push_back (tempo::toTime (*old, b).inSeconds());

                    tempo::setTempoPlan (*old, tempo::getTempoPlan (*old));
                    auto worst = 0.0;

                    for (size_t i = 0; i < times.size(); ++i)
                        worst = juce::jmax (worst, std::abs (tempo::toTime (*old, (double) i * 0.5).inSeconds() - times[i]));

                    check (tempo::getTempoPlan (*old).points.size() == 4 && worst < 1.0e-9,
                           "first-build ramp projects read as points and play the same after an edit (worst "
                             + juce::String (worst * 1000.0, 6) + " ms)");
                    mixtopia::destroyEdit (old);
                }

                tempo::TempoPlan messy;
                messy.projectBpm = 150.0;
                messy.points = { { 21.0, 130.0 }, { 20.0, 300.0 }, { 0.0, 120.0 }, { 22.0, 125.0 } };
                auto tidy = tempo::tidied (messy);
                check (tidy.points.size() == 3 && tidy.points[0].beat == 4.0 && tidy.points[0].bpm == 120.0
                         && tidy.points[1].beat == 20.0 && tidy.points[1].bpm == tempo::maxBpm
                         && tidy.points[2].beat == 24.0 && tidy.points[2].bpm == 125.0 && tidy.projectBpm == 120.0,
                       "tempo points tidied: on bar lines from bar 2, one per bar (later wins), in order, BPM in range, project tempo = first point's");

                auto file = juce::File::createTempFile (".mixtopia");
                auto saved = mixtopia::project::save (*e, file).isEmpty();
                auto contents = mixtopia::project::read (file);
                auto reopened = contents.error.isEmpty() ? mixtopia::project::createEdit (engine, contents) : nullptr;
                auto reopenedPlan = reopened != nullptr ? tempo::getTempoPlan (*reopened) : tempo::TempoPlan {};
                auto* reopenedClip = reopened != nullptr ? te::getAudioTracks (*reopened)[0]->getClips()[0] : nullptr;
                auto samePlace = [] (te::ClipPosition x, te::ClipPosition y)
                {
                    return std::abs ((x.getStart() - y.getStart()).inSeconds()) < 1.0e-6 && std::abs ((x.getEnd() - y.getEnd()).inSeconds()) < 1.0e-6
                             && std::abs ((x.getOffset() - y.getOffset()).inSeconds()) < 1.0e-6;
                };
                check (saved && reopenedPlan.points.size() == 2 && reopenedPlan.points[1].beat == 80.0 && reopenedPlan.points[1].bpm == 135.0
                         && reopenedClip != nullptr && samePlace (reopenedClip->getPosition(), rampedPosition),
                       "tempo points: saved and reopened with the song in the same place");

                // A full disk can't destroy a saved project (CLEANUP-AUDIT C2): with writes cut short the way a filling
                // disk cuts them (this process's file-size limit lowered, its signal ignored), the save reports an error and
                // the project on disk stays as it was. (Before the fix the save "succeeded" and left a cut-off file.)
                {
                    juce::MemoryBlock good;
                    file.loadFileAsData (good);
                    rlimit normal {};
                    getrlimit (RLIMIT_FSIZE, &normal);
                    auto oldHandler = std::signal (SIGXFSZ, SIG_IGN);
                    rlimit small { 2048, normal.rlim_max };
                    setrlimit (RLIMIT_FSIZE, &small);
                    auto error = mixtopia::project::save (*e, file);
                    auto copy = file.getSiblingFile (file.getFileNameWithoutExtension() + "-copy.mixtopia");
                    auto copyWritten = mixtopia::settings::replaceFileContents (copy, good);
                    setrlimit (RLIMIT_FSIZE, &normal);
                    std::signal (SIGXFSZ, oldHandler);
                    juce::MemoryBlock after;
                    file.loadFileAsData (after);
                    check (good.getSize() > 2048 && error.isNotEmpty() && after == good && ! copyWritten && ! copy.existsAsFile(),
                           "save on a full disk: reported (" + error.upToFirstOccurrenceOf (" (", false, false).substring (0, 14)
                             + "...), the project on disk unchanged, no cut-off copy written");
                    check (mixtopia::settings::replaceFileContents (copy, good) && copy.loadFileAsString() == file.loadFileAsString(),
                           "safe write: a normal write is complete and byte-identical");
                    copy.deleteFile();
                }

                // ...and an export can't come out short (CLEANUP-AUDIT C3): the render stops at the limit, which is now
                // noticed - an error and no file - where before the export "succeeded" with only the start of the mix.
                {
                    rlimit normal {};
                    getrlimit (RLIMIT_FSIZE, &normal);
                    auto oldHandler = std::signal (SIGXFSZ, SIG_IGN);
                    rlimit small { 262144, normal.rlim_max };
                    setrlimit (RLIMIT_FSIZE, &small);
                    juce::String renderError;
                    auto wav = mixtopia::exporter::renderMix (*e, renderError);
                    setrlimit (RLIMIT_FSIZE, &normal);
                    std::signal (SIGXFSZ, oldHandler);
                    check (wav == juce::File() && renderError.contains ("before the end"),
                           "export on a full disk: refused (" + renderError.substring (0, 40) + "...), no short mix written");
                }

                // ...nor can the encoded file: the disk filling while the FLAC/MP3 is written (half its size), or while its
                // tags are written (one byte short at the very end). A FLAC's header holds the length it was GIVEN, so
                // reading it back can't tell (audit review of C3); each file is checked against what was written to it.
                {
                    juce::String renderError;
                    auto wav = mixtopia::exporter::renderMix (*e, renderError);
                    mixtopia::exporter::Tags tags { "Artist", "Album", "Genre", "2026", {} };
                    auto whole = file.getSiblingFile ("whole.flac");
                    auto fullError = mixtopia::exporter::encode (wav, mixtopia::exporter::Format::flac, 1.0f, whole, tags);

                    auto encodeLimited = [&] (mixtopia::exporter::Format format, juce::int64 limit, const juce::String& name)
                    {
                        auto out = file.getSiblingFile (name);
                        out.deleteFile();
                        rlimit normal {};
                        getrlimit (RLIMIT_FSIZE, &normal);
                        auto oldHandler = std::signal (SIGXFSZ, SIG_IGN);
                        rlimit small { (rlim_t) limit, normal.rlim_max };
                        setrlimit (RLIMIT_FSIZE, &small);
                        auto error = mixtopia::exporter::encode (wav, format, 1.0f, out, tags);
                        setrlimit (RLIMIT_FSIZE, &normal);
                        std::signal (SIGXFSZ, oldHandler);
                        return std::make_pair (error, out.existsAsFile());
                    };

                    auto size = whole.getSize();
                    auto [flacHalfError, flacHalfFile] = encodeLimited (mixtopia::exporter::Format::flac, size / 2, "half.flac");
                    auto [mp3HalfError, mp3HalfFile] = encodeLimited (mixtopia::exporter::Format::mp3, size / 8, "half.mp3");

                    // Only the encoder's very last write cut short (audit review): the MP3 encoder is exact run to run, so its
                    // own output's size is the whole MP3 minus the ID3 tag Mixtopia puts in front; the disk is "full" one byte
                    // before that.
                    auto wholeMp3 = file.getSiblingFile ("whole.mp3");
                    auto mp3Error = mixtopia::exporter::encode (wav, mixtopia::exporter::Format::mp3, 1.0f, wholeMp3, tags);
                    juce::MemoryBlock mp3Bytes;
                    wholeMp3.loadFileAsData (mp3Bytes);
                    auto* b = static_cast<const juce::uint8*> (mp3Bytes.getData());
                    auto id3Size = mp3Bytes.getSize() > 10 && std::memcmp (b, "ID3", 3) == 0
                                     ? 10 + ((juce::int64) (b[6] & 0x7f) << 21 | (b[7] & 0x7f) << 14 | (b[8] & 0x7f) << 7 | (b[9] & 0x7f)) : 0;
                    auto encoderBytes = (juce::int64) mp3Bytes.getSize() - id3Size;
                    auto [mp3FlushError, mp3FlushFile] = encodeLimited (mixtopia::exporter::Format::mp3, encoderBytes - 1, "flush-short.mp3");
                    check (mp3Error.isEmpty() && id3Size > 10 && encoderBytes > 65536,
                           "encode control: the whole MP3 written (" + juce::String (encoderBytes) + " bytes from the encoder + " + juce::String (id3Size) + " of tags)");
                    check (mp3FlushError.isNotEmpty() && ! mp3FlushFile, "MP3 export, disk full on the encoder's last byte: refused ("
                                                                           + mp3FlushError.substring (0, 40) + "...), no file saved");
                    wholeMp3.deleteFile();

                    // The tags: written once to learn the tagged size (each FLAC encode differs by its random dither, so
                    // this is done on one fixed file), then again with the disk "full" one byte before the end.
                    auto tagged = file.getSiblingFile ("tagged.flac"), tagShort = file.getSiblingFile ("tag-short.flac");
                    whole.copyFileTo (tagged);
                    whole.copyFileTo (tagShort);
                    auto taggedError = mixtopia::exporter::writeTags (tagged, mixtopia::exporter::Format::flac, "Title", tags);
                    juce::MemoryBlock untagged;
                    tagShort.loadFileAsData (untagged);
                    juce::String flacTagError;
                    {
                        rlimit normal {};
                        getrlimit (RLIMIT_FSIZE, &normal);
                        auto oldHandler = std::signal (SIGXFSZ, SIG_IGN);
                        rlimit small { (rlim_t) (tagged.getSize() - 1), normal.rlim_max };
                        setrlimit (RLIMIT_FSIZE, &small);
                        flacTagError = mixtopia::exporter::writeTags (tagShort, mixtopia::exporter::Format::flac, "Title", tags);
                        setrlimit (RLIMIT_FSIZE, &normal);
                        std::signal (SIGXFSZ, oldHandler);
                    }
                    juce::MemoryBlock afterTagging;
                    tagShort.loadFileAsData (afterTagging);
                    auto flacTagFile = afterTagging != untagged;   // (refused: the file is left as it was)
                    check (taggedError.isEmpty() && tagged.getSize() > 0, "tags control: written to a fixed FLAC (" + juce::String (tagged.getSize()) + " bytes)");
                    tagged.deleteFile();
                    tagShort.deleteFile();
                    check (wav != juce::File() && fullError.isEmpty() && size > 65536,
                           "encode control: the whole FLAC written (" + juce::String (size) + " bytes)");
                    check (flacHalfError.isNotEmpty() && ! flacHalfFile, "FLAC export, disk full while encoding: refused ("
                                                                           + flacHalfError.substring (0, 40) + "...), no file saved");
                    check (flacTagError.isNotEmpty() && ! flacTagFile, "FLAC export, disk full on the last byte of its tags: refused ("
                                                                         + flacTagError.substring (0, 40) + "...), the file left as it was");
                    check (mp3HalfError.isNotEmpty() && ! mp3HalfFile, "MP3 export, disk full while encoding: refused ("
                                                                         + mp3HalfError.substring (0, 40) + "...), no file saved");
                    whole.deleteFile();
                    wav.deleteFile();
                }

                reopenedClip = nullptr;
                mixtopia::destroyEdit (reopened);
                file.deleteFile();
                c = nullptr;
                mixtopia::destroyEdit (e);
            }

            // Lanes: remove one with a clip on it, then undo.
            auto lanesBefore = te::getAudioTracks (*edit).size();
            um.beginNewTransaction ("Remove lane");
            edit->deleteTrack (te::getAudioTracks (*edit)[0]);
            auto lanesAfter = te::getAudioTracks (*edit).size();
            um.undo();
            clip = dynamic_cast<te::WaveAudioClip*> (te::findClipForID (*edit, clip->itemID));
            check (lanesAfter == lanesBefore - 1 && te::getAudioTracks (*edit).size() == lanesBefore && clip != nullptr,
                   "remove lane, then undo -> lane and its clip are back");
        }

        // Projects: save, reopen, moved folder, moved song + relink.
        {
            namespace tempo = mixtopia::tempo;
            namespace project = mixtopia::project;
            auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mixtopia-selftest");
            root.deleteRecursively();
            auto dirA = root.getChildFile ("A"), dirB = root.getChildFile ("B"), dirC = root.getChildFile ("C");
            dirA.createDirectory();
            dirB.createDirectory();
            auto songA = dirA.getChildFile ("songA" + audioFile.getFileExtension());
            auto songB = dirA.getChildFile ("songB" + audioFile.getFileExtension());
            audioFile.copyFileTo (songA);
            audioFile.copyFileTo (songB);

            auto e1 = mixtopia::createEdit (engine);
            tempo::setProjectBpm (*e1, 128.0);
            auto c1 = mixtopia::insertAudioFile (*te::getAudioTracks (*e1)[0], songA, te::TimePosition::fromSeconds (2.0));
            auto c2 = mixtopia::insertAudioFile (*te::getAudioTracks (*e1)[1], songB, te::TimePosition::fromSeconds (5.0));
            tempo::setBpmAndDownbeat (*c1, 120.0, 0.25, tempo::Snap::beats);
            c2->setPosition ({ { te::TimePosition::fromSeconds (5.5), te::TimePosition::fromSeconds (8.0) }, te::TimeDuration::fromSeconds (0.5) });
            te::getAudioTracks (*e1)[1]->setMute (true);
            tempo::setStretchMode (*e1, te::TimeStretcher::rubberbandMelodic);
            e1->clickTrackEnabled = true;
            e1->clickTrackGain = 0.5f;

            namespace env = mixtopia::envelopes;
            std::vector<env::Point> volumeLine { { 1.0, 0.0f }, { 3.0, -12.0f }, { 5.0, env::killDb } };
            std::vector<env::Point> bassLine { { 2.0, -6.0f } };
            std::vector<env::Point> filterLine { { 1.5, 0.0f }, { 2.5, -80.0f }, { 4.0, 60.0f } };   // (filter: a position, not dB)
            env::setPoints (c1->state, env::Param::volume, volumeLine, &e1->getUndoManager());
            env::setPoints (c1->state, env::Param::filter, filterLine, &e1->getUndoManager());
            env::setPoints (c2->state, env::Param::bass, bassLine, &e1->getUndoManager());

            auto samePoints = [] (const std::vector<env::Point>& a, const std::vector<env::Point>& b)
            {
                if (a.size() != b.size())
                    return false;

                for (size_t i = 0; i < a.size(); ++i)
                    if (std::abs (a[i].sourceTime - b[i].sourceTime) > 1.0e-9 || a[i].db != b[i].db)
                        return false;

                return true;
            };

            auto countMixers = [] (te::Clip& clip)
            {
                int n = 0;

                for (auto* plugin : *clip.getPluginList())
                    n += dynamic_cast<ClipMixerPlugin*> (plugin) != nullptr ? 1 : 0;

                return n;
            };

            check (countMixers (*c1) == 1 && countMixers (*c2) == 1, "every new clip has one clip mixer");

            // Undo of a line edit.
            e1->getUndoManager().beginNewTransaction ("Edit line");
            env::setPoints (c1->state, env::Param::volume, { { 2.0, -3.0f } }, &e1->getUndoManager());
            e1->getUndoManager().undo();
            auto afterUndo = env::getPoints (c1->state, env::Param::volume);
            check (afterUndo.size() == 3 && afterUndo[1].db == -12.0f, "undo a line edit -> previous points back");

            // Moving a clip to another lane keeps its lines and its (working) clip mixer; undo too.
            {
                auto mixerFollows = [&] (te::Clip& clip)
                {
                    auto mixers = clip.getPluginList()->getPluginsOfType<ClipMixerPlugin>();
                    return mixers.size() == 1 && mixers[0]->getOwnerClip() == &clip;
                };

                auto id = c1->itemID;
                e1->getUndoManager().beginNewTransaction ("Move clip");
                c1->moveTo (*te::getAudioTracks (*e1)[2]);
                auto* moved = te::findClipForID (*e1, id);
                check (moved != nullptr && moved->getTrack() == te::getAudioTracks (*e1)[2]
                         && samePoints (env::getPoints (moved->state, env::Param::volume), volumeLine) && mixerFollows (*moved),
                       "move a clip to another lane -> its lines and clip mixer go with it");

                e1->getUndoManager().undo();
                c1 = dynamic_cast<te::WaveAudioClip*> (te::findClipForID (*e1, id));
                check (c1 != nullptr && c1->getTrack() == te::getAudioTracks (*e1)[0]
                         && samePoints (env::getPoints (c1->state, env::Param::volume), volumeLine) && mixerFollows (*c1),
                       "undo the lane move -> back with its lines");
            }

            auto projectFile = dirA.getChildFile ("mix.mixtopia");
            check (project::save (*e1, projectFile).isEmpty(), "save project");

            auto contents = project::read (projectFile);
            auto e2 = contents.error.isEmpty() ? project::createEdit (engine, contents) : nullptr;
            check (e2 != nullptr && contents.missingFiles.isEmpty(), "reopen project");

            if (e2 != nullptr)
            {
                auto clipsOf = [] (te::Edit& e, int lane) { return te::getAudioTracks (e)[lane]->getClips(); };
                auto a1 = dynamic_cast<te::WaveAudioClip*> (clipsOf (*e1, 0)[0]);
                auto a2 = dynamic_cast<te::WaveAudioClip*> (clipsOf (*e2, 0)[0]);
                auto b1 = clipsOf (*e1, 1)[0];
                auto b2 = clipsOf (*e2, 1)[0];
                check (std::abs (tempo::getProjectBpm (*e2) - 128.0) < 1.0e-9, "reopened: project tempo");
                check (a2 != nullptr && a2->getPosition() == a1->getPosition() && b2->getPosition() == b1->getPosition(),
                       "reopened: clip lanes, positions, trims");
                check (a2 != nullptr && tempo::getClipBpm (*a2) == 120.0 && tempo::getDownbeat (*a2) == 0.25 && a2->getAutoTempo(),
                       "reopened: song BPM and beat 1");
                check (te::getAudioTracks (*e2)[1]->isMuted (false) && ! te::getAudioTracks (*e2)[0]->isMuted (false), "reopened: mute");
                check (tempo::getStretchMode (*e2) == te::TimeStretcher::rubberbandMelodic, "reopened: stretch mode");
                check (e2->clickTrackEnabled.get() && std::abs (e2->clickTrackGain.get() - 0.5f) < 1.0e-6, "reopened: metronome on and volume");
                check (a2 != nullptr && mixtopia::getSourceFile (*a2) == songA && a2->getPlaybackFile().isValid(), "reopened: song plays");

                auto countLimiters = [] (te::Edit& e)
                {
                    int n = 0;

                    for (auto* plugin : e.getMasterPluginList())
                        n += dynamic_cast<MasterLimiterPlugin*> (plugin) != nullptr ? 1 : 0;

                    return n;
                };

                check (countLimiters (*e1) == 1 && countLimiters (*e2) == 1, "master limiter: one in a new and in a reopened project");

                check (samePoints (env::getPoints (a2->state, env::Param::volume), volumeLine)
                         && samePoints (env::getPoints (b2->state, env::Param::bass), bassLine)
                         && env::getPoints (a2->state, env::Param::treble).empty(),
                       "reopened: volume and EQ lines");
                check (samePoints (env::getPoints (a2->state, env::Param::filter), filterLine), "reopened: the filter line (low-pass -80, high-pass +60)");
                check (countMixers (*a2) == 1 && countMixers (*b2) == 1, "reopened: one clip mixer per clip");
            }

            // A project saved before the limiter existed gets one when opened.
            {
                auto old = projectFile.getSiblingFile ("old.mixtopia");
                auto xml = juce::parseXML (projectFile);

                if (auto* master = xml != nullptr ? xml->getChildByName ("MASTERPLUGINS") : nullptr)
                    master->deleteAllChildElements();

                // ... and from before clip mixers: strip the clips' plugins too.
                std::function<void (juce::XmlElement&)> stripClipPlugins = [&] (juce::XmlElement& e)
                {
                    if (e.hasTagName ("AUDIOCLIP"))
                        e.deleteAllChildElementsWithTagName ("PLUGIN");

                    for (auto* child : e.getChildIterator())
                        stripClipPlugins (*child);
                };

                stripClipPlugins (*xml);

                xml->writeTo (old);
                auto oldContents = project::read (old);
                auto e4 = project::createEdit (engine, oldContents);
                auto has = e4 != nullptr && ! e4->getMasterPluginList().getPluginsOfType<MasterLimiterPlugin>().isEmpty();
                check (has, "project saved without a limiter gets one when opened");

                auto clipsHaveMixers = e4 != nullptr;

                for (auto* track : e4 != nullptr ? te::getAudioTracks (*e4) : juce::Array<te::AudioTrack*>())
                    for (auto* clip : track->getClips())
                        clipsHaveMixers = clipsHaveMixers && countMixers (*clip) == 1;

                check (clipsHaveMixers, "project saved without clip mixers gets them when opened");
                mixtopia::destroyEdit (e4);
            }

            // Autosave (user's option 1, 2026-10-27): a saved project is autosaved into its own file,
            // the last manual save stays apart (made first for a project saved by hand before this
            // existed), undo still works; with autosave off only the backup copy is written.
            {
                auto autosaved = dirA.getChildFile ("autosave.mixtopia");
                auto manual = project::getLastManualSaveFile (autosaved);
                auto tempoIn = [&] (const juce::File& f)
                {
                    auto c = project::read (f);
                    auto e = c.error.isEmpty() ? project::createEdit (engine, c) : nullptr;
                    auto bpm = e != nullptr ? tempo::getProjectBpm (*e) : 0.0;
                    mixtopia::destroyEdit (e);
                    return bpm;
                };

                check (project::save (*e1, autosaved).isEmpty() && ! manual.existsAsFile(), "autosave: a project saved before the last-manual-save copy existed");
                e1->getUndoManager().beginNewTransaction ("Tempo for the autosave test");
                e1->tempoSequence.getTempo (0)->setBpm (133.0);
                check (project::autosave (*e1, autosaved, true).isEmpty() && std::abs (tempoIn (autosaved) - 133.0) < 1.0e-9
                         && std::abs (tempoIn (manual) - 128.0) < 1.0e-9,
                       "autosave writes into the project itself (133 BPM); the manual save before it is kept as '(last manual save)' (128 BPM)");

                e1->getUndoManager().beginNewTransaction ("Tempo again");
                e1->tempoSequence.getTempo (0)->setBpm (136.0);
                check (project::autosave (*e1, autosaved, true).isEmpty() && std::abs (tempoIn (autosaved) - 136.0) < 1.0e-9
                         && std::abs (tempoIn (manual) - 128.0) < 1.0e-9 && e1->getUndoManager().canUndo(),
                       "a second autosave never touches the last manual save, and undo is still there");

                e1->getUndoManager().undo();
                e1->getUndoManager().undo();
                check (std::abs (tempo::getProjectBpm (*e1) - 128.0) < 1.0e-9, "undo after two autosaves -> back to 128 BPM");

                project::keepManualSave (autosaved);   // as a manual save does: the copy follows it
                check (std::abs (tempoIn (manual) - 136.0) < 1.0e-9, "a manual save replaces the last-manual-save copy");

                e1->getUndoManager().beginNewTransaction ("Tempo, autosave off");
                e1->tempoSequence.getTempo (0)->setBpm (131.0);
                check (project::autosave (*e1, autosaved, false).isEmpty() && std::abs (tempoIn (autosaved) - 136.0) < 1.0e-9
                         && std::abs (tempoIn (project::getBackupFile (autosaved)) - 131.0) < 1.0e-9,
                       "autosave off: only the backup copy is written (131 BPM), the project stays as saved");

                e1->getUndoManager().undo();
                autosaved.deleteFile();
                manual.deleteFile();
                project::getBackupFile (autosaved).deleteFile();
            }

            mixtopia::destroyEdit (e2);
            c1 = nullptr;   // clips must not outlive their Edit
            c2 = nullptr;
            mixtopia::destroyEdit (e1);

            // The whole folder moved (project + songs together): found relative to the project.
            dirA.moveFileTo (dirC);
            contents = project::read (dirC.getChildFile ("mix.mixtopia"));
            check (contents.missingFiles.isEmpty(), "project folder moved with its songs -> songs found");

            // One song moved elsewhere: reported missing, then found by locating its folder.
            dirC.getChildFile (songA.getFileName()).moveFileTo (dirB.getChildFile (songA.getFileName()));
            contents = project::read (dirC.getChildFile ("mix.mixtopia"));
            check (contents.missingFiles.size() == 1, "song moved away -> reported missing");
            check (project::relinkFromFolder (contents, dirB) == 1 && contents.missingFiles.isEmpty(), "locate its folder -> relinked");
            auto e3 = project::createEdit (engine, contents);
            auto relinked = e3 != nullptr ? dynamic_cast<te::WaveAudioClip*> (te::getAudioTracks (*e3)[0]->getClips()[0]) : nullptr;
            check (relinked != nullptr && mixtopia::getSourceFile (*relinked) == dirB.getChildFile (songA.getFileName())
                     && relinked->getPlaybackFile().isValid(), "relinked song plays");
            mixtopia::destroyEdit (e3);
            root.deleteRecursively();
        }

        // Beat grid: a synthetic track (click + pitch-drop kick on every beat, a bass note that
        // changes on every bar's beat 1) gets its BPM, beat lines and beat 1 back.
        for (auto bpm : { 140.0, 175.0 })
        {
            constexpr double sampleRate = 44100.0, firstKick = 0.3;
            auto beat = 60.0 / bpm;
            juce::AudioBuffer<float> song (1, (int) (60.0 * sampleRate));
            song.clear();
            juce::Random random (7);
            auto firstBarBeat = 1;   // the bar starts on the second kick

            for (int k = 0; firstKick + k * beat < 59.0; ++k)
            {
                auto start = (int) ((firstKick + k * beat) * sampleRate);
                double phase = 0.0;

                for (int i = 0; i < (int) (beat * sampleRate) && start + i < song.getNumSamples(); ++i)
                {
                    auto t = i / sampleRate;
                    phase += juce::MathConstants<double>::twoPi * (50.0 + 150.0 * std::exp (-t / 0.02)) / sampleRate;
                    auto kickSample = 0.7 * std::sin (phase) * std::exp (-t / 0.12)
                                        + (t < 0.002 ? 0.6 * (random.nextFloat() * 2.0f - 1.0f) * std::exp (-t / 0.0005) : 0.0);
                    song.addSample (0, start + i, (float) kickSample);
                }

                // Bass + pad: a new note on every bar start, held for the bar.
                if ((k - firstBarBeat) % 4 == 0 && k >= firstBarBeat)
                {
                    auto bar = (k - firstBarBeat) / 4;
                    auto frequency = std::array<double, 4> { 55.0, 73.4, 61.7, 82.4 }[(size_t) (bar % 4)];

                    for (int i = 0; i < (int) (4 * beat * sampleRate) && start + i < song.getNumSamples(); ++i)
                    {
                        auto t = i / sampleRate;
                        song.addSample (0, start + i, (float) (0.15 * std::sin (juce::MathConstants<double>::twoPi * frequency * t)
                                                               + 0.05 * std::sin (juce::MathConstants<double>::twoPi * frequency * 8 * t)));
                    }
                }
            }

            auto file = juce::File::createTempFile (".wav");
            {
                std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                auto writer = juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                                               .withSampleRate (sampleRate)
                                                                               .withNumChannels (1)
                                                                               .withBitsPerSample (32)
                                                                               .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                writer->writeFromAudioSampleBuffer (song, 0, song.getNumSamples());
            }

            std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
            auto grid = mixtopia::beatgrid::analyse (*reader);
            auto offBy = [] (double a, double b, double period) { auto d = std::fmod (a - b, period); d = d < 0 ? d + period : d; return std::min (d, period - d) * 1000.0; };
            auto beatMs = offBy (grid.firstBeat, firstKick, beat);
            auto barMs = offBy (grid.downbeat, firstKick + firstBarBeat * beat, 4 * beat);
            check (grid.valid && grid.bpm == bpm && beatMs < 3.0 && barMs < 3.0,
                   "beat grid of a " + juce::String (bpm, 0) + " BPM track: found " + juce::String (grid.bpm, 3) + " BPM, beat lines "
                     + juce::String (beatMs, 1) + " ms and beat 1 " + juce::String (barMs, 1) + " ms from the real ones");
            reader = nullptr;
            file.deleteFile();
        }

        // Loudness (EBU R128): the standard's test tones, gating, and level matching in a render.
        {
            namespace loudness = mixtopia::loudness;
            constexpr double sampleRate = 48000.0;

            // `parts`: (seconds, sine amplitude in dBFS, or below -200 for silence) one after another.
            auto writeTone = [&] (int channels, const std::vector<std::pair<double, double>>& parts)
            {
                auto total = 0.0;

                for (auto& p : parts)
                    total += p.first;

                juce::AudioBuffer<float> song (channels, (int) (total * sampleRate));
                song.clear();
                int pos = 0;

                for (auto& [seconds, db] : parts)
                {
                    auto n = (int) (seconds * sampleRate);

                    if (db > -200.0)
                        for (int i = 0; i < n; ++i)
                            for (int ch = 0; ch < channels; ++ch)
                                song.setSample (ch, pos + i, (float) (juce::Decibels::decibelsToGain (db)
                                                                       * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (pos + i) / sampleRate)));
                    pos += n;
                }

                auto file = juce::File::createTempFile (".wav");
                std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                auto writer = juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                                               .withSampleRate (sampleRate)
                                                                               .withNumChannels (channels)
                                                                               .withBitsPerSample (32)
                                                                               .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                writer->writeFromAudioSampleBuffer (song, 0, song.getNumSamples());
                writer = nullptr;
                return file;
            };

            auto measureFile = [&] (const juce::File& file)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
                return loudness::measure (*reader);
            };

            auto measureTone = [&] (int channels, const std::vector<std::pair<double, double>>& parts)
            {
                auto file = writeTone (channels, parts);
                auto lufs = measureFile (file);
                file.deleteFile();
                return lufs;
            };

            // EBU Tech 3341 cases 1 and 2: stereo 1 kHz sine at -23 / -33 dBFS = -23 / -33 LUFS.
            auto at23 = measureTone (2, { { 20.0, -23.0 } }), at33 = measureTone (2, { { 20.0, -33.0 } });
            auto mono = measureTone (1, { { 20.0, -23.0 } });
            check (std::abs (at23 + 23.0) < 0.1 && std::abs (at33 + 33.0) < 0.1 && std::abs (mono + 23.0) < 0.1,
                   "loudness of the standard test tones: " + juce::String (at23, 2) + " / " + juce::String (at33, 2)
                     + " LUFS (want -23 / -33), mono " + juce::String (mono, 2) + " (want -23)");

            // Gating: silence and parts more than 10 LU quieter don't count.
            auto withSilence = measureTone (2, { { 10.0, -23.0 }, { 10.0, -300.0 } });
            auto withQuietPart = measureTone (2, { { 10.0, -20.0 }, { 10.0, -40.0 } });
            check (std::abs (withSilence + 23.0) < 0.1 && std::abs (withQuietPart + 20.0) < 0.1,
                   "loudness gating: with silence " + juce::String (withSilence, 2) + " (want -23), with a quiet part "
                     + juce::String (withQuietPart, 2) + " (want -20)");

            check (loudness::matchGainDb (loudness::referenceLufs - 3.0) == 3.0 && loudness::matchGainDb (loudness::referenceLufs + 2.0) == 0.0
                     && loudness::matchGainDb (-40.0) == loudness::maxRaiseDb && loudness::matchGainDb (-std::numeric_limits<double>::infinity()) == 0.0,
                   "level matching raises quieter songs only (3 dB under -> +3 dB; louder -> 0; at most +9; silence -> 0)");

            // In a render: a song 3 dB under the reference comes out 3 dB louder with matching on, and
            // bit-identical to before with it switched off (project or song).
            auto toneFile = writeTone (2, { { 12.0, -20.0 } });
            auto renderWith = [&] (bool projectOn, bool clipOn, bool measured)
            {
                auto e = mixtopia::createEdit (engine);
                e->state.setProperty (mixtopia::levelmatch::projectMatchProperty, projectOn ? 1 : 0, nullptr);
                auto c = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], toneFile, te::TimePosition::fromSeconds (2.0));

                if (measured)
                    c->state.setProperty (mixtopia::levelmatch::loudnessProperty, loudness::referenceLufs - 3.0, nullptr);

                if (! clipOn)
                    c->state.setProperty (mixtopia::levelmatch::matchOffProperty, true, nullptr);

                auto out = juce::File::createTempFile (".wav");
                te::Renderer::renderToFile ({}, out, *e, { te::TimePosition(), te::TimePosition::fromSeconds (14.0) },
                                            mixtopia::getAudibleTracksForRender (*e), true, true, {}, false);
                c = nullptr;
                mixtopia::destroyEdit (e);

                std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (out), true));
                juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
                reader = nullptr;
                out.deleteFile();
                return audio;
            };

            auto rmsDb = [] (const juce::AudioBuffer<float>& a)   // over 4..12 s (the tone, away from its edges)
            {
                auto start = (int) (4.0 * 44100.0), n = (int) (8.0 * 44100.0);
                return juce::Decibels::gainToDecibels (a.getRMSLevel (0, start, juce::jmin (n, a.getNumSamples() - start)));
            };

            auto plain = renderWith (true, true, false);          // not measured: no gain
            auto matched = renderWith (true, true, true);
            auto projectOff = renderWith (false, true, true);
            auto songOff = renderWith (true, false, true);
            auto identical = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
            {
                if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels())
                    return false;

                for (int ch = 0; ch < a.getNumChannels(); ++ch)
                    if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                        return false;

                return true;
            };

            auto raise = rmsDb (matched) - rmsDb (plain);
            check (std::abs (raise - 3.0) < 0.01, "matched song renders " + juce::String (raise, 3) + " dB louder (want +3.000)");
            check (identical (plain, projectOff) && identical (plain, songOff),
                   "matching switched off (project or song) -> render bit-identical to an unmatched song");
            toneFile.deleteFile();

            // The master panel's live loudness: the limiter measures what it outputs in 100 ms steps.
            // A -23 dBFS stereo tone fed through it in playback-sized blocks reads -23 LUFS (short-term
            // and since Play), like the file measurement; exports don't feed the live readings.
            {
                auto e = mixtopia::createEdit (engine);
                auto* limiter = MasterLimiterPlugin::ensureOnMaster (*e);
                limiter->baseClassInitialise ({ te::TimePosition(), 44100.0, 512 });

                loudness::Meter meter, inputMeter;
                std::array<double, 64> steps;
                juce::AudioBuffer<float> block (2, 512);

                // What comes out, measured here (the limiter only measures what comes in).
                loudness::KWeighting outK[2];
                outK[0].prepare (44100.0);
                outK[1].prepare (44100.0);
                double outSum = 0.0;
                int outCount = 0;
                auto measureOutput = [&]
                {
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        auto l = outK[0].process (block.getSample (0, i)), r = outK[1].process (block.getSample (1, i));
                        outSum += l * l + r * r;

                        if (++outCount == 4410)
                        {
                            meter.addStep (outSum / 4410.0);
                            outSum = 0.0;
                            outCount = 0;
                        }
                    }
                };
                long long sample = 0;

                auto feed = [&] (double seconds, bool rendering)
                {
                    for (int b = 0; b < (int) (seconds * 44100.0 / 512); ++b)
                    {
                        for (int i = 0; i < 512; ++i, ++sample)
                            for (int ch = 0; ch < 2; ++ch)
                                block.setSample (ch, i, (float) (juce::Decibels::decibelsToGain (-23.0)
                                                                   * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) sample / 44100.0)));

                        te::PluginRenderContext fc (&block, 0, 512, nullptr, 0.0, {}, true, false, rendering, false);
                        limiter->applyToBuffer (fc);

                        measureOutput();

                        while (auto num = limiter->takeLoudnessSteps (steps.data(), (int) steps.size()))
                            for (int i = 0; i < num; ++i)
                                inputMeter.addStep (steps[(size_t) i]);
                    }
                };

                feed (10.0, false);
                auto shortTerm = meter.getShortTerm(), integrated = meter.getIntegrated(), input = inputMeter.getShortTerm();
                inputMeter.reset();
                feed (2.0, true);
                auto duringExport = inputMeter.getIntegrated();   // (the limiter doesn't measure exports)
                check (std::abs (shortTerm + 23.0) < 0.1 && std::abs (integrated + 23.0) < 0.1 && std::abs (input + 23.0) < 0.1
                         && ! std::isfinite (duringExport),
                       "master loudness meter: -23 dBFS tone reads " + juce::String (shortTerm, 2) + " (last 3 s) / "
                         + juce::String (integrated, 2) + " (since Play) / " + juce::String (input, 2)
                         + " (before the limiter) LUFS, want -23; exports don't count");

                // Too loud for the limiter (a -3 dBFS tone, about -3 LUFS): before the limiter it reads
                // the real level, after it at most the ceiling allows.
                {
                    auto loudTone = [&] (double db, double seconds)
                    {
                        meter.reset();
                        inputMeter.reset();

                        for (int b = 0; b < (int) (seconds * 44100.0 / 512); ++b)
                        {
                            for (int i = 0; i < 512; ++i, ++sample)
                                for (int ch = 0; ch < 2; ++ch)
                                    block.setSample (ch, i, (float) (juce::Decibels::decibelsToGain (db)
                                                                       * 2.0 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) sample / 44100.0)));

                            te::PluginRenderContext fc (&block, 0, 512, nullptr, 0.0, {}, true, false, false, false);
                            limiter->applyToBuffer (fc);

                            measureOutput();

                            while (auto num = limiter->takeLoudnessSteps (steps.data(), (int) steps.size()))
                                for (int i = 0; i < num; ++i)
                                    inputMeter.addStep (steps[(size_t) i]);
                        }
                    };

                    loudTone (-3.0, 5.0);   // peaks at +3 dBFS
                    auto before = inputMeter.getShortTerm(), after = meter.getShortTerm();
                    check (before > after + 3.0,
                           "master loudness meter: a mix too loud for the limiter reads " + juce::String (before, 2) + " LUFS before the limiter, "
                             + juce::String (after, 2) + " after (the Mix bar uses 'before', so bumps show)");
                }

                limiter->baseClassDeinitialise();
                mixtopia::destroyEdit (e);
            }

            // The one limiter knob: old three-knob settings are put on its scheme; push sets drive + release.
            {
                auto e = mixtopia::createEdit (engine);
                auto* l = MasterLimiterPlugin::ensureOnMaster (*e);
                l->ceilingDb = 0.0f;
                l->driveDb = 2.0f;
                l->releaseMs = 300.0f;
                l->applyOneKnobScheme();
                auto converted = l->ceilingDb.get() == -1.0f && l->driveDb.get() == 2.0f && l->releaseMs.get() == 80.0f;
                l->setPush (20.0f);
                auto clamped = l->driveDb.get() == 9.0f && l->releaseMs.get() == 150.0f;
                l->setPush (0.0f);
                auto zero = l->driveDb.get() == 0.0f && l->releaseMs.get() == 60.0f && l->ceilingDb.get() == -1.0f;
                check (converted && clamped && zero,
                       "limiter knob: old settings (ceiling 0, drive 2, release 300) -> ceiling -1, push 2, release 80 ms; "
                       "push 20 -> 9 dB / 150 ms; push 0 -> 60 ms");
                mixtopia::destroyEdit (e);
            }

            // Per-song meter: a -23 dBFS tone raised +3 dB by level matching and lowered by a -6 dB
            // volume line reads -26 (what it adds to the mix).
            {
                auto e = mixtopia::createEdit (engine);
                auto file = writeTone (2, { { 3.0, -23.0 } });
                auto c = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], file, te::TimePosition::fromSeconds (2.0));
                c->state.setProperty (mixtopia::levelmatch::loudnessProperty, loudness::referenceLufs - 3.0, nullptr);
                mixtopia::envelopes::setPoints (c->state, mixtopia::envelopes::Param::volume, { { 0.0, -6.0f }, { 3.0, -6.0f } }, nullptr);

                auto* mixer = ClipMixerPlugin::ensureOnClip (*c);
                mixer->baseClassInitialise ({ te::TimePosition(), 44100.0, 512 });
                mixer->updateSnapshot();

                loudness::Meter after;
                std::array<double, 64> steps;
                juce::AudioBuffer<float> block (2, 512);
                long long sample = 0;

                auto feed = [&] (double seconds, bool rendering)
                {
                    for (int b = 0; b < (int) (seconds * 44100.0 / 512); ++b)
                    {
                        for (int i = 0; i < 512; ++i, ++sample)
                            for (int ch = 0; ch < 2; ++ch)
                                block.setSample (ch, i, (float) (juce::Decibels::decibelsToGain (-23.0)
                                                                   * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) sample / 44100.0)));

                        te::PluginRenderContext fc (&block, 0, 512, nullptr, 0.0, { te::TimePosition::fromSeconds (3.0), te::TimeDuration::fromSeconds (0.01) },
                                                    true, false, rendering, false);
                        mixer->applyToBuffer (fc);

                        while (auto num = mixer->takeLoudnessSteps (steps.data(), (int) steps.size()))
                            for (int i = 0; i < num; ++i)
                                after.addStep (steps[(size_t) i]);
                    }
                };

                feed (10.0, false);
                auto afterLufs = after.getShortTerm();
                after.reset();
                feed (2.0, true);
                check (std::abs (afterLufs + 26.0) < 0.1 && ! std::isfinite (after.getShortTerm()),
                       "per-song meter: matched (+3) tone with a -6 dB volume line reads " + juce::String (afterLufs, 2)
                         + " LUFS (want -26); exports don't count");

                mixer->baseClassDeinitialise();
                c = nullptr;
                mixtopia::destroyEdit (e);
                file.deleteFile();
            }
        }

        // Envelope points after a trim: pushed to the new edge, level kept, nearest one kept.
        {
            namespace env = mixtopia::envelopes;
            std::vector<env::Point> points { { 1.0, -6.0f }, { 2.0, -3.0f }, { 5.0, 0.0f }, { 9.0, -12.0f }, { 9.5, -20.0f } };
            auto pushed = env::pushedInside (points, 3.0, 8.0);
            auto untouched = env::pushedInside (points, 0.0, 10.0);
            check (pushed.size() == 3 && pushed[0].sourceTime == 3.0 && pushed[0].db == -3.0f && pushed[1].sourceTime == 5.0
                     && pushed[2].sourceTime == 8.0 && pushed[2].db == -12.0f && untouched.size() == points.size(),
                   "envelope points in a trimmed-off part are pushed to the new edge (level kept, nearest kept)");
        }

        // Song library (M7): tags from file names, adding (files, folders, no duplicates), background
        // analysis, collections, remembered corrections, saving and reloading, missing files.
        {
            namespace lib = mixtopia::library;
            namespace tags = mixtopia::tags;
            auto name1 = tags::fromFileName (juce::String (juce::CharPointer_UTF8 ("02. N\xc3\x98RTH SHORE - Bright Lights (Example Extended Remix)")));
            auto name2 = tags::fromFileName ("01 Distant Shores (Original Mix)");
            auto name3 = tags::fromFileName ("DJ Example - Echo  - Echo (Extended Mix)");
            check (name1.artist == juce::String (juce::CharPointer_UTF8 ("N\xc3\x98RTH SHORE")) && name1.title == "Bright Lights (Example Extended Remix)"
                     && name2.artist.isEmpty() && name2.title == "Distant Shores (Original Mix)"
                     && name3.artist == "DJ Example" && name3.title == "Echo  - Echo (Extended Mix)",
                   "library: artist and title from file names (track numbers dropped)");

            auto dir = juce::File::createTempFile ("").getSiblingFile ("mixtopia-library-test");
            dir.deleteRecursively();
            auto songs = dir.getChildFile ("Set one");
            songs.createDirectory();
            auto a = songs.getChildFile ("Artist A - Song A.mp3"), b = songs.getChildFile ("Artist B - Song B.mp3");
            audioFile.copyFileTo (a);
            audioFile.copyFileTo (b);
            auto storage = dir.getChildFile ("library.xml");

            {
                lib::Library library (engine, storage);
                auto added = library.add ({ a });
                auto again = library.add ({ a });
                auto folder = library.add ({ songs });
                auto analysed = library.waitUntilAnalysed (60000);
                auto* sa = library.find (a.getFullPathName());
                check (added == 1 && again == 0 && folder == 1 && library.getSongs().size() == 2 && analysed
                         && sa != nullptr && sa->lengthSeconds > 1.0 && ! sa->title.isEmpty() && ! sa->analysing && ! sa->missing,
                       "library: add a file, the same again (skipped), its folder (one new); analysed in the background (length "
                         + juce::String (sa != nullptr ? sa->lengthSeconds : 0.0, 1) + " s, title \"" + (sa != nullptr ? sa->title : juce::String()) + "\")");

                check (library.createCollection ("Sunset set") && ! library.createCollection ("Sunset set") && library.createCollection ("Warm-up")
                         && library.renameCollection ("Warm-up", "Warm up") && ! library.renameCollection ("Warm up", "Sunset set"),
                       "library: collections created, renamed; duplicate names refused");
                library.addToCollection ("Sunset set", { b.getFullPathName(), a.getFullPathName(), b.getFullPathName() });
                auto* set = library.findCollection ("Sunset set");
                auto addedInOrder = set->paths == juce::StringArray { b.getFullPathName(), a.getFullPathName() };
                library.moveInCollection ("Sunset set", { a.getFullPathName() }, 0);
                check (addedInOrder && set->paths == juce::StringArray { a.getFullPathName(), b.getFullPathName() },
                       "library: songs added to a collection in order (no duplicates) and moved to change the set order");

                library.rememberGrid (a.getFullPathName(), 128.5, 0.75);
                library.rememberKey (a.getFullPathName(), "4A");
                mixtopia::analysis::SongInfo info;
                info.grid.valid = true;
                info.grid.bpm = 140.0;
                info.key = "8A";
                library.applyCorrections (a.getFullPathName(), info);
                check (info.grid.bpm == 128.5 && info.grid.downbeat == 0.75 && info.key == "4A",
                       "library: corrections from the timeline are remembered and used when the song is placed again");
                library.saveNow();
            }

            {
                lib::Library reloaded (engine, storage);
                auto* sa = reloaded.find (a.getFullPathName());
                auto* set = reloaded.findCollection ("Sunset set");
                check (reloaded.getSongs().size() == 2 && sa != nullptr && sa->gridByHand && sa->bpm == 128.5 && sa->key == "4A"
                         && set != nullptr && set->paths[0] == a.getFullPathName() && reloaded.findCollection ("Warm up") != nullptr,
                       "library: saved and reloaded (songs, corrections, collections in set order)");

                auto detectedBpm = [&] { reloaded.waitUntilAnalysed (60000); return reloaded.find (a.getFullPathName())->bpm; };
                reloaded.forgetGrid (a.getFullPathName());
                auto bpmAfterForget = detectedBpm();
                check (! reloaded.find (a.getFullPathName())->gridByHand && bpmAfterForget != 128.5,
                       "library: \"Use the detected BPM\" forgets the correction (back to " + juce::String (bpmAfterForget, 2) + ")");

                auto moved = dir.getChildFile ("Moved");
                moved.createDirectory();
                a.moveFileTo (moved.getChildFile (a.getFileName()));
                b.moveFileTo (moved.getChildFile (b.getFileName()));
                reloaded.refreshMissing();
                auto bothMissing = reloaded.find (a.getFullPathName())->missing && reloaded.find (b.getFullPathName())->missing;
                auto found = reloaded.relink (a.getFullPathName(), moved.getChildFile (a.getFileName()));
                auto newA = moved.getChildFile (a.getFileName()).getFullPathName();
                check (bothMissing && found == 2 && reloaded.find (newA) != nullptr && ! reloaded.find (newA)->missing
                         && reloaded.findCollection ("Sunset set")->paths[0] == newA,
                       "library: moved songs show as missing; locating one finds the other in the same folder; collections follow");

                reloaded.remove ({ newA });
                check (reloaded.getSongs().size() == 1 && reloaded.findCollection ("Sunset set")->paths.size() == 1,
                       "library: removing a song takes it out of its collections too");
            }

            // Speed: a 1,000-song library (written straight to a file; the songs needn't exist) loads,
            // searches and sorts at once.
            {
                juce::XmlElement xml ("MIXTOPIA_LIBRARY");
                juce::Random random (7);

                for (int i = 0; i < 1000; ++i)
                {
                    auto* e = xml.createNewChildElement ("SONG");
                    e->setAttribute ("path", dir.getChildFile ("big/song" + juce::String (i) + ".mp3").getFullPathName());
                    e->setAttribute ("title", "Title " + juce::String (random.nextInt (100000)) + " (Extended Mix)");
                    e->setAttribute ("artist", "Artist " + juce::String (random.nextInt (500)));
                    e->setAttribute ("length", 400.0);
                    e->setAttribute ("bpm", 130 + random.nextInt (20));
                    e->setAttribute ("key", juce::String (1 + random.nextInt (12)) + (random.nextBool() ? "A" : "B"));
                }

                dir.createDirectory();
                xml.writeTo (dir.getChildFile ("big.xml"));
                auto t0 = juce::Time::getMillisecondCounterHiRes();
                lib::Library big (engine, dir.getChildFile ("big.xml"));
                LibraryPanel panel (big);
                panel.setSize (700, 400);
                auto t1 = juce::Time::getMillisecondCounterHiRes();
                panel.setSearchText ("artist 12");
                auto t2 = juce::Time::getMillisecondCounterHiRes();
                auto found = panel.getNumListed();
                panel.setSearchText ({});
                auto t3 = juce::Time::getMillisecondCounterHiRes();
                panel.sortBy (LibraryPanel::artistColumnId, true);
                auto t4 = juce::Time::getMillisecondCounterHiRes();
                check (big.getSongs().size() == 1000 && found > 0 && found < 1000 && panel.getNumListed() == 1000
                         && t1 - t0 < 1000.0 && t2 - t1 < 100.0 && t4 - t3 < 100.0,
                       "library: 1,000 songs load in " + juce::String (t1 - t0, 0) + " ms, search in " + juce::String (t2 - t1, 1)
                         + " ms (" + juce::String (found) + " found), sort in " + juce::String (t4 - t3, 1) + " ms");
            }

            dir.deleteRecursively();
        }

        // A library save that failed (full disk) is tried again when the library closes, so the change isn't lost at exit
        // (audit review of C11: the failed save had stopped the timer, and closing only saved while the timer ran).
        {
            auto dir = juce::File::createTempFile ("").getSiblingFile ("mixtopia-library-retry-test");
            dir.deleteRecursively();
            dir.createDirectory();
            auto storage = dir.getChildFile ("library.xml");
            auto failedOnce = false;

            {
                mixtopia::library::Library library (engine, storage);
                library.onSaveFailed = [&] (const juce::String&) { failedOnce = true; };
                library.createCollection ("Kept set");
                rlimit normal {};
                getrlimit (RLIMIT_FSIZE, &normal);
                auto oldHandler = std::signal (SIGXFSZ, SIG_IGN);
                rlimit tiny { 16, normal.rlim_max };
                setrlimit (RLIMIT_FSIZE, &tiny);
                library.saveNow();
                setrlimit (RLIMIT_FSIZE, &normal);
                std::signal (SIGXFSZ, oldHandler);
                library.onSaveFailed = nullptr;
            }

            auto reopened = juce::parseXML (storage);
            auto kept = reopened != nullptr && reopened->getChildByName ("COLLECTION") != nullptr
                          && reopened->getChildByName ("COLLECTION")->getStringAttribute ("name") == "Kept set";
            check (failedOnce && kept, juce::String ("library: a save the full disk stopped is ") + (failedOnce ? "reported" : "NOT reported")
                                         + " and tried again on closing (the change " + (kept ? "kept" : "LOST") + ")");
            dir.deleteRecursively();
        }

        // A damaged library file (cut off by a crash or a full disk) is set aside, never saved over (CLEANUP-AUDIT C4).
        {
            auto dir = juce::File::createTempFile ("").getSiblingFile ("mixtopia-damaged-library-test");
            dir.deleteRecursively();
            dir.createDirectory();
            auto storage = dir.getChildFile ("library.xml");
            const juce::String damaged = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MIXTOPIA_LIBRARY version=\"1\">\n"
                                         "  <SONG path=\"/music/One.mp3\" title=\"Song One\" bpm=\"140.0\"/>\n  <COLLECTION name=\"My set\">\n";
            storage.replaceWithText (damaged);
            juce::MemoryBlock onDisk;   // (replaceWithText writes CR LF line ends: compare with the file's own bytes)
            storage.loadFileAsData (onDisk);

            {
                mixtopia::library::Library library (engine, storage);
                library.createCollection ("New set");
                library.saveNow();
            }

            auto aside = dir.findChildFiles (juce::File::findFiles, false, "library.xml.damaged-*");
            auto reopened = juce::parseXML (storage);
            juce::MemoryBlock keptBytes;
            auto keptSame = aside.size() == 1 && aside[0].loadFileAsData (keptBytes) && keptBytes == onDisk;
            auto savedNew = reopened != nullptr && reopened->getChildByName ("COLLECTION") != nullptr;
            check (keptSame && savedNew && ! mixtopia::settings::getSetAsideFiles().isEmpty(),
                   "a damaged library file is kept as library.xml.damaged-<date> (unchanged) and the new library saved beside it ("
                     + juce::String (aside.size()) + " set aside, " + (keptSame ? "same" : "changed") + ", new library "
                     + (savedNew ? "saved" : "missing") + ", " + juce::String (mixtopia::settings::getSetAsideFiles().size()) + " listed)");

            // A good file is left alone.
            auto good = dir.getChildFile ("good.xml");
            good.replaceWithText ("<?xml version=\"1.0\"?>\n<MIXTOPIA_LIBRARY version=\"1\"/>\n");
            check (! mixtopia::settings::setAsideIfDamaged (good, "MIXTOPIA_LIBRARY") && good.existsAsFile()
                     && mixtopia::settings::setAsideIfDamaged (good, "PROPERTIES"),
                   "set-aside check: a readable file stays; one with the wrong top element is set aside");
            auto& listed = mixtopia::settings::getSetAsideFiles();   // (forget only this test's files)

            for (int i = listed.size(); --i >= 0;)
                if (juce::File (listed[i]).isAChildOf (dir))
                    listed.remove (i);

            dir.deleteRecursively();
        }

        // Keys: Camelot names, and detection of chord progressions in known keys.
        {
            namespace key = mixtopia::key;
            check (key::allCamelotKeys().size() == 24 && key::toMusicalName ("8A") == "A minor" && key::toMusicalName ("8B") == "C major"
                     && key::toMusicalName ("5A") == "C minor" && key::toMusicalName ("12B") == "E major" && key::toMusicalName ("1A") == "Ab minor",
                   "Camelot names (8A = A minor, 8B = C major, 5A = C minor, 12B = E major, 1A = Ab minor)");

            // Chords (root, third, fifth) of a few harmonics each, two seconds per chord, 40 s.
            auto keyOf = [&] (const std::vector<std::vector<int>>& chords, bool withNaN = false)   // MIDI note numbers
            {
                constexpr double sampleRate = 44100.0;
                juce::AudioBuffer<float> song (1, (int) (40.0 * sampleRate));
                song.clear();

                for (int i = 0; i < song.getNumSamples(); ++i)
                {
                    auto t = i / sampleRate;
                    auto& chord = chords[(size_t) ((int) (t / 2.0) % (int) chords.size())];
                    double v = 0.0;

                    for (auto note : chord)
                        for (int h = 1; h <= 3; ++h)
                            v += 0.08 / h * std::sin (juce::MathConstants<double>::twoPi * 440.0 * std::pow (2.0, (note - 69) / 12.0) * h * t);

                    song.setSample (0, i, (float) v);
                }

                if (withNaN)   // 10 NaN samples at 5 s (a damaged float WAV)
                    for (int i = 0; i < 10; ++i)
                        song.setSample (0, (int) (5.0 * sampleRate) + i, std::numeric_limits<float>::quiet_NaN());

                auto file = juce::File::createTempFile (".wav");
                {
                    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                    auto writer = juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                                                   .withSampleRate (sampleRate)
                                                                                   .withNumChannels (1)
                                                                                   .withBitsPerSample (32)
                                                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                    writer->writeFromAudioSampleBuffer (song, 0, song.getNumSamples());
                }

                std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
                auto found = key::analyse (*reader);
                reader = nullptr;
                file.deleteFile();
                return found;
            };

            // A minor: Am, Dm, E (with the G# leading note), Am.   E major: E, A, B, E.
            auto aMinor = keyOf ({ { 57, 60, 64 }, { 50, 53, 57 }, { 52, 56, 59 }, { 57, 60, 64 } });
            auto eMajor = keyOf ({ { 52, 56, 59 }, { 57, 61, 64 }, { 59, 63, 66 }, { 52, 56, 59 } });
            check (aMinor == "8A" && eMajor == "12B", "key of an A minor progression: " + aMinor + " (want 8A); E major: " + eMajor + " (want 12B)");

            // Audio key detection can't handle never stops it (CLEANUP-AUDIT C5: libKeyFinder threw, which ended the
            // program or the library's analysis thread): NaN samples count as silence; an empty file has no key.
            auto withNaN = keyOf ({ { 57, 60, 64 }, { 50, 53, 57 }, { 52, 56, 59 }, { 57, 60, 64 } }, true);
            auto emptyFile = juce::File::createTempFile (".wav");
            {
                std::unique_ptr<juce::OutputStream> out (emptyFile.createOutputStream());
                juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (2)
                                                                 .withBitsPerSample (16));
            }
            std::unique_ptr<juce::AudioFormatReader> emptyReader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (emptyFile), true));
            auto emptyKey = emptyReader != nullptr ? key::analyse (*emptyReader) : juce::String ("(no reader)");
            emptyReader = nullptr;
            emptyFile.deleteFile();
            check (withNaN == "8A" && emptyKey.isEmpty(), "key detection with 10 NaN samples: " + withNaN + " (want 8A); an empty WAV: '"
                                                           + emptyKey + "' (want no key, no crash)");
        }

        // Song starts: silence trimmed, kept on the grid at 0:00, a pickup moves to bar 2, a slow
        // fade-in keeps its beginning, and "Restore the start" brings the trimmed part back.
        {
            namespace tempo = mixtopia::tempo;
            namespace analysis = mixtopia::analysis;
            constexpr double sampleRate = 44100.0, bpm = 140.0, beat = 60.0 / bpm, bar = 4 * beat;

            // `leadIn` seconds before the first kick (= beat 1): silence, or a sound (pickup), or a
            // slow fade-in from far below -60 dB.
            enum class LeadIn { silence, pickup, fadeIn };

            auto makeSong = [&] (double firstKick, LeadIn leadIn)
            {
                juce::AudioBuffer<float> song (2, (int) (40.0 * sampleRate));
                song.clear();
                juce::Random random (3);

                for (int k = 0; firstKick + k * beat < 39.0; ++k)
                {
                    auto start = (int) ((firstKick + k * beat) * sampleRate);
                    double phase = 0.0;

                    for (int i = 0; i < (int) (beat * sampleRate); ++i)
                    {
                        auto t = i / sampleRate;
                        phase += juce::MathConstants<double>::twoPi * (50.0 + 150.0 * std::exp (-t / 0.02)) / sampleRate;
                        auto v = 0.7 * std::sin (phase) * std::exp (-t / 0.12)
                                   + (t < 0.002 ? 0.6 * (random.nextFloat() * 2.0f - 1.0f) * std::exp (-t / 0.0005) : 0.0);

                        // a chord change on every bar start (k = 0, 4, 8, ...)
                        v += 0.1 * std::sin (juce::MathConstants<double>::twoPi * std::array<double, 2> { 220.0, 261.6 }[(size_t) ((k / 4) % 2)] * (k % 4 * beat + t));
                        song.addSample (0, start + i, (float) v);
                        song.addSample (1, start + i, (float) v);
                    }
                }

                auto kickSample = (int) (firstKick * sampleRate);

                if (leadIn == LeadIn::pickup)        // a riser: noise in the 0.2 s before beat 1
                    for (int i = kickSample - (int) (0.2 * sampleRate); i < kickSample; ++i)
                        song.addSample (0, i, 0.1f * (random.nextFloat() * 2.0f - 1.0f));

                if (leadIn == LeadIn::fadeIn)        // noise fading in from -100 dB over 2 s before beat 1
                    for (int i = kickSample - (int) (2.0 * sampleRate); i < kickSample; ++i)
                    {
                        auto db = -100.0 + 90.0 * (i - (kickSample - 2.0 * sampleRate)) / (2.0 * sampleRate);
                        song.addSample (0, i, (float) (juce::Decibels::decibelsToGain (db) * (random.nextFloat() * 2.0f - 1.0f)));
                    }

                auto file = juce::File::createTempFile (".wav");
                std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                auto writer = juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                                               .withSampleRate (sampleRate)
                                                                               .withNumChannels (2)
                                                                               .withBitsPerSample (32)
                                                                               .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                writer->writeFromAudioSampleBuffer (song, 0, song.getNumSamples());
                writer = nullptr;
                return file;
            };

            auto analyseFile = [&] (const juce::File& file)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
                return analysis::analyseReader (*reader);
            };

            auto beatOneAt = [] (te::AudioClipBase& clip)   // edit time of the song's beat 1
            {
                return tempo::sourceToEditSeconds (clip, tempo::getDownbeat (clip));
            };

            auto e5 = mixtopia::createEdit (engine);
            e5->ensureNumberOfAudioTracks (5);   // (this test uses lanes 1-5)
            auto& lane = *te::getAudioTracks (*e5)[0];

            // 1. Silence before beat 1, dropped at 0:00: trimmed, beat 1 exactly on bar 1.
            auto silentFile = makeSong (0.5, LeadIn::silence);
            auto silentInfo = analyseFile (silentFile);
            auto c = mixtopia::insertAudioFile (lane, silentFile, te::TimePosition());
            auto moved = analysis::prepareNewClip (*c, silentInfo, te::TimePosition(), true, true);
            auto trimmed = c->getPosition().getOffset().inSeconds();
            check (! moved && c->getPosition().getStart() == te::TimePosition() && std::abs (beatOneAt (*c)) < 0.001
                     && trimmed > 0.45 && trimmed <= silentInfo.soundStart,
                   "silent start at 0:00 -> trimmed " + juce::String (trimmed, 3) + " s, beat 1 on bar 1 ("
                     + juce::String (beatOneAt (*c) * 1000.0, 1) + " ms)");

            // 2. The same song dropped mid-mix: trimmed too, beat 1 on a bar; then restored.
            auto c2 = mixtopia::insertAudioFile (*te::getAudioTracks (*e5)[1], silentFile, te::TimePosition::fromSeconds (10.0));
            analysis::prepareNewClip (*c2, silentInfo, te::TimePosition::fromSeconds (10.0), true, true);
            auto beatOne = beatOneAt (*c2);
            auto onBar = std::abs (beatOne / bar - std::round (beatOne / bar)) * bar < 0.001;
            check (onBar && c2->getPosition().getOffset().inSeconds() > 0.45, "silent start mid-mix -> trimmed, beat 1 on a bar");
            check (analysis::restoreTrimmedStart (*c2) && c2->getPosition().getOffset() == te::TimeDuration()
                     && std::abs (beatOneAt (*c2) - beatOne) < 1.0e-9,
                   "restore the start -> whole song back, beat 1 where it was");

            // 3. A pickup before beat 1, dropped at 0:00: kept, so the song moves to bar 2.
            auto pickupFile = makeSong (0.3, LeadIn::pickup);
            auto pickupInfo = analyseFile (pickupFile);
            auto c3 = mixtopia::insertAudioFile (*te::getAudioTracks (*e5)[2], pickupFile, te::TimePosition());
            moved = analysis::prepareNewClip (*c3, pickupInfo, te::TimePosition(), true, true);
            check (moved && std::abs (beatOneAt (*c3) - bar) < 0.001 && c3->getPosition().getOffset().inSeconds() <= pickupInfo.soundStart,
                   "pickup before beat 1 at 0:00 -> moved to bar 2 (beat 1 at " + juce::String (beatOneAt (*c3), 3)
                     + " s), pickup kept");

            // 3b. Dropped late in bar 1 (1.5 s of 1.71): beat 1 still on bar 1.
            auto c4 = mixtopia::insertAudioFile (*te::getAudioTracks (*e5)[3], silentFile, te::TimePosition::fromSeconds (1.5));
            moved = analysis::prepareNewClip (*c4, silentInfo, te::TimePosition::fromSeconds (1.5), true, true);
            check (! moved && std::abs (beatOneAt (*c4)) < 0.001, "dropped late in bar 1 -> beat 1 on bar 1");

            // 3c. Dragged far past the left edge (beat 1 aimed 5 bars before 0:00): a silent start
            //     lands on bar 1 without a pickup message, a pickup song on bar 2.
            auto dragFarLeft = [&] (te::AudioClipBase& clip)
            {
                auto aim = te::TimePosition::fromSeconds (-5 * bar) - te::TimeDuration::fromSeconds (tempo::getDownbeat (clip) * tempo::getStretchRatio (clip));
                auto keep = tempo::getSourceRange (clip);   // (counted from the part that plays, as a drag does)
                return tempo::placeKeepingTheGrid (clip, aim, keep, analysis::getMayCutUntil (clip), keep.start);
            };
            moved = dragFarLeft (*c4);
            check (! moved && std::abs (beatOneAt (*c4)) < 0.001 && c4->getPosition().getStart() == te::TimePosition(),
                   "silent start dragged far past the left edge -> beat 1 on bar 1, no pickup message");
            moved = dragFarLeft (*c3);
            check (moved && std::abs (beatOneAt (*c3) - bar) < 0.001 && c3->getPosition().getOffset().inSeconds() <= pickupInfo.soundStart,
                   "pickup song dragged far past the left edge -> beat 1 on bar 2, pickup kept");
            c4 = nullptr;

            // 4. A slow fade-in: the trim stops at the beat line where the sound begins.
            auto fadeFile = makeSong (3.0, LeadIn::fadeIn);
            auto fadeInfo = analyseFile (fadeFile);
            auto lineBeforeSound = 3.0 - std::ceil ((3.0 - fadeInfo.soundStart) / beat) * beat;
            check (fadeInfo.silentUntil <= fadeInfo.soundStart - 0.02 + 1.0e-9 && std::abs (fadeInfo.silentUntil - lineBeforeSound) < 0.002,
                   "fade-in -> trim stops at the beat line before the sound (" + juce::String (fadeInfo.silentUntil, 3)
                     + " s; -60 dB reached at " + juce::String (fadeInfo.soundStart, 3) + " s)");

            // A key set by hand, then undone.
            silentInfo.key = "8A";
            auto c5 = mixtopia::insertAudioFile (*te::getAudioTracks (*e5)[0], silentFile, te::TimePosition::fromSeconds (60.0));
            analysis::prepareNewClip (*c5, silentInfo, te::TimePosition::fromSeconds (60.0), true, true);
            auto detectedKey = c5->state[analysis::keyProperty].toString();
            e5->getUndoManager().beginNewTransaction ("Set key");
            analysis::setKey (*c5, "11B", true);
            auto byHand = c5->state[analysis::keyProperty].toString() + " " + c5->state[analysis::keySourceProperty].toString();
            e5->getUndoManager().undo();
            check (detectedKey == "8A" && byHand == "11B set by hand" && c5->state[analysis::keyProperty].toString() == "8A",
                   "new song gets its detected key; a key set by hand undoes back to it");
            c5 = nullptr;

            c = nullptr; c2 = nullptr; c3 = nullptr;
            mixtopia::destroyEdit (e5);

            for (auto* f : { &silentFile, &pickupFile, &fadeFile })
                f->deleteFile();
        }

        // Waveform: at maximum zoom a kick shows on its hit. The kick: a short click, then a body
        // whose pitch falls from ~200 Hz to 50 Hz. (On real songs the low-end-only view showed kicks
        // 40-50 ms late; this synthetic kick doesn't reproduce that, it checks the full-sound view.)
        {
            constexpr double sampleRate = 44100.0, hit = 1.0;
            juce::AudioBuffer<float> kick (1, (int) (3.0 * sampleRate));
            kick.clear();
            juce::Random random (42);
            double phase = 0.0;

            for (int i = (int) (hit * sampleRate); i < kick.getNumSamples(); ++i)
            {
                auto t = i / sampleRate - hit;
                phase += juce::MathConstants<double>::twoPi * (50.0 + 150.0 * std::exp (-t / 0.02)) / sampleRate;
                auto body = 0.7 * std::sin (phase) * std::exp (-t / 0.25);
                auto click = t < 0.002 ? 0.9 * (random.nextFloat() * 2.0f - 1.0f) * std::exp (-t / 0.0005) : 0.0;
                kick.setSample (0, i, (float) (body + click));
            }

            auto file = juce::File::createTempFile (".wav");
            {
                std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                auto writer = juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                                               .withSampleRate (sampleRate)
                                                                               .withNumChannels (1)
                                                                               .withBitsPerSample (32)
                                                                               .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                writer->writeFromAudioSampleBuffer (kick, 0, kick.getNumSamples());
            }

            std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
            mixtopia::LoudnessOverview overview (mixtopia::LoudnessOverview::analyse (*reader));
            // Where each view first reaches half of its highest value around the hit.
            auto edgeOf = [&] (auto valueAt)
            {
                constexpr double step = 0.0005;
                auto highest = 0.0f;

                for (double t = 0.9; t < 1.2; t += step)
                    highest = juce::jmax (highest, valueAt (t, t + step));

                for (double t = 0.9; t < 1.2; t += step)
                    if (valueAt (t, t + step) > 0.5f * highest)
                        return t;

                return -1.0;
            };

            auto fullEdge = edgeOf ([&] (double a, double b) { return overview.getRelativeFullPeak (a, b); });
            auto bassEdge = edgeOf ([&] (double a, double b) { return overview.getRelativeBassPeak (a, b); });
            check (std::abs (fullEdge - hit) < 0.002,
                   "waveform: at maximum zoom a kick shows within 2 ms of its hit (" + juce::String ((fullEdge - hit) * 1000.0, 1)
                     + " ms; the low end alone: " + juce::String ((bassEdge - hit) * 1000.0, 1) + " ms)");
            reader = nullptr;
            file.deleteFile();
        }

        // Repeat part of a song (looping): the user's example - loop bars 97-105 at 138 BPM, 3 repeats
        // (+24 bars); volume dip at 99 back at 103, bass cut at 104 back at 113, outro fade 129-137.
        {
            namespace loops = mixtopia::loops;
            namespace env = mixtopia::envelopes;
            auto bar = 4.0 * 60.0 / 138.0;
            auto at = [bar] (double barNumber) { return (barNumber - 1.0) * bar; };   // bar 1 at 0 s
            loops::Loop loop;
            loop.start = at (97.0);
            loop.end = at (105.0);
            loop.repeats = 3;

            std::vector<env::Point> volumeSong { { at (98.0), 0.0f }, { at (99.0), -6.0f }, { at (103.0), 0.0f }, { at (129.0), 0.0f }, { at (137.0), -60.0f } };
            std::vector<env::Point> bassSong { { at (103.75), 0.0f }, { at (104.0), -60.0f }, { at (113.0), 0.0f } };

            // Stored as loops::apply leaves them: moved with the music.
            auto stored = [&] (std::vector<env::Point> points)
            {
                for (auto& p : points)
                    p.sourceTime = loops::transform ({}, loop, p.sourceTime);

                return points;
            };

            auto near = [] (float a, float b) { return std::abs (a - b) < 0.01f; };

            for (auto copy : { false, true })
            {
                loop.copyLines = copy;
                auto volume = loops::effectivePoints (stored (volumeSong), loop);
                auto bass = loops::effectivePoints (stored (bassSong), loop);
                auto v = [&] (double b) { return env::evaluate (volume, at (b) + 1.0e-3); };
                auto bs = [&] (double b) { return env::evaluate (bass, at (b) + 1.0e-3); };

                if (! copy)
                    check (near (v (99), -6) && near (v (107), 0) && near (v (115), 0) && near (v (123), 0)
                             && near (v (153), 0) && near (v (161) , -60) && near (bs (104), -60)
                             && near (bs (105), -60.0f + 60.0f * (1.0f / 33.0f)) && near (bs (121), -60.0f + 60.0f * (17.0f / 33.0f)) && near (bs (137), 0),
                           "loop bar 97-105 x3, lines only the first time: dip once (bar 99), bass rises slowly across the repeats to bar 137, "
                           "fade moves to 153-161");
                else
                    check (near (v (99), -6) && near (v (107), -6) && near (v (115), -6) && near (v (123), -6) && near (v (104), 0)
                             && near (bs (104), -60) && near (bs (112), -60) && near (bs (120), -60) && near (bs (128), -60)
                             && near (bs (105), 0) && near (bs (113), 0) && near (bs (121), 0)
                             && near (bs (129), -60.0f + 60.0f / 9.0f) && near (bs (137), 0) && near (v (153), 0) && near (v (161), -60),
                           "loop bar 97-105 x3, lines copied into every repeat: dip at 99/107/115/123, bass cut at 104 of every pass and back "
                           "when the next starts, back for good at 137, fade 153-161");
            }

            // New points start on their own line (user, 2026-10-27): 0 dB on an empty line, the
            // held value outside the points, on the straight line between two, and inside a repeat
            // whose lines are copied, the copied line's value.
            {
                std::vector<env::Point> line;
                auto first = loops::newPointValue (line, {}, 10.0);
                line.push_back ({ 10.0, first });
                auto second = loops::newPointValue (line, {}, 20.0);
                line.push_back ({ 20.0, -3.0f });
                auto third = loops::newPointValue (line, {}, 30.0);
                auto before = loops::newPointValue (line, {}, 5.0);
                auto between = loops::newPointValue ({ { 10.0, -6.0f }, { 20.0, 0.0f } }, {}, 15.0);
                loop.copyLines = true;
                auto inRepeat = loops::newPointValue (stored (volumeSong), loop, at (107.0) + 1.0e-3);
                check (first == 0.0f && second == 0.0f && near (third, -3.0f) && before == 0.0f && near (between, -3.0f) && near (inRepeat, -6.0f),
                       "new points start on their own line: first 0 dB, next = the line there (0, then -3 after a -3 point), "
                       "halfway between -6 and 0 = -3, in a copied repeat = the copied dip (-6)");
            }

            // The filter line (M11): position -100..+100, off in the middle with a dead zone, log steps
            // to low-pass 80 Hz / high-pass 8 kHz; new points start at off; values aren't clamped to dB.
            {
                auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-6 * b; };
                juce::ValueTree clipState ("CLIP");
                env::setPoints (clipState, env::Param::filter, { { 1.0, -100.0f }, { 2.0, 150.0f } }, nullptr);
                auto stored = env::getPoints (clipState, env::Param::filter);
                check (near (env::lowPassHz (-100.0f), 80.0) && near (env::highPassHz (100.0f), 8000.0)
                         && env::lowPassHz (-4.0f) == env::lowPassOpenHz && env::highPassHz (4.0f) == env::highPassOpenHz
                         && near (env::lowPassHz (-52.5f), std::sqrt (80.0 * 20000.0)),
                       "filter: low-pass 80 Hz at the bottom, high-pass 8 kHz at the top, open inside the dead zone, halfway = the musical middle");
                check (env::describeFilter (0.0f) == "off" && env::describeFilter (-100.0f) == "low-pass 80 Hz"
                         && env::describeFilter (100.0f) == "high-pass 8.0 kHz" && env::describeFilter (-30.0f) == "low-pass 4.7 kHz",
                       "filter readout: off / low-pass 80 Hz / high-pass 8.0 kHz / low-pass 4.7 kHz");
                check (loops::newPointValue ({}, {}, 5.0) == 0.0f && stored.size() == 2 && stored[0].db == -100.0f && stored[1].db == 100.0f,
                       "filter: a first point starts at off; values kept from -100 to +100 (not clamped to the dB range)");
                check (env::filterWet (0.0f) == 0.0f && env::filterWet (5.0f) == 1.0f && env::filterQ (-6.0f) > env::openQ
                         && env::filterQ (0.0f) == env::openQ && env::filterQ (-100.0f) == env::resonanceQ,
                       "filter: the untouched song at the middle, all filtered from the dead zone's edge, resonance only once it's working");
            }

            // Mapping: the song plays to the loop end, the range again, then on from the end.
            loop.copyLines = false;
            check (loops::playedToPlace (loop, at (100)).pass == 0 && std::abs (loops::playedToSong (loop, at (110)) - at (102)) < 1.0e-9
                     && loops::playedToPlace (loop, at (110)).pass == 1 && std::abs (loops::playedToSong (loop, at (130)) - at (106)) < 1.0e-9
                     && std::abs (loops::songToPlayed (loop, at (106)) - at (130)) < 1.0e-9,
                   "loop mapping: before / inside the repeats / after (bar 102 = 110 in repeat 1, song bar 106 plays at 130)");
        }

        // A loop on a real clip: longer by repeats x length, lines after it move, undo/redo exact,
        // saved and reopened, removing it restores the song.
        if (auto e = mixtopia::createEdit (engine))
        {
            namespace loops = mixtopia::loops;
            namespace env = mixtopia::envelopes;

            if (auto c = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], audioFile, te::TimePosition::fromSeconds (2.0)))
            {
                auto& um = e->getUndoManager();
                env::setPoints (c->state, env::Param::volume, { { 1.0, 0.0f }, { 3.0, -6.0f }, { 8.0, -60.0f } }, nullptr);
                auto before = c->getPosition();
                auto beforeFile = c->getOriginalFile();
                loops::Loop loop;
                loop.start = 2.0;
                loop.end = 4.0;
                loop.repeats = 2;

                um.beginNewTransaction ("Repeat part");
                loops::apply (*c, loop, false);
                auto after = c->getPosition();
                auto points = env::getPoints (c->state, env::Param::volume);
                auto longer = std::abs ((after.getLength() - before.getLength()).inSeconds() - 4.0) < 1.0e-6;
                check (longer && loops::isLoopFile (c->getOriginalFile()) && std::abs (c->getAudioFile().getLength() - 14.0) < 1.0e-6
                         && points.size() == 3 && points[0].sourceTime == 1.0 && points[1].sourceTime == 3.0 && std::abs (points[2].sourceTime - 12.0) < 1.0e-9,
                       "loop on a clip: 2 s x 2 makes it 4 s longer, plays the .mixloop, points before/inside stay, after move by 4 s");

                um.undo();
                auto undone = c->getPosition() == before && c->getOriginalFile() == beforeFile && ! loops::getLoop (*c)
                               && env::getPoints (c->state, env::Param::volume)[2].sourceTime == 8.0;
                um.redo();
                check (undone && c->getPosition() == after && loops::getLoop (*c) == loop,
                       "loop: undo restores the song exactly (length, file, lines), redo brings the loop back");

                // Trim the end into the outro, then add a repeat: the end moves with the music.
                um.beginNewTransaction ("Trim");
                c->setEnd (c->getPosition().getEnd() - te::TimeDuration::fromSeconds (1.0), true);
                auto trimmedEnd = c->getPosition().getEnd();
                auto more = loop;
                more.repeats = 3;
                um.beginNewTransaction ("More repeats");
                loops::apply (*c, more, false);
                check (std::abs ((c->getPosition().getEnd() - trimmedEnd).inSeconds() - 2.0) < 1.0e-6 && loops::getLoop (*c) == more,
                       "loop: a trimmed end after the loop moves with an extra repeat (same musical end)");

                auto file = juce::File::createTempFile (".mixtopia");
                auto saved = mixtopia::project::save (*e, file).isEmpty();
                auto contents = mixtopia::project::read (file);
                auto reopened = contents.error.isEmpty() ? mixtopia::project::createEdit (engine, contents) : nullptr;
                auto* r = reopened != nullptr ? dynamic_cast<te::AudioClipBase*> (te::getAudioTracks (*reopened)[0]->getClips()[0]) : nullptr;
                check (saved && r != nullptr && loops::getLoop (*r) == more && loops::isLoopFile (r->getOriginalFile())
                         && r->getPosition() == c->getPosition() && std::abs (r->getAudioFile().getLength() - 16.0) < 1.0e-6,
                       "loop: saved and reopened (same loop, same place, plays the looped song)");
                r = nullptr;
                mixtopia::destroyEdit (reopened);
                file.deleteFile();

                um.beginNewTransaction ("Remove");
                loops::apply (*c, std::nullopt, false);
                check (! loops::getLoop (*c) && c->getOriginalFile() == beforeFile && std::abs (c->getAudioFile().getLength() - 10.0) < 1.0e-6
                         && std::abs ((c->getPosition().getEnd() - trimmedEnd).inSeconds() + 4.0) < 1.0e-6
                         && std::abs (env::getPoints (c->state, env::Param::volume)[2].sourceTime - 8.0) < 1.0e-9,
                       "loop removed: the song plays its own file again, 4 s shorter than with 2 repeats, lines back in place");
            }

            mixtopia::destroyEdit (e);
        }

        // Chopping (M11): cuts make pieces without moving anything; a piece separated keeps its part, the song's
        // lines and fades only at its cut edges; put back, pieces join into one clip again; undo/redo; Ctrl+D
        // copies; labels by the piece's place in the song; colours; saved and reopened; a loop moves the cuts.
        if (auto e = mixtopia::createEdit (engine))
        {
            namespace chops = mixtopia::chops;
            namespace env = mixtopia::envelopes;
            auto tracks = te::getAudioTracks (*e);
            auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-6; };

            if (auto c = mixtopia::insertAudioFile (*tracks[0], audioFile, te::TimePosition::fromSeconds (2.0)))
            {
                auto& um = e->getUndoManager();
                tempo::setBpmAndDownbeat (*c, 120.0, 0.25, tempo::Snap::bars);
                env::setPoints (c->state, env::Param::volume, { { 1.0, 0.0f }, { 9.0, -12.0f } }, nullptr);
                um.clearUndoHistory();
                auto whole = c->getPosition();
                auto range = tempo::getSourceRange (*c);

                um.beginNewTransaction ("Cut");
                auto cut1 = chops::cut (*c, 4.25), cut2 = chops::cut (*c, 6.25);
                auto tooClose = chops::cut (*c, 6.27), atEdge = chops::cut (*c, range.end - 0.01);
                check (cut1 && cut2 && ! tooClose && ! atEdge && chops::getPieces (*c).size() == 3 && c->getPosition() == whole
                         && tracks[0]->getClips().size() == 1,
                       "chopping: two cuts make three pieces and nothing moves; a cut next to another or at the song's edge is refused");

                um.beginNewTransaction ("Move piece");
                auto* mid = chops::separate (*c, 1);
                te::AudioClipBase* last = nullptr;

                for (auto* other : tracks[0]->getClips())
                    if (other != c.get() && other != mid)
                        last = dynamic_cast<te::AudioClipBase*> (other);

                auto linesKept = [&] (te::AudioClipBase* x) { return x != nullptr && env::getPoints (x->state, env::Param::volume).size() == 2; };
                auto r0 = tempo::getSourceRange (*c), r1 = tempo::getSourceRange (*mid);
                auto r2 = last != nullptr ? tempo::getSourceRange (*last) : tempo::SourceRange {};
                check (tracks[0]->getClips().size() == 3 && last != nullptr && near (r0.end, 4.25) && near (r1.start, 4.25) && near (r1.end, 6.25)
                         && near (r2.start, 6.25) && near (r2.end, range.end)
                         && ! chops::hasFadeIn (*c) && chops::hasFadeOut (*c) && chops::hasFadeIn (*mid) && chops::hasFadeOut (*mid)
                         && chops::hasFadeIn (*last) && ! chops::hasFadeOut (*last) && linesKept (c.get()) && linesKept (mid) && linesKept (last)
                         && near (mid->getPosition().getStart().inSeconds(), c->getPosition().getEnd().inSeconds())
                         && near (tempo::getContentStartBeat (*mid), tempo::getContentStartBeat (*c)),
                       "chopping: a separated piece = its own clip playing its part in place; fades only at cut edges; every piece keeps the song's lines");

                auto merges = chops::joinTouching (*e);
                auto cuts = chops::getCuts (*c);
                check (merges.size() == 2 && tracks[0]->getClips().size() == 1 && cuts.size() == 2 && near (cuts[0], 4.25) && near (cuts[1], 6.25)
                         && ! chops::hasFadeIn (*c) && ! chops::hasFadeOut (*c) && near (c->getPosition().getEnd().inSeconds(), whole.getEnd().inSeconds()),
                       "chopping: pieces put back in place join into one clip again, with their cuts and no fades");

                um.undo();
                auto undoneMove = tracks[0]->getClips().size() == 1 && chops::getCuts (*c).size() == 2;
                um.undo();
                auto undoneCuts = chops::getCuts (*c).empty() && ! chops::isPiece (*c) && c->getPosition() == whole;
                um.redo();
                check (undoneMove && undoneCuts && chops::getCuts (*c).size() == 2, "chopping: undo the move, undo the cuts (the song whole again), redo the cuts");

                // Ctrl+D: piece b has piece c after it on lane 1 -> the copy goes on lane 2; piece c's copy fits after it.
                um.beginNewTransaction ("Copy");
                juce::String status, tailStatus;
                auto* copy = chops::duplicate (*c, 1, status);
                auto* tailCopy = chops::duplicate (*c, 2, tailStatus);
                auto copyRange = copy != nullptr ? tempo::getSourceRange (*copy) : tempo::SourceRange {};
                check (copy != nullptr && copy->getTrack() == tracks[1] && status.contains ("lane 2") && near (copyRange.start, 4.25) && near (copyRange.end, 6.25)
                         && near (copy->getPosition().getStart().inSeconds(), chops::getPieceEditRange (*c, 1).getEnd())
                         && chops::hasFadeIn (*copy) && chops::hasFadeOut (*copy)
                         && tailCopy != nullptr && tailCopy->getTrack() == tracks[0] && tailStatus.isEmpty()
                         && near (tailCopy->getPosition().getStart().inSeconds(), c->getPosition().getEnd().inSeconds()),
                       "Ctrl+D: a copy right after the piece - on its own lane if there's room, else the next lane (status says so)");

                auto labels = chops::listPieces (*e);
                check (chops::labelFor (labels, *c, 0) == "1a" && chops::labelFor (labels, *c, 1) == "1b" && chops::labelFor (labels, *c, 2) == "1c"
                         && chops::labelFor (labels, *copy, 0) == juce::String (juce::CharPointer_UTF8 ("1b\xc2\xb7" "2"))
                         && chops::labelFor (labels, *tailCopy, 0) == juce::String (juce::CharPointer_UTF8 ("1c\xc2\xb7" "2")),
                       juce::String (juce::CharPointer_UTF8 ("labels: song number + a letter by the piece's place in the song; copies 1b\xc2\xb7" "2, 1c\xc2\xb7" "2")));

                check (mixtopia::clipcolours::get (*copy) == mixtopia::clipcolours::get (*c) && mixtopia::clipcolours::keepApartFromNeighbours (*copy).isEmpty()
                         && mixtopia::clipcolours::get (*copy) == mixtopia::clipcolours::get (*c),
                       "colours: pieces and copies keep the song's colour (the neighbour rule is between different songs)");

                auto file = juce::File::createTempFile (".mixtopia");
                auto saved = mixtopia::project::save (*e, file).isEmpty();
                auto contents = mixtopia::project::read (file);
                auto reopened = contents.error.isEmpty() ? mixtopia::project::createEdit (engine, contents) : nullptr;
                auto reopenedOk = false;

                if (reopened != nullptr)
                {
                    auto rt = te::getAudioTracks (*reopened);
                    auto* r = dynamic_cast<te::AudioClipBase*> (rt[0]->getClips()[0]);
                    auto* rc = rt[1]->getClips().isEmpty() ? nullptr : dynamic_cast<te::AudioClipBase*> (rt[1]->getClips()[0]);
                    reopenedOk = r != nullptr && rc != nullptr && chops::getCuts (*r).size() == 2 && chops::hasFadeIn (*rc) && chops::hasFadeOut (*rc)
                                   && chops::labelFor (chops::listPieces (*reopened), *rc, 0) == juce::String (juce::CharPointer_UTF8 ("1b\xc2\xb7" "2"));
                }

                check (saved && reopenedOk, "chopping: saved and reopened (cuts, fades, labels)");
                mixtopia::destroyEdit (reopened);
                file.deleteFile();

                // A loop before the cuts: they move with the music (1 s added).
                um.beginNewTransaction ("Loop");
                mixtopia::loops::Loop loop;
                loop.start = 1.0;
                loop.end = 2.0;
                loop.repeats = 1;
                mixtopia::loops::apply (*c, loop, false);
                auto looped = chops::getCuts (*c);
                check (looped.size() == 2 && near (looped[0], 5.25) && near (looped[1], 7.25), "chopping: a loop before the cuts moves them with the music");
            }

            mixtopia::destroyEdit (e);
        }

        // Chopping on the timeline itself, driven by the mouse handlers (2026-10-28): dragging the first,
        // a middle or the last piece to another lane moves only that piece (the first one used to drag
        // the whole song along); Shift+click two pieces and drag one: both move by the same amount; a
        // Shift+drag box selects the pieces it touches and Del deletes them all.
        if (auto e = mixtopia::createEdit (engine))
        {
            namespace chops = mixtopia::chops;
            auto tracks = te::getAudioTracks (*e);
            auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-6; };

            if (auto c = mixtopia::insertAudioFile (*tracks[0], audioFile, te::TimePosition::fromSeconds (2.0)))
            {
                auto& um = e->getUndoManager();
                tempo::setBpmAndDownbeat (*c, 120.0, 0.25, tempo::Snap::bars);
                chops::cut (*c, 4.25);
                chops::cut (*c, 6.25);
                um.clearUndoHistory();
                auto pieces = chops::getPieces (*c);

                TimelineComponent tl (*e);
                tl.setBounds (0, 0, 1600, 700);
                tl.setVisibleRange (0.0, 16.0);

                auto event = [&] (juce::Point<int> at, juce::Point<int> down, juce::ModifierKeys mods, bool dragged)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                             &tl, &tl, juce::Time::getCurrentTime(), down.toFloat(), juce::Time::getCurrentTime(), 1, dragged);
                };
                auto drag = [&] (juce::Point<int> from, juce::Point<int> to, juce::ModifierKeys mods)
                {
                    tl.mouseDown (event (from, from, mods, false));
                    tl.mouseDrag (event (to, from, mods, true));
                    tl.mouseUp (event (to, from, mods, true));
                };
                auto click = [&] (juce::Point<int> at, juce::ModifierKeys mods)
                {
                    tl.mouseDown (event (at, at, mods, false));
                    tl.mouseUp (event (at, at, mods, false));
                };
                auto middleOf = [&] (int i) { return chops::getPieceEditRange (*c, i).getStart() + 0.5 * chops::getPieceEditRange (*c, i).getLength(); };
                double middles[3] { middleOf (0), middleOf (1), middleOf (2) };

                auto onlyThatPieceMoved = true;

                for (int i = 0; i < 3; ++i)
                {
                    drag (tl.pointFor (middles[i], 0), tl.pointFor (middles[i], 1), {});
                    auto moved = tracks[1]->getClips();
                    auto* m = moved.size() == 1 ? dynamic_cast<te::AudioClipBase*> (moved[0]) : nullptr;
                    // (What each plays from where it's seen to start: a part that starts where another ends
                    // starts a few ms earlier for its crossfade.)
                    auto shown = [] (te::AudioClipBase& x)
                    {
                        return tempo::SourceRange { tempo::editToSourceSeconds (x, chops::visibleStart (x)), tempo::getSourceRange (x).end };
                    };
                    auto range = m != nullptr ? shown (*m) : tempo::SourceRange {};
                    auto left = 0.0;

                    for (auto* other : tracks[0]->getClips())
                        if (auto* o = dynamic_cast<te::AudioClipBase*> (other))
                            left += shown (*o).end - shown (*o).start;

                    onlyThatPieceMoved = onlyThatPieceMoved && m != nullptr && near (range.start, pieces[(size_t) i].start) && near (range.end, pieces[(size_t) i].end)
                                           && near (left, (pieces.back().end - pieces.front().start) - (range.end - range.start));
                    um.undo();
                    onlyThatPieceMoved = onlyThatPieceMoved && tracks[1]->getClips().isEmpty() && tracks[0]->getClips().size() == 1 && chops::getCuts (*c).size() == 2;
                }

                check (onlyThatPieceMoved, "chopping (mouse): dragging the first, a middle or the last piece to another lane moves only that piece; undo puts it back");

                // Two pieces selected (click, Shift+click), the first dragged ~2 bars later: both move by 2 bars.
                click (tl.pointFor (middles[0], 0), {});
                click (tl.pointFor (middles[2], 0), juce::ModifierKeys::shiftModifier);
                auto startsBefore = std::pair (chops::getPieceEditRange (*c, 0).getStart(), chops::getPieceEditRange (*c, 2).getStart());
                auto middleStart = chops::getPieceEditRange (*c, 1).getStart();
                drag (tl.pointFor (middles[0], 0), tl.pointFor (middles[0] + 4.0, 0), {});
                std::vector<double> starts;

                for (auto* clip : tracks[0]->getClips())
                    starts.push_back (clip->getPosition().getStart().inSeconds());

                std::sort (starts.begin(), starts.end());
                auto twoBars = 2.0 * 4.0 * 60.0 / 140.0;   // (the drag snaps to bars of the 140 BPM project)
                check (starts.size() == 3 && near (starts[0], startsBefore.first + twoBars) && near (starts[2], startsBefore.second + twoBars)
                         && near (starts[1], middleStart),
                       "chopping (mouse): Shift+click two pieces, drag one 2 bars: both move 2 bars, the one between stays");
                um.undo();

                // A box (Shift+drag in the empty third lane up across the song) selects all three pieces; Del deletes them.
                drag (tl.pointFor (3.0, 2), tl.pointFor (11.0, 0), juce::ModifierKeys::shiftModifier);
                auto selected = tl.getSelectedClip() != nullptr;
                tl.deleteSelectedClip();
                auto gone = tracks[0]->getClips().isEmpty();
                um.undo();
                check (selected && gone && tracks[0]->getClips().size() == 1 && chops::getCuts (*c).size() == 2,
                       "chopping (mouse): a Shift+drag box selects every piece it touches, Del deletes them all, undo brings them back");
            }

            mixtopia::destroyEdit (e);
        }

        // A song whose start was trimmed off, dragged far left with Snap on (the timeline's mouse handlers):
        // trimmed on a bar line it reaches 0:00; trimmed mid-bar, its first whole bar lands on bar 2 with the
        // part bar before it in bar 1 (user, 2026-10-28: it stopped short of the start, as if the trimmed
        // part were still there).
        if (auto e = mixtopia::createEdit (engine))
        {
            auto tracks = te::getAudioTracks (*e);
            auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-6; };

            if (auto c = mixtopia::insertAudioFile (*tracks[0], audioFile, te::TimePosition::fromSeconds (4.0)))
            {
                tempo::setBpmAndDownbeat (*c, 120.0, 0.25, tempo::Snap::bars);   // the song's bars: 0.25, 2.25, 4.25, ...
                TimelineComponent tl (*e);
                tl.setBounds (0, 0, 1600, 700);
                tl.setVisibleRange (0.0, 16.0);
                auto event = [&] (juce::Point<int> at, juce::Point<int> down, bool dragged)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                             &tl, &tl, juce::Time::getCurrentTime(), down.toFloat(), juce::Time::getCurrentTime(), 1, dragged);
                };
                auto dragFarLeft = [&]
                {
                    auto middle = 0.5 * (c->getPosition().getStart().inSeconds() + c->getPosition().getEnd().inSeconds());
                    auto from = tl.pointFor (middle, 0), to = tl.pointFor (middle - 12.0, 0);
                    tl.mouseDown (event (from, from, false));
                    tl.mouseDrag (event (to, from, true));
                    tl.mouseUp (event (to, from, true));
                };
                auto projectBar = 4.0 * 60.0 / 140.0;

                tempo::place (*c, tempo::getContentStartBeat (*c), { 4.25, tempo::getSourceRange (*c).end });   // trimmed to a bar line
                dragFarLeft();
                auto onBar = near (c->getPosition().getStart().inSeconds(), 0.0) && near (tempo::getSourceRange (*c).start, 4.25);

                tempo::place (*c, tempo::getContentStartBeat (*c), { 5.25, tempo::getSourceRange (*c).end });   // trimmed mid-bar (beat 3)
                dragFarLeft();
                auto midBar = near (tempo::sourceToEditSeconds (*c, 6.25), projectBar) && near (tempo::getSourceRange (*c).start, 5.25);

                check (onBar && midBar, "a song trimmed on a bar line, dragged to the start with Snap on, reaches 0:00; trimmed mid-bar, "
                                        "its first whole bar lands on bar 2 (nothing cut)");
            }

            mixtopia::destroyEdit (e);
        }

        // Looping step 2: quick loops are exact bars of the song; a loop that runs into the next song on
        // its lane makes room (songs on every lane after the loop, and the tempo points, move later).
        if (auto e = mixtopia::createEdit (engine))
        {
            namespace loops = mixtopia::loops;
            namespace tempo = mixtopia::tempo;
            tempo::setProjectBpm (*e, 120.0);   // a bar = 2 s
            auto tracks = te::getAudioTracks (*e);
            auto a = mixtopia::insertAudioFile (*tracks[0], audioFile, te::TimePosition::fromSeconds (2.0));    // 2-12 s
            auto b = mixtopia::insertAudioFile (*tracks[0], audioFile, te::TimePosition::fromSeconds (13.0));   // same lane, next
            auto c = mixtopia::insertAudioFile (*tracks[1], audioFile, te::TimePosition::fromSeconds (14.0));   // another lane, later
            auto d = mixtopia::insertAudioFile (*tracks[2], audioFile, te::TimePosition::fromSeconds (3.0));    // starts before the loop's end

            if (a != nullptr && b != nullptr && c != nullptr && d != nullptr)
            {
                // Quick loop: 8 bars of a 138 BPM song from 10.5 s = 8 * 4 * 60 / 138 s, to the sample.
                a->state.setProperty (tempo::bpmProperty, 138.0, nullptr);
                a->state.setProperty (tempo::downbeatProperty, 0.25, nullptr);
                auto beat = 60.0 / 138.0;
                check (std::abs (loops::nearestBeat (*a, 0.25 + 3 * beat + 0.1) - (0.25 + 3 * beat)) < 1.0e-9
                         && std::abs (loops::nearestBeat (*a, 0.25 + 3 * beat + 0.3) - (0.25 + 4 * beat)) < 1.0e-9,
                       "quick loop with Snap on starts on the song's nearest beat tick (0.1 s after beat 4 -> beat 4; 0.3 s after -> beat 5)");
                a->state.removeProperty (tempo::downbeatProperty, nullptr);
                juce::String problem;
                auto quick = loops::makeQuick (*a, 1.0, 2, problem);
                auto tooLong = loops::makeQuick (*a, 5.0, 8, problem);
                a->state.removeProperty (tempo::bpmProperty, nullptr);
                check (quick && std::abs (quick->length() - 2 * 4 * 60.0 / 138.0) < 1.0 / 44100.0 && quick->start == 1.0 && quick->repeats == 1
                         && ! tooLong && problem.contains ("Not enough"),
                       "quick loop: 2 bars at 138 BPM = " + juce::String (quick ? quick->length() : 0.0, 6) + " s (exact to the sample); 8 bars "
                       "from 5 s of a 10 s song refused: \"" + problem + "\"");

                auto plan = tempo::getTempoPlan (*e);
                plan.points = { { 40.0, 120.0 } };   // bar 11 = 20 s
                tempo::setTempoPlan (*e, plan);
                auto& um = e->getUndoManager();
                um.beginNewTransaction ("Loop");
                loops::Loop loop;
                loop.start = 2.0;
                loop.end = 4.0;
                loop.repeats = 2;                    // +4 s = 2 bars: song A now ends at 16 s, past B (13 s)
                auto result = loops::apply (*a, loop, false);
                auto near = [] (te::TimePosition t, double s) { return std::abs (t.inSeconds() - s) < 1.0e-6; };
                auto movedPlan = tempo::getTempoPlan (*e);
                check (result.madeRoom.contains ("2 bars") && near (b->getPosition().getStart(), 17.0) && near (c->getPosition().getStart(), 18.0)
                         && near (d->getPosition().getStart(), 3.0) && movedPlan.points.size() == 1 && movedPlan.points[0].beat == 48.0,
                       "loop running into the next song on its lane makes room: B 13 -> 17 s, lane 2's later song 14 -> 18 s, the song that "
                       "starts before the loop's end stays, the tempo point moves 2 bars (\"" + result.madeRoom + "\")");

                um.undo();
                check (near (b->getPosition().getStart(), 13.0) && near (c->getPosition().getStart(), 14.0) && tempo::getTempoPlan (*e).points[0].beat == 40.0
                         && ! loops::getLoop (*a),
                       "make room: undo puts every song and the tempo point back");

                // Plenty of room: nothing else moves.
                b->setStart (te::TimePosition::fromSeconds (40.0), false, true);
                um.beginNewTransaction ("Loop with room");
                result = loops::apply (*a, loop, false);
                check (result.madeRoom.isEmpty() && near (b->getPosition().getStart(), 40.0) && near (c->getPosition().getStart(), 14.0),
                       "a loop with room before the next song moves nothing else");
            }

            a = b = c = d = nullptr;
            mixtopia::destroyEdit (e);
        }

        // Bug fixes 2026-10-23: (1) a song dragged freely past 0:00 stops there with nothing cut;
        // (2) songs dropped on the library while a collection is showing go into it too.
        if (auto e = mixtopia::createEdit (engine))
        {
            namespace tempo = mixtopia::tempo;
            tempo::setProjectBpm (*e, 120.0);

            if (auto c = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], audioFile, te::TimePosition::fromSeconds (6.0)))
            {
                tempo::apply (*c, 120.0, 0.0, { 0.0, 10.0 }, te::TimePosition::fromSeconds (6.0));
                auto keep = tempo::getSourceRange (*c);
                auto past = tempo::toBeats (*e, te::TimePosition::fromSeconds (1.0)) - 10.0;   // a drag 5 s past 0:00
                tempo::place (*c, tempo::contentStartNotBeforeZero (*c, past, keep), keep);
                check (c->getPosition().getStart() == te::TimePosition() && std::abs (c->getPosition().getOffset().inSeconds() - keep.start) < 1.0e-9
                         && std::abs (c->getPosition().getLength().inSeconds() - (keep.end - keep.start)) < 1.0e-6,
                       "a song dragged freely past 0:00 stops at 0:00 with nothing cut");
            }

            mixtopia::destroyEdit (e);
        }

        {
            namespace lib = mixtopia::library;
            auto storage = juce::File::createTempFile (".xml");
            {
                lib::Library library (engine, storage);
                library.createCollection ("Friday set");
                LibraryPanel panel (library);
                panel.showCollection ("Friday set");
                panel.dropFiles ({ audioFile.getFullPathName() });
                auto* collection = library.findCollection ("Friday set");
                auto inCollection = collection != nullptr && collection->paths.contains (audioFile.getFullPathName());
                auto inLibrary = library.find (audioFile.getFullPathName()) != nullptr;

                // All songs showing: dropped songs go only into the library.
                library.createCollection ("Other");
                panel.showCollection ({});
                panel.dropFiles ({ audioFile.getFullPathName() });
                // (Look the collection up again: creating another one may have moved it.)
                check (inCollection && inLibrary && library.findCollection ("Other")->paths.isEmpty()
                         && library.findCollection ("Friday set")->paths.size() == 1,
                       "songs dropped on the library while a collection is showing go into it (and the library); with All songs, only the library");
                library.waitUntilAnalysed (60000);
            }
            storage.deleteFile();
        }

        // Bug fix 2026-10-23 ("Light At The End"): two parts of Mixtopia decoding the same song at once
        // both get it (they used to clash on one temporary file); a song the library marked as
        // failed is tried again when the library opens.
        if (audioFile.hasFileExtension ("mp3"))
        {
            auto copy = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mixtopia-decode-twice-" + juce::String (juce::Random().nextInt64()) + ".mp3");
            audioFile.copyFileTo (copy);
            juce::File first, second;
            std::thread a ([&] { first = mixtopia::decodedCache::getPlayableFile (engine, copy); });
            std::thread b ([&] { second = mixtopia::decodedCache::getPlayableFile (engine, copy); });
            a.join();
            b.join();
            auto length = te::AudioFile (engine, first).getInfo().lengthInSamples;
            check (first != juce::File() && first == second && first.existsAsFile() && length == 441000,
                   "the same song decoded from two threads at once: both get the one decoded copy (" + juce::String (length) + " samples)");

            auto storage = juce::File::createTempFile (".xml");
            storage.replaceWithText ("<MIXTOPIA_LIBRARY version=\"1\"><SONG path=\"" + copy.getFullPathName()
                                       + "\" title=\"t\" artist=\"a\" length=\"0.0\" bpm=\"0.0\" downbeat=\"0.0\" key=\"\" failed=\"1\" added=\"1\"/></MIXTOPIA_LIBRARY>");
            {
                mixtopia::library::Library library (engine, storage);
                library.waitUntilAnalysed (60000);
                auto* song = library.find (copy.getFullPathName());
                check (song != nullptr && ! song->failed && std::abs (song->lengthSeconds - 10.0) < 0.01,
                       "a song the library marked as failed is analysed again when the library opens (length " + juce::String (song != nullptr ? song->lengthSeconds : 0.0, 2) + " s)");
            }
            storage.deleteFile();
            first.deleteFile();
            copy.deleteFile();
        }

        // Library Length column (user, 2026-10-23, after Key): formatted, and sorts by length.
        {
            check (LibraryPanel::formatLength (402.4) == "6:42" && LibraryPanel::formatLength (3735.0) == "1:02:15"
                     && LibraryPanel::formatLength (59.6) == "1:00" && LibraryPanel::formatLength (0.0) == "-",
                   "library Length column: 6:42, 1:02:15, 1:00 (rounded), \"-\" while unknown");

            // A 3 s song and the 10 s test song, sorted by length both ways.
            auto shortSong = juce::File::createTempFile (".wav");
            {
                juce::AudioBuffer<float> tone (2, 3 * 44100);
                for (int i = 0; i < tone.getNumSamples(); ++i)
                    tone.setSample (0, i, 0.2f * std::sin (i * 0.06f)), tone.setSample (1, i, tone.getSample (0, i));
                std::unique_ptr<juce::OutputStream> stream (shortSong.createOutputStream());
                auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                                  .withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
                writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
            }

            auto storage = juce::File::createTempFile (".xml");
            {
                mixtopia::library::Library library (engine, storage);
                library.add ({ audioFile, shortSong });
                library.waitUntilAnalysed (120000);
                LibraryPanel panel (library);
                panel.sortBy (LibraryPanel::lengthColumnId, true);
                auto shortestFirst = panel.getListedPath (0) == shortSong.getFullPathName();
                panel.sortBy (LibraryPanel::lengthColumnId, false);
                check (shortestFirst && panel.getListedPath (0) == audioFile.getFullPathName(),
                       "library sorts by Length: 3 s song first (shortest first), 10 s song first (longest first)");
            }
            storage.deleteFile();
            shortSong.deleteFile();
        }

        // Song colours (M8): songs near each other never share one; far apart they may; colours
        // from before M8 become their new counterparts.
        {
            namespace colours = mixtopia::clipcolours;
            auto e = mixtopia::createEdit (engine);
            auto tracks = te::getAudioTracks (*e);
            auto a = mixtopia::insertAudioFile (*tracks[0], audioFile, te::TimePosition());
            auto b = mixtopia::insertAudioFile (*tracks[1], audioFile, te::TimePosition::fromSeconds (5.0));
            auto c = mixtopia::insertAudioFile (*tracks[2], audioFile, te::TimePosition::fromSeconds (200.0));

            if (a != nullptr && b != nullptr && c != nullptr)
            {
                check (colours::get (*a) != colours::get (*b), "overlapping new songs get different colours");

                b->state.setProperty (colours::property, colours::get (*a).toString(), nullptr);
                auto message = colours::keepApartFromNeighbours (*b);
                check (colours::get (*a) != colours::get (*b) && message.isNotEmpty(),
                       "a song moved onto one of its colour changes colour: \"" + message + "\"");

                c->state.setProperty (colours::property, colours::get (*a).toString(), nullptr);
                check (colours::keepApartFromNeighbours (*c).isEmpty() && colours::get (*c) == colours::get (*a),
                       "songs more than a minute apart may share a colour");

                a->state.setProperty (colours::property, juce::Colour (0xff22d3ee).toString(), nullptr);
                colours::assignMissing (*e);
                check (colours::getName (colours::get (*a)) == "yellow", "an old cyan song becomes yellow");
            }

            a = b = c = nullptr;
            mixtopia::destroyEdit (e);
        }

        // Song cache (user, 2026-10-23: keep the OS drive small; songs may live on an external drive).
        // All in a cache folder of the test's own (MIXTOPIA_CACHE_DIR), never the user's.
        {
            namespace cache = mixtopia::decodedCache;
            namespace project = mixtopia::project;
            auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mixtopia-cache-selftest");
            root.deleteRecursively();
            setenv ("MIXTOPIA_CACHE_DIR", root.getChildFile ("cache").getFullPathName().toRawUTF8(), 1);

            // Trim: least recently used first down to the size limit, never the kept songs (and their
            // waveforms); Clear: everything but the kept songs. Big files are sparse (no real space).
            auto savedSize = mixtopia::settings::getFile().getValue ("mixtopiaSongCacheGB");
            auto decodedFolder = cache::getCacheFolder ("decoded"), overviewFolder = cache::getCacheFolder ("overviews");
            decodedFolder.createDirectory();
            overviewFolder.createDirectory();
            auto fake = [&] (const juce::String& name, double gb, int daysAgo)
            {
                auto f = decodedFolder.getChildFile (name);
                {
                    juce::FileOutputStream out (f);
                    out.setPosition ((juce::int64) (gb * 1024 * 1024 * 1024) - 1);
                    out.writeByte (0);
                }
                f.setLastAccessTime (juce::Time::getCurrentTime() - juce::RelativeTime::days (daysAgo));
                return f;
            };
            auto kept = fake ("Kept Song_0123abcd.wav", 3.0, 30);   // oldest, but in the open project
            auto older = fake ("Older_00aa11bb.wav", 2.0, 20);
            auto newer = fake ("Newer_00cc22dd.wav", 1.5, 1);
            auto keptWave = overviewFolder.getChildFile ("0123abcd.bin"), olderWave = overviewFolder.getChildFile ("00aa11bb.bin");
            keptWave.replaceWithText ("x");
            olderWave.replaceWithText ("x");
            olderWave.setLastAccessTime (juce::Time::getCurrentTime() - juce::RelativeTime::days (25));
            keptWave.setLastAccessTime (juce::Time::getCurrentTime() - juce::RelativeTime::days (40));

            check (cache::getSongKey (kept) == "0123abcd" && cache::getSongKey (audioFile) == cache::getCacheKey (audioFile),
                   "song cache: a decoded copy's key is its song's (" + cache::getSongKey (kept) + ")");

            cache::setSizeLimitGB (5);
            auto freed = cache::trimToSizeLimit ({ kept });
            check (kept.existsAsFile() && keptWave.existsAsFile() && ! older.existsAsFile() && ! olderWave.existsAsFile()
                     && newer.existsAsFile() && cache::getUsedBytes() <= (juce::int64) 5 * 1024 * 1024 * 1024,
                   "song cache trimmed to 5 GB: the oldest unused copy and waveform go, the open project's song stays although "
                   "it's the oldest (freed " + juce::File::descriptionOfSizeInBytes (freed) + ")");

            cache::clear ({ kept });
            check (kept.existsAsFile() && keptWave.existsAsFile() && ! newer.existsAsFile(),
                   "Clear song cache: everything but the open project's songs deleted");

            if (savedSize.isEmpty())
                mixtopia::settings::getFile().removeValue ("mixtopiaSongCacheGB");
            else
                mixtopia::settings::getFile().setValue ("mixtopiaSongCacheGB", savedSize);

            mixtopia::settings::getFile().saveIfNeeded();
            check (cache::getSizeLimitGB() == (savedSize.isEmpty() ? 10 : savedSize.getIntValue()), "song cache size setting restored");

            // The library analyses a song straight from its file: nothing decoded to disk, and the
            // result is found again (same key) once the song is decoded for the timeline.
            if (audioFile.hasFileExtension ("mp3"))
            {
                auto song = root.getChildFile ("drive").getChildFile ("Song On Drive.mp3");
                song.getParentDirectory().createDirectory();
                audioFile.copyFileTo (song);

                auto fromFile = mixtopia::analysis::analyse (engine, cache::getReadableFile (song));
                check (fromFile.has_value() && ! cache::isReady (song) && cache::getReadableFile (song) == song,
                       "library analysis read the MP3 itself; no decoded copy written");

                auto decoded = cache::getPlayableFile (engine, song);
                auto fromCopy = mixtopia::analysis::getCached (decoded), cached = mixtopia::analysis::getCached (song);
                check (fromCopy.has_value() && cached.has_value() && fromCopy->loudness == cached->loudness && fromCopy->key == cached->key
                         && fromCopy->soundStart == cached->soundStart && fromCopy->grid.downbeat == cached->grid.downbeat,
                       "the library's analysis is found for the decoded copy (same song key)");

                // "Open without them" with the drive unplugged: songs with a decoded copy still play -
                // a plain one and one with a loop (its .mixloop names the copy).
                auto e = mixtopia::createEdit (engine);
                auto plain = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], song, te::TimePosition::fromSeconds (2.0));
                auto looped = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[1], song, te::TimePosition::fromSeconds (2.0));
                mixtopia::loops::Loop loop;
                loop.start = 2.0;
                loop.end = 4.0;
                mixtopia::loops::apply (*looped, loop, false);
                auto projectFile = root.getChildFile ("Unplugged.mixtopia");
                check (project::save (*e, projectFile).isEmpty(), "project with songs on the 'drive' saved");
                check (project::getDecodedCopiesUsedBy (projectFile).contains (decoded), "the project's decoded copies are found (kept when trimming)");
                plain = looped = nullptr;
                mixtopia::destroyEdit (e);

                std::unique_ptr<te::Edit> reopened;
                auto peakOnLane = [&] (int lane)
                {
                    auto out = root.getChildFile ("lane" + juce::String (lane) + ".wav");
                    out.deleteFile();
                    auto lanes = te::getAudioTracks (*reopened);
                    lanes[0]->setMute (lane != 0);
                    lanes[1]->setMute (lane != 1);
                    te::Renderer::renderToFile ({}, out, *reopened, { te::TimePosition(), te::TimePosition::fromSeconds (14.0) },
                                                mixtopia::getAudibleTracksForRender (*reopened), true, true, {}, false);
                    std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (out), true));
                    juce::Range<float> levels[2];

                    if (reader != nullptr)
                        reader->readMaxLevels (0, reader->lengthInSamples, levels, 2);

                    return juce::jmax (levels[0].getEnd(), -levels[0].getStart(), levels[1].getEnd(), -levels[1].getStart());
                };

                {
                    auto plugged = project::read (projectFile);
                    reopened = project::createEdit (engine, plugged);
                    auto p1 = reopened != nullptr ? peakOnLane (0) : 0.0f, p2 = reopened != nullptr ? peakOnLane (1) : 0.0f;
                    check (p1 > 0.1f && p2 > 0.1f, "control, drive plugged in: both songs play (peaks " + juce::String (p1, 2) + ", " + juce::String (p2, 2) + ")");

                    if (reopened != nullptr)
                        mixtopia::destroyEdit (reopened);
                }

                auto unplugged = song.getParentDirectory().getSiblingFile ("unplugged");
                song.getParentDirectory().moveFileTo (unplugged);
                auto contents = project::read (projectFile);
                check (contents.missingFiles.size() == 1 && contents.missingButCopied.size() == 1,
                       "drive unplugged: the song is missing but has its copy (" + juce::String (contents.missingFiles.size()) + " missing, "
                         + juce::String (contents.missingButCopied.size()) + " with a copy)");

                reopened = project::createEdit (engine, contents);   // = "Open without them"

                if (reopened != nullptr)
                {
                    auto clips = te::getAudioTracks (*reopened)[1]->getClips();
                    auto* loopClip = clips.isEmpty() ? nullptr : dynamic_cast<te::AudioClipBase*> (clips.getFirst());
                    check (loopClip != nullptr && mixtopia::loops::getSongFile (*loopClip) == decoded,
                           "drive unplugged: the looped song still finds its decoded copy");
                    auto p1 = peakOnLane (0), p2 = peakOnLane (1);
                    check (p1 > 0.1f && p2 > 0.1f, "drive unplugged, opened without them: both songs play from their copies (peaks "
                                                     + juce::String (p1, 2) + ", " + juce::String (p2, 2) + ")");
                    mixtopia::destroyEdit (reopened);
                }
                else
                {
                    check (false, "drive unplugged: the project opens");
                }
            }

            // A looped 16- or 24-bit WAV plays its music (CLEANUP-AUDIT C1: the loop reader read such WAVs as 32-bit
            // float - noise, or silence once stretched): it renders exactly like a 32-bit float WAV of the same samples.
            {
                std::unique_ptr<juce::AudioFormatReader> source (juce::WavAudioFormat().createReaderFor (
                    new juce::FileInputStream (mixtopia::decodedCache::getPlayableFile (engine, audioFile)), true));
                juce::AudioBuffer<float> music (2, (int) (8.0 * 44100.0));
                music.clear();

                if (source != nullptr)
                    source->read (&music, 0, music.getNumSamples(), 0, true, true);

                source = nullptr;

                auto writeWav = [&] (const juce::File& file, const juce::AudioBuffer<float>& audio, int bits)
                {
                    using Format = juce::AudioFormatWriterOptions::SampleFormat;
                    file.deleteFile();
                    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                    auto writer = juce::WavAudioFormat().createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                                               .withSampleRate (44100.0)
                                                                               .withNumChannels (2)
                                                                               .withBitsPerSample (bits)
                                                                               .withSampleFormat (bits == 32 ? Format::floatingPoint : Format::integral));

                    if (writer != nullptr)
                        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
                };

                auto loopedRender = [&] (const juce::File& song)
                {
                    auto e = mixtopia::createEdit (engine);
                    auto c = mixtopia::insertAudioFile (*te::getAudioTracks (*e)[0], song, te::TimePosition::fromSeconds (2.0));
                    juce::AudioBuffer<float> audio;

                    if (c == nullptr)
                    {
                        mixtopia::destroyEdit (e);
                        return audio;
                    }

                    mixtopia::loops::Loop loop;
                    loop.start = 2.0;
                    loop.end = 4.0;
                    mixtopia::loops::apply (*c, loop, false);
                    auto out = root.getChildFile ("looped-render.wav");
                    out.deleteFile();
                    te::Renderer::renderToFile ({}, out, *e, { te::TimePosition(), te::TimePosition::fromSeconds (14.0) },
                                                mixtopia::getAudibleTracksForRender (*e), true, true, {}, false);
                    c = nullptr;
                    mixtopia::destroyEdit (e);
                    std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (out), true));

                    if (reader != nullptr)
                    {
                        audio.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
                        reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
                    }

                    return audio;
                };

                for (int bits : { 16, 24 })
                {
                    auto wav = root.getChildFile ("Song " + juce::String (bits) + "-bit.wav");
                    writeWav (wav, music, bits);

                    // The same samples as 32-bit float (read back from the WAV, so the values are identical).
                    juce::AudioBuffer<float> sameSamples (2, music.getNumSamples());
                    std::unique_ptr<juce::AudioFormatReader> back (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (wav), true));

                    if (back != nullptr)
                        back->read (&sameSamples, 0, sameSamples.getNumSamples(), 0, true, true);

                    back = nullptr;
                    auto floatWav = root.getChildFile ("Song " + juce::String (bits) + "-bit as float.wav");
                    writeWav (floatWav, sameSamples, 32);

                    auto fromWav = loopedRender (wav), fromFloat = loopedRender (floatWav);
                    bool same = fromWav.getNumSamples() > 0 && fromWav.getNumSamples() == fromFloat.getNumSamples()
                                  && fromWav.getNumChannels() == fromFloat.getNumChannels();

                    for (int ch = 0; same && ch < fromWav.getNumChannels(); ++ch)
                        same = std::memcmp (fromWav.getReadPointer (ch), fromFloat.getReadPointer (ch), sizeof (float) * (size_t) fromWav.getNumSamples()) == 0;

                    auto level = fromFloat.getNumSamples() > 0 ? fromFloat.getMagnitude (0, fromFloat.getNumSamples()) : 0.0f;
                    check (same && level > 0.1f, "a looped " + juce::String (bits) + "-bit WAV plays exactly like the same samples as a float WAV (peak "
                                                   + juce::String (level, 2) + ")");
                }
            }

            unsetenv ("MIXTOPIA_CACHE_DIR");
            root.deleteRecursively();
        }

        // The Edit's temp folder (decoded audio) is deleted with it.
        clip = nullptr;
        auto tempDir = edit->getTempDirectory (false);
        check (tempDir.getChildFile (mixtopia::tempOwnerFileName).existsAsFile(), "temp folder claimed by this process");
        mixtopia::destroyEdit (edit);
        check (! tempDir.exists(), "temp folder deleted with the edit");

        return allPassed;
    }
}
