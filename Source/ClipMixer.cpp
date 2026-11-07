#include "ClipMixer.h"
#include "Chops.h"
#include "LevelMatch.h"
#include "Loops.h"

const char* ClipMixerPlugin::xmlTypeName = "mixtopiaClipMixer";

ClipMixerPlugin::ClipMixerPlugin (te::PluginCreationInfo info)
    : Plugin (info)
{
    edit.state.addListener (&watcher);
}

ClipMixerPlugin::~ClipMixerPlugin()
{
    edit.state.removeListener (&watcher);
    watcher.cancelPendingUpdate();
    notifyListenersOfDeletion();
}

juce::ValueTree ClipMixerPlugin::create()
{
    return te::createValueTree (te::IDs::PLUGIN, te::IDs::type, xmlTypeName);
}

ClipMixerPlugin* ClipMixerPlugin::ensureOnClip (te::AudioClipBase& clip)
{
    auto* list = clip.getPluginList();

    if (list == nullptr)
        return nullptr;

    for (auto* plugin : *list)
        if (auto* mixer = dynamic_cast<ClipMixerPlugin*> (plugin))
            return mixer;

    return dynamic_cast<ClipMixerPlugin*> (list->insertPlugin (create(), 0).get());
}

void ClipMixerPlugin::updateSnapshot()
{
    auto fresh = std::make_unique<Snapshot>();

    if (auto* clip = dynamic_cast<te::AudioClipBase*> (getOwnerClip()))
    {
        for (int p = 0; p < mixtopia::envelopes::numParams; ++p)
            for (auto& point : mixtopia::loops::getEffectivePoints (clip->state, (mixtopia::envelopes::Param) p))   // (with a loop's copies)
                fresh->points[(size_t) p].push_back ({ mixtopia::tempo::sourceToEditSeconds (*clip, point.sourceTime), point.db });

        fresh->usesEq = ! (fresh->points[1].empty() && fresh->points[2].empty() && fresh->points[3].empty());
        fresh->usesFilter = ! fresh->points[(size_t) mixtopia::envelopes::Param::filter].empty();
        fresh->fadeIn = mixtopia::chops::hasFadeIn (*clip);
        fresh->fadeOut = mixtopia::chops::hasFadeOut (*clip);
        fresh->clipStart = clip->getPosition().getStart().inSeconds();
        fresh->clipEnd = clip->getPosition().getEnd().inSeconds();
        fresh->matchGain = juce::Decibels::decibelsToGain ((float) mixtopia::levelmatch::getAppliedGainDb (*clip));
    }

    {
        const juce::SpinLock::ScopedLockType lock (snapshotLock);
        std::swap (snapshot, fresh);
    }
    // The old snapshot is freed here, on the message thread.
}

void ClipMixerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
    wetSmoothing = (float) std::exp (-1.0 / (wetSeconds * sampleRate));

    for (int i = 0; i < 2; ++i)
    {
        filters[(size_t) i].setLowPass (info.sampleRate, bassCrossoverHz);
        filters[(size_t) i + 2].setHighPass (info.sampleRate, bassCrossoverHz);
        filters[(size_t) i + 4].setLowPass (info.sampleRate, trebleCrossoverHz);
        filters[(size_t) i + 6].setHighPass (info.sampleRate, trebleCrossoverHz);
        filters[(size_t) i + 8].setLowPass (info.sampleRate, trebleCrossoverHz);
        filters[(size_t) i + 10].setHighPass (info.sampleRate, trebleCrossoverHz);
    }

    for (auto& k : kWeighting)
        k.prepare (info.sampleRate);

    stepLength = std::max (1, (int) std::lround (info.sampleRate * 0.1));
    reset();
    updateSnapshot();
}

void ClipMixerPlugin::deinitialise()
{
}

void ClipMixerPlugin::reset()
{
    for (auto& filter : filters)
        filter.clear();

    hasLastGains = false;
    eqRunning = false;
    filterRunning = false;
    wetNow = -1.0f;
    for (auto& stage : lowPassStages)   stage.clear();
    for (auto& stage : highPassStages)  stage.clear();

    for (auto& k : kWeighting)
        k.reset();

    stepSum = 0.0;
    inStep = 0;
}

void ClipMixerPlugin::meter (const juce::AudioBuffer<float>& buffer, int start, int num)
{
    auto* left = buffer.getReadPointer (0, start);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1, start) : left;   // mono: both speakers

    for (int i = 0; i < num; ++i)
    {
        auto l = kWeighting[0].process (left[i]), r = kWeighting[1].process (right[i]);
        stepSum += l * l + r * r;

        if (++inStep == stepLength)
        {
            if (stepFifo.getFreeSpace() > 0)   // if nobody collects, drop
                stepFifo.write (1).forEach ([this] (int index) { pendingSteps[(size_t) index] = stepSum / stepLength; });

            stepSum = 0.0;
            inStep = 0;
        }
    }
}

int ClipMixerPlugin::takeLoudnessSteps (double* dest, int maxSteps)
{
    auto num = std::min (maxSteps, stepFifo.getNumReady());
    int i = 0;
    stepFifo.read (num).forEach ([&] (int index) { dest[i++] = pendingSteps[(size_t) index]; });
    return num;
}

std::array<float, mixtopia::envelopes::numParams> ClipMixerPlugin::valuesAt (const Snapshot& s, double editSeconds) const
{
    std::array<float, mixtopia::envelopes::numParams> gains;

    for (size_t p = 0; p < gains.size(); ++p)
    {
        auto value = mixtopia::envelopes::evaluate (s.points[p], editSeconds, [] (const auto& point) { return point.first; },
                                                    [] (const auto& point) { return point.second; });
        gains[p] = p == (size_t) mixtopia::envelopes::Param::filter ? value   // (a position, not dB)
                                                                     : mixtopia::envelopes::toGain (value);
    }

    gains[0] *= s.matchGain;
    return gains;
}

void ClipMixerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto end = lastGains;
    auto usesEq = eqRunning;
    auto usesFilter = filterRunning;

    {
        const juce::SpinLock::ScopedTryLockType lock (snapshotLock);

        if (lock.isLocked() && snapshot != nullptr)
        {
            end = valuesAt (*snapshot, fc.editTime.getEnd().inSeconds());
            usesEq = snapshot->usesEq;
            usesFilter = snapshot->usesFilter;
            fades.fadeIn = snapshot->fadeIn;
            fades.fadeOut = snapshot->fadeOut;
            fades.clipStart = snapshot->clipStart;
            fades.clipEnd = snapshot->clipEnd;

            if (! hasLastGains)
                lastGains = valuesAt (*snapshot, fc.editTime.getStart().inSeconds());
        }
    }

    // Ramp every value from where the last block ended (or, after a reset, from this block's
    // start value, set above) to this block's end value.
    auto start = lastGains;
    hasLastGains = true;
    lastGains = end;

    if (usesEq && ! eqRunning)
        for (auto& filter : filters)
            filter.clear();

    eqRunning = usesEq;

    // Nothing to do without EQ and at unity gain: bit-exact pass-through.
    if (usesEq || start[0] != 1.0f || end[0] != 1.0f)
        process (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples, start, end, usesEq);

    if (usesFilter && ! filterRunning)
    {
        for (auto& stage : lowPassStages)   stage.clear();
        for (auto& stage : highPassStages)  stage.clear();
        wetNow = -1.0f;
    }

    filterRunning = usesFilter;
    constexpr auto filterIndex = (size_t) mixtopia::envelopes::Param::filter;

    if (usesFilter)
        applyFilter (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples, start[filterIndex], end[filterIndex]);

    if (fades.fadeIn || fades.fadeOut)
        applyEdgeFades (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples, fc.editTime, fades);

    if (! fc.isRendering)   // exports don't feed the live meters
        meter (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);
}

void ClipMixerPlugin::process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                               const Gains& start, const Gains& end, bool usesEq)
{
    auto numChannels = std::min (2, buffer.getNumChannels());
    auto f = [this] (size_t index, float x, int ch) { return filters[index].process (x, ch); };

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = buffer.getWritePointer (ch, startSample);

        for (int i = 0; i < numSamples; ++i)
        {
            auto alpha = (float) (i + 1) / (float) numSamples;
            auto gain = [&] (size_t p) { return start[p] + alpha * (end[p] - start[p]); };
            auto x = data[i];

            if (usesEq)
            {
                auto bass = f (1, f (0, x, ch), ch);
                auto upper = f (3, f (2, x, ch), ch);
                auto mids = f (5, f (4, upper, ch), ch);
                auto treble = f (7, f (6, upper, ch), ch);
                auto alignedBass = f (9, f (8, bass, ch), ch) + f (11, f (10, bass, ch), ch);
                x = gain (1) * alignedBass + gain (2) * mids + gain (3) * treble;
            }

            data[i] = x * gain (0);
        }
    }
}

void ClipMixerPlugin::applyEdgeFades (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, te::TimeRange editTime, const Snapshot& s)
{
    // Linear over fadeSeconds, from silence at the edge itself; untouched elsewhere.
    constexpr auto length = mixtopia::chops::fadeSeconds;
    auto from = editTime.getStart().inSeconds();
    auto step = editTime.getLength().inSeconds() / numSamples;
    auto inFadeIn = s.fadeIn && from < s.clipStart + length;
    auto inFadeOut = s.fadeOut && from + editTime.getLength().inSeconds() > s.clipEnd - length;

    if (! inFadeIn && ! inFadeOut)
        return;

    auto numChannels = std::min (2, buffer.getNumChannels());

    for (int i = 0; i < numSamples; ++i)
    {
        auto t = from + i * step;
        auto gain = 1.0;

        if (s.fadeIn)
            gain = std::min (gain, (t - s.clipStart) / length);

        if (s.fadeOut)
            gain = std::min (gain, (s.clipEnd - t) / length);

        if (gain >= 1.0)
            continue;

        gain = std::max (0.0, gain);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch, startSample)[i] *= (float) gain;
    }
}

void ClipMixerPlugin::Svf::setCutoff (double hz, double q, double rate)
{
    // Simper's trapezoidal SVF coefficients (cutoff kept under Nyquist).
    auto g = std::tan (juce::MathConstants<double>::pi * std::min (hz, 0.45 * rate) / rate);
    auto damping = 1.0 / q;
    auto b1 = 1.0 / (1.0 + g * (g + damping));
    k = (float) damping;
    a1 = (float) b1;
    a2 = (float) (g * b1);
    a3 = (float) (g * g * b1);
}

void ClipMixerPlugin::applyFilter (juce::AudioBuffer<float>& buffer, int startSample, int numSamples, float startPosition, float endPosition)
{
    namespace env = mixtopia::envelopes;
    auto numChannels = std::min (2, buffer.getNumChannels());
    auto steady = startPosition == endPosition;
    auto targetWet = env::filterWet (endPosition);

    if (wetNow < 0.0f)   // (just started: no fade in from nothing)
        wetNow = env::filterWet (startPosition);

    // Each stage: its cutoff and resonance at a position (the unused side fully open, no resonance).
    auto setStages = [this] (float position)
    {
        auto lowHz = env::lowPassHz (position), highHz = env::highPassHz (position);
        lowPassStages[0].setCutoff (lowHz, position < 0.0f ? env::filterQ (position) : env::openQ, sampleRate);
        highPassStages[0].setCutoff (highHz, position > 0.0f ? env::filterQ (position) : env::openQ, sampleRate);
        lowPassStages[1].setCutoff (lowHz, env::fixedQ, sampleRate);
        highPassStages[1].setCutoff (highHz, env::fixedQ, sampleRate);
    };

    if (steady)
        setStages (endPosition);

    for (int i = 0; i < numSamples; ++i)
    {
        // The cutoffs follow the line on every sample (no steps you could hear on a fast sweep).
        if (! steady)
        {
            auto position = startPosition + (float) (i + 1) / (float) numSamples * (endPosition - startPosition);
            setStages (position);
            targetWet = env::filterWet (position);
        }

        // The blend follows smoothly, and lands exactly on 0 (untouched) or 1 (all filtered).
        wetNow = targetWet + wetSmoothing * (wetNow - targetWet);

        if (std::abs (wetNow - targetWet) < 1.0e-5f)
            wetNow = targetWet;

        auto wet = wetNow;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch, startSample);
            auto dry = data[i];
            auto filtered = dry;

            for (size_t n = 0; n < lowPassStages.size(); ++n)
            {
                auto& lp = lowPassStages[n];
                auto v3 = filtered - lp.ic2[ch];
                auto v1 = lp.a1 * lp.ic1[ch] + lp.a2 * v3;
                auto v2 = lp.ic2[ch] + lp.a2 * lp.ic1[ch] + lp.a3 * v3;
                lp.ic1[ch] = 2.0f * v1 - lp.ic1[ch];
                lp.ic2[ch] = 2.0f * v2 - lp.ic2[ch];
                filtered = v2;   // the low-passed output
            }

            for (size_t n = 0; n < highPassStages.size(); ++n)
            {
                auto& hp = highPassStages[n];
                auto w3 = filtered - hp.ic2[ch];
                auto w1 = hp.a1 * hp.ic1[ch] + hp.a2 * w3;
                auto w2 = hp.ic2[ch] + hp.a2 * hp.ic1[ch] + hp.a3 * w3;
                hp.ic1[ch] = 2.0f * w1 - hp.ic1[ch];
                hp.ic2[ch] = 2.0f * w2 - hp.ic2[ch];
                filtered = filtered - hp.k * w1 - w2;   // the high-passed output
            }

            data[i] = wet >= 1.0f ? filtered : dry + wet * (filtered - dry);
        }
    }
}
