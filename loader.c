/*
** 8bb:
** AHX loader, from AHX 2.3d-sp3.
** NOTE: The loader used in AHX is actually different
** than the loader in the supplied replayer binaries.
** It does some extra stuff like fixing rev-0 modules.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "replayer.h"
#include "paula.h"

#define READ_BYTE(x, p) x = *p++
#define READ_WORD(x, p) x = *(uint16_t *)p; p += 2; x = SWAP16(x)

extern uint8_t ahxErrCode; // 8bb: replayer.c

// 8bb: AHX-header speed value (0..3) -> Amiga PAL CIA period
static const uint16_t tabler[4] = { 14209, 7104, 4736, 3552 };

static bool ahxInitModule(const uint8_t *p)
{
    song_t* ret;
	bool trkNullEmpty;
	uint16_t flags;

	ahx.songLoaded = false;

	// 8bb: added this check
	if (!isInitWaveforms)
	{
		ahxErrCode = ERR_NO_WAVES;
		return false;
	}

	ret = (song_t *)calloc(1, sizeof (song_t));
	if (ret == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	ret->Revision = p[3];

	if (memcmp("THX", p, 3) != 0 || ret->Revision > 1) // 8bb: added revision check
	{
		ahxErrCode = ERR_NOT_AN_AHX;
		return false;
	}

	p += 6;

	READ_WORD(flags, p);
	trkNullEmpty = !!(flags & 32768);
	ret->LenNr = flags & 0x3FF;
	READ_WORD(ret->ResNr, p);
	READ_BYTE(ret->TrackLength, p);
	READ_BYTE(ret->highestTrack, p); // max track nr. like 0
	READ_BYTE(ret->numInstruments, p); // max instr nr. 0/1-63
	READ_BYTE(ret->Subsongs, p);
	uint32_t numTracks = ret->highestTrack + 1;

	if (ret->ResNr >= ret->LenNr) // 8bb: safety bug-fix...
		ret->ResNr = 0;

	// 8bb: read sub-song table
	const int32_t subSongTableBytes = ret->Subsongs << 1;

	ret->SubSongTable = (uint16_t *)malloc(subSongTableBytes);
	if (ret->SubSongTable == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	const uint16_t *ptr16 = (uint16_t *)p;
	for (int32_t i = 0; i < ret->Subsongs; i++)
		ret->SubSongTable[i] = SWAP16(ptr16[i]);
	p += subSongTableBytes;


	// 8bb: read position table
	const int32_t posTableBytes = ret->LenNr << 3;

	ret->PosTable = (uint8_t *)malloc(posTableBytes);
	if (ret->PosTable == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	for (int32_t i = 0; i < posTableBytes; i++)
		ret->PosTable[i] = *p++;


	// 8bb: read track table
	ret->TrackTable = (uint8_t *)calloc(numTracks, 3*64);
	if (ret->TrackTable == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	int32_t tracksToRead = numTracks;
	uint8_t *dst8 = ret->TrackTable;
	if (trkNullEmpty)
	{
		dst8 += 3*64;
		tracksToRead--;
	}
	
	if (tracksToRead > 0)
	{
		const int32_t trackBytes = ret->TrackLength * 3;
		for (int32_t i = 0; i < tracksToRead; i++)
		{
			memcpy(&dst8[i * 3 * 64], p, trackBytes);
			p += trackBytes;
		}
	}

	// 8bb: read instruments
	for (int32_t i = 0; i < ret->numInstruments; i++)
	{
		instrument_t *ins = (instrument_t *)p;

		const int32_t instrBytes = 22 + (ins->perfLength << 2);

		// 8bb: calloc is needed here, to clear all non-written perfList bytes!
		ret->Instruments[i] = (instrument_t *)calloc(1, sizeof (instrument_t));
		if (ret->Instruments[i] == NULL)
		{
			ahxFree();
			ahxErrCode = ERR_OUT_OF_MEMORY;
			return false;
		}

		memcpy(ret->Instruments[i], p, instrBytes);
		p += instrBytes;
	}

	ret->Name[255] = '\0';
	for (int32_t i = 0; i < 255; i++)
	{
		ret->Name[i] = (char)p[i];
		if (ret->Name[i] == '\0')
			break;
	}

	// 8bb: remove filter commands on rev-0 songs, if present
	if (ret->Revision == 0)
	{
		uint8_t *ptr8;

		// 8bb: clear command 4 (override filter) parameter
		ptr8 = ret->TrackTable;
		for (int32_t i = 0; i <= ret->highestTrack; i++)
		{
			for (int32_t j = 0; j < ret->TrackLength; j++)
			{
				const uint8_t fx = ptr8[1] & 0x0F;
				if (fx == 4) // FX: OVERRIDE FILTER!
				{
					ptr8[1] &= 0xF0;
					ptr8[2] = 0; // override w/ zero!!
				}
				
				ptr8 += 3;
			}
		}

		// 8bb: clear command 0/4 parameter in instrument plists
		for (int32_t i = 0; i < ret->numInstruments; i++)
		{
			instrument_t *ins = ret->Instruments[i];
			if (ins == NULL)
				continue;

			ptr8 = ins->perfList;
			for (int32_t j = 0; j < ins->perfLength; j++)
			{
				const uint8_t fx1 = (ptr8[0] >> 2) & 7;
				if (fx1 == 0 || fx1 == 4)
					ptr8[2] = 0; // 8bb: clear fx1 parameter

				const uint8_t fx2 = (ptr8[0] >> 5) & 7;
				if (fx2 == 0 || fx2 == 4)
					ptr8[3] = 0; // 8bb: clear fx2 parameter

				ptr8 += 4;
			}
		}
	}

	ret->SongCIAPeriod = tabler[(flags >> 13) & 3];

	// 8bb: set up waveform pointers (Note: ret->WaveformTab[2] is setup in the replayer!)
	ahx.WaveformTab[0] = waves.triangle04;
	ahx.WaveformTab[1] = waves.sawtooth04;
	ahx.WaveformTab[3] = waves.whiteNoiseBig;
    
    song = ret;
	ahx.songLoaded = true;
	return true;
}

bool ahxLoadFromRAM(const uint8_t *data)
{
	ahxErrCode = ERR_SUCCESS;
	if (!ahxInitModule(data))
	{
		ahxFree();
		return false;
	}

	return true;
}

bool ahxLoad(const char *filename)
{
	ahxErrCode = ERR_SUCCESS;

	FILE *f = fopen(filename, "rb");
	if (f == NULL)
	{
		ahxErrCode = ERR_FILE_IO;
		return false;
	}

	fseek(f, 0, SEEK_END);
	const uint32_t filesize = (uint32_t)ftell(f);
	rewind(f);

	uint8_t *fileBuffer = (uint8_t *)malloc(filesize);
	if (fileBuffer == NULL)
	{
		fclose(f);
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	if (fread(fileBuffer, 1, filesize, f) != filesize)
	{
		free(fileBuffer);
		fclose(f);
		ahxErrCode = ERR_FILE_IO;
		return false;
	}

	fclose(f);

	if (!ahxLoadFromRAM((const uint8_t *)fileBuffer))
	{
		free(fileBuffer);
		return false;
	}

	free(fileBuffer);
	return true;
}

void ahxFree(void)
{
	ahxStop();
	paulaStopAllDMAs(); // 8bb: song can be free'd now
    
    if (song != NULL)
    {
        if (song->SubSongTable != NULL)
            free(song->SubSongTable);

        if (song->PosTable != NULL)
            free(song->PosTable);

        if (song->TrackTable != NULL)
            free(song->TrackTable);

        for (int32_t i = 0; i < song->numInstruments; i++)
        {
            if (song->Instruments[i] != NULL)
                free(song->Instruments[i]);
        }

        free(song);
        song = NULL;
    }
}
