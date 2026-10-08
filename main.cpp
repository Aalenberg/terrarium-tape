#include "daisy_seed.h"

#include <cmath>
#include <cstdint>

using namespace daisy;

namespace
{
// Keep power-of-two; increasing this permits longer delays at additional RAM cost.
constexpr size_t kDelayBufferSize = 32768;
constexpr size_t kDelayBufferMask = kDelayBufferSize - 1;
constexpr float  kTwoPi           = 6.28318530718f;
constexpr float  kRandom24Max     = 16777215.0f;

namespace tuning
{
// Delay and pitch motion: raise depths for stronger modulation; adjust rates for character.
constexpr float baseDelayMs = 14.0f;
constexpr float wowDepthMs = 6.0f;
constexpr float unstableWowDepthMs = 12.0f;
constexpr float flutterDepthMs = 1.2f;
constexpr float wowRateHz = 0.28f;
constexpr float unstableWowRateHz = 0.43f;
constexpr float flutterRateHz = 5.4f;
constexpr float unstableFlutterRateHz = 7.1f;
constexpr float secondaryWowRateRatio = 0.37f;
constexpr float secondaryWowDepthRatio = 0.42f;
constexpr float secondaryWowPhaseOffsetRadians = 1.1f;
constexpr float minimumDelaySamples = 2.0f;

// Wear darkens the tape and increases compression; the dark switch lowers bandwidth further.
constexpr float minimumCutoffHz = 900.0f;
constexpr float maximumCutoffHz = 18000.0f;
constexpr float wearCutoffReductionHz = 13000.0f;
constexpr float darkTapeCutoffRatio = 0.58f;
constexpr float baseSaturationDrive = 1.0f;
constexpr float wearSaturationDrive = 4.5f;
constexpr float extraSaturationDrive = 2.0f;

// Texture: hiss values are output amplitude; dropouts are events per second at full knob.
constexpr float hissBaseAmplitude = 0.0004f;
constexpr float hissWearAmplitude = 0.006f;
constexpr float hissWithoutSwitchAmplitude = 0.0007f;
constexpr float dropoutRateAtMaxPerSecond = 2.0f;
constexpr float dropoutMinMs = 20.0f;
constexpr float dropoutDurationRangeMs = 50.0f;
constexpr float dropoutGain = 0.08f;

// Transport: stop speed sets the held pitch, recovery speed controls release behavior.
constexpr float tapeStopSpeed = 0.025f;
constexpr float tapeRecoverySpeed = 1.7f;
constexpr float stopAttackSmoothing = 0.0002f;
constexpr float stopReleaseSmoothing = 0.00025f;
constexpr float maximumStopDelaySeconds = 0.25f;
constexpr float stopResumeThresholdSamples = 1.0f;

// Output and smoothing: alter these to set the volume range and transition responsiveness.
constexpr float minimumOutputGain = 0.6f;
constexpr float maximumOutputGain = 1.5f;
constexpr float blendSmoothing = 0.001f;
constexpr float potSlewSeconds = 0.01f;
} // namespace tuning

DaisySeed hw;
AnalogControl pots[6];
Switch        toggles[4];
Switch        bypassSwitch;
Switch        stopSwitch;
GPIO          effectLed;
GPIO          stopLed;

float delayBuffer[kDelayBufferSize];
size_t writeIndex = 0;
float  wowPhase = 0.0f;
float  flutterPhase = 0.0f;
float  tapeSpeed = 1.0f;
float  stopDelay = 0.0f;
float  lowpassState = 0.0f;
float  blendState = 0.0f;
float  dropoutSamples = 0.0f;
uint32_t noiseState = 0x6d2b79f5u;
bool effectEnabled = true;
volatile bool toggleOn[4] = {};
volatile bool tapeStopHeld = false;

uint32_t NextRandom()
{
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >> 17;
    noiseState ^= noiseState << 5;
    return noiseState;
}

float RandomBipolar(uint32_t value)
{
    return static_cast<float>(value & 0xffffu) * (2.0f / 65535.0f) - 1.0f;
}

float ReadDelay(float delaySamples)
{
    float readPosition = static_cast<float>(writeIndex) - delaySamples;
    if(readPosition < 0.0f)
        readPosition += static_cast<float>(kDelayBufferSize);

    const size_t index0 = static_cast<size_t>(readPosition);
    const size_t index1 = (index0 + 1) & kDelayBufferMask;
    const float  frac   = readPosition - static_cast<float>(index0);
    return delayBuffer[index0] + (delayBuffer[index1] - delayBuffer[index0]) * frac;
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    const float sampleRate = hw.AudioSampleRate();
    const float wear       = pots[0].Process();
    const float wow        = pots[1].Process();
    const float flutter    = pots[2].Process();
    const float dropouts   = pots[3].Process();
    const float mix        = pots[4].Process();
    const float level = tuning::minimumOutputGain
                        + (tuning::maximumOutputGain - tuning::minimumOutputGain)
                              * pots[5].Process();

    const bool hissEnabled     = toggleOn[0];
    const bool darkTape        = toggleOn[1];
    const bool unstableTape    = toggleOn[2];
    const bool extraSaturation = toggleOn[3];
    const float cutoff
        = fmaxf(tuning::minimumCutoffHz,
                (tuning::maximumCutoffHz - wear * tuning::wearCutoffReductionHz)
                    * (darkTape ? tuning::darkTapeCutoffRatio : 1.0f));
    const float lowpassCoeff = 1.0f - expf(-kTwoPi * cutoff / sampleRate);
    const float drive = tuning::baseSaturationDrive
                        + wear * tuning::wearSaturationDrive
                        + (extraSaturation ? tuning::extraSaturationDrive : 0.0f);
    const float saturationNorm = 1.0f / tanhf(drive);
    const float wowDepth
        = wow * (unstableTape ? tuning::unstableWowDepthMs : tuning::wowDepthMs)
          * 0.001f * sampleRate;
    const float flutterDepth = flutter * tuning::flutterDepthMs * 0.001f * sampleRate;
    const float stopDelayLimit = sampleRate * tuning::maximumStopDelaySeconds;

    for(size_t i = 0; i < size; ++i)
    {
        const float input = in[0][i];
        const uint32_t randomValue = NextRandom();
        const float noise = RandomBipolar(randomValue);

        const float dropoutProbability = dropouts
                                        * (tuning::dropoutRateAtMaxPerSecond / sampleRate)
                                        * kRandom24Max;
        if(dropoutSamples <= 0.0f && dropouts > 0.0f
           && static_cast<float>(randomValue & 0x00ffffffu) < dropoutProbability)
            dropoutSamples
                = sampleRate
                  * (tuning::dropoutMinMs
                     + (noise + 1.0f) * 0.5f * tuning::dropoutDurationRangeMs)
                  * 0.001f;

        const float dropoutLevel
            = dropoutSamples > 0.0f ? tuning::dropoutGain : 1.0f;
        dropoutSamples = fmaxf(0.0f, dropoutSamples - 1.0f);

        const float saturated = tanhf(input * drive) * saturationNorm;
        delayBuffer[writeIndex] = saturated * dropoutLevel;

        const float wowMotion = sinf(wowPhase) * wowDepth;
        const float unstableMotion = unstableTape
                                         ? sinf(wowPhase * tuning::secondaryWowRateRatio
                                                  + tuning::secondaryWowPhaseOffsetRadians)
                                               * wowDepth
                                               * tuning::secondaryWowDepthRatio
                                         : 0.0f;
        const float flutterMotion = sinf(flutterPhase) * flutterDepth;
        const float modulation = wowMotion + unstableMotion + flutterMotion;
        const float baseDelay = sampleRate * tuning::baseDelayMs * 0.001f;
        const float delaySamples = fminf(static_cast<float>(kDelayBufferSize - 2),
                                             fmaxf(tuning::minimumDelaySamples,
                                               baseDelay + modulation + stopDelay));

        const float delayed = ReadDelay(delaySamples);
          const float hiss = noise
                       * (hissEnabled
                            ? tuning::hissBaseAmplitude
                                + wear * tuning::hissWearAmplitude
                            : wear * tuning::hissWithoutSwitchAmplitude);
        lowpassState += lowpassCoeff * (delayed + hiss - lowpassState);

          const float targetSpeed
            = tapeStopHeld
                ? tuning::tapeStopSpeed
                : (stopDelay > tuning::stopResumeThresholdSamples
                     ? tuning::tapeRecoverySpeed
                     : 1.0f);
          const float speedCoeff = tapeStopHeld ? tuning::stopAttackSmoothing
                                    : tuning::stopReleaseSmoothing;
        tapeSpeed += (targetSpeed - tapeSpeed) * speedCoeff;
        stopDelay = fminf(stopDelayLimit,
                          fmaxf(0.0f, stopDelay + 1.0f - tapeSpeed));

        const float effectMix = effectEnabled ? mix : 0.0f;
        blendState += (effectMix - blendState) * tuning::blendSmoothing;
        const float wet = lowpassState * tapeSpeed * level;
        const float output = input + (wet - input) * blendState;
        out[0][i] = output;
        out[1][i] = output;

        wowPhase += kTwoPi
                * (unstableTape ? tuning::unstableWowRateHz : tuning::wowRateHz)
                / sampleRate;
        flutterPhase += kTwoPi
                * (unstableTape ? tuning::unstableFlutterRateHz
                        : tuning::flutterRateHz)
                / sampleRate;
        if(wowPhase >= kTwoPi)
            wowPhase -= kTwoPi;
        if(flutterPhase >= kTwoPi)
            flutterPhase -= kTwoPi;

        writeIndex = (writeIndex + 1) & kDelayBufferMask;
    }
}
} // namespace

int main(void)
{
    hw.Init();

    const Pin potPins[6] = {seed::D16, seed::D17, seed::D18,
                            seed::D19, seed::D20, seed::D21};
    AdcChannelConfig adcConfig[6];
    for(size_t i = 0; i < 6; ++i)
        adcConfig[i].InitSingle(potPins[i]);
    hw.adc.Init(adcConfig, 6);
    hw.adc.Start();

    const float controlRate = hw.AudioCallbackRate();
    for(size_t i = 0; i < 6; ++i)
        pots[i].Init(hw.adc.GetPtr(i), controlRate, false, false,
                 tuning::potSlewSeconds);

    const Pin togglePins[4] = {seed::D10, seed::D9, seed::D8, seed::D7};
    for(size_t i = 0; i < 4; ++i)
        toggles[i].Init(togglePins[i], 1000.0f, Switch::TYPE_TOGGLE,
                        Switch::POLARITY_INVERTED);
    bypassSwitch.Init(seed::D25, 1000.0f, Switch::TYPE_MOMENTARY,
                      Switch::POLARITY_INVERTED);
    stopSwitch.Init(seed::D26, 1000.0f, Switch::TYPE_MOMENTARY,
                    Switch::POLARITY_INVERTED);

    effectLed.Init(seed::D22, GPIO::Mode::OUTPUT);
    stopLed.Init(seed::D23, GPIO::Mode::OUTPUT);

    hw.StartAudio(AudioCallback);
    while(1)
    {
        for(size_t i = 0; i < 4; ++i)
        {
            toggles[i].Debounce();
            toggleOn[i] = toggles[i].Pressed();
        }

        bypassSwitch.Debounce();
        stopSwitch.Debounce();
        if(bypassSwitch.RisingEdge())
            effectEnabled = !effectEnabled;
        tapeStopHeld = stopSwitch.Pressed();

        effectLed.Write(effectEnabled);
        stopLed.Write(tapeStopHeld);
        System::Delay(1);
    }
}