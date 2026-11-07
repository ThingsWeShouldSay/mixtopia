#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// Tempo matching for clips.
//
// Each clip can carry the song's own BPM and the position (in the source file) of one downbeat
// ("beat 1"). With a BPM set, the clip uses Tracktion's auto-tempo mode and is time-stretched
// (pitch preserved) in real time to follow the Edit's tempo sequence. Clips never use proxy
// files (see mixtopia::insertAudioFile), so Tracktion counts the clip's source beats from the
// start of the file; we line the song's beats up with the project's by placing the clip so that
// one of its beat lines (the "grid anchor") sits on a project beat.
//
// Tempo changes and ramps (M6): Tracktion plays a tempo-matched clip by counting beats - source
// beat = edit beat - the clip's "content start beat" (the edit beat where the file's start would
// play; the trim offset is converted at the tempo where the clip starts). All conversions here
// go through beats, so a song stays glued to its bars wherever the tempo changes, and clips are
// placed by content start beat + the part of the source to play (place()).
namespace mixtopia::tempo
{
    // The range of BPMs that can be typed in (project tempo and song BPM).
    constexpr double minBpm = 40.0, maxBpm = 250.0;

    // New projects start at this tempo (user's choice: songs adjust to the project, never the
    // other way round).
    constexpr double defaultProjectBpm = 140.0;
    constexpr int beatsPerBar = 4;

    // How a song's grid is snapped onto the project's: beat lines onto beats, or its beat 1 onto
    // bar starts (so songs can't end up a beat or two out of step).
    enum class Snap { none, beats, bars };

    inline const juce::Identifier bpmProperty { "mixtopiaBpm" };
    inline const juce::Identifier downbeatProperty { "mixtopiaDownbeat" };
    inline const juce::Identifier stretchModeProperty { "mixtopiaStretchMode" };

    // Where a clip's BPM and beat 1 came from: "detected" (automatic) or "adjusted" (by hand).
    inline const juce::Identifier gridSourceProperty { "mixtopiaGridSource" };

    // A part of the source file, in seconds from the file's start.
    struct SourceRange
    {
        double start = 0.0, end = 0.0;
    };

    inline double getClipBpm (const te::AudioClipBase& clip)    { return clip.state.getProperty (bpmProperty, 0.0); }

    // "140" for whole numbers, otherwise "137.50".
    inline juce::String formatBpm (double bpm)
    {
        return juce::String (bpm, std::abs (bpm - std::round (bpm)) < 0.0001 ? 0 : 2);
    }
    inline double getDownbeat (const te::AudioClipBase& clip)   { return clip.state.getProperty (downbeatProperty, 0.0); }

    inline double getProjectBpm (te::Edit& edit, te::TimePosition at = {})
    {
        return edit.tempoSequence.getBpmAt (at);
    }

    inline te::TimeStretcher::Mode getStretchMode (te::Edit& edit)
    {
        return (te::TimeStretcher::Mode) (int) edit.state.getProperty (stretchModeProperty,
                                                                       (int) te::TimeStretcher::signalsmithDefault);
    }

    inline double getSourceLength (te::AudioClipBase& clip)
    {
        return clip.getAudioFile().getLength();
    }

    // Where a song's beat lines fall within one beat: the downbeat modulo one beat, in source
    // seconds (0 when the BPM is unknown).
    inline double beatPhase (double bpm, double downbeat)
    {
        if (bpm <= 0.0)
            return 0.0;

        auto beatLength = 60.0 / bpm;
        auto phase = std::fmod (downbeat, beatLength);
        return phase < 0.0 ? phase + beatLength : phase;
    }

    inline double getBeatPhase (const te::AudioClipBase& clip)
    {
        return beatPhase (getClipBpm (clip), getDownbeat (clip));
    }

    inline bool isTempoMatched (const te::AudioClipBase& clip)
    {
        return clip.getAutoTempo() && getClipBpm (clip) > 0.0;
    }

    inline double toBeats (te::Edit& edit, te::TimePosition t)     { return edit.tempoSequence.toBeats (t).inBeats(); }
    inline te::TimePosition toTime (te::Edit& edit, double beats)  { return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beats)); }

    // Edit seconds per source second where the clip starts (the tempo can change further on).
    inline double getStretchRatio (te::AudioClipBase& clip)
    {
        auto bpm = getClipBpm (clip);

        if (! clip.getAutoTempo() || bpm <= 0.0)
            return 1.0;

        return bpm / getProjectBpm (clip.edit, clip.getPosition().getStart());
    }

    // Tempo-matched clips: the edit beat at which the start of the source file would play (as
    // Tracktion's playback counts it: the offset converted at the tempo where the clip starts).
    inline double getContentStartBeat (te::AudioClipBase& clip)
    {
        auto pos = clip.getPosition();
        return toBeats (clip.edit, pos.getStart()) - pos.getOffset().inSeconds() * getProjectBpm (clip.edit, pos.getStart()) / 60.0;
    }

    // Edit time at which the start of the source file would play.
    inline te::TimePosition getContentStart (te::AudioClipBase& clip)
    {
        if (isTempoMatched (clip))
            return toTime (clip.edit, getContentStartBeat (clip));

        auto pos = clip.getPosition();
        return pos.getStart() - pos.getOffset();
    }

    inline double sourceToEditSeconds (te::AudioClipBase& clip, double sourceSeconds)
    {
        if (isTempoMatched (clip))
            return toTime (clip.edit, getContentStartBeat (clip) + sourceSeconds * getClipBpm (clip) / 60.0).inSeconds();

        return getContentStart (clip).inSeconds() + sourceSeconds;
    }

    inline double editToSourceSeconds (te::AudioClipBase& clip, double editSeconds)
    {
        if (isTempoMatched (clip))
            return (toBeats (clip.edit, te::TimePosition::fromSeconds (editSeconds)) - getContentStartBeat (clip)) * 60.0 / getClipBpm (clip);

        return editSeconds - getContentStart (clip).inSeconds();
    }

    inline SourceRange getSourceRange (te::AudioClipBase& clip)
    {
        auto pos = clip.getPosition();
        return { editToSourceSeconds (clip, pos.getStart().inSeconds()), editToSourceSeconds (clip, pos.getEnd().inSeconds()) };
    }

    // The content start (an edit time, as getContentStart) that makes source time `sourceSeconds`
    // of a song at `bpm` (<= 0: not tempo-matched) play at `editTime`.
    inline te::TimePosition contentStartFor (te::Edit& edit, double bpm, double sourceSeconds, te::TimePosition editTime)
    {
        if (bpm <= 0.0)
            return editTime - te::TimeDuration::fromSeconds (sourceSeconds);

        return toTime (edit, toBeats (edit, editTime) - sourceSeconds * bpm / 60.0);
    }

    // Places a tempo-matched clip: the source file's start at edit beat `contentStartBeat`, playing
    // source seconds keep.start to keep.end. A clip can't start before 0:00: the part before is
    // dropped. Changes go through the Edit's undo manager.
    inline void place (te::AudioClipBase& clip, double contentStartBeat, SourceRange keep)
    {
        auto& edit = clip.edit;
        auto beatsPerSecond = getClipBpm (clip) / 60.0;
        auto startBeat = contentStartBeat + keep.start * beatsPerSecond;
        auto endBeat = contentStartBeat + keep.end * beatsPerSecond;

        if (startBeat < 0.0)
            startBeat = 0.0;

        auto start = toTime (edit, startBeat);
        auto end = juce::jmax (toTime (edit, endBeat), start + te::TimeDuration::fromSeconds (0.01));
        auto offset = te::TimeDuration::fromSeconds ((startBeat - contentStartBeat) * 60.0 / getProjectBpm (edit, start));
        clip.setPosition ({ { start, end }, offset });
    }

    // A content start beat (as for place) moved later if needed so the clip's start isn't before
    // 0:00 - a song dragged past the start of the mix stops there, with nothing cut (user, 2026-10-23:
    // a free drag used to trim off whatever went past 0:00).
    inline double contentStartNotBeforeZero (te::AudioClipBase& clip, double contentStartBeat, SourceRange keep)
    {
        return juce::jmax (contentStartBeat, -keep.start * getClipBpm (clip) / 60.0);
    }

    // Edit time of the clip's first source beat line (its "grid anchor").
    inline te::TimePosition getGridAnchor (te::AudioClipBase& clip)
    {
        return te::TimePosition::fromSeconds (sourceToEditSeconds (clip, getBeatPhase (clip)));
    }

    inline te::TimePosition snapToBeat (te::Edit& edit, te::TimePosition t)
    {
        auto beats = edit.tempoSequence.toBeats (t).inBeats();
        return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (std::round (beats)));
    }

    inline te::TimePosition snapToBar (te::Edit& edit, te::TimePosition t)
    {
        auto beats = edit.tempoSequence.toBeats (t).inBeats();
        return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (std::round (beats / beatsPerBar) * beatsPerBar));
    }

    // Where a song's bar starts fall within one bar: the downbeat modulo one bar, in source seconds.
    inline double barPhase (double bpm, double downbeat)
    {
        if (bpm <= 0.0)
            return 0.0;

        auto barLength = beatsPerBar * 60.0 / bpm;
        auto phase = std::fmod (downbeat, barLength);
        return phase < 0.0 ? phase + barLength : phase;
    }

    // The content start that puts a song's beat lines on the project's beats (or its beat 1 on a
    // bar start), nearest to the given content start.
    inline te::TimePosition snapContentStart (te::Edit& edit, double bpm, double downbeat, te::TimePosition contentStart, Snap snap)
    {
        if (snap == Snap::none || bpm <= 0.0)
            return contentStart;

        auto phase = snap == Snap::bars ? barPhase (bpm, downbeat) : beatPhase (bpm, downbeat);
        auto target = toTime (edit, toBeats (edit, contentStart) + phase * bpm / 60.0);
        return contentStartFor (edit, bpm, phase, snap == Snap::bars ? snapToBar (edit, target) : snapToBeat (edit, target));
    }

    inline te::TimePosition snapContentStart (te::AudioClipBase& clip, te::TimePosition contentStart, Snap snap)
    {
        return snapContentStart (clip.edit, getClipBpm (clip), getDownbeat (clip), contentStart, snap);
    }

    // Sets a clip's song BPM and downbeat (bpm <= 0 means "unknown": play at original speed),
    // keeping the given part of the source, and places the clip so its content starts at
    // contentStart (an edit time). Changes go through the Edit's undo manager.
    inline void apply (te::AudioClipBase& clip, double bpm, double downbeat, SourceRange keep,
                       te::TimePosition contentStart)
    {
        auto* um = &clip.edit.getUndoManager();
        clip.state.setProperty (bpmProperty, bpm, um);
        clip.state.setProperty (downbeatProperty, downbeat, um);

        auto sourceLength = getSourceLength (clip);
        auto& loopInfo = clip.getLoopInfo();
        loopInfo.setInMarker (0);
        loopInfo.setOutMarker (-1);

        if (bpm > 0.0)
        {
            loopInfo.setNumBeats (sourceLength * bpm / 60.0);
            clip.setTimeStretchMode (getStretchMode (clip.edit));
            clip.setAutoTempo (true);
        }
        else
        {
            clip.setAutoTempo (false);
        }

        keep.start = juce::jlimit (0.0, sourceLength, keep.start);
        keep.end = juce::jlimit (keep.start, sourceLength, keep.end);

        if (bpm > 0.0)
        {
            place (clip, toBeats (clip.edit, contentStart), keep);
            return;
        }

        auto offset = te::TimeDuration::fromSeconds (keep.start);
        auto length = te::TimeDuration::fromSeconds (keep.end - keep.start);
        auto start = contentStart + offset;

        // Clips can't start before 0: drop the part that would.
        if (start < te::TimePosition())
        {
            auto cut = te::TimePosition() - start;
            start = te::TimePosition();
            offset = offset + cut;
            length = length - cut;
        }

        clip.setPosition ({ { start, start + juce::jmax (length, te::TimeDuration::fromSeconds (0.01)) }, offset });
    }

    // Places a tempo-matched clip so its source starts at `contentStart` (an edit time; its beat 1
    // already snapped there by the caller), playing the source from `keep.start` to `keep.end`.
    // A clip can't start before 0:00:
    // 1. If beat 1 would land before bar 1, the clip moves by whole bars until beat 1 is on bar 1
    //    or later (a drag past the left edge, or the mouse leaving the window).
    // 2. If it then still needs audio from before 0:00 and that audio is silence (before
    //    `mayCutUntil`, source seconds), the silence is trimmed.
    // 3. If that audio holds sound (a pickup before beat 1), the clip moves a bar later, bar by bar,
    //    until it fits. Returns true only in this case.
    // Counted from the first bar start at or after `barsFrom` (source seconds: where the part that
    // plays begins) - a song whose start was trimmed off, or a piece of a chopped song (Chops.h), must
    // be able to reach 0:00 (user, 2026-10-28: a trimmed song stopped short of the start with Snap on).
    inline bool placeKeepingTheGrid (te::AudioClipBase& clip, te::TimePosition contentStart, SourceRange keep, double mayCutUntil,
                                     double barsFrom = 0.0)
    {
        auto beatsPerSecond = getClipBpm (clip) / 60.0;
        auto contentStartBeat = toBeats (clip.edit, contentStart);

        // 1. Beat 1 (the first bar start in the song, or in the piece) on bar 1 or later.
        auto firstBar = barPhase (getClipBpm (clip), getDownbeat (clip));

        if (auto barLength = beatsPerBar / beatsPerSecond; barsFrom > firstBar)
            firstBar += std::ceil ((barsFrom - firstBar) / barLength - 1.0e-9) * barLength;

        auto firstBarStartBeat = contentStartBeat + firstBar * beatsPerSecond;

        if (firstBarStartBeat < -1.0e-6)
            contentStartBeat += std::ceil (-firstBarStartBeat / beatsPerBar - 1.0e-6) * beatsPerBar;

        // 2-3. Trim silence before 0:00, or move later while sound would be cut.
        auto moved = false;

        for (;;)
        {
            if (contentStartBeat + keep.start * beatsPerSecond >= 0.0)
                break;

            if (auto sourceAtZero = -contentStartBeat / beatsPerSecond; sourceAtZero <= mayCutUntil)
            {
                keep.start = sourceAtZero;
                break;
            }

            contentStartBeat += beatsPerBar;
            moved = true;
        }

        keep.end = juce::jmax (keep.start + 0.01, keep.end);
        place (clip, contentStartBeat, keep);
        return moved;
    }

    // The bar line at or before a time (the bar it's in).
    inline te::TimePosition barAtOrBefore (te::Edit& edit, te::TimePosition t)
    {
        auto beats = edit.tempoSequence.toBeats (t).inBeats();
        return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (std::floor (beats / beatsPerBar + 1.0e-9) * beatsPerBar));
    }

    // Changes BPM/downbeat while keeping the clip's audible part and where it starts on the
    // timeline (then snapping its beats onto the grid if requested).
    inline void setBpmAndDownbeat (te::AudioClipBase& clip, double bpm, double downbeat, Snap snap)
    {
        auto keep = getSourceRange (clip);
        auto visibleStart = clip.getPosition().getStart();

        // Work out where content must start for the visible start to stay put. (The clip doesn't
        // have the new BPM yet, so work with the new values.)
        auto contentStart = contentStartFor (clip.edit, bpm, keep.start, visibleStart);
        contentStart = snapContentStart (clip.edit, bpm, downbeat, contentStart, snap);

        apply (clip, bpm, downbeat, keep, contentStart);
    }

    // Changes the tempo map (`change` edits edit.tempoSequence), keeping every song glued to its
    // bars: tempo-matched clips keep their content start beat and the part of the song they play;
    // others keep the beat they start on.
    template <typename Change>
    inline void changeTempoMap (te::Edit& edit, Change&& change)
    {
        struct Saved
        {
            te::AudioClipBase::Ptr clip;
            SourceRange range;
            double beat;   // content start beat (tempo-matched) or start beat (others)
        };

        std::vector<Saved> saved;

        for (auto* track : te::getAudioTracks (edit))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                    saved.push_back ({ clip, getSourceRange (*clip),
                                       isTempoMatched (*clip) ? getContentStartBeat (*clip) : toBeats (edit, clip->getPosition().getStart()) });

        change();

        for (auto& s : saved)
        {
            if (isTempoMatched (*s.clip))
                place (*s.clip, s.beat, s.range);
            else
                s.clip->setStart (toTime (edit, s.beat), false, true);
        }
    }

    // Sets the project tempo: the whole mix's when it has no tempo points; otherwise the tempo
    // before the first point, which is the first point's own tempo too (TempoRamps.h).
    inline void setProjectBpm (te::Edit& edit, double newBpm)
    {
        changeTempoMap (edit, [&]
        {
            if (auto* tempo = edit.tempoSequence.getTempo (0))
                tempo->setBpm (newBpm);

            if (auto* rampStart = edit.tempoSequence.getTempo (1); rampStart != nullptr && rampStart->getCurve() == 0.0f)
                rampStart->setBpm (newBpm);
        });
    }

    // How far the song is sped up or slowed down at most while it plays (percent of its own
    // tempo), allowing for tempo changes along it. Above noticeableStretchPercent stretching starts
    // to sound unnatural: the clip title says so.
    constexpr double noticeableStretchPercent = 10.0;

    inline double getMaxStretchPercent (te::AudioClipBase& clip)
    {
        if (! isTempoMatched (clip))
            return 0.0;

        auto range = clip.getPosition().time;
        auto& ts = clip.edit.tempoSequence;
        auto worst = 0.0;
        auto consider = [&] (te::TimePosition t) { worst = juce::jmax (worst, std::abs (ts.getBpmAt (t) / getClipBpm (clip) - 1.0)); };

        consider (range.getStart());
        consider (range.getEnd());

        for (int i = 0; i < ts.getNumTempos(); ++i)
            if (auto t = ts.getTempo (i)->getStartTime(); range.contains (t))
                consider (t);

        return worst * 100.0;
    }

    inline void setStretchMode (te::Edit& edit, te::TimeStretcher::Mode mode)
    {
        edit.state.setProperty (stretchModeProperty, (int) mode, &edit.getUndoManager());

        for (auto* track : te::getAudioTracks (edit))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                    if (clip->getAutoTempo())
                        clip->setTimeStretchMode (mode);
    }
}
