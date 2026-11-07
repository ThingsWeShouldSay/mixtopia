#pragma once

#include <JuceHeader.h>
#include "ClipTempo.h"
#include "TempoRamps.h"
#include "EditHelpers.h"
#include "MasterLimiter.h"
#include "ClipEnvelopes.h"
#include "Loops.h"
#include "LevelMatch.h"
#include "Chops.h"

namespace te = tracktion;

// Developer/test helpers: build an arrangement from command-line style specs.
//
//   --tempo <bpm>                      set the project tempo (before or after clips)
//   --stretch rb3|rbperc|ss            time-stretch quality (default: Signalsmith)
//   --click                            metronome on
//   --nolimit                          remove the master limiter (for comparisons)
//   --match                            level matching on (each song's loudness measured)
//   --env <lane> <volume|bass|mids|treble> <sourceSec:dB,...>   envelope points on the lane's first clip
//   --loop <lane> <start> <end> <repeats> <crossfadeMs> [once|copy]   a loop on the lane's first clip (song s)
//   --cut <lane> <playedSec>           chopping: a cut in the lane's piece playing there (song seconds unlooped)
//   --move-piece <lane> <n> <beats> [<toLane>]   the lane's n-th piece (time order, from 1) moved by whole beats
//   --copy-piece <lane> <n> <beats> [<toLane>]   a copy of it, that many beats later
//   --join                             pieces that follow on join up, crossfades set (also done after each move/copy)
//   --mute <lane> / --solo <lane>      (lanes count from 1)
//   <file>[@start][#bpm[#downbeat]]    a clip on the next lane; with a BPM it is tempo-matched
//                                      and `start` is where its downbeat grid lands
//
// Other arguments are left for the caller.
namespace mixtopia::dev
{
    inline bool addClipsFromSpecs (te::Edit& edit, const juce::StringArray& args)
    {
        int lane = 0;
        bool match = false;

        for (int i = 0; i < args.size(); ++i)
        {
            auto arg = args[i].unquoted();

            if (arg == "--tempo")
            {
                tempo::setProjectBpm (edit, args[++i].getDoubleValue());
                continue;
            }

            // --ramp <bar> <bpm> <bars>: from the start of <bar> (counted from 1) the tempo glides
            // to <bpm> over <bars> bars, then holds: two tempo points, as placed in the app. Songs
            // already added stay on their bars.
            if (arg == "--ramp")
            {
                auto startBeat = (args[i + 1].getDoubleValue() - 1.0) * tempo::beatsPerBar;
                auto toBpm = args[i + 2].getDoubleValue();
                auto endBeat = startBeat + args[i + 3].getDoubleValue() * tempo::beatsPerBar;
                i += 3;
                auto plan = tempo::getTempoPlan (edit);
                plan = tempo::withTempoAt (plan, startBeat, edit.tempoSequence.getBpmAt (tempo::toTime (edit, startBeat)));
                plan = tempo::withTempoAt (plan, endBeat, toBpm);
                tempo::setTempoPlan (edit, plan);
                continue;
            }

            // GUI only (MainComponent): --library-add <files...>, --preview <file>.
            if (arg == "--library-add" || arg == "--preview")
            {
                while (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    ++i;

                continue;
            }

            if (arg == "--stretch")
            {
                auto name = args[++i];
                tempo::setStretchMode (edit, name == "rbperc" ? te::TimeStretcher::rubberbandPercussive
                                           : name == "ss"     ? te::TimeStretcher::signalsmithDefault
                                                              : te::TimeStretcher::rubberbandMelodic);
                continue;
            }

            if (arg == "--click")
            {
                edit.clickTrackEnabled = true;
                continue;
            }

            // --env <lane> <volume|bass|mids|treble> <sourceSeconds:dB,...> on the lane's first clip
            if (arg == "--env")
            {
                auto* track = te::getAudioTracks (edit)[args[i + 1].getIntValue() - 1];
                auto paramName = args[i + 2];
                auto spec = juce::StringArray::fromTokens (args[i + 3], ",", {});
                i += 3;

                auto* clip = track != nullptr ? dynamic_cast<te::AudioClipBase*> (track->getClips().getFirst()) : nullptr;

                if (clip == nullptr)
                    return false;

                std::vector<envelopes::Point> points;

                for (auto& p : spec)
                    points.push_back ({ p.upToFirstOccurrenceOf (":", false, false).getDoubleValue(),
                                        p.fromFirstOccurrenceOf (":", false, false).getFloatValue() });

                for (int p = 0; p < envelopes::numParams; ++p)
                    if (envelopes::getId ((envelopes::Param) p) == paramName)
                        envelopes::setPoints (clip->state, (envelopes::Param) p, points, nullptr);

                continue;
            }

            // --loop <lane> <startSec> <endSec> <repeats> <crossfadeMs> [once|copy]: a loop on the lane's
            // first clip (song seconds), applied as in the app. --env before it is in song seconds
            // and moves with the loop, as when a loop is added in the app.
            if (arg == "--loop")
            {
                auto* track = te::getAudioTracks (edit)[args[i + 1].getIntValue() - 1];
                auto* clip = track != nullptr ? dynamic_cast<te::AudioClipBase*> (track->getClips().getFirst()) : nullptr;

                if (clip == nullptr)
                    return false;

                loops::Loop loop;
                loop.start = args[i + 2].getDoubleValue();
                loop.end = args[i + 3].getDoubleValue();
                loop.repeats = args[i + 4].getIntValue();
                loop.crossfadeMs = args[i + 5].getDoubleValue();
                i += 5;

                if (i + 1 < args.size() && (args[i + 1] == "copy" || args[i + 1] == "once"))
                    loop.copyLines = args[++i] == "copy";

                loops::apply (*clip, loop, false);
                continue;
            }

            if (arg == "--cut" || arg == "--move-piece" || arg == "--copy-piece")
            {
                auto* track = te::getAudioTracks (edit)[args[i + 1].getIntValue() - 1];

                if (track == nullptr)
                    return false;

                if (arg == "--cut")
                {
                    auto played = args[i + 2].getDoubleValue();
                    i += 2;
                    auto done = false;

                    for (auto* c : track->getClips())
                        if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && ! done)
                        {
                            auto range = tempo::getSourceRange (*clip);

                            if (played > range.start && played < range.end)
                                done = chops::cut (*clip, played);
                        }

                    if (! done)
                        return false;

                    continue;
                }

                // The n-th piece on the lane in time order.
                auto n = args[i + 2].getIntValue();
                auto beats = args[i + 3].getDoubleValue();
                i += 3;
                auto* to = track;

                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--") && ! args[i + 1].containsChar ('/'))
                {
                    edit.ensureNumberOfAudioTracks (args[i + 1].getIntValue());
                    to = te::getAudioTracks (edit)[args[++i].getIntValue() - 1];
                }

                te::AudioClipBase* found = nullptr;
                int piece = 0, count = 0;

                for (auto& l : chops::listPieces (edit))
                    if (l.clip->getTrack() == track && ++count == n)
                    {
                        found = l.clip;
                        piece = l.piece;
                    }

                if (found == nullptr)
                    return false;

                te::AudioClipBase* moved = nullptr;

                if (arg == "--copy-piece")
                {
                    moved = chops::copyPiece (*found, piece, *to, beats);
                }
                else
                {
                    moved = chops::separate (*found, piece);

                    if (tempo::isTempoMatched (*moved))
                        tempo::place (*moved, tempo::getContentStartBeat (*moved) + beats, tempo::getSourceRange (*moved));
                    else
                        moved->setStart (tempo::toTime (edit, tempo::toBeats (edit, moved->getPosition().getStart()) + beats), false, true);

                    if (to != track)
                        moved->moveTo (*to);
                }

                if (moved == nullptr)
                    return false;

                chops::tidy (edit);   // (as after a drag: joins, crossfades)
                continue;
            }

            // --trim-start <lane> <playedSec>: the lane's first clip starts that far into its song (as a trim).
            if (arg == "--trim-start")
            {
                auto* track = te::getAudioTracks (edit)[args[i + 1].getIntValue() - 1];
                auto* clip = track != nullptr ? dynamic_cast<te::AudioClipBase*> (track->getClips().getFirst()) : nullptr;
                auto played = args[i + 2].getDoubleValue();
                i += 2;

                if (clip == nullptr || ! tempo::isTempoMatched (*clip))
                    return false;

                tempo::place (*clip, tempo::getContentStartBeat (*clip), { played, tempo::getSourceRange (*clip).end });
                continue;
            }

            if (arg == "--join")
            {
                chops::tidy (edit);
                continue;
            }

            if (arg == "--match")
            {
                match = true;
                continue;
            }

            if (arg == "--nolimit")
            {
                for (auto* plugin : edit.getMasterPluginList())
                    if (dynamic_cast<MasterLimiterPlugin*> (plugin) != nullptr)
                        plugin->deleteFromParent();

                continue;
            }

            if (arg == "--mute" || arg == "--solo")
            {
                auto laneIndex = args[++i].getIntValue() - 1;
                edit.ensureNumberOfAudioTracks (laneIndex + 1);
                auto* track = te::getAudioTracks (edit)[laneIndex];
                arg == "--mute" ? track->setMute (true) : track->setSolo (true);
                edit.updateMuteSoloStatuses();   // normally happens asynchronously
                continue;
            }

            if (arg.startsWith ("--"))
            {
                // Caller's option and its values.
                while (i + 1 < args.size() && ! args[i + 1].startsWith ("--") && ! args[i + 1].containsChar ('/'))
                    ++i;

                continue;
            }

            auto tempoSpec = arg.fromFirstOccurrenceOf ("#", false, false);
            auto placeSpec = arg.upToFirstOccurrenceOf ("#", false, false);
            auto start = placeSpec.containsChar ('@') ? placeSpec.fromLastOccurrenceOf ("@", false, false).getDoubleValue() : 0.0;
            juce::File input (placeSpec.upToLastOccurrenceOf ("@", false, false));

            edit.ensureNumberOfAudioTracks (++lane);
            auto clip = insertAudioFile (*te::getAudioTracks (edit)[lane - 1], input, te::TimePosition::fromSeconds (start));

            if (clip == nullptr)
                return false;

            if (tempoSpec.isNotEmpty())
            {
                auto bpm = tempoSpec.upToFirstOccurrenceOf ("#", false, false).getDoubleValue();
                auto downbeat = tempoSpec.fromFirstOccurrenceOf ("#", false, false).getDoubleValue();
                auto whole = tempo::SourceRange { 0.0, tempo::getSourceLength (*clip) };
                tempo::apply (*clip, bpm, downbeat, whole,
                              tempo::contentStartFor (edit, bpm, tempo::beatPhase (bpm, downbeat), te::TimePosition::fromSeconds (start)));
            }

        }

        if (match)
        {
            edit.state.setProperty (levelmatch::projectMatchProperty, 1, nullptr);

            for (auto* track : te::getAudioTracks (edit))
                for (auto* c : track->getClips())
                    if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                        if (std::unique_ptr<juce::AudioFormatReader> reader { edit.engine.getAudioFileFormatManager()
                                                                                .readFormatManager.createReaderFor (clip->getOriginalFile()) })
                            clip->state.setProperty (levelmatch::loudnessProperty, loudness::measure (*reader), nullptr);
        }

        edit.getUndoManager().clearUndoHistory();
        return true;
    }

    // Value after `option` in args (e.g. "--select 2" -> "2"), or {}.
    inline juce::StringArray getOptionValues (const juce::StringArray& args, const juce::String& option, int count)
    {
        auto index = args.indexOf (option);
        juce::StringArray values;

        for (int i = 1; index >= 0 && i <= count && index + i < args.size(); ++i)
            values.add (args[index + i]);

        return values;
    }
}
