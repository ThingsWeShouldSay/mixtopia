#pragma once

#include <JuceHeader.h>

// Waveform overviews in the MixMeister style: instead of each moment's loudest sample (which, for
// loud modern masters, fills the whole height everywhere), the timeline draws one shape growing up
// from the bottom, scaled to the song's own loudest moments:
// - zoomed out: the average loudness, with the low end (< 150 Hz) counting extra, so the song's
//   flow (breakdowns, drops) and, at medium zoom, the kicks stand out;
// - medium zoom: the low end's peaks over ~3 ms, so each kick (and bass note) is a block - easy to
//   see the rhythm, but a kick's deep bass builds up 40-50 ms after it hits;
// - maximum zoom: the whole sound's peaks over ~1.5 ms, where a kick's click is a spike or cliff
//   exactly on the hit (for placing beat 1).
//
// A song is analysed once on a background thread; the result (~2 MB for 7 minutes) is cached in
// ~/.cache/Mixtopia/overviews, keyed by the file's path, size and modification time.
namespace mixtopia
{
    class LoudnessOverview
    {
    public:
        struct Analysis
        {
            double sampleRate = 0.0;
            std::vector<float> energies;    // per blockSize samples
            std::vector<float> bassPeaks;   // per peakBlockSize samples
            std::vector<float> fullPeaks;   // per fullPeakBlockSize samples
        };

        explicit LoudnessOverview (Analysis);

        // Analyses a whole file (slow: call on a background thread). Empty if cancelled or unreadable.
        static Analysis analyse (juce::AudioFormatReader&, const std::function<bool()>& shouldStop = [] { return false; });

        // Average loudness between two times in the file (seconds), relative to the song's loudest
        // moments (1.0); 0 outside the file.
        float getRelativeLevel (double startSeconds, double endSeconds) const;

        // The low end's peak between two times, relative to the song's strongest kicks (1.0).
        float getRelativeBassPeak (double startSeconds, double endSeconds) const;

        // The whole sound's peak between two times, relative to the song's loud peaks (1.0).
        float getRelativeFullPeak (double startSeconds, double endSeconds) const;

        // The same, but read exactly from the song's audio: for zooms closer than the stored peaks
        // (one per fullPeakBlockSize samples). Same measure (the mono mix's peak), same scale.
        float getRelativeFullPeak (juce::AudioFormatReader&, double startSeconds, double endSeconds) const;

        static constexpr int blockSize = 256;
        static constexpr int peakBlockSize = 128;
        static constexpr int fullPeakBlockSize = 64;

    private:
        double sampleRate;
        std::vector<double> cumulative;   // running sum of block energies (one more than blocks)
        std::vector<float> bassPeaks, fullPeaks;
        double energyReference = 1.0, bassPeakReference = 1.0, fullPeakReference = 1.0;

        float peakIn (const std::vector<float>& peaks, int peakBlock, double reference, double startSeconds, double endSeconds) const;
    };

    class LoudnessOverviews
    {
    public:
        explicit LoudnessOverviews (juce::AudioFormatManager&);
        ~LoudnessOverviews();

        // The overview of a file, or nullptr while it's being analysed (started on first request).
        const LoudnessOverview* get (const juce::File&);

        // Forgets files not in the set (they're cheap to reload from the disk cache).
        void retainOnly (const std::set<juce::String>& paths);

        // Called on the message thread whenever an overview becomes ready.
        std::function<void()> onReady;

    private:
        class AnalysisJob;

        juce::AudioFormatManager& formatManager;
        juce::ThreadPool pool { juce::ThreadPoolOptions().withThreadName ("Waveform analysis").withNumberOfThreads (1) };
        std::map<juce::String, std::shared_ptr<const LoudnessOverview>> ready;
        std::set<juce::String> pending, failed;
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessOverviews)
    };
}
