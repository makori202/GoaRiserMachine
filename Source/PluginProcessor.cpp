#include "PluginProcessor.h"
#include "PluginEditor.h"

GoaRiserMachineProcessor::GoaRiserMachineProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

void GoaRiserMachineProcessor::prepareToPlay(double sampleRate, int) {
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    svfLp = svfBp = 0.0;
    oscPh = lfoPh = snarePh = snareEnv = 0.0;
}

void GoaRiserMachineProcessor::releaseResources() {}

bool GoaRiserMachineProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void GoaRiserMachineProcessor::setParamsFromJson(const juce::String& jsonStr) {
    auto v = juce::JSON::parse(jsonStr);
    if (!v.isObject()) return;
    pBars.store((float)v.getProperty("bars", 8.0));
    pNoiseMix.store((float)v.getProperty("noise", 65.0) * 0.01f);
    pPitchMix.store((float)v.getProperty("pitch", 75.0) * 0.01f);
    pSnareMix.store((float)v.getProperty("snare", 55.0) * 0.01f);
    pReso.store((float)v.getProperty("reso", 78.0) * 0.01f);
    pWobble.store((float)v.getProperty("wobble", 60.0) * 0.01f);
    pReverse.store((float)v.getProperty("down", 0.0));
    pGain.store((float)v.getProperty("gain", 75.0) * 0.01f);
    bool trig = (bool)v.getProperty("trig", false);
    if (trig && !pTrigger.load()) {
        risePos = 0.0;
        running = true;
    } else if (!trig && pTrigger.load()) {
        running = false;
    }
    pTrigger.store(trig);
}

void GoaRiserMachineProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    const int n = buffer.getNumSamples();
    buffer.clear();
    if (buffer.getNumChannels() < 2) return;
    auto* L = buffer.getWritePointer(0);
    auto* R = buffer.getWritePointer(1);

    for (const auto meta : midi) {
        if (meta.getMessage().isNoteOn()) {
            risePos = 0.0;
            running = true;
        } else if (meta.getMessage().isNoteOff() && !pTrigger.load()) {
            running = false;
        }
    }

    if (!running) {
        progress.store(0.0f);
        return;
    }

    double bpm = 140.0;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (pos->getBpm().hasValue() && *pos->getBpm() > 20.0) bpm = *pos->getBpm();
        }
    }

    const double bars = juce::jmax(1.0f, pBars.load());
    const double totalSec = (bars * 4.0 * 60.0) / bpm;
    const double stepInc = 1.0 / (totalSec * sr);
    const double nMix = pNoiseMix.load(), pMix = pPitchMix.load(), sMix = pSnareMix.load();
    const double reso = pReso.load(), wob = pWobble.load(), gain = pGain.load();
    const bool isDown = pReverse.load() > 0.5f;
    const double pi2 = 6.283185307179586;

    for (int i = 0; i < n; ++i) {
        risePos += stepInc;
        if (risePos >= 1.0) risePos = 0.0;
        double t = isDown ? (1.0 - risePos) : risePos;

        // White Noise
        rngState = rngState * 1664525u + 1013904223u;
        double noise = ((double)(rngState & 0xFFFF) / 32768.0) - 1.0;

        // Accelerating LFO wobble
        double lfoHz = (bpm / 60.0) * (1.0 + t * 7.0);
        lfoPh = std::fmod(lfoPh + lfoHz / sr, 1.0);
        double lfo = 0.5 + 0.5 * std::sin(lfoPh * pi2);

        // Pitch-rising Sawtooth (55 Hz -> 880 Hz)
        double pitchHz = 55.0 * std::pow(2.0, t * 4.0);
        oscPh = std::fmod(oscPh + pitchHz / sr, 1.0);
        double saw = (oscPh * 2.0 - 1.0);

        // Resonant Filter Sweep (180 Hz -> 12000 Hz)
        double cutHz = juce::jlimit(120.0, sr * 0.42, 180.0 * std::pow(65.0, t) * (1.0 - wob * 0.35 + wob * 0.35 * lfo));
        double f = 2.0 * std::sin(3.14159265358979 * cutHz / sr);
        double q = juce::jlimit(0.05, 1.0, 1.0 - reso * 0.94);
        double mixSig = noise * nMix * 0.55 + saw * pMix * 0.45;
        double hp = mixSig - svfLp - q * svfBp;
        svfBp = juce::jlimit(-4.0, 4.0, svfBp + f * hp);
        svfLp = juce::jlimit(-4.0, 4.0, svfLp + f * svfBp);

        // Accelerating 909 Snare Roll (1 -> 2 -> 4 -> 8 per beat)
        double rollHz = (bpm / 60.0) * (t < 0.5 ? 2.0 : (t < 0.75 ? 4.0 : 8.0));
        double prevS = snarePh;
        snarePh = std::fmod(snarePh + rollHz / sr, 1.0);
        if (snarePh < prevS) snareEnv = 1.0;
        snareEnv *= std::exp(-1.0 / (0.045 * sr));
        double snare = (noise * 0.7 + std::sin(snarePh * pi2 * 3.0) * 0.3) * snareEnv * sMix * (0.3 + 0.7 * t);

        double env = isDown ? (1.0 - risePos) : (0.15 + 0.85 * t);
        float out = (float)std::tanh((svfBp * 0.8 + svfLp * 0.5 + snare) * env * gain);
        L[i] = out * 0.95f;
        R[i] = out;
    }
    progress.store((float)risePos);
}

juce::AudioProcessorEditor* GoaRiserMachineProcessor::createEditor() { return new GoaRiserMachineEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GoaRiserMachineProcessor(); }
