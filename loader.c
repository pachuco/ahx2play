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

#define SWAP16(x) \
( \
	(((uint16_t)((x) & 0x00FF)) << 8) | \
	(((uint16_t)((x) & 0xFF00)) >> 8)   \
)

#define SWAP32(x) \
( \
	(((uint32_t)((x) & 0x000000FF)) << 24) | \
	(((uint32_t)((x) & 0x0000FF00)) <<  8) | \
	(((uint32_t)((x) & 0x00FF0000)) >>  8) | \
	(((uint32_t)((x) & 0xFF000000)) >> 24)   \
)

#define READ_BYTE(x, p)  {x = *(uint8_t  *)p; p += sizeof (uint8_t);                }
#define READ_WORD(x, p)  {x = *(uint16_t *)p; p += sizeof (uint16_t); x = SWAP16(x);}
#define READ_DWORD(x, p) {x = *(uint32_t *)p; p += sizeof (uint32_t); x = SWAP32(x);}

extern uint8_t ahxErrCode; // 8bb: replayer.c

song_t* ahxLoadFromRAM(const uint8_t *p)
{
    #define ERR(_E) {ahxErrCode = _E; goto l_fail;}
    song_t* ret = NULL;
	bool trkNullEmpty;
	uint16_t flags;

    ahxErrCode = ERR_SUCCESS;
    
	ret = (song_t *)calloc(1, sizeof (song_t));
	if (ret == NULL)
        ERR(ERR_OUT_OF_MEMORY);

	ret->Revision = p[3];

	if (memcmp("THX", p, 3) != 0 || ret->Revision > 1) // 8bb: added revision check
        ERR(ERR_NOT_AN_AHX);
    
	p += 6;

	READ_WORD(flags, p);
	trkNullEmpty = !!(flags & 32768);
	ret->LenNr = flags & 0x3FF;
	ret->SongCIAPeriodIndex = (flags >> 13) & 3; // 8bb: added this (BPM/tempo)
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
        ERR(ERR_OUT_OF_MEMORY);

	const uint16_t *ptr16 = (uint16_t *)p;
	for (int32_t i = 0; i < ret->Subsongs; i++)
		ret->SubSongTable[i] = SWAP16(ptr16[i]);
	p += subSongTableBytes;


	// 8bb: read position table
	const int32_t posTableBytes = ret->LenNr << 3;

	ret->PosTable = (uint8_t *)malloc(posTableBytes);
	if (ret->PosTable == NULL)
        ERR(ERR_OUT_OF_MEMORY);

	for (int32_t i = 0; i < posTableBytes; i++)
		ret->PosTable[i] = *p++;


	// 8bb: read track table
	ret->TrackTable = (uint8_t *)calloc(numTracks, 3*64);
	if (ret->TrackTable == NULL)
        ERR(ERR_OUT_OF_MEMORY);

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
            ERR(ERR_OUT_OF_MEMORY);

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

	// 8bb: remove filter commands on rev-0 songs, if present (AHX does this)
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
    
	return ret;
    
    #undef ERR
    l_fail:
        ahxFreeSong(ret);
        return NULL;
}

song_t* ahxLoadFromFile(const char *filename)
{
    #define ERR(_E) {ahxErrCode = _E; goto l_fail;}
    song_t* ret = NULL;
    uint8_t *fileBuffer = NULL;
    FILE *f = NULL;
    
	ahxErrCode = ERR_SUCCESS;

	f = fopen(filename, "rb");
	if (f == NULL)
        ERR(ERR_FILE_IO);
    
	fseek(f, 0, SEEK_END);
	const uint32_t filesize = (uint32_t)ftell(f);
	rewind(f);

	fileBuffer = (uint8_t *)malloc(filesize);
	if (fileBuffer == NULL)
        ERR(ERR_OUT_OF_MEMORY);

	if (fread(fileBuffer, 1, filesize, f) != filesize)
        ERR(ERR_FILE_IO);
    
	fclose(f);

    ret = ahxLoadFromRAM((const uint8_t *)fileBuffer);

	free(fileBuffer);
	return ret;
    
    #undef ERR
    l_fail:
        if (fileBuffer != NULL) free(fileBuffer);
        if (f != NULL) fclose(f);
        return NULL;
}

void ahxFreeSong(song_t* pSong)
{
    if (pSong != NULL)
    {
        if (pSong->SubSongTable != NULL)
            free(pSong->SubSongTable);

        if (pSong->PosTable != NULL)
            free(pSong->PosTable);

        if (pSong->TrackTable != NULL)
            free(pSong->TrackTable);

        for (int32_t i = 0; i < pSong->numInstruments; i++)
        {
            if (pSong->Instruments[i] != NULL)
                free(pSong->Instruments[i]);
        }

        free(pSong);
    }
}
