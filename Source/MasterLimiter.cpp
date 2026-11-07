#include "MasterLimiter.h"

const char* MasterLimiterPlugin::xmlTypeName = "mixtopiaLimiter";

MasterLimiterPlugin::MasterLimiterPlugin (te::PluginCreationInfo info)
    : Plugin (info)
{
    ceilingDb.referTo (state, "ceilingDb", nullptr, -1.0f);
    driveDb.referTo (state, "driveDb", nullptr, 0.0f);
    releaseMs.referTo (state, "releaseMs", nullptr, 60.0f);
}

MasterLimiterPlugin::~MasterLimiterPlugin()
{
    notifyListenersOfDeletion();
}

juce::ValueTree MasterLimiterPlugin::create()
{
    return te::createValueTree (te::IDs::PLUGIN, te::IDs::type, xmlTypeName);
}

MasterLimiterPlugin* MasterLimiterPlugin::ensureOnMaster (te::Edit& edit)
{
    auto& list = edit.getMasterPluginList();

    for (auto* plugin : list)
        if (auto* limiter = dynamic_cast<MasterLimiterPlugin*> (plugin))
            return limiter;

    return dynamic_cast<MasterLimiterPlugin*> (list.insertPlugin (create(), -1).get());
}

void MasterLimiterPlugin::setPush (float pushDb)
{
    pushDb = juce::jlimit (0.0f, maxPushDb, pushDb);

    // Only write what differs: a CachedValue at its default writes the property anyway, which
    // would change every saved project.
    auto set = [] (juce::CachedValue<float>& value, float v) { if (value.get() != v) value = v; };
    set (ceilingDb, fixedCeilingDb);
    set (driveDb, pushDb);
    set (releaseMs, releaseForPush (pushDb));
}

void MasterLimiterPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    lookAhead = std::max (1, (int) std::lround (lookAheadSeconds * info.sampleRate));
    auto window = lookAhead + 1;

    delayL.assign ((size_t) lookAhead, 0.0f);
    delayR.assign ((size_t) lookAhead, 0.0f);
    required.assign ((size_t) window, 1.0f);
    minQueue.assign ((size_t) window + 1, 0);
    averageRing.assign ((size_t) window, 1.0f);

    for (auto& k : kWeighting)
        k.prepare (info.sampleRate);

    stepLength = std::max (1, (int) std::lround (info.sampleRate * 0.1));
    inputStepSum = 0.0;
    inStep = 0;
    reset();
}

void MasterLimiterPlugin::deinitialise()
{
}

void MasterLimiterPlugin::reset()
{
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    std::fill (required.begin(), required.end(), 1.0f);
    std::fill (averageRing.begin(), averageRing.end(), 1.0f);
    minHead = minTail = 0;
    sampleIndex = 0;
    averageSum = (double) averageRing.size();
    envelope = 1.0f;
}

float MasterLimiterPlugin::processSample (float& left, float& right, float ceiling, float drive, float releaseCoeff)
{
    const auto window = lookAhead + 1;
    const auto queueSize = (int) minQueue.size();
    const auto pos = (int) (sampleIndex % window);

    auto inL = left * drive, inR = right * drive;
    auto peak = std::max (std::abs (inL), std::abs (inR));
    auto needed = peak > ceiling ? ceiling / peak : 1.0f;

    // Sliding minimum of the needed gain over the last `window` samples (monotonic queue).
    required[(size_t) pos] = needed;

    while (minHead != minTail && required[(size_t) (minQueue[(size_t) ((minTail + queueSize - 1) % queueSize)] % window)] >= needed)
        minTail = (minTail + queueSize - 1) % queueSize;

    minQueue[(size_t) minTail] = sampleIndex;
    minTail = (minTail + 1) % queueSize;

    while (sampleIndex - minQueue[(size_t) minHead] >= window)
        minHead = (minHead + 1) % queueSize;

    auto minimum = required[(size_t) (minQueue[(size_t) minHead] % window)];

    // Drop instantly, recover at the release rate.
    envelope = minimum < envelope ? minimum : envelope + (minimum - envelope) * releaseCoeff;

    // Moving average over the window turns the drop into a smooth fade that has fully arrived by
    // the time the delayed peak comes out.
    averageSum += envelope - averageRing[(size_t) pos];
    averageRing[(size_t) pos] = envelope;
    auto gain = (float) (averageSum / window);

    // Delay the audio by the look-ahead.
    auto delayPos = (size_t) (sampleIndex % lookAhead);
    auto outL = delayL[delayPos], outR = delayR[delayPos];
    delayL[delayPos] = inL;
    delayR[delayPos] = inR;

    left = juce::jlimit (-ceiling, ceiling, outL * gain);
    right = juce::jlimit (-ceiling, ceiling, outR * gain);
    ++sampleIndex;
    return gain;
}

void MasterLimiterPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || lookAhead == 0)
        return;

    auto& buffer = *fc.destBuffer;
    auto ceiling = juce::Decibels::decibelsToGain (ceilingDb.get());
    auto drive = juce::Decibels::decibelsToGain (driveDb.get());
    auto releaseCoeff = (float) (1.0 - std::exp (-1.0 / (std::max (1.0f, releaseMs.get()) * 0.001 * sampleRate)));

    auto* left = buffer.getWritePointer (0, fc.bufferStartSample);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, fc.bufferStartSample) : nullptr;
    auto lowestGain = 1.0f;
    const bool measureLoudness = ! fc.isRendering;   // exports don't count towards the live readings

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        auto r = right != nullptr ? right[i] : left[i];

        if (measureLoudness)
        {
            auto il = kWeighting[0].process (left[i]), ir = kWeighting[1].process (r);
            inputStepSum += il * il + ir * ir;
        }

        lowestGain = std::min (lowestGain, processSample (left[i], r, ceiling, drive, releaseCoeff));

        if (right != nullptr)
            right[i] = r;

        if (measureLoudness && ++inStep == stepLength)
        {
            if (stepFifo.getFreeSpace() > 0)   // if the UI hasn't collected for a while, drop the step
                stepFifo.write (1).forEach ([this] (int index) { pendingSteps[(size_t) index] = inputStepSum / stepLength; });

            inputStepSum = 0.0;
            inStep = 0;
        }
    }

    storeMax (peakReductionDb, -juce::Decibels::gainToDecibels (lowestGain));
}

int MasterLimiterPlugin::takeLoudnessSteps (double* dest, int maxSteps)
{
    auto num = std::min (maxSteps, stepFifo.getNumReady());
    int i = 0;
    stepFifo.read (num).forEach ([&] (int index) { dest[i++] = pendingSteps[(size_t) index]; });
    return num;
}
