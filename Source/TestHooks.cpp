#include "TestHooks.h"
#include <JuceHeader.h>
#include <thread>
#include <string_view>
#include "AppBehaviour.h"
#include "EditHelpers.h"
#include "Mpg123AudioFormat.h"
#include "Loops.h"
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
#include "AppLookAndFeel.h"
#include "SelfTests.h"

// The developer test hooks (moved out of Main.cpp in the 2026-10-23 code-health review). Each is a
// command line `Mixtopia --<hook> ...`; see CLAUDE.md "How to build" for what they do.
namespace mixtopia::testhooks
{
    namespace
    {
        bool renderTest (te::Engine& engine, juce::StringArray args)
        {
            juce::File output (args[0].unquoted());
            args.remove (0);
            auto edit = mixtopia::createEdit (engine);

            if (! mixtopia::dev::addClipsFromSpecs (*edit, args))
            {
                mixtopia::destroyEdit (edit);
                return false;
            }

            // `--print-beats <from> <to> <step>`: the tempo map's time of those beats (M6 checks).
            if (auto b = mixtopia::dev::getOptionValues (args, "--print-beats", 3); b.size() == 3)
                for (auto beat = b[0].getDoubleValue(); beat <= b[1].getDoubleValue() + 1.0e-9; beat += b[2].getDoubleValue())
                    std::cout << "beat " << beat << " time " << juce::String (edit->tempoSequence.toTime (te::BeatPosition::fromBeats (beat)).inSeconds(), 6)
                              << std::endl;

            // `--from <seconds>` starts the render part-way through, like pressing Play mid-mix.
            auto from = te::TimePosition::fromSeconds (mixtopia::dev::getOptionValues (args, "--from", 1)[0].getDoubleValue());
            auto ok = te::Renderer::renderToFile ({}, output, *edit, { from, te::TimePosition() + edit->getLength() },
                                                  mixtopia::getAudibleTracksForRender (*edit), true, true, {}, false);
            mixtopia::destroyEdit (edit);
            return ok;
        }

        // Developer test hook: `Mixtopia --export-test <out> <mp3|flac> <normalise 0|1> <specs>
        // [--tags <artist> <album> <genre> <year>] [--artwork <image>]` runs the same export as File > Export and prints
        // the measured peak and applied gain.
        bool exportTest (te::Engine& engine, juce::StringArray args)
        {
            namespace ex = mixtopia::exporter;
            juce::File output (args[0].unquoted());
            auto format = args[1] == "mp3" ? ex::Format::mp3 : ex::Format::flac;
            auto normalise = args[2] == "1";
            args.removeRange (0, 3);

            ex::Tags tags;

            if (auto values = mixtopia::dev::getOptionValues (args, "--tags", 4); values.size() == 4)
            {
                tags = { values[0].unquoted(), values[1].unquoted(), values[2].unquoted(), values[3].unquoted() };
                args.removeRange (args.indexOf ("--tags"), 5);
            }

            if (auto values = mixtopia::dev::getOptionValues (args, "--artwork", 1); values.size() == 1)
            {
                tags.artwork = juce::File (values[0].unquoted());
                args.removeRange (args.indexOf ("--artwork"), 2);
            }

            auto edit = mixtopia::createEdit (engine);
            juce::String error;
            auto ok = mixtopia::dev::addClipsFromSpecs (*edit, args);
            auto wav = ok ? ex::renderMix (*edit, error) : juce::File();

            if (wav != juce::File())
            {
                auto peak = ex::measurePeak (wav);
                auto gain = normalise ? ex::getNormalisingGain (format, peak) : 1.0f;
                std::cout << "peak " << juce::Decibels::gainToDecibels (peak) << " dBFS, gain " << juce::Decibels::gainToDecibels (gain) << " dB, loudness "
                          << ex::measureLoudness (wav) + juce::Decibels::gainToDecibels (gain) << " LUFS" << std::endl;
                error = ex::encode (wav, format, gain, output, tags);
            }

            if (error.isNotEmpty())
                std::cout << "ERROR " << error << std::endl;

            mixtopia::destroyEdit (edit);
            return error.isEmpty();
        }

        // Developer test hook: `Mixtopia --self-test <audio file>` checks behaviour that doesn't need
        // ears or a mouse. Prints PASS/FAIL lines; returns non-zero if anything failed.

        // The whole mix rendered (as --render-test does), as a fingerprint of its samples - renders are compared bit for bit.
        juce::String renderFingerprint (te::Edit& edit, const juce::File& wav)
        {
            if (edit.getLength() <= te::TimeDuration())
                return "nothing to render";

            wav.deleteFile();

            if (! te::Renderer::renderToFile ({}, wav, edit, { te::TimePosition(), te::TimePosition() + edit.getLength() },
                                              mixtopia::getAudibleTracksForRender (edit), true, true, {}, false))
                return "render failed";

            std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (wav), true));

            if (reader == nullptr)
                return "render unreadable";

            juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
            juce::MemoryBlock samples;

            for (int c = 0; c < audio.getNumChannels(); ++c)
                samples.append (audio.getReadPointer (c), sizeof (float) * (size_t) audio.getNumSamples());

            auto hash = std::hash<std::string_view>() ({ static_cast<const char*> (samples.getData()), samples.getSize() });   // (compared within one run)
            return juce::String::toHexString ((juce::int64) hash) + " (" + juce::String (audio.getNumSamples()) + " samples)";
        }

        // A project opened the way File > Open does it (songs decoded first, then the Edit built); the missing songs
        // are reported. (Not done: measuring the loudness of songs in projects saved before level matching existed -
        // that step needs its progress window.)
        std::unique_ptr<te::Edit> openProject (te::Engine& engine, const juce::File& file)
        {
            auto contents = mixtopia::project::read (file);

            if (contents.error.isNotEmpty())
            {
                std::cout << "ERROR " << contents.error << std::endl;
                return {};
            }

            for (auto& missing : contents.missingFiles)
                std::cout << "missing " << missing.getFullPathName()
                          << (contents.missingButCopied.contains (missing) ? " (plays from its decoded copy)" : "") << std::endl;

            for (auto& song : contents.filesToDecode)
                mixtopia::decodedCache::getPlayableFile (engine, song);

            return mixtopia::project::createEdit (engine, contents);
        }

        // The whole Edit as text, to compare states exactly - except Tracktion's lastSignificantChange, the clock time
        // of the latest change (a time stamp, not undoable; like the creationTime compare.py leaves out).
        juce::String stateOf (te::Edit& edit)
        {
            edit.flushState();
            auto copy = edit.state.createCopy();
            copy.removeProperty ("lastSignificantChange", nullptr);
            return copy.toXmlString();
        }

        // The first line where two states differ (for the report).
        juce::String firstDifference (const juce::String& a, const juce::String& b)
        {
            auto linesA = juce::StringArray::fromLines (a), linesB = juce::StringArray::fromLines (b);

            for (int i = 0; i < juce::jmax (linesA.size(), linesB.size()); ++i)
                if (linesA[i] != linesB[i])
                    return "line " + juce::String (i + 1) + ": " + linesA[i].trim().substring (0, 160) + "  <->  " + linesB[i].trim().substring (0, 160);

            return {};
        }

        // Lets the message thread catch up, as it does between two clicks in the app: Tracktion and Mixtopia update some
        // things asynchronously after an edit (e.g. Tracktion re-orders a lane's state after a clip is added - in the
        // same undo step), and Tracktion closes the undo step 350 ms after a change (Edit::UndoTransactionTimer). Too
        // short a pause let that timer fire in the middle of the next edit and split it into two undo steps.
        void settle()
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (450);
        }

        // One random edit, made the way the UI makes it (the same functions, one undo step each). Returns what was
        // done, or {} if it couldn't be done in this arrangement (nothing changed).
        juce::String randomEdit (te::Edit& edit, juce::Random& random)
        {
            namespace tempo = mixtopia::tempo;
            namespace chops = mixtopia::chops;
            namespace loops = mixtopia::loops;
            auto& um = edit.getUndoManager();
            std::vector<te::AudioClipBase*> clips;

            for (auto* track : te::getAudioTracks (edit))
                for (auto* c : track->getClips())
                    if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                        clips.push_back (clip);

            auto pieces = chops::listPieces (edit);

            if (clips.empty() || pieces.empty())
                return {};

            auto& clip = *clips[(size_t) random.nextInt ((int) clips.size())];
            auto& piece = pieces[(size_t) random.nextInt ((int) pieces.size())];
            auto range = tempo::getSourceRange (clip);
            auto name = clip.getName().substring (0, 20);
            auto beatsAway = [&] (double editSeconds)   // whole beats, not before 0:00, not 0
            {
                auto beat = tempo::toBeats (edit, te::TimePosition::fromSeconds (editSeconds));
                auto beats = (double) random.nextInt ({ -8, 17 });
                return beats == 0.0 || beat + beats < 0.0 ? 4.0 : beats;
            };

            switch (random.nextInt (9))
            {
                case 0:
                {
                    auto played = range.start + random.nextDouble() * (range.end - range.start);
                    um.beginNewTransaction ("Cut");
                    return chops::cut (clip, played) ? "cut " + name + " at " + juce::String (played, 3) + " s" : juce::String();
                }

                case 1:
                {
                    auto beats = beatsAway (piece.time.getStart());
                    auto lanes = te::getAudioTracks (edit);
                    auto toLane = random.nextInt (3) == 0 ? juce::jmin (random.nextInt (lanes.size() + 1), 3) : lanes.indexOf (dynamic_cast<te::AudioTrack*> (piece.clip->getTrack()));
                    um.beginNewTransaction ("Move clip");
                    auto* moved = chops::separate (*piece.clip, piece.piece);

                    if (tempo::isTempoMatched (*moved))
                        tempo::place (*moved, tempo::getContentStartBeat (*moved) + beats, tempo::getSourceRange (*moved));
                    else
                        moved->setStart (tempo::toTime (edit, tempo::toBeats (edit, moved->getPosition().getStart()) + beats), false, true);

                    edit.ensureNumberOfAudioTracks (toLane + 1);
                    moved->moveTo (*te::getAudioTracks (edit)[toLane]);
                    chops::tidy (edit);
                    return "moved piece " + piece.label + " by " + juce::String (beats) + " beats to lane " + juce::String (toLane + 1);
                }

                case 2:
                {
                    auto beats = beatsAway (piece.time.getStart());
                    um.beginNewTransaction ("Copy piece");
                    auto ok = chops::copyPiece (*piece.clip, piece.piece, *dynamic_cast<te::AudioTrack*> (piece.clip->getTrack()), beats) != nullptr;
                    chops::tidy (edit);
                    return ok ? "copied piece " + piece.label + " " + juce::String (beats) + " beats away" : juce::String();
                }

                case 3:
                {
                    juce::String status;
                    um.beginNewTransaction ("Copy piece");
                    auto ok = chops::duplicate (*piece.clip, piece.piece, status) != nullptr;
                    chops::tidy (edit);
                    return ok ? "duplicated piece " + piece.label : juce::String();
                }

                case 4:
                {
                    if (pieces.size() < 2)
                        return {};

                    um.beginNewTransaction ("Delete piece");

                    if (auto* target = chops::separate (*piece.clip, piece.piece))
                        target->removeFromParent();

                    chops::tidy (edit);
                    return "deleted piece " + piece.label;
                }

                case 5:
                {
                    auto param = (envelopes::Param) random.nextInt (envelopes::numParams);
                    auto points = envelopes::getPoints (clip.state, param);
                    auto t = range.start + random.nextDouble() * (range.end - range.start);
                    auto low = juce::jmax (envelopes::getMinValue (param), -24.0f), high = envelopes::getMaxValue (param);
                    auto value = low + random.nextFloat() * (high - low);
                    points.push_back ({ t, value });
                    um.beginNewTransaction ("Add point");
                    envelopes::setPoints (clip.state, param, points, &um);
                    return envelopes::getName (param) + " point on " + name + " at " + juce::String (t, 3) + " s: " + juce::String (value, 1);
                }

                case 6:
                {
                    if (loops::getLoop (clip) && random.nextBool())
                    {
                        um.beginNewTransaction ("Remove the loop");
                        loops::apply (clip, std::nullopt, false);
                        return "removed the loop on " + name;
                    }

                    auto played = range.start + random.nextDouble() * (range.end - range.start);
                    auto loop = loops::makeDefault (clip, loops::playedToSong (loops::getLoop (clip), played), true);
                    loop.repeats = 1 + random.nextInt (4);
                    loop.crossfadeMs = std::array<double, 3> { 0.0, 4.0, 20.0 }[(size_t) random.nextInt (3)];
                    loop.copyLines = random.nextBool();
                    um.beginNewTransaction ("Repeat part of a song");
                    loops::apply (clip, loop, false);
                    return "loop on " + name + ": " + juce::String (loop.start, 3) + "-" + juce::String (loop.end, 3) + " s x" + juce::String (loop.repeats)
                             + ", " + juce::String (loop.crossfadeMs) + " ms" + (loop.copyLines ? ", lines copied" : "");
                }

                case 7:
                {
                    auto bars = juce::jmax (2, (int) (tempo::toBeats (edit, te::TimePosition() + edit.getLength()) / tempo::beatsPerBar));
                    auto bar = 2 + random.nextInt (bars);
                    auto bpm = 120.0 + random.nextInt (31);
                    um.beginNewTransaction ("Change tempo");
                    tempo::setTempoPlan (edit, tempo::withTempoAt (tempo::getTempoPlan (edit), (bar - 1) * tempo::beatsPerBar, bpm));
                    return "tempo point: bar " + juce::String (bar) + " at " + juce::String (bpm) + " BPM";
                }

                default:
                {
                    for (auto* c : clips)
                        if (chops::nextJoinable (*c) != nullptr)
                        {
                            um.beginNewTransaction ("Join pieces");
                            chops::joinWithNext (*c);
                            chops::tidy (edit);
                            return "joined " + c->getName().substring (0, 20) + " with the next piece";
                        }

                    return {};
                }
            }
        }

        // Developer hook: `Mixtopia --stress <seed> <steps> <outdir> <specs>` (CLEANUP-AUDIT check 9): builds the
        // arrangement, makes <steps> random edits (as above), recording the Edit's state and a render after each;
        // then undoes them all (each state and render must come back exactly), redoes them all (the same), and saves
        // and reopens the result (its render must equal the last one). Use --stretch rb3 (Signalsmith renders aren't
        // bit-repeatable). Prints every step and every difference; exit 0 = all the same.
        bool stressTest (te::Engine& engine, juce::StringArray args)
        {
            juce::Random random (args[0].getLargeIntValue());
            auto steps = args[1].getIntValue();
            juce::File outDir (args[2].unquoted());
            args.removeRange (0, 3);
            outDir.createDirectory();
            auto wav = outDir.getChildFile ("stress-render.wav");
            auto edit = mixtopia::createEdit (engine);

            if (! mixtopia::dev::addClipsFromSpecs (*edit, args))
            {
                std::cout << "ERROR the specs couldn't be built" << std::endl;
                mixtopia::destroyEdit (edit);
                return false;
            }

            auto& um = edit->getUndoManager();
            renderFingerprint (*edit, wav);   // (a lane's first render fills in its default properties, e.g. soloIsolate="0" -
            settle();                         // not undoable, no effect on the sound: every state is recorded after a render)
            std::vector<juce::String> states { stateOf (*edit) }, renders { renderFingerprint (*edit, wav) }, done;
            auto ok = true;
            std::cout << "start: " << renders.back() << std::endl;

            for (int attempt = 0; (int) done.size() < steps && attempt < steps * 20; ++attempt)
            {
                auto undoSteps = um.getUndoDescriptions().size();   // (Tracktion also starts new undo steps on a timer)
                auto what = randomEdit (*edit, random);
                auto stepsRightAfter = um.getUndoDescriptions().size();
                auto stateRightAfter = stateOf (*edit);
                settle();
                auto state = stateOf (*edit);

                if (state == states.back())
                    continue;   // (nothing changed: not a step)

                if (what.isEmpty() || um.getUndoDescriptions().size() != undoSteps + 1)
                {
                    std::cout << "DIFFERENT the Edit changed without exactly one new undo step (" << um.getUndoDescriptions().size() - undoSteps << ", " << (what.isEmpty() ? "an edit that reported nothing done" : what) << "): "
                              << firstDifference (states.back(), state) << std::endl;
                    std::cout << "  undo steps right after the edit: " << stepsRightAfter - undoSteps << "; newest: "
                              << um.getUndoDescriptions().joinIntoString (" | ") << std::endl;
                    if (stateRightAfter != state)
                        std::cout << "  changed afterwards (message thread): " << firstDifference (stateRightAfter, state) << std::endl;
                    ok = false;
                    break;
                }

                done.push_back (what);
                renders.push_back (renderFingerprint (*edit, wav));
                states.push_back (stateOf (*edit));   // (after the render: it fills in a new lane's defaults, see above)
                std::cout << "step " << done.size() << ": " << what << " -> " << renders.back() << std::endl;
            }

            auto compare = [&] (const juce::String& when, size_t k)
            {
                settle();
                auto render = renderFingerprint (*edit, wav);
                auto state = stateOf (*edit);
                auto same = state == states[k] && render == renders[k];
                std::cout << (same ? "SAME      " : "DIFFERENT ") << when << " = after step " << k << (render == renders[k] ? "" : ", render " + render + " vs " + renders[k])
                          << (state == states[k] ? juce::String() : ", state " + firstDifference (states[k], state)) << std::endl;
                ok = ok && same;
            };

            for (auto k = done.size(); k-- > 0;)
            {
                if (! um.undo())
                {
                    std::cout << "DIFFERENT couldn't undo step " << k + 1 << std::endl;
                    ok = false;
                    break;
                }

                compare ("undo of step " + juce::String ((int) k + 1), k);
            }

            for (size_t k = 1; ok && k <= done.size(); ++k)
            {
                if (! um.redo())
                {
                    std::cout << "DIFFERENT couldn't redo step " << (int) k << std::endl;
                    ok = false;
                    break;
                }

                compare ("redo of step " + juce::String ((int) k), k);
            }

            auto projectFile = outDir.getChildFile ("stress.mixtopia");

            if (auto error = mixtopia::project::save (*edit, projectFile); error.isNotEmpty())
            {
                std::cout << "ERROR " << error << std::endl;
                ok = false;
            }

            mixtopia::destroyEdit (edit);

            if (auto reopened = openProject (engine, projectFile))
            {
                auto render = renderFingerprint (*reopened, wav);
                std::cout << (render == renders.back() ? "SAME      " : "DIFFERENT ") << "saved and reopened = after step " << done.size()
                          << (render == renders.back() ? "" : ", render " + render + " vs " + renders.back()) << std::endl;
                ok = ok && render == renders.back();
                mixtopia::destroyEdit (reopened);
            }
            else
            {
                std::cout << "ERROR the saved project didn't open" << std::endl;
                ok = false;
            }

            wav.deleteFile();
            std::cout << (ok ? "STRESS: ALL SAME" : "STRESS: SOMETHING DIFFERS") << " (" << done.size() << " of " << steps << " steps made)" << std::endl;
            return ok && (int) done.size() == steps;
        }
    }

    bool isHeadless (const juce::String& commandLine)
    {
        for (auto hook : { "--self-test", "--render-test", "--export-test", "--beatgrid-test", "--project-check", "--placement-check",
                           "--key-test", "--loudness-test", "--tags-test", "--library-test", "--loop-dump", "--cache-check",
                           "--render-project", "--stress" })
            if (commandLine.contains (hook))
                return true;

        return false;
    }

    std::optional<int> run (te::Engine& engine, juce::StringArray args)
    {
        // Developer test hook: `Mixtopia --self-test <audio file>` (see selfTest()).
        if (args[0] == "--self-test" && args.size() == 2)
        {
            return mixtopia::selfTest (engine, args[1].unquoted()) ? 0 : 1;
        }

        // Developer hook: `Mixtopia --save-test <project.mixtopia> <specs>` saves an arrangement.
        if (args[0] == "--save-test" && args.size() >= 3)
        {
            auto edit = mixtopia::createEdit (engine);
            auto ok = mixtopia::dev::addClipsFromSpecs (*edit, juce::StringArray (args.begin() + 2, args.size() - 2))
                        && mixtopia::project::save (*edit, juce::File (args[1].unquoted())).isEmpty();
            mixtopia::destroyEdit (edit);
            return ok ? 0 : 1;
        }

        // Developer hook: `Mixtopia --beatgrid-test <audio files...>` prints each file's beat grid.
        if (args[0] == "--beatgrid-test" && args.size() >= 2)
        {
            for (int i = 1; i < args.size(); ++i)
            {
                juce::File file (args[i].unquoted());
                std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (engine, file));
                auto start = juce::Time::getMillisecondCounterHiRes();
                auto grid = reader != nullptr ? mixtopia::beatgrid::analyse (*reader) : mixtopia::beatgrid::Result();
                std::cout << file.getFileName() << "\tvalid " << grid.valid << "\tbpm " << grid.bpm << "\tmeasured " << grid.measuredBpm
                          << "\tfirst " << grid.firstBeat << "\tdownbeat " << grid.downbeat << "\tdrift " << grid.driftMs
                          << "\tms " << (juce::Time::getMillisecondCounterHiRes() - start) << std::endl;
            }

            return 0;
        }

        // Developer hook: `Mixtopia --project-check <project.mixtopia>` opens a project (never saves),
        // prints where each song and its beat 1 sit, then tries on the first lane's song (a) a drag
        // that aims its beat 1 at bar 1 and (b) "Use the detected BPM and beat 1".
        if (args[0] == "--project-check" && args.size() == 2)
        {
            namespace tempo = mixtopia::tempo;
            juce::File projectFile (args[1].unquoted());
            auto contents = mixtopia::project::read (projectFile);
            auto describe = [] (te::AudioClipBase& clip)
            {
                auto beatOne = tempo::sourceToEditSeconds (clip, tempo::getDownbeat (clip));
                auto bars = beatOne / (tempo::beatsPerBar * 60.0 / tempo::getProjectBpm (clip.edit));
                return clip.getName().substring (0, 30) + ": start " + juce::String (clip.getPosition().getStart().inSeconds(), 3)
                         + " s, trimmed " + juce::String (clip.getPosition().getOffset().inSeconds(), 3) + " s, beat 1 at "
                         + juce::String (beatOne, 3) + " s = bar " + juce::String (bars + 1.0, 3);
            };

            for (int pass = 0; pass < 2 && contents.error.isEmpty(); ++pass)
            {
                contents = mixtopia::project::read (projectFile);   // fresh: (a) mustn't carry over into (b)
                auto edit = mixtopia::project::createEdit (engine, contents);
                std::cout << (pass == 0 ? "as saved:" : "fresh from the file again, for (b):") << std::endl;

                for (auto* track : te::getAudioTracks (*edit))
                    for (auto* c : track->getClips())
                        if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && pass == 0)
                            std::cout << "  " << describe (*clip) << std::endl;

                auto* first = dynamic_cast<te::AudioClipBase*> (te::getAudioTracks (*edit)[0]->getClips().getFirst());

                if (first != nullptr && pass == 0)
                {
                    auto beatOneOffset = tempo::sourceToEditSeconds (*first, tempo::getDownbeat (*first)) - tempo::getContentStart (*first).inSeconds();
                    auto moved = tempo::placeKeepingTheGrid (*first, te::TimePosition::fromSeconds (-beatOneOffset), tempo::getSourceRange (*first),
                                                             mixtopia::analysis::getMayCutUntil (*first));
                    std::cout << "(a) dragged to bar 1: " << describe (*first) << (moved ? "  [moved a bar later]" : "") << std::endl;
                }
                else if (first != nullptr)
                {
                    if (auto info = mixtopia::analysis::analyse (engine, first->getOriginalFile()))
                    {
                        std::cout << "  analysis: bpm " << info->grid.bpm << ", beat 1 at " << info->grid.downbeat << " s, sound starts "
                                  << info->soundStart << " s, may trim until " << info->silentUntil << " s; may cut at 0:00 until "
                                  << mixtopia::analysis::getMayCutUntil (*first) << " s" << std::endl;
                        mixtopia::analysis::reapplyDetectedGrid (*first, *info, true);
                    }

                    std::cout << "(b) use the detected BPM and beat 1: " << describe (*first) << std::endl;
                }

                mixtopia::destroyEdit (edit);
            }

            return 0;
        }

        // Developer hook: `Mixtopia --placement-check <audio files...>` runs the song-start tests on
        // real songs without a screen: drop early and late in bar 1, drag far past the left edge,
        // restore the start, and a drop with auto-trim off.
        if (args[0] == "--placement-check" && args.size() >= 2)
        {
            namespace tempo = mixtopia::tempo;
            namespace analysis = mixtopia::analysis;

            for (int i = 1; i < args.size(); ++i)
            {
                juce::File file (args[i].unquoted());
                auto playable = mixtopia::decodedCache::getPlayableFile (engine, file);
                auto info = analysis::analyse (engine, playable);

                if (! info || ! info->grid.valid)
                {
                    std::cout << file.getFileName() << ": no beat grid" << std::endl;
                    continue;
                }

                auto edit = mixtopia::createEdit (engine);
                auto bar = tempo::beatsPerBar * 60.0 / tempo::getProjectBpm (*edit);
                auto describe = [&] (te::AudioClipBase& clip, bool moved)
                {
                    auto beatOne = tempo::sourceToEditSeconds (clip, tempo::getDownbeat (clip));
                    auto bars = beatOne / bar;
                    auto onBar = std::abs (bars - std::round (bars)) * bar < 0.001;
                    return "beat 1 " + (onBar ? "on bar " + juce::String ((int) std::round (bars) + 1) : "OFF the bars (" + juce::String (bars + 1.0, 3) + ")")
                             + ", starts " + juce::String (clip.getPosition().getStart().inSeconds(), 3) + " s, trimmed "
                             + juce::String (clip.getPosition().getOffset().inSeconds() / tempo::getStretchRatio (clip), 3) + " s"
                             + (moved ? ", MOVED for a pickup" : "");
                };

                auto add = [&] (int lane, double dropSeconds, bool autoTrim)
                {
                    edit->ensureNumberOfAudioTracks (lane + 1);
                    auto clip = mixtopia::insertAudioFile (*te::getAudioTracks (*edit)[lane], file, te::TimePosition::fromSeconds (dropSeconds));
                    auto moved = analysis::prepareNewClip (*clip, *info, te::TimePosition::fromSeconds (dropSeconds), true, autoTrim);
                    return std::make_pair (clip, moved);
                };

                std::cout << file.getFileName() << "  (sound starts " << info->soundStart << " s, beat 1 at " << info->grid.downbeat << " s)" << std::endl;
                auto [early, m1] = add (0, 0.3, true);
                std::cout << "  drop early in bar 1:   " << describe (*early, m1) << std::endl;
                auto [late, m2] = add (1, 1.5, true);
                std::cout << "  drop late in bar 1:    " << describe (*late, m2) << std::endl;
                auto aim = te::TimePosition::fromSeconds (-5 * bar) - te::TimeDuration::fromSeconds (tempo::getDownbeat (*late) * tempo::getStretchRatio (*late));
                auto m3 = tempo::placeKeepingTheGrid (*late, aim, tempo::getSourceRange (*late), analysis::getMayCutUntil (*late));
                std::cout << "  dragged far left:      " << describe (*late, m3) << std::endl;
                auto [mid, m4] = add (2, 30.0, true);
                auto before = describe (*mid, m4);
                analysis::restoreTrimmedStart (*mid);
                std::cout << "  mid-mix, restored:     " << before << "  ->  " << describe (*mid, false) << std::endl;
                auto [untrimmed, m5] = add (3, 0.3, false);
                std::cout << "  auto-trim off, bar 1:  " << describe (*untrimmed, m5) << std::endl;
                early = nullptr; late = nullptr; mid = nullptr; untrimmed = nullptr;
                mixtopia::destroyEdit (edit);
            }

            return 0;
        }

        // Developer hook: `Mixtopia --library-test <audio files...>` adds them to a temporary library,
        // waits for the background analysis and prints each song's state.
        if (args[0] == "--library-test" && args.size() >= 2)
        {
            auto storage = juce::File::createTempFile (".xml");
            {
                mixtopia::library::Library library (engine, storage);
                juce::Array<juce::File> files;

                for (int i = 1; i < args.size(); ++i)
                    files.add (juce::File (args[i].unquoted()));

                library.add (files);
                library.waitUntilAnalysed (300000);

                for (auto& s : library.getSongs())
                    std::cout << juce::File (s.path).getFileName() << "\tmissing " << (int) s.missing << " failed " << (int) s.failed
                              << " exists " << (int) juce::File (s.path).existsAsFile() << "\tbpm " << s.bpm << " key " << s.key
                              << " length " << s.lengthSeconds << std::endl;
            }
            storage.deleteFile();
            return 0;
        }

        // Developer hook: `Mixtopia --tags-test <audio files...>` prints each file's title and artist.
        if (args[0] == "--tags-test" && args.size() >= 2)
        {
            for (int i = 1; i < args.size(); ++i)
            {
                juce::File f (args[i].unquoted());
                auto t = mixtopia::tags::read (f);
                std::cout << f.getFileName() << "\ttitle: " << t.title << "\tartist: " << t.artist << std::endl;
            }

            return 0;
        }

        // Developer hook: `Mixtopia --loudness-test <audio files...>` prints each file's loudness.
        if (args[0] == "--loudness-test" && args.size() >= 2)
        {
            for (int i = 1; i < args.size(); ++i)
            {
                juce::File file (args[i].unquoted());
                std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (engine, file));
                auto start = juce::Time::getMillisecondCounterHiRes();
                auto lufs = reader != nullptr ? mixtopia::loudness::measure (*reader) : -std::numeric_limits<double>::infinity();
                std::cout << file.getFileName() << "\t" << juce::String (lufs, 1) << " LUFS\tmatch +"
                          << juce::String (mixtopia::loudness::matchGainDb (lufs), 1) << " dB\t"
                          << (int) (juce::Time::getMillisecondCounterHiRes() - start) << " ms" << std::endl;
            }

            return 0;
        }

        // Developer hook: `Mixtopia --cache-check <audio files...>`: the library analyses and draws
        // songs straight from their files, the timeline from the decoded copies - both must give the
        // same beat grid, sound start, key, loudness and waveform. Run with MIXTOPIA_CACHE_DIR set
        // (the decoded copies go there).
        if (args[0] == "--cache-check" && args.size() >= 2)
        {
            auto allSame = true;

            for (int i = 1; i < args.size(); ++i)
            {
                juce::File file (args[i].unquoted());
                auto decoded = mixtopia::decodedCache::getPlayableFile (engine, file);
                auto start = juce::Time::getMillisecondCounterHiRes();
                std::unique_ptr<juce::AudioFormatReader> direct (te::AudioFileUtils::createReaderFor (engine, file));
                std::unique_ptr<juce::AudioFormatReader> copy (te::AudioFileUtils::createReaderFor (engine, decoded));

                if (direct == nullptr || copy == nullptr)
                {
                    std::cout << file.getFileName() << "\tUNREADABLE" << std::endl;
                    allSame = false;
                    continue;
                }

                auto a = mixtopia::analysis::analyseReader (*direct);
                auto ms = (int) (juce::Time::getMillisecondCounterHiRes() - start);
                auto b = mixtopia::analysis::analyseReader (*copy);
                auto wa = mixtopia::LoudnessOverview::analyse (*direct, [] { return false; });
                auto wb = mixtopia::LoudnessOverview::analyse (*copy, [] { return false; });
                auto same = direct->lengthInSamples == copy->lengthInSamples
                              && a.grid.valid == b.grid.valid && a.grid.bpm == b.grid.bpm && a.grid.downbeat == b.grid.downbeat
                              && a.grid.firstBeat == b.grid.firstBeat && a.soundStart == b.soundStart && a.silentUntil == b.silentUntil
                              && a.key == b.key && a.loudness == b.loudness
                              && wa.energies == wb.energies && wa.bassPeaks == wb.bassPeaks && wa.fullPeaks == wb.fullPeaks;
                auto keyOk = mixtopia::decodedCache::getSongKey (decoded) == mixtopia::decodedCache::getCacheKey (file);
                allSame = allSame && same && keyOk;
                std::cout << file.getFileName() << "\t" << (same ? "SAME" : "DIFFERENT") << (keyOk ? "" : " (KEY MISMATCH)")
                          << "\tBPM " << a.grid.bpm << "\tbeat 1 " << a.grid.downbeat << "\tkey " << a.key
                          << "\t" << a.loudness << " LUFS\tfrom the file in " << ms << " ms" << std::endl;
            }

            std::cout << (allSame ? "CACHE CHECK: ALL SAME" : "CACHE CHECK: DIFFERENCES") << std::endl;
            return allSame ? 0 : 1;
        }

        // Developer hook: `Mixtopia --key-test <audio files...>` prints each file's detected key.
        if (args[0] == "--key-test" && args.size() >= 2)
        {
            for (int i = 1; i < args.size(); ++i)
            {
                juce::File file (args[i].unquoted());
                std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (engine, file));
                auto start = juce::Time::getMillisecondCounterHiRes();
                auto key = reader != nullptr ? mixtopia::key::analyse (*reader) : juce::String();
                std::cout << file.getFileName() << "\t" << key << "\t" << mixtopia::key::toMusicalName (key) << "\t"
                          << (int) (juce::Time::getMillisecondCounterHiRes() - start) << " ms" << std::endl;
            }

            return 0;
        }

        if (args[0] == "--export-test" && args.size() >= 5)
        {
            args.remove (0);
            return exportTest (engine, args) ? 0 : 1;
        }

        // Developer test hook: `Mixtopia --render-test <out.wav> <specs>` (see DevSpecs.h) renders
        // an arrangement through the same Edit/graph used for playback, so the output can be
        // compared with the inputs.
        // Developer hook: `Mixtopia --loop-dump <song> <startSec> <endSec> <repeats> <crossfadeMs> <outdir>`
        // reads a looped song through both of Mixtopia's readers - the direct one that playback and
        // export use (via Tracktion's file cache, as in playback, in 512-sample blocks) and the
        // .mixloop format (song analysis) - and writes them as direct.wav and format.wav (32-bit
        // float), for scripts/regression/loop-proof.py to compare with its own copy. Prints the
        // decoded WAV the loop reads.
        if (args[0] == "--loop-dump" && args.size() >= 7)
        {
            auto wav = mixtopia::decodedCache::getPlayableFile (engine, juce::File (args[1].unquoted()));
            mixtopia::loops::Loop loop;
            loop.start = args[2].getDoubleValue();
            loop.end = args[3].getDoubleValue();
            loop.repeats = args[4].getIntValue();
            loop.crossfadeMs = args[5].getDoubleValue();
            auto out = juce::File (args[6].unquoted());
            out.createDirectory();
            auto loopFile = mixtopia::loops::getLoopFile (wav, loop);
            std::cout << "wav " << wav.getFullPathName() << std::endl;

            auto write = [] (const juce::File& file, juce::AudioBuffer<float>& audio, double rate)
            {
                file.deleteFile();
                std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
                auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                                  .withSampleRate (rate)
                                                                                  .withNumChannels (audio.getNumChannels())
                                                                                  .withBitsPerSample (32)
                                                                                  .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
            };

            // Direct (playback / export path).
            te::AudioFile af (engine, loopFile);
            auto info = af.getInfo();
            auto reader = engine.getAudioFileManager().cache.createReader (af);
            juce::AudioBuffer<float> direct (info.numChannels, (int) info.lengthInSamples);
            direct.clear();

            for (int pos = 0; pos < direct.getNumSamples(); pos += 512)
            {
                auto n = juce::jmin (512, direct.getNumSamples() - pos);
                juce::HeapBlock<float*> chans (direct.getNumChannels());

                for (int c = 0; c < direct.getNumChannels(); ++c)
                    chans[c] = direct.getWritePointer (c, pos);

                reader->setReadPosition (pos);

                if (! reader->readSamples (reinterpret_cast<int* const*> (chans.get()), direct.getNumChannels(), 0, n, -1))
                    std::cout << "direct read failed at " << pos << std::endl;
            }

            write (out.getChildFile ("direct.wav"), direct, info.sampleRate);

            // Format (analysis path).
            std::unique_ptr<juce::AudioFormatReader> formatReader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (loopFile));
            juce::AudioBuffer<float> viaFormat ((int) formatReader->numChannels, (int) formatReader->lengthInSamples);
            formatReader->read (&viaFormat, 0, viaFormat.getNumSamples(), 0, true, true);
            write (out.getChildFile ("format.wav"), viaFormat, formatReader->sampleRate);
            std::cout << "length " << info.lengthInSamples << " samples" << std::endl;
            return 0;
        }

        if (args[0] == "--render-test" && args.size() >= 3)
        {
            args.remove (0);
            return renderTest (engine, args) ? 0 : 1;
        }

        // Developer hook: `Mixtopia --render-project <project.mixtopia> <out.wav>` (CLEANUP-AUDIT check 8): opens a saved
        // project as File > Open does and renders it like --render-test, so a render from specs can be compared with
        // the same arrangement saved and reopened.
        if (args[0] == "--render-project" && args.size() == 3)
        {
            auto edit = openProject (engine, juce::File (args[1].unquoted()));

            if (edit == nullptr)
                return 1;

            juce::File output (args[2].unquoted());
            output.deleteFile();
            auto ok = te::Renderer::renderToFile ({}, output, *edit, { te::TimePosition(), te::TimePosition() + edit->getLength() },
                                                  mixtopia::getAudibleTracksForRender (*edit), true, true, {}, false);
            mixtopia::destroyEdit (edit);
            return ok ? 0 : 1;
        }

        if (args[0] == "--stress" && args.size() >= 5)
        {
            args.remove (0);
            return stressTest (engine, args) ? 0 : 1;
        }

        return {};
    }
}
