#include "PluginProcessor.h"
#include "PluginEditor.h"

GoaRiserMachineProcessor::GoaRiserMachineProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    liveState.reset(44100.0);
}

void GoaRiserMachineProcessor::prepareToPlay(double sampleRate, int) {
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    liveState.reset(sr);
}

void GoaRiserMachineProcessor::releaseResources() {}

bool GoaRiserMachineProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void GoaRiserMachineProcessor::setParamsFromJson(const juce::String& jsonStr) {
    auto v = juce::JSON::parse(jsonStr);
    if (!v.isObject()) return;

    FxParams next;
    next.mode     = juce::jlimit(0, 4, (int)v.getProperty("mode", 0));
    next.subVar   = juce::jlimit(0, 3, (int)v.getProperty("subVar", 0));
    next.fltMode  = juce::jlimit(0, 3, (int)v.getProperty("fltMode", 0));
    next.bars     = juce::jlimit(1.0, 16.0, (double)v.getProperty("bars", 4.0));
    next.pitchSt  = juce::jlimit(-24.0, 24.0, (double)v.getProperty("pitch", 0.0));
    next.cutoffHz = juce::jlimit(80.0, 14000.0, (double)v.getProperty("cut", 2400.0));
    next.reso     = juce::jlimit(0.0, 0.96, (double)v.getProperty("reso", 75.0) * 0.01);
    next.drive    = juce::jlimit(0.0, 1.0, (double)v.getProperty("drive", 30.0) * 0.01);
    next.fm       = juce::jlimit(0.0, 1.0, (double)v.getProperty("fm", 40.0) * 0.01);
    next.comb     = juce::jlimit(0.0, 1.0, (double)v.getProperty("comb", 35.0) * 0.01);
    next.speed    = juce::jlimit(0.0, 1.0, (double)v.getProperty("speed", 55.0) * 0.01);
    next.noise    = juce::jlimit(0.0, 1.0, (double)v.getProperty("noise", 60.0) * 0.01);
    next.body     = juce::jlimit(0.0, 1.0, (double)v.getProperty("body", 55.0) * 0.01);
    next.space    = juce::jlimit(0.0, 1.0, (double)v.getProperty("space", 45.0) * 0.01);
    next.hole     = (bool)v.getProperty("hole", false);
    next.gain     = juce::jlimit(0.0, 1.0, (double)v.getProperty("gain", 80.0) * 0.01);

    {
        std::lock_guard<std::mutex> lock(paramMutex);
        params = next;
    }

    bool trig = (bool)v.getProperty("trig", false);
    if (trig && !pTrigger.load()) {
        liveState.risePos = 0.0;
        liveState.elapsedSec = 0.0;
        running = true;
    } else if (!trig && pTrigger.load()) {
        running = false;
    }
    pTrigger.store(trig);
}

void GoaRiserMachineProcessor::renderSample(FxSynthState& st, const FxParams& pr, double sr, double bpm,
                                            double baseHz, bool active, float& outL, float& outR) {
    const double pi2 = 6.283185307179586;
    const double totalSec = (pr.bars * 4.0 * 60.0) / bpm;
    const double stepInc = 1.0 / (totalSec * sr);
    const double pitchMult = std::pow(2.0, pr.pitchSt / 12.0);
    const int dSampsL = juce::jlimit(64, 90000, (int)((0.75 * 60.0 / bpm) * sr));
    const int dSampsR = juce::jlimit(64, 90000, (int)((0.50 * 60.0 / bpm) * sr));

    double dryL = 0.0, dryR = 0.0;

    if (active) {
        st.risePos += stepInc;
        st.elapsedSec += 1.0 / sr;

        st.rngState = st.rngState * 1664525u + 1013904223u;
        double noise = ((double)(st.rngState & 0xFFFF) / 32768.0) - 1.0;

        // Global LFO + S&H
        double lfoRate = (bpm / 60.0) * (0.5 + pr.speed * 7.5);
        st.prevLfo = st.lfoPh;
        st.lfoPh = std::fmod(st.lfoPh + lfoRate / sr, 1.0);
        if (st.lfoPh < st.prevLfo) {
            st.rngState = st.rngState * 1664525u + 1013904223u;
            st.shVal = (double)(st.rngState & 0xFFFF) / 65535.0;
        }
        double lfo = 0.5 + 0.5 * std::sin(st.lfoPh * pi2);

        double rawL = 0.0, rawR = 0.0;
        double cutMod = pr.cutoffHz;

        if (pr.mode == 0 || pr.mode == 2) {
            // === RISER (0) & DOWNLIFTER (2) ===
            if (st.risePos >= 1.0) st.risePos = 0.0;
            double t = (pr.mode == 2) ? (1.0 - st.risePos) : st.risePos;
            double holeGate = (pr.mode == 0 && pr.hole && st.risePos > (1.0 - 0.25 / pr.bars)) ? 0.0 : 1.0;

            double pHz = 55.0 * pitchMult * std::pow(2.0, t * (2.5 + pr.speed * 2.5));
            st.fmPh = std::fmod(st.fmPh + (pHz * (1.5 + pr.subVar * 0.9)) / sr, 1.0);
            double fmSig = std::sin(st.fmPh * pi2) * pr.fm * 1.4;
            st.oscPh = std::fmod(st.oscPh + pHz / sr, 1.0);
            double saw = std::sin((st.oscPh + fmSig) * pi2) * 0.5 + (st.oscPh * 2.0 - 1.0) * 0.5;

            double rollHz = (bpm / 60.0) * (t < 0.5 ? 2.0 : (t < 0.75 ? 4.0 : 8.0));
            double prevS = st.snarePh;
            st.snarePh = std::fmod(st.snarePh + rollHz / sr, 1.0);
            if (st.snarePh < prevS) st.snareEnv = 1.0;
            st.snareEnv *= std::exp(-1.0 / (0.042 * sr));
            double snare = (noise * 0.7 + std::sin(st.snarePh * pi2 * 3.0) * 0.3) * st.snareEnv * pr.body * (0.25 + 0.75 * t);

            double env = (pr.mode == 2) ? (1.0 - st.risePos) : (0.15 + 0.85 * t);
            double s = (noise * pr.noise * 0.6 + saw * 0.55 + snare) * env * holeGate;
            rawL = s * (0.85 + 0.15 * lfo);
            rawR = s * (1.0 - 0.15 * lfo);
            cutMod = juce::jlimit(100.0, sr * 0.42, (140.0 + pr.cutoffHz * 0.25) * std::pow(45.0, t) * (0.7 + 0.3 * lfo));
        }
        else if (pr.mode == 1) {
            // === IMPACT (4 DISTINCT SYNTHESIS ENGINES!) ===
            double decLen = 0.6 + pr.speed * 3.2; // 0.6s punchy to 3.8s huge bomb!
            double t = std::fmod(st.elapsedSec, decLen + 0.35);
            double envSub = std::exp(-t * (4.5 / decLen));
            double envMid = std::exp(-t * (11.0 / decLen));
            double envAtk = std::exp(-t * 32.0);

            if (pr.subVar == 0) {
                // 0: Sub Bomb (massive 808/Psy sub drop + filtered blast)
                double subHz = (34.0 + 75.0 * std::exp(-t * 6.0)) * pitchMult;
                st.oscPh = std::fmod(st.oscPh + subHz / sr, 1.0);
                double sub = std::sin(st.oscPh * pi2) * envSub * (0.5 + pr.body * 0.7);
                double blast = noise * pr.noise * envMid * 0.75;
                rawL = rawR = sub + blast + noise * envAtk * 0.6;
                cutMod = juce::jlimit(120.0, sr * 0.42, pr.cutoffHz * std::exp(-t * 2.2) + 120.0);
            } else if (pr.subVar == 1) {
                // 1: Cyber Metallic Slam (harsh FM clang + ring mod + whip crack)
                double mHz = 160.0 * pitchMult * (1.0 + 1.5 * std::exp(-t * 14.0));
                st.fmPh = std::fmod(st.fmPh + (mHz * (2.73 + pr.fm * 4.0)) / sr, 1.0);
                st.oscPh = std::fmod(st.oscPh + mHz / sr, 1.0);
                double clang = std::sin((st.oscPh + std::sin(st.fmPh * pi2) * (1.2 + pr.fm * 3.5)) * pi2) * envMid;
                double sub = std::sin(st.oscPh * pi2 * 0.5) * envSub * pr.body;
                rawL = clang * 0.75 + sub * 0.6 + noise * envAtk * pr.noise;
                rawR = clang * 0.65 * std::cos(t * 25.0) + sub * 0.6 + noise * envAtk * pr.noise;
                cutMod = juce::jlimit(180.0, sr * 0.42, pr.cutoffHz * (0.3 + 0.7 * envMid));
            } else if (pr.subVar == 2) {
                // 2: Alien Laser Implosion (fast sci-fi pitch dive + resonant zap boom)
                double zHz = 60.0 * pitchMult * std::pow(2.0, 4.2 * std::exp(-t * (5.0 + pr.speed * 8.0)));
                st.oscPh = std::fmod(st.oscPh + zHz / sr, 1.0);
                st.fmPh = std::fmod(st.fmPh + (zHz * 3.1) / sr, 1.0);
                double zap = std::sin((st.oscPh + std::sin(st.fmPh * pi2) * pr.fm * 2.0) * pi2) * envSub;
                rawL = zap * 0.8 + noise * envAtk * pr.noise * 0.7;
                rawR = zap * 0.8 * std::sin(t * 18.0 + 0.5) + noise * envAtk * pr.noise * 0.7;
                cutMod = juce::jlimit(180.0, sr * 0.42, 250.0 + pr.cutoffHz * std::exp(-t * 3.5));
            } else {
                // 3: Dark Cinematic Thunder (detuned sub rumble + stereo noise swell)
                double rHz = 45.0 * pitchMult;
                st.oscPh  = std::fmod(st.oscPh  + rHz / sr, 1.0);
                st.oscPh2 = std::fmod(st.oscPh2 + (rHz * 1.07) / sr, 1.0);
                double rumb = (std::sin(st.oscPh * pi2) + (st.oscPh2 * 2.0 - 1.0) * 0.45) * envSub * pr.body;
                double thund = noise * pr.noise * envSub * (0.5 + 0.5 * lfo);
                rawL = rumb + thund; rawR = rumb * 0.9 - thund * 0.85;
                cutMod = juce::jlimit(100.0, sr * 0.42, pr.cutoffHz * (0.2 + 0.8 * envSub));
            }
            st.risePos = juce::jlimit(0.0, 1.0, t / decLen);
        }
        else if (pr.mode == 3) {
            // === PSY FX / ZAPS (4 WILDLY DIFFERENT PSYCHEDELIC FX!) ===
            double rate = (bpm / 60.0) * (0.75 + pr.speed * 4.5);
            double period = 1.0 / rate;
            double lt = std::fmod(st.elapsedSec, period) / period;
            double fBase = juce::jlimit(55.0, 880.0, baseHz * 2.0 * pitchMult);

            if (pr.subVar == 0) {
                // 0: Alien Laser Cannon (ultra-sharp exponential pitch drop)
                double zHz = fBase * std::pow(2.0, 3.8 * std::exp(-lt * 8.0));
                st.oscPh = std::fmod(st.oscPh + zHz / sr, 1.0);
                st.fmPh = std::fmod(st.fmPh + (zHz * (1.5 + pr.fm * 3.5)) / sr, 1.0);
                double z = std::sin((st.oscPh + std::sin(st.fmPh * pi2) * pr.fm * 1.8) * pi2) * std::exp(-lt * 4.5);
                rawL = z; rawR = z * 0.85;
                cutMod = juce::jlimit(160.0, sr * 0.42, 250.0 + pr.cutoffHz * std::exp(-lt * 5.0));
            } else if (pr.subVar == 1) {
                // 1: Alien Talker (S&H formant pitch + vowel BP modulation)
                double tHz = fBase * (0.5 + st.shVal * 2.0);
                st.oscPh = std::fmod(st.oscPh + tHz / sr, 1.0);
                st.fmPh = std::fmod(st.fmPh + (tHz * 2.01) / sr, 1.0);
                double sig = ((st.oscPh * 2.0 - 1.0) * 0.6 + std::sin(st.fmPh * pi2) * pr.fm) * (0.2 + 0.8 * std::exp(-lt * 3.0));
                rawL = sig * (0.4 + 0.6 * st.shVal); rawR = sig * (1.0 - 0.6 * st.shVal);
                cutMod = juce::jlimit(200.0, sr * 0.42, 300.0 + pr.cutoffHz * st.shVal);
            } else if (pr.subVar == 2) {
                // 2: Forest Glitch Blips (random micro-bursts + metallic FM)
                double gHz = fBase * (0.4 + st.shVal * 4.5);
                st.oscPh = std::fmod(st.oscPh + gHz / sr, 1.0);
                st.fmPh = std::fmod(st.fmPh + (gHz * 5.31) / sr, 1.0);
                double gate = (lt < 0.32) ? 1.0 : 0.0;
                double blip = std::sin((st.oscPh + std::sin(st.fmPh * pi2) * (1.0 + pr.fm * 4.0)) * pi2) * gate;
                rawL = (st.shVal > 0.5) ? blip : blip * 0.25;
                rawR = (st.shVal <= 0.5) ? blip : blip * 0.25;
                cutMod = juce::jlimit(250.0, sr * 0.42, pr.cutoffHz * (0.4 + 0.6 * lfo));
            } else {
                // 3: Goa 303 Squelch Spiral (resonant climbing acid burps)
                double sHz = fBase * 0.5 * std::pow(2.0, std::floor(st.shVal * 12.0) / 12.0);
                st.oscPh = std::fmod(st.oscPh + sHz / sr, 1.0);
                double saw = (st.oscPh * 2.0 - 1.0) * std::exp(-lt * 3.5);
                rawL = rawR = saw + noise * pr.noise * 0.15;
                cutMod = juce::jlimit(160.0, sr * 0.42, 180.0 + pr.cutoffHz * std::exp(-lt * 4.5));
            }
            st.risePos = lt;
        }
        else {
            // === ATMOS / COSMIC DRONE (4 TEXTURES!) ===
            st.slowLfoPh = std::fmod(st.slowLfoPh + (0.08 + pr.speed * 0.65) / sr, 1.0);
            double lfoL = 0.5 + 0.5 * std::sin(st.slowLfoPh * pi2);
            double lfoR = 0.5 + 0.5 * std::cos(st.slowLfoPh * pi2);
            double rHz = juce::jlimit(45.0, 330.0, baseHz * pitchMult);

            double ratio2 = (pr.subVar == 1) ? 1.0595 : ((pr.subVar == 2) ? 1.414 : 1.498); // minor2nd / tritone / 5th
            double ratio3 = (pr.subVar == 1) ? 1.414 : 2.006;
            st.oscPh  = std::fmod(st.oscPh  + rHz / sr, 1.0);
            st.oscPh2 = std::fmod(st.oscPh2 + (rHz * ratio2) / sr, 1.0);
            st.oscPh3 = std::fmod(st.oscPh3 + (rHz * ratio3) / sr, 1.0);
            st.fmPh   = std::fmod(st.fmPh   + (rHz * 3.01) / sr, 1.0);
            double fmM = std::sin(st.fmPh * pi2) * pr.fm * 0.75 * lfoL;

            if (pr.subVar == 3) {
                // Ethereal Wind + Bells
                double bell = std::sin((st.oscPh3 + fmM) * pi2) * 0.35 * lfoR;
                rawL = noise * pr.noise * 0.65 + bell;
                rawR = -noise * pr.noise * 0.65 + bell;
            } else {
                rawL = (std::sin((st.oscPh + fmM) * pi2) * 0.45 + (st.oscPh2 * 2.0 - 1.0) * 0.30 + (st.oscPh3 * 2.0 - 1.0) * 0.20) * pr.body + noise * pr.noise * 0.25;
                rawR = (std::sin((st.oscPh - fmM) * pi2) * 0.45 + (st.oscPh2 * 2.0 - 1.0) * 0.20 + (st.oscPh3 * 2.0 - 1.0) * 0.30) * pr.body + noise * pr.noise * 0.25;
            }
            cutMod = juce::jlimit(120.0, sr * 0.42, 160.0 + pr.cutoffHz * (0.25 + 0.75 * lfoL));
            st.risePos = lfoL;
        }

        // === SHARED PRO FX CHAIN: DRIVE/CRUSH -> 4-MODE SVF -> STEREO LASER COMB ===
        if (pr.drive > 0.01) {
            double drv = 1.0 + pr.drive * 14.0;
            rawL = std::tanh(rawL * drv) * 0.75 + std::sin(rawL * drv * 0.6) * 0.35;
            rawR = std::tanh(rawR * drv) * 0.75 + std::sin(rawR * drv * 0.6) * 0.35;
            if (pr.drive > 0.55) { // Subtle psy bitcrush on high drive
                double bits = 24.0 - pr.drive * 18.0;
                double steps = std::pow(2.0, juce::jmax(3.0, bits));
                rawL = std::round(rawL * steps) / steps;
                rawR = std::round(rawR * steps) / steps;
            }
        }

        // 4-Mode Chamberlin SVF (LP / BP / HP / Notch)
        double f = 2.0 * std::sin(3.14159265358979 * juce::jlimit(60.0, sr * 0.42, cutMod) / sr);
        double q = juce::jlimit(0.05, 1.0, 1.0 - pr.reso * 0.94);

        double hpL = rawL - st.svfLpL - q * st.svfBpL;
        st.svfBpL = juce::jlimit(-4.0, 4.0, st.svfBpL + f * hpL);
        st.svfLpL = juce::jlimit(-4.0, 4.0, st.svfLpL + f * st.svfBpL);
        double hpR = rawR - st.svfLpR - q * st.svfBpR;
        st.svfBpR = juce::jlimit(-4.0, 4.0, st.svfBpR + f * hpR);
        st.svfLpR = juce::jlimit(-4.0, 4.0, st.svfLpR + f * st.svfBpR);

        double fltL = (pr.fltMode == 1) ? st.svfBpL : ((pr.fltMode == 2) ? hpL : ((pr.fltMode == 3) ? (hpL + st.svfLpL) : st.svfLpL));
        double fltR = (pr.fltMode == 1) ? st.svfBpR : ((pr.fltMode == 2) ? hpR : ((pr.fltMode == 3) ? (hpR + st.svfLpR) : st.svfLpR));

        // Stereo Laser Comb Filter
        int cSampsL = juce::jlimit(8, 4400, (int)((1.5 + lfo * 12.0) * 0.001 * sr));
        int cSampsR = juce::jlimit(8, 4400, (int)((2.1 + (1.0 - lfo) * 12.0) * 0.001 * sr));
        double cRL = st.combL[(size_t)((st.combIdx - cSampsL + 4800) % 4800)];
        double cRR = st.combR[(size_t)((st.combIdx - cSampsR + 4800) % 4800)];
        st.combL[(size_t)st.combIdx] = std::tanh(fltL + cRL * 0.78);
        st.combR[(size_t)st.combIdx] = std::tanh(fltR + cRR * 0.78);
        st.combIdx = (st.combIdx + 1) % 4800;

        dryL = fltL * (1.0 - pr.comb * 0.65) + cRL * (pr.comb * 0.65);
        dryR = fltR * (1.0 - pr.comb * 0.65) + cRR * (pr.comb * 0.65);
    }

    // Stereo Ping-Pong Delay Space (always runs for lush tails!)
    double readL = st.dlyL[(size_t)((st.dlyIdx - dSampsL + 96000) % 96000)];
    double readR = st.dlyR[(size_t)((st.dlyIdx - dSampsR + 96000) % 96000)];
    double fb = 0.25 + pr.space * 0.55;
    st.dlyL[(size_t)st.dlyIdx] = std::tanh(dryL + readR * fb);
    st.dlyR[(size_t)st.dlyIdx] = std::tanh(dryR + readL * fb);
    st.dlyIdx = (st.dlyIdx + 1) % 96000;

    outL = (float)std::tanh((dryL + readL * pr.space * 0.85) * pr.gain);
    outR = (float)std::tanh((dryR + readR * pr.space * 0.85) * pr.gain);
}

void GoaRiserMachineProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    const int n = buffer.getNumSamples();
    buffer.clear();
    if (buffer.getNumChannels() < 2 || liveState.dlyL.empty()) return;
    auto* L = buffer.getWritePointer(0);
    auto* R = buffer.getWritePointer(1);

    FxParams pr;
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        pr = params;
    }

    for (const auto meta : midi) {
        auto m = meta.getMessage();
        if (m.isNoteOn()) {
            liveState.risePos = 0.0;
            liveState.elapsedSec = 0.0;
            running = true;
            baseMidiHz = juce::MidiMessage::getMidiNoteInHertz(m.getNoteNumber());
        } else if (m.isNoteOff() && !pTrigger.load()) {
            if (pr.mode != 1) running = false;
        }
    }

    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (pos->getBpm().hasValue() && *pos->getBpm() > 20.0) lastBpm = *pos->getBpm();
        }
    }

    for (int i = 0; i < n; ++i) {
        renderSample(liveState, pr, sr, lastBpm, baseMidiHz, running, L[i], R[i]);
    }
    progress.store(running ? (float)liveState.risePos : 0.0f);
}

juce::File GoaRiserMachineProcessor::renderCurrentFxToWav(const juce::File& targetFile) {
    FxParams pr;
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        pr = params;
    }
    const double expSr = 44100.0;
    const double bpm = lastBpm > 20.0 ? lastBpm : 140.0;
    const double activeSec = (pr.mode == 1) ? (0.8 + pr.speed * 3.0) : ((pr.bars * 4.0 * 60.0) / bpm);
    const double tailSec = (pr.mode == 0) ? 0.35 : (0.8 + pr.space * 1.6);
    const int activeSamps = (int)(activeSec * expSr);
    const int totalSamps  = (int)((activeSec + tailSec) * expSr);

    juce::AudioBuffer<float> outBuf(2, totalSamps);
    FxSynthState offState;
    offState.reset(expSr);

    auto* L = outBuf.getWritePointer(0);
    auto* R = outBuf.getWritePointer(1);
    for (int i = 0; i < totalSamps; ++i) {
        renderSample(offState, pr, expSr, bpm, baseMidiHz, i < activeSamps, L[i], R[i]);
    }

    // Smooth 10ms fade-out at end to prevent any click
    const int fadeN = juce::jmin(441, totalSamps);
    for (int i = 0; i < fadeN; ++i) {
        float g = (float)(fadeN - i) / (float)fadeN;
        L[totalSamps - fadeN + i] *= g;
        R[totalSamps - fadeN + i] *= g;
    }

    targetFile.getParentDirectory().createDirectory();
    if (targetFile.existsAsFile()) targetFile.deleteFile();
    juce::WavAudioFormat wavFmt;
    if (auto stream = std::unique_ptr<juce::FileOutputStream>(targetFile.createOutputStream())) {
        if (auto writer = std::unique_ptr<juce::AudioFormatWriter>(
                wavFmt.createWriterFor(stream.get(), expSr, 2, 24, {}, 0))) {
            stream.release();
            writer->writeFromAudioSampleBuffer(outBuf, 0, totalSamps);
        }
    }
    return targetFile;
}

juce::AudioProcessorEditor* GoaRiserMachineProcessor::createEditor() { return new GoaRiserMachineEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GoaRiserMachineProcessor(); }
