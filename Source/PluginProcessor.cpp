#include "PluginProcessor.h"
#include "PluginEditor.h"

GoaRiserMachineProcessor::GoaRiserMachineProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

void GoaRiserMachineProcessor::prepareToPlay(double sampleRate, int) {
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    svfLpL = svfBpL = svfLpR = svfBpR = 0.0;
    oscPh = oscPh2 = oscPh3 = fmPh = lfoPh = slowLfoPh = snarePh = snareEnv = 0.0;
    dlyL.assign(96000, 0.0);
    dlyR.assign(96000, 0.0);
    dlyIdx = 0;
}

void GoaRiserMachineProcessor::releaseResources() {}

bool GoaRiserMachineProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void GoaRiserMachineProcessor::setParamsFromJson(const juce::String& jsonStr) {
    auto v = juce::JSON::parse(jsonStr);
    if (!v.isObject()) return;
    pMode.store((int)v.getProperty("mode", 0));
    pBars.store((float)v.getProperty("bars", 8.0));
    pNoiseMix.store((float)v.getProperty("noise", 65.0) * 0.01f);
    pPitchMix.store((float)v.getProperty("pitch", 75.0) * 0.01f);
    pSnareMix.store((float)v.getProperty("snare", 55.0) * 0.01f);
    pReso.store((float)v.getProperty("reso", 78.0) * 0.01f);
    pWobble.store((float)v.getProperty("wobble", 60.0) * 0.01f);
    pFmDepth.store((float)v.getProperty("fm", 35.0) * 0.01f);
    pSpaceMix.store((float)v.getProperty("space", 45.0) * 0.01f);
    pDropHole.store((float)v.getProperty("hole", 0.0));
    pGain.store((float)v.getProperty("gain", 80.0) * 0.01f);

    bool trig = (bool)v.getProperty("trig", false);
    if (trig && !pTrigger.load()) {
        risePos = 0.0;
        elapsedSec = 0.0;
        running = true;
    } else if (!trig && pTrigger.load()) {
        running = false;
    }
    pTrigger.store(trig);
}

void GoaRiserMachineProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    const int n = buffer.getNumSamples();
    buffer.clear();
    if (buffer.getNumChannels() < 2 || dlyL.empty()) return;
    auto* L = buffer.getWritePointer(0);
    auto* R = buffer.getWritePointer(1);

    for (const auto meta : midi) {
        auto m = meta.getMessage();
        if (m.isNoteOn()) {
            risePos = 0.0;
            elapsedSec = 0.0;
            running = true;
            baseMidiHz = juce::MidiMessage::getMidiNoteInHertz(m.getNoteNumber());
        } else if (m.isNoteOff() && !pTrigger.load()) {
            if (pMode.load() != 1 && pMode.load() != 3) // Impacts & Zaps finish their tail naturally
                running = false;
        }
    }

    double bpm = 140.0;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (pos->getBpm().hasValue() && *pos->getBpm() > 20.0) bpm = *pos->getBpm();
        }
    }

    const int mode = pMode.load();
    const double bars = juce::jmax(1.0f, pBars.load());
    const double totalSec = (bars * 4.0 * 60.0) / bpm;
    const double stepInc = 1.0 / (totalSec * sr);
    const double nMix = pNoiseMix.load(), pMix = pPitchMix.load(), sMix = pSnareMix.load();
    const double reso = pReso.load(), wob = pWobble.load(), fmAmt = pFmDepth.load();
    const double space = pSpaceMix.load(), gain = pGain.load();
    const bool dropHole = pDropHole.load() > 0.5f;
    const double pi2 = 6.283185307179586;

    // Dotted-8th stereo ping-pong delay samples
    const int dSampsL = juce::jlimit(64, 90000, (int)((0.75 * 60.0 / bpm) * sr));
    const int dSampsR = juce::jlimit(64, 90000, (int)((0.50 * 60.0 / bpm) * sr));

    for (int i = 0; i < n; ++i) {
        double dryL = 0.0, dryR = 0.0;

        if (running) {
            risePos += stepInc;
            elapsedSec += 1.0 / sr;

            // White Noise
            rngState = rngState * 1664525u + 1013904223u;
            double noise = ((double)(rngState & 0xFFFF) / 32768.0) - 1.0;

            if (mode == 0 || mode == 2) {
                // === MODE 0: RISER / MODE 2: DOWNLIFTER (Enhanced Original Base!) ===
                if (risePos >= 1.0) risePos = 0.0;
                double t = (mode == 2) ? (1.0 - risePos) : risePos;
                // Optional 1-beat Drop Hole silence right before 100%
                double holeGate = (mode == 0 && dropHole && risePos > (1.0 - 0.25 / bars)) ? 0.0 : 1.0;

                double lfoHz = (bpm / 60.0) * (1.0 + t * 7.0);
                lfoPh = std::fmod(lfoPh + lfoHz / sr, 1.0);
                double lfo = 0.5 + 0.5 * std::sin(lfoPh * pi2);

                double pitchHz = 55.0 * std::pow(2.0, t * 4.0);
                fmPh = std::fmod(fmPh + (pitchHz * 2.41) / sr, 1.0);
                double fmMod = std::sin(fmPh * pi2) * fmAmt * 0.65;
                oscPh = std::fmod(oscPh + pitchHz / sr, 1.0);
                double saw = std::sin((oscPh + fmMod) * pi2) * 0.5 + (oscPh * 2.0 - 1.0) * 0.5;

                double cutHz = juce::jlimit(120.0, sr * 0.42, 180.0 * std::pow(65.0, t) * (1.0 - wob * 0.35 + wob * 0.35 * lfo));
                double f = 2.0 * std::sin(3.14159265358979 * cutHz / sr);
                double q = juce::jlimit(0.05, 1.0, 1.0 - reso * 0.94);

                double mixSig = (noise * nMix * 0.55 + saw * pMix * 0.45) * holeGate;
                double hp = mixSig - svfLpL - q * svfBpL;
                svfBpL = juce::jlimit(-4.0, 4.0, svfBpL + f * hp);
                svfLpL = juce::jlimit(-4.0, 4.0, svfLpL + f * svfBpL);

                double rollHz = (bpm / 60.0) * (t < 0.5 ? 2.0 : (t < 0.75 ? 4.0 : 8.0));
                double prevS = snarePh;
                snarePh = std::fmod(snarePh + rollHz / sr, 1.0);
                if (snarePh < prevS) snareEnv = 1.0;
                snareEnv *= std::exp(-1.0 / (0.045 * sr));
                double snare = (noise * 0.7 + std::sin(snarePh * pi2 * 3.0) * 0.3) * snareEnv * sMix * (0.3 + 0.7 * t) * holeGate;

                double env = (mode == 2) ? (1.0 - risePos) : (0.15 + 0.85 * t);
                double sig = (svfBpL * 0.8 + svfLpL * 0.5 + snare) * env;
                dryL = sig * (0.85 + 0.15 * lfo);
                dryR = sig * (1.0 - 0.15 * lfo);
            }
            else if (mode == 1) {
                // === MODE 1: 3-LAYER PSYTRANCE IMPACT / SUB BOMB ===
                double boomLen = juce::jmax(1.2, totalSec * 0.5);
                double t = juce::jlimit(0.0, 1.0, elapsedSec / boomLen);
                if (t >= 1.0 && !pTrigger.load()) running = false;
                if (t >= 1.0 && pTrigger.load()) { elapsedSec = 0.0; risePos = 0.0; t = 0.0; }

                // Layer 1: Sub Boom (95 Hz -> 38 Hz sine drop)
                double subHz = 38.0 + 57.0 * std::exp(-elapsedSec * 5.5);
                oscPh = std::fmod(oscPh + subHz / sr, 1.0);
                double subBoom = std::sin(oscPh * pi2) * std::exp(-elapsedSec * 1.8) * pMix;

                // Layer 2: Distorted Metallic Mid Clang (220-750 Hz FM burst)
                fmPh = std::fmod(fmPh + 510.0 / sr, 1.0);
                oscPh2 = std::fmod(oscPh2 + (190.0 + std::sin(fmPh * pi2) * (180.0 + fmAmt * 420.0)) / sr, 1.0);
                double midBody = std::tanh(std::sin(oscPh2 * pi2) * 3.5) * std::exp(-elapsedSec * 6.5) * (0.4 + sMix * 0.6);

                // Layer 3: High Crack Transient (3-8 kHz noise whip)
                double crack = noise * std::exp(-elapsedSec * 28.0) * nMix * 1.1;

                // Filtered explosion wash
                double washCut = juce::jlimit(180.0, sr * 0.4, 7500.0 * std::exp(-elapsedSec * 2.4));
                double f = 2.0 * std::sin(3.14159265358979 * washCut / sr);
                double q = juce::jlimit(0.08, 1.0, 1.0 - reso * 0.85);
                double hp = (noise * nMix * 0.6 * std::exp(-elapsedSec * 1.5)) - svfLpL - q * svfBpL;
                svfBpL = juce::jlimit(-4.0, 4.0, svfBpL + f * hp);
                svfLpL = juce::jlimit(-4.0, 4.0, svfLpL + f * svfBpL);

                double imp = subBoom * 0.85 + midBody * 0.55 + crack * 0.65 + svfLpL * 0.5;
                dryL = imp; dryR = imp * 0.96;
                risePos = t;
            }
            else if (mode == 3) {
                // === MODE 3: PSY ALIEN ZAP / FM LASER / SQUELCH FX ===
                // Fast +24 to 0 semitone pitch drop + FM burst + tempo-synced re-zaps!
                double zapRate = (bpm / 60.0) * (1.0 + wob * 3.0);
                double zapPeriod = 1.0 / zapRate;
                double localT = std::fmod(elapsedSec, zapPeriod) / zapPeriod;
                if (elapsedSec > totalSec && !pTrigger.load()) running = false;

                double baseF = juce::jlimit(65.0, 660.0, baseMidiHz * 2.0);
                double zapHz = baseF * std::pow(2.0, 3.2 * std::exp(-localT * (5.5 + pMix * 6.0)));
                fmPh = std::fmod(fmPh + (zapHz * (1.5 + fmAmt * 4.5)) / sr, 1.0);
                double fmSig = std::sin(fmPh * pi2) * (fmAmt * 2.2) * std::exp(-localT * 4.0);
                oscPh = std::fmod(oscPh + zapHz / sr, 1.0);
                double zapOsc = std::sin((oscPh + fmSig) * pi2) * 0.7 + (oscPh * 2.0 - 1.0) * 0.3;

                double cutHz = juce::jlimit(150.0, sr * 0.42, 320.0 + 7500.0 * std::exp(-localT * 6.0));
                double f = 2.0 * std::sin(3.14159265358979 * cutHz / sr);
                double q = juce::jlimit(0.05, 1.0, 1.0 - reso * 0.94);
                double hp = (zapOsc * pMix + noise * nMix * 0.25) - svfLpL - q * svfBpL;
                svfBpL = juce::jlimit(-4.0, 4.0, svfBpL + f * hp);
                svfLpL = juce::jlimit(-4.0, 4.0, svfLpL + f * svfBpL);

                double amp = std::exp(-localT * 4.2);
                double pan = 0.5 + 0.45 * std::sin(elapsedSec * 3.7);
                double sig = (svfBpL * 0.75 + svfLpL * 0.45) * amp;
                dryL = sig * (1.0 - pan * 0.5);
                dryR = sig * (0.5 + pan * 0.5);
                risePos = std::fmod(elapsedSec / totalSec, 1.0);
            }
            else {
                // === MODE 4: PSYCHEDELIC ATMOS / COSMIC DRONE ===
                // Detuned Root + 5th + Octave + slow breathing 0.2 Hz stereo filter + FM shimmer
                slowLfoPh = std::fmod(slowLfoPh + (0.12 + wob * 0.45) / sr, 1.0);
                double lfoL = 0.5 + 0.5 * std::sin(slowLfoPh * pi2);
                double lfoR = 0.5 + 0.5 * std::cos(slowLfoPh * pi2);

                double rootHz = juce::jlimit(55.0, 220.0, baseMidiHz);
                fmPh = std::fmod(fmPh + (rootHz * 3.01) / sr, 1.0);
                double fm = std::sin(fmPh * pi2) * fmAmt * 0.45 * lfoL;

                oscPh  = std::fmod(oscPh  + rootHz / sr, 1.0);
                oscPh2 = std::fmod(oscPh2 + (rootHz * 1.498) / sr, 1.0); // 5th
                oscPh3 = std::fmod(oscPh3 + (rootHz * 2.005) / sr, 1.0); // Detuned Octave

                double padL = (std::sin((oscPh + fm) * pi2) * 0.45 + (oscPh2 * 2.0 - 1.0) * 0.25 + (oscPh3 * 2.0 - 1.0) * 0.20) * pMix + noise * nMix * 0.22;
                double padR = (std::sin((oscPh - fm) * pi2) * 0.45 + (oscPh2 * 2.0 - 1.0) * 0.20 + (oscPh3 * 2.0 - 1.0) * 0.25) * pMix + noise * nMix * 0.22;

                double cutL = juce::jlimit(140.0, sr * 0.4, 240.0 + lfoL * 3800.0);
                double cutR = juce::jlimit(140.0, sr * 0.4, 240.0 + lfoR * 3800.0);
                double fL = 2.0 * std::sin(3.14159265358979 * cutL / sr);
                double fR = 2.0 * std::sin(3.14159265358979 * cutR / sr);
                double q  = juce::jlimit(0.08, 1.0, 1.0 - reso * 0.88);

                double hpL = padL - svfLpL - q * svfBpL;
                svfBpL = juce::jlimit(-4.0, 4.0, svfBpL + fL * hpL);
                svfLpL = juce::jlimit(-4.0, 4.0, svfLpL + fL * svfBpL);

                double hpR = padR - svfLpR - q * svfBpR;
                svfBpR = juce::jlimit(-4.0, 4.0, svfBpR + fR * hpR);
                svfLpR = juce::jlimit(-4.0, 4.0, svfLpR + fR * svfBpR);

                dryL = svfLpL * 0.65 + svfBpL * 0.45;
                dryR = svfLpR * 0.65 + svfBpR * 0.45;
                risePos = lfoL;
            }
        }

        // Stereo Ping-Pong Delay Space (always runs so tails ring out smoothly!)
        double readL = dlyL[(size_t)((dlyIdx - dSampsL + 96000) % 96000)];
        double readR = dlyR[(size_t)((dlyIdx - dSampsR + 96000) % 96000)];
        double fb = 0.25 + space * 0.52;
        dlyL[(size_t)dlyIdx] = std::tanh(dryL + readR * fb);
        dlyR[(size_t)dlyIdx] = std::tanh(dryR + readL * fb);
        dlyIdx = (dlyIdx + 1) % 96000;

        L[i] = (float)std::tanh((dryL + readL * space * 0.85) * gain);
        R[i] = (float)std::tanh((dryR + readR * space * 0.85) * gain);
    }
    progress.store(running ? (float)risePos : 0.0f);
}

juce::AudioProcessorEditor* GoaRiserMachineProcessor::createEditor() { return new GoaRiserMachineEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GoaRiserMachineProcessor(); }
