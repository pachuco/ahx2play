#pragma once

#include <stdint.h>
#include <stdbool.h>

// main crystal oscillator for PAL Amiga systems
#define AMIGA_PAL_XTAL_HZ 28375160

#define AMIGA_PAL_CCK_HZ (AMIGA_PAL_XTAL_HZ / 8.0)
#define PAULA_PAL_CLK AMIGA_PAL_CCK_HZ
#define CIA_PAL_CLK (AMIGA_PAL_CCK_HZ / 5.0)

#define AMIGA_VOICES 4

typedef struct voice_t
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

typedef struct audio_t
{
    paulaVoice_t paula[AMIGA_VOICES];
    volatile bool playing, pause;
    int32_t outputFreq, masterVol, stereoSeparation;
    int64_t tickSampleCounter64, samplesPerTick64;
    
    //
    double dSideFactor, dPeriodToDeltaDiv, dMixNormalize;
    int32_t avgSmpMul;
} audio_t;

void paulaClearFilterState(void);
void resetCachedMixerPeriod(void);
void resetAudioDithering(void);

double amigaCIAPeriod2Hz(uint16_t period);
bool amigaSetCIAPeriod(uint16_t period); // replayer ticker speed

void paulaInit(int32_t audioFrequency);

void paulaSetMasterVolume(int32_t vol);
void paulaSetStereoSeparation(int32_t percentage); // 0..100 (percentage)

void paulaTogglePause(void);
void paulaOutputSamples(int16_t *stream, int32_t numSamples);
void paulaStopAllDMAs(void);
void paulaStartAllDMAs(void);
void paulaSetPeriod(int32_t ch, uint16_t period);
void paulaSetVolume(int32_t ch, uint16_t vol);
void paulaSetLength(int32_t ch, uint16_t len);
void paulaSetData(int32_t ch, const int8_t *src);

extern audio_t audio; // paula.c
