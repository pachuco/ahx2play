/*
** 8bb:
** Port of AHX 2.3d-sp3's replayer.
**
** This is a port of the replayer found in AHX 2.3d-sp3's tracker code,
** not the small external replayer binary. There are some minor
** differences between them.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> // ceil()
#include "replayer.h"

static waveforms_t waves;
static bool isInitWaveforms = false;

// 8bb: added +1 to all values in this table (was meant for 68k DBRA loop)
static const uint16_t lengthTable[6+6+32+1] =
{
    0x04,0x08,0x10,0x20,0x40,0x80,
    0x04,0x08,0x10,0x20,0x40,0x80,

    0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,

    NOIZE_SIZE
};

static void triangleGenerate(int8_t *dst8, int16_t delta, int32_t offset, int32_t length)
{
    int16_t data = 0;
    for (int32_t i = 0; i < length+1; i++)
    {
        *dst8++ = (uint8_t)data;
        data += delta;
    }
    *dst8++ = 127;

    data = 128;
    for (int32_t i = 0; i < length; i++)
    {
        data -= delta;
        *dst8++ = (uint8_t)data;
    }

    int8_t *src8 = &dst8[offset];
    for (int32_t i = 0; i < (length+1)*2; i++)
    {
        int8_t sample = *src8++;
        if (sample == 127)
            sample = -128;
        else
            sample = 0 - sample;

        *dst8++ = sample;
    }
}

static void sawToothGenerate(int8_t *dst8, int32_t length)
{
    const int8_t delta = (int8_t)(256 / (length-1));

    int8_t data = -128;
    for (int32_t i = 0; i < length; i++)
    {
        *dst8++ = data;
        data += delta;
    }
}

static void squareGenerate(int8_t *dst8)
{
    uint16_t *dst16 = (uint16_t *)dst8;
    for (int32_t i = 1; i <= 32; i++)
    {
        for (int32_t j = 0; j < 64-i; j++)
            *dst16++ = 0x8080;

        for (int32_t j = 0; j < i; j++)
            *dst16++ = 0x7F7F;
    }
}

static void whiteNoiseGenerate(int8_t *dst8, int32_t length)
{
    uint32_t seed = 0x41595321; // 8bb: "AYS!"
    for (int32_t i = 0; i < length; i++)
    {
        if (!(seed & 256))
            *dst8++ = (uint8_t)seed;
        else if (seed & 0x8000)
            *dst8++ = -128;
        else
            *dst8++ = 127;

        ROR32(seed, 5);
        seed ^= 0b10011010;
        uint16_t tmp16 = (uint16_t)seed;
        ROL32(seed, 2);
        tmp16 += (uint16_t)seed;
        seed ^= tmp16;
        ROR32(seed, 3);
    }
}

static inline int32_t fp16Clip(int32_t x)
{
    int16_t fp16Int = x >> 16;

    if (fp16Int > 127)
    {
        fp16Int = 127;
        return fp16Int << 16;
    }
    
    if (fp16Int < -128)
    {
        fp16Int = -128;
        return fp16Int << 16;
    }

    return x;
}

static void setUpFilterWaveForms(int8_t *dst8Hi, int8_t *dst8Lo, int8_t *src8)
{
    int32_t d5 = ((((8<<16)*125)/100)/100)>>8;
    for (int32_t i = 0; i < 31; i++)
    {
        int32_t wlAdd = 0;
        for (int32_t j = 0; j < 6+6+32+1; j++)
        {
            const int32_t waveLength = lengthTable[j];
            
            int32_t d1;
            int32_t d2 = 0;
            int32_t d3 = 0;
            
            // 4 passes
            for (int32_t k = 1; k <= 4; k++)
            {
                /* Truncate lower 8 bits on 4th pass
                ** to simulate bit reduced LUT of ahx->
                ** Bit perfect result.
                */
                if (k == 4)
                {
                    d2 &= ~0xFF;
                    d3 &= ~0xFF;
                }
                
                for (int32_t l = 0; l < waveLength; l++)
                {
                    const int32_t d0 = (int16_t)src8[wlAdd + l] << 16;

                    d1 = fp16Clip(d0 - d2 - d3);
                    d2 = fp16Clip(d2 + ((d1 >> 8) * d5));
                    d3 = fp16Clip(d3 + ((d2 >> 8) * d5));
                    
                    if (k == 4) 
                    {
                        *dst8Hi++ = (uint8_t)(d1 >> 16);
                        *dst8Lo++ = (uint8_t)(d3 >> 16);
                    }
                }
            }
            
            wlAdd += waveLength; // 8bb: go to next waveform
        }

        d5 += ((((3<<16)*125)/100)/100)>>8;
    }
}

uint32_t crc32b(uint8_t *data, int length) {
   uint32_t crc = 0xFFFFFFFF;
   
   for (int i = 0; i < length; i++) {
      crc ^= data[i];
      
      for (int j = 0; j < 8; j++) { crc = (crc >> 1) ^ (0xEDB88320 * (crc & 1)); }
   }
   return ~crc;
}

    #include <assert.h>
void ahxInitWaves(void) // 8bb: this generates bit-accurate AHX 2.3d-sp3 waveforms
{
    if (isInitWaveforms) return;

    int8_t *dst8 =  waves.triangle04;
    for (int32_t i = 0; i < 6; i++)
    {
        uint16_t fullLength = 4 << i;
        uint16_t length = fullLength >> 2;
        uint16_t delta = 128 / length;
        int32_t offset = 0 - (fullLength >> 1);

        triangleGenerate(dst8, delta, offset, length-1);
        dst8 += fullLength;
    }

    sawToothGenerate(waves.sawtooth04, 0x04);
    sawToothGenerate(waves.sawtooth08, 0x08);
    sawToothGenerate(waves.sawtooth10, 0x10);
    sawToothGenerate(waves.sawtooth20, 0x20);
    sawToothGenerate(waves.sawtooth40, 0x40);
    sawToothGenerate(waves.sawtooth80, 0x80);
    squareGenerate(waves.squares);
    whiteNoiseGenerate(waves.whiteNoiseBig, NOIZE_SIZE);

    setUpFilterWaveForms(waves.highPasses, waves.lowPasses, waves.triangle04);
    isInitWaveforms = true;
    
    assert(crc32b((uint8_t *)&waves, 410760) == 0x40EEB1B9);
}
//---------------------------------------------------------------------------------------------------------------

#define TEMPBUFSIZE 512

static const uint8_t waveOffsets[6] =
{
    0x00,0x04,0x04+0x08,0x04+0x08+0x10,0x04+0x08+0x10+0x20,0x04+0x08+0x10+0x20+0x40
};

/* 8bb:
** Added this. 129 words from before the periodTable in AHX 2.3d-sp3 (68020 version).
** This is needed because the final note can accidentally be -1 .. -129, which is not
** safe when accesing the periodTable.
*/
static const int16_t beforePeriodTable_68020[129] =
{
    0xF6F2,0xEEEA,0xE6E3,0x201B,0x1612,0x0E0A,0x0603,0x00FD,0xFAF8,0xF6F4,
    0xF2F1,0x100D,0x0A08,0x0604,0x0201,0x00FF,0xFEFE,0xFEFE,0xFEFF,0x4A30,
    0x0170,0x0000,0x0027,0x66FF,0x0000,0x00B2,0x4A30,0x0170,0x0000,0x0026,
    0x6712,0x3770,0x0170,0x0000,0x0064,0x0006,0x51F0,0x0170,0x0000,0x0026,
    0x4A30,0x0170,0x0000,0x0022,0x67FF,0x0000,0x007C,0x48E7,0x3F68,0x2470,
    0x0170,0x0000,0x005C,0x0C30,0x0003,0x0170,0x0000,0x0014,0x67FF,0x0000,
    0x0042,0x7C01,0x7405,0x9430,0x0170,0x0000,0x0015,0xE56E,0xCCFC,0x0005,
    0x5346,0x2270,0x0170,0x0000,0x0060,0x7E01,0x7400,0x1430,0x0170,0x0000,
    0x0015,0xE52F,0x5347,0x2619,0x24C3,0x51CF,0xFFFA,0x51CE,0xFFDE,0x60FF,
    0x0000,0x0016,0x2270,0x0170,0x0000,0x0060,0x7E4F,0x24D9,0x24D9,0x51CF,
    0xFFFA,0x4CDF,0x16FC,0x51F0,0x0170,0x0000,0x0022,0x3770,0x0170,0x0000,
    0x0066,0x0008,0x4E75,0x377C,0x0000,0x0008,0x4E75,0x0004,0x0000,0x0001,
    0x0000,0x0015,0x4C70,0x0015,0x4D6C,0x000E,0xA9C4,0x0015,0x5E68
};

static const int16_t periodTable[1+60] =
{
    0,
    3424,3232,3048,2880,2712,2560,2416,2280,2152,2032,1920,1812,
    1712,1616,1524,1440,1356,1280,1208,1140,1076,1016, 960, 906,
     856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
     428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
     214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113
};

static const int16_t vibTable[64] =
{
       0,  24,  49,  74,  97, 120, 141, 161,
     180, 197, 212, 224, 235, 244, 250, 253,
     255, 253, 250, 244, 235, 224, 212, 197,
     180, 161, 141, 120,  97,  74,  49,  24,
       0, -24, -49, -74, -97,-120,-141,-161,
    -180,-197,-212,-224,-235,-244,-250,-253,
    -255,-253,-250,-244,-235,-224,-212,-197,
    -180,-161,-141,-120, -97, -74, -49, -24
};

// 8bb: AHX-header tempo value (0..3) -> Amiga PAL CIA period
static const uint16_t tabler[4] = { 14209, 7104, 4736, 3552 };

// 8bb: set default values for EmptyInstrument (used for non-loaded instruments in replayer)
static instrument_t EmptyInstrument =
{
    .aFrames = 1,
    .dFrames = 1,
    .sFrames = 1,
    .rFrames = 1,
    .perfSpeed = 1,
    .squareLowerLimit = 0x20,
    .squareUpperLimit = 0x3F,
    .squareSpeed = 1,
    .filterLowerLimit = 1,
    .filterUpperLimit = 0x1F,
    .filterSpeedWavelength = 4<<3, // fs 3 wl 04 !!
};

// 8bb: The size is just big enough, don't change it!
static int8_t EmptyFilterSection[0x80 * 32] = {0};

// 8bb: globalized
replayer_t ahx = {0};
uint8_t ahxErrCode;

// ------------

static void SetUpAudioChannels(replayer_t *ahx) // 8bb: only call this while mixer is locked!
{
    plyVoiceTemp_t *ch;

    paulaStopAllDMAs(&ahx->audio);

    ch = ahx->pvt;
    for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
    {
        ch->audioPointer = ahx->currentVoice[i];

        paulaSetPeriod(&ahx->audio, i, 0x88);
        paulaSetData(&ahx->audio, i, ch->audioPointer);
        paulaSetVolume(&ahx->audio, i, 0);
        paulaSetLength(&ahx->audio, i, 0x280 / 2);
    }

    paulaStartAllDMAs(&ahx->audio);
}

static void InitVoiceXTemp(plyVoiceTemp_t *ch) // 8bb: only call this while mixer is locked!
{
    //int8_t *oldAudioPointer = ch->audioPointer;

    memset(ch, 0, sizeof (plyVoiceTemp_t));

    ch->TrackMasterVolume = 64;
    ch->squareSignum = 1;
    ch->squareLowerLimit = 1;
    ch->squareUpperLimit = 63;

    //ch->audioPointer = oldAudioPointer;
}

static void ahxQuietAudios(replayer_t *ahx)
{
    for (int32_t i = 0; i < AMIGA_VOICES; i++)
        paulaSetVolume(&ahx->audio, i, 0);
}

static void CopyWaveformToPaulaBuffer(plyVoiceTemp_t *ch) // 8bb: I put this code in an own function
{
    if (ch->Waveform == 4-1) // 8bb: noise, copy in one go
    {
        memcpy(ch->audioPointer, ch->audioSource, 0x280);
    }
    else
    {
        const int32_t waveLoops = (1 << (5 - ch->Wavelength)) * 5;
        const int32_t copyLength = (1 << ch->Wavelength) * 4;

        for (int32_t i = 0; i < waveLoops; i++)
            memcpy(ch->audioPointer + (i * copyLength), ch->audioSource, copyLength);
    }
}

static void SetAudio(replayer_t *ahx, int32_t chNum, plyVoiceTemp_t *ch)
{
    // new PERIOD to plant ???
    if (ch->PlantPeriod)
    {
        paulaSetPeriod(&ahx->audio, chNum, ch->audioPeriod);
        ch->PlantPeriod = false;
    }

    // new FILTER or new WAVEFORM ???
    if (ch->NewWaveform)
    {
        CopyWaveformToPaulaBuffer(ch);
        ch->NewWaveform = false;
    }

    paulaSetVolume(&ahx->audio, chNum, ch->audioVolume);
}

static void ProcessStep(replayer_t *ahx, plyVoiceTemp_t *ch)
{
    uint8_t note, instr, cmd, param;

    ch->volumeSlideUp = 0; // means A cmd
    ch->volumeSlideDown = 0; // means A cmd

    if (ch->Track > ahx->song->highestTrack) // 8bb: added this (this is technically what happens in AHX on illegal tracks)
    {
        note = 0;
        instr = 0;
        cmd = 0;
        param = 0;
    }
    else
    {
        const uint8_t *bytes = &ahx->song->TrackTable[((ch->Track << 6) + ahx->NoteNr) * 3];

        note = (bytes[0] >> 2) & 0x3F;
        instr = ((bytes[0] & 3) << 4) | (bytes[1] >> 4);
        cmd = bytes[1] & 0xF;
        param = bytes[2];
    }

    // Effect  > E <  -  Enhanced Commands
    if (cmd == 0xE)
    {
        uint8_t eCmd = param >> 4;
        uint8_t eParam = param & 0xF;

        if (eCmd == 0xC) // Effect  > EC<  -  NoteCut
        {
            if (eParam < ahx->Tempo)
            {
                ch->NoteCutWait = eParam;
                ch->NoteCutOn = true;
                ch->HardCutRelease = false;
            }
        }

        if (eCmd == 0xD) // Effect  > ED<  -  NoteDelay
        {
            if (ch->NoteDelayOn)
            {
                ch->NoteDelayOn = false;
            }
            else if (eParam < ahx->Tempo)
            {
                ch->NoteDelayWait = eParam;
                if (ch->NoteDelayWait != 0)
                {
                    ch->NoteDelayOn = true;
                    return; // 8bb: yes, get out of here!
                }
            }
        }
    }

    if (cmd == 0x0) // Effect  > 0 <  -  PositionjumpHI
    {
        if (param != 0)
        {
            uint8_t pos = param & 0xF;
            if (pos <= 9)
                ahx->PosJump = (param & 0xF) << 8; // 8bb: yes, this clears the lower byte too!
        }
    }

    // 8bb: effect 8 is for external timing/syncing (probably for games/demos).
    if (cmd == 0x8) {
        
    }

    if (cmd == 0xD) // Effect  > D <  -  Patternbreak
    {
        ahx->PosJump = ahx->PosNr + 1; // jump to next position (8bb: yes, it clears PosJump hi-byte)

        ahx->PosJumpNote = ((param >> 4) * 10) + (param & 0xF);
        if (ahx->PosJumpNote >= ahx->song->TrackLength)
            ahx->PosJumpNote = 0;

        ahx->PatternBreak = true;
    }

    if (cmd == 0xB) // Effect  > B <  -  Positionjump
    {
        ahx->PosJump = (ahx->PosJump * 100) + ((param >> 4) * 10) + (param & 0xF);
        ahx->PatternBreak = true;
    }

    if (cmd == 0xF) // Effect  > F <  -  Set Tempo
    {
        ahx->Tempo = param;

        // 8bb: added this for the WAV renderer
        if (ahx->Tempo == 0)
            ahx->isRecordingToWAV = false;
    }

    // Effect  > 5 <  -  Volume Slide + Tone Portamento
    // Effect  > A <  -  Volume Slide
    if (cmd == 0x5 || cmd == 0xA)
    {
        ch->volumeSlideDown = param & 0xF;
        ch->volumeSlideUp = param >> 4;
    }

    // Instrument to initialize ?
    if (instr > 0)
    {
        int16_t delta;

        ch->perfSubVolume = 64;

        // reset portamento
        ch->periodPerfSlideSpeed = 0;
        ch->periodSlidePeriod = 0;
        ch->periodSlideLimit = 0;

        // init adsr-envelope
        instrument_t *ins = ahx->song->Instruments[instr-1];
        if (ins == NULL) // 8bb: added this (this is technically what happens in AHX on illegal instruments)
            ins = &EmptyInstrument;

        ch->adsr = 0; // adsr starting at vol. 0!

        ch->aFrames = ins->aFrames;
        delta = ins->aVolume << 8;
        if (ch->aFrames != 0)
            delta /= ch->aFrames;
        ch->aDelta = delta;

        ch->dFrames = ins->dFrames;
        delta = ((int8_t)ins->dVolume - (int8_t)ins->aVolume) << 8;
        if (ch->dFrames != 0)
            delta /= ch->dFrames;
        ch->dDelta = delta;

        ch->sFrames = ins->sFrames;

        ch->rFrames = ins->rFrames;
        delta = ((int8_t)ins->rVolume - (int8_t)ins->dVolume) << 8;
        if (ch->rFrames != 0)
            delta /= ch->rFrames;
        ch->rDelta = delta;

        // copy Instrument values
        ch->Wavelength = ins->filterSpeedWavelength & 0b00000111;

        if (ch->Wavelength > 5) // 8bb: safety bug-fix...
            ch->Wavelength = 5;

        ch->NoteMaxVolume = ins->Volume;

        ch->vibratoCurrent = 0;
        ch->vibratoDelay = ins->vibratoDelay;
        ch->vibratoDepth = ins->vibratoDepth & 0b00001111;
        ch->vibratoSpeed = ins->vibratoSpeed;
        ch->VibratoPeriod = 0;
        ch->HardCutRelease = !!(ins->vibratoDepth & 128);
        ch->HardCut = (ins->vibratoDepth & 0b01110000) >> 4;

        ch->IgnoreSquare = false; // don't ignore the 3xx...
        ch->squareSlidingIn = false;
        ch->squareWait = 0;
        ch->squareOn = false;

        uint8_t lowerLimit = ins->squareLowerLimit >> (5 - ch->Wavelength);
        uint8_t upperLimit = ins->squareUpperLimit >> (5 - ch->Wavelength);

        if (lowerLimit <= upperLimit)
        {
            ch->squareLowerLimit = lowerLimit;
            ch->squareUpperLimit = upperLimit;
        }
        else
        {
            ch->squareLowerLimit = upperLimit;
            ch->squareUpperLimit = lowerLimit;
        }

        ch->IgnoreFilter = 0;
        ch->filterWait = 0;
        ch->filterOn = false;
        ch->filterSlidingIn = false;

        ch->filterSpeed = ins->filterSpeedWavelength >> 3; // shift out wavelength!

        lowerLimit = ins->filterLowerLimit;
        upperLimit = ins->filterUpperLimit;
        if (lowerLimit & 128) ch->filterSpeed |= 32;
        if (upperLimit & 128) ch->filterSpeed |= 64;
        lowerLimit &= ~128;
        upperLimit &= ~128;

        if (lowerLimit <= upperLimit)
        {
            ch->filterLowerLimit = lowerLimit;
            ch->filterUpperLimit = upperLimit;
        }
        else
        {
            ch->filterLowerLimit = upperLimit;
            ch->filterUpperLimit = lowerLimit;
        }

        ch->filterPos = 32; // std: no filter!
        ch->perfWait = 0;
        ch->perfSpeed = ins->perfSpeed;
        ch->perfCurrent = 0;

        ch->Instrument = ins;
        ch->perfList = ins->perfList;
    }

    if (cmd == 0x9) // Effect  > 9 <  -  Set Squarewave-Offset
    {
        ch->squarePos = param >> (5 - ch->Wavelength);
        ch->PlantSquare = true; // now set relation...
        ch->IgnoreSquare = true; // ignore next following 3xx cmd.
    }

    if (cmd == 0x4) // Effect  > 4 <  -  Override Filter
    {
        if (param < 0x40)
            ch->IgnoreFilter = param;
        else
            ch->filterPos = param - 0x40;
    }

    ch->periodSlideOn = false;

    // Effect  > 5 <  -  TonePortamento+Volume Slide
    // Effect  > 3 <  -  TonePortamento (periodSlide Up/Down w/ Limit)
    if (cmd == 0x3 || cmd == 0x5)
    {
        if (cmd == 0x3 && param != 0)
            ch->periodSlideSpeed = param;

        bool doSlide = true;
        if (note != 0)
        {
            int16_t periodLimit = periodTable[ch->TrackPeriod] - periodTable[note]; // (ABS) SLIDE LIMIT

            const uint16_t test = periodLimit + ch->periodSlidePeriod;
            if (test == 0) // c-1 -> c-1....
                doSlide = false;
            else
                ch->periodSlideLimit = 0 - periodLimit; // neg/pos!!
        }

        if (doSlide)
        {
            ch->periodSlideOn = true;
            ch->periodSlideWithLimit = true;
            note = 0; // 8bb: don't trigger note
        }
    }

    if (note != 0)
    {
        ch->TrackPeriod = note;
        ch->PlantPeriod = true;
    }

    if (cmd == 0x1) // Effect  > 1 <  -  Portamento Up (periodSlide Down)
    {
        ch->periodSlideSpeed = 0 - param;
        ch->periodSlideOn = true;
        ch->periodSlideWithLimit = false;
    }

    if (cmd == 0x2) // Effect  > 2 <  -  Portamento Down (periodSlide Up)
    {
        ch->periodSlideSpeed = param;
        ch->periodSlideOn = true;
        ch->periodSlideWithLimit = false;
    }

    if (cmd == 0xE) // Effect  > E <  -  Enhanced Commands
    {
        uint8_t eCmd = param >> 4;
        uint8_t eParam = param & 0xF;

        if (eCmd == 0x1) // Effect  > E1<  -  FineSlide Up (periodFineSlide Down)
        {
            ch->periodSlidePeriod += 0 - eParam;
            ch->PlantPeriod = true;
        }

        if (eCmd == 0x2) // Effect  > E2<  -  FineSlide Down (periodFineSlide Up)
        {
            ch->periodSlidePeriod += eParam;
            ch->PlantPeriod = true;
        }

        if (eCmd == 0x4) // Effect  > E4<  -  Vibrato Control
            ch->vibratoDepth = eParam;

        if (eCmd == 0xA) // Effect  > EA<  -  FineVolume Up
        {
            ch->NoteMaxVolume += eParam;
            if (ch->NoteMaxVolume > 0x40)
                ch->NoteMaxVolume = 0x40;
        }

        if (eCmd == 0xB) // Effect  > EB<  -  FineVolume Down
        {
            ch->NoteMaxVolume -= eParam;
            if ((int8_t)ch->NoteMaxVolume < 0)
                ch->NoteMaxVolume = 0;
        }
    }

    if (cmd == 0xC) // Effect  > C <  -  Set Volume
    {
        int16_t p = param;
        if (p <= 0x40)
        {
            ch->NoteMaxVolume = (uint8_t)p;
        }
        else
        {
            p -= 0x50;
            if (p >= 0)
            {
                if (p <= 0x40)
                {
                    // 8bb: set TrackMasterVolume for all channels
                    plyVoiceTemp_t *c = ahx->pvt;
                    for (int32_t i = 0; i < AMIGA_VOICES; i++, c++)
                        c->TrackMasterVolume = (uint8_t)p;
                }
                else
                {
                    p -= 0xA0-0x50;
                    if (p >= 0 && p <= 0x40)
                        ch->TrackMasterVolume = (uint8_t)p;
                }
            }
        }
    }
}

static void pListCommandParse(plyVoiceTemp_t *ch, uint8_t cmd, uint8_t param)
{
    if (cmd == 0x0) // 8bb: Init Filter Modulation
    {
        if (param == 0)
            return; // cmd 0-00 is STILL nuttin'

        if (ch->IgnoreFilter)
        {
            ch->filterPos = ch->IgnoreFilter;
            ch->IgnoreFilter = false;
        }
        else
        {
            ch->filterPos = param;
            ch->NewWaveform = true;
        }
    }

    else if (cmd == 0x1) // 8bb: Slide Up
    {
        ch->periodPerfSlideSpeed = param;
        ch->periodPerfSlideOn = true;
    }

    else if (cmd == 0x2) // 8bb: Slide Down
    {
        ch->periodPerfSlideSpeed = 0 - param;
        ch->periodPerfSlideOn = true;
    }

    else if (cmd == 0x3) // Init Square Modulation
    {
        if (ch->IgnoreSquare)
            ch->IgnoreSquare = false;
        else
            ch->squarePos = param >> (5 - ch->Wavelength);
    }

    else if (cmd == 0x4) // Start/Stop Modulation
    {
        if (param == 0) // 400 is downwards-compatible, means modulate square!!
        {
            ch->squareOn ^= 1;
            ch->squareInit = ch->squareOn;
            ch->squareSignum = 1;
        }
        else
        {
            if (param & 0x0F)
            {
                ch->squareOn ^= 1; // any value? FILTER MOD!!
                ch->squareInit = ch->squareOn;

                ch->squareSignum = 1;
                if ((param & 0x0F) == 0x0F) // filter +1 ???
                    ch->squareSignum = 0 - ch->squareSignum;
            }

            if (param & 0xF0)
            {
                ch->filterOn ^= 1; // any value? FILTER MOD!!
                ch->filterInit = ch->filterOn;

                ch->filterSignum = 1;
                if ((param & 0xF0) == 0xF0) // filter +1 ???
                    ch->filterSignum = 0 - ch->filterSignum;
            }
        }
    }

    else if (cmd == 0x5) // Jump to Step [xx]
    {
        instrument_t *ins = ch->Instrument;
        if (ins == NULL) // 8bb: safety bug-fix...
            ins = &EmptyInstrument;

        // 8bb: 4 bytes before perfList (this is apparently what AHX does...)
        uint8_t *perfList = ins->perfList - 4;

        /* 8bb: AHX quirk! There's no range check here.
        ** You should have 4*256 perfList bytes for every instrument.
        ** The bytes after 4*perfLength should be zeroed out in the loader.
        **
        ** AHX does this, and it HAS to be done! Example: lead instrument on "GavinsQuest.ahx".
        */

        ch->perfCurrent = param - 1; // 8bb: param-1 is correct (0 -> 255 = safe)
        ch->perfList = &perfList[param << 2]; // 8bb: don't do param-1 here!
    }

    else if (cmd == 0x6) // Set Volume (Command C)
    {
        int16_t p = param;
        if (p <= 0x40)
        {
            ch->NoteMaxVolume = (uint8_t)p;
        }
        else
        {
            p -= 0x50;
            if (p >= 0)
            {
                if (p <= 0x40)
                {
                    ch->perfSubVolume = (uint8_t)p;
                }
                else
                {
                    p -= 0xA0-0x50;
                    if (p >= 0 && p <= 0x40)
                        ch->TrackMasterVolume = (uint8_t)p;
                }
            }
        }
    }

    else if (cmd == 0x7) // Set Speed (Command F)
    {
        ch->perfSpeed = param;
        ch->perfWait = param;
    }
}

static void ProcessFrame(replayer_t *ahx, plyVoiceTemp_t *ch)
{
    if (ch->HardCut != 0)
    {
        uint8_t track = ch->Track;

        uint16_t noteNr = ahx->NoteNr + 1; // chk next note!
        if (noteNr == ahx->song->TrackLength)
        {
            noteNr = 0; // note 0 from next pos!
            track = ch->NextTrack;
        }

        const uint8_t *bytes = &ahx->song->TrackTable[((track << 6) + noteNr) * 3];

        uint8_t nextInstr = ((bytes[0] & 3) << 4) | (bytes[1] >> 4);
        if (nextInstr != 0)
        {
            int8_t range = ahx->Tempo - ch->HardCut; // range 1->7, tempo=6, hc=1, cut at tick 5, right
            if (range < 0)
                range = 0; // tempo=2, hc=7, cut at tick 0 (NOW!!)

            if (!ch->NoteCutOn)
            {
                ch->NoteCutOn = true;
                ch->NoteCutWait = range;
                ch->HardCutReleaseF = 0 - (ch->NoteCutWait - ahx->Tempo);
            }

            ch->HardCut = 0;
        }
    }

    if (ch->NoteCutOn)
    {
        if (ch->NoteCutWait == 0)
        {
            ch->NoteCutOn = false;
            if (ch->HardCutRelease)
            {
                instrument_t *ins = ch->Instrument;
                if (ins == NULL) // 8bb: safety bug-fix...
                    ins = &EmptyInstrument;

                ch->rFrames = ch->HardCutReleaseF;
                ch->rDelta = 0 - ((ch->adsr - (ins->rVolume << 8)) / ch->HardCutReleaseF);
                ch->aFrames = 0;
                ch->dFrames = 0;
                ch->sFrames = 0;
            }
            else
            {
                ch->NoteMaxVolume = 0;
            }
        }

        ch->NoteCutWait--;
    }

    if (ch->NoteDelayOn)
    {
        if (ch->NoteDelayWait == 0)
            ProcessStep(ahx, ch);
        else
            ch->NoteDelayWait--;
    }

    instrument_t *ins = ch->Instrument;
    if (ins == NULL) // 8bb: safety bug-fix...
        ins = &EmptyInstrument;

    if (ch->aFrames != 0)
    {
        ch->adsr += ch->aDelta;

        ch->aFrames--;
        if (ch->aFrames == 0)
            ch->adsr = ins->aVolume << 8;
    }
    else if (ch->dFrames != 0)
    {
        ch->adsr += ch->dDelta;

        ch->dFrames--;
        if (ch->dFrames == 0)
            ch->adsr = ins->dVolume << 8;
    }
    else if (ch->sFrames != 0)
    {
        ch->sFrames--;
    }
    else if (ch->rFrames != 0)
    {
        ch->adsr += ch->rDelta;

        ch->rFrames--;
        if (ch->rFrames == 0)
            ch->adsr = ins->rVolume << 8;
    }

    // Volume Slide Treatin'
    ch->NoteMaxVolume -= ch->volumeSlideDown;
    ch->NoteMaxVolume += ch->volumeSlideUp;
    ch->NoteMaxVolume = CLAMP((int8_t)ch->NoteMaxVolume, 0, 0x40);

    // Portamento Treatin' (periodSlide)
    if (ch->periodSlideOn)
    {
        if (ch->periodSlideWithLimit)
        {
            int16_t speed = ch->periodSlideSpeed;

            int16_t period = ch->periodSlidePeriod - ch->periodSlideLimit; // source-value
            if (period != 0)
            {
                if (period > 0)
                    speed = -speed;

                int16_t limitTest = (period + speed) ^ period;
                if (limitTest >= 0)
                    ch->periodSlidePeriod += speed;
                else
                    ch->periodSlidePeriod = ch->periodSlideLimit;

                ch->PlantPeriod = true;
            }
        }
        else
        {
            ch->periodSlidePeriod += ch->periodSlideSpeed;  // normal 1er/2er period slide!
            ch->PlantPeriod = true;
        }
    }

    // Vibrato Treatin'
    if (ch->vibratoDepth != 0)
    {
        if (ch->vibratoDelay != 0)
        {
            ch->vibratoDelay--;
        }
        else
        {
            ch->VibratoPeriod = (vibTable[ch->vibratoCurrent] * (int16_t)ch->vibratoDepth) >> 7;
            ch->PlantPeriod = true;
            ch->vibratoCurrent = (ch->vibratoCurrent + ch->vibratoSpeed) & 63;
        }
    }

    // pList Treatin'
    ins = ch->Instrument;
    if (ins != NULL)
    {
        if (ch->perfCurrent == ins->perfLength)
        {
            if (ch->perfWait != 0)
                ch->perfWait--;
            else
                ch->periodPerfSlideSpeed = 0; // only STOP sliding!!
        }
        else
        {
            /* 8bb: AHX QUIRK! Perf speed $80 results in no delay. This has to do with
            ** "sub.b #1,Dn | bgt.s .Delay". The BGT instruction will not branch if
            ** the register got overflown before the comparison (V flag set).
            ** WinAHX/ahx->cpp is not handling this correctly, porters beware!
            **
            ** "Enchanted Friday Nights" by JazzCat is a song that depends on this quirk
            ** for the lead instrument to sound right.
            */
            bool signedOverflow = (ch->perfWait == 128); // -128 as signed

            ch->perfWait--;
            if (signedOverflow || (int8_t)ch->perfWait <= 0) // 8bb: signed comparison is needed here
            {
                const uint8_t *bytes = ch->perfList;

                uint8_t cmd2 = (bytes[0] >> 5) & 7;
                uint8_t cmd1 = (bytes[0] >> 2) & 7;
                uint8_t wave = ((bytes[0] << 1) & 6) | (bytes[1] >> 7);
                bool fixed = (bytes[1] >> 6) & 1;
                uint8_t note = bytes[1] & 0x3F;
                uint8_t param1 = bytes[2];
                uint8_t param2 = bytes[3];
                
                // Check Waveform-Field from pList
                if (wave != 0)
                {
                    if (wave > 4) // 8bb: safety bug-fix...
                        wave = 0;

                    ch->Waveform = wave-1; // 0 to 3...
                    ch->NewWaveform = true; // New Waveform hit!
                    ch->periodPerfSlideSpeed = 0;
                    ch->periodPerfSlidePeriod = 0;
                }

                ch->periodPerfSlideOn = false;

                pListCommandParse(ch, cmd1, param1); // Check Command 1 in pList
                pListCommandParse(ch, cmd2, param2); // Check Command 2 in pList

                // Check Note(Fixed)-Field from pList
                if (note != 0)
                {
                    ch->InstrPeriod = note;
                    ch->PlantPeriod = true;
                    ch->FixedNote = fixed;
                }

                // End of Treatin! Goto next entry for next step!
                ch->perfList += 4;
                ch->perfCurrent++;
                ch->perfWait = ch->perfSpeed;
            }
        }
    }

    // ==========================================================================
    // =========================== Treat Waveforms ==============================
    // ==========================================================================
    // =========================== And Modulations ==============================
    // ==========================================================================

    // perfPortamento Treatin' (periodPerfSlide)
    if (ch->periodPerfSlideOn)
    {
        ch->periodPerfSlidePeriod -= ch->periodPerfSlideSpeed;
        if (ch->periodPerfSlidePeriod != 0)
            ch->PlantPeriod = true;
    }

    // Square Treatin' (Modulation-Stuff)
    if (ch->Waveform == 3-1 && ch->squareOn)
    {
        ch->squareWait--;
        if ((int8_t)ch->squareWait <= 0) // 8bb: signed comparison is needed here
        {
            if (ch->squareInit)
            {
                ch->squareInit = false;

                // 8bb: signed comparison is needed here
                if ((int8_t)ch->squarePos <= (int8_t)ch->squareLowerLimit)
                {
                    ch->squareSlidingIn = true;
                    ch->squareSignum = 1;
                }
                else if ((int8_t)ch->squarePos >= (int8_t)ch->squareUpperLimit)
                {
                    ch->squareSlidingIn = true;
                    ch->squareSignum = -1;
                }
            }

            if (ch->squarePos == ch->squareLowerLimit || ch->squarePos == ch->squareUpperLimit)
            {
                if (ch->squareSlidingIn)
                    ch->squareSlidingIn = false;
                else
                    ch->squareSignum = 0 - ch->squareSignum;
            }

            ch->squarePos += ch->squareSignum;
            ch->PlantSquare = true; // when modulating, refresh square!!
            ch->squareWait = ins->squareSpeed;
        }
    }

    // Filter Treatin' (Modulation-Stuff)
    if (ch->filterOn)
    {
        ch->filterWait--;
        if ((int8_t)ch->filterWait <= 0) // 8bb: signed comparison is needed here
        {
            if (ch->filterInit)
            {
                ch->filterInit = false;

                // 8bb: signed comparison is needed here
                if ((int8_t)ch->filterPos <= (int8_t)ch->filterLowerLimit)
                {
                    ch->filterSlidingIn = true;
                    ch->filterSignum = 1;
                }
                else if ((int8_t)ch->filterPos >= (int8_t)ch->filterUpperLimit)
                {
                    ch->filterSlidingIn = true;
                    ch->filterSignum = -1;
                }
            }

            int32_t cycles = 1;
            if (ch->filterSpeed < 4) // 8bb: < 4 is correct, not < 3 like in WinAHX/ahx->cpp!
                cycles = 5 - ch->filterSpeed;

            for (int32_t i = 0; i < cycles; i++)
            {
                if (ch->filterPos == ch->filterLowerLimit || ch->filterPos == ch->filterUpperLimit)
                {
                    if (ch->filterSlidingIn)
                        ch->filterSlidingIn = false;
                    else
                        ch->filterSignum = 0 - ch->filterSignum;
                }

                ch->filterPos += ch->filterSignum;
            }

            ch->NewWaveform = true;

            ch->filterWait = ch->filterSpeed - 3;
            if ((int8_t)ch->filterWait < 1)
                ch->filterWait = 1;
        }
    }

    // Square Treatin' (Calculation-Stuff)
    if (ch->Waveform == 3-1 || ch->PlantSquare)
    {
        const int8_t *src8;

        // 8bb: safety bug-fix... If filter is out of range, use empty buffer (yes, this can easily happen)
        if (ch->filterPos == 0 || ch->filterPos > 63)
            src8 = EmptyFilterSection;
        else
            src8 = (const int8_t *)&waves.squares[((int32_t)ch->filterPos - 32) * WAV_FILTER_LENGTH]; // squares@desired.filter

        uint8_t whichSquare = ch->squarePos << (5 - ch->Wavelength);
        if ((int8_t)whichSquare > 0x20)
        {
            whichSquare = 0x40 - whichSquare;
            ch->SquareReverse = true;
        }

        whichSquare--;
        if ((int8_t)whichSquare < 0)
            whichSquare = 0;

        src8 += whichSquare << 7; // *$80

        ahx->WaveformTab[2] = ch->SquareTempBuffer;

        const int32_t delta = (1 << 5) >> ch->Wavelength;
        const int32_t cycles = (1 << ch->Wavelength) << 2; // 8bb: <<2 since we do bytes not dwords, unlike AHX

        // And calc it, too!
        for (int32_t i = 0; i < cycles; i++)
        {
            ch->SquareTempBuffer[i] = *src8;
            src8 += delta;
        }

        ch->NewWaveform = true;
        ch->PlantSquare = false; // enough mod. for this frame
    }

    // Noise Treatin'
    if (ch->Waveform == 4-1)
        ch->NewWaveform = true;

    // Init the final audioPointer
    if (ch->NewWaveform)
    {
        const int8_t *audioSource = ahx->WaveformTab[ch->Waveform];

        // Waveform 3 (doesn't need filter add)..
        if (ch->Waveform != 3-1)
        {
            // 8bb: safety bug-fix... If filter is out of range, use empty buffer (yes, this can easily happen)
            if (ch->filterPos == 0 || ch->filterPos > 63)
                audioSource = EmptyFilterSection;
            else
                audioSource += ((int32_t)ch->filterPos - 32) * WAV_FILTER_LENGTH;
        }

        // Waveform 1 or 2
        if (ch->Waveform < 3-1)
            audioSource += waveOffsets[ch->Wavelength];

        // Waveform 4
        if (ch->Waveform == 4-1)
        {
            uint32_t seed = ahx->WNRandom;

            audioSource += seed & ((NOIZE_SIZE-0x280) - 1);

            seed += 2239384;
            ROR32(seed, 8); // 8bb: 32-bit right-bit-rotate by 8
            seed += 782323;
            seed ^= 0b1001011;
            seed -= 6735;
            ahx->WNRandom = seed;
        }

        ch->audioSource = audioSource;
    }

    // Init the final audioPeriod - always cal. not always write2audio!
    int16_t note = ch->InstrPeriod;
    if (!ch->FixedNote)
    {
        // not fixed, add other note-stuff!!
        note += ch->Transpose; // 8bb: -128 .. 127
        note += ch->TrackPeriod-1; // 8bb: results in -1 if no note
    }

    if (note > 5*12) // 8bb: signed comparison. Allows negative notes, read note below.
        note = 5*12;

    int16_t period;
    if (note < 0)
    {
        /* 8bb:
        ** Note can be negative (a common tradition in AHX is to not properly clamp),
        ** and this results in reading up to 129 words from before the period table.
        ** I added a table which has the correct byte-swapped underflow-words
        ** from AHX 2.3d-sp3 (68020 version).
        */

        if (note < -129) // 8bb: just in case my calculations were off
            note = -129;

        note += 129; // -1 .. -129 -> 0 .. 128
        period = beforePeriodTable_68020[note];
    }
    else
    {
        period = periodTable[note];
    }

    if (!ch->FixedNote)
        period += ch->periodSlidePeriod;

    // but nevertheless add PERFportamento/Vibrato!!!
    period += ch->periodPerfSlidePeriod;
    period += ch->VibratoPeriod;
    ch->audioPeriod = CLAMP(period, 113, 3424);

    // Init the final audioVolume
    uint16_t finalVol = ch->adsr >> 8;
    finalVol = (finalVol * ch->NoteMaxVolume) >> 6;
    finalVol = (finalVol * ch->perfSubVolume) >> 6;
    ch->audioVolume = (finalVol * ch->TrackMasterVolume) >> 6;
}

void SIDInterrupt(replayer_t *ahx)
{
    plyVoiceTemp_t *ch;

    if (!ahx->intPlaying)
        return;

    // set audioregisters... (8bb: yes, this is done here, NOT last like in WinAHX/ahx->cpp!)
    ch = ahx->pvt;
    for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
        SetAudio(ahx, i, ch);

    if (ahx->StepWaitFrames == 0)
    {
        if (ahx->GetNewPosition)
        {
            uint16_t posNext = ahx->PosNr + 1;
            if (posNext == ahx->song->LenNr)
                posNext = 0;

            // get Track AND Transpose (8bb: also for next position)
            uint8_t *posTable = &ahx->song->PosTable[ahx->PosNr << 3];
            uint8_t *posTableNext = &ahx->song->PosTable[posNext << 3];

            ch = ahx->pvt;
            for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
            {
                const int32_t offset = i << 1;
                ch->Track = posTable[offset+0];
                ch->Transpose = posTable[offset+1];
                ch->NextTrack = posTableNext[offset+0];
                ch->NextTranspose = posTableNext[offset+1];
            }

            ahx->GetNewPosition = false; // got new pos.
        }

        // - new pos or not, now treat STEPs (means 'em notes 'emself)
        ch = ahx->pvt;
        for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
            ProcessStep(ahx, ch);

        ahx->StepWaitFrames = ahx->Tempo;
    }

    ch = ahx->pvt;
    for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
        ProcessFrame(ahx, ch);

    ahx->StepWaitFrames--;
    if (ahx->StepWaitFrames == 0)
    {
        if (!ahx->PatternBreak)
        {
            ahx->NoteNr++;
            if (ahx->NoteNr == ahx->song->TrackLength)
            {
                // norm. next pos. does just position-jump!
                ahx->PosJump = ahx->PosNr + 1;
                ahx->PatternBreak = true;
            }
        }

        if (ahx->PatternBreak)
        {
            ahx->PatternBreak = false;

            ahx->NoteNr = ahx->PosJumpNote;
            ahx->PosJumpNote = 0;

            ahx->PosNr = ahx->PosJump;
            ahx->PosJump = 0;

            if (ahx->PosNr == ahx->song->LenNr)
            {
                ahx->PosNr = ahx->song->ResNr;

                // 8bb: added this (for WAV rendering)
                if (ahx->loopCounter >= ahx->loopTimes)
                    ahx->isRecordingToWAV = false;
                else
                    ahx->loopCounter++;
            }

            // 8bb: safety bug-fix..
            if (ahx->PosNr >= ahx->song->LenNr)
            {
                ahx->PosNr = 0;

                // 8bb: added this (for WAV rendering)
                if (ahx->loopCounter >= ahx->loopTimes)
                    ahx->isRecordingToWAV = false; // 8bb: stop WAV recording
                else
                    ahx->loopCounter++;
            }

            ahx->GetNewPosition = true;
        }
    }
}

/***************************************************************************
 *        PLAYER INTERFACING ROUTINES                                      *
 ***************************************************************************/

void ahxNextPattern(replayer_t *ahx)
{
    if (ahx->PosNr+1 < ahx->song->LenNr)
    {
        ahx->PosJump = ahx->PosNr + 1;
        ahx->PatternBreak = true;
        ahx->audio.tickSampleCounter64 = 0; // 8bb: clear tick sample counter so that it will instantly initiate a tick
    }
}

void ahxPrevPattern(replayer_t *ahx)
{
    if (ahx->PosNr > 0)
    {
        ahx->PosJump = ahx->PosNr - 1;
        ahx->PatternBreak = true;
        ahx->audio.tickSampleCounter64 = 0; // 8bb: clear tick sample counter so that it will instantly initiate a tick
    }
}

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
replayer_t* ahxInit(int32_t audioFreq, int32_t masterVol, int32_t stereoSeparation)
{
    replayer_t* ahx = calloc(1, sizeof(replayer_t));
    
    ahxErrCode = ERR_SUCCESS;
    ahxInitWaves();
    
    if (ahx == NULL)
    {
        ahxErrCode = ERR_OUT_OF_MEMORY;
        return NULL;
    }
    
    paulaInit(&ahx->audio, audioFreq);

    paulaSetStereoSeparation(&ahx->audio, stereoSeparation);
    paulaSetMasterVolume(&ahx->audio, masterVol);

    return ahx;
}

bool ahxLoadSong(replayer_t *ahx, song_t* pSong)
{
    if (!pSong)
    {
        ahxErrCode = ERR_NOT_AN_AHX;
        return false;
    }
    
    ahx->songLoaded = false;
    // 8bb: set up waveform pointers (Note: ret->WaveformTab[2] gets initialized in the replayer!)
    ahx->WaveformTab[0] = waves.triangle04;
    ahx->WaveformTab[1] = waves.sawtooth04;
    ahx->WaveformTab[3] = waves.whiteNoiseBig;
    ahx->song = pSong;
    ahx->songLoaded = true;
    return true;
}

void ahxUnloadSong(replayer_t *ahx)
{
    ahxStop(ahx);
    paulaStopAllDMAs(&ahx->audio); // 8bb: song can be free'd now
    ahx->song = NULL;
}

void ahxOutputSamples(replayer_t *ahx, int16_t *stream, int32_t numSamples)
{
    int16_t *streamOut = (int16_t *)stream;

    if (ahx->audio.pause)
    {
        memset(stream, 0, numSamples * 2 * sizeof (short));
        return;
    }

    int32_t samplesLeft = numSamples;
    while (samplesLeft > 0)
    {
        int32_t mixL[TEMPBUFSIZE] = {0};
        int32_t mixR[TEMPBUFSIZE] = {0};
        
        if (ahx->audio.tickSampleCounter64 <= 0) // new replayer tick
        {
            SIDInterrupt(ahx);
            ahx->audio.tickSampleCounter64 += ahx->audio.samplesPerTick64;
        }

        const int32_t remainingTick = (ahx->audio.tickSampleCounter64 + UINT32_MAX) >> 32; // ceil rounding (upwards)

        int32_t samplesToMix = samplesLeft;
        if (samplesToMix > remainingTick)
            samplesToMix = remainingTick;
        if (samplesToMix > TEMPBUFSIZE)
            samplesToMix = TEMPBUFSIZE;

        paulaMixSamples(&ahx->audio, mixL, mixR, samplesToMix);
        for (int32_t i = 0; i < samplesToMix; i++)
        {
            *streamOut++ = (int16_t)mixL[i];
            *streamOut++ = (int16_t)mixR[i];
        }

        samplesLeft -= samplesToMix;
        ahx->audio.tickSampleCounter64 -= (int64_t)samplesToMix << 32;
    }
}

void ahxClose(replayer_t *ahx)
{
    if (ahx) free(ahx);
}

bool ahxPlay(replayer_t *ahx, int32_t subSong)
{
    ahxErrCode = ERR_SUCCESS;

    if (!ahx->songLoaded)
    {
        ahxErrCode = ERR_SONG_NOT_LOADED;
        return false;
    }

    if (!isInitWaveforms)
    {
        ahxErrCode = ERR_NO_WAVES;
        return false; // 8bb: waves not set up!
    }
    
    ahx->Subsong = 0;
    ahx->PosNr = 0;
    if (subSong > 0 && ahx->song->Subsongs > 0)
    {
        subSong--;
        if (subSong >= ahx->song->Subsongs)
            subSong = ahx->song->Subsongs-1;

        ahx->Subsong = (uint8_t)(subSong + 1);
        ahx->PosNr = ahx->song->SubSongTable[subSong];
    }

    ahx->StepWaitFrames = 0;
    ahx->GetNewPosition = true;
    ahx->NoteNr = 0;

    ahxQuietAudios(ahx);

    for (int32_t i = 0; i < AMIGA_VOICES; i++)
        InitVoiceXTemp(&ahx->pvt[i]);

    SetUpAudioChannels(ahx);
    amigaSetCIAPeriod(&ahx->audio, tabler[ahx->song->SongCIAPeriodIndex]);

    // 8bb: Added this. Clear custom data (these are put in the waves struct for dword-alignment)
    memset(ahx->SquareTempBuffer,   0, sizeof (ahx->SquareTempBuffer));
    memset(ahx->currentVoice,       0, sizeof (ahx->currentVoice));

    plyVoiceTemp_t *ch = ahx->pvt;
    for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
        ch->SquareTempBuffer = ahx->SquareTempBuffer[i];

    ahx->PosJump = false;
    ahx->Tempo = 6;
    ahx->intPlaying = true;

    ahx->loopCounter = 0;
    ahx->loopTimes = 0; // 8bb: updated later in WAV writing mode

    ahx->audio.tickSampleCounter64 = 0; // 8bb: clear tick sample counter so that it will instantly initiate a tick

    resetCachedMixerPeriod(&ahx->audio);

    ahx->dBPM = amigaCIAPeriod2Hz(tabler[ahx->song->SongCIAPeriodIndex]) * 2.5;

    ahx->WNRandom = 0; // 8bb: Clear RNG seed (AHX doesn't do this)

    return true;
}

void ahxStop(replayer_t *ahx)
{
    ahx->intPlaying = false;
    ahxQuietAudios(ahx);

    for (int32_t i = 0; i < AMIGA_VOICES; i++)
        InitVoiceXTemp(&ahx->pvt[i]);
}

int32_t ahxGetErrorCode(void)
{
    return ahxErrCode;
}
