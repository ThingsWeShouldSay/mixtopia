#include "LoudnessOverview.h"
#include "DecodedAudioCache.h"
#include "Biquad.h"

namespace mixtopia
{
    namespace
    {
        constexpr float bassCutoffHz = 150.0f;
        constexpr float bassWeight = 3.0f;          // the low end's energy counts this much extra
        constexpr double energyPercentile = 0.995;
        constexpr double bassPeakPercentile = 0.999;
        constexpr double fullPeakPercentile = 0.99;
        const char* cacheMagic = "MXOV3";

        // Named by the song's key, so the library's preview of the MP3 and the timeline's decoded copy
        // share one file. Files named the old way (by the decoded copy itself) are renamed.
        juce::File getCacheFile (const juce::File& file)
        {
            auto folder = decodedCache::getCacheFolder ("overviews");
            auto cacheFile = folder.getChildFile (decodedCache::getSongKey (file) + ".bin");

            if (auto old = folder.getChildFile (decodedCache::getCacheKey (file) + ".bin"); old != cacheFile && old.existsAsFile())
            {
                if (cacheFile.existsAsFile())
                    old.deleteFile();
                else
                    old.moveFileTo (cacheFile);
            }

            return cacheFile;
        }

        bool readFloats (juce::InputStream& in, std::vector<float>& values)
        {
            auto count = in.readInt();

            if (count < 0 || (juce::int64) count * 4 > in.getNumBytesRemaining())
                return false;

            values.resize ((size_t) count);
            return in.read (values.data(), count * 4) == count * 4;
        }

        bool readCache (const juce::File& cacheFile, LoudnessOverview::Analysis& a)
        {
            juce::FileInputStream in (cacheFile);

            if (! in.openedOk() || in.readString() != cacheMagic)
                return false;

            cacheFile.setLastAccessTime (juce::Time::getCurrentTime());   // (trimmed least recently used first)

            a.sampleRate = in.readDouble();
            return a.sampleRate > 0.0 && readFloats (in, a.energies) && readFloats (in, a.bassPeaks)
                     && readFloats (in, a.fullPeaks);
        }

        void writeCache (const juce::File& cacheFile, const LoudnessOverview::Analysis& a)
        {
            cacheFile.getParentDirectory().createDirectory();
            auto temp = cacheFile.getSiblingFile (cacheFile.getFileName() + ".tmp");

            {
                juce::FileOutputStream out (temp);

                if (! out.openedOk())
                    return;

                out.setPosition (0);
                out.truncate();
                out.writeString (cacheMagic);
                out.writeDouble (a.sampleRate);

                for (auto* values : { &a.energies, &a.bassPeaks, &a.fullPeaks })
                {
                    out.writeInt ((int) values->size());
                    out.write (values->data(), values->size() * 4);
                }
            }

            temp.moveFileTo (cacheFile);
        }

        double percentile (std::vector<float> values, double fraction)
        {
            if (values.empty())
                return 1.0;

            auto index = (size_t) ((double) (values.size() - 1) * fraction);
            std::nth_element (values.begin(), values.begin() + (long) index, values.end());
            return juce::jmax (1.0e-12, (double) values[index]);
        }
    }

    //==============================================================================
    // Energies: mean square of the mono signal plus the weighted mean square of its low end, per
    // block, smoothed over 3 blocks (~17 ms). Bass peaks: the low end's largest sample per
    // (smaller) block, moved earlier by the filters' delay. Full peaks: the largest sample of the
    // whole sound per (smallest) block - a kick's click shows there as a spike on the hit, while
    // its deep bass only builds up 40-50 ms later. Empty if cancelled or unreadable.
    LoudnessOverview::Analysis LoudnessOverview::analyse (juce::AudioFormatReader& reader, const std::function<bool()>& shouldStop)
    {
        using LO = LoudnessOverview;
        constexpr int chunk = 64 * LO::blockSize;

        // Each Butterworth stage delays the low end by sqrt(2) / (2 pi fc).
        auto filterDelay = (juce::int64) std::round (2.0 * std::sqrt (2.0) / (juce::MathConstants<double>::twoPi * bassCutoffHz)
                                                     * reader.sampleRate);

        juce::AudioBuffer<float> buffer ((int) juce::jmax (1u, reader.numChannels), chunk);
        Biquad low1, low2;
        low1.setLowPass (reader.sampleRate, bassCutoffHz);
        low2.setLowPass (reader.sampleRate, bassCutoffHz);
        LO::Analysis a;
        a.sampleRate = reader.sampleRate;
        a.energies.reserve ((size_t) (reader.lengthInSamples / LO::blockSize + 1));
        a.bassPeaks.assign ((size_t) (reader.lengthInSamples / LO::peakBlockSize + 1), 0.0f);
        a.fullPeaks.assign ((size_t) (reader.lengthInSamples / LO::fullPeakBlockSize + 1), 0.0f);

        double sum = 0.0;
        int inBlock = 0;

        for (juce::int64 pos = 0; pos < reader.lengthInSamples; pos += chunk)
        {
            if (shouldStop())
                return {};

            auto num = (int) juce::jmin ((juce::int64) chunk, reader.lengthInSamples - pos);

            if (! reader.read (&buffer, 0, num, pos, true, true))
                return {};

            auto numChannels = buffer.getNumChannels();

            for (int i = 0; i < num; ++i)
            {
                auto x = 0.0f;

                for (int ch = 0; ch < numChannels; ++ch)
                    x += buffer.getSample (ch, i);

                x /= (float) numChannels;
                auto& fullPeak = a.fullPeaks[(size_t) ((pos + i) / LO::fullPeakBlockSize)];
                fullPeak = juce::jmax (fullPeak, std::abs (x));

                auto bass = low2.process (low1.process (x));
                sum += x * x + bassWeight * bass * bass;

                if (auto t = pos + i - filterDelay; t >= 0)
                {
                    auto& peak = a.bassPeaks[(size_t) (t / LO::peakBlockSize)];
                    peak = juce::jmax (peak, std::abs (bass));
                }

                if (++inBlock == LO::blockSize)
                {
                    a.energies.push_back ((float) (sum / LO::blockSize));
                    sum = 0.0;
                    inBlock = 0;
                }
            }
        }

        if (inBlock > 0)
            a.energies.push_back ((float) (sum / inBlock));

        auto smoothed = a.energies;

        for (size_t i = 1; i + 1 < a.energies.size(); ++i)
            smoothed[i] = (a.energies[i - 1] + a.energies[i] + a.energies[i + 1]) / 3.0f;

        a.energies = std::move (smoothed);
        return a;
    }

    //==============================================================================
    LoudnessOverview::LoudnessOverview (Analysis a)
        : sampleRate (a.sampleRate), bassPeaks (std::move (a.bassPeaks)), fullPeaks (std::move (a.fullPeaks))
    {
        cumulative.resize (a.energies.size() + 1, 0.0);

        for (size_t i = 0; i < a.energies.size(); ++i)
            cumulative[i + 1] = cumulative[i] + a.energies[i];

        energyReference = percentile (std::move (a.energies), energyPercentile);
        bassPeakReference = percentile (bassPeaks, bassPeakPercentile);
        fullPeakReference = percentile (fullPeaks, fullPeakPercentile);
    }

    float LoudnessOverview::getRelativeLevel (double startSeconds, double endSeconds) const
    {
        auto numBlocks = (double) cumulative.size() - 1.0;
        auto first = juce::jlimit (0.0, numBlocks, startSeconds * sampleRate / blockSize);
        auto last = juce::jlimit (0.0, numBlocks, endSeconds * sampleRate / blockSize);

        // Narrower than a block (zoomed far in): use the block it's in.
        if (last - first < 1.0)
        {
            if (first >= numBlocks || endSeconds <= 0.0)
                return 0.0f;

            first = std::floor (juce::jmin (first, numBlocks - 1.0));
            last = first + 1.0;
        }

        auto a = (size_t) first, b = (size_t) last;
        auto mean = (cumulative[b] - cumulative[a]) / (double) (b - a);
        return (float) std::sqrt (juce::jmax (0.0, mean) / energyReference);
    }

    float LoudnessOverview::peakIn (const std::vector<float>& peaks, int peakBlock, double reference,
                                     double startSeconds, double endSeconds) const
    {
        auto numBlocks = (juce::int64) peaks.size();
        auto first = (juce::int64) std::floor (startSeconds * sampleRate / peakBlock);
        auto last = juce::jmax (first + 1, (juce::int64) std::ceil (endSeconds * sampleRate / peakBlock));
        first = juce::jlimit ((juce::int64) 0, numBlocks, first);
        last = juce::jlimit ((juce::int64) 0, numBlocks, last);

        auto peak = 0.0f;

        for (auto i = first; i < last; ++i)
            peak = juce::jmax (peak, peaks[(size_t) i]);

        return (float) (peak / reference);
    }

    float LoudnessOverview::getRelativeFullPeak (juce::AudioFormatReader& reader, double startSeconds, double endSeconds) const
    {
        auto first = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (startSeconds * reader.sampleRate));
        auto last = juce::jmin (reader.lengthInSamples, juce::jmax (first + 1, (juce::int64) std::ceil (endSeconds * reader.sampleRate)));

        if (last <= first)
            return 0.0f;

        // (Painting runs on the message thread; the buffer is kept between calls.)
        thread_local juce::AudioBuffer<float> buffer;
        auto num = (int) (last - first);
        buffer.setSize ((int) reader.numChannels, num, false, false, true);

        if (! reader.read (&buffer, 0, num, first, true, true))
            return 0.0f;

        auto peak = 0.0f;

        for (int i = 0; i < num; ++i)
        {
            auto x = 0.0f;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                x += buffer.getSample (ch, i);

            peak = juce::jmax (peak, std::abs (x / (float) buffer.getNumChannels()));
        }

        return (float) (peak / fullPeakReference);
    }

    float LoudnessOverview::getRelativeBassPeak (double startSeconds, double endSeconds) const
    {
        return peakIn (bassPeaks, peakBlockSize, bassPeakReference, startSeconds, endSeconds);
    }

    float LoudnessOverview::getRelativeFullPeak (double startSeconds, double endSeconds) const
    {
        return peakIn (fullPeaks, fullPeakBlockSize, fullPeakReference, startSeconds, endSeconds);
    }

    //==============================================================================
    class LoudnessOverviews::AnalysisJob : public juce::ThreadPoolJob
    {
    public:
        AnalysisJob (LoudnessOverviews& o, const juce::File& f)
            : ThreadPoolJob ("Waveform analysis"), owner (o), file (f), alive (o.alive)
        {
        }

        JobStatus runJob() override
        {
            auto cacheFile = getCacheFile (file);
            LoudnessOverview::Analysis analysis;

            if (! readCache (cacheFile, analysis))
            {
                analysis = {};
                std::unique_ptr<juce::AudioFormatReader> reader (owner.formatManager.createReaderFor (file));

                if (reader != nullptr)
                {
                    analysis = LoudnessOverview::analyse (*reader, [this] { return shouldExit(); });

                    if (! analysis.energies.empty())
                        writeCache (cacheFile, analysis);
                }
            }

            if (shouldExit())
                return jobHasFinished;

            auto result = analysis.energies.empty() ? nullptr : std::make_shared<LoudnessOverview> (std::move (analysis));
            auto path = file.getFullPathName();
            auto* ownerPtr = &owner;

            juce::MessageManager::callAsync ([weak = alive, ownerPtr, path, result]
            {
                if (weak.lock() == nullptr)
                    return;

                ownerPtr->pending.erase (path);

                if (result == nullptr)
                    ownerPtr->failed.insert (path);   // unreadable: don't retry on every repaint
                else
                {
                    ownerPtr->ready[path] = result;

                    if (ownerPtr->onReady)
                        ownerPtr->onReady();
                }
            });

            return jobHasFinished;
        }

    private:
        LoudnessOverviews& owner;
        juce::File file;
        std::weak_ptr<bool> alive;
    };

    LoudnessOverviews::LoudnessOverviews (juce::AudioFormatManager& fm)
        : formatManager (fm)
    {
    }

    LoudnessOverviews::~LoudnessOverviews()
    {
        alive.reset();
        pool.removeAllJobs (true, 10000);
    }

    const LoudnessOverview* LoudnessOverviews::get (const juce::File& file)
    {
        auto path = file.getFullPathName();

        if (auto it = ready.find (path); it != ready.end())
            return it->second.get();

        if (failed.count (path) == 0 && pending.insert (path).second)
            pool.addJob (new AnalysisJob (*this, file), true);

        return nullptr;
    }

    void LoudnessOverviews::retainOnly (const std::set<juce::String>& paths)
    {
        for (auto it = ready.begin(); it != ready.end();)
            it = paths.count (it->first) == 0 ? ready.erase (it) : std::next (it);
    }
}
