#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <vector>
#include <cmath>

class GoaRiserMachineProcessor : public juce::AudioProcessor {
public:
    GoaRiserMachineProcessor();
    ~GoaRiserMachineProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    void setParamsFromJson(const juce::String& jsonStr);
    float getProgress() const { return progress.load(); }

private:
    // Engine Modes: 0=Riser, 1=Impact/Bomb, 2=Downlifter, 3=Psy Zap/FM FX, 4=Cosmic Atmos/Drone
    std::atomic<int>   pMode{0};
    std::atomic<float> pBars{8.0f}, pNoiseMix{0.65f}, pPitchMix{0.75f}, pSnareMix{0.55f};
    std::atomic<float> pReso{0.78f}, pWobble{0.60f}, pFmDepth{0.35f}, pSpaceMix{0.45f};
    std::atomic<float> pDropHole{0.0f}, pGain{0.80f};
    std::atomic<bool>  pTrigger{false};
    std::atomic<float> progress{0.0f};

    double sr = 44100.0;
    double risePos = 0.0;
    double elapsedSec = 0.0;
    bool running = false;
    double baseMidiHz = 55.0;

    double oscPh = 0.0, oscPh2 = 0.0, oscPh3 = 0.0, fmPh = 0.0;
    double lfoPh = 0.0, slowLfoPh = 0.0, snarePh = 0.0, snareEnv = 0.0;
    double svfLpL = 0.0, svfBpL = 0.0, svfLpR = 0.0, svfBpR = 0.0;
    uint32_t rngState = 1234567u;

    // Stereo Ping-Pong Delay + Comb Space
    std::vector<double> dlyL, dlyR;
    int dlyIdx = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GoaRiserMachineProcessor)
};
