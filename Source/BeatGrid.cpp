#include "BeatGrid.h"
#include "Biquad.h"

namespace mixtopia::beatgrid
{
    namespace
    {
        constexpr int hop = 128;
        constexpr double minBpm = 100.0, maxBpm = 200.0;
        constexpr double wholeNumberMaxDriftMs = 15.0;
        constexpr double kickSearchSeconds = 0.035;    // look for a kick this far either side of a beat line

        // Spectrum for finding beat 1: 36 bands from 40 Hz to 8 kHz, every 1024 samples.
        constexpr int fftOrder = 12, fftSize = 1 << fftOrder, spectrumHop = 1024, numBands = 36;
        using BandEnergies = std::array<float, numBands>;

        // Per hop (128 samples) of the mono signal, plus the band spectrum.
        struct Features
        {
            double framesPerSecond = 0.0, spectraPerSecond = 0.0;
            std::vector<float> low, full, peak;     // low-end energy, whole-sound energy and peak
            std::vector<BandEnergies> spectra;
        };

        // Turns mono samples into band energies every spectrumHop samples.
        struct Spectrogram
        {
            explicit Spectrogram (double sampleRate)
            {
                for (int b = 0; b <= numBands; ++b)
                    edges[(size_t) b] = (int) std::round (40.0 * std::pow (8000.0 / 40.0, (double) b / numBands) * fftSize / sampleRate);
            }

            void add (float x, std::vector<BandEnergies>& out)
            {
                history[(size_t) (written++ % fftSize)] = x;

                if (written < fftSize || (written - fftSize) % spectrumHop != 0)
                    return;

                for (int i = 0; i < fftSize; ++i)
                    data[(size_t) i] = history[(size_t) ((written + (juce::uint64) i) % fftSize)];

                window.multiplyWithWindowingTable (data.data(), fftSize);
                fft.performFrequencyOnlyForwardTransform (data.data());

                BandEnergies bands {};

                for (int b = 0; b < numBands; ++b)
                    for (int k = edges[(size_t) b]; k < edges[(size_t) b + 1]; ++k)
                        bands[(size_t) b] += data[(size_t) k] * data[(size_t) k];

                out.push_back (bands);
            }

            juce::dsp::FFT fft { fftOrder };
            juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
            std::array<float, fftSize> history {};
            std::array<float, 2 * fftSize> data {};
            std::array<int, numBands + 1> edges {};
            juce::uint64 written = 0;
        };

        Features measure (juce::AudioFormatReader& reader, const std::function<bool()>& shouldStop)
        {
            Features f;
            f.framesPerSecond = reader.sampleRate / hop;
            f.spectraPerSecond = reader.sampleRate / spectrumHop;
            auto rate = reader.sampleRate;

            Biquad low1, low2;
            low1.setLowPass (rate, 150.0);
            low2.setLowPass (rate, 150.0);
            auto spectrogram = std::make_unique<Spectrogram> (rate);

            constexpr int chunk = 256 * hop;
            juce::AudioBuffer<float> buffer ((int) juce::jmax (1u, reader.numChannels), chunk);
            double sums[2] {};
            float peak = 0.0f;
            int inHop = 0;

            for (juce::int64 pos = 0; pos < reader.lengthInSamples; pos += chunk)
            {
                if (shouldStop())
                    return {};

                auto num = (int) juce::jmin ((juce::int64) chunk, reader.lengthInSamples - pos);

                if (! reader.read (&buffer, 0, num, pos, true, true))
                    return {};

                for (int i = 0; i < num; ++i)
                {
                    auto x = 0.0f;

                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        x += buffer.getSample (ch, i);

                    x /= (float) buffer.getNumChannels();
                    auto l = low2.process (low1.process (x));
                    sums[0] += l * l;
                    sums[1] += x * x;
                    peak = juce::jmax (peak, std::abs (x));
                    spectrogram->add (x, f.spectra);

                    if (++inHop == hop)
                    {
                        f.low.push_back ((float) (sums[0] / hop));
                        f.full.push_back ((float) (sums[1] / hop));
                        f.peak.push_back (peak);
                        sums[0] = sums[1] = 0.0;
                        peak = 0.0f;
                        inHop = 0;
                    }
                }
            }

            return f;
        }

        // How fast the (log) energy is rising: change over ~12 ms of a lightly smoothed log curve.
        std::vector<float> rise (const std::vector<float>& energy)
        {
            auto n = energy.size();
            std::vector<float> logs (n), smooth (n), out (n);

            for (size_t i = 0; i < n; ++i)
                logs[i] = std::log10 (energy[i] + 1.0e-10f);

            for (size_t i = 0; i < n; ++i)
                smooth[i] = ((i > 0 ? logs[i - 1] : 0.0f) + logs[i] + (i + 1 < n ? logs[i + 1] : 0.0f)) / 3.0f;

            for (size_t i = 0; i < n; ++i)
                out[i] = juce::jmax (0.0f, smooth[i] - smooth[i >= 4 ? i - 4 : 0]);

            return out;
        }

        // Folds the onset curve by the beat period: how strongly the onsets line up at one phase.
        // Returns the score and that phase (in hops).
        std::pair<double, double> fold (const std::vector<float>& onsets, double onsetSum, double period, int bins)
        {
            std::vector<double> hist ((size_t) bins, 0.0);

            for (size_t i = 0; i < onsets.size(); ++i)
            {
                auto bin = (int) (std::fmod ((double) i, period) / period * bins) % bins;
                hist[(size_t) bin] += onsets[i];
            }

            auto best = 0;
            auto bestValue = -1.0;

            for (int k = 0; k < bins; ++k)
            {
                // Tolerate one bin of jitter either side.
                auto value = hist[(size_t) k] + hist[(size_t) ((k + 1) % bins)] + hist[(size_t) ((k + bins - 1) % bins)];

                if (value > bestValue)
                {
                    bestValue = value;
                    best = k;
                }
            }

            return { bestValue / (onsetSum + 1.0e-12) * bins / 3.0, (best + 0.5) / bins * period };
        }

        double median (std::vector<double> values)
        {
            if (values.empty())
                return 0.0;

            auto middle = values.begin() + (long) (values.size() / 2);
            std::nth_element (values.begin(), middle, values.end());
            return *middle;
        }

        struct Residual
        {
            double beatTime, offset;   // seconds; kick start minus beat line
        };

        // Where the kicks start relative to each beat line, for the beats with a clear kick.
        std::vector<Residual> kickResiduals (const std::vector<float>& peakRise, double fps, double bpm, double first)
        {
            struct Candidate { double beatTime, offset; float strength; };
            std::vector<Candidate> all;
            auto n = (int) peakRise.size();

            for (auto beat = first; beat < n / fps; beat += 60.0 / bpm)
            {
                auto a = (int) ((beat - kickSearchSeconds) * fps), z = (int) ((beat + kickSearchSeconds) * fps);

                if (a < 1 || z >= n)
                    continue;

                auto k = a;

                for (int i = a; i < z; ++i)
                    if (peakRise[(size_t) i] > peakRise[(size_t) k])
                        k = i;

                all.push_back ({ beat, k / fps - beat, peakRise[(size_t) k] });
            }

            if (all.empty())
                return {};

            // Beats with a clear kick: stronger than 40% of them.
            std::vector<float> strengths;

            for (auto& c : all)
                strengths.push_back (c.strength);

            auto cut = strengths.begin() + (long) (strengths.size() * 2 / 5);
            std::nth_element (strengths.begin(), cut, strengths.end());

            std::vector<Residual> out;

            for (auto& c : all)
                if (c.strength > *cut)
                    out.push_back ({ c.beatTime, c.offset });

            return out;
        }

        // A grid at this BPM: its first line on the kick starts, and how far it drifts over the song.
        struct Grid
        {
            double first = 0.0, driftMs = 0.0;
        };

        Grid fitGrid (const std::vector<float>& onsets, double onsetSum, const std::vector<float>& peakRise, double fps, double bpm)
        {
            auto period = 60.0 / bpm * fps;
            auto first = fold (onsets, onsetSum, period, (int) std::round (period) * 3).second / fps;

            auto residuals = kickResiduals (peakRise, fps, bpm, first);
            std::vector<double> offsets;

            for (auto& r : residuals)
                offsets.push_back (r.offset);

            first += median (offsets);
            residuals = kickResiduals (peakRise, fps, bpm, first);

            // Drift: slope of the offsets over the song (least squares).
            double st = 0, so = 0, stt = 0, sto = 0, count = (double) residuals.size();

            for (auto& r : residuals)
            {
                st += r.beatTime; so += r.offset; stt += r.beatTime * r.beatTime; sto += r.beatTime * r.offset;
            }

            auto denominator = count * stt - st * st;
            auto slope = count > 2 && denominator > 0.0 ? (count * sto - st * so) / denominator : 0.0;
            return { first, slope * (peakRise.size() / fps) * 1000.0 };
        }
    }

    Result analyse (juce::AudioFormatReader& reader, const std::function<bool()>& shouldStop)
    {
        auto f = measure (reader, shouldStop);
        auto n = f.full.size();
        auto fps = f.framesPerSecond;

        if (n < (size_t) (20.0 * fps))   // too short to find a steady beat
            return {};

        // 1. Onset curve: rise of the low end plus rise of the whole sound.
        auto riseLow = rise (f.low), riseFull = rise (f.full);
        std::vector<float> onsets (n);
        auto maxOnset = 0.0f;

        for (size_t i = 0; i < n; ++i)
            maxOnset = juce::jmax (maxOnset, onsets[i] = riseLow[i] + riseFull[i]);

        if (maxOnset <= 0.0f)
            return {};

        double onsetSum = 0.0, mean = 0.0;

        for (auto& o : onsets)
        {
            o /= maxOnset;
            onsetSum += o;
        }

        mean = onsetSum / (double) n;

        // 2a. Coarse tempo: autocorrelation at 1-4 beats, 100-200 BPM in 0.05 steps.
        auto maxLag = (int) std::ceil (4.0 * 60.0 / minBpm * fps) + 2;
        std::vector<double> autocorrelation ((size_t) maxLag + 1, 0.0);

        for (int lag = 0; lag <= maxLag; ++lag)
        {
            if (shouldStop())
                return {};

            double sum = 0.0;

            for (size_t i = 0; i + (size_t) lag < n; ++i)
                sum += (onsets[i] - mean) * (onsets[i + (size_t) lag] - mean);

            autocorrelation[(size_t) lag] = sum;
        }

        auto acfAt = [&] (double lag)
        {
            auto i = (int) lag;
            auto t = lag - i;
            return i + 1 <= maxLag ? autocorrelation[(size_t) i] * (1.0 - t) + autocorrelation[(size_t) i + 1] * t : 0.0;
        };

        auto coarse = minBpm;
        auto bestCoarse = -1.0e300;

        for (auto bpm = minBpm; bpm < maxBpm; bpm += 0.05)
        {
            auto lag = 60.0 / bpm * fps;
            auto score = acfAt (lag) + acfAt (2 * lag) + acfAt (3 * lag) + acfAt (4 * lag);

            if (score > bestCoarse)
            {
                bestCoarse = score;
                coarse = bpm;
            }
        }

        // 2b. Fine tempo: fold the onsets at each candidate period.
        auto measured = coarse;
        auto bestFine = -1.0;

        for (auto bpm = coarse - 0.6; bpm < coarse + 0.6; bpm += 0.002)
        {
            if (shouldStop())
                return {};

            auto period = 60.0 / bpm * fps;
            auto score = fold (onsets, onsetSum, period, juce::jmax (16, (int) std::round (period))).first;

            if (score > bestFine)
            {
                bestFine = score;
                measured = bpm;
            }
        }

        // 3-4. Whole-number BPM if its grid stays on the kicks; the grid on the kick starts.
        std::vector<float> peakRise (n, 0.0f);

        for (size_t i = 1; i < n; ++i)
            peakRise[i] = juce::jmax (0.0f, f.peak[i] - f.peak[i - 1]);

        Result result;
        result.measuredBpm = measured;
        result.bpm = std::round (measured);
        auto grid = fitGrid (onsets, onsetSum, peakRise, fps, result.bpm);
        result.driftMs = grid.driftMs;

        if (std::abs (grid.driftMs) > wholeNumberMaxDriftMs)
        {
            result.bpm = std::round (measured * 100.0) / 100.0;
            grid = fitGrid (onsets, onsetSum, peakRise, fps, result.bpm);
        }

        auto beatLength = 60.0 / result.bpm;
        result.firstBeat = std::fmod (grid.first, beatLength);

        if (result.firstBeat < 0.0)
            result.firstBeat += beatLength;

        // 5. Beat 1: the bar position where the spectrum changes most from the beat before (new
        //    chords and bass notes, fills, new sections all start on beat 1).
        std::array<double, 4> changeAt {};
        BandEnergies previous {};
        auto beatIndex = 0;

        // Changes far below the song's level (near-silent bands) don't count: 40 dB under its average.
        double meanEnergy = 0.0;

        for (auto& spectrum : f.spectra)
            for (auto e : spectrum)
                meanEnergy += e;

        auto floorEnergy = (float) (1.0e-4 * meanEnergy / juce::jmax ((size_t) 1, f.spectra.size() * (size_t) numBands)) + 1.0e-12f;

        for (auto beat = result.firstBeat; ; beat += beatLength, ++beatIndex)
        {
            // The spectra whose centre (half a window after they start) falls in this beat.
            auto halfWindow = fftSize / 2 / reader.sampleRate;
            auto a = (size_t) juce::jmax (0.0, std::ceil ((beat - halfWindow) * f.spectraPerSecond));
            auto z = (size_t) juce::jmax (0.0, std::ceil ((beat + beatLength - halfWindow) * f.spectraPerSecond));
            z = juce::jmax (z, a + 1);

            if (z > f.spectra.size())
                break;

            BandEnergies bands {};

            for (auto i = a; i < z; ++i)
                for (size_t b = 0; b < (size_t) numBands; ++b)
                    bands[b] += f.spectra[i][b];

            double change = 0.0;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                bands[b] = std::log10 (bands[b] / (float) (z - a) + floorEnergy);
                change += std::abs (bands[b] - previous[b]);
            }

            if (beatIndex > 0)
                changeAt[(size_t) (beatIndex % 4)] += change;

            previous = bands;
        }

        auto barPosition = (int) (std::max_element (changeAt.begin(), changeAt.end()) - changeAt.begin());
        result.downbeat = result.firstBeat + barPosition * beatLength;
        result.valid = true;
        return result;
    }
}
