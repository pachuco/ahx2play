/*
** - Amiga 1200 Paula emulator w/ Amiga high-pass filter -
** Doesn't include the 34kHz low-pass filter, nor the optional "LED" filter.
**
** This code has been crafted for use with ahx2play.
** Usage outside of ahx2play may be unstable or give unwanted results.
*/

// for finding memory leaks in debug mode with Visual Studio 
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "paula.h" // AMIGA_VOICES
#include <math.h> // ceil()
#include "replayer.h" // SIDInterrupt(), AHX_LOWEST_CIA_PERIOD, AHX_DEFAULT_CIA_PERIOD
#include "audiodrivers/driver.h" // Read "audiodrivers/how_to_write_drivers.txt"

#define MAX_SAMPLE_LENGTH (0x280/2) /* in words. AHX buffer size */
#define NORM_FACTOR 1.5 /* can clip from high-pass filter overshoot */
#define STEREO_NORM_FACTOR 0.5 /* cumulative mid/side normalization factor (1/sqrt(2))*(1/sqrt(2)) */

static int8_t emptySample[MAX_SAMPLE_LENGTH*2];
static double *dMixBufferL, *dMixBufferR, dSideFactor, dPeriodToDeltaDiv, dMixNormalize;

// globalized
audio_t audio;
paulaVoice_t paula[AMIGA_VOICES];

// -----------------------------------------------
// -----------------------------------------------

void paulaSetMasterVolume(int32_t vol) // 0..256
{
	audio.masterVol = CLAMP(vol, 0, 256);

	// normalization w/ phase-inversion (A1200 has a phase-inverted audio signal)
	dMixNormalize = (NORM_FACTOR * (-INT16_MAX / (double)AMIGA_VOICES)) * (audio.masterVol / 256.0);
}

void resetCachedMixerPeriod(void)
{
	paulaVoice_t *v = paula;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		v->oldPeriod = -1;
		v->dOldVoiceDelta = 0.0;
	}
}

/* The following routines are only safe to call from the mixer thread,
** or from another thread if the DMAs are stopped first.
*/

void paulaSetPeriod(int32_t ch, uint16_t period)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realPeriod = period;
	if (realPeriod == 0)
		realPeriod = 1+65535; // confirmed behavior on real Amiga
	else if (realPeriod < 113)
		realPeriod = 113; // close to what happens on real Amiga (and needed for BLEP synthesis)

	// if the new period was the same as the previous period, use cached delta
	if (realPeriod != v->oldPeriod)
	{
		v->oldPeriod = realPeriod;

		// this period is not cached, calculate mixer deltas
		v->dOldVoiceDelta = dPeriodToDeltaDiv / realPeriod;
	}

	v->AUD_PER_delta = v->dOldVoiceDelta;
}

void paulaSetVolume(int32_t ch, uint16_t vol)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realVol = vol;

	realVol &= 127;
	if (realVol > 64)
		realVol = 64;

	// multiplying by this also scales the sample from -128 .. 127 -> -1.0 .. ~0.99
	v->AUD_VOL = realVol * (1.0 / (128.0 * 64.0));
}

void paulaSetLength(int32_t ch, uint16_t len)
{
	if (len == 0) // not what happens on a real Amiga, but this is fine for AHX
		len = 1;

	// since AHX has a fixed Paula buffer size, clamp it here
	if (len > MAX_SAMPLE_LENGTH)
		len = MAX_SAMPLE_LENGTH;
		
	paula[ch].AUD_LEN = len;
}

void paulaSetData(int32_t ch, const int8_t *src)
{
	if (src == NULL)
		src = emptySample;

	paula[ch].AUD_LC = src;
}

/* The following DMA functions are NOT to be
** used inside the audio thread!
** These are hard-written to be used the way AHX interfaces
** Paula (it initializes it outside of the replayer ticker).
*/

void paulaStopAllDMAs(void)
{
	lockMixer();

	paulaVoice_t *v = paula;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		v->DMA_active = false;
		v->location = v->AUD_LC = emptySample;
		v->lengthCounter = v->AUD_LEN = 1;
	}

	unlockMixer();
}

void paulaStartAllDMAs(void)
{
	paulaVoice_t *v;

	lockMixer();

	v = paula;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		if (v->AUD_LC == NULL)
			v->AUD_LC = emptySample;

		if (v->AUD_LEN == 0) // not what happens on a real Amiga, but this is fine for AHX
			v->AUD_LEN = 1;

		// since AHX has a fixed Paula buffer size, clamp it here
		if (v->AUD_LEN > MAX_SAMPLE_LENGTH)
			v->AUD_LEN = MAX_SAMPLE_LENGTH;

		/* This is not really accurate to what happens on Paula
		** during DMA start, but it's good enough.
		*/

		v->dDelta = v->AUD_PER_delta;
		v->location = v->AUD_LC;
		v->lengthCounter = v->AUD_LEN;

		// pre-fill AUDxDAT buffer
		v->AUD_DAT[0] = *v->location++;
		v->AUD_DAT[1] = *v->location++;
		v->sampleCounter = 2;

		// set current sample point
		v->dSample = v->AUD_DAT[0] * v->AUD_VOL; // -128 .. 127 -> -1.0 .. ~0.99

		// progress AUD_DAT buffer
		v->AUD_DAT[0] = v->AUD_DAT[1];
		v->sampleCounter--;

		v->dPhase = 0.0;

		v->DMA_active = true;
	}

	unlockMixer();
}

static void mixChannels(int32_t numSamples)
{
	double *dMixBufSelect[AMIGA_VOICES] = { dMixBufferL, dMixBufferR, dMixBufferR, dMixBufferL };

	paulaVoice_t *v = paula;

	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		if (!v->DMA_active)
			continue;

		double *dMixBuf = dMixBufSelect[i]; // what output channel to mix into (L, R, R, L)
		for (int32_t j = 0; j < numSamples; j++)
		{
			dMixBuf[j] += v->dSample;

			v->dPhase += v->dDelta;
			if (v->dPhase >= 1.0) // next sample point
			{
				v->dPhase -= 1.0; // we use single-step deltas (< 1.0), so this is safe

				v->dDelta = v->AUD_PER_delta; // Paula only updates period (delta) during sample fetching

				if (v->sampleCounter == 0)
				{
					// it's time to read new samples from DMA

					if (--v->lengthCounter == 0)
					{
						v->lengthCounter = v->AUD_LEN;
						v->location = v->AUD_LC;
					}

					// fill DMA data buffer
					v->AUD_DAT[0] = *v->location++;
					v->AUD_DAT[1] = *v->location++;
					v->sampleCounter = 2;
				}

				/* Pre-compute current sample point.
				** Output volume is only read from AUD_VOL at this stage,
				** and we don't emulate volume PWM anyway, so we can
				** pre-multiply by volume at this point.
				*/
				v->dSample = v->AUD_DAT[0] * v->AUD_VOL; // -128 .. 127 -> -1.0 .. ~0.99

				// progress AUD_DAT buffer
				v->AUD_DAT[0] = v->AUD_DAT[1];
				v->sampleCounter--;
			}
		}
	}
}

void paulaMixSamples(int16_t *target, int32_t numSamples)
{
	int32_t smp32;
	double dOut[2];

	mixChannels(numSamples);

	// apply filter, normalize, adjust stereo separation (if needed), dither and quantize
	
	if (audio.stereoSeparation == 100) // Amiga panning (no stereo separation)
	{
		for (int32_t i = 0; i < numSamples; i++)
		{
			dOut[0] = dMixBufferL[i];
			dOut[1] = dMixBufferR[i];

			// clear what we read
			dMixBufferL[i] = 0.0;
			dMixBufferR[i] = 0.0;

			double dL = dOut[0] * dMixNormalize;
			double dR = dOut[1] * dMixNormalize;

			// left channel
			smp32 = (int32_t)dL;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;

			// right channel
			smp32 = (int32_t)dR;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;
		}
	}
	else
	{
		for (int32_t i = 0; i < numSamples; i++)
		{
			dOut[0] = dMixBufferL[i];
			dOut[1] = dMixBufferR[i];

			dMixBufferL[i] = 0.0;
			dMixBufferR[i] = 0.0;

			double dL = dOut[0] * dMixNormalize;
			double dR = dOut[1] * dMixNormalize;

			// apply stereo separation
			const double dOldL = dL;
			const double dOldR = dR;
			double dMid  = (dOldL + dOldR) * STEREO_NORM_FACTOR;
			double dSide = (dOldL - dOldR) * dSideFactor;
			dL = dMid + dSide;
			dR = dMid - dSide;
			// -----------------------

			// left channel
			smp32 = (int32_t)dL;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;

			// right channel
			smp32 = (int32_t)dR;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;
		}
	}
}

void paulaTogglePause(void)
{
	audio.pause ^= 1;
}

void paulaOutputSamples(int16_t *stream, int32_t numSamples)
{
	int16_t *streamOut = (int16_t *)stream;

	if (audio.pause)
	{
		memset(stream, 0, numSamples * 2 * sizeof (short));
		return;
	}

	int32_t samplesLeft = numSamples;
	while (samplesLeft > 0)
	{
		if (audio.tickSampleCounter64 <= 0) // new replayer tick
		{
			SIDInterrupt(); // replayer.c
			audio.tickSampleCounter64 += audio.samplesPerTick64;
		}

		const int32_t remainingTick = (audio.tickSampleCounter64 + UINT32_MAX) >> 32; // ceil rounding (upwards)

		int32_t samplesToMix = samplesLeft;
		if (samplesToMix > remainingTick)
			samplesToMix = remainingTick;

		paulaMixSamples(streamOut, samplesToMix);
		streamOut += samplesToMix * 2;

		samplesLeft -= samplesToMix;
		audio.tickSampleCounter64 -= (int64_t)samplesToMix << 32;
	}
}

void paulaSetStereoSeparation(int32_t percentage) // 0..100 (percentage)
{
	audio.stereoSeparation = CLAMP(percentage, 0, 100);
	dSideFactor = (percentage / 100.0) * STEREO_NORM_FACTOR;
}

double amigaCIAPeriod2Hz(uint16_t period)
{
	if (period == 0)
		return 0.0;

	return (double)CIA_PAL_CLK / (period+1); // +1, CIA triggers on underflow
}

bool amigaSetCIAPeriod(uint16_t period) // replayer ticker
{
	const double dCIAHz = amigaCIAPeriod2Hz(period);
	if (dCIAHz == 0.0)
		return false;

	const double dSamplesPerTick = audio.outputFreq / dCIAHz;
	audio.samplesPerTick64 = (int64_t)(dSamplesPerTick * (UINT32_MAX+1.0)); // 32.32fp

	return true;
}

bool paulaInit(int32_t audioFrequency)
{
	const int32_t minFreq = (int32_t)(PAULA_PAL_CLK / 113.0)+1; // mixer requires single-step deltas
	audio.outputFreq = CLAMP(audioFrequency, minFreq, 384000);

	// set defaults
	paulaSetStereoSeparation(20);
	paulaSetMasterVolume(256);

	dPeriodToDeltaDiv = (double)PAULA_PAL_CLK / audio.outputFreq;

	int32_t maxSamplesToMix = (int32_t)ceil(audio.outputFreq / amigaCIAPeriod2Hz(AHX_HIGHEST_CIA_PERIOD));

	const int32_t bufferBytes = maxSamplesToMix * sizeof (double);

	dMixBufferL = (double *)calloc(1, bufferBytes);
	dMixBufferR = (double *)calloc(1, bufferBytes);

	if (dMixBufferL == NULL || dMixBufferR == NULL)
	{
		paulaClose();
		return false;
	}

	amigaSetCIAPeriod(AHX_DEFAULT_CIA_PERIOD);
	audio.tickSampleCounter64 = 0; // clear tick sample counter so that it will instantly initiate a tick

	resetCachedMixerPeriod();
	return true;
}

void paulaClose(void)
{
	if (dMixBufferL != NULL)
	{
		free(dMixBufferL);
		dMixBufferL = NULL;
	}

	if (dMixBufferR != NULL)
	{
		free(dMixBufferR);
		dMixBufferR = NULL;
	}
}
