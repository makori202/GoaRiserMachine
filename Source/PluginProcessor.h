#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <vector>
#include <cmath>

struct FxSynthState {
    double risePos = 0.0;
    double elapsedSec = 0.0;
    double oscPh = 0.0, oscPh2 = 0.0, oscPh3 = 0.0, fmPh = 0.0;
    double lfoPh = 0.0, slowLfoPh = 0.0, snarePh = 0.0, snareEnv = 0.0;
    double svfLpL = 0.0, svfBpL = 0.0, svfLpR = 0.0, svfBpR = 0.0;
    double shVal = 0.5, prevLfo = 0.0;
    uint32_t rngState = 1234567u;
    std::vector<double> dlyL, dlyR, combL, combR;
    int dlyIdx = 0, combIdx = 0;

    void reset(double) {
        risePos = elapsedSec = 0.0;
        oscPh = oscPh2 = oscPh3 = fmPh = lfoPh = slowLfoPh = snarePh = snareEnv = 0.0;
        svfLpL = svfBpL = svfLpR = svfBpR = 0.0;
        shVal = 0.5; prevLfo = 0.0; rngState = 1234567u;
        dlyL.assign(96000, 0.0); dlyR.assign(96000, 0.0);
        combL.assign(4800, 0.0); combR.assign(4800, 0.0);
        dlyIdx = combIdx = 0;
    }
};

struct FxParams {
    int mode = 0;      // 0=Riser, 1=Impact, 2=Down, 3=ZapFX, 4=Atmos
    int subVar = 0;    // 0..3 character variant per mode
    int fltMode = 0;   // 0=LP, 1=BP, 2=HP, 3=Notch
    double bars = 4.0;
    double pitchSt = 0.0;
    double cutoffHz = 2400.0;
    double reso = 0.75;
    double drive = 0.30;
    double fm = 0.40;
    double comb = 0.35;
    double speed = 0.55;
    double noise = 0.60;
    double body = 0.55;
    double space = 0.45;
    bool hole = false;
    double gain = 0.80;
};

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
    juce::File renderCurrentFxToWav(const juce::File& targetFile);
    float getProgress() const { return progress.load(); }

private:
    static void renderSample(FxSynthState& st, const FxParams& pr, double sr, double bpm,
                             double baseHz, bool active, float& outL, float& outR);

    std::mutex paramMutex;
    FxParams params;
    FxSynthState liveState;

    std::atomic<bool> pTrigger{false};
    std::atomic<float> progress{0.0f};
    double sr = 44100.0;
    double lastBpm = 140.0;
    double baseMidiHz = 55.0;
    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GoaRiserMachineProcessor)
};
