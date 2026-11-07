#pragma once

#include <JuceHeader.h>

// Automatic beat grids for dance music: the song's BPM, where its beats fall (on the kicks) and a
// guess at beat 1 of a bar. Dance tracks are made in DAWs at a steady, usually whole-number
// tempo, so instead of following the beat bit by bit, one exact grid is fitted to the whole song:
//
// 1. An onset curve (every 128 samples) that peaks where kicks start: the rise of the low end
//    (< 150 Hz) plus the rise of the whole sound.
// 2. Coarse tempo (100-200 BPM) from that curve's autocorrelation, then a fine search (0.002 BPM
//    steps) for the tempo whose beat lines collect the most onsets.
// 3. Whole-number BPM if a grid at that tempo stays on the kick starts from the start of the song
//    to the end (drift under 15 ms); otherwise the measured BPM (2 decimals).
// 4. The grid is shifted so its lines sit on the kick starts (the steepest rise of the whole
//    sound's peak - what the maximum-zoom waveform shows).
// 5. Beat 1: of the four positions in a bar, the one where the spectrum (36 bands, 40 Hz-8 kHz)
//    changes most from the beat before, since chords, bass notes and sections change on bar starts.
//
// Prototype and measurements: build/beatbench (grid.py; 32 songs).
namespace mixtopia::beatgrid
{
    struct Result
    {
        double bpm = 0.0;              // what the song is set to (whole number when it checks out)
        double measuredBpm = 0.0;      // what the fine search found
        double firstBeat = 0.0;        // seconds into the file of the first beat line
        double downbeat = 0.0;         // seconds into the file of a bar start (beat 1)
        double driftMs = 0.0;          // whole-number grid's drift over the song
        bool valid = false;
    };

    // Analyses a whole file (call on a background thread). `valid` is false if there's no steady
    // beat to be found, or if cancelled.
    Result analyse (juce::AudioFormatReader&, const std::function<bool()>& shouldStop = [] { return false; });
}
