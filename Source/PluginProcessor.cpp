#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

namespace
{
    constexpr std::array<float, EchoplasmAudioProcessor::kNumBands + 1> kLogBandEdges = {
        20.0f, 115.0f, 200.0f, 315.0f,
        500.0f, 750.0f, 1100.0f, 1600.0f,
        2300.0f, 3200.0f, 4400.0f, 6000.0f,
        8200.0f, 11000.0f, 13500.0f, 16000.0f,
        20000.0f
    };

    const std::array<const char*, EchoplasmAudioProcessor::kNumBands> kBandLabels = {
        "20-115 Hz", "115-200", "200-315", "315-500",
        "500-750", "750-1.1k", "1.1k-1.6k", "1.6k-2.3k",
        "2.3k-3.2k", "3.2k-4.4k", "4.4k-6.0k", "6.0k-8.2k",
        "8.2k-11k", "11k-13.5k", "13.5k-16k", "16k-20k"
    };
}

EchoplasmAudioProcessor::EchoplasmAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
#else
    : apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
#endif
{
    mixParam = apvts.getRawParameterValue("mix");
    feedbackParam = apvts.getRawParameterValue("feedback");
    outputParam = apvts.getRawParameterValue("output");
    opacityParam = apvts.getRawParameterValue("opacity");
    freezeParam = apvts.getRawParameterValue("freeze");
    bypassParam = apvts.getRawParameterValue("bypass");

    for (auto& frame : frames)
        for (auto& row : frame)
            row.fill(SwatchWhite);
}

EchoplasmAudioProcessor::~EchoplasmAudioProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout EchoplasmAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("mix", 1),
        "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("feedback", 1),
        "Feedback",
        juce::NormalisableRange<float>(0.0f, 95.0f, 0.1f),
        25.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("output", 1),
        "Output",
        juce::NormalisableRange<float>(-24.0f, 12.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID("opacity", 1),
        "Opacity",
        juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("freeze", 1),
        "Freeze",
        false));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID("bypass", 1),
        "Bypass",
        false));

    return { params.begin(), params.end() };
}

const std::array<float, EchoplasmAudioProcessor::kNumBands + 1>& EchoplasmAudioProcessor::getBandEdges() noexcept
{
    return kLogBandEdges;
}

juce::String EchoplasmAudioProcessor::getBandLabel(int bandIndex)
{
    if (bandIndex >= 0 && bandIndex < kNumBands)
        return kBandLabels[static_cast<size_t>(bandIndex)];
    return {};
}

float EchoplasmAudioProcessor::swatchToGain(int swatchIndex) noexcept
{
    switch (swatchIndex)
    {
        case SwatchWhite:     return 0.00f;
        case SwatchLightGray: return 0.15f;
        case SwatchDarkGray:  return 0.85f;
        case SwatchBlack:     return 1.00f;
        case SwatchYellow:    return 0.25f;
        case SwatchOrange:    return 0.50f;
        case SwatchRed:       return 0.75f;
        case SwatchGreen:     return 0.35f;
        case SwatchBlue:      return 0.65f;
        default:              return 0.00f;
    }
}

juce::Colour EchoplasmAudioProcessor::swatchToColour(int swatchIndex) noexcept
{
    switch (swatchIndex)
    {
        case SwatchWhite:     return juce::Colour(0xFFFFFFFF);
        case SwatchLightGray: return juce::Colour(0xFFC0C0C0);
        case SwatchDarkGray:  return juce::Colour(0xFF808080);
        case SwatchBlack:     return juce::Colour(0xFF000000);
        case SwatchYellow:    return juce::Colour(0xFFFFE600);
        case SwatchOrange:    return juce::Colour(0xFFFF8C00);
        case SwatchRed:       return juce::Colour(0xFFE62222);
        case SwatchGreen:     return juce::Colour(0xFF28A745);
        case SwatchBlue:      return juce::Colour(0xFF1F6FEB);
        default:              return juce::Colour(0xFFFFFFFF);
    }
}

const juce::String EchoplasmAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool EchoplasmAudioProcessor::acceptsMidi() const { return false; }
bool EchoplasmAudioProcessor::producesMidi() const { return false; }
bool EchoplasmAudioProcessor::isMidiEffect() const { return false; }

double EchoplasmAudioProcessor::getTailLengthSeconds() const
{
    return 4.0;
}

int EchoplasmAudioProcessor::getNumPrograms() { return 1; }
int EchoplasmAudioProcessor::getCurrentProgram() { return 0; }
void EchoplasmAudioProcessor::setCurrentProgram(int) {}
const juce::String EchoplasmAudioProcessor::getProgramName(int) { return {}; }
void EchoplasmAudioProcessor::changeProgramName(int, const juce::String&) {}

juce::AudioProcessorParameter* EchoplasmAudioProcessor::getBypassParameter() const
{
    return apvts.getParameter("bypass");
}

void EchoplasmAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = (sampleRate > 8000.0) ? sampleRate : 44100.0;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(juce::jmax(1, samplesPerBlock));
    spec.numChannels = 2;

    const float nyquist = static_cast<float>(currentSampleRate * 0.48);

    for (int b = 0; b < kNumBands; ++b)
    {
        const float fLow = juce::jmin(kLogBandEdges[static_cast<size_t>(b)], nyquist * 0.85f);
        const float fHigh = juce::jmin(kLogBandEdges[static_cast<size_t>(b + 1)], nyquist);
        const float centerFreq = juce::jlimit(25.0f, nyquist, std::sqrt(fLow * fHigh));
        const float bandwidth = juce::jmax(18.0f, fHigh - fLow);
        const float q = juce::jlimit(0.65f, 4.5f, centerFreq / bandwidth);

        auto& filter = bandFilters[static_cast<size_t>(b)];
        filter.prepare(spec);
        filter.reset();
        filter.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
        filter.setCutoffFrequency(centerFreq);
        filter.setResonance(q);

        // Normalize SVF bandpass resonance boost and overlapping filterbank energy
        bandNormGains[static_cast<size_t>(b)] = 0.55f / std::max(0.75f, q);
    }

    // Allocate up to 10 seconds per band delay line for slow BPMs
    const int maxDelaySamples = static_cast<int>(currentSampleRate * 10.0) + 16;
    for (auto& dl : delayLines)
        dl.prepare(maxDelaySamples);

    for (int b = 0; b < kNumBands; ++b)
    {
        for (int t = 0; t < kNumTaps; ++t)
        {
            tapGainSmoothers[static_cast<size_t>(b)][static_cast<size_t>(t)].reset(currentSampleRate, 0.020);
            tapGainSmoothers[static_cast<size_t>(b)][static_cast<size_t>(t)].setCurrentAndTargetValue(0.0f);
        }
    }

    const double initialBpm = lastHostBpm.load(std::memory_order_relaxed);
    const double secondsPerBeat = 60.0 / initialBpm;
    for (int t = 0; t < kNumTaps; ++t)
    {
        const float tapSamples = static_cast<float>((static_cast<double>(t + 1) * 0.25) * secondsPerBeat * currentSampleRate);
        tapDelaySmoothers[static_cast<size_t>(t)].reset(currentSampleRate, 0.040);
        tapDelaySmoothers[static_cast<size_t>(t)].setCurrentAndTargetValue(tapSamples);
    }

    mixSmoother.reset(currentSampleRate, 0.015);
    mixSmoother.setCurrentAndTargetValue(mixParam->load() * 0.01f);

    feedbackSmoother.reset(currentSampleRate, 0.015);
    feedbackSmoother.setCurrentAndTargetValue(feedbackParam->load() * 0.01f);

    outputGainSmoother.reset(currentSampleRate, 0.015);
    outputGainSmoother.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputParam->load()));

    frameSampleAccumulator = 0.0;
    abstractFifo.reset();
    fifoData.fill(0.0f);
}

void EchoplasmAudioProcessor::releaseResources()
{
    for (auto& filter : bandFilters)
        filter.reset();
    for (auto& dl : delayLines)
        dl.reset();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool EchoplasmAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainIn = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    if (mainIn != mainOut)
        return false;

    return true;
}
#endif

void EchoplasmAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    if (numSamples <= 0)
        return;

    const bool bypassed = (bypassParam != nullptr) && (bypassParam->load() > 0.5f);
    if (bypassed)
    {
        if (totalNumInputChannels > 0)
        {
            const float* ch0 = buffer.getReadPointer(0);
            const float* ch1 = (totalNumInputChannels > 1) ? buffer.getReadPointer(1) : ch0;
            std::array<float, 512> monoScratch {};
            int offset = 0;
            while (offset < numSamples)
            {
                const int chunk = juce::jmin(512, numSamples - offset);
                for (int i = 0; i < chunk; ++i)
                    monoScratch[static_cast<size_t>(i)] = 0.5f * (ch0[offset + i] + ch1[offset + i]);
                pushSamplesToFifo(monoScratch.data(), chunk);
                offset += chunk;
            }
        }
        return;
    }

    // Read host BPM via JUCE 8 AudioPlayHead::getPosition()
    double bpm = 120.0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto hostBpm = pos->getBpm())
            {
                if (*hostBpm >= 20.0 && *hostBpm <= 400.0)
                    bpm = *hostBpm;
            }
        }
    }
    lastHostBpm.store(bpm, std::memory_order_relaxed);

    // Advance Flipbook Frame Sequence / Loop
    if (sequenceActive.load(std::memory_order_relaxed))
    {
        const int count = juce::jlimit(1, kMaxFrames, numFrames.load(std::memory_order_relaxed));
        if (count > 1)
        {
            const double frameDurationSamples = (static_cast<double>(frameTimeMs.load(std::memory_order_relaxed)) * 0.001) * currentSampleRate;
            frameSampleAccumulator += static_cast<double>(numSamples);

            if (frameSampleAccumulator >= frameDurationSamples)
            {
                frameSampleAccumulator = std::fmod(frameSampleAccumulator, frameDurationSamples);
                int cur = activeFrame.load(std::memory_order_relaxed);
                int next = cur + 1;
                if (next >= count)
                {
                    if (loopActive.load(std::memory_order_relaxed))
                        next = 0;
                    else
                    {
                        next = count - 1;
                        sequenceActive.store(false, std::memory_order_relaxed);
                    }
                }
                activeFrame.store(next, std::memory_order_relaxed);
            }
        }
    }

    // Snapshot active frame safely
    FrameGrid currentGrid {};
    {
        const int curIdx = juce::jlimit(0, kMaxFrames - 1, activeFrame.load(std::memory_order_relaxed));
        const juce::SpinLock::ScopedLockType lock(frameLock);
        currentGrid = frames[static_cast<size_t>(curIdx)];
    }

    const float opacityNorm = opacityParam->load() * 0.01f;
    for (int b = 0; b < kNumBands; ++b)
    {
        for (int t = 0; t < kNumTaps; ++t)
        {
            const float targetGain = swatchToGain(currentGrid[static_cast<size_t>(b)][static_cast<size_t>(t)]) * opacityNorm;
            tapGainSmoothers[static_cast<size_t>(b)][static_cast<size_t>(t)].setTargetValue(targetGain);
        }
    }

    // Compute delay tap positions (0 to 4 beats in Sync mode, or evenly spaced ms in Free mode)
    const bool syncMode = tempoSync.load(std::memory_order_relaxed);
    const double secondsPerBeat = 60.0 / bpm;
    const double totalSpanSeconds = syncMode ? (4.0 * secondsPerBeat) : (static_cast<double>(freeTimeMs.load(std::memory_order_relaxed)) * 0.001);

    for (int t = 0; t < kNumTaps; ++t)
    {
        const double tapRatio = static_cast<double>(t + 1) / static_cast<double>(kNumTaps);
        const float tapSamples = static_cast<float>(tapRatio * totalSpanSeconds * currentSampleRate);
        tapDelaySmoothers[static_cast<size_t>(t)].setTargetValue(tapSamples);
    }

    mixSmoother.setTargetValue(mixParam->load() * 0.01f);
    feedbackSmoother.setTargetValue(juce::jlimit(0.0f, 0.95f, feedbackParam->load() * 0.01f));
    outputGainSmoother.setTargetValue(juce::Decibels::decibelsToGain(outputParam->load()));
    const bool frozen = freezeParam->load() > 0.5f;

    float* leftPtr = buffer.getWritePointer(0);
    float* rightPtr = (totalNumOutputChannels > 1) ? buffer.getWritePointer(1) : nullptr;

    std::array<float, 512> fifoScratch {};
    int fifoScratchPos = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float dryL = leftPtr[i];
        const float dryR = (rightPtr != nullptr) ? rightPtr[i] : dryL;

        const float mix = mixSmoother.getNextValue();
        const float fb = feedbackSmoother.getNextValue();
        const float outGain = outputGainSmoother.getNextValue();

        std::array<float, kNumTaps> currentTapDelays {};
        for (int t = 0; t < kNumTaps; ++t)
            currentTapDelays[static_cast<size_t>(t)] = tapDelaySmoothers[static_cast<size_t>(t)].getNextValue();

        float wetSumL = 0.0f;
        float wetSumR = 0.0f;

        for (int b = 0; b < kNumBands; ++b)
        {
            auto& dl = delayLines[static_cast<size_t>(b)];
            auto& filter = bandFilters[static_cast<size_t>(b)];

            const float norm = bandNormGains[static_cast<size_t>(b)];
            const float bandInL = filter.processSample(0, dryL) * norm;
            const float bandInR = filter.processSample(1, dryR) * norm;

            float bandTapSumL = 0.0f;
            float bandTapSumR = 0.0f;
            float tapWeightSum = 0.0f;

            for (int t = 0; t < kNumTaps; ++t)
            {
                const float g = tapGainSmoothers[static_cast<size_t>(b)][static_cast<size_t>(t)].getNextValue();
                if (g > 0.0001f)
                {
                    float tapL = 0.0f;
                    float tapR = 0.0f;
                    dl.readInterpolated(currentTapDelays[static_cast<size_t>(t)], tapL, tapR);
                    bandTapSumL += tapL * g;
                    bandTapSumR += tapR * g;
                    tapWeightSum += g;
                }
            }

            // Gentle normalization when many taps in a band are painted simultaneously
            const float tapScale = (tapWeightSum > 1.5f) ? (1.5f / tapWeightSum) : 1.0f;
            bandTapSumL *= tapScale;
            bandTapSumR *= tapScale;

            wetSumL += bandTapSumL;
            wetSumR += bandTapSumR;

            if (frozen)
            {
                // Freeze locks delay buffers by recirculating the 4-beat span without new input
                float frozenL = 0.0f;
                float frozenR = 0.0f;
                const float loopDelay = juce::jmax(16.0f, currentTapDelays[kNumTaps - 1]);
                dl.readInterpolated(loopDelay, frozenL, frozenR);
                dl.writeSample(frozenL, frozenR);
            }
            else
            {
                const float writeL = std::tanh(bandInL + bandTapSumL * fb);
                const float writeR = std::tanh(bandInR + bandTapSumR * fb);
                dl.writeSample(writeL, writeR);
            }

            dl.advance();
        }

        const float outL = ((dryL * (1.0f - mix)) + (wetSumL * mix)) * outGain;
        const float outR = ((dryR * (1.0f - mix)) + (wetSumR * mix)) * outGain;

        leftPtr[i] = outL;
        if (rightPtr != nullptr)
            rightPtr[i] = outR;

        fifoScratch[static_cast<size_t>(fifoScratchPos++)] = 0.5f * (outL + outR);
        if (fifoScratchPos == 512)
        {
            pushSamplesToFifo(fifoScratch.data(), 512);
            fifoScratchPos = 0;
        }
    }

    if (fifoScratchPos > 0)
        pushSamplesToFifo(fifoScratch.data(), fifoScratchPos);
}

void EchoplasmAudioProcessor::pushSamplesToFifo(const float* samples, int numSamples) noexcept
{
    const auto scope = abstractFifo.write(numSamples);
    if (scope.blockSize1 > 0)
        std::copy(samples, samples + scope.blockSize1, fifoData.begin() + scope.startIndex1);
    if (scope.blockSize2 > 0)
        std::copy(samples + scope.blockSize1, samples + scope.blockSize1 + scope.blockSize2, fifoData.begin() + scope.startIndex2);
}

int EchoplasmAudioProcessor::pullSamplesForFFT(float* destination, int maxSamples)
{
    const int ready = juce::jmin(maxSamples, abstractFifo.getNumReady());
    if (ready <= 0)
        return 0;

    const auto scope = abstractFifo.read(ready);
    if (scope.blockSize1 > 0)
        std::copy(fifoData.begin() + scope.startIndex1, fifoData.begin() + scope.startIndex1 + scope.blockSize1, destination);
    if (scope.blockSize2 > 0)
        std::copy(fifoData.begin() + scope.startIndex2, fifoData.begin() + scope.startIndex2 + scope.blockSize2, destination + scope.blockSize1);

    return ready;
}

void EchoplasmAudioProcessor::setCell(int frameIndex, int band, int tap, int swatchIndex)
{
    if (frameIndex < 0 || frameIndex >= kMaxFrames || band < 0 || band >= kNumBands || tap < 0 || tap >= kNumTaps)
        return;

    const int clamped = juce::jlimit(0, kNumSwatches - 1, swatchIndex);
    const juce::SpinLock::ScopedLockType lock(frameLock);
    frames[static_cast<size_t>(frameIndex)][static_cast<size_t>(band)][static_cast<size_t>(tap)] = clamped;
}

int EchoplasmAudioProcessor::getCell(int frameIndex, int band, int tap) const
{
    if (frameIndex < 0 || frameIndex >= kMaxFrames || band < 0 || band >= kNumBands || tap < 0 || tap >= kNumTaps)
        return SwatchWhite;

    const juce::SpinLock::ScopedLockType lock(frameLock);
    return frames[static_cast<size_t>(frameIndex)][static_cast<size_t>(band)][static_cast<size_t>(tap)];
}

EchoplasmAudioProcessor::FrameGrid EchoplasmAudioProcessor::getFrameCopy(int frameIndex) const
{
    const int idx = juce::jlimit(0, kMaxFrames - 1, frameIndex);
    const juce::SpinLock::ScopedLockType lock(frameLock);
    return frames[static_cast<size_t>(idx)];
}

void EchoplasmAudioProcessor::setFrameData(int frameIndex, const FrameGrid& grid)
{
    if (frameIndex < 0 || frameIndex >= kMaxFrames)
        return;

    const juce::SpinLock::ScopedLockType lock(frameLock);
    frames[static_cast<size_t>(frameIndex)] = grid;
}

void EchoplasmAudioProcessor::clearFrame(int frameIndex)
{
    if (frameIndex < 0 || frameIndex >= kMaxFrames)
        return;

    const juce::SpinLock::ScopedLockType lock(frameLock);
    for (auto& row : frames[static_cast<size_t>(frameIndex)])
        row.fill(SwatchWhite);
}

void EchoplasmAudioProcessor::clearAllFrames()
{
    const juce::SpinLock::ScopedLockType lock(frameLock);
    for (auto& frame : frames)
        for (auto& row : frame)
            row.fill(SwatchWhite);
    numFrames.store(1, std::memory_order_relaxed);
    activeFrame.store(0, std::memory_order_relaxed);
    sequenceActive.store(false, std::memory_order_relaxed);
}

void EchoplasmAudioProcessor::setNumFrames(int count)
{
    const int clamped = juce::jlimit(1, kMaxFrames, count);
    numFrames.store(clamped, std::memory_order_relaxed);
    if (activeFrame.load(std::memory_order_relaxed) >= clamped)
        activeFrame.store(clamped - 1, std::memory_order_relaxed);
}

int EchoplasmAudioProcessor::addFrame(bool duplicateCurrent)
{
    const int curCount = numFrames.load(std::memory_order_relaxed);
    if (curCount >= kMaxFrames)
        return curCount - 1;

    const int newIdx = curCount;
    {
        const juce::SpinLock::ScopedLockType lock(frameLock);
        if (duplicateCurrent)
        {
            const int srcIdx = juce::jlimit(0, curCount - 1, activeFrame.load(std::memory_order_relaxed));
            frames[static_cast<size_t>(newIdx)] = frames[static_cast<size_t>(srcIdx)];
        }
        else
        {
            for (auto& row : frames[static_cast<size_t>(newIdx)])
                row.fill(SwatchWhite);
        }
    }

    numFrames.store(curCount + 1, std::memory_order_relaxed);
    activeFrame.store(newIdx, std::memory_order_relaxed);
    return newIdx;
}

void EchoplasmAudioProcessor::removeFrame(int index)
{
    const int curCount = numFrames.load(std::memory_order_relaxed);
    if (curCount <= 1 || index < 0 || index >= curCount)
        return;

    {
        const juce::SpinLock::ScopedLockType lock(frameLock);
        for (int i = index; i < curCount - 1; ++i)
            frames[static_cast<size_t>(i)] = frames[static_cast<size_t>(i + 1)];

        for (auto& row : frames[static_cast<size_t>(curCount - 1)])
            row.fill(SwatchWhite);
    }

    const int newCount = curCount - 1;
    numFrames.store(newCount, std::memory_order_relaxed);
    if (activeFrame.load(std::memory_order_relaxed) >= newCount)
        activeFrame.store(newCount - 1, std::memory_order_relaxed);
}

void EchoplasmAudioProcessor::setActiveFrame(int index)
{
    const int count = juce::jlimit(1, kMaxFrames, numFrames.load(std::memory_order_relaxed));
    activeFrame.store(juce::jlimit(0, count - 1, index), std::memory_order_relaxed);
}

void EchoplasmAudioProcessor::setSequenceActive(bool shouldPlay)
{
    frameSampleAccumulator = 0.0;
    sequenceActive.store(shouldPlay, std::memory_order_relaxed);
}

bool EchoplasmAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* EchoplasmAudioProcessor::createEditor()
{
    return new EchoplasmAudioProcessorEditor(*this);
}

void EchoplasmAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    state.setProperty("numFrames", numFrames.load(std::memory_order_relaxed), nullptr);
    state.setProperty("activeFrame", activeFrame.load(std::memory_order_relaxed), nullptr);
    state.setProperty("sequenceActive", sequenceActive.load(std::memory_order_relaxed), nullptr);
    state.setProperty("loopActive", loopActive.load(std::memory_order_relaxed), nullptr);
    state.setProperty("frameTimeMs", frameTimeMs.load(std::memory_order_relaxed), nullptr);
    state.setProperty("tempoSync", tempoSync.load(std::memory_order_relaxed), nullptr);
    state.setProperty("freeTimeMs", freeTimeMs.load(std::memory_order_relaxed), nullptr);

    juce::ValueTree framesTree("Frames");
    {
        const juce::SpinLock::ScopedLockType lock(frameLock);
        const int count = juce::jlimit(1, kMaxFrames, numFrames.load(std::memory_order_relaxed));
        for (int f = 0; f < count; ++f)
        {
            juce::String encoded;
            encoded.preallocateBytes(kNumBands * kNumTaps);
            for (int b = 0; b < kNumBands; ++b)
                for (int t = 0; t < kNumTaps; ++t)
                    encoded += juce::String(frames[static_cast<size_t>(f)][static_cast<size_t>(b)][static_cast<size_t>(t)]);

            juce::ValueTree frameNode("Frame");
            frameNode.setProperty("index", f, nullptr);
            frameNode.setProperty("data", encoded, nullptr);
            framesTree.appendChild(frameNode, nullptr);
        }
    }
    state.appendChild(framesTree, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void EchoplasmAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState == nullptr)
        return;

    auto tree = juce::ValueTree::fromXml(*xmlState);
    if (!tree.isValid())
        return;

    // Extract custom properties and frames before replacing APVTS state
    const int loadedNumFrames = juce::jlimit(1, kMaxFrames, static_cast<int>(tree.getProperty("numFrames", 1)));
    const int loadedActive = juce::jlimit(0, loadedNumFrames - 1, static_cast<int>(tree.getProperty("activeFrame", 0)));
    const bool loadedSeq = static_cast<bool>(tree.getProperty("sequenceActive", false));
    const bool loadedLoop = static_cast<bool>(tree.getProperty("loopActive", true));
    const float loadedFrameMs = juce::jlimit(20.0f, 4000.0f, static_cast<float>(tree.getProperty("frameTimeMs", 250.0f)));
    const bool loadedSync = static_cast<bool>(tree.getProperty("tempoSync", true));
    const float loadedFreeMs = juce::jlimit(100.0f, 8000.0f, static_cast<float>(tree.getProperty("freeTimeMs", 2000.0f)));

    auto framesTree = tree.getChildWithName("Frames");
    if (framesTree.isValid())
    {
        const juce::SpinLock::ScopedLockType lock(frameLock);
        for (auto& frame : frames)
            for (auto& row : frame)
                row.fill(SwatchWhite);

        for (int i = 0; i < framesTree.getNumChildren(); ++i)
        {
            auto frameNode = framesTree.getChild(i);
            const int idx = static_cast<int>(frameNode.getProperty("index", -1));
            const juce::String encoded = frameNode.getProperty("data", "");
            if (idx >= 0 && idx < kMaxFrames && encoded.length() >= kNumBands * kNumTaps)
            {
                int pos = 0;
                for (int b = 0; b < kNumBands; ++b)
                {
                    for (int t = 0; t < kNumTaps; ++t)
                    {
                        const int val = juce::jlimit(0, kNumSwatches - 1, encoded[pos] - '0');
                        frames[static_cast<size_t>(idx)][static_cast<size_t>(b)][static_cast<size_t>(t)] = val;
                        ++pos;
                    }
                }
            }
        }
        tree.removeChild(framesTree, nullptr);
    }

    numFrames.store(loadedNumFrames, std::memory_order_relaxed);
    activeFrame.store(loadedActive, std::memory_order_relaxed);
    sequenceActive.store(loadedSeq, std::memory_order_relaxed);
    loopActive.store(loadedLoop, std::memory_order_relaxed);
    frameTimeMs.store(loadedFrameMs, std::memory_order_relaxed);
    tempoSync.store(loadedSync, std::memory_order_relaxed);
    freeTimeMs.store(loadedFreeMs, std::memory_order_relaxed);

    if (tree.hasType(apvts.state.getType()))
        apvts.replaceState(tree);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EchoplasmAudioProcessor();
}
