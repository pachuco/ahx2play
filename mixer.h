#pragma once

#include <stdint.h>
#include <stdbool.h>

// main crystal oscillator for PAL Amiga systems
#define AMIGA_PAL_XTAL_HZ 28375160

#define AMIGA_PAL_CCK_HZ (AMIGA_PAL_XTAL_HZ / 8.0)
#define PAULA_PAL_CLK AMIGA_PAL_CCK_HZ
#define CIA_PAL_CLK (AMIGA_PAL_CCK_HZ / 5.0)

#define AMIGA_VOICES 4

typedef struct
{
    volatile bool DMA_active;

    // internal values (don't modify directly!)
    int8_t AUD_DAT[2]; // DMA data buffer
    const int8_t *location; // current location
    uint16_t lengthCounter; // current length
    int32_t sampleCounter; // how many bytes left in AUD_DAT
    int32_t sample; // current sample point

    // registers modified by Paula functions
    const int8_t *AUD_LC; // location
    uint16_t AUD_LEN; // length (in words)
    uint64_t AUD_PER_delta; // delta
    int32_t AUD_VOL; // volume

    uint64_t delta, oldVoiceDelta, phase;

    // period cache
    int32_t oldPeriod;
} paulaVoice_t;

typedef struct
{
    paulaVoice_t paula[AMIGA_VOICES];
    volatile bool playing, pause;
    int32_t outputFreq, masterVol, stereoSeparation;
    int64_t tickSampleCounter64, samplesPerTick64;
    
    //
    double dSideFactor, dPeriodToDeltaDiv, dMixNormalize;
    int32_t avgSmpMul;
} audio_t;

void resetCachedMixerPeriod(audio_t *audio);

double amigaCIAPeriod2Hz(uint16_t period);
bool amigaSetCIAPeriod(audio_t *audio, uint16_t period); // replayer ticker speed

void paulaInit(audio_t *audio, int32_t audioFrequency);

void paulaSetMasterVolume(audio_t *audio, int32_t vol);
void paulaSetStereoSeparation(audio_t *audio, int32_t percentage); // 0..100 (percentage)

void paulaTogglePause(audio_t *audio);
void paulaOutputSamples(audio_t *audio, int16_t *stream, int32_t numSamples);
void paulaStopAllDMAs(audio_t *audio);
void paulaStartAllDMAs(audio_t *audio);
void paulaSetPeriod(audio_t *audio, int32_t ch, uint16_t period);
void paulaSetVolume(audio_t *audio, int32_t ch, uint16_t vol);
void paulaSetLength(audio_t *audio, int32_t ch, uint16_t len);
void paulaSetData(audio_t *audio, int32_t ch, const int8_t *src);
