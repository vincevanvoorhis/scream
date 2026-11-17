#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include <atomic>

class EchoplasmAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kNumBands = 16;
    static constexpr int kNumTaps = 16;
    static constexpr int kMaxFrames = 16;
    static constexpr int kNumSwatches = 9;
    static constexpr int kFifoSize = 2048;

    using FrameGrid = std::array<std::array<int, kNumTaps>, kNumBands>;

    enum SwatchIndex
    {
        SwatchWhite = 0,
        SwatchLightGray = 1,
        SwatchDarkGray = 2,
        SwatchBlack = 3,
        SwatchYellow = 4,
        SwatchOrange = 5,
        SwatchRed = 6,
        SwatchGreen = 7,
        SwatchBlue = 8
    };

    EchoplasmAudioProcessor();
    ~EchoplasmAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
   #endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    // Thread-safe Grid & Frame Management
    void setCell(int frameIndex, int band, int tap, int swatchIndex);
    int getCell(int frameIndex, int band, int tap) const;
    FrameGrid getFrameCopy(int frameIndex) const;
    void setFrameData(int frameIndex, const FrameGrid& grid);
    void clearFrame(int frameIndex);
    void clearAllFrames();

    int getNumFrames() const noexcept { return numFrames.load(std::memory_order_relaxed); }
    void setNumFrames(int count);
    int addFrame(bool duplicateCurrent = false);
    void removeFrame(int index);

    int getActiveFrame() const noexcept { return activeFrame.load(std::memory_order_relaxed); }
    void setActiveFrame(int index);

    bool isSequenceActive() const noexcept { return sequenceActive.load(std::memory_order_relaxed); }
    void setSequenceActive(bool shouldPlay);

    bool isLoopActive() const noexcept { return loopActive.load(std::memory_order_relaxed); }
    void setLoopActive(bool shouldLoop) noexcept { loopActive.store(shouldLoop, std::memory_order_relaxed); }

    float getFrameTimeMs() const noexcept { return frameTimeMs.load(std::memory_order_relaxed); }
    void setFrameTimeMs(float ms) noexcept { frameTimeMs.store(juce::jlimit(20.0f, 4000.0f, ms), std::memory_order_relaxed); }

    bool isTempoSync() const noexcept { return tempoSync.load(std::memory_order_relaxed); }
    void setTempoSync(bool shouldSync) noexcept { tempoSync.store(shouldSync, std::memory_order_relaxed); }

    float getFreeTimeMs() const noexcept { return freeTimeMs.load(std::memory_order_relaxed); }
    void setFreeTimeMs(float ms) noexcept { freeTimeMs.store(juce::jlimit(100.0f, 8000.0f, ms), std::memory_order_relaxed); }

    double getLastHostBpm() const noexcept { return lastHostBpm.load(std::memory_order_relaxed); }

    // Lock-free audio FIFO for editor FFT spectrum analyzer
    int getNumFifoAvailable() const noexcept { return abstractFifo.getNumReady(); }
    int pullSamplesForFFT(float* destination, int maxSamples);

    static float swatchToGain(int swatchIndex) noexcept;
    static juce::Colour swatchToColour(int swatchIndex) noexcept;
    static const std::array<float, kNumBands + 1>& getBandEdges() noexcept;
    static juce::String getBandLabel(int bandIndex);

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // 16 logarithmic frequency band filters
    std::array<juce::dsp::StateVariableTPTFilter<float>, kNumBands> bandFilters;
    std::array<float, kNumBands> bandNormGains {};

    // 16 stereo circular delay buffers (one per frequency band)
    struct BandDelayLine
    {
        std::vector<float> left;
        std::vector<float> right;
        int writePos = 0;
        int bufferSize = 0;

        void prepare(int maxSamples)
        {
            bufferSize = juce::jmax(16, maxSamples);
            left.assign(static_cast<size_t>(bufferSize), 0.0f);
            right.assign(static_cast<size_t>(bufferSize), 0.0f);
            writePos = 0;
        }

        void reset()
        {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            writePos = 0;
        }

        inline void writeSample(float l, float r) noexcept
        {
            left[static_cast<size_t>(writePos)] = l;
            right[static_cast<size_t>(writePos)] = r;
        }

        inline void advance() noexcept
        {
            if (++writePos >= bufferSize)
                writePos = 0;
        }

        inline void readInterpolated(float delaySamples, float& outL, float& outR) const noexcept
        {
            const float clamped = juce::jlimit(1.0f, static_cast<float>(bufferSize - 2), delaySamples);
            float readPos = static_cast<float>(writePos) - clamped;
            if (readPos < 0.0f)
                readPos += static_cast<float>(bufferSize);

            const int idx0 = static_cast<int>(readPos);
            const int idx1 = (idx0 + 1 < bufferSize) ? (idx0 + 1) : 0;
            const float frac = readPos - static_cast<float>(idx0);

            const size_t s0 = static_cast<size_t>(idx0);
            const size_t s1 = static_cast<size_t>(idx1);

            outL = left[s0] + frac * (left[s1] - left[s0]);
            outR = right[s0] + frac * (right[s1] - right[s0]);
        }
    };

    std::array<BandDelayLine, kNumBands> delayLines;

    // Smoothers for tap gains (avoids clicks on frame change / live draw / opacity)
    std::array<std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, kNumTaps>, kNumBands> tapGainSmoothers;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, kNumTaps> tapDelaySmoothers;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmoother;

    // Animation frames protected by SpinLock
    mutable juce::SpinLock frameLock;
    std::array<FrameGrid, kMaxFrames> frames {};

    std::atomic<int> numFrames { 1 };
    std::atomic<int> activeFrame { 0 };
    std::atomic<bool> sequenceActive { false };
    std::atomic<bool> loopActive { true };
    std::atomic<float> frameTimeMs { 250.0f };
    std::atomic<bool> tempoSync { true };
    std::atomic<float> freeTimeMs { 2000.0f };
    std::atomic<double> lastHostBpm { 120.0 };

    double currentSampleRate = 44100.0;
    double frameSampleAccumulator = 0.0;

    // Cached atomic parameter pointers
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* feedbackParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* opacityParam = nullptr;
    std::atomic<float>* freezeParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    // Lock-free FIFO for spectrum analyzer
    juce::AbstractFifo abstractFifo { kFifoSize };
    std::array<float, kFifoSize> fifoData {};

    void pushSamplesToFifo(const float* samples, int numSamples) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoplasmAudioProcessor)
};
