// for finding memory leaks in debug mode with Visual Studio
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "ft2_header.h"
#include "ft2_audio.h"
#include "ft2_config.h"
#include "ft2_scopes.h"
#include "ft2_trim.h"
#include "ft2_inst_ed.h"
#include "ft2_sample_ed.h"
#include "ft2_wav_renderer.h"
#include "ft2_pattern_ed.h"
#include "ft2_gui.h"
#include "ft2_diskop.h"
#include "ft2_sample_loader.h"
#include "ft2_mouse.h"
#include "ft2_scopes.h"
#include "ft2_midi.h"
#include "ft2_events.h"
#include "ft2_video.h"

/* This is a *HUGE* mess!
** I hope you never have to modify it, and you probably shouldn't either.
** You will experience a lot of headaches if you dig into this file...
** If something looks to be wrong, it's probably right!
**
** The actual module load routines are ported from FT2 and slightly modified. */

enum
{
	FORMAT_NONE = 0,
	FORMAT_XM = 1,
	FORMAT_MOD = 2,
	FORMAT_S3M = 3,
	FORMAT_STM = 4
};

// DO NOT TOUCH THESE STRUCTS!

#ifdef _MSC_VER
#pragma pack(push)
#pragma pack(1)
#endif
typedef struct songSTMinstrHeaderTyp_t
{
	char name[12];
	uint8_t nul, insDisk;
	uint16_t reserved1, len, repS, repE;
	uint8_t vol, reserved2;
	uint16_t rate;
	int32_t reserved3;
	uint16_t paraLen;
}
#ifdef __GNUC__
__attribute__ ((packed))
#endif
songSTMinstrHeaderTyp;

typedef struct songSTMHeaderTyp_t
{
	char name[20], sig[8];
	uint8_t id1a, typ;
	uint8_t verMajor, verMinor;
	uint8_t tempo, ap, vol, reserved[13];
	songSTMinstrHeaderTyp instr[31];
	uint8_t songTab[128];
}
#ifdef __GNUC__
__attribute__ ((packed))
#endif
songSTMHeaderTyp;

typedef struct songS3MinstrHeaderTyp_t
{
	uint8_t typ;
	char dosName[12];
	uint8_t memSegH;
	uint16_t memSeg;
	int32_t len, repS, repE;
	uint8_t vol, dsk, pack, flags;
	int32_t c2Spd, res1;
	uint16_t gusPos;
	uint8_t res2[6];
	char name[28], id[4];
}
#ifdef __GNUC__
__attribute__ ((packed))
#endif
songS3MinstrHeaderTyp;

typedef struct songS3MHeaderTyp_t
{
	char name[28];
	uint8_t id1a, typ;
	uint16_t res1;
	int16_t songTabLen, antInstr, antPatt;
	uint16_t flags, trackerID, ver;
	char id[4];
	uint8_t globalVol, defSpeed, defTempo, masterVol, res2[12], chanType[32];
}
#ifdef __GNUC__
__attribute__ ((packed))
#endif
songS3MHeaderTyp;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

static volatile uint8_t loadedFormat;
static volatile bool stereoSamplesWarn, linearFreqTable, musicIsLoading, moduleLoaded, moduleFailedToLoad;
static uint8_t oldPlayMode, pattBuff[12288];
static const uint8_t stmEff[16] = { 0, 0, 11, 0, 10, 2, 1, 3, 4, 7, 0, 5 ,6, 0, 0, 0 };
static SDL_Thread *thread;

// these temporarily read to, then copied to real struct if load was OK (should not need to be volatile'd)
static int16_t pattLensTmp[MAX_PATTERNS];
static tonTyp *pattTmp[MAX_PATTERNS];
static instrTyp *instrTmp[1 + MAX_INST];
static songTyp songTmp;

static void setupLoadedModule(void);
static void freeTmpModule(void);
static bool loadInstrHeader(FILE *f, uint16_t i);
static void checkSampleRepeat(sampleTyp *s);
static bool loadInstrSample(FILE *f, uint16_t i);
void unpackPatt(uint8_t *dst, uint16_t inn, uint16_t len, uint8_t antChn);
static bool tmpPatternEmpty(uint16_t nr);
static bool loadPatterns(FILE *f, uint16_t antPtn);

// ft2_replayer.c
extern const char modSig[32][5];
extern const uint16_t amigaPeriod[12*8];

static bool allocateTmpInstr(int16_t nr)
{
	instrTyp *p;

	if (instrTmp[nr] != NULL)
		return false; // already allocated

	p = (instrTyp *)malloc(sizeof (instrTyp));
	if (p == NULL)
		return false;

	memset(p, 0, sizeof (instrTyp));

	for (int8_t i = 0; i < 16; i++) // set standard sample pan/vol
	{
		p->samp[i].pan = 128;
		p->samp[i].vol = 64;
	}

	instrTmp[nr] = p;
	return true;
}

static bool loadMusicMOD(FILE *f, uint32_t fileLength)
{
	char ID[16];
	bool modIsUST, modIsFEST, modIsNT;
	uint8_t bytes[4];
	int16_t i, j, k, ai;
	uint16_t a, b, period, ciaPeriod;
	tonTyp *ton;
	sampleTyp *s;
	songMOD31HeaderTyp h_MOD31;
	songMOD15HeaderTyp h_MOD15;

	// start loading MOD

	loadedFormat = FORMAT_MOD;

	rewind(f);
	fread(ID, 1, 16, f);
	fseek(f, 0x1D, SEEK_SET);
	fread(bytes, 1, 1, f);
	rewind(f);
	
	// since .mod is the last format tested, check if the file is an .it module (Impulse Tracker)
	if (!memcmp(ID, "IMPM", 4) && bytes[0] == 0)
	{
		okBoxThreadSafe(0, "System message", "Error: Impulse Tracker modules are not supported!");
		goto modLoadError;
	}

	// check if the file to load is a WAV, if so reject it
	if (!memcmp(ID, "RIFF", 4) && !memcmp(&ID[8], "WAVEfmt", 7))
	{
		okBoxThreadSafe(0, "System message", "Error: Can't load a .wav file as a module!");
		goto modLoadError;
	}

	if (fileLength < 1596 || fileLength > 20842494) // minimum and maximum possible size for an FT2 .mod
	{
		okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
		goto modLoadError;
	}

	if (fread(&h_MOD31, 1, sizeof (h_MOD31), f) != sizeof (h_MOD31))
	{
		okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
		goto modLoadError;
	}

	modIsFEST = false;
	modIsNT = false;
	modIsUST = false;

	if (!strncmp(h_MOD31.sig, "N.T.", 4))
	{
		j = 4;
		modIsNT = true;
	}
	else if (!strncmp(h_MOD31.sig, "FEST", 4) || !strncmp(h_MOD31.sig, "M&K!", 4))
	{
		modIsFEST = true;
		modIsNT = true;
		j = 4;
	}
	else if (!strncmp(h_MOD31.sig, "M!K!", 4) || !strncmp(h_MOD31.sig, "M.K.", 4) || !strncmp(h_MOD31.sig, "FLT4", 4))
	{
		j = 4;
	}
	else if (!strncmp(h_MOD31.sig, "OCTA", 4) || !strncmp(h_MOD31.sig, "FLT8", 4) || !strncmp(h_MOD31.sig, "CD81", 4))
	{
		j = 8;
	}
	else
	{
		j = 0;
		for (i = 0; i < 32; i++)
		{
			if (!strncmp(h_MOD31.sig, modSig[i], 4))
			{
				j = i + 1;
				break;
			}
			else if (j == 31)
			{
				j = -1; // ID not recignized
			}
		}
	}

	// unsupported MOD
	if (j == -1)
	{
		okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
		goto modLoadError;
	}

	if (j > 0)
	{
		modIsUST = false;
		if (fileLength < sizeof (h_MOD31))
		{
			okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
			goto modLoadError;
		}

		songTmp.antChn = (uint8_t)j;
		songTmp.len = h_MOD31.len;
		songTmp.repS = h_MOD31.repS;
		memcpy(songTmp.songTab, h_MOD31.songTab, 128);
		ai = 31;
	}
	else
	{
		modIsUST = true;
		if (fileLength < sizeof (h_MOD15))
		{
			okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
			goto modLoadError;
		}

		fseek(f, 0, SEEK_SET);
		if (fread(&h_MOD15, 1, sizeof (h_MOD15), f) != sizeof (h_MOD15))
		{
			okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
			goto modLoadError;
		}

		songTmp.antChn = 4;
		songTmp.len = h_MOD15.len;
		songTmp.repS = h_MOD15.repS;
		memcpy(songTmp.songTab, h_MOD15.songTab, 128);
		ai = 15;
	}

	if (songTmp.antChn == 0 || songTmp.len < 1)
	{
		okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
		goto modLoadError;
	}

	if (!strncmp(h_MOD31.sig, "M.K.", 4) && songTmp.len == 129)
		songTmp.len = 127; // fixes a specific copy of beatwave.mod by Sidewinder

	if (songTmp.len > 128 || (modIsUST && (songTmp.repS == 0 || songTmp.repS > 220)))
	{
		okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
		goto modLoadError;
	}

	// trim off spaces at end of name
	for (i = 19; i >= 0; i--)
	{
		if (h_MOD31.name[i] == ' ' || h_MOD31.name[i] == 0x1A)
			h_MOD31.name[i] = '\0';
		else
			break;
	}

	memcpy(songTmp.name, h_MOD31.name, 20);
	songTmp.name[20] = '\0';

	for (a = 0; a < ai; a++)
	{
		// trim off spaces at end of name
		for (i = 21; i >= 0; i--)
		{
			if (h_MOD31.instr[a].name[i] == ' ' || h_MOD31.instr[a].name[i] == 0x1A)
				h_MOD31.instr[a].name[i] = '\0';
			else
				break;
		}

		memcpy(songTmp.instrName[1+a], h_MOD31.instr[a].name, 22);
		songTmp.instrName[1+a][22] = '\0';
	}

	b = 0;
	for (a = 0; a < 128; a++)
	{
		if (songTmp.songTab[a] > b)
			b = songTmp.songTab[a];
	}

	if (songTmp.len < 255)
		memset(&songTmp.songTab[songTmp.len], 0, 256 - songTmp.len);

	for (a = 0; a <= b; a++)
	{
		pattTmp[a] = (tonTyp *)calloc((MAX_PATT_LEN * TRACK_WIDTH) + 16, 1);
		if (pattTmp[a] == NULL)
		{
			okBoxThreadSafe(0, "System message", "Not enough memory!");
			goto modLoadError;
		}

		pattLensTmp[a] = 64;
		for (j = 0; j < 64; j++)
		{
			for (k = 0; k < songTmp.antChn; k++)
			{
				ton = &pattTmp[a][(j * MAX_VOICES) + k];

				if (fread(bytes, 1, 4, f) != 4)
				{
					okBoxThreadSafe(0, "System message", "Error: This file is either not a module, or is not supported.");
					goto modLoadError;
				}

				// period to note
				period = ((bytes[0] & 0x0F) << 8) | bytes[1];
				for (i = 0; i < 8*12; i++)
				{
					if (period >= amigaPeriod[i])
					{
						ton->ton = (uint8_t)i + 1;
						break;
					}
				}

				ton->instr = (bytes[0] & 0xF0) | (bytes[2] >> 4);
				ton->effTyp = bytes[2] & 0x0F;
				ton->eff = bytes[3];

				if (ton->effTyp == 0xC)
				{
					if (ton->eff > 64)
						ton->eff = 64;
				}
				else if (ton->effTyp == 0x1)
				{
					if (ton->eff == 0)
						ton->effTyp = 0;
				}
				else if (ton->effTyp == 0x2)
				{
					if (ton->eff == 0)
						ton->effTyp = 0;
				}
				else if (ton->effTyp == 0x5)
				{
					if (ton->eff == 0)
						ton->effTyp = 0x3;
				}
				else if (ton->effTyp == 0x6)
				{
					if (ton->eff == 0)
						ton->effTyp = 0x4;
				}
				else if (ton->effTyp == 0xA)
				{
					if (ton->eff == 0)
						ton->effTyp = 0;
				}
				else if (ton->effTyp == 0xE)
				{
					// check if certain E commands are empty
					if (ton->eff == 0x10 || ton->eff == 0x20 || ton->eff == 0xA0 || ton->eff == 0xB0)
					{
						ton->effTyp = 0;
						ton->eff = 0;
					}
				}

				if (modIsUST)
				{
					if (ton->effTyp == 0x01)
					{
						// arpeggio
						ton->effTyp = 0x00;
					}
					else if (ton->effTyp == 0x02)
					{
						// pitch slide
						if (ton->eff & 0xF0)
						{
							ton->effTyp = 0x02;
							ton->eff >>= 4;
						}
						else if (ton->eff & 0x0F)
						{
							ton->effTyp = 0x01;
						}
					}

					// I don't remember why I did this...
					if (ton->effTyp == 0x0D && ton->eff > 0)
						ton->effTyp = 0x0A;
				}

				if (modIsNT && ton->effTyp == 0x0D)
					ton->eff = 0;
			}
		}

		if (tmpPatternEmpty(a))
		{
			if (pattTmp[a] != NULL)
			{
				free(pattTmp[a]);
				pattTmp[a] = NULL;
			}
		}
	}

	for (a = 0; a < ai; a++)
	{
		if (h_MOD31.instr[a].len == 0)
			continue;

		if (!allocateTmpInstr(1 + a))
		{
			okBoxThreadSafe(0, "System message", "Not enough memory!");
			goto modLoadError;
		}

		setNoEnvelope(instrTmp[1 + a]);

		s = &instrTmp[1+a]->samp[0];

		s->len = 2 * SWAP16(h_MOD31.instr[a].len);

		s->pek = (int8_t *)malloc(s->len + LOOP_FIX_LEN);
		if (s->pek == NULL)
		{
			okBoxThreadSafe(0, "System message", "Not enough memory!");
			goto modLoadError;
		}

		memcpy(s->name, songTmp.instrName[1+a], 22);

		if (modIsFEST)
			h_MOD31.instr[a].fine = (32 - (h_MOD31.instr[a].fine & 0x1F)) >> 1;

		if (!modIsUST)
			s->fine = 8 * ((2 * ((h_MOD31.instr[a].fine & 0x0F) ^ 8)) - 16);
		else
			s->fine = 0;

		s->pan = 128;

		s->vol = h_MOD31.instr[a].vol;
		if (s->vol > 64) s->vol = 64;

		s->repS = 2 * SWAP16(h_MOD31.instr[a].repS);

		if (modIsUST)
			s->repS /= 2;

		s->repL = 2 * SWAP16(h_MOD31.instr[a].repL);

		if (s->repL <= 2)
		{
			s->repS = 0;
			s->repL = 0;
		}

		if (s->repS+s->repL > s->len)
		{
			if (s->repS >= s->len)
			{
				s->repS = 0;
				s->repL = 0;
			}
			else
			{
				s->repL = s->len - s->repS;
			}
		}

		if (s->repL > 2)
			s->typ = 1;
		else
			s->typ = 0;

		if (modIsUST && (s->repS > 2 && s->repS < s->len))
		{
			s->len -= s->repS;
			fseek(f, s->repS, SEEK_CUR);
			s->repS = 0;
		}

		if (fread(s->pek, s->len, 1, f) == 1)
		{
			fixSample(s);
		}
		else
		{
			free(s->pek);
			s->pek = NULL;
			s->len = 0;
		}
	}

	songTmp.speed = 125;

	if (modIsUST)
	{
		// repS is initialBPM in UST MODs

		if (songTmp.repS != 120) // 120 is a special case and means 50Hz (125BPM)
		{
			if (songTmp.repS > 239)
				songTmp.repS = 239;

			// convert UST tempo to BPM
			const double dPALAmigaCiaClk = 709379.0;
			ciaPeriod = (240 - songTmp.repS) * 122;
			songTmp.speed = (uint16_t)round((dPALAmigaCiaClk / ciaPeriod) * (125.0 / 50.0));
		}

		songTmp.repS = 0;
	}
	else if (songTmp.repS >= songTmp.len)
	{
		songTmp.repS = 0;
	}

	fclose(f);

	songTmp.initialTempo = songTmp.tempo = 6;

	moduleLoaded = true;
	return true;

modLoadError:
	fclose(f);
	freeTmpModule();
	moduleFailedToLoad = true;
	return false;
}

static uint8_t stmTempoToBPM(uint8_t tempo) // ported from original ST2.3 replayer code
{
	const uint8_t slowdowns[16] = { 140, 50, 25, 15, 10, 7, 6, 4, 3, 3, 2, 2, 2, 2, 1, 1 };
	uint32_t bpm;
	uint16_t hz = 50;

	hz -= ((slowdowns[tempo >> 4] * (tempo & 15)) >> 4); // can and will underflow

	bpm = (uint32_t)round(hz * 2.5);
	return (uint8_t)CLAMP(bpm, 32, 255); // result can be slightly off, but close enough...
}

static bool loadMusicSTM(FILE *f, uint32_t fileLength)
{
	bool check3xx;
	uint8_t typ, tmp8, tempo;
	int16_t i, j, k, ai, ap, tmp;
	uint16_t a;
	int32_t len;
	tonTyp *ton;
	sampleTyp *s;
	songSTMHeaderTyp h_STM;

	rewind(f);

	// start loading STM

	if (fread(&h_STM, 1, sizeof (h_STM), f) != sizeof (h_STM))
		return loadMusicMOD(f, fileLength); // file is not a .stm, try to load as .mod

	if (memcmp(h_STM.sig, "!Scream!", 8) && memcmp(h_STM.sig, "BMOD2STM", 8) &&
		memcmp(h_STM.sig, "WUZAMOD!", 8) && memcmp(h_STM.sig, "SWavePro", 8))
	{
		return loadMusicMOD(f, fileLength); // file is not a .stm, try to load as .mod
	}

	loadedFormat = FORMAT_STM;

	if (h_STM.verMinor == 0 || h_STM.typ != 2)
	{
		okBoxThreadSafe(0, "System message", "Error loading .stm: Incompatible module!");
		goto stmLoadError;
	}

	songTmp.antChn = 4;
	memcpy(songTmp.songTab, h_STM.songTab, 128);

	i = 0;
	while (i < 128 && songTmp.songTab[i] < 99) i++;
	songTmp.len = i + (i == 0);

	if (songTmp.len < 255)
		memset(&songTmp.songTab[songTmp.len], 0, 256 - songTmp.len);

	// trim off spaces at end of name
	for (i = 19; i >= 0; i--)
	{
		if (h_STM.name[i] == ' ' || h_STM.name[i] == 0x1A)
			h_STM.name[i] = '\0';
		else
			break;
	}

	memcpy(songTmp.name, h_STM.name, 20);
	songTmp.name[20] = '\0';

	tempo = h_STM.tempo;
	if (h_STM.verMinor < 21)
		tempo = ((tempo / 10) << 4) + (tempo % 10);

	if (tempo == 0)
		tempo = 96;

	songTmp.initialTempo = songTmp.tempo = CLAMP(h_STM.tempo >> 4, 1, 31);
	songTmp.speed = stmTempoToBPM(tempo);

	if (h_STM.verMinor > 10)
		songTmp.globVol = MIN(h_STM.vol, 64);

	ap = h_STM.ap;
	for (i = 0; i < ap; i++)
	{
		pattTmp[i] = (tonTyp *)calloc((MAX_PATT_LEN * TRACK_WIDTH) + 16, 1);
		if (pattTmp[i] == NULL)
		{
			okBoxThreadSafe(0, "System message", "Not enough memory!");
			goto stmLoadError;
		}

		pattLensTmp[i] = 64;
		if (fread(pattBuff, 64 * 4 * 4, 1, f) != 1)
		{
			okBoxThreadSafe(0, "System message", "General I/O error during loading!");
			goto stmLoadError;
		}

		a = 0;
		for (j = 0; j < 64; j++)
		{
			for (k = 0; k < 4; k++)
			{
				ton = &pattTmp[i][(j * MAX_VOICES) + k];
				
				if (pattBuff[a] == 254)
				{
					ton->ton = 97;
				}
				else if (pattBuff[a] < 96)
				{
					ton->ton = (12 * (pattBuff[a] >> 4)) + (25 + (pattBuff[a] & 0x0F));
					if (ton->ton > 96)
						ton->ton = 0;
				}
				else
				{
					ton->ton = 0;
				}

				ton->instr = pattBuff[a + 1] >> 3;
				typ = (pattBuff[a + 1] & 7) + ((pattBuff[a + 2] & 0xF0) >> 1);
				if (typ <= 64)
					ton->vol = typ + 0x10;

				ton->eff = pattBuff[a + 3];

				tmp = pattBuff[a + 2] & 0x0F;
				if (tmp == 1)
				{
					ton->effTyp = 15;

					if (h_STM.verMinor < 21)
						ton->eff = ((ton->eff / 10) << 4) + (ton->eff % 10);
					
					ton->eff >>= 4;
				}
				else if (tmp == 3)
				{
					ton->effTyp = 13;
					ton->eff = 0;
				}
				else if (tmp == 2 || (tmp >= 4 && tmp <= 12))
				{
					ton->effTyp = stmEff[tmp];
					if (ton->effTyp == 0xA)
					{
						if (ton->eff & 0x0F)
							ton->eff &= 0x0F;
						else
							ton->eff &= 0xF0;
					}
				}
				else
				{
					ton->eff = 0;
				}

				/* Remove any EDx with no note.
				** SDx with no note in ST3 = does nothing
				** EDx with no note in FT2 = still retriggers */
				if (ton->effTyp == 0xE && (ton->eff & 0xF0) == 0xD0)
				{
					if (ton->ton == 0 || ton->ton == 97)
					{
						ton->eff = 0;
						ton->effTyp = 0;
					}
				}

				if (ton->effTyp > 35)
				{
					ton->effTyp = 0;
					ton->eff = 0;
				}

				a += 4;
			}
		}

		if (tmpPatternEmpty(i))
		{
			if (pattTmp[i] != NULL)
			{
				free(pattTmp[i]);
				pattTmp[i] = NULL;
			}
		}
	}

	ai = 31;
	for (i = 0; i < 31; i++)
	{
		// trim off spaces at end of name
		for (j = 11; j >= 0; j--)
		{
			if (h_STM.instr[i].name[j] == ' ' || h_STM.instr[i].name[j] == 0x1A)
				h_STM.instr[i].name[j] = '\0';
			else
				break;
		}

		memset(&songTmp.instrName[1+i], 0, sizeof (songTmp.instrName[1+i]));
		memcpy(&songTmp.instrName[1+i], h_STM.instr[i].name, 12);

		if (h_STM.instr[i].len != 0 && h_STM.instr[i].reserved1 != 0)
		{
			allocateTmpInstr(1 + i);
			setNoEnvelope(instrTmp[i]);

			s = &instrTmp[1+i]->samp[0];

			s->pek = (int8_t *)malloc(h_STM.instr[i].len + LOOP_FIX_LEN);
			if (s->pek == NULL)
			{
				okBoxThreadSafe(0, "System message", "Not enough memory!");
				goto stmLoadError;
			}

			s->len = h_STM.instr[i].len;
			tuneSample(s, h_STM.instr[i].rate);
			s->vol = h_STM.instr[i].vol;
			s->repS = h_STM.instr[i].repS;
			s->repL = h_STM.instr[i].repE - h_STM.instr[i].repS;
			s->pan = 128;

			if (s->repS < s->len && h_STM.instr[i].repE > s->repS && h_STM.instr[i].repE != 0xFFFF)
			{
				if (s->repS+s->repL > s->len)
					s->repL = s->len - s->repS;

				s->typ = 1;
			}
			else
			{
				s->repS = 0;
				s->repL = 0;
				s->typ  = 0;
			}

			if (s->vol > 64)
				s->vol = 64;

			if (fread(s->pek, s->len, 1, f) != 1)
			{
				okBoxThreadSafe(0, "System message", "General I/O error during loading! Possibly corrupt module?");
				goto stmLoadError;
			}

			fixSample(s);
		}
	}

	// non-FT2: fix overflown 9xx and illegal 3xx

	for (i = 0; i < ap; i++)
	{
		if (pattTmp[i] == NULL)
			continue;

		for (k = 0; k < songTmp.antChn; k++)
		{
			check3xx = false;
			for (j = 0; j < 64; j++)
			{
				ton = &pattTmp[i][(j * MAX_VOICES) + k];

				if (ton->ton > 0 && ton->ton < 97 && ton->effTyp != 0x3)
					check3xx = true;

				if (ton->ton > 0 && ton->ton < 97 && ton->effTyp == 0x3)
					check3xx = false;

				if (check3xx && ton->effTyp == 0x3)
				{
					if (ton->ton == 0 || ton->ton == 97)
					{
						ton->effTyp = 0;
						ton->eff = 0;
					}
				}

				if (ton->effTyp == 0x9 && ton->eff > 0)
				{
					if (ton->instr != 0 && ton->instr <= ai)
					{
						s = &instrTmp[ton->instr]->samp[0];
						len = s->len;

						tmp8 = 0;
						if (len > 0)
						{
							tmp8 = ton->eff;
							if (tmp8 >= len/256)
							{
								if (len/256 < 1)
									tmp8 = 0;
								else
									tmp8 = (uint8_t)((len/256) - 1);
							}
						}

						if (tmp8 > 0)
						{
							ton->eff = tmp8;
						}
						else
						{
							ton->effTyp = 0;
							ton->eff = 0;
						}
					}
					else
					{
						ton->effTyp = 0;
						ton->eff = 0;
					}
				}
			}
		}
	}

	fclose(f);

	moduleLoaded = true;
	return true;

stmLoadError:
	fclose(f);
	freeTmpModule();
	moduleFailedToLoad = true;
	return false;
}

static int8_t countS3MChannels(uint16_t antPtn)
{
	uint8_t j, k, channels;
	int16_t i;
	tonTyp ton;

	channels = 0;
	for (i = 0; i < antPtn; i++)
	{
		if (pattTmp[i] == NULL)
			continue;

		for (j = 0; j < 64; j++)
		{
			for (k = 0; k < MAX_VOICES; k++)
			{
				ton = pattTmp[i][(j * MAX_VOICES) + k];
				if (ton.eff == 0 && ton.effTyp == 0 && ton.instr == 0 && ton.ton == 0 && ton.vol == 0)
					continue;

				if (k > channels)
					channels = k;
			}
		}
	}
	channels++;

	return channels;
}

static bool loadMusicS3M(FILE *f, uint32_t dataLength)
{
	int8_t *tmpSmp;
	bool check3xx, illegalUxx;
	uint8_t ha[2048];
	uint8_t s3mLastDEff[32], s3mLastEEff[32], s3mLastFEff[32];
	uint8_t s3mLastSEff[32], s3mLastJEff[32], s3mLastGInstr[32], tmp8, typ;
	int16_t i, j, k, ai, ap, ver, ii, kk, tmp;
	uint16_t ptnOfs[256];
	int32_t len;
	tonTyp ton, *pattTon;
	sampleTyp *s;
	songS3MHeaderTyp h_S3M;
	songS3MinstrHeaderTyp h_S3MInstr;

	stereoSamplesWarn = false;

	rewind(f);

	// start loading S3M

	if (fread(&h_S3M, 1, sizeof (h_S3M), f) != sizeof (h_S3M))
		return loadMusicSTM(f, dataLength); // not a .s3m, try loading as .stm

	if (memcmp(h_S3M.id, "SCRM", 4))
		return loadMusicSTM(f, dataLength); // not a .s3m, try loading as .stm

	loadedFormat = FORMAT_S3M;

	if (h_S3M.antInstr > MAX_INST || h_S3M.songTabLen > 256 || h_S3M.antPatt > 256 ||
		h_S3M.typ != 16 || h_S3M.ver < 1 || h_S3M.ver > 2)
	{
		okBoxThreadSafe(0, "System message", "Error loading .s3m: Incompatible module!");
		goto s3mLoadError;
	}

	memset(songTmp.songTab, 255, sizeof (songTmp.songTab));
	if (fread(songTmp.songTab, h_S3M.songTabLen, 1, f) != 1)
	{
		okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
		goto s3mLoadError;
	}

	// count real song table entries
	songTmp.len = 256;
	while (songTmp.len > 0 && songTmp.songTab[songTmp.len-1] == 255)
		songTmp.len--;

	if (songTmp.len == 256)
		songTmp.len = 255;

	// remove pattern separators (254)
	k = 0;
	j = 0;
	for (i = 0; i < songTmp.len; i++)
	{
		if (songTmp.songTab[i] != 254)
			songTmp.songTab[j++] = songTmp.songTab[i];
		else
			k++;
	}

	if (k <= songTmp.len)
		songTmp.len -= k;
	else
		songTmp.len = 0;
	
	// clear unused song table entries
	if (songTmp.len < 255)
		memset(&songTmp.songTab[songTmp.len], 0, 256 - songTmp.len);

	songTmp.speed = h_S3M.defTempo;
	if (songTmp.speed < 32)
		songTmp.speed = 32;

	songTmp.tempo = h_S3M.defSpeed;
	if (songTmp.tempo == 0)
		songTmp.tempo = 6;

	if (songTmp.tempo > 31)
		songTmp.tempo = 31;

	songTmp.initialTempo = songTmp.tempo;

	// trim off spaces at end of name
	for (i = 19; i >= 0; i--)
	{
		if (h_S3M.name[i] == ' ' || h_S3M.name[i] == 0x1A)
			h_S3M.name[i] = '\0';
		else
			break;
	}

	memcpy(songTmp.name, h_S3M.name, 20);
	songTmp.name[20] = '\0';

	ap  = h_S3M.antPatt;
	ai  = h_S3M.antInstr;
	ver = h_S3M.ver;

	k = 31;
	while (k >= 0 && h_S3M.chanType[k] >= 16) k--;
	songTmp.antChn = (k + 2) & 254;

	if (fread(ha, ai + ai, 1, f) != 1)
	{
		okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
		goto s3mLoadError;
	}

	if (fread(ptnOfs, ap + ap, 1, f) != 1)
	{
		okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
		goto s3mLoadError;
	}

	// *** PATTERNS ***

	illegalUxx = false;

	k = 0;
	for (i = 0; i < ap; i++)
	{
		if (ptnOfs[i]  == 0)
			continue; // empty pattern

		memset(s3mLastDEff,   0, sizeof (s3mLastDEff));
		memset(s3mLastEEff,   0, sizeof (s3mLastEEff));
		memset(s3mLastFEff,   0, sizeof (s3mLastFEff));
		memset(s3mLastSEff,   0, sizeof (s3mLastSEff));
		memset(s3mLastJEff,   0, sizeof (s3mLastJEff));
		memset(s3mLastGInstr, 0, sizeof (s3mLastGInstr));

		fseek(f, ptnOfs[i] << 4, SEEK_SET);
		if (feof(f))
			continue;

		if (fread(&j, 2, 1, f) != 1)
		{
			okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
			goto s3mLoadError;
		}

		if (j > 0 && j <= 12288)
		{
			pattTmp[i] = (tonTyp *)calloc((MAX_PATT_LEN * TRACK_WIDTH) + 16, 1);
			if (pattTmp[i] == NULL)
			{
				okBoxThreadSafe(0, "System message", "Not enough memory!");
				goto s3mLoadError;
			}

			pattLensTmp[i] = 64;
			if (fread(pattBuff, j, 1, f) != 1)
			{
				okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
				goto s3mLoadError;
			}

			k = 0;
			kk = 0;

			while (k < j && kk < 64)
			{
				typ = pattBuff[k++];

				if (typ == 0)
				{
					kk++;
				}
				else
				{
					ii = typ & 31;

					memset(&ton, 0, sizeof (ton));

					// note and sample
					if (typ & 32)
					{
						ton.ton   = pattBuff[k++];
						ton.instr = pattBuff[k++];

						if (ton.instr > MAX_INST)
							ton.instr = 0;

						     if (ton.ton == 254) ton.ton = 97;
						else if (ton.ton == 255) ton.ton = 0;
						else
						{
							ton.ton = 1 + (ton.ton & 0xF) + (ton.ton >> 4) * 12;
							if (ton.ton > 96)
								ton.ton = 0;
						}
					}

					// volume
					if (typ & 64)
					{
						ton.vol = pattBuff[k++];

						if (ton.vol <= 64)
							ton.vol += 0x10;
						else
							ton.vol = 0;
					}

					// effect
					if (typ & 128)
					{
						ton.effTyp = pattBuff[k++];
						ton.eff = pattBuff[k++];

						if (ton.eff == 0)
						{
							if (ton.effTyp == 4)
							{
								if ((s3mLastDEff[ii] & 0xF0) == 0xF0 || (s3mLastDEff[ii] & 0x0F) == 0x0F)
									ton.eff = s3mLastDEff[ii];
							}
							else if (ton.effTyp ==  5) ton.eff = s3mLastEEff[ii];
							else if (ton.effTyp ==  6) ton.eff = s3mLastFEff[ii];
							else if (ton.effTyp == 10) ton.eff = s3mLastJEff[ii];
							else if (ton.effTyp == 19) ton.eff = s3mLastSEff[ii];
						}
						
						if (ton.eff != 0)
						{
							     if (ton.effTyp ==  4) s3mLastDEff[ii] = ton.eff;
							else if (ton.effTyp ==  5) s3mLastEEff[ii] = ton.eff;
							else if (ton.effTyp ==  6) s3mLastFEff[ii] = ton.eff;
							else if (ton.effTyp == 10) s3mLastJEff[ii] = ton.eff;
							else if (ton.effTyp == 19) s3mLastSEff[ii] = ton.eff;
						}

						switch (ton.effTyp)
						{
							case 1: // A
							{
								ton.effTyp = 0xF;
								if (ton.eff == 0 || ton.eff > 0x1F)
								{
									ton.effTyp = 0;
									ton.eff = 0;
								}
							}
							break;

							case 2: ton.effTyp = 0xB; break; // B
							case 3: ton.effTyp = 0xD; break; // C
							case 4: // D
							{
								     if ((ton.eff & 0xF0) == 0) ton.effTyp = 0xA;
								else if ((ton.eff & 0x0F) == 0) ton.effTyp = 0xA;
								else if ((ton.eff & 0xF0) == 0xF0)
								{
									ton.effTyp = 0xE;
									ton.eff = 0xB0 | (ton.eff & 15);
								}
								else if ((ton.eff & 0x0F) == 0x0F)
								{
									ton.effTyp = 0xE;
									ton.eff = 0xA0 | (ton.eff >> 4);
								}
								else
								{
									ton.effTyp = 0xA;
									if (ton.eff & 0x0F)
										ton.eff &= 0x0F;
									else
										ton.eff &= 0xF0;
								}
							}
							break;

							case 5: // E
							case 6: // F
							{
								if ((ton.eff & 0xF0) >= 0xE0)
								{
									if ((ton.eff & 0xF0) == 0xE0)
										tmp = 0x21;
									else
										tmp = 0xE;

									ton.eff &= 0x0F;

									if (ton.effTyp == 0x05)
										ton.eff |= 0x20;
									else
										ton.eff |= 0x10;

									ton.effTyp = (uint8_t)tmp;
								}
								else
								{
									ton.effTyp = 7 - ton.effTyp;
								}
							}
							break;

							case 7: // G
							{
								// fix illegal slides (to new instruments)
								if (ton.instr != 0 && ton.instr != s3mLastGInstr[ii])
									ton.instr = s3mLastGInstr[ii];

								ton.effTyp = 0x03;
							}
							break;

							case 8:  ton.effTyp = 0x04; break; // H
							case 9:  ton.effTyp = 0x1D; break; // I
							case 10: ton.effTyp = 0x00; break; // J
							case 11: ton.effTyp = 0x06; break; // K
							case 12: ton.effTyp = 0x05; break; // L
							case 15: ton.effTyp = 0x09; break; // O
							case 17: ton.effTyp = 0x1B; break; // Q
							case 18: ton.effTyp = 0x07; break; // R

							case 19: // S
							{
								ton.effTyp = 0xE;
								tmp = ton.eff >> 4;
								ton.eff &= 0x0F;

								     if (tmp == 0x1) ton.eff |= 0x30;
								else if (tmp == 0x2) ton.eff |= 0x50;
								else if (tmp == 0x3) ton.eff |= 0x40;
								else if (tmp == 0x4) ton.eff |= 0x70;
								else if (tmp == 0x08)
								{
									ton.effTyp = 0x8;
									ton.eff <<= 4;
								}
								else if (tmp == 0xB) ton.eff |= 0x60;
								else if (tmp == 0xC) ton.eff |= 0xC0;
								else if (tmp == 0xD) ton.eff |= 0xD0;
								else if (tmp == 0xE) ton.eff |= 0xE0;
								else if (tmp == 0xF) ton.eff |= 0xF0;
								else
								{
									ton.effTyp = 0;
									ton.eff = 0;
								}
							}
							break;

							case 20: // T
							{
								ton.effTyp = 0x0F;
								if (ton.eff < 0x20)
								{
									ton.effTyp = 0;
									ton.eff = 0;
								}
							}
							break;

							case 21: // U
							{
								if ((ton.eff & 0x0F) != 0)
								{
									ton.eff = (ton.eff & 0xF0) | (((ton.eff & 15) + 1) / 4);
									if ((ton.eff & 0x0F) == 0)
									{
										illegalUxx = true;
										ton.effTyp = 0;
										ton.eff = 0;
									}
									else
									{
										illegalUxx = false;
										ton.effTyp = 0x04;
									}
								}
								else
								{
									if (!illegalUxx)
									{
										ton.effTyp = 0x04;
									}
									else
									{
										ton.effTyp = 0;
										ton.eff = 0;
									}
								}
							}
							break;

							case 22: ton.effTyp = 0x10; break; // V

							default:
							{
								ton.effTyp = 0;
								ton.eff = 0;
							}
							break;
						}
					}

					if (ton.instr != 0 && ton.effTyp != 0x3)
						s3mLastGInstr[ii] = ton.instr;

					/* Remove any EDx with no note.
					** SDx with no note in ST3 = does nothing
					** EDx with no note in FT2 = still retriggers */
					if (ton.effTyp == 0xE && (ton.eff & 0xF0) == 0xD0)
					{
						if (ton.ton == 0 || ton.ton == 97)
						{
							ton.effTyp = 0;
							ton.eff = 0;
						}
					}

					pattTmp[i][(kk * MAX_VOICES) + ii] = ton;
				}
			}

			if (tmpPatternEmpty(i))
			{
				if (pattTmp[i] != NULL)
				{
					free(pattTmp[i]);
					pattTmp[i] = NULL;
				}
			}
		}
	}

	// *** SAMPLES ***

	memcpy(ptnOfs, ha, 512);
	for (i = 0; i < ai; i++)
	{
		fseek(f, ptnOfs[i] << 4, SEEK_SET);

		if (fread(&h_S3MInstr, 1, sizeof (h_S3MInstr), f) != sizeof (h_S3MInstr))
		{
			okBoxThreadSafe(0, "System message", "Not enough memory!");
			goto s3mLoadError;
		}

		// trim off spaces at end of name
		for (j = 21; j >= 0; j--)
		{
			if (h_S3MInstr.name[j] == ' ' || h_S3MInstr.name[j] == 0x1A)
				h_S3MInstr.name[j] = '\0';
			else
				break;
		}

		memcpy(songTmp.instrName[1+i], h_S3MInstr.name, 22);
		songTmp.instrName[1+i][22] = '\0';

		if (h_S3MInstr.typ > 1)
		{
			okBoxThreadSafe(0, "System message", "Error loading .s3m: Incompatible module!");
			goto s3mLoadError;
		}
		else if (h_S3MInstr.typ == 1)
		{
			if ((h_S3MInstr.flags & (255-1-2-4)) != 0 || h_S3MInstr.pack != 0)
			{
				okBoxThreadSafe(0, "System message", "Error loading .s3m: Incompatible module!");
				goto s3mLoadError;
			}
			else if (h_S3MInstr.memSeg > 0 && h_S3MInstr.len > 0)
			{
				if (!allocateTmpInstr(1 + i))
				{
					okBoxThreadSafe(0, "System message", "Not enough memory!");
					goto s3mLoadError;
				}

				setNoEnvelope(instrTmp[1 + i]);
				s = &instrTmp[1+i]->samp[0];

				len = h_S3MInstr.len;

				if ((h_S3MInstr.flags & 2) != 0) // stereo
				{
					stereoSamplesWarn = false;
					len *= 2;
				}

				if ((h_S3MInstr.flags & 4) != 0) // 16-bit
					len *= 2;

				tmpSmp = (int8_t *)malloc(len + LOOP_FIX_LEN);
				if (tmpSmp == NULL)
				{
					okBoxThreadSafe(0, "System message", "Not enough memory!");
					goto s3mLoadError;
				}

				memcpy(s->name, h_S3MInstr.name, 21);

				tuneSample(s, h_S3MInstr.c2Spd);

				s->len  = h_S3MInstr.len;
				s->vol  = h_S3MInstr.vol;
				s->repS = h_S3MInstr.repS;
				s->repL = h_S3MInstr.repE - h_S3MInstr.repS;

				// non-FT2: fixes "miracle man.s3m"
				if ((h_S3MInstr.memSeg<<4)+s->len > (int32_t)dataLength)
					s->len = dataLength - (h_S3MInstr.memSeg << 4);

				if (s->repL <= 2 || s->repS+s->repL > s->len)
				{
					s->repS = 0;
					s->repL = 0;
				}

				s->typ = (h_S3MInstr.flags & 1) + ((h_S3MInstr.flags & 4) << 2);

				if (s->repL == 0)
					s->typ &= 16; // turn off loop, keep 16-bit flag only

				if (s->vol > 64)
					s->vol = 64;

				s->pan = 128;

				fseek(f, h_S3MInstr.memSeg << 4, SEEK_SET);

				// non-FT2: fixes "miracle man.s3m"
				if ((h_S3MInstr.memSeg<<4)+len > (int32_t)dataLength)
					len = dataLength - (h_S3MInstr.memSeg << 4);

				if (fread(tmpSmp, len, 1, f) != 1)
				{
					free(tmpSmp);
					okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
					goto s3mLoadError;
				}

				if (ver != 1)
				{
					if ((h_S3MInstr.flags & 4) != 0)
					{
						conv16BitSample(tmpSmp, len, h_S3MInstr.flags & 2);

						s->pek = (int8_t *)malloc((h_S3MInstr.len * 2) + LOOP_FIX_LEN);
						if (s->pek == NULL)
						{
							free(tmpSmp);
							okBoxThreadSafe(0, "System message", "Not enough memory!");
							goto s3mLoadError;
						}

						memcpy(s->pek, tmpSmp, h_S3MInstr.len * 2);

						s->len  *= 2;
						s->repS *= 2;
						s->repL *= 2;
					}
					else
					{
						conv8BitSample(tmpSmp, len, h_S3MInstr.flags & 2);

						s->pek = (int8_t *)malloc(h_S3MInstr.len + LOOP_FIX_LEN);
						if (s->pek == NULL)
						{
							free(tmpSmp);
							okBoxThreadSafe(0, "System message", "Not enough memory!");
							goto s3mLoadError;
						}

						memcpy(s->pek, tmpSmp, h_S3MInstr.len);
					}

					fixSample(s);
				}

				free(tmpSmp);
			}
		}
	}

	if (stereoSamplesWarn)
		okBoxThreadSafe(0, "System message", "Stereo samples were found and will be converted to mono.");

	// non-FT2: fix overflown 9xx and illegal 3xx slides

	for (i = 0; i < ap; i++)
	{
		if (pattTmp[i] == NULL)
			continue;

		for (k = 0; k < songTmp.antChn; k++)
		{
			check3xx = false;
			for (j = 0; j < 64; j++)
			{
				pattTon = &pattTmp[i][(j * MAX_VOICES) + k];

				if (pattTon->ton > 0 && pattTon->ton < 97)
					check3xx = pattTon->effTyp != 0x3;

				if (check3xx && pattTon->effTyp == 0x3)
				{
					if (pattTon->ton == 0 || pattTon->ton == 97)
					{
						pattTon->effTyp = 0;
						pattTon->eff = 0;
					}
				}

				if (pattTon->effTyp == 0x9 && pattTon->eff > 0)
				{
					if (pattTon->instr != 0 && pattTon->instr <= ai)
					{
						s = &instrTmp[pattTon->instr]->samp[0];

						len = s->len;

						tmp8 = 0;
						if (len > 0)
						{
							tmp8 = pattTon->eff;
							if (tmp8 >= len/256)
							{
								if (len/256 < 1)
									tmp8 = 0;
								else
									tmp8 = (uint8_t)((len/256) - 1);
							}
						}

						if (tmp8 > 0)
						{
							pattTon->eff = tmp8;
						}
						else
						{
							pattTon->effTyp = 0;
							pattTon->eff = 0;
						}
					}
					else
					{
						pattTon->effTyp = 0;
						pattTon->eff = 0;
					}
				}
			}
		}
	}

	fclose(f);

	songTmp.antChn = countS3MChannels(ap);

	if (!(config.dontShowAgainFlags & DONT_SHOW_S3M_LOAD_WARNING_FLAG))
		okBoxThreadSafe(6, "System message", "Warning: S3M channel panning is not compatible with FT2!");

	moduleLoaded = true;
	return true;

s3mLoadError:
	fclose(f);
	freeTmpModule();
	moduleFailedToLoad = true;
	return false;
}

static int32_t SDLCALL loadMusicThread(void *ptr)
{
	char tmpText[128];
	int16_t k;
	uint16_t i;
	uint32_t filelength;
	songHeaderTyp h;
	FILE *f;

	(void)ptr;

	stereoSamplesWarn = false;
	linearFreqTable = false;

	if (editor.tmpFilenameU == NULL)
	{
		okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
		moduleFailedToLoad = true;
		return false;
	}

	f = UNICHAR_FOPEN(editor.tmpFilenameU, "rb");
	if (f == NULL)
	{
		okBoxThreadSafe(0, "System message", "General I/O error during loading! Is the file in use?");
		moduleFailedToLoad = true;
		return false;
	}

	fseek(f, 0, SEEK_END);
	filelength = ftell(f);
	rewind(f);

	// start loading
	if (fread(&h, 1, sizeof (h), f) != sizeof (h))
		return loadMusicS3M(f, filelength); // not a .xm file, try to load as .s3m

	if (memcmp(h.sig, "Extended Module: ", 17))
		return loadMusicS3M(f, filelength); // not a .xm file, try to load as .s3m

	loadedFormat = FORMAT_XM;

	if (h.ver < 0x0102 || h.ver > 0x0104)
	{
		fclose(f);

		sprintf(tmpText, "Error loading .xm: Unsupported XM version (v%1d.%1d%1d)",
			'0' + (((h.ver >> 8) & 0x0F) % 10), '0' + (((h.ver >> 4) & 0x0F)) % 10, '0' + ((h.ver & 0x0F)) % 10);
		okBoxThreadSafe(0, "System message", tmpText);

		moduleFailedToLoad = true;
		return false;
	}

	if (h.len > MAX_ORDERS)
	{
		okBoxThreadSafe(0, "System message", "Error loading .xm: The song has more than 256 orders!");
		goto xmLoadError;
	}

	if (h.antPtn > MAX_PATTERNS)
	{
		okBoxThreadSafe(0, "System message", "Error loading .xm: The song has more than 256 patterns!");
		goto xmLoadError;
	}

	if (h.antChn == 0 || h.antChn > MAX_VOICES)
	{
		okBoxThreadSafe(0, "System message", "Error loading .xm: Incompatible amount of channels!");
		goto xmLoadError;
	}

	if (h.antInstrs > MAX_INST)
		okBoxThreadSafe(0, "System message", "This module has over 128 instruments! Only the first 128 will be loaded.");

	fseek(f, 60 + h.headerSize, SEEK_SET);
	if (filelength != 336 && feof(f)) // 336 in length at this point = empty XM
	{
		okBoxThreadSafe(0, "System message", "Error loading .xm: The module is empty!");
		goto xmLoadError;
	}

	// trim off spaces at end of name
	for (k = 19; k >= 0; k--)
	{
		if (h.name[k] == ' ' || h.name[k] == 0x1A)
			h.name[k] = '\0';
		else
			break;
	}

	memcpy(songTmp.name, h.name, 20);
	songTmp.name[20] = '\0';

	songTmp.len = h.len;
	songTmp.repS = h.repS;
	songTmp.antChn = (uint8_t)h.antChn;
	songTmp.speed = h.defSpeed ? h.defSpeed : 125;
	songTmp.tempo = h.defTempo ? h.defTempo : 6;
	songTmp.ver = h.ver;
	linearFreqTable = h.flags & 1;

	songTmp.speed = CLAMP(songTmp.speed, 32, 255);
	if (songTmp.tempo > 31)
		songTmp.tempo = 31;

	songTmp.initialTempo = songTmp.tempo;

	if (songTmp.globVol > 64)
		songTmp.globVol = 64;

	if (songTmp.len == 0)
		songTmp.len = 1; // songTmp.songTab is already empty
	else
		memcpy(songTmp.songTab, h.songTab, songTmp.len);

	if (songTmp.ver < 0x0104)
	{
		// old FT2 format

		for (i = 1; i <= h.antInstrs; i++)
		{
			if (!loadInstrHeader(f, i))
			{
				okBoxThreadSafe(0, "System message", "Error loading .xm: Either a corrupt or a non-supported module!");
				goto xmLoadError;
			}
		}

		if (!loadPatterns(f, h.antPtn))
		{
			// error message is shown inside loadPattern()
			goto xmLoadError;
		}

		for (i = 1; i <= h.antInstrs; i++)
		{
			if (!loadInstrSample(f, i))
			{
				okBoxThreadSafe(0, "System message", "Not enough memory!");
				goto xmLoadError;
			}
		}
	}
	else
	{
		// current FT2 format

		if (!loadPatterns(f, h.antPtn))
		{
			// error message is shown inside loadPattern()
			goto xmLoadError;
		}

		for (i = 1; i <= h.antInstrs; i++)
		{
			if (!loadInstrHeader(f, i))
			{
				okBoxThreadSafe(0, "System message", "Error loading .xm: Either a corrupt or a non-supported module!");
				goto xmLoadError;
			}

			if (!loadInstrSample(f, i))
			{
				okBoxThreadSafe(0, "System message", "Not enough memory!");
				goto xmLoadError;
			}
		}
	}

	if (stereoSamplesWarn)
		okBoxThreadSafe(0, "System message", "Stereo samples were found and will be converted to mono.");

	fclose(f);

	moduleLoaded = true;
	return true;

xmLoadError:
	fclose(f);
	freeTmpModule();
	moduleFailedToLoad = true;
	return false;
}

void loadMusic(UNICHAR *filenameU)
{
	int32_t i;

	if (musicIsLoading)
		return;

	mouseAnimOn();

	musicIsLoading = true;
	moduleLoaded = false;
	moduleFailedToLoad = false;
	loadedFormat = FORMAT_NONE;

	UNICHAR_STRCPY(editor.tmpFilenameU, filenameU);

	// clear deprecated pointers from possible last loading session (super important)
	memset(pattTmp,  0, sizeof (pattTmp));
	memset(instrTmp, 0, sizeof (instrTmp));

	// prevent stuck instrument names from previous module
	memset(&songTmp, 0, sizeof (songTmp));

	for (i = 0; i < MAX_PATTERNS; i++)
		pattLensTmp[i] = 64;

	thread = SDL_CreateThread(loadMusicThread, NULL, NULL);
	if (thread == NULL)
	{
		editor.loadMusicEvent = EVENT_NONE;
		okBox(0, "System message", "Couldn't create thread!");
		musicIsLoading = false;
		return;
	}

	SDL_DetachThread(thread);
}

static void freeTmpModule(void)
{
	uint16_t i;

	// free all patterns
	for (i = 0; i < MAX_PATTERNS; i++)
	{
		if (pattTmp[i] != NULL)
		{
			free(pattTmp[i]);
			pattTmp[i] = NULL;
		}
	}

	// free all samples
	for (i = 1; i <= MAX_INST; i++)
	{
		if (instrTmp[i] != NULL)
		{
			for (uint8_t j = 0; j < MAX_SMP_PER_INST; j++)
			{
				if (instrTmp[i]->samp[j].pek != NULL)
					free(instrTmp[i]->samp[j].pek);
			}

			free(instrTmp[i]);
			instrTmp[i] = NULL;
		}
	}
}

static bool loadInstrHeader(FILE *f, uint16_t i)
{
	int8_t k;
	uint8_t j;
	uint32_t readSize;
	instrHeaderTyp ih;
	sampleTyp *s;

	memset(&ih, 0, INSTR_HEADER_SIZE);

	fread(&ih.instrSize, 4, 1, f);

	readSize = ih.instrSize;
	if (readSize < 4 || readSize > INSTR_HEADER_SIZE)
		readSize = INSTR_HEADER_SIZE;

	// load instrument data into temp buffer
	fread(ih.name, readSize - 4, 1, f); // -4 = skip ih.instrSize

	// FT2 bugfix: skip instrument header data if instrSize is above INSTR_HEADER_SIZE
	if (ih.instrSize > INSTR_HEADER_SIZE)
		fseek(f, ih.instrSize - INSTR_HEADER_SIZE, SEEK_CUR);

	if (ih.antSamp > MAX_SMP_PER_INST)
		return false;

	if (i <= MAX_INST)
	{
		// trim off spaces at end of name
		for (k = 21; k >= 0; k--)
		{
			if (ih.name[k] == ' ' || ih.name[k] == 0x1A)
				ih.name[k] = '\0';
			else
				break;
		}

		memcpy(songTmp.instrName[i], ih.name, 22);
		songTmp.instrName[i][22] = '\0';
	}

	if (ih.antSamp > 0)
	{
		if (i <= MAX_INST)
		{
			if (!allocateTmpInstr(i))
				return false;

			// sanitize stuff for malicious instruments
			ih.midiProgram = CLAMP(ih.midiProgram, 0, 127);
			ih.midiBend = CLAMP(ih.midiBend, 0, 36);

			if (ih.midiChannel > 15) ih.midiChannel = 15;
			if (ih.mute != 1) ih.mute = 0;
			if (ih.midiOn != 1) ih.midiOn = 0;
			if (ih.vibDepth > 0x0F) ih.vibDepth = 0x0F;
			if (ih.vibRate > 0x3F) ih.vibRate = 0x3F;
			if (ih.vibTyp > 3) ih.vibTyp = 0;

			for (j = 0; j < 96; j++)
			{
				if (ih.ta[j] > 15)
					ih.ta[j] = 15;
			}
			// ----------------------------------------

			// copy over final instrument data from temp buffer
			memcpy(instrTmp[i], ih.ta, INSTR_SIZE);
			instrTmp[i]->antSamp = ih.antSamp;

			if (instrTmp[i]->envVPAnt > 12) instrTmp[i]->envVPAnt = 12;
			if (instrTmp[i]->envVRepS > 11) instrTmp[i]->envVRepS = 11;
			if (instrTmp[i]->envVRepE > 11) instrTmp[i]->envVRepE = 11;
			if (instrTmp[i]->envVSust > 11) instrTmp[i]->envVSust = 11;
			if (instrTmp[i]->envPPAnt > 12) instrTmp[i]->envPPAnt = 12;
			if (instrTmp[i]->envPRepS > 11) instrTmp[i]->envPRepS = 11;
			if (instrTmp[i]->envPRepE > 11) instrTmp[i]->envPRepE = 11;
			if (instrTmp[i]->envPSust > 11) instrTmp[i]->envPSust = 11;
		}

		if (fread(ih.samp, ih.antSamp * sizeof (sampleHeaderTyp), 1, f) != 1)
			return false;

		if (i <= MAX_INST)
		{
			for (j = 0; j < ih.antSamp; j++)
			{
				s = &instrTmp[i]->samp[j];
				memcpy(s, &ih.samp[j], 12+4+24);
				// s->pek is set up later

				// trim off spaces at end of name
				for (k = 21; k >= 0; k--)
				{
					if (s->name[k] == ' ' || s->name[k] == 0x1A)
						s->name[k] = '\0';
					else
						break;
				}

				// sanitize stuff for malicious modules
				if (s->vol > 64)
					s->vol = 64;

				s->relTon = CLAMP(s->relTon, -48, 71);
			}
		}
	}

	return true;
}

static void checkSampleRepeat(sampleTyp *s)
{
	if (s->repS < 0) s->repS = 0;
	if (s->repL < 0) s->repL = 0;
	if (s->repS > s->len) s->repS = s->len;
	if (s->repS+s->repL > s->len) s->repL = s->len - s->repS;
	if (s->repL == 0) s->typ &= ~3; // non-FT2 fix: force loop off if looplen is 0
}

static bool loadInstrSample(FILE *f, uint16_t i)
{
	int8_t *newPtr;
	uint16_t j, k;
	int32_t l, bytesToSkip;
	sampleTyp *s;

	if (i > MAX_INST || instrTmp[i] == NULL)
		return true; // yes, let's just pretend they got loaded

	k = instrTmp[i]->antSamp;
	for (j = 0; j < k; j++)
	{
		s = &instrTmp[i]->samp[j];

		// if a sample has both forward loop and pingpong loop set, make it pingpong loop only (FT2 behavior)
		if ((s->typ & 3) == 3)
			s->typ &= 0xFE;

		l = s->len;
		if (l <= 0)
		{
			s->pek = NULL;
			s->len = 0;
			s->repL = 0;
			s->repS = 0;

			if (s->typ & 32)
				s->typ &= ~32; // remove stereo flag
		}
		else
		{
			bytesToSkip = 0;
			if (l > MAX_SAMPLE_LEN)
			{
				bytesToSkip = l - MAX_SAMPLE_LEN;
				l = MAX_SAMPLE_LEN;
			}

			s->pek = (int8_t *)malloc(l + LOOP_FIX_LEN);
			if (s->pek == NULL)
				return false;

			if (fread(s->pek, l, 1, f) != 1)
				return false;

			if (bytesToSkip > 0)
				fseek(f, bytesToSkip, SEEK_CUR);

			delta2Samp(s->pek, l, s->typ);

			if (s->typ & 32) // stereo sample - already downmixed to mono in delta2samp()
			{
				s->typ &= ~32; // remove stereo flag

				s->len /= 2;
				s->repL /= 2;
				s->repS /= 2;

				newPtr = (int8_t *)realloc(s->pek, s->len + LOOP_FIX_LEN);
				if (newPtr != NULL)
					s->pek = newPtr;

				stereoSamplesWarn = true;
			}
		}

		// NON-FT2 FIX: Align to 2-byte if 16-bit sample
		if (s->typ & 16)
		{
			s->repL &= 0xFFFFFFFE;
			s->repS &= 0xFFFFFFFE;
			s->len &= 0xFFFFFFFE;
		}

		checkSampleRepeat(s);
		fixSample(s);
	}

	return true;
}

void unpackPatt(uint8_t *dst, uint16_t inn, uint16_t len, uint8_t antChn)
{
	uint8_t note, data, *src;
	int32_t srcEnd, srcIdx;

	if (dst == NULL)
		return;

	src = dst + inn;
	srcEnd = len * TRACK_WIDTH;
	srcIdx = 0;

	for (int32_t i = 0; i < len; i++)
	{
		for (int32_t j = 0; j < antChn; j++)
		{
			if (srcIdx >= srcEnd)
				return; // error!

			note = *src++;
			if (note & 0x80)
			{
				data = 0; if (note & 0x01) data = *src++; *dst++ = data;
				data = 0; if (note & 0x02) data = *src++; *dst++ = data;
				data = 0; if (note & 0x04) data = *src++; *dst++ = data;
				data = 0; if (note & 0x08) data = *src++; *dst++ = data;
				data = 0; if (note & 0x10) data = *src++; *dst++ = data;
			}
			else
			{
				*dst++ = note;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
			}

			// if note is overflowing (>97), remove it
			if (*(dst-5) > 97)
				*(dst-5) = 0;

			// non-FT2 security fix: if effect is above 35 (Z), clear effect and parameter
			if (*(dst-2) > 35)
			{
				*(dst-2) = 0;
				*(dst-1) = 0;
			}

			srcIdx += sizeof (tonTyp);
		}

		// skip unused channels
		dst += sizeof (tonTyp) * (MAX_VOICES - antChn);
	}
}

static bool tmpPatternEmpty(uint16_t nr)
{
	uint8_t *scanPtr;
	uint32_t scanLen;

	if (pattTmp[nr] == NULL)
		return true;

	scanPtr = (uint8_t *)pattTmp[nr];
	scanLen = pattLensTmp[nr] * TRACK_WIDTH;

	for (uint32_t i = 0; i < scanLen; i++)
	{
		if (scanPtr[i] != 0)
			return false;
	}

	return true;
}

void clearUnusedChannels(tonTyp *p, int16_t pattLen, uint8_t antChn)
{
	if (p == NULL || antChn >= MAX_VOICES)
		return;

	for (int32_t i = 0; i < pattLen; i++)
		memset(&p[(i * MAX_VOICES) + antChn], 0, sizeof (tonTyp) * (MAX_VOICES - antChn));
}

static bool loadPatterns(FILE *f, uint16_t antPtn)
{
	bool pattLenWarn;
	uint8_t tmpLen, *pattPtr;
	uint16_t i, a;
	patternHeaderTyp ph;

	pattLenWarn = false;

	for (i = 0; i < antPtn; i++)
	{
		if (fread(&ph.patternHeaderSize, 4, 1, f) != 1)
			goto pattCorrupt;

		if (fread(&ph.typ, 1, 1, f) != 1)
			goto pattCorrupt;

		ph.pattLen = 0;
		if (songTmp.ver == 0x0102)
		{
			if (fread(&tmpLen, 1, 1, f) != 1)
				goto pattCorrupt;

			if (fread(&ph.dataLen, 2, 1, f) != 1)
				goto pattCorrupt;

			ph.pattLen = tmpLen + 1; // +1 in v1.02

			if (ph.patternHeaderSize > 8)
				fseek(f, ph.patternHeaderSize - 8, SEEK_CUR);
		}
		else
		{
			if (fread(&ph.pattLen, 2, 1, f) != 1)
				goto pattCorrupt;

			if (fread(&ph.dataLen, 2, 1, f) != 1)
				goto pattCorrupt;

			if (ph.patternHeaderSize > 9)
				fseek(f, ph.patternHeaderSize - 9, SEEK_CUR);
		}

		if (feof(f))
			goto pattCorrupt;

		pattLensTmp[i] = ph.pattLen;

		if (ph.dataLen > 0)
		{
			pattTmp[i] = (tonTyp *)calloc((MAX_PATT_LEN * TRACK_WIDTH) + 16, 1);
			if (pattTmp[i] == NULL)
			{
				okBoxThreadSafe(0, "System message", "Not enough memory!");
				return false;
			}

			a = ph.pattLen * TRACK_WIDTH;

			pattPtr = (uint8_t *)pattTmp[i];
			memset(pattPtr, 0, a);

			if (fread(&pattPtr[a - ph.dataLen], 1, ph.dataLen, f) != ph.dataLen)
				goto pattCorrupt;

			unpackPatt(pattPtr, a - ph.dataLen, ph.pattLen, songTmp.antChn);
			clearUnusedChannels(pattTmp[i], pattLensTmp[i], songTmp.antChn);
		}

		if (tmpPatternEmpty(i))
		{
			if (pattTmp[i] != NULL)
			{
				free(pattTmp[i]);
				pattTmp[i] = NULL;
			}

			pattLensTmp[i] = 64;
		}

		if (pattLensTmp[i] > 256)
		{
			pattLensTmp[i] = 64;
			pattLenWarn = true;
		}
	}

	if (pattLenWarn)
		okBoxThreadSafe(0, "System message", "The module contains pattern lengths above 256! They will be set to 64.");

	return true;

pattCorrupt:
	okBoxThreadSafe(0, "System message", "Error loading .xm: Either a corrupt or a non-supported module!");
	return false;
}

// called from input/video thread after the module was done loading
static void setupLoadedModule(void)
{
	int16_t i;

	lockMixerCallback();

	freeAllInstr();
	freeAllPatterns();

	oldPlayMode = playMode;
	playMode = PLAYMODE_IDLE;
	songPlaying = false;

	editor.currVolEnvPoint = 0;
	editor.currPanEnvPoint = 0;

#ifdef HAS_MIDI
	midi.currMIDIVibDepth = 0;
	midi.currMIDIPitch = 0;
#endif

	memset(editor.keyOnTab, 0, sizeof (editor.keyOnTab));

	// copy over new pattern pointers and lengths
	for (i = 0; i < MAX_PATTERNS; i++)
	{
		patt[i] = pattTmp[i];
		pattLens[i] = pattLensTmp[i];
	}

	// copy over new instruments (includes sample pointers)
	for (i = 1; i <= MAX_INST; i++)
	{
		instr[i] = instrTmp[i];
		fixSampleName(i);
	}

	// copy over song struct
	memcpy(&song, &songTmp, sizeof (songTyp));
	fixSongName();

	// we are the owners of the allocated memory ptrs set by the loader thread now

	// support non-even channel numbers
	if (song.antChn & 1)
	{
		if (++song.antChn > MAX_VOICES)
			song.antChn = MAX_VOICES;
	}

	if (song.repS > song.len)
		song.repS = 0;

	song.globVol = 64;
	song.timer = 1;

	setScrollBarEnd(SB_POS_ED, (song.len - 1) + 5);
	setScrollBarPos(SB_POS_ED, 0, false);

	resetChannels();
	refreshScopes();
	setPos(0, 0, false);
	setSpeed(song.speed);

	editor.tmpPattern = editor.editPattern; // set kludge variable
	editor.speed = song.speed;
	editor.tempo = song.tempo;
	editor.timer = 1;
	editor.globalVol = song.globVol;

	setFrqTab((loadedFormat == FORMAT_XM) ? linearFreqTable : false);
	unlockMixerCallback();

	exitTextEditing();
	updateTextBoxPointers();
	resetChannelOffset();
	updateChanNums();
	resetWavRenderer();
	clearPattMark();
	song.musicTime = 0;
	resetTrimSizes();

	diskOpSetFilename(DISKOP_ITEM_MODULE, editor.tmpFilenameU);

	// redraw top part of screen
	if (editor.ui.extended)
	{
		// first exit extended mode, then re-enter
		togglePatternEditorExtended();
		togglePatternEditorExtended();
	}
	else
	{
		// redraw top screen
		hideTopScreen();
		showTopScreen(true);
	}

	updateSampleEditorSample();
	showBottomScreen(); // redraw bottom screen (also redraws pattern editor)

	if (editor.ui.instEditorShown)
		drawPiano(); // redraw piano now (since if playing = wait for next tick update)

	removeSongModifiedFlag();

	moduleFailedToLoad = false;
	moduleLoaded = false;
	editor.loadMusicEvent = EVENT_NONE;
}

bool handleModuleLoadFromArg(int argc, char **argv)
{
	int32_t filesize;
	uint32_t filenameLen;
	UNICHAR *filenameU, tmpPathU[PATH_MAX + 2];

	// this is crude, we always expect only one parameter, and that it is the module.

	if (argc != 2)
		return false;

#ifdef __APPLE__
	if (argc == 2 && !strncmp(argv[1], "-psn_", 5))
		return false; // OS X < 10.9 passes a -psn_x_xxxxx parameter on double-click launch
#endif

	filenameLen = (uint32_t)strlen(argv[1]);

	filenameU = (UNICHAR *)calloc((filenameLen + 1), sizeof (UNICHAR));
	if (filenameU == NULL)
	{
		okBox(0, "System message", "Not enough memory!");
		return false;
	}

#ifdef _WIN32
	MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, filenameU, filenameLen);
#else
	strcpy(filenameU, argv[1]);
#endif

	// store old path
	UNICHAR_GETCWD(tmpPathU, PATH_MAX);

	// set binary path
	UNICHAR_CHDIR(editor.binaryPathU);

	filesize = getFileSize(filenameU);

	if (filesize == -1) // >2GB
	{
		okBox(0, "System message", "The file is too big and can't be loaded (over 2GB).");
		goto argLoadErr;
	}

	if (filesize >= 128L*1024*1024) // 128MB
	{
		if (okBox(2, "System request", "Are you sure you want to load such a big file?") != 1)
			goto argLoadErr;
	}

	editor.loadMusicEvent = EVENT_LOADMUSIC_ARGV;
	loadMusic(filenameU);
	
	UNICHAR_CHDIR(tmpPathU); // set old path back
	free(filenameU);
	return true;

argLoadErr:
	UNICHAR_CHDIR(tmpPathU); // set old path back
	free(filenameU);
	return false;
}

void loadDroppedFile(char *fullPathUTF8, bool songModifiedCheck)
{
	int32_t fullPathLen, filesize;
	UNICHAR *fullPathU;

	if (editor.ui.sysReqShown || fullPathUTF8 == NULL)
		return;

	fullPathLen = (int32_t)strlen(fullPathUTF8);
	if (fullPathLen == 0)
		return;

	fullPathU = (UNICHAR *)calloc(fullPathLen + 2, sizeof (UNICHAR));
	if (fullPathU == NULL)
	{
		okBox(0, "System message", "Not enough memory!");
		return;
	}

#ifdef _WIN32
	MultiByteToWideChar(CP_UTF8, 0, fullPathUTF8, -1, fullPathU, fullPathLen);
#else
	strcpy(fullPathU, fullPathUTF8);
#endif

	filesize = getFileSize(fullPathU);

	if (filesize == -1) // >2GB
	{
		okBox(0, "System message", "The file is too big and can't be loaded (over 2GB).");
		free(fullPathU);
		return;
	}

	if (filesize >= 128L*1024*1024) // 128MB
	{
		if (okBox(2, "System request", "Are you sure you want to load such a big file?") != 1)
		{
			free(fullPathU);
			return;
		}
	}

	// pass UTF8 to these tests so that we can test file ending in ASCII/ANSI

	if (fileIsInstrument(fullPathUTF8))
	{
		loadInstr(fullPathU);
	}
	else if (fileIsSample(fullPathUTF8))
	{
		loadSample(fullPathU, editor.curSmp, false);
	}
	else
	{
		SDL_RestoreWindow(video.window);

		if (songModifiedCheck && song.isModified)
		{
			// de-minimize window and set focus so that the user sees the message box
			SDL_RestoreWindow(video.window);
			SDL_RaiseWindow(video.window);

			if (okBox(2, "System request", "You have unsaved changes in your song. Load new song and lose all changes?") != 1)
			{
				free(fullPathU);
				return;
			}
		}

		editor.loadMusicEvent = EVENT_LOADMUSIC_DRAGNDROP;
		loadMusic(fullPathU);
	}

	free(fullPathU);
}

static void handleOldPlayMode(void)
{
	playMode = oldPlayMode;
	if (oldPlayMode != PLAYMODE_IDLE && oldPlayMode != PLAYMODE_EDIT)
		startPlaying(oldPlayMode, 0);

	songPlaying = (playMode >= PLAYMODE_SONG);
}

// called from input/video thread after module load thread was finished
void handleLoadMusicEvents(void)
{
	if (!moduleLoaded && !moduleFailedToLoad)
		return; // no event to handle

	if (moduleFailedToLoad)
	{
		// module failed to load from loading thread
		musicIsLoading = false;
		moduleFailedToLoad = false;
		moduleLoaded = false;
		editor.loadMusicEvent = EVENT_NONE;
		setMouseBusy(false);
		return;
	}

	if (moduleLoaded)
	{
		// module was successfully loaded from loading thread

		switch (editor.loadMusicEvent)
		{
			// module dragged and dropped *OR* user double clicked a file associated with FT2 clone
			case EVENT_LOADMUSIC_DRAGNDROP:
			{
				setupLoadedModule();
				if (editor.autoPlayOnDrop)
					startPlaying(PLAYMODE_SONG, 0);
				else
					handleOldPlayMode();
			}
			break;

			// filename passed as an exe argument *OR* user double clicked a file associated with FT2 clone
			case EVENT_LOADMUSIC_ARGV:
			{
				setupLoadedModule();
				startPlaying(PLAYMODE_SONG, 0);
			}
			break;

			// module filename pressed in Disk Op.
			case EVENT_LOADMUSIC_DISKOP:
			{
				setupLoadedModule();
				handleOldPlayMode();
			}
			break;

			default: break;
		}

		moduleLoaded = false;
		editor.loadMusicEvent = EVENT_NONE;
		musicIsLoading = false;
		mouseAnimOff();
	}
}
