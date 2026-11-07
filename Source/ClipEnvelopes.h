#pragma once

#include <JuceHeader.h>
#include "ClipTempo.h"

namespace te = tracktion;

// Per-clip envelope lines: volume, a 3-band DJ EQ (bass, mids, treble) and a filter.
//
// Points are stored in the clip's state in SOURCE time (seconds into the song file) with a value,
// so a line stays attached to the music when the clip is moved, trimmed, re-tempoed or the project
// tempo changes. No points = flat at 0. Between points the value is interpolated linearly; before
// the first / after the last point it holds.
//
// Volume and EQ values are dB, from killDb (silence / full band cut) to maxDb. The FILTER line
// (M11, the user's chosen exception to "no effects") is a position from -100 to +100 (stored in the
// same "db" field): 0 = off (with a small dead zone), below = low-pass sweeping 20 kHz down to
// 80 Hz, above = high-pass sweeping 20 Hz up to 8 kHz, in even musical steps (log frequency).
namespace mixtopia::envelopes
{
    enum class Param { volume = 0, bass, mids, treble, filter };
    constexpr int numParams = 5;
    constexpr float killDb = -60.0f;
    constexpr float maxDb = 6.0f;

    // The filter line: position -100 (low-pass at its lowest) .. 0 (off) .. +100 (high-pass at its highest).
    constexpr float filterMin = -100.0f, filterMax = 100.0f, filterDeadZone = 5.0f;
    constexpr double lowPassOpenHz = 20000.0, lowPassClosedHz = 80.0, highPassOpenHz = 20.0, highPassClosedHz = 8000.0;

    inline float getMinValue (Param p)   { return p == Param::filter ? filterMin : killDb; }
    inline float getMaxValue (Param p)   { return p == Param::filter ? filterMax : maxDb; }

    // How far past the dead zone a position is, 0..1 (0 inside it).
    inline double filterAmount (float position)
    {
        return juce::jlimit (0.0, 1.0, (std::abs ((double) position) - filterDeadZone) / (filterMax - filterDeadZone));
    }

    // The cutoffs at a position (log-frequency steps); the other stage stays fully open.
    inline double lowPassHz (float position)
    {
        return position < 0.0f ? lowPassOpenHz * std::pow (lowPassClosedHz / lowPassOpenHz, filterAmount (position)) : lowPassOpenHz;
    }

    inline double highPassHz (float position)
    {
        return position > 0.0f ? highPassOpenHz * std::pow (highPassClosedHz / highPassOpenHz, filterAmount (position)) : highPassOpenHz;
    }

    // 24 dB/oct per side = two stages: a fixed Q 0.541 one and a resonant one whose Q rises from 1.307
    // (together a flat 4-pole Butterworth, no bump) to 3.0 (a +4.7 dB peak at the cutoff) over the
    // first 15% of its travel. The user's pick from a blind A/B/C test against a hardware DJ mixer's filter
    // (2026-10-27: 12 dB/oct +7 dB "too mellow", 24 dB/oct +10.3 dB "too harsh", this "Goldilocks").
    constexpr double openQ = 1.30656296, fixedQ = 0.54119610, resonanceQ = 3.0;

    inline double filterQ (float position)
    {
        return openQ + (resonanceQ - openQ) * juce::jmin (1.0, filterAmount (position) / 0.15);
    }

    // How much of the filtered sound is heard: 0 at the very middle (the song untouched), 1 at the
    // dead zone's edge and beyond. Both stages keep running, and the mixer follows this over ~20 ms
    // (a fast sweep straight through the middle barely touches it), so the blend can't click.
    inline float filterWet (float position)
    {
        return juce::jlimit (0.0f, 1.0f, std::abs (position) / filterDeadZone);
    }

    // "off", "low-pass 150 Hz", "high-pass 2.1 kHz".
    inline juce::String describeFilter (float position)
    {
        if (filterAmount (position) <= 0.0)
            return "off";

        auto hz = position < 0.0f ? lowPassHz (position) : highPassHz (position);
        auto text = hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
        return (position < 0.0f ? "low-pass " : "high-pass ") + text;
    }

    inline const juce::Identifier envelopeType { "MIXTOPIA_ENVELOPE" };
    inline const juce::Identifier pointType { "POINT" };
    inline const juce::Identifier paramProperty { "param" };
    inline const juce::Identifier timeProperty { "t" };
    inline const juce::Identifier valueProperty { "db" };

    inline juce::String getName (Param p)
    {
        static const char* names[] = { "Volume", "Bass", "Mid", "Treble", "Filter" };
        return names[(int) p];
    }

    inline juce::String getId (Param p)
    {
        static const char* ids[] = { "volume", "bass", "mids", "treble", "filter" };
        return ids[(int) p];
    }

    inline juce::Colour getColour (Param p)
    {
        static const juce::uint32 colours[] = { 0xffffffff, 0xffef4444, 0xff4ade80, 0xff60a5fa, 0xffffb547 };   // filter: amber, drawn dashed
        return juce::Colour (colours[(int) p]);
    }

    // dB -> linear, with killDb meaning exactly 0.
    inline float toGain (float db)
    {
        return db <= killDb + 0.05f ? 0.0f : juce::Decibels::decibelsToGain (db);
    }

    struct Point
    {
        double sourceTime = 0.0;   // seconds into the song file
        float db = 0.0f;
    };

    inline juce::ValueTree findEnvelope (const juce::ValueTree& clipState, Param p)
    {
        for (auto child : clipState)
            if (child.hasType (envelopeType) && child[paramProperty].toString() == getId (p))
                return child;

        return {};
    }

    inline std::vector<Point> getPoints (const juce::ValueTree& clipState, Param p)
    {
        std::vector<Point> points;

        for (auto point : findEnvelope (clipState, p))
            points.push_back ({ (double) point[timeProperty], (float) point[valueProperty] });

        std::sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.sourceTime < b.sourceTime; });
        return points;
    }

    // Replaces a clip's points for one parameter (undoable).
    // Points (in time order) with those outside [start, end] pushed to the nearer edge, keeping their
    // level; of several past the same edge only the one nearest the song stays. (After a trim: a dot
    // always stays on the visible song - user's choice.) Unchanged if all are inside.
    inline std::vector<Point> pushedInside (const std::vector<Point>& points, double start, double end)
    {
        std::vector<Point> kept;
        std::optional<Point> lastBefore, firstAfter;

        for (auto& point : points)
        {
            if (point.sourceTime < start)
                lastBefore = point;
            else if (point.sourceTime > end)
                firstAfter = firstAfter ? firstAfter : std::optional (point);
            else
                kept.push_back (point);
        }

        if (lastBefore)
            kept.insert (kept.begin(), { start, lastBefore->db });

        if (firstAfter)
            kept.push_back ({ end, firstAfter->db });

        return kept;
    }

    inline void setPoints (juce::ValueTree clipState, Param p, std::vector<Point> points, juce::UndoManager* um)
    {
        std::sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.sourceTime < b.sourceTime; });
        auto envelope = findEnvelope (clipState, p);

        if (! envelope.isValid())
        {
            envelope = juce::ValueTree (envelopeType);
            envelope.setProperty (paramProperty, getId (p), nullptr);
            clipState.appendChild (envelope, um);
        }

        envelope.removeAllChildren (um);

        for (auto& point : points)
            envelope.appendChild (juce::ValueTree (pointType, { { timeProperty, point.sourceTime },
                                                                { valueProperty, juce::jlimit (getMinValue (p), getMaxValue (p), point.db) } }), um);
    }

    // Value of a sorted point list at time t (same time base as the points).
    template <typename TimeOf, typename ValueOf, typename Container>
    inline float evaluate (const Container& points, double t, TimeOf timeOf, ValueOf valueOf)
    {
        if (points.empty())
            return 0.0f;

        if (t <= timeOf (points.front()))
            return valueOf (points.front());

        if (t >= timeOf (points.back()))
            return valueOf (points.back());

        auto next = std::upper_bound (points.begin(), points.end(), t, [&] (double v, auto& p) { return v < timeOf (p); });
        auto prev = std::prev (next);
        auto span = timeOf (*next) - timeOf (*prev);
        auto alpha = span > 0.0 ? (t - timeOf (*prev)) / span : 1.0;
        return (float) (valueOf (*prev) + alpha * (valueOf (*next) - valueOf (*prev)));
    }

    inline float evaluate (const std::vector<Point>& points, double sourceTime)
    {
        return evaluate (points, sourceTime, [] (const Point& p) { return p.sourceTime; }, [] (const Point& p) { return p.db; });
    }
}
