#pragma once

#include <JuceHeader.h>
#include "ClipTempo.h"

namespace te = tracktion;

// Tempo changes along the mix (M6), as the user places them (user's model, 2026-10-21): the
// project tempo, then tempo POINTS (a bar and a BPM). Before the first point the mix plays at the
// project tempo; the first point IS at the project tempo (changing its BPM changes the tempo
// before it too - the strip marks it). Between two points the tempo ramps in a straight line
// (linear in BPM per beat); after the last point it holds. A single point never makes a ramp: a
// ramp only forms between two placed points. An instant change = two points a bar apart.
//
// Stored as Tracktion tempo points, all with curve 0 (a straight line to the next point): point 0
// at bar 1 with the project tempo (not shown), then one per placed point. Projects made with the
// first M6 build (ramps as start/end pairs) read as the same points and play the same. Every edit
// rebuilds the points through tempo::changeTempoMap, so songs stay on their bars; one undo step.
namespace mixtopia::tempo
{
    struct TempoPoint
    {
        double beat = 0.0, bpm = 0.0;
    };

    struct TempoPlan
    {
        double projectBpm = defaultProjectBpm;
        std::vector<TempoPoint> points;   // in order, on bar lines from bar 2, one per bar
    };

    inline TempoPlan getTempoPlan (te::Edit& edit)
    {
        auto& ts = edit.tempoSequence;
        TempoPlan plan;
        plan.projectBpm = ts.getTempo (0)->getBpm();

        for (int i = 1; i < ts.getNumTempos(); ++i)
            plan.points.push_back ({ ts.getTempo (i)->getStartBeat().inBeats(), ts.getTempo (i)->getBpm() });

        return plan;
    }

    inline bool hasTempoChanges (te::Edit& edit)    { return edit.tempoSequence.getNumTempos() > 1; }

    // Points on bar lines from bar 2, one per bar (a later one wins), in order, BPMs in range; the
    // project tempo is the first point's.
    inline TempoPlan tidied (TempoPlan plan)
    {
        std::map<double, double> byBeat;

        for (auto& p : plan.points)
            byBeat[juce::jmax ((double) beatsPerBar, std::round (p.beat / beatsPerBar) * beatsPerBar)] = juce::jlimit (minBpm, maxBpm, p.bpm);

        plan.points.clear();

        for (auto& [beat, bpm] : byBeat)
            plan.points.push_back ({ beat, bpm });

        plan.projectBpm = plan.points.empty() ? juce::jlimit (minBpm, maxBpm, plan.projectBpm) : plan.points.front().bpm;
        return plan;
    }

    // Replaces the tempo map with the plan (tidied), keeping songs on their bars.
    inline void setTempoPlan (te::Edit& edit, TempoPlan plan)
    {
        plan = tidied (std::move (plan));

        changeTempoMap (edit, [&]
        {
            auto& ts = edit.tempoSequence;

            for (int i = ts.getNumTempos(); --i > 0;)
                ts.removeTempo (i, false);

            auto* first = ts.getTempo (0);
            first->setBpm (plan.projectBpm);

            if (! plan.points.empty())
                first->setCurve (0.0f);

            for (auto& p : plan.points)
                ts.insertTempo (te::BeatPosition::fromBeats (p.beat), p.bpm, 0.0f);
        });
    }

    // The ramp a beat is in: i when it's between points i and i + 1 and their BPMs differ; else -1.
    inline int rampAt (const TempoPlan& plan, double beat)
    {
        for (size_t i = 0; i + 1 < plan.points.size(); ++i)
            if (beat >= plan.points[i].beat && beat < plan.points[i + 1].beat)
                return plan.points[i].bpm != plan.points[i + 1].bpm ? (int) i : -1;

        return -1;
    }

    // The points whose BPM sets the steady tempo at a beat (not inside a ramp): before the first
    // point, the first point (= the project tempo; none if there are no points); between two points
    // with the same BPM, both; after the last, the last.
    inline std::vector<size_t> steadyPointsAt (const TempoPlan& plan, double beat)
    {
        if (plan.points.empty())
            return {};

        if (beat < plan.points.front().beat)
            return { 0 };

        for (size_t i = 0; i + 1 < plan.points.size(); ++i)
            if (beat >= plan.points[i].beat && beat < plan.points[i + 1].beat)
                return plan.points[i].bpm == plan.points[i + 1].bpm ? std::vector<size_t> { i, i + 1 } : std::vector<size_t> {};

        return { plan.points.size() - 1 };
    }

    // The ramp from point i - 1 to point i, per bar ("+0.38 BPM per bar"); 0 for the first point.
    inline double bpmPerBarInto (const TempoPlan& plan, size_t i)
    {
        if (i == 0 || i >= plan.points.size())
            return 0.0;

        auto bars = (plan.points[i].beat - plan.points[i - 1].beat) / beatsPerBar;
        return bars > 0.0 ? (plan.points[i].bpm - plan.points[i - 1].bpm) / bars : 0.0;
    }

    // Sets the tempo at a beat: moves or adds the point on the nearest bar line.
    inline TempoPlan withTempoAt (TempoPlan plan, double beat, double bpm)
    {
        plan.points.push_back ({ std::round (beat / beatsPerBar) * beatsPerBar, bpm });
        return tidied (std::move (plan));
    }

    constexpr double steepBpmPerBar = 1.0;   // above this a ramp gets noticeable: shown in orange
}
