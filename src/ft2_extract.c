/* Headless extraction of an XM (or any loadable module) into DAW-ready material.
**
** One render pass: every mixer voice is handed to a sink tagged with (channel, instrument),
** and accumulated into four stem families at once:
**   stems/chan-instr/chNN-iNNN-name.wav   one per (channel, instrument) pair that made sound
**   stems/channels/ch-NN.wav              per channel (sum of its pairs)
**   stems/instruments/iNNN-name.wav       per instrument (sum of its pairs)
**   stems/mix.wav                         everything (reference, == normal WAV render)
** All sums happen in float in the same pass, so stems add up to the mix exactly.
**
** Also written: samples (WAV with loop points), MIDI files, meta.json, timeline.json, patterns.txt.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/resource.h>
#endif
#include "ft2_header.h"
#include "ft2_config.h"
#include "ft2_audio.h"
#include "ft2_replayer.h"
#include "ft2_module_loader.h"
#include "ft2_sample_ed.h" // fixSample / unfixSample
#include "ft2_structs.h"
#include "ft2_extract.h"
#include "mixer/ft2_mix.h"
#include "mixer/ft2_windowed_sinc.h"

void dump_TickReplayer(void); // ft2_wav_renderer.c

#define MIDI_PPQ 960
#define MAX_TIMELINE_ROWS (MAX_ORDERS * MAX_PATT_LEN * 4)
#define FILE_IO_BUF_SIZE (64 * 1024)

/* ---------------------------------------------------------------------------
** options
*/

typedef struct extractOpts_t
{
	const char *modPath, *outDir;
	uint32_t rate;
	uint8_t bits, interpolation;
	int16_t amp; // 1..32, FT2 "Amp." (boost level); 10 = FT2 default config
	int32_t rowsPerBeat; // 0 = auto
	double maxSeconds;
	bool stems, samples, midi;
	bool allSamples; // also write samples of instruments never played in the song
	bool stemPairs, stemChannels, stemInstruments, stemMix;
	int16_t orderFrom, orderTo; // -1 = whole song
	int16_t patternNum; // -1 = not pattern mode
	double tailSeconds; // < 0 = auto (0 for whole song, 2 for ranges)
	bool carryVoices; // keep notes ringing from before an order range instead of cutting them
} extractOpts_t;

typedef enum rangeMode_t { RANGE_SONG, RANGE_ORDERS, RANGE_PATTERN } rangeMode_t;

static rangeMode_t rangeMode(const extractOpts_t *o)
{
	if (o->patternNum >= 0) return RANGE_PATTERN;
	if (o->orderFrom >= 0) return RANGE_ORDERS;
	return RANGE_SONG;
}

static double tailSeconds(const extractOpts_t *o)
{
	if (o->tailSeconds >= 0.0)
		return o->tailSeconds;
	return (rangeMode(o) == RANGE_SONG) ? 0.0 : 2.0;
}

static void printUsage(void)
{
	fprintf(stderr,
		"usage: ft2-clone --extract <module> <outdir> [options]\n"
		"  --rate <hz>          stem sample rate (default 48000)\n"
		"  --bits 32|16         stem bit depth, 32 = float (default 32)\n"
		"  --amp <1..32>        FT2 amp/boost level (default 10 = FT2 default config)\n"
		"  --stems <list>       comma list of channels,instruments,pairs,mix (default all)\n"
		"  --interp <mode>      none|linear|quadratic|cubic|sinc8|sinc16 (default sinc8)\n"
		"  --rows-per-beat <n>  MIDI grid, default 24/speed (speed 6 = 4, speed 3 = 8)\n"
		"  --max-seconds <s>    stop rendering after s seconds (default 1800)\n"
		"  --order <A>[-<B>]    only order positions A..B (0-based), with the song state at A;\n"
		"                       output goes to <outdir>/orders/oAA-oBB/\n"
		"  --pattern <N>        only pattern N, alone, from a clean state (FT2 play-pattern);\n"
		"                       output goes to <outdir>/patterns/pNN/\n"
		"  --tail <s>           seconds of ring-out after the range (default 2, 0 for whole song)\n"
		"  --carry-voices       --order: keep notes ringing from before A instead of cutting them\n"
		"  --no-stems           skip audio stems\n"
		"  --no-samples         skip sample WAVs\n"
		"  --all-samples        also write samples of instruments the song never plays\n"
		"  --no-midi            skip MIDI files\n");
}

static bool parseInterpolation(const char *s, uint8_t *out)
{
	static const struct { const char *name; uint8_t value; } table[] =
	{
		{ "none", INTERPOLATION_DISABLED }, { "linear", INTERPOLATION_LINEAR },
		{ "quadratic", INTERPOLATION_QUADRATIC }, { "cubic", INTERPOLATION_CUBIC },
		{ "sinc8", INTERPOLATION_SINC8 }, { "sinc16", INTERPOLATION_SINC16 },
	};

	for (size_t i = 0; i < sizeof (table) / sizeof (table[0]); i++)
	{
		if (strcmp(s, table[i].name) == 0)
		{
			*out = table[i].value;
			return true;
		}
	}

	return false;
}

static bool parseOpts(int argc, char **argv, extractOpts_t *o)
{
	memset(o, 0, sizeof (*o));
	o->rate = 48000;
	o->bits = 32;
	o->interpolation = INTERPOLATION_SINC8;
	o->amp = 10;
	o->maxSeconds = 1800.0;
	o->stems = o->samples = o->midi = true;
	o->stemPairs = o->stemChannels = o->stemInstruments = o->stemMix = true;
	o->orderFrom = o->orderTo = o->patternNum = -1;
	o->tailSeconds = -1.0;

	if (argc < 4)
		return false;

	o->modPath = argv[2];
	o->outDir = argv[3];

	for (int32_t i = 4; i < argc; i++)
	{
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i+1] : NULL;

		if (strcmp(a, "--no-stems") == 0) o->stems = false;
		else if (strcmp(a, "--no-samples") == 0) o->samples = false;
		else if (strcmp(a, "--all-samples") == 0) o->allSamples = true;
		else if (strcmp(a, "--no-midi") == 0) o->midi = false;
		else if (v != NULL && strcmp(a, "--rate") == 0) { o->rate = (uint32_t)atoi(v); i++; }
		else if (v != NULL && strcmp(a, "--bits") == 0) { o->bits = (uint8_t)atoi(v); i++; }
		else if (v != NULL && strcmp(a, "--amp") == 0) { o->amp = (int16_t)atoi(v); i++; }
		else if (v != NULL && strcmp(a, "--stems") == 0)
		{
			o->stemPairs = (strstr(v, "pairs") != NULL);
			o->stemChannels = (strstr(v, "channels") != NULL);
			o->stemInstruments = (strstr(v, "instruments") != NULL);
			o->stemMix = (strstr(v, "mix") != NULL);
			if (strcmp(v, "all") == 0)
				o->stemPairs = o->stemChannels = o->stemInstruments = o->stemMix = true;
			if (!(o->stemPairs || o->stemChannels || o->stemInstruments || o->stemMix))
			{
				fprintf(stderr, "--stems: nothing selected in \"%s\"\n", v);
				return false;
			}
			i++;
		}
		else if (v != NULL && strcmp(a, "--rows-per-beat") == 0) { o->rowsPerBeat = atoi(v); i++; }
		else if (v != NULL && strcmp(a, "--max-seconds") == 0) { o->maxSeconds = atof(v); i++; }
		else if (v != NULL && strcmp(a, "--tail") == 0) { o->tailSeconds = atof(v); i++; }
		else if (strcmp(a, "--carry-voices") == 0) o->carryVoices = true;
		else if (v != NULL && strcmp(a, "--pattern") == 0) { o->patternNum = (int16_t)atoi(v); i++; }
		else if (v != NULL && strcmp(a, "--order") == 0)
		{
			int from = -1, to = -1;
			const int got = sscanf(v, "%d-%d", &from, &to);
			if (got < 1 || from < 0)
			{
				fprintf(stderr, "--order: expected A or A-B, got \"%s\"\n", v);
				return false;
			}
			o->orderFrom = (int16_t)from;
			o->orderTo = (int16_t)((got == 2) ? to : from);
			if (o->orderTo < o->orderFrom)
			{
				fprintf(stderr, "--order: B must be >= A\n");
				return false;
			}
			i++;
		}
		else if (v != NULL && strcmp(a, "--interp") == 0)
		{
			if (!parseInterpolation(v, &o->interpolation))
			{
				fprintf(stderr, "unknown interpolation \"%s\"\n", v);
				return false;
			}
			i++;
		}
		else
		{
			fprintf(stderr, "unknown option \"%s\"\n", a);
			return false;
		}
	}

	if (o->rate < 8000 || o->rate > 384000 || (o->bits != 16 && o->bits != 32) || o->maxSeconds <= 0.0)
		return false;

	if (o->amp < 1 || o->amp > 32)
		return false;

	if (o->patternNum >= 0 && o->orderFrom >= 0)
	{
		fprintf(stderr, "--order and --pattern are exclusive\n");
		return false;
	}

	if (o->patternNum >= MAX_PATTERNS)
		return false;

	return true;
}

/* ---------------------------------------------------------------------------
** small helpers: paths, names, little-endian writers, JSON strings
*/

static bool makeOneDir(const char *path)
{
#ifdef _WIN32
	int32_t r = _mkdir(path);
#else
	int32_t r = mkdir(path, 0755);
#endif
	return (r == 0 || errno == EEXIST);
}

// mkdir -p
static bool makeDir(const char *path)
{
	char tmp[PATH_MAX];
	const size_t len = strlen(path);

	if (len == 0 || len >= sizeof (tmp))
		return false;

	strcpy(tmp, path);
	for (size_t i = 1; i < len; i++)
	{
		if (tmp[i] == '/')
		{
			tmp[i] = '\0';
			if (!makeOneDir(tmp))
				return false;
			tmp[i] = '/';
		}
	}

	return makeOneDir(tmp);
}

static void joinPath(char *out, size_t outSize, const char *dir, const char *name)
{
	snprintf(out, outSize, "%s/%s", dir, name);
}

// filesystem-safe version of an FT2 name (CP437 bytes, spaces...). Empty -> "untitled".
static bool hasSuffixNoCase(const char *s, size_t len, const char *suffix)
{
	const size_t sl = strlen(suffix);
	if (len < sl)
		return false;

	for (size_t i = 0; i < sl; i++)
	{
		char a = s[len - sl + i], b = suffix[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (a != b)
			return false;
	}

	return true;
}

// sample names in old modules often carry their source file extension ("Break.wav")
static size_t nameLengthWithoutExtension(const char *in)
{
	static const char *exts[] = { ".wav", ".aif", ".aiff", ".iff", ".raw", ".smp", ".8svx", ".snd", ".voc" };
	const size_t len = strlen(in);

	for (size_t i = 0; i < sizeof (exts) / sizeof (exts[0]); i++)
	{
		if (hasSuffixNoCase(in, len, exts[i]) && len > strlen(exts[i]))
			return len - strlen(exts[i]);
	}

	return len;
}

static void sanitizeName(const char *in, char *out, size_t outSize)
{
	size_t n = 0;
	bool lastWasSep = true;
	const char *end = in + nameLengthWithoutExtension(in);

	for (const char *p = in; p < end && n + 1 < outSize; p++)
	{
		const uint8_t c = (uint8_t)*p;
		const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';

		if (keep)
		{
			out[n++] = (char)c;
			lastWasSep = false;
		}
		else if (!lastWasSep)
		{
			out[n++] = '_';
			lastWasSep = true;
		}
	}

	while (n > 0 && (out[n-1] == '_' || out[n-1] == '.'))
		n--;

	out[n] = '\0';
	if (n == 0)
		strncpy(out, "untitled", outSize);
}

/* All output names go through these, so the naming scheme is a one-place change. */

static void channelStemName(char *out, size_t n, int32_t ch)
{
	snprintf(out, n, "ch-%02d.wav", ch + 1);
}

static void instrumentStemName(char *out, size_t n, int32_t ins)
{
	char name[64];
	sanitizeName(song.instrName[ins], name, sizeof (name));
	snprintf(out, n, "i%03d-%s.wav", ins, name);
}

static void pairStemName(char *out, size_t n, int32_t ch, int32_t ins)
{
	char name[64];
	sanitizeName(song.instrName[ins], name, sizeof (name));
	snprintf(out, n, "ch%02d-i%03d-%s.wav", ch + 1, ins, name);
}

// sub directory of <outdir> for a range render: "orders/o05-o07", "patterns/p12", "" for the whole song
static void rangeDirName(char *out, size_t n, const extractOpts_t *o)
{
	switch (rangeMode(o))
	{
		case RANGE_ORDERS: snprintf(out, n, "orders/o%02d-o%02d", o->orderFrom, o->orderTo); break;
		case RANGE_PATTERN: snprintf(out, n, "patterns/p%02d", o->patternNum); break;
		default: out[0] = '\0'; break;
	}
}

static void sampleFileName(char *out, size_t n, int32_t ins, int32_t smp, const char *smpName)
{
	char name[64];
	sanitizeName(smpName, name, sizeof (name));
	snprintf(out, n, "i%03d-s%02d-%s.wav", ins, smp + 1, name);
}

static void channelMidiName(char *out, size_t n, int32_t ch)
{
	snprintf(out, n, "ch-%02d.mid", ch + 1);
}

static void instrumentMidiName(char *out, size_t n, int32_t ins)
{
	char name[64];
	sanitizeName(song.instrName[ins], name, sizeof (name));
	snprintf(out, n, "i%03d-%s.mid", ins, name);
}

static void fputLE16(FILE *f, uint16_t v)
{
	fputc(v & 0xFF, f);
	fputc((v >> 8) & 0xFF, f);
}

static void fputLE32(FILE *f, uint32_t v)
{
	fputLE16(f, (uint16_t)(v & 0xFFFF));
	fputLE16(f, (uint16_t)(v >> 16));
}

static void fputBE16(FILE *f, uint16_t v)
{
	fputc((v >> 8) & 0xFF, f);
	fputc(v & 0xFF, f);
}

static void fputBE32(FILE *f, uint32_t v)
{
	fputBE16(f, (uint16_t)(v >> 16));
	fputBE16(f, (uint16_t)(v & 0xFFFF));
}

// bytes >= 0x80 are CP437 in FT2; we have no table here, so they become '?'
static void fputJsonString(FILE *f, const char *s)
{
	fputc('"', f);
	for (const char *p = s; *p != '\0'; p++)
	{
		const uint8_t c = (uint8_t)*p;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else if (c >= 0x80)
			fputc('?', f);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

// 44-byte canonical WAV header (PCM or IEEE float)
static void writeWavHeader(FILE *f, uint32_t rate, uint16_t channels, uint16_t bits, bool isFloat, uint32_t dataBytes)
{
	const uint32_t riffSize = 36 + dataBytes + (dataBytes & 1);

	fwrite("RIFF", 1, 4, f);
	fputLE32(f, riffSize);
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	fputLE32(f, 16);
	fputLE16(f, isFloat ? 3 : 1);
	fputLE16(f, channels);
	fputLE32(f, rate);
	fputLE32(f, (rate * channels * bits) / 8);
	fputLE16(f, (uint16_t)((channels * bits) / 8));
	fputLE16(f, bits);
	fwrite("data", 1, 4, f);
	fputLE32(f, dataBytes);
}

/* ---------------------------------------------------------------------------
** stems
*/

typedef struct stem_t
{
	FILE *f;
	char path[PATH_MAX];
	uint64_t framesWritten;
	float peak, *fTick; // fTick: interleaved stereo accumulation for the current tick
	bool dirty;
} stem_t;

typedef struct stemSet_t
{
	stem_t *pair[MAX_CHANNELS][MAX_INST+1];
	stem_t *chan[MAX_CHANNELS];
	stem_t *inst[MAX_INST+1];
	stem_t *mix;

	stem_t **all;
	int32_t numAll, capAll;

	uint32_t maxTickFrames, tickFrames, rate;
	uint64_t tickStartFrame;
	uint64_t fadeStartFrame, fadeEndFrame; // linear fade-out applied to every stem (0,0 = none), used at the end of the tail
	uint8_t bits;
	char dirPairs[PATH_MAX], dirChannels[PATH_MAX], dirInstruments[PATH_MAX], dirStems[PATH_MAX];
	bool wantPairs, wantChannels, wantInstruments;
	bool ioError;
} stemSet_t;

static stem_t *createStem(stemSet_t *set, const char *dir, const char *fileName)
{
	stem_t *s = (stem_t *)calloc(1, sizeof (stem_t));
	if (s == NULL)
		return NULL;

	s->fTick = (float *)calloc(set->maxTickFrames * 2, sizeof (float));
	joinPath(s->path, sizeof (s->path), dir, fileName);

	s->f = fopen(s->path, "wb");
	if (s->fTick == NULL || s->f == NULL)
	{
		fprintf(stderr, "can't create stem \"%s\"\n", s->path);
		set->ioError = true;
		free(s->fTick);
		free(s);
		return NULL;
	}

	setvbuf(s->f, NULL, _IOFBF, FILE_IO_BUF_SIZE);
	writeWavHeader(s->f, set->rate, 2, set->bits, set->bits == 32, 0); // placeholder, rewritten at close

	if (set->numAll == set->capAll)
	{
		set->capAll = set->capAll ? set->capAll * 2 : 64;
		set->all = (stem_t **)realloc(set->all, set->capAll * sizeof (stem_t *));
	}
	set->all[set->numAll++] = s;

	return s;
}

static void accumulateToStem(stem_t *s, const float *fL, const float *fR, uint32_t n)
{
	if (s == NULL)
		return;

	float *out = s->fTick;
	for (uint32_t i = 0; i < n; i++)
	{
		out[(i * 2) + 0] += fL[i];
		out[(i * 2) + 1] += fR[i];
	}

	s->dirty = true;
}

// mixer callback: one voice's output for the current tick
static void stemSink(int32_t chNum, uint8_t instrTag, const float *fL, const float *fR, uint32_t n, void *ctx)
{
	stemSet_t *set = (stemSet_t *)ctx;
	char name[PATH_MAX];

	if (set->wantPairs && set->pair[chNum][instrTag] == NULL)
	{
		pairStemName(name, sizeof (name), chNum, instrTag);
		set->pair[chNum][instrTag] = createStem(set, set->dirPairs, name);
	}

	if (set->wantChannels && set->chan[chNum] == NULL)
	{
		channelStemName(name, sizeof (name), chNum);
		set->chan[chNum] = createStem(set, set->dirChannels, name);
	}

	if (set->wantInstruments && set->inst[instrTag] == NULL)
	{
		instrumentStemName(name, sizeof (name), instrTag);
		set->inst[instrTag] = createStem(set, set->dirInstruments, name);
	}

	accumulateToStem(set->pair[chNum][instrTag], fL, fR, n);
	accumulateToStem(set->chan[chNum], fL, fR, n);
	accumulateToStem(set->inst[instrTag], fL, fR, n);
	accumulateToStem(set->mix, fL, fR, n);
}

static void writeSilence(stemSet_t *set, stem_t *s, uint64_t frames)
{
	static const uint8_t zeros[8192];
	uint64_t bytes = frames * 2 * (set->bits / 8);

	while (bytes > 0)
	{
		const size_t chunk = (bytes > sizeof (zeros)) ? sizeof (zeros) : (size_t)bytes;
		if (fwrite(zeros, 1, chunk, s->f) != chunk)
			set->ioError = true;
		bytes -= chunk;
	}

	s->framesWritten += frames;
}

static void writeStemFrames(stemSet_t *set, stem_t *s, const float *fInterleaved, uint32_t frames)
{
	const uint32_t n = frames * 2;

	for (uint32_t i = 0; i < n; i++)
	{
		const float a = fabsf(fInterleaved[i]);
		if (a > s->peak)
			s->peak = a;
	}

	if (set->bits == 32)
	{
		if (fwrite(fInterleaved, sizeof (float), n, s->f) != n)
			set->ioError = true;
	}
	else
	{
		for (uint32_t i = 0; i < n; i++)
		{
			const float v = CLAMP(fInterleaved[i], -1.0f, 1.0f);
			fputLE16(s->f, (uint16_t)(int16_t)lrintf(v * 32767.0f));
		}
	}

	s->framesWritten += frames;
}

// after each tick: pad gaps with silence, write accumulated audio, clear accumulators
static void flushStems(stemSet_t *set)
{
	for (int32_t i = 0; i < set->numAll; i++)
	{
		stem_t *s = set->all[i];
		if (!s->dirty)
			continue;

		if (s->framesWritten < set->tickStartFrame)
			writeSilence(set, s, set->tickStartFrame - s->framesWritten);

		if (set->fadeEndFrame > 0 && set->tickStartFrame + set->tickFrames > set->fadeStartFrame)
		{
			// same gain curve on every stem, so the stems still sum to the mix
			const double fadeLen = (double)(set->fadeEndFrame - set->fadeStartFrame);
			for (uint32_t j = 0; j < set->tickFrames; j++)
			{
				const uint64_t frame = set->tickStartFrame + j;
				const double gain = (frame < set->fadeStartFrame) ? 1.0 : (frame >= set->fadeEndFrame) ? 0.0 : 1.0 - (frame - set->fadeStartFrame) / fadeLen;
				s->fTick[j*2+0] *= (float)gain;
				s->fTick[j*2+1] *= (float)gain;
			}
		}

		writeStemFrames(set, s, s->fTick, set->tickFrames);
		memset(s->fTick, 0, set->tickFrames * 2 * sizeof (float));
		s->dirty = false;
	}
}

static bool setupStemSet(stemSet_t *set, const extractOpts_t *o)
{
	memset(set, 0, sizeof (*set));
	set->rate = o->rate;
	set->bits = o->bits;
	set->maxTickFrames = (uint32_t)ceil(o->rate / (MIN_BPM / 2.5)) + 2;
	set->wantPairs = o->stemPairs;
	set->wantChannels = o->stemChannels;
	set->wantInstruments = o->stemInstruments;

	joinPath(set->dirStems, sizeof (set->dirStems), o->outDir, "stems");
	joinPath(set->dirPairs, sizeof (set->dirPairs), set->dirStems, "chan-instr");
	joinPath(set->dirChannels, sizeof (set->dirChannels), set->dirStems, "channels");
	joinPath(set->dirInstruments, sizeof (set->dirInstruments), set->dirStems, "instruments");

	if (!makeDir(set->dirStems)
		|| (set->wantPairs && !makeDir(set->dirPairs))
		|| (set->wantChannels && !makeDir(set->dirChannels))
		|| (set->wantInstruments && !makeDir(set->dirInstruments)))
	{
		fprintf(stderr, "can't create stem directories under \"%s\"\n", o->outDir);
		return false;
	}

	if (!o->stemMix)
		return true;

	set->mix = createStem(set, set->dirStems, "mix.wav");
	return set->mix != NULL;
}

// finalize headers, drop silent stems, free everything. Returns number of kept files.
static int32_t closeStemSet(stemSet_t *set, uint64_t totalFrames, FILE *index)
{
	int32_t kept = 0;

	for (int32_t i = 0; i < set->numAll; i++)
	{
		stem_t *s = set->all[i];

		if (s->framesWritten < totalFrames)
			writeSilence(set, s, totalFrames - s->framesWritten);

		const uint32_t dataBytes = (uint32_t)(s->framesWritten * 2 * (set->bits / 8));
		if (dataBytes & 1)
			fputc(0, s->f);

		rewind(s->f);
		writeWavHeader(s->f, set->rate, 2, set->bits, set->bits == 32, dataBytes);
		fclose(s->f);

		if (s->peak == 0.0f)
		{
			remove(s->path);
		}
		else
		{
			if (index != NULL)
			{
				fprintf(index, "%s", kept ? ",\n    " : "    ");
				fprintf(index, "{ \"file\": ");
				fputJsonString(index, s->path);
				fprintf(index, ", \"peak\": %.6f }", s->peak);
			}
			kept++;
		}

		free(s->fTick);
		free(s);
	}

	free(set->all);
	return kept;
}

/* ---------------------------------------------------------------------------
** timeline: one entry per pattern row the replayer actually read
*/

typedef struct rowEvent_t
{
	uint64_t frame;
	uint16_t songPos, pattNum, row, speed, BPM;
} rowEvent_t;

typedef struct timeline_t
{
	rowEvent_t *rows;
	int32_t numRows;
	uint64_t totalFrames; // range + tail
	uint64_t rangeEndFrame; // where the last pattern row ends (tail starts here)
	uint32_t rate;
	bool capped;
} timeline_t;

static void timelinePush(timeline_t *t, uint64_t frame)
{
	if (t->numRows >= MAX_TIMELINE_ROWS)
		return;

	rowEvent_t *e = &t->rows[t->numRows++];
	e->frame = frame;
	e->songPos = (uint16_t)song.songPos;
	e->pattNum = (uint16_t)song.pattNum;
	e->row = (uint16_t)song.row;
	e->speed = song.speed;
	e->BPM = song.BPM;
}

/* ---------------------------------------------------------------------------
** render pass
*/

// same rule as the GUI WAV renderer (dump_EndOfTune)
static bool endOfTune(int16_t endSongPos)
{
	bool done = (editor.wavReachedEndFlag && song.row == 0 && song.tick == 1) || (song.speed == 0);

	if (song.pattDelTime2 > 0)
		done = false;

	if (song.songPos == endSongPos && song.row == 0 && song.tick == 1)
		editor.wavReachedEndFlag = true;

	return done;
}

static void startHeadlessPlayback(const extractOpts_t *o)
{
	if (rangeMode(o) == RANGE_PATTERN)
	{
		// like FT2 "play pattern": no order list involved
		song.songPos = 0;
		song.pattNum = o->patternNum;
		song.currNumRows = patternNumRows[o->patternNum];
		song.row = 0;
		song.tick = 1;
		playMode = PLAYMODE_PATT;
	}
	else
	{
		setSongPos(0, 0, RESET_SONG_TICK);
		playMode = PLAYMODE_SONG;
	}
	songPlaying = true;

	resetChannels();
	resetReplayerState();
	setNewAudioFreq(o->rate);
	setAudioAmp(o->amp, 256, true); // always float normalization: 16-bit conversion is done in writeStemFrames

	stopVoices();
	song.globalVolume = 64;
	setMixerBPM(song.BPM);

	song.playbackSeconds = 0;
	song.playbackSecondsFrac = 0;
	editor.wavReachedEndFlag = false;
}

static void stopHeadlessPlayback(void)
{
	stopPlaying();
	if (song.speed == 0)
		song.speed = 6;
}

// kills every mixer voice without touching the replayer channel state (latched instruments etc.)
static void killVoices(void)
{
	for (int32_t i = 0; i < MAX_CHANNELS; i++)
		stopVoice(i);
}

// true on a tick where the replayer is about to read a new pattern row
static bool rowReadTick(void)
{
	return song.tick == 1 && song.pattDelTime2 == 0;
}

// true once the selected range has played out (checked on a row-read tick, before the row is read)
static bool rangeDone(const extractOpts_t *o, int16_t endSongPos, uint32_t patternEndsAtStart)
{
	switch (rangeMode(o))
	{
		case RANGE_ORDERS:
			return song.speed == 0 || (rowReadTick()
				&& (replayerPatternEnds - patternEndsAtStart >= (uint32_t)(o->orderTo - o->orderFrom + 1)
				|| song.songPos < o->orderFrom || song.songPos > o->orderTo));

		case RANGE_PATTERN:
			return song.speed == 0 || (rowReadTick() && replayerPatternEnds - patternEndsAtStart >= 1);

		default:
			return endOfTune(endSongPos);
	}
}

/*
** Runs the replayer over the whole song, an order range or one pattern.
** If set != NULL, audio is routed into stems.
**
** Order ranges pre-roll the song silently from order 0 up to A so speed/BPM,
** global volume and latched instruments are the real in-song ones. Then
** ringing voices are cut (unless --carry-voices) and capture starts at frame 0.
** After the range, songPlaying is cleared so no more rows are read while the
** voices ring out for the tail.
*/
static bool renderSong(const extractOpts_t *o, stemSet_t *set, timeline_t *t)
{
	const rangeMode_t mode = rangeMode(o);
	const int16_t endSongPos = song.songLength - 1;
	const uint64_t maxFrames = (uint64_t)(o->maxSeconds * o->rate);
	const uint64_t tailFrames = (uint64_t)(tailSeconds(o) * o->rate);
	uint64_t frame = 0, preRollFrames = 0, tailLeft = 0, tickSamplesFrac = 0;
	uint32_t patternEndsAtStart = 0;
	bool capturing = (mode != RANGE_ORDERS) || (o->orderFrom == 0);
	bool inTail = false;

	startHeadlessPlayback(o);
	patternEndsAtStart = replayerPatternEnds;
	t->rangeEndFrame = 0;

	for (;;)
	{
		if (!capturing)
		{
			// pre-roll: wait for the first row of order A
			if (rowReadTick() && song.songPos == o->orderFrom)
			{
				capturing = true;
				patternEndsAtStart = replayerPatternEnds;
				editor.wavReachedEndFlag = false;
				if (!o->carryVoices)
					killVoices();
			}
			else if (endOfTune(endSongPos) || preRollFrames >= maxFrames)
			{
				fprintf(stderr, "--order: position %d is never reached when playing the song\n", o->orderFrom);
				stopHeadlessPlayback();
				return false;
			}
		}

		if (capturing && !inTail)
		{
			if (rangeDone(o, endSongPos, patternEndsAtStart))
			{
				t->rangeEndFrame = frame;
				if (tailFrames == 0)
					break;

				inTail = true;
				tailLeft = tailFrames;
				songPlaying = false; // stop reading rows, keep voices + envelopes running

				if (set != NULL)
				{
					// fade the last 100 ms of the tail so looping samples don't end on a click
					const uint64_t fadeFrames = (tailFrames < o->rate / 10) ? tailFrames : o->rate / 10;
					set->fadeEndFrame = frame + tailFrames;
					set->fadeStartFrame = set->fadeEndFrame - fadeFrames;
				}
			}
			else if (frame >= maxFrames)
			{
				t->capped = true;
				t->rangeEndFrame = frame;
				break;
			}
			else if (rowReadTick())
			{
				timelinePush(t, frame); // this tick reads a new pattern row
			}
		}
		else if (inTail && tailLeft == 0)
		{
			break;
		}

		dump_TickReplayer();

		uint32_t tickSamples = audio.samplesPerTickInt;
		tickSamplesFrac += audio.samplesPerTickFrac;
		if (tickSamplesFrac >= BPM_FRAC_SCALE)
		{
			tickSamplesFrac &= BPM_FRAC_MASK;
			tickSamples++;
		}

		if (!capturing)
		{
			mixReplayerTickToStems(tickSamples, NULL, NULL); // advance voices, drop audio
			preRollFrames += tickSamples;
			continue;
		}

		if (inTail)
		{
			if (tickSamples > tailLeft)
				tickSamples = (uint32_t)tailLeft;
			tailLeft -= tickSamples;
		}

		if (set != NULL)
		{
			set->tickStartFrame = frame;
			set->tickFrames = tickSamples;
			mixReplayerTickToStems(tickSamples, stemSink, set);
			flushStems(set);

			if (set->ioError)
			{
				stopHeadlessPlayback();
				return false;
			}
		}
		else
		{
			// no audio wanted: still advance voices so the replayer state stays identical
			mixReplayerTickToStems(tickSamples, NULL, NULL);
		}

		frame += tickSamples;
	}

	t->totalFrames = frame;
	t->rate = o->rate;

	stopHeadlessPlayback();
	return true;
}

/* ---------------------------------------------------------------------------
** samples -> WAV (mono PCM, loop points in "smpl", name in LIST/INFO/INAM)
*/

static bool writeSampleWav(const char *path, instr_t *ins, sample_t *s)
{
	const bool is16Bit = !!(s->flags & SAMPLE_16BIT);
	const uint16_t bits = is16Bit ? 16 : 8;
	const uint32_t rate = (uint32_t)getSampleC4Hz(s);
	const uint32_t dataBytes = (uint32_t)s->length << is16Bit;
	const uint8_t loopType = GET_LOOPTYPE(s->flags);

	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return false;

	setvbuf(f, NULL, _IOFBF, FILE_IO_BUF_SIZE);

	unfixSample(s); // restore the bytes the mixer patched after the loop end

	writeWavHeader(f, rate ? rate : 8363, 1, bits, false, dataBytes);
	if (is16Bit)
	{
		fwrite(s->dataPtr, 1, dataBytes, f);
	}
	else
	{
		for (int32_t i = 0; i < s->length; i++)
			fputc((uint8_t)s->dataPtr[i] ^ 0x80, f); // unsigned 8-bit
	}
	if (dataBytes & 1)
		fputc(0, f);

	fixSample(s);

	if (loopType != LOOP_DISABLED && s->loopLength > 0)
	{
		fwrite("smpl", 1, 4, f);
		fputLE32(f, 36 + 24);
		fputLE32(f, 0); // manufacturer
		fputLE32(f, 0); // product
		fputLE32(f, rate ? 1000000000 / rate : 0); // sample period (ns)
		fputLE32(f, 60); // MIDI unity note
		fputLE32(f, 0); // pitch fraction
		fputLE32(f, 0); // SMPTE format
		fputLE32(f, 0); // SMPTE offset
		fputLE32(f, 1); // num loops
		fputLE32(f, 0); // sampler data
		fputLE32(f, 0); // loop id
		fputLE32(f, loopType - 1); // 0 = forward, 1 = ping-pong
		fputLE32(f, (uint32_t)s->loopStart);
		fputLE32(f, (uint32_t)(s->loopStart + s->loopLength) - 1);
		fputLE32(f, 0); // fraction
		fputLE32(f, 0); // play count (0 = infinite)
	}

	// modplug "xtra" chunk: default pan/volume + auto-vibrato, same as the GUI sample saver
	fwrite("xtra", 1, 4, f);
	fputLE32(f, 16);
	fputLE32(f, 0x20); // pan flag
	fputLE16(f, s->panning);
	fputLE16(f, (uint16_t)(s->volume * 4));
	fputLE16(f, 64);
	fputLE16(f, 0);
	fputc(ins->autoVibType, f);
	fputc(ins->autoVibSweep, f);
	fputc(ins->autoVibDepth, f);
	fputc(ins->autoVibRate, f);

	const size_t nameLen = strnlen(s->name, 22);
	if (nameLen > 0)
	{
		const uint32_t inamLen = (uint32_t)nameLen + 1;
		const uint32_t listLen = 4 + 8 + inamLen + (inamLen & 1);

		fwrite("LIST", 1, 4, f);
		fputLE32(f, listLen);
		fwrite("INFO", 1, 4, f);
		fwrite("INAM", 1, 4, f);
		fputLE32(f, inamLen);
		fwrite(s->name, 1, nameLen, f);
		fputc(0, f);
		if (inamLen & 1)
			fputc(0, f);
	}

	const uint32_t riffSize = (uint32_t)ftell(f) - 8;
	fseek(f, 4, SEEK_SET);
	fputLE32(f, riffSize);
	fclose(f);

	return true;
}

/* ---------------------------------------------------------------------------
** pattern analysis: notes resolved through the timeline (handles jumps/loops)
*/

typedef struct noteEvent_t
{
	uint64_t startFrame, endFrame;
	uint8_t ch, note, vel, instr; // note = MIDI note
} noteEvent_t;

typedef struct noteList_t
{
	noteEvent_t *v;
	int32_t n, cap;
} noteList_t;

static void noteListPush(noteList_t *l, noteEvent_t e)
{
	if (l->n == l->cap)
	{
		l->cap = l->cap ? l->cap * 2 : 1024;
		l->v = (noteEvent_t *)realloc(l->v, l->cap * sizeof (noteEvent_t));
	}

	l->v[l->n++] = e;
}

static uint8_t volumeToVelocity(int32_t vol64)
{
	const int32_t vel = (int32_t)lrint((CLAMP(vol64, 0, 64) * 127.0) / 64.0);
	return (uint8_t)CLAMP(vel, 1, 127);
}

static uint8_t defaultVelocity(uint8_t instrNum, uint8_t xmNote)
{
	instr_t *ins = (instrNum >= 1 && instrNum <= MAX_INST) ? instr[instrNum] : NULL;
	if (ins == NULL)
		return 100;

	const uint8_t smpNum = ins->note2SampleLUT[(xmNote - 1) % 96];
	return volumeToVelocity(ins->smp[smpNum & 0x0F].volume);
}

static uint8_t cellVelocity(const note_t *p, uint8_t instrNum)
{
	if (p->vol >= 0x10 && p->vol <= 0x50)
		return volumeToVelocity(p->vol - 0x10);

	return defaultVelocity(instrNum, p->note);
}

// walks the timeline and builds note on/off pairs per channel
static void buildNoteEvents(const timeline_t *t, noteList_t *out)
{
	int32_t active[MAX_CHANNELS]; // index into out->v, -1 = no note ringing
	uint8_t lastInstr[MAX_CHANNELS];

	for (int32_t i = 0; i < MAX_CHANNELS; i++)
		active[i] = -1;
	memset(lastInstr, 0, sizeof (lastInstr));

	for (int32_t r = 0; r < t->numRows; r++)
	{
		const rowEvent_t *e = &t->rows[r];
		if (pattern[e->pattNum] == NULL)
			continue;

		const note_t *p = &pattern[e->pattNum][e->row * MAX_CHANNELS];
		for (int32_t ch = 0; ch < song.numChannels; ch++, p++)
		{
			if (p->instr >= 1 && p->instr <= MAX_INST)
				lastInstr[ch] = p->instr;

			const bool noteCut = (p->efx == 0x0E && (p->efxData >> 4) == 0x0C);
			uint64_t endFrame = e->frame;
			if (noteCut)
				endFrame += (uint64_t)((p->efxData & 0x0F) * (2.5 / e->BPM) * t->rate);

			if (p->note == NOTE_OFF || noteCut)
			{
				if (active[ch] >= 0)
				{
					out->v[active[ch]].endFrame = endFrame;
					active[ch] = -1;
				}
				continue;
			}

			if (p->note < 1 || p->note > 96)
				continue;

			if (active[ch] >= 0)
				out->v[active[ch]].endFrame = e->frame;

			noteEvent_t ev;
			ev.startFrame = e->frame;
			ev.endFrame = t->rangeEndFrame;
			ev.ch = (uint8_t)ch;
			ev.note = (uint8_t)CLAMP(p->note + 11, 0, 127); // C-4 (49) -> MIDI 60
			ev.vel = cellVelocity(p, lastInstr[ch]);
			ev.instr = lastInstr[ch];

			noteListPush(out, ev);
			active[ch] = out->n - 1;
		}
	}

	// anything still ringing ends with the song
	for (int32_t i = 0; i < out->n; i++)
	{
		if (out->v[i].endFrame <= out->v[i].startFrame)
			out->v[i].endFrame = out->v[i].startFrame + 1;
	}
}

/* ---------------------------------------------------------------------------
** MIDI (SMF type 1): track 0 = tempo, then one track per XM channel
*/

typedef struct byteBuf_t
{
	uint8_t *d;
	size_t n, cap;
} byteBuf_t;

static void bbPut(byteBuf_t *b, uint8_t v)
{
	if (b->n == b->cap)
	{
		b->cap = b->cap ? b->cap * 2 : 4096;
		b->d = (uint8_t *)realloc(b->d, b->cap);
	}
	b->d[b->n++] = v;
}

static void bbPutVLQ(byteBuf_t *b, uint32_t v)
{
	uint8_t bytes[5];
	int32_t n = 0;

	do
	{
		bytes[n++] = v & 0x7F;
		v >>= 7;
	}
	while (v > 0);

	while (n > 0)
	{
		n--;
		bbPut(b, bytes[n] | (n > 0 ? 0x80 : 0));
	}
}

typedef struct midiEv_t
{
	uint32_t tick;
	uint8_t status, data1, data2; // status includes channel
} midiEv_t;

static int compareMidiEv(const void *a, const void *b)
{
	const midiEv_t *x = (const midiEv_t *)a;
	const midiEv_t *y = (const midiEv_t *)b;

	if (x->tick != y->tick)
		return (x->tick < y->tick) ? -1 : 1;

	// note-offs (0x80) before note-ons (0x90) at the same tick
	return (int)(x->status & 0xF0) - (int)(y->status & 0xF0);
}

typedef struct midiTiming_t
{
	double tempo; // BPM used to place events
	uint32_t rate;
} midiTiming_t;

static uint32_t frameToMidiTick(const midiTiming_t *mt, uint64_t frame)
{
	const double seconds = (double)frame / mt->rate;
	return (uint32_t)llrint(seconds * (mt->tempo / 60.0) * MIDI_PPQ);
}

static void writeMidiTrack(FILE *f, const byteBuf_t *b)
{
	fwrite("MTrk", 1, 4, f);
	fputBE32(f, (uint32_t)b->n + 4);
	fwrite(b->d, 1, b->n, f);
	fputc(0x00, f); fputc(0xFF, f); fputc(0x2F, f); fputc(0x00, f); // end of track
}

static void writeMidiTempoTrack(FILE *f, const midiTiming_t *mt, const char *name)
{
	byteBuf_t b = { NULL, 0, 0 };
	const uint32_t usPerBeat = (uint32_t)llrint(60000000.0 / mt->tempo);
	const size_t nameLen = strlen(name);

	bbPut(&b, 0); bbPut(&b, 0xFF); bbPut(&b, 0x03); bbPutVLQ(&b, (uint32_t)nameLen);
	for (size_t i = 0; i < nameLen; i++) bbPut(&b, (uint8_t)name[i]);

	bbPut(&b, 0); bbPut(&b, 0xFF); bbPut(&b, 0x51); bbPut(&b, 3);
	bbPut(&b, (usPerBeat >> 16) & 0xFF); bbPut(&b, (usPerBeat >> 8) & 0xFF); bbPut(&b, usPerBeat & 0xFF);

	bbPut(&b, 0); bbPut(&b, 0xFF); bbPut(&b, 0x58); bbPut(&b, 4);
	bbPut(&b, 4); bbPut(&b, 2); bbPut(&b, 24); bbPut(&b, 8); // 4/4

	writeMidiTrack(f, &b);
	free(b.d);
}

// one note track from the events of a single XM channel, filtered by instrument (0 = all)
static bool writeMidiNoteTrack(FILE *f, const noteList_t *notes, const midiTiming_t *mt, int32_t ch, int32_t instrFilter, const char *name)
{
	midiEv_t *ev = (midiEv_t *)malloc((size_t)notes->n * 2 * sizeof (midiEv_t));
	int32_t n = 0;

	if (ev == NULL)
		return false;

	const uint8_t midiChannel = (uint8_t)(ch & 0x0F);
	for (int32_t i = 0; i < notes->n; i++)
	{
		const noteEvent_t *ne = &notes->v[i];
		if (ne->ch != ch || (instrFilter != 0 && ne->instr != instrFilter))
			continue;

		uint32_t on = frameToMidiTick(mt, ne->startFrame);
		uint32_t off = frameToMidiTick(mt, ne->endFrame);
		if (off <= on)
			off = on + 1;

		ev[n++] = (midiEv_t){ on, (uint8_t)(0x90 | midiChannel), ne->note, ne->vel };
		ev[n++] = (midiEv_t){ off, (uint8_t)(0x80 | midiChannel), ne->note, 0 };
	}

	if (n == 0)
	{
		free(ev);
		return false;
	}

	qsort(ev, n, sizeof (midiEv_t), compareMidiEv);

	byteBuf_t b = { NULL, 0, 0 };
	const size_t nameLen = strlen(name);
	bbPut(&b, 0); bbPut(&b, 0xFF); bbPut(&b, 0x03); bbPutVLQ(&b, (uint32_t)nameLen);
	for (size_t i = 0; i < nameLen; i++) bbPut(&b, (uint8_t)name[i]);

	uint32_t last = 0;
	for (int32_t i = 0; i < n; i++)
	{
		bbPutVLQ(&b, ev[i].tick - last);
		last = ev[i].tick;
		bbPut(&b, ev[i].status);
		bbPut(&b, ev[i].data1);
		bbPut(&b, ev[i].data2);
	}

	writeMidiTrack(f, &b);
	free(b.d);
	free(ev);
	return true;
}

static bool writeMidiFile(const char *path, const noteList_t *notes, const midiTiming_t *mt, int32_t chFilter, int32_t instrFilter)
{
	// count tracks first (channels that have matching notes)
	bool chHasNotes[MAX_CHANNELS] = { false };
	int32_t numTracks = 0;

	for (int32_t i = 0; i < notes->n; i++)
	{
		const noteEvent_t *ne = &notes->v[i];
		if ((chFilter < 0 || ne->ch == chFilter) && (instrFilter == 0 || ne->instr == instrFilter))
			chHasNotes[ne->ch] = true;
	}
	for (int32_t ch = 0; ch < MAX_CHANNELS; ch++)
		numTracks += chHasNotes[ch];

	if (numTracks == 0)
		return false;

	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return false;

	fwrite("MThd", 1, 4, f);
	fputBE32(f, 6);
	fputBE16(f, 1);
	fputBE16(f, (uint16_t)(1 + numTracks));
	fputBE16(f, MIDI_PPQ);

	writeMidiTempoTrack(f, mt, song.name);

	char trackName[64];
	for (int32_t ch = 0; ch < MAX_CHANNELS; ch++)
	{
		if (!chHasNotes[ch])
			continue;

		snprintf(trackName, sizeof (trackName), "ch %02d", ch + 1);
		writeMidiNoteTrack(f, notes, mt, ch, instrFilter, trackName);
	}

	fclose(f);
	return true;
}

/* ---------------------------------------------------------------------------
** text + JSON outputs
*/

static const char *noteNames[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
static const char volEfxChars[16] = { '.', '.', '.', '.', '.', '.', '-', '+', 'D', 'U', 'S', 'V', 'P', 'L', 'R', 'M' };

static void formatCell(const note_t *p, char *out) // out: at least 15 chars
{
	char noteStr[4], instrStr[3], volStr[3], efxStr[4];

	if (p->note == 0) strcpy(noteStr, "...");
	else if (p->note == NOTE_OFF) strcpy(noteStr, "===");
	else snprintf(noteStr, sizeof (noteStr), "%s%d", noteNames[(p->note - 1) % 12], (p->note - 1) / 12);

	if (p->instr == 0) strcpy(instrStr, "..");
	else snprintf(instrStr, sizeof (instrStr), "%02X", p->instr);

	if (p->vol < 0x10) strcpy(volStr, "..");
	else if (p->vol <= 0x50) snprintf(volStr, sizeof (volStr), "%02X", p->vol - 0x10);
	else snprintf(volStr, sizeof (volStr), "%c%X", volEfxChars[p->vol >> 4], p->vol & 0x0F);

	if (p->efx == 0 && p->efxData == 0) strcpy(efxStr, "...");
	else snprintf(efxStr, sizeof (efxStr), "%c%02X", (p->efx < 10) ? ('0' + p->efx) : ('A' + p->efx - 10), p->efxData);

	sprintf(out, "%s %s %s %s", noteStr, instrStr, volStr, efxStr);
}

static void writePatternsText(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return;

	bool used[MAX_PATTERNS] = { false };
	for (int32_t i = 0; i < song.songLength; i++)
		used[song.orders[i]] = true;

	fprintf(f, "%s\n%d channels, speed %d, BPM %d\n", song.name, song.numChannels, song.initialSpeed, song.BPM);
	fprintf(f, "order:");
	for (int32_t i = 0; i < song.songLength; i++)
		fprintf(f, " %d", song.orders[i]);
	fprintf(f, "\n");

	char cell[16];
	for (int32_t pn = 0; pn < MAX_PATTERNS; pn++)
	{
		if (!used[pn])
			continue;

		fprintf(f, "\n=== pattern %d (%d rows) ===\n", pn, patternNumRows[pn]);
		for (int32_t row = 0; row < patternNumRows[pn]; row++)
		{
			fprintf(f, "%02X |", row);
			for (int32_t ch = 0; ch < song.numChannels; ch++)
			{
				static const note_t empty = { 0, 0, 0, 0, 0 };
				const note_t *p = pattern[pn] ? &pattern[pn][(row * MAX_CHANNELS) + ch] : &empty;
				formatCell(p, cell);
				fprintf(f, " %s |", cell);
			}
			fprintf(f, "\n");
		}
	}

	fclose(f);
}

/*
** patterns.json: every used pattern as rows of cells, for GUIs.
** Cell = [note, instr, vol, efx, efxData] raw XM values (note 1..96, 97 = key off,
** 0 = none), plus "text" with the FT2 display string. Empty cells are null.
*/
static void writePatternsJson(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return;

	bool used[MAX_PATTERNS] = { false };
	for (int32_t i = 0; i < song.songLength; i++)
		used[song.orders[i]] = true;

	char cell[16];
	bool firstPatt = true;
	fprintf(f, "{\n  \"numChannels\": %d,\n  \"patterns\": [\n", song.numChannels);
	for (int32_t pn = 0; pn < MAX_PATTERNS; pn++)
	{
		if (!used[pn])
			continue;

		fprintf(f, "%s    { \"index\": %d, \"rows\": [\n", firstPatt ? "" : ",\n", pn);
		firstPatt = false;

		for (int32_t row = 0; row < patternNumRows[pn]; row++)
		{
			fprintf(f, "      [");
			for (int32_t ch = 0; ch < song.numChannels; ch++)
			{
				static const note_t empty = { 0, 0, 0, 0, 0 };
				const note_t *p = pattern[pn] ? &pattern[pn][(row * MAX_CHANNELS) + ch] : &empty;
				const bool isEmpty = (p->note == 0 && p->instr == 0 && p->vol == 0 && p->efx == 0 && p->efxData == 0);

				if (ch > 0)
					fprintf(f, ", ");

				if (isEmpty)
				{
					fprintf(f, "null");
					continue;
				}

				formatCell(p, cell);
				fprintf(f, "{ \"note\": %d, \"instr\": %d, \"vol\": %d, \"efx\": %d, \"efxData\": %d, \"text\": ",
					p->note, p->instr, p->vol, p->efx, p->efxData);
				fputJsonString(f, cell);
				fprintf(f, " }");
			}
			fprintf(f, "]%s\n", (row + 1 < patternNumRows[pn]) ? "," : "");
		}
		fprintf(f, "    ] }");
	}
	fprintf(f, "\n  ]\n}\n");
	fclose(f);
}

static void writeTimelineJson(const char *path, const timeline_t *t)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return;

	fprintf(f, "[\n");
	for (int32_t i = 0; i < t->numRows; i++)
	{
		const rowEvent_t *e = &t->rows[i];
		fprintf(f, "  { \"order\": %d, \"pattern\": %d, \"row\": %d, \"frame\": %llu, \"seconds\": %.6f, \"speed\": %d, \"bpm\": %d }%s\n",
			e->songPos, e->pattNum, e->row, (unsigned long long)e->frame, (double)e->frame / t->rate, e->speed, e->BPM,
			(i + 1 < t->numRows) ? "," : "");
	}
	fprintf(f, "]\n");
	fclose(f);
}

// speed/BPM in force when the rendered range started (falls back to the song header)
static int32_t rangeStartSpeed(const timeline_t *t) { return t->numRows > 0 ? t->rows[0].speed : song.initialSpeed; }
static int32_t rangeStartBPM(const timeline_t *t) { return t->numRows > 0 ? t->rows[0].BPM : song.BPM; }

static int32_t autoRowsPerBeat(int32_t speed)
{
	if (speed > 0 && 24 % speed == 0)
		return CLAMP(24 / speed, 1, 24);

	return 4;
}

static double derivedTempo(int32_t BPM, int32_t speed, int32_t rowsPerBeat)
{
	return (24.0 * BPM) / ((double)rowsPerBeat * speed);
}

typedef struct usage_t
{
	int32_t pairNotes[MAX_CHANNELS][MAX_INST+1];
} usage_t;

static void countUsage(const noteList_t *notes, usage_t *u)
{
	memset(u, 0, sizeof (*u));
	for (int32_t i = 0; i < notes->n; i++)
		u->pairNotes[notes->v[i].ch][notes->v[i].instr]++;
}

// true if at least one note of instrument i is played in the song
static bool instrUsed(const usage_t *u, int32_t i)
{
	for (int32_t ch = 0; ch < song.numChannels; ch++)
	{
		if (u->pairNotes[ch][i] > 0)
			return true;
	}
	return false;
}

// true if the sample WAVs of instrument i get written
static bool instrSamplesExported(const extractOpts_t *o, const usage_t *u, int32_t i)
{
	return o->samples && (o->allSamples || instrUsed(u, i));
}

// FT2 envelope: up to 12 points (tick, value), flags bit0 on, bit1 sustain, bit2 loop. Point indices are 0-based.
static void fputEnvelopeJson(FILE *f, const int16_t points[12][2], uint8_t length, uint8_t sustain, uint8_t loopStart, uint8_t loopEnd, uint8_t flags)
{
	fprintf(f, "{ \"on\": %s, \"sustain\": %s, \"loop\": %s, \"sustainPoint\": %d, \"loopStart\": %d, \"loopEnd\": %d, \"points\": [",
		(flags & ENV_ENABLED) ? "true" : "false", (flags & ENV_SUSTAIN) ? "true" : "false", (flags & ENV_LOOP) ? "true" : "false",
		sustain, loopStart, loopEnd);
	const int32_t n = (length > 12) ? 12 : length;
	for (int32_t k = 0; k < n; k++)
		fprintf(f, "%s[%d,%d]", (k > 0) ? "," : "", points[k][0], points[k][1]);
	fprintf(f, "] }");
}

static void writeMetaJson(const char *path, const extractOpts_t *o, const timeline_t *t, const usage_t *u,
	int32_t rowsPerBeat, const char *stemIndexJson, int32_t numStems)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return;

	const int32_t speed = rangeStartSpeed(t), BPM = rangeStartBPM(t);
	const double tempo = derivedTempo(BPM, speed, rowsPerBeat);
	static const char *rangeModeNames[] = { "song", "orders", "pattern" };

	fprintf(f, "{\n");
	fprintf(f, "  \"file\": "); fputJsonString(f, o->modPath); fprintf(f, ",\n");
	fprintf(f, "  \"name\": "); fputJsonString(f, song.name); fprintf(f, ",\n");
	fprintf(f, "  \"numChannels\": %d,\n", song.numChannels);
	fprintf(f, "  \"songLength\": %d,\n", song.songLength);
	fprintf(f, "  \"restartPos\": %d,\n", song.songLoopStart);
	fprintf(f, "  \"speed\": %d,\n", song.initialSpeed);
	fprintf(f, "  \"bpm\": %d,\n", song.BPM);
	fprintf(f, "  \"linearPeriods\": %s,\n", audio.linearPeriodsFlag ? "true" : "false");
	fprintf(f, "  \"tempo\": { \"speed\": %d, \"xmBpm\": %d, \"ticksPerSecond\": %.6f, \"msPerRow\": %.6f, \"rowsPerBeat\": %d, \"bpm\": %.6f },\n",
		speed, BPM, BPM / 2.5, speed * 2500.0 / BPM, rowsPerBeat, tempo);
	fprintf(f, "  \"range\": { \"mode\": \"%s\", \"orderFrom\": %d, \"orderTo\": %d, \"pattern\": %d, \"carryVoices\": %s, \"rows\": %d, \"rangeFrames\": %llu, \"rangeSeconds\": %.6f, \"tailSeconds\": %.3f },\n",
		rangeModeNames[rangeMode(o)], o->orderFrom, o->orderTo, o->patternNum, o->carryVoices ? "true" : "false",
		t->numRows, (unsigned long long)t->rangeEndFrame, (double)t->rangeEndFrame / o->rate, tailSeconds(o));
	fprintf(f, "  \"render\": { \"rate\": %u, \"bits\": %d, \"amp\": %d, \"interpolation\": %d, \"durationSeconds\": %.6f, \"totalFrames\": %llu, \"capped\": %s, \"numStems\": %d },\n",
		o->rate, o->bits, o->amp, o->interpolation, (double)t->totalFrames / o->rate, (unsigned long long)t->totalFrames,
		t->capped ? "true" : "false", numStems);

	fprintf(f, "  \"orders\": [");
	for (int32_t i = 0; i < song.songLength; i++)
		fprintf(f, "%s%d", i ? ", " : "", song.orders[i]);
	fprintf(f, "],\n");

	fprintf(f, "  \"patterns\": [");
	bool first = true;
	for (int32_t i = 0; i < MAX_PATTERNS; i++)
	{
		bool used = false;
		for (int32_t j = 0; j < song.songLength; j++)
			used |= (song.orders[j] == i);
		if (!used)
			continue;

		fprintf(f, "%s{ \"index\": %d, \"rows\": %d }", first ? "" : ", ", i, patternNumRows[i]);
		first = false;
	}
	fprintf(f, "],\n");

	fprintf(f, "  \"instruments\": [\n");
	first = true;
	for (int32_t i = 1; i <= MAX_INST; i++)
	{
		instr_t *ins = instr[i];
		bool hasData = false;
		if (ins != NULL)
		{
			for (int32_t j = 0; j < MAX_SMP_PER_INST; j++)
				hasData |= (ins->smp[j].dataPtr != NULL && ins->smp[j].length > 0);
		}
		if (!hasData && song.instrName[i][0] == '\0')
			continue;

		fprintf(f, "%s    { \"num\": %d, \"name\": ", first ? "" : ",\n", i);
		fputJsonString(f, song.instrName[i]);
		first = false;

		if (ins == NULL)
		{
			fprintf(f, ", \"samples\": [] }");
			continue;
		}

		fprintf(f, ", \"used\": %s, \"fadeout\": %d, \"volEnv\": %s, \"panEnv\": %s, \"autoVib\": { \"type\": %d, \"sweep\": %d, \"depth\": %d, \"rate\": %d },\n",
			instrUsed(u, i) ? "true" : "false", ins->fadeout, (ins->volEnvFlags & 1) ? "true" : "false", (ins->panEnvFlags & 1) ? "true" : "false",
			ins->autoVibType, ins->autoVibSweep, ins->autoVibDepth, ins->autoVibRate);

		fprintf(f, "      \"volEnvelope\": ");
		fputEnvelopeJson(f, ins->volEnvPoints, ins->volEnvLength, ins->volEnvSustain, ins->volEnvLoopStart, ins->volEnvLoopEnd, ins->volEnvFlags);
		fprintf(f, ",\n      \"panEnvelope\": ");
		fputEnvelopeJson(f, ins->panEnvPoints, ins->panEnvLength, ins->panEnvSustain, ins->panEnvLoopStart, ins->panEnvLoopEnd, ins->panEnvFlags);
		fprintf(f, ",\n      \"noteSampleMap\": [");
		for (int32_t n = 0; n < 96; n++)
			fprintf(f, "%s%d", (n > 0) ? "," : "", ins->note2SampleLUT[n] + 1);
		fprintf(f, "],\n");

		fprintf(f, "      \"usedOnChannels\": [");
		bool firstCh = true;
		for (int32_t ch = 0; ch < song.numChannels; ch++)
		{
			if (u->pairNotes[ch][i] == 0)
				continue;
			fprintf(f, "%s{ \"channel\": %d, \"notes\": %d }", firstCh ? "" : ", ", ch + 1, u->pairNotes[ch][i]);
			firstCh = false;
		}
		fprintf(f, "],\n");

		fprintf(f, "      \"samples\": [");
		bool firstSmp = true;
		for (int32_t j = 0; j < MAX_SMP_PER_INST; j++)
		{
			sample_t *s = &ins->smp[j];
			if (s->dataPtr == NULL || s->length <= 0)
				continue;

			char fileName[PATH_MAX];
			sampleFileName(fileName, sizeof (fileName), i, j, s->name);

			fprintf(f, "%s\n        { \"num\": %d, \"name\": ", firstSmp ? "" : ",", j + 1);
			fputJsonString(f, s->name);
			fprintf(f, ", \"file\": ");
			if (instrSamplesExported(o, u, i))
				fputJsonString(f, fileName);
			else
				fprintf(f, "null");
			fprintf(f, ", \"length\": %d, \"bits\": %d, \"loopType\": %d, \"loopStart\": %d, \"loopLength\": %d, \"c4hz\": %d, \"finetune\": %d, \"relativeNote\": %d, \"volume\": %d, \"panning\": %d }",
				s->length, (s->flags & SAMPLE_16BIT) ? 16 : 8, GET_LOOPTYPE(s->flags), s->loopStart, s->loopLength,
				getSampleC4Hz(s), s->finetune, s->relativeNote, s->volume, s->panning);
			firstSmp = false;
		}
		fprintf(f, "%s] }", firstSmp ? "" : "\n      ");
	}
	fprintf(f, "\n  ],\n");

	fprintf(f, "  \"channels\": [\n");
	for (int32_t ch = 0; ch < song.numChannels; ch++)
	{
		fprintf(f, "    { \"channel\": %d, \"instruments\": [", ch + 1);
		bool firstIns = true;
		for (int32_t i = 1; i <= MAX_INST; i++)
		{
			if (u->pairNotes[ch][i] == 0)
				continue;
			fprintf(f, "%s{ \"num\": %d, \"notes\": %d }", firstIns ? "" : ", ", i, u->pairNotes[ch][i]);
			firstIns = false;
		}
		fprintf(f, "] }%s\n", (ch + 1 < song.numChannels) ? "," : "");
	}
	fprintf(f, "  ],\n");

	fprintf(f, "  \"stems\": [\n%s\n  ]\n", stemIndexJson ? stemIndexJson : "");
	fprintf(f, "}\n");
	fclose(f);
}

/* ---------------------------------------------------------------------------
** engine bring-up without window / audio device
*/

static bool setupHeadlessEngine(const extractOpts_t *o)
{
	if (SDL_Init(0) != 0)
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}

	if (!setupWindowedSincTables())
	{
		fprintf(stderr, "out of memory (sinc tables)\n");
		return false;
	}

	config.interpolation = o->interpolation;
	config.masterVol = 256;
	config.boostLevel = o->amp;
	config.specialFlags = BUFFSIZE_1024 | ((o->bits == 32) ? BITDEPTH_32 : BITDEPTH_16); // volume ramping stays on
	config.specialFlags2 = 0; // no PRECISE_BPM, same as a default config
	config.audioFreq = DEFAULT_AUDIO_FREQ;

	audioSetInterpolationType(config.interpolation);
	audioSetVolRamp(true);
	setAudioAmp(o->amp, 256, true); // always float normalization: 16-bit conversion is done in writeStemFrames

	if (!allocateMixBuffers())
	{
		fprintf(stderr, "out of memory (mix buffers)\n");
		return false;
	}

	if (!setupReplayer())
		return false;

	audio.locked = false; // no audio device: nothing to lock

	return true;
}

static void raiseOpenFileLimit(void)
{
#ifndef _WIN32
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
	{
		rlim_t want = 8192;
		if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max)
			want = rl.rlim_max;

		if (rl.rlim_cur < want)
		{
			rl.rlim_cur = want;
			setrlimit(RLIMIT_NOFILE, &rl);
		}
	}
#endif
}

/* ---------------------------------------------------------------------------
** main
*/

int extractMain(int argc, char **argv)
{
	extractOpts_t o;
	char path[PATH_MAX], dir[PATH_MAX], fileName[PATH_MAX];

	if (!parseOpts(argc, argv, &o))
	{
		printUsage();
		return 2;
	}

	raiseOpenFileLimit();

	char rangeDir[64], outDir[PATH_MAX];
	rangeDirName(rangeDir, sizeof (rangeDir), &o);
	if (rangeDir[0] != '\0')
	{
		joinPath(outDir, sizeof (outDir), o.outDir, rangeDir);
		o.outDir = outDir;
	}

	if (!setupHeadlessEngine(&o))
		return 1;

	if (!loadModuleHeadless(o.modPath))
		return 1;

	fprintf(stderr, "loaded \"%s\": %d channels, %d orders, speed %d, BPM %d\n",
		song.name, song.numChannels, song.songLength, song.initialSpeed, song.BPM);

	if (rangeMode(&o) == RANGE_ORDERS && o.orderTo >= song.songLength)
	{
		fprintf(stderr, "--order: song has %d positions (0..%d)\n", song.songLength, song.songLength - 1);
		return 1;
	}

	if (!makeDir(o.outDir))
	{
		fprintf(stderr, "can't create output directory \"%s\"\n", o.outDir);
		return 1;
	}

	// --- render pass (always runs: the timeline drives MIDI and metadata)
	timeline_t t;
	memset(&t, 0, sizeof (t));
	t.rows = (rowEvent_t *)calloc(MAX_TIMELINE_ROWS, sizeof (rowEvent_t));
	if (t.rows == NULL)
		return 1;

	stemSet_t set;
	char *stemIndexJson = NULL;
	int32_t numStems = 0;

	if (o.stems)
	{
		if (!setupStemSet(&set, &o))
			return 1;

		fprintf(stderr, "rendering stems at %u Hz / %d-bit, amp %d...\n", o.rate, o.bits, o.amp);
		if (!renderSong(&o, &set, &t))
		{
			fprintf(stderr, "render failed\n");
			return 1;
		}

		// collect the stem index for meta.json via a temp file
		joinPath(path, sizeof (path), o.outDir, ".stems.tmp");
		FILE *idx = fopen(path, "w+");
		numStems = closeStemSet(&set, t.totalFrames, idx);
		if (idx != NULL)
		{
			const long len = ftell(idx);
			stemIndexJson = (char *)calloc((size_t)len + 1, 1);
			rewind(idx);
			if (stemIndexJson != NULL && fread(stemIndexJson, 1, (size_t)len, idx) != (size_t)len)
				stemIndexJson[0] = '\0';
			fclose(idx);
			remove(path);
		}
	}
	else
	{
		fprintf(stderr, "running replayer (no stems)...\n");
		if (!renderSong(&o, NULL, &t))
			return 1;
	}

	fprintf(stderr, "rendered: %.2f s (%.2f s + %.2f s tail), %d rows%s, %d stems kept\n",
		(double)t.totalFrames / o.rate, (double)t.rangeEndFrame / o.rate, (double)(t.totalFrames - t.rangeEndFrame) / o.rate,
		t.numRows, t.capped ? " (capped by --max-seconds)" : "", numStems);

	// --- notes + usage
	noteList_t notes = { NULL, 0, 0 };
	buildNoteEvents(&t, &notes);

	usage_t usage;
	countUsage(&notes, &usage);

	const int32_t rowsPerBeat = (o.rowsPerBeat > 0) ? o.rowsPerBeat : autoRowsPerBeat(rangeStartSpeed(&t));

	// --- samples
	if (o.samples)
	{
		joinPath(dir, sizeof (dir), o.outDir, "samples");
		makeDir(dir);

		int32_t written = 0, skipped = 0;
		for (int32_t i = 1; i <= MAX_INST; i++)
		{
			instr_t *ins = instr[i];
			if (ins == NULL)
				continue;

			if (!instrSamplesExported(&o, &usage, i))
			{
				skipped++;
				continue;
			}

			for (int32_t j = 0; j < MAX_SMP_PER_INST; j++)
			{
				sample_t *s = &ins->smp[j];
				if (s->dataPtr == NULL || s->length <= 0)
					continue;

				sampleFileName(fileName, sizeof (fileName), i, j, s->name);
				joinPath(path, sizeof (path), dir, fileName);
				written += writeSampleWav(path, ins, s);
			}
		}
		fprintf(stderr, "samples: %d written, %d unused instruments skipped%s\n", written, skipped, skipped ? " (--all-samples to keep them)" : "");
	}

	// --- MIDI
	if (o.midi)
	{
		joinPath(dir, sizeof (dir), o.outDir, "midi");
		makeDir(dir);

		midiTiming_t mt;
		mt.rate = o.rate;
		mt.tempo = derivedTempo(rangeStartBPM(&t), rangeStartSpeed(&t), rowsPerBeat);

		int32_t written = 0;
		for (int32_t ch = 0; ch < song.numChannels; ch++)
		{
			channelMidiName(fileName, sizeof (fileName), ch);
			joinPath(path, sizeof (path), dir, fileName);
			written += writeMidiFile(path, &notes, &mt, ch, 0);
		}
		for (int32_t i = 1; i <= MAX_INST; i++)
		{
			instrumentMidiName(fileName, sizeof (fileName), i);
			joinPath(path, sizeof (path), dir, fileName);
			written += writeMidiFile(path, &notes, &mt, -1, i);
		}
		fprintf(stderr, "midi: %d files at %.3f BPM (%d rows/beat)\n", written, mt.tempo, rowsPerBeat);
	}

	// --- text + json
	joinPath(path, sizeof (path), o.outDir, "patterns.txt");
	writePatternsText(path);

	joinPath(path, sizeof (path), o.outDir, "patterns.json");
	writePatternsJson(path);

	joinPath(path, sizeof (path), o.outDir, "timeline.json");
	writeTimelineJson(path, &t);

	joinPath(path, sizeof (path), o.outDir, "meta.json");
	writeMetaJson(path, &o, &t, &usage, rowsPerBeat, stemIndexJson, numStems);

	free(stemIndexJson);
	free(notes.v);
	free(t.rows);
	SDL_Quit();

	fprintf(stderr, "done: %s\n", o.outDir);
	return 0;
}
