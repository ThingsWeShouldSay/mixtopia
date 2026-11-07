#pragma once

#include <JuceHeader.h>

// How loud a song sounds: integrated loudness in LUFS, the broadcast standard (EBU R128 / ITU-R
// BS.1770-4). The signal is K-weighted (a high-shelf around 1.7 kHz and a high-pass at 38 Hz, which
// mimic how the ear weighs frequencies), measured in 400 ms blocks every 100 ms, and gated: blocks
// below -70 LUFS are ignored, then blocks more than 10 LU below the average of the rest, so
// silences and quiet breakdowns don't pull the number down.
//
// Used for automatic level matching: quieter songs are raised towards a loud reference (never
// lowered - the user's rule: "turning down is easy, restoring lost level isn't").
namespace mixtopia::loudness
{
    // The K-weighting filter for one channel: every LUFS reading is taken through it.
    class KWeighting
    {
    public:
        void prepare (double sampleRate);
        void reset()                    { shelf.z1 = shelf.z2 = highPass.z1 = highPass.z2 = 0.0; }
        double process (double x)       { return highPass.process (shelf.process (x)); }

    private:
        struct Section   // one biquad, direct form II transposed
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

            double process (double x)
            {
                auto y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };

        Section shelf, highPass;
    };

    // Mean square of the K-weighted signal (summed over channels) -> LUFS; -inf for silence.
    double toLufs (double meanSquare);

    // Readings from consecutive 100 ms steps (each: K-weighted mean square, summed over channels).
    // Used for whole files (measure) and live on the master output (the master panel).
    class Meter
    {
    public:
        void addStep (double meanSquare);
        void reset()                    { steps.clear(); blocks.clear(); }

        double getShortTerm() const;    // the last 3 s
        double getIntegrated() const;   // gated, everything since reset (what measure() gives for a file)

    private:
        std::vector<double> steps;    // the last 3 s
        std::vector<double> blocks;   // 400 ms, every 100 ms, since reset
    };

    // Integrated loudness of a whole file in LUFS; -inf for silence (or if cancelled).
    double measure (juce::AudioFormatReader&, const std::function<bool()>& shouldStop = [] { return false; });

    // Songs quieter than this are raised to it (where the user's loud trance masters sit).
    // Measured on the user's 32 test songs (2026-10-21): trance mostly -6 to -8.5 LUFS (typical
    // -7.5), hard dance -4.3 to -5.5, the quietest trance -9.6. -7 is the loud side of their trance.
    constexpr double referenceLufs = -7.0;
    constexpr double maxRaiseDb = 9.0;

    // The raise for a song of the given loudness: 0 if it's at or above the reference, at most
    // maxRaiseDb; 0 if unknown (silence).
    double matchGainDb (double songLufs);
}
