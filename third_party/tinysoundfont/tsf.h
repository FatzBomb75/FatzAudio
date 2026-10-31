/* TinySoundFont - v0.9 - SoundFont2 synthesizer - https://github.com/schellingb/TinySoundFont
                                     no warranty implied; use at your own risk

   ALTERED SOURCE VERSION (FatzAudio): this is not the original tsf.h. Loading is
   hardened for untrusted files; every change is marked "FatzAudio" and listed in
   README.md next to this file.
   Do this:
      #define TSF_IMPLEMENTATION
   before you include this file in *one* C or C++ file to create the implementation.
   // i.e. it should look like this:
   #include ...
   #include ...
   #define TSF_IMPLEMENTATION
   #include "tsf.h"

   [OPTIONAL] #define TSF_NO_STDIO to remove stdio dependency
   [OPTIONAL] #define TSF_MALLOC, TSF_REALLOC, and TSF_FREE to avoid stdlib.h
   [OPTIONAL] #define TSF_MEMCPY, TSF_MEMSET to avoid string.h
   [OPTIONAL] #define TSF_POW, TSF_POWF, TSF_EXPF, TSF_LOG, TSF_TAN, TSF_LOG10, TSF_SQRT to avoid math.h

   NOT YET IMPLEMENTED
     - Better low-pass filter without lowering performance too much
     - Linked modulators (FatzAudio: other modulators are supported, see README.md)

   LICENSE (MIT)

   Copyright (C) 2017-2025 Bernhard Schelling
   Based on SFZero, Copyright (C) 2012 Steve Folta (https://github.com/stevefolta/SFZero)

   Permission is hereby granted, free of charge, to any person obtaining a copy of this
   software and associated documentation files (the "Software"), to deal in the Software
   without restriction, including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
   to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
   USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#ifndef TSF_INCLUDE_TSF_INL
#define TSF_INCLUDE_TSF_INL

#ifdef __cplusplus
extern "C" {
#  define CPP_DEFAULT0 = 0
#else
#  define CPP_DEFAULT0
#endif

//define this if you want the API functions to be static
#ifdef TSF_STATIC
#define TSFDEF static
#else
#define TSFDEF extern
#endif

// The load functions will return a pointer to a struct tsf which all functions
// thereafter take as the first parameter.
// On error the tsf_load* functions will return NULL most likely due to invalid
// data (or if the file did not exist in tsf_load_filename).
typedef struct tsf tsf;

#ifndef TSF_NO_STDIO
// Directly load a SoundFont from a .sf2 file path
TSFDEF tsf* tsf_load_filename(const char* filename);
#endif

// Load a SoundFont from a block of memory
TSFDEF tsf* tsf_load_memory(const void* buffer, int size);

// Stream structure for the generic loading
struct tsf_stream
{
	// Custom data given to the functions as the first parameter
	void* data;

	// Function pointer will be called to read 'size' bytes into ptr (returns number of read bytes)
	int (*read)(void* data, void* ptr, unsigned int size);

	// Function pointer will be called to skip ahead over 'count' bytes (returns 1 on success, 0 on error)
	int (*skip)(void* data, unsigned int count);
};

// Generic SoundFont loading method using the stream structure above
TSFDEF tsf* tsf_load(struct tsf_stream* stream);

// Copy a tsf instance from an existing one, use tsf_close to close it as well.
// All copied tsf instances and their original instance are linked, and share the underlying soundfont.
// This allows loading a soundfont only once, but using it for multiple independent playbacks.
// (This function isn't thread-safe without locking.)
TSFDEF tsf* tsf_copy(tsf* f);

// Free the memory related to this tsf instance
TSFDEF void tsf_close(tsf* f);

// Stop all playing notes immediately and reset all channel parameters
TSFDEF void tsf_reset(tsf* f);

// Returns the preset index from a bank and preset number, or -1 if it does not exist in the loaded SoundFont
TSFDEF int tsf_get_presetindex(const tsf* f, int bank, int preset_number);

// Returns the number of presets in the loaded SoundFont
TSFDEF int tsf_get_presetcount(const tsf* f);

// Returns the name of a preset index >= 0 and < tsf_get_presetcount()
TSFDEF const char* tsf_get_presetname(const tsf* f, int preset_index);

// Returns the name of a preset by bank and preset number
TSFDEF const char* tsf_bank_get_presetname(const tsf* f, int bank, int preset_number);

// Supported output modes by the render methods
enum TSFOutputMode
{
	// Two channels with single left/right samples one after another
	TSF_STEREO_INTERLEAVED,
	// Two channels with all samples for the left channel first then right
	TSF_STEREO_UNWEAVED,
	// A single channel (stereo instruments are mixed into center)
	TSF_MONO
};

// Thread safety:
//
// 1. Rendering / voices:
//
// Your audio output which calls the tsf_render* functions will most likely
// run on a different thread than where the playback tsf_note* functions
// are called. In which case some sort of concurrency control like a
// mutex needs to be used so they are not called at the same time.
// Alternatively, you can pre-allocate a maximum number of voices that can
// play simultaneously by calling tsf_set_max_voices after loading.
// That way memory re-allocation will not happen during tsf_note_on and
// TSF should become mostly thread safe.
// There is a theoretical chance that ending notes would negatively influence
// a voice that is rendering at the time but it is hard to say.
// Also be aware, this has not been tested much.
//
// 2. Channels:
//
// Calls to tsf_channel_set_... functions may allocate new channels
// if no channel with that number was previously used. Make sure to
// create all channels at the beginning as required if you call tsf_render*
// from a different thread.

// Setup the parameters for the voice render methods
//   outputmode: if mono or stereo and how stereo channel data is ordered
//   samplerate: the number of samples per second (output frequency)
//   global_gain_db: volume gain in decibels (>0 means higher, <0 means lower)
TSFDEF void tsf_set_output(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db CPP_DEFAULT0);

// Set the global gain as a volume factor
//   global_gain: the desired volume where 1.0 is 100%
TSFDEF void tsf_set_volume(tsf* f, float global_gain);

// Set the maximum number of voices to play simultaneously
// Depending on the soundfond, one note can cause many new voices to be started,
// so don't keep this number too low or otherwise sounds may not play.
//   max_voices: maximum number to pre-allocate and set the limit to
//   (tsf_set_max_voices returns 0 if allocation failed, otherwise 1)
TSFDEF int tsf_set_max_voices(tsf* f, int max_voices);

// Start playing a note
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   bank: instrument bank number (alternative to preset_index)
//   preset_number: preset number (alternative to preset_index)
//   (tsf_note_on returns 0 if the allocation of a new voice failed, otherwise 1)
//   (tsf_bank_note_on returns 0 if preset does not exist or allocation failed, otherwise 1)
TSFDEF int tsf_note_on(tsf* f, int preset_index, int key, float vel);
TSFDEF int tsf_bank_note_on(tsf* f, int bank, int preset_number, int key, float vel);

// Stop playing a note
//   (bank_note_off returns 0 if preset does not exist, otherwise 1)
TSFDEF void tsf_note_off(tsf* f, int preset_index, int key);
TSFDEF int  tsf_bank_note_off(tsf* f, int bank, int preset_number, int key);

// Stop playing all notes (end with sustain and release)
TSFDEF void tsf_note_off_all(tsf* f);

// Returns the number of active voices
TSFDEF int tsf_active_voice_count(tsf* f);

// Render output samples into a buffer
// You can either render as signed 16-bit values (tsf_render_short) or
// as 32-bit float values (tsf_render_float)
//   buffer: target buffer of size samples * output_channels * sizeof(type)
//   samples: number of samples to render
//   flag_mixing: if 0 clear the buffer first, otherwise mix into existing data
TSFDEF void tsf_render_short(tsf* f, short* buffer, int samples, int flag_mixing CPP_DEFAULT0);
TSFDEF void tsf_render_float(tsf* f, float* buffer, int samples, int flag_mixing CPP_DEFAULT0);

// FatzAudio: like tsf_render_float, and also adds each voice to the reverb and
// chorus send buses (same layout as buffer) at its send levels: the region's
// effect-send generator plus CC91 / CC93 * 200 / 127 (the SoundFont 2.04 default
// modulators), at most 100%. Mono and stereo interleaved output only (stereo
// unweaved renders dry, like tsf_render_float). The buses are always added to.
// variation (may be NULL): a third bus, fed by CC94 * 200 / 127 (the GS delay and XG
// variation send) times the drum key's TSF_DRUM_VARIATION. insert (may be NULL): the
// voices of channel insertChannel go there, without sends, instead of to the others
// (an insertion effect's input; the caller sends its output on).
TSFDEF void tsf_render_float_fx(tsf* f, float* buffer, float* reverb, float* chorus, float* variation, float* insert, int insertChannel, int samples, int flag_mixing);

// Higher level channel based functions, set up channel parameters
//   channel: channel number
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   preset_number: preset number (alternative to preset_index)
//   flag_mididrums: 0 for normal channels, otherwise apply MIDI drum channel rules
//   bank: instrument bank number (alternative to preset_index)
//   pan: stereo panning value from 0.0 (left) to 1.0 (right) (default 0.5 center)
//   volume: linear volume scale factor (default 1.0 full)
//   pitch_wheel: pitch wheel position 0 to 16383 (default 8192 unpitched)
//   pitch_range: range of the pitch wheel in semitones (default 2.0, total +/- 2 semitones)
//   tuning: tuning of all playing voices in semitones (default 0.0, standard (A440) tuning)
//   flag_sustain: 0 to end notes that were held sustained and disable holding sustain otherwise enable it
//   (tsf_set_preset_number and set_bank_preset return 0 if preset does not exist, otherwise 1)
//   (tsf_channel_set_... return 0 if a new channel needed allocation and that failed, otherwise 1)
TSFDEF int tsf_channel_set_presetindex(tsf* f, int channel, int preset_index);
TSFDEF int tsf_channel_set_presetnumber(tsf* f, int channel, int preset_number, int flag_mididrums CPP_DEFAULT0);
TSFDEF int tsf_channel_set_bank(tsf* f, int channel, int bank);
TSFDEF int tsf_channel_set_bank_preset(tsf* f, int channel, int bank, int preset_number);
TSFDEF int tsf_channel_set_pan(tsf* f, int channel, float pan);
TSFDEF int tsf_channel_set_volume(tsf* f, int channel, float volume);
TSFDEF int tsf_channel_set_pitchwheel(tsf* f, int channel, int pitch_wheel);
TSFDEF int tsf_channel_set_pitchrange(tsf* f, int channel, float pitch_range);
TSFDEF int tsf_channel_set_tuning(tsf* f, int channel, float tuning);
TSFDEF void tsf_set_tuning(tsf* f, float tuning); // FatzAudio: master tuning in semitones, for every channel
TSFDEF int tsf_channel_set_sustain(tsf* f, int channel, int flag_sustain);
// FatzAudio: sostenuto (CC66: holds the notes whose keys are down when it is pressed),
// channel and polyphonic key pressure (0..127), and the vibrato depth in cents that full
// channel pressure adds (the SoundFont 2.04 default modulator's 50; 0 after a GS, XG or GM2 reset)
TSFDEF int tsf_channel_set_sostenuto(tsf* f, int channel, int flag_sostenuto);
TSFDEF int tsf_channel_set_pressure(tsf* f, int channel, int pressure);
TSFDEF int tsf_channel_set_key_pressure(tsf* f, int channel, int key, int pressure);
TSFDEF int tsf_channel_set_pressure_vibrato(tsf* f, int channel, float cents);
// FatzAudio: scale tuning (GS/XG): cents added to every C (note 0), C# (1) ... B (11) the
// channel plays, sounding notes included
TSFDEF int tsf_channel_set_scale_tuning(tsf* f, int channel, int note, float cents);
// FatzAudio: drum setup (GS drum maps, XG drum setups, the drum NRPNs). A synth holds
// TSF_DRUM_KITS edit tables of 128 keys; a channel playing drums reads one (`kit`, -1 none).
// Values are MIDI data bytes (0..127): pitch 64 = as is (coarse: semitones, fine: cents),
// level 127 = as is (0 silent), pan 1..127 replaces the soundfont's (0: the middle), sends
// 127 = as is, group 0 = none (1..127: an exclusive group of the kit's own), note off/on
// 0 = ignored, filter and envelope times 64 = as is (the CC71-78 steps). Edits take effect
// from the key's next note.
enum TSFDrumParam
{
	TSF_DRUM_PITCH_COARSE, TSF_DRUM_PITCH_FINE, TSF_DRUM_LEVEL, TSF_DRUM_PAN, TSF_DRUM_REVERB, TSF_DRUM_CHORUS,
	TSF_DRUM_GROUP, TSF_DRUM_NOTE_OFF, TSF_DRUM_NOTE_ON, TSF_DRUM_CUTOFF, TSF_DRUM_RESONANCE,
	TSF_DRUM_ATTACK, TSF_DRUM_DECAY, TSF_DRUM_RELEASE, TSF_DRUM_VARIATION, TSF_DRUM_PARAMS
};
#define TSF_DRUM_KITS 4
TSFDEF int tsf_channel_set_drum_kit(tsf* f, int channel, int kit);
TSFDEF int tsf_set_drum_param(tsf* f, int kit, int key, int param, int value);
TSFDEF void tsf_reset_drum_kit(tsf* f, int kit); // -1: every kit

// Start or stop playing notes on a channel (needs channel preset to be set)
//   channel: channel number
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   (tsf_channel_note_on returns 0 on allocation failure of new voice, otherwise 1)
TSFDEF int tsf_channel_note_on(tsf* f, int channel, int key, float vel);
TSFDEF void tsf_channel_note_off(tsf* f, int channel, int key);
TSFDEF void tsf_channel_note_off_all(tsf* f, int channel); //end with sustain and release
TSFDEF void tsf_channel_sounds_off_all(tsf* f, int channel); //end immediately

// Apply a MIDI control change to the channel (not all controllers are supported!)
// FatzAudio: also CC1, CC5, CC65-67, CC71-78, CC84, NRPN 01 xx (GS/XG part sound controllers),
// NRPN 14-1F rr (GS/XG drum key edits, on a channel with a drum kit), RPN 5
// and any controller a soundfont modulator reads
//    (tsf_channel_midi_control returns 0 on allocation failure of new channel, otherwise 1)
TSFDEF int tsf_channel_midi_control(tsf* f, int channel, int controller, int control_value);

// Get current values set on the channels
TSFDEF int tsf_channel_get_preset_index(tsf* f, int channel);
TSFDEF int tsf_channel_get_preset_bank(tsf* f, int channel);
TSFDEF int tsf_channel_get_preset_number(tsf* f, int channel);
TSFDEF float tsf_channel_get_pan(tsf* f, int channel);
TSFDEF float tsf_channel_get_volume(tsf* f, int channel);
TSFDEF int tsf_channel_get_pitchwheel(tsf* f, int channel);
TSFDEF float tsf_channel_get_pitchrange(tsf* f, int channel);
TSFDEF float tsf_channel_get_tuning(tsf* f, int channel);
TSFDEF int tsf_channel_get_midi_control(tsf* f, int channel, int controller); // FatzAudio: last value (0..127)

#ifdef __cplusplus
#  undef CPP_DEFAULT0
}
#endif

// end header
// ---------------------------------------------------------------------------------------------------------
#endif //TSF_INCLUDE_TSF_INL

#ifdef TSF_IMPLEMENTATION
#undef TSF_IMPLEMENTATION

// The lower this block size is the more accurate the effects are.
// Increasing the value significantly lowers the CPU usage of the voice rendering.
// If LFO affects the low-pass filter it can be hearable even as low as 8.
#ifndef TSF_RENDER_EFFECTSAMPLEBLOCK
#define TSF_RENDER_EFFECTSAMPLEBLOCK 64
#endif

// When using tsf_render_short, to do the conversion a buffer of a fixed size is
// allocated on the stack. On low memory platforms this could be made smaller.
// Increasing this above 512 should not have a significant impact on performance.
// The value should be a multiple of TSF_RENDER_EFFECTSAMPLEBLOCK.
#ifndef TSF_RENDER_SHORTBUFFERBLOCK
#define TSF_RENDER_SHORTBUFFERBLOCK 512
#endif

// Grace release time for quick voice off (avoid clicking noise)
#define TSF_FASTRELEASETIME 0.01f

#if !defined(TSF_MALLOC) || !defined(TSF_FREE) || !defined(TSF_REALLOC)
#  include <stdlib.h>
#  define TSF_MALLOC  malloc
#  define TSF_FREE    free
#  define TSF_REALLOC realloc
#endif

#if !defined(TSF_MEMCPY) || !defined(TSF_MEMSET)
#  include <string.h>
#  define TSF_MEMCPY  memcpy
#  define TSF_MEMSET  memset
#endif

#if !defined(TSF_POW) || !defined(TSF_POWF) || !defined(TSF_EXPF) || !defined(TSF_LOG) || !defined(TSF_TAN) || !defined(TSF_LOG10) || !defined(TSF_SQRT)
#  include <math.h>
#  if !defined(__cplusplus) && !defined(NAN) && !defined(powf) && !defined(expf) && !defined(sqrtf)
#    define powf (float)pow // deal with old math.h
#    define expf (float)exp // files that come without
#    define sqrtf (float)sqrt // powf, expf and sqrtf
#  endif
#  define TSF_POW     pow
#  define TSF_POWF    powf
#  define TSF_EXPF    expf
#  define TSF_LOG     log
#  define TSF_TAN     tan
#  define TSF_LOG10   log10
#  define TSF_SQRTF   sqrtf
#endif

#ifndef TSF_NO_STDIO
#  include <stdio.h>
#endif

#include <stddef.h> // FatzAudio: offsetof

#define TSF_TRUE 1
#define TSF_FALSE 0
#define TSF_BOOL unsigned char
#define TSF_PI 3.14159265358979323846264338327950288
#define TSF_NULL 0

#ifdef __cplusplus
extern "C" {
#endif

typedef char tsf_fourcc[4];
typedef signed char tsf_s8;
typedef unsigned char tsf_u8;
typedef unsigned short tsf_u16;
typedef signed short tsf_s16;
typedef unsigned int tsf_u32;
typedef char tsf_char20[20];

#define TSF_FourCCEquals(value1, value2) (value1[0] == value2[0] && value1[1] == value2[1] && value1[2] == value2[2] && value1[3] == value2[3])

struct tsf
{
	struct tsf_preset* presets;
	float* fontSamples;
	struct tsf_voice* voices;
	struct tsf_channels* channels;

	int presetNum;
	int voiceNum;
	int maxVoiceNum;
	unsigned int voicePlayIndex;

	enum TSFOutputMode outputmode;
	float outSampleRate;
	float globalGainDB;
	int* refCount;
	float globalTuning; // FatzAudio: semitones added to every channel (master tuning)
	struct tsf_modulator* modulators; // FatzAudio: every region's modulator list, shared by copies
	int modulatorNum;
	tsf_u32 modCC[4];   // FatzAudio: controllers some modulator reads (bit n = CC n)
	tsf_u32 modGC;      // FatzAudio: general controllers some modulator reads (bit n = source n)
	struct tsf_drumkey* drumKeys; // FatzAudio: TSF_DRUM_KITS * 128 drum key edits, of this synth only (NULL: none yet)
};

#ifndef TSF_NO_STDIO
static int tsf_stream_stdio_read(FILE* f, void* ptr, unsigned int size) { return (int)fread(ptr, 1, size, f); }
static int tsf_stream_stdio_skip(FILE* f, unsigned int count) { return !fseek(f, count, SEEK_CUR); }
TSFDEF tsf* tsf_load_filename(const char* filename)
{
	tsf* res;
	struct tsf_stream stream = { TSF_NULL, (int(*)(void*,void*,unsigned int))&tsf_stream_stdio_read, (int(*)(void*,unsigned int))&tsf_stream_stdio_skip };
	#if __STDC_WANT_SECURE_LIB__
	FILE* f = TSF_NULL; fopen_s(&f, filename, "rb");
	#else
	FILE* f = fopen(filename, "rb");
	#endif
	if (!f)
	{
		//if (e) *e = TSF_FILENOTFOUND;
		return TSF_NULL;
	}
	stream.data = f;
	res = tsf_load(&stream);
	fclose(f);
	return res;
}
#endif

struct tsf_stream_memory { const char* buffer; unsigned int total, pos; };
static int tsf_stream_memory_read(struct tsf_stream_memory* m, void* ptr, unsigned int size) { if (size > m->total - m->pos) size = m->total - m->pos; TSF_MEMCPY(ptr, m->buffer+m->pos, size); m->pos += size; return size; }
static int tsf_stream_memory_skip(struct tsf_stream_memory* m, unsigned int count) { if (m->pos + count > m->total) return 0; m->pos += count; return 1; }
static tsf* tsf_load_sized(struct tsf_stream* stream, tsf_u32 total);
TSFDEF tsf* tsf_load_memory(const void* buffer, int size)
{
	struct tsf_stream stream = { TSF_NULL, (int(*)(void*,void*,unsigned int))&tsf_stream_memory_read, (int(*)(void*,unsigned int))&tsf_stream_memory_skip };
	struct tsf_stream_memory f = { 0, 0, 0 };
	if (!buffer || size < 12) return TSF_NULL; // FatzAudio
	f.buffer = (const char*)buffer;
	f.total = size;
	stream.data = &f;
	return tsf_load_sized(&stream, (tsf_u32)size); // FatzAudio: chunk sizes are checked against the real size
}

enum { TSF_LOOPMODE_NONE, TSF_LOOPMODE_CONTINUOUS, TSF_LOOPMODE_SUSTAIN };

enum { TSF_SEGMENT_NONE, TSF_SEGMENT_DELAY, TSF_SEGMENT_ATTACK, TSF_SEGMENT_HOLD, TSF_SEGMENT_DECAY, TSF_SEGMENT_SUSTAIN, TSF_SEGMENT_RELEASE, TSF_SEGMENT_DONE };

struct tsf_hydra
{
	struct tsf_hydra_phdr *phdrs; struct tsf_hydra_pbag *pbags; struct tsf_hydra_pmod *pmods;
	struct tsf_hydra_pgen *pgens; struct tsf_hydra_inst *insts; struct tsf_hydra_ibag *ibags;
	struct tsf_hydra_imod *imods; struct tsf_hydra_igen *igens; struct tsf_hydra_shdr *shdrs;
	int phdrNum, pbagNum, pmodNum, pgenNum, instNum, ibagNum, imodNum, igenNum, shdrNum;
};

union tsf_hydra_genamount { struct { tsf_u8 lo, hi; } range; tsf_s16 shortAmount; tsf_u16 wordAmount; };
struct tsf_hydra_phdr { tsf_char20 presetName; tsf_u16 preset, bank, presetBagNdx; tsf_u32 library, genre, morphology; };
struct tsf_hydra_pbag { tsf_u16 genNdx, modNdx; };
struct tsf_hydra_pmod { tsf_u16 modSrcOper, modDestOper; tsf_s16 modAmount; tsf_u16 modAmtSrcOper, modTransOper; };
struct tsf_hydra_pgen { tsf_u16 genOper; union tsf_hydra_genamount genAmount; };
struct tsf_hydra_inst { tsf_char20 instName; tsf_u16 instBagNdx; };
struct tsf_hydra_ibag { tsf_u16 instGenNdx, instModNdx; };
struct tsf_hydra_imod { tsf_u16 modSrcOper, modDestOper; tsf_s16 modAmount; tsf_u16 modAmtSrcOper, modTransOper; };
struct tsf_hydra_igen { tsf_u16 genOper; union tsf_hydra_genamount genAmount; };
struct tsf_hydra_shdr { tsf_char20 sampleName; tsf_u32 start, end, startLoop, endLoop, sampleRate; tsf_u8 originalPitch; tsf_s8 pitchCorrection; tsf_u16 sampleLink, sampleType; };

#define TSFR(FIELD) stream->read(stream->data, &i->FIELD, sizeof(i->FIELD));
static void tsf_hydra_read_phdr(struct tsf_hydra_phdr* i, struct tsf_stream* stream) { TSFR(presetName) TSFR(preset) TSFR(bank) TSFR(presetBagNdx) TSFR(library) TSFR(genre) TSFR(morphology) }
static void tsf_hydra_read_pbag(struct tsf_hydra_pbag* i, struct tsf_stream* stream) { TSFR(genNdx) TSFR(modNdx) }
static void tsf_hydra_read_pmod(struct tsf_hydra_pmod* i, struct tsf_stream* stream) { TSFR(modSrcOper) TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper) }
static void tsf_hydra_read_pgen(struct tsf_hydra_pgen* i, struct tsf_stream* stream) { TSFR(genOper) TSFR(genAmount) }
static void tsf_hydra_read_inst(struct tsf_hydra_inst* i, struct tsf_stream* stream) { TSFR(instName) TSFR(instBagNdx) }
static void tsf_hydra_read_ibag(struct tsf_hydra_ibag* i, struct tsf_stream* stream) { TSFR(instGenNdx) TSFR(instModNdx) }
static void tsf_hydra_read_imod(struct tsf_hydra_imod* i, struct tsf_stream* stream) { TSFR(modSrcOper) TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper) }
static void tsf_hydra_read_igen(struct tsf_hydra_igen* i, struct tsf_stream* stream) { TSFR(genOper) TSFR(genAmount) }
static void tsf_hydra_read_shdr(struct tsf_hydra_shdr* i, struct tsf_stream* stream) { TSFR(sampleName) TSFR(start) TSFR(end) TSFR(startLoop) TSFR(endLoop) TSFR(sampleRate) TSFR(originalPitch) TSFR(pitchCorrection) TSFR(sampleLink) TSFR(sampleType) }
#undef TSFR

struct tsf_riffchunk { tsf_fourcc id; tsf_u32 size; };
struct tsf_envelope { float delay, attack, hold, decay, sustain, release, keynumToHold, keynumToDecay; };
struct tsf_voice_envelope { unsigned char segment, segmentIsExponential : 1, isAmpEnv : 1; short midiVelocity; float level, slope; int samplesUntilNextSegment; struct tsf_envelope parameters; };
struct tsf_voice_lowpass { double QInv, a0, a1, b1, b2, z1, z2; TSF_BOOL active; };
struct tsf_voice_lfo { int samplesUntil; float level, delta; };

struct tsf_region
{
	int loop_mode;
	unsigned int sample_rate;
	unsigned char lokey, hikey, lovel, hivel;
	unsigned int group, offset, end, loop_start, loop_end;
	int transpose, tune, pitch_keycenter, pitch_keytrack;
	float attenuation, pan;
	struct tsf_envelope ampenv, modenv;
	int initialFilterQ, initialFilterFc;
	int modEnvToPitch, modEnvToFilterFc, modLfoToFilterFc, modLfoToVolume;
	float delayModLFO;
	int freqModLFO, modLfoToPitch;
	float delayVibLFO;
	int freqVibLFO, vibLfoToPitch;
	float chorusSend, reverbSend; // FatzAudio: effect sends, 0.1% units (generators 15 and 16)
	int modStart, modNum; // FatzAudio: the region's modulators in tsf::modulators
};

// FatzAudio: a SoundFont modulator, ready to evaluate: sources as in the file (SF2 2.04
// section 8.2), the destination as a TSF_MOD_* slot, the amount in the generator's units.
struct tsf_modulator { tsf_u16 src, amtSrc; unsigned char slot, absolute, kind; float amount; };

struct tsf_preset
{
	tsf_char20 presetName;
	tsf_u16 preset, bank;
	struct tsf_region* regions;
	int regionNum;
};

struct tsf_voice
{
	int playingPreset, playingKey, playingChannel, heldSustain;
	struct tsf_region* region;
	double pitchInputTimecents, pitchOutputFactor;
	double sourceSamplePosition;
	float  noteGainDB, panFactorLeft, panFactorRight;
	unsigned int playIndex, loopStart, loopEnd;
	struct tsf_voice_envelope ampenv, modenv;
	struct tsf_voice_lowpass lowpass;
	struct tsf_voice_lfo modlfo, viblfo;
	// FatzAudio: the region's values with the modulators and sound controllers applied
	// (generator units), and what they change on top of the region: gain (dB), pitch
	// (cents), pan and effect sends (0.1%); a portamento glide (cents, fading to 0 over
	// portaTotal samples); held by sostenuto; struck with the soft pedal down
	float vibLfoToPitch, modLfoToPitch, modEnvToPitch, filterFc, filterQ, modLfoToFilterFc, modEnvToFilterFc, modLfoToVolume;
	float freqVibLFO, freqModLFO, modGainDB, modPitch, modPan, modReverb, modChorus;
	float portaCents; int portaLeft, portaTotal;
	float envMod[15]; // the envelope offsets last applied: both envelopes' 6 slots, the sound controllers' 3 times
	unsigned char sostenuto, soft;
	// FatzAudio: scale tuning and drum key edits: pitch (cents), the pan the channel's is added
	// to (the region's unless edited), effect send factors, filter and envelope offsets (generator
	// units), the exclusive group (the region's, or 0x10000 + kit * 128 + an edited group) and
	// whether the key was set not to receive note-offs when the note started
	unsigned char drumHold;
	float scalePitch, drumPitch, basePan, sendReverb, sendChorus, sendVariation, drumFc, drumQ, drumAttack, drumDecay, drumRelease;
	unsigned int group;
};

struct tsf_channel
{
	unsigned short presetIndex, bank, pitchWheel, midiPan, midiVolume, midiExpression, midiRPN, midiData : 14, sustain : 1;
	float panOffset, gainDB, pitchRange, tuning;
	unsigned char midiReverb, midiChorus; // FatzAudio: CC91 and CC93
	// FatzAudio: every controller's last value, pressure, NRPN select (0xFFFF none),
	// sostenuto, modulation depth range (RPN 5) and pressure vibrato in cents, the
	// last key struck and where the next note glides from (portamento; -1 none)
	unsigned char cc[128], keyPressure[128], chanPressure, sostenuto;
	unsigned short midiNRPN;
	float modRange, pressureVibrato;
	short lastKey, portaKey, portaFrom;
	float scaleTune[12]; // FatzAudio: scale tuning, cents per note of the octave
	signed char drumKit; // FatzAudio: the drum key edits the channel plays with (-1 none)
};

// FatzAudio: a drum key's edits, as the MIDI values tsf_set_drum_param takes
struct tsf_drumkey { unsigned char edited, param[TSF_DRUM_PARAMS]; };
static const unsigned char tsf_drumkey_defaults[TSF_DRUM_PARAMS] = { 64, 64, 127, 0, 127, 127, 0xFF, 1, 1, 64, 64, 64, 64, 64, 127 }; // pan 0, group 0xFF: the soundfont's

struct tsf_channels
{
	void (*setupVoice)(tsf* f, struct tsf_voice* voice);
	int channelNum, activeChannel;
	struct tsf_channel channels[1];
};

static double tsf_timecents2Secsd(double timecents) { return TSF_POW(2.0, timecents / 1200.0); }
static float tsf_timecents2Secsf(float timecents) { return TSF_POWF(2.0f, timecents / 1200.0f); }
static float tsf_cents2Hertz(float cents) { return 8.176f * TSF_POWF(2.0f, cents / 1200.0f); }
static float tsf_decibelsToGain(float db) { return (db > -100.f ? TSF_POWF(10.0f, db * 0.05f) : 0); }
static float tsf_gainToDecibels(float gain) { return (gain <= .00001f ? -100.f : (float)(20.0 * TSF_LOG10(gain))); }

static TSF_BOOL tsf_riffchunk_read(struct tsf_riffchunk* parent, struct tsf_riffchunk* chunk, struct tsf_stream* stream)
{
	TSF_BOOL IsRiff, IsList;
	if (parent && sizeof(tsf_fourcc) + sizeof(tsf_u32) > parent->size) return TSF_FALSE;
	if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) || *chunk->id <= ' ' || *chunk->id >= 'z') return TSF_FALSE;
	if (!stream->read(stream->data, &chunk->size, sizeof(tsf_u32))) return TSF_FALSE;
	if (parent && chunk->size > parent->size - (sizeof(tsf_fourcc) + sizeof(tsf_u32))) return TSF_FALSE; // FatzAudio: no overflow
	if (parent) parent->size -= sizeof(tsf_fourcc) + sizeof(tsf_u32) + chunk->size;
	IsRiff = TSF_FourCCEquals(chunk->id, "RIFF"), IsList = TSF_FourCCEquals(chunk->id, "LIST");
	if (IsRiff && parent) return TSF_FALSE; //not allowed
	if (!IsRiff && !IsList) return TSF_TRUE; //custom type without sub type
	if (chunk->size < sizeof(tsf_fourcc)) return TSF_FALSE; // FatzAudio: the size must hold the sub type
	if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) || *chunk->id <= ' ' || *chunk->id >= 'z') return TSF_FALSE;
	chunk->size -= sizeof(tsf_fourcc);
	return TSF_TRUE;
}

static void tsf_region_clear(struct tsf_region* i, TSF_BOOL for_relative)
{
	TSF_MEMSET(i, 0, sizeof(struct tsf_region));
	i->hikey = i->hivel = 127;
	i->pitch_keycenter = 60; // C4
	if (for_relative) return;

	i->pitch_keytrack = 100;

	i->pitch_keycenter = -1;

	// SF2 defaults in timecents.
	i->ampenv.delay = i->ampenv.attack = i->ampenv.hold = i->ampenv.decay = i->ampenv.release = -12000.0f;
	i->modenv.delay = i->modenv.attack = i->modenv.hold = i->modenv.decay = i->modenv.release = -12000.0f;

	i->initialFilterFc = 13500;

	i->delayModLFO = -12000.0f;
	i->delayVibLFO = -12000.0f;
}

static void tsf_region_operator(struct tsf_region* region, tsf_u16 genOper, union tsf_hydra_genamount* amount, struct tsf_region* merge_region)
{
	enum
	{
		_GEN_TYPE_MASK       = 0x0F,
		GEN_FLOAT            = 0x01,
		GEN_INT              = 0x02,
		GEN_UINT_ADD         = 0x03,
		GEN_UINT_ADD15       = 0x04,
		GEN_KEYRANGE         = 0x05,
		GEN_VELRANGE         = 0x06,
		GEN_LOOPMODE         = 0x07,
		GEN_GROUP            = 0x08,
		GEN_KEYCENTER        = 0x09,

		_GEN_LIMIT_MASK      = 0xF0,
		GEN_INT_LIMIT12K     = 0x10, //min -12000, max 12000
		GEN_INT_LIMITFC      = 0x20, //min 1500, max 13500
		GEN_INT_LIMITQ       = 0x30, //min 0, max 960
		GEN_INT_LIMIT960     = 0x40, //min -960, max 960
		GEN_INT_LIMIT16K4500 = 0x50, //min -16000, max 4500
		GEN_FLOAT_LIMIT12K5K = 0x60, //min -12000, max 5000
		GEN_FLOAT_LIMIT12K8K = 0x70, //min -12000, max 8000
		GEN_FLOAT_LIMIT1200  = 0x80, //min -1200, max 1200
		GEN_FLOAT_LIMITPAN   = 0x90, //* .001f, min -.5f, max .5f,
		GEN_FLOAT_LIMITATTN  = 0xA0, //* .1f, min 0, max 144.0
		GEN_FLOAT_MAX1000    = 0xB0, //min 0, max 1000
		GEN_FLOAT_MAX1440    = 0xC0, //min 0, max 1440

		_GEN_MAX = 59
	};
	// FatzAudio: offsetof instead of pointer arithmetic on a null pointer (undefined behaviour)
	#define _TSFREGIONOFFSET(TYPE, FIELD) (unsigned char)(offsetof(struct tsf_region, FIELD) / sizeof(TYPE))
	#define _TSFREGIONENVOFFSET(TYPE, ENV, FIELD) (unsigned char)((offsetof(struct tsf_region, ENV) + offsetof(struct tsf_envelope, FIELD)) / sizeof(TYPE))
	static const struct { unsigned char mode, offset; } genMetas[_GEN_MAX] =
	{
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, offset               ) }, // 0 StartAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, end                  ) }, // 1 EndAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, loop_start           ) }, // 2 StartloopAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, loop_end             ) }, // 3 EndloopAddrsOffset
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, offset               ) }, // 4 StartAddrsCoarseOffset
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modLfoToPitch        ) }, // 5 ModLfoToPitch
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, vibLfoToPitch        ) }, // 6 VibLfoToPitch
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modEnvToPitch        ) }, // 7 ModEnvToPitch
		{ GEN_INT   | GEN_INT_LIMITFC      , _TSFREGIONOFFSET(         int, initialFilterFc      ) }, // 8 InitialFilterFc
		{ GEN_INT   | GEN_INT_LIMITQ       , _TSFREGIONOFFSET(         int, initialFilterQ       ) }, // 9 InitialFilterQ
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modLfoToFilterFc     ) }, //10 ModLfoToFilterFc
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modEnvToFilterFc     ) }, //11 ModEnvToFilterFc
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, end                  ) }, //12 EndAddrsCoarseOffset
		{ GEN_INT   | GEN_INT_LIMIT960     , _TSFREGIONOFFSET(         int, modLfoToVolume       ) }, //13 ModLfoToVolume
		{ 0                                , (0                                                  ) }, //   Unused
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONOFFSET(       float, chorusSend           ) }, //15 ChorusEffectsSend (FatzAudio)
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONOFFSET(       float, reverbSend           ) }, //16 ReverbEffectsSend (FatzAudio)
		{ GEN_FLOAT | GEN_FLOAT_LIMITPAN   , _TSFREGIONOFFSET(       float, pan                  ) }, //17 Pan
		{ 0                                , (0                                                  ) }, //   Unused
		{ 0                                , (0                                                  ) }, //   Unused
		{ 0                                , (0                                                  ) }, //   Unused
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONOFFSET(       float, delayModLFO          ) }, //21 DelayModLFO
		{ GEN_INT   | GEN_INT_LIMIT16K4500 , _TSFREGIONOFFSET(         int, freqModLFO           ) }, //22 FreqModLFO
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONOFFSET(       float, delayVibLFO          ) }, //23 DelayVibLFO
		{ GEN_INT   | GEN_INT_LIMIT16K4500 , _TSFREGIONOFFSET(         int, freqVibLFO           ) }, //24 FreqVibLFO
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, modenv, delay        ) }, //25 DelayModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, attack       ) }, //26 AttackModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, modenv, hold         ) }, //27 HoldModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, decay        ) }, //28 DecayModEnv
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONENVOFFSET(    float, modenv, sustain      ) }, //29 SustainModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, release      ) }, //30 ReleaseModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, modenv, keynumToHold ) }, //31 KeynumToModEnvHold
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, modenv, keynumToDecay) }, //32 KeynumToModEnvDecay
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, ampenv, delay        ) }, //33 DelayVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, attack       ) }, //34 AttackVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, ampenv, hold         ) }, //35 HoldVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, decay        ) }, //36 DecayVolEnv
		{ GEN_FLOAT | GEN_FLOAT_MAX1440    , _TSFREGIONENVOFFSET(    float, ampenv, sustain      ) }, //37 SustainVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, release      ) }, //38 ReleaseVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, ampenv, keynumToHold ) }, //39 KeynumToVolEnvHold
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, ampenv, keynumToDecay) }, //40 KeynumToVolEnvDecay
		{ 0                                , (0                                                  ) }, //   Instrument (special)
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_KEYRANGE                     , (0                                                  ) }, //43 KeyRange
		{ GEN_VELRANGE                     , (0                                                  ) }, //44 VelRange
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, loop_start           ) }, //45 StartloopAddrsCoarseOffset
		{ 0                                , (0                                                  ) }, //46 Keynum (special)
		{ 0                                , (0                                                  ) }, //47 Velocity (special)
		{ GEN_FLOAT | GEN_FLOAT_LIMITATTN  , _TSFREGIONOFFSET(       float, attenuation          ) }, //48 InitialAttenuation
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, loop_end             ) }, //50 EndloopAddrsCoarseOffset
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, transpose            ) }, //51 CoarseTune
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, tune                 ) }, //52 FineTune
		{ 0                                , (0                                                  ) }, //   SampleID (special)
		{ GEN_LOOPMODE                     , _TSFREGIONOFFSET(         int, loop_mode            ) }, //54 SampleModes
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, pitch_keytrack       ) }, //56 ScaleTuning
		{ GEN_GROUP                        , _TSFREGIONOFFSET(unsigned int, group                ) }, //57 ExclusiveClass
		{ GEN_KEYCENTER                    , _TSFREGIONOFFSET(         int, pitch_keycenter      ) }, //58 OverridingRootKey
	};
	#undef _TSFREGIONOFFSET
	#undef _TSFREGIONENVOFFSET
	if (amount)
	{
		int offset;
		if (genOper >= _GEN_MAX) return;
		offset = genMetas[genOper].offset;
		switch (genMetas[genOper].mode & _GEN_TYPE_MASK)
		{
			case GEN_FLOAT:      ((       float*)region)[offset]  = amount->shortAmount;     return;
			case GEN_INT:        ((         int*)region)[offset]  = amount->shortAmount;     return;
			case GEN_UINT_ADD:   ((unsigned int*)region)[offset] += amount->shortAmount;     return;
			case GEN_UINT_ADD15: ((unsigned int*)region)[offset] += (unsigned int)amount->shortAmount * 32768u; return; // FatzAudio: no negative shift
			case GEN_KEYRANGE:   region->lokey = amount->range.lo; region->hikey = amount->range.hi; return;
			case GEN_VELRANGE:   region->lovel = amount->range.lo; region->hivel = amount->range.hi; return;
			case GEN_LOOPMODE:   region->loop_mode       = ((amount->wordAmount&3) == 3 ? TSF_LOOPMODE_SUSTAIN : ((amount->wordAmount&3) == 1 ? TSF_LOOPMODE_CONTINUOUS : TSF_LOOPMODE_NONE)); return;
			case GEN_GROUP:      region->group           = amount->wordAmount;  return;
			case GEN_KEYCENTER:  region->pitch_keycenter = amount->shortAmount; return;
		}
	}
	else //merge regions and clamp values
	{
		for (genOper = 0; genOper != _GEN_MAX; genOper++)
		{
			int offset = genMetas[genOper].offset;
			switch (genMetas[genOper].mode & _GEN_TYPE_MASK)
			{
				case GEN_FLOAT:
				{
					float *val = &((float*)region)[offset], vfactor, vmin, vmax;
					*val += ((float*)merge_region)[offset];
					switch (genMetas[genOper].mode & _GEN_LIMIT_MASK)
					{
						case GEN_FLOAT_LIMIT12K5K: vfactor =   1.0f; vmin = -12000.0f; vmax = 5000.0f; break;
						case GEN_FLOAT_LIMIT12K8K: vfactor =   1.0f; vmin = -12000.0f; vmax = 8000.0f; break;
						case GEN_FLOAT_LIMIT1200:  vfactor =   1.0f; vmin =  -1200.0f; vmax = 1200.0f; break;
						case GEN_FLOAT_LIMITPAN:   vfactor = 0.001f; vmin =     -0.5f; vmax =    0.5f; break;
						case GEN_FLOAT_LIMITATTN:  vfactor =   0.1f; vmin =      0.0f; vmax =  144.0f; break; // FatzAudio: centibels to dB per the SF2 spec (was 0.01, at most 14.4 dB)
						case GEN_FLOAT_MAX1000:    vfactor =   1.0f; vmin =      0.0f; vmax = 1000.0f; break;
						case GEN_FLOAT_MAX1440:    vfactor =   1.0f; vmin =      0.0f; vmax = 1440.0f; break;
						default: continue;
					}
					*val *= vfactor;
					if      (*val < vmin) *val = vmin;
					else if (*val > vmax) *val = vmax;
					continue;
				}
				case GEN_INT:
				{
					int *val = &((int*)region)[offset], vmin, vmax;
					*val += ((int*)merge_region)[offset];
					switch (genMetas[genOper].mode & _GEN_LIMIT_MASK)
					{
						case GEN_INT_LIMIT12K:     vmin = -12000; vmax = 12000; break;
						case GEN_INT_LIMITFC:      vmin =   1500; vmax = 13500; break;
						case GEN_INT_LIMITQ:       vmin =      0; vmax =   960; break;
						case GEN_INT_LIMIT960:     vmin =   -960; vmax =   960; break;
						case GEN_INT_LIMIT16K4500: vmin = -16000; vmax =  4500; break;
						default: continue;
					}
					if      (*val < vmin) *val = vmin;
					else if (*val > vmax) *val = vmax;
					continue;
				}
				case GEN_UINT_ADD:
				{
					((unsigned int*)region)[offset] += ((unsigned int*)merge_region)[offset];
					continue;
				}
			}
		}
	}
}

static void tsf_region_envtosecs(struct tsf_envelope* p, TSF_BOOL sustainIsGain)
{
	// EG times need to be converted from timecents to seconds.
	// Pin very short EG segments.  Timecents don't get to zero, and our EG is
	// happier with zero values.
	p->delay   = (p->delay   < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->delay));
	p->attack  = (p->attack  < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->attack));
	p->release = (p->release < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->release));

	// If we have dynamic hold or decay times depending on key number we need
	// to keep the values in timecents so we can calculate it during startNote
	if (!p->keynumToHold)  p->hold  = (p->hold  < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->hold));
	if (!p->keynumToDecay) p->decay = (p->decay < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->decay));

	if (p->sustain < 0.0f) p->sustain = 0.0f;
	else if (sustainIsGain) p->sustain = tsf_decibelsToGain(-p->sustain / 10.0f);
	else p->sustain = 1.0f - (p->sustain / 1000.0f);
}

// FatzAudio: presets are sorted with qsort (bank, preset, file order) instead of an
// O(n^2) count, and the hydra's indices are validated before use.
static int tsf_phdr_compare(const void* a, const void* b)
{
	const struct tsf_hydra_phdr *x = *(const struct tsf_hydra_phdr* const*)a, *y = *(const struct tsf_hydra_phdr* const*)b;
	if (x->bank != y->bank) return (x->bank < y->bank ? -1 : 1);
	if (x->preset != y->preset) return (x->preset < y->preset ? -1 : 1);
	return (x < y ? -1 : (x > y ? 1 : 0));
}

static int tsf_hydra_valid(const struct tsf_hydra* h)
{
	int i;
	if (h->phdrNum < 2 || h->pbagNum < 1 || h->pgenNum < 1 || h->instNum < 2 || h->ibagNum < 1 || h->igenNum < 1 || h->shdrNum < 1) return 0;
	for (i = 0; i < h->phdrNum - 1; i++)
		if (h->phdrs[i].presetBagNdx > h->phdrs[i + 1].presetBagNdx || h->phdrs[i + 1].presetBagNdx > h->pbagNum - 1) return 0;
	for (i = 0; i < h->pbagNum - 1; i++)
		if (h->pbags[i].genNdx > h->pbags[i + 1].genNdx || h->pbags[i + 1].genNdx > h->pgenNum) return 0;
	for (i = 0; i < h->instNum - 1; i++)
		if (h->insts[i].instBagNdx > h->insts[i + 1].instBagNdx || h->insts[i + 1].instBagNdx > h->ibagNum - 1) return 0;
	for (i = 0; i < h->ibagNum - 1; i++)
		if (h->ibags[i].instGenNdx > h->ibags[i + 1].instGenNdx || h->ibags[i + 1].instGenNdx > h->igenNum) return 0;
	return 1;
}

#ifndef TSF_MAX_REGIONS
#define TSF_MAX_REGIONS 0x400000 // FatzAudio: refuse soundfonts that would need more regions in total
#endif

// FatzAudio: modulators (SoundFont 2.04 sections 8.2 to 8.5 and 9.5). Each destination
// generator TinySoundFont models gets a slot; a voice sums its modulators' outputs per
// slot (tsf_voice_modulate). Linked modulators and other destinations are left out.
enum
{
	TSF_MOD_MODLFOTOPITCH, TSF_MOD_VIBLFOTOPITCH, TSF_MOD_MODENVTOPITCH, TSF_MOD_FILTERFC, TSF_MOD_FILTERQ,
	TSF_MOD_MODLFOTOFILTERFC, TSF_MOD_MODENVTOFILTERFC, TSF_MOD_MODLFOTOVOLUME, TSF_MOD_CHORUS, TSF_MOD_REVERB, TSF_MOD_PAN,
	TSF_MOD_DELAYMODLFO, TSF_MOD_FREQMODLFO, TSF_MOD_DELAYVIBLFO, TSF_MOD_FREQVIBLFO,
	TSF_MOD_MODENV,                     // delay, attack, hold, decay, sustain, release
	TSF_MOD_AMPENV = TSF_MOD_MODENV + 6, // the same for the volume envelope
	TSF_MOD_ATTENUATION = TSF_MOD_AMPENV + 6, TSF_MOD_COARSETUNE, TSF_MOD_FINETUNE,
	TSF_MOD_SLOTS,
	TSF_MOD_NONE = 0xFF
};
// The two default vibrato modulators scale with the channel's modulation depth range
// (RPN 5) and pressure vibrato; a soundfont modulator that replaces one is used as it is.
enum { TSF_MODKIND_FILE, TSF_MODKIND_MODWHEEL, TSF_MODKIND_PRESSURE };
#define TSF_MAX_ZONE_MODS 64        // per region (instrument and preset level each); more are ignored
#define TSF_MAX_MODULATORS 0x100000 // per soundfont; regions past it get the default modulators only
#define TSF_DEFAULT_MODS 2

static unsigned char tsf_mod_slot(tsf_u16 gen)
{
	switch (gen)
	{
		case  5: return TSF_MOD_MODLFOTOPITCH;    case  6: return TSF_MOD_VIBLFOTOPITCH;  case  7: return TSF_MOD_MODENVTOPITCH;
		case  8: return TSF_MOD_FILTERFC;         case  9: return TSF_MOD_FILTERQ;        case 10: return TSF_MOD_MODLFOTOFILTERFC;
		case 11: return TSF_MOD_MODENVTOFILTERFC; case 13: return TSF_MOD_MODLFOTOVOLUME; case 15: return TSF_MOD_CHORUS;
		case 16: return TSF_MOD_REVERB;           case 17: return TSF_MOD_PAN;            case 21: return TSF_MOD_DELAYMODLFO;
		case 22: return TSF_MOD_FREQMODLFO;       case 23: return TSF_MOD_DELAYVIBLFO;    case 24: return TSF_MOD_FREQVIBLFO;
		case 48: return TSF_MOD_ATTENUATION;      case 51: return TSF_MOD_COARSETUNE;     case 52: return TSF_MOD_FINETUNE;
	}
	if (gen >= 25 && gen <= 30) return (unsigned char)(TSF_MOD_MODENV + (gen - 25));
	if (gen >= 33 && gen <= 38) return (unsigned char)(TSF_MOD_AMPENV + (gen - 33));
	return TSF_MOD_NONE;
}

// A source the synth feeds (8.2.1): a known curve, and a general controller (velocity, key,
// poly and channel pressure, pitch wheel and its range) or a MIDI CC the spec allows.
// "No controller" is 1 as the amount source and makes a primary source unusable.
static int tsf_mod_source_valid(tsf_u16 src, int primary)
{
	int index = src & 127;
	if ((src >> 10) > 3) return 0;
	if (src & 0x80) return !(index == 0 || index == 6 || (index >= 32 && index <= 63) || (index >= 98 && index <= 101) || index >= 120);
	switch (index) { case 0: return !primary; case 2: case 3: case 10: case 13: case 14: case 16: return 1; }
	return 0;
}

// The defaults TinySoundFont applies itself (velocity, CC7 and CC11 to attenuation, CC10 to
// pan, CC91 and CC93 to the sends): a soundfont modulator identical to one of them is not
// used, so they keep their fixed amounts (as BASSMIDI, which reads no CC modulators but CC21-24).
static int tsf_mod_builtin(tsf_u16 src, tsf_u16 dest, tsf_u16 amtSrc)
{
	return amtSrc == 0 && ((dest == 48 && (src == 0x0502 || src == 0x0587 || src == 0x058B)) ||
		(dest == 17 && src == 0x028A) || (dest == 16 && src == 0x00DB) || (dest == 15 && src == 0x00DD));
}

struct tsf_modlist { int num; struct tsf_modulator mods[TSF_MAX_ZONE_MODS]; unsigned char zone[TSF_MAX_ZONE_MODS]; };

// Adds a modulator of `zone`; an identical one (same sources and destination) from an
// earlier zone is replaced, one from the same zone wins over this one.
static void tsf_modlist_add(struct tsf_modlist* l, tsf_u16 src, tsf_u16 dest, tsf_s16 amount, tsf_u16 amtSrc, tsf_u16 trans, unsigned char zone)
{
	unsigned char slot = ((dest & 0x8000) ? TSF_MOD_NONE : tsf_mod_slot(dest));
	int i;
	if (slot == TSF_MOD_NONE || (trans != 0 && trans != 2) || !tsf_mod_source_valid(src, 1) || !tsf_mod_source_valid(amtSrc, 0) || tsf_mod_builtin(src, dest, amtSrc)) return;
	for (i = 0; i != l->num; i++)
		if (l->mods[i].src == src && l->mods[i].slot == slot && l->mods[i].amtSrc == amtSrc) break;
	if (i != l->num && l->zone[i] == zone) return;
	if (i == l->num) { if (l->num == TSF_MAX_ZONE_MODS) return; l->num++; }
	l->mods[i].src = src; l->mods[i].amtSrc = amtSrc; l->mods[i].slot = slot;
	l->mods[i].absolute = (unsigned char)(trans == 2); l->mods[i].kind = TSF_MODKIND_FILE; l->mods[i].amount = amount;
	l->zone[i] = zone;
}

// SoundFont 2.04 8.4.3 and 8.4.4: channel pressure and the mod wheel to vibrato depth, 50
// cents each. 8.4.2 (velocity to filter cutoff) is left out, as in BASSMIDI; the others are
// built in (tsf_mod_builtin).
static void tsf_modlist_defaults(struct tsf_modlist* l)
{
	static const struct tsf_modulator defaults[TSF_DEFAULT_MODS] = {
		{ 0x000D, 0, TSF_MOD_VIBLFOTOPITCH, 0, TSF_MODKIND_PRESSURE, 50.0f },
		{ 0x0081, 0, TSF_MOD_VIBLFOTOPITCH, 0, TSF_MODKIND_MODWHEEL, 50.0f } };
	int i;
	l->num = TSF_DEFAULT_MODS;
	for (i = 0; i != TSF_DEFAULT_MODS; i++) { l->mods[i] = defaults[i]; l->zone[i] = 0; }
}

static void tsf_modlist_add_pbag(struct tsf_modlist* l, const struct tsf_hydra* h, const struct tsf_hydra_pbag* bag, unsigned char zone)
{
	int i;
	if (bag[0].modNdx > bag[1].modNdx || bag[1].modNdx > h->pmodNum) return; // FatzAudio: out of order or range: none
	for (i = bag[0].modNdx; i != bag[1].modNdx; i++)
		tsf_modlist_add(l, h->pmods[i].modSrcOper, h->pmods[i].modDestOper, h->pmods[i].modAmount, h->pmods[i].modAmtSrcOper, h->pmods[i].modTransOper, zone);
}

static void tsf_modlist_add_ibag(struct tsf_modlist* l, const struct tsf_hydra* h, const struct tsf_hydra_ibag* bag, unsigned char zone)
{
	int i;
	if (bag[0].instModNdx > bag[1].instModNdx || bag[1].instModNdx > h->imodNum) return;
	for (i = bag[0].instModNdx; i != bag[1].instModNdx; i++)
		tsf_modlist_add(l, h->imods[i].modSrcOper, h->imods[i].modDestOper, h->imods[i].modAmount, h->imods[i].modAmtSrcOper, h->imods[i].modTransOper, zone);
}

static int tsf_modulator_equal(const struct tsf_modulator* a, const struct tsf_modulator* b)
{
	return a->src == b->src && a->amtSrc == b->amtSrc && a->slot == b->slot && a->absolute == b->absolute && a->kind == b->kind && a->amount == b->amount;
}

// Stores a region's modulators: the instrument's (defaults replaced), then the preset's,
// which add to them (9.5). Equal lists share storage with the previous region or the
// defaults at index 0. Returns 0 when out of memory.
static int tsf_store_mods(tsf* res, struct tsf_region* region, const struct tsf_modlist* il, const struct tsf_modlist* pl, int* capacity, int* lastStart)
{
	int n = il->num + pl->num, i;
	struct tsf_modulator* m;
	region->modStart = 0; region->modNum = TSF_DEFAULT_MODS;
	if (n == TSF_DEFAULT_MODS && !pl->num)
	{
		for (i = 0; i != n && tsf_modulator_equal(&il->mods[i], &res->modulators[i]); i++) {}
		if (i == n) return 1;
	}
	if (*lastStart >= 0 && *lastStart + n <= res->modulatorNum)
	{
		m = res->modulators + *lastStart;
		for (i = 0; i != n && tsf_modulator_equal(i < il->num ? &il->mods[i] : &pl->mods[i - il->num], &m[i]); i++) {}
		if (i == n) { region->modStart = *lastStart; region->modNum = n; return 1; }
	}
	if (res->modulatorNum + n > TSF_MAX_MODULATORS) return 1;
	if (res->modulatorNum + n > *capacity)
	{
		int newCapacity = (*capacity * 2 > res->modulatorNum + n ? *capacity * 2 : res->modulatorNum + n);
		m = (struct tsf_modulator*)TSF_REALLOC(res->modulators, newCapacity * sizeof(struct tsf_modulator));
		if (!m) return 0;
		res->modulators = m;
		*capacity = newCapacity;
	}
	m = res->modulators + res->modulatorNum;
	for (i = 0; i != il->num; i++) m[i] = il->mods[i];
	for (i = 0; i != pl->num; i++) m[il->num + i] = pl->mods[i];
	region->modStart = *lastStart = res->modulatorNum;
	region->modNum = n;
	res->modulatorNum += n;
	return 1;
}

static int tsf_load_presets(tsf* res, struct tsf_hydra *hydra, unsigned int fontSampleCount)
{
	enum { GenInstrument = 41, GenKeyRange = 43, GenVelRange = 44, GenSampleID = 53 };
	// Read each preset.
	struct tsf_hydra_phdr *pphdr, **sortedPhdrs;
	int phdrIndex;
	long long totalRegions = 0;
	struct tsf_modlist instBase, presetGlobal, il, pl; // FatzAudio: modulators
	int modCapacity = 256, lastModStart = -1;
	if (!tsf_hydra_valid(hydra)) return 0;
	res->presetNum = hydra->phdrNum - 1;
	res->presets = (struct tsf_preset*)TSF_MALLOC(res->presetNum * sizeof(struct tsf_preset));
	sortedPhdrs = (struct tsf_hydra_phdr**)TSF_MALLOC(res->presetNum * sizeof(struct tsf_hydra_phdr*));
	res->modulators = (struct tsf_modulator*)TSF_MALLOC(modCapacity * sizeof(struct tsf_modulator));
	if (!res->presets || !sortedPhdrs || !res->modulators) { TSF_FREE(res->presets); res->presets = TSF_NULL; TSF_FREE(sortedPhdrs); TSF_FREE(res->modulators); res->modulators = TSF_NULL; return 0; }
	tsf_modlist_defaults(&il);
	for (res->modulatorNum = 0; res->modulatorNum != TSF_DEFAULT_MODS; res->modulatorNum++) res->modulators[res->modulatorNum] = il.mods[res->modulatorNum];
	{ int i; for (i = 0; i != res->presetNum; i++) { res->presets[i].regions = TSF_NULL; res->presets[i].regionNum = 0; sortedPhdrs[i] = &hydra->phdrs[i]; } }
	qsort(sortedPhdrs, (size_t)res->presetNum, sizeof(struct tsf_hydra_phdr*), tsf_phdr_compare);
	for (phdrIndex = 0; phdrIndex != res->presetNum; phdrIndex++)
	{
		int sortedIndex = phdrIndex, region_index = 0;
		struct tsf_preset* preset;
		struct tsf_hydra_pbag *ppbag, *ppbagEnd;
		struct tsf_region globalRegion;
		pphdr = sortedPhdrs[phdrIndex];

		preset = &res->presets[sortedIndex];
		TSF_MEMCPY(preset->presetName, pphdr->presetName, sizeof(preset->presetName));
		preset->presetName[sizeof(preset->presetName)-1] = '\0'; //should be zero terminated in source file but make sure
		preset->bank = pphdr->bank;
		preset->preset = pphdr->preset;
		preset->regionNum = 0;

		//count regions covered by this preset
		for (ppbag = hydra->pbags + pphdr->presetBagNdx, ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx; ppbag != ppbagEnd; ppbag++)
		{
			unsigned char plokey = 0, phikey = 127, plovel = 0, phivel = 127;
			struct tsf_hydra_pgen *ppgen, *ppgenEnd; struct tsf_hydra_inst *pinst; struct tsf_hydra_ibag *pibag, *pibagEnd; struct tsf_hydra_igen *pigen, *pigenEnd;
			for (ppgen = hydra->pgens + ppbag->genNdx, ppgenEnd = hydra->pgens + ppbag[1].genNdx; ppgen != ppgenEnd; ppgen++)
			{
				if (ppgen->genOper == GenKeyRange) { plokey = ppgen->genAmount.range.lo; phikey = ppgen->genAmount.range.hi; continue; }
				if (ppgen->genOper == GenVelRange) { plovel = ppgen->genAmount.range.lo; phivel = ppgen->genAmount.range.hi; continue; }
				if (ppgen->genOper != GenInstrument) continue;
				if (ppgen->genAmount.wordAmount >= hydra->instNum - 1) continue; // FatzAudio: the last one is the terminal record
				pinst = hydra->insts + ppgen->genAmount.wordAmount;
				for (pibag = hydra->ibags + pinst->instBagNdx, pibagEnd = hydra->ibags + pinst[1].instBagNdx; pibag != pibagEnd; pibag++)
				{
					unsigned char ilokey = 0, ihikey = 127, ilovel = 0, ihivel = 127;
					for (pigen = hydra->igens + pibag->instGenNdx, pigenEnd = hydra->igens + pibag[1].instGenNdx; pigen != pigenEnd; pigen++)
					{
						if (pigen->genOper == GenKeyRange) { ilokey = pigen->genAmount.range.lo; ihikey = pigen->genAmount.range.hi; continue; }
						if (pigen->genOper == GenVelRange) { ilovel = pigen->genAmount.range.lo; ihivel = pigen->genAmount.range.hi; continue; }
						if (pigen->genOper == GenSampleID && ihikey >= plokey && ilokey <= phikey && ihivel >= plovel && ilovel <= phivel) preset->regionNum++;
					}
				}
			}
		}

		totalRegions += preset->regionNum;
		preset->regions = (totalRegions <= TSF_MAX_REGIONS ? (struct tsf_region*)TSF_MALLOC((preset->regionNum ? preset->regionNum : 1) * sizeof(struct tsf_region)) : TSF_NULL);
		if (!preset->regions)
		{
			int i; for (i = 0; i != res->presetNum; i++) TSF_FREE(res->presets[i].regions);
			TSF_FREE(res->presets);
			res->presets = TSF_NULL;
			TSF_FREE(sortedPhdrs);
			TSF_FREE(res->modulators); res->modulators = TSF_NULL;
			return 0;
		}
		tsf_region_clear(&globalRegion, TSF_TRUE);
		presetGlobal.num = 0;

		// Zones.
		for (ppbag = hydra->pbags + pphdr->presetBagNdx, ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx; ppbag != ppbagEnd; ppbag++)
		{
			struct tsf_hydra_pgen *ppgen, *ppgenEnd; struct tsf_hydra_inst *pinst; struct tsf_hydra_ibag *pibag, *pibagEnd; struct tsf_hydra_igen *pigen, *pigenEnd;
			struct tsf_region presetRegion = globalRegion;
			int hadGenInstrument = 0;

			pl = presetGlobal; // FatzAudio: the zone's modulators over the global zone's
			tsf_modlist_add_pbag(&pl, hydra, ppbag, 2);

			// Generators.
			for (ppgen = hydra->pgens + ppbag->genNdx, ppgenEnd = hydra->pgens + ppbag[1].genNdx; ppgen != ppgenEnd; ppgen++)
			{
				// Instrument.
				if (ppgen->genOper == GenInstrument)
				{
					struct tsf_region instRegion;
					tsf_u16 whichInst = ppgen->genAmount.wordAmount;
					if (whichInst >= hydra->instNum - 1) continue; // FatzAudio: the last one is the terminal record

					tsf_region_clear(&instRegion, TSF_FALSE);
					tsf_modlist_defaults(&instBase);
					pinst = &hydra->insts[whichInst];
					for (pibag = hydra->ibags + pinst->instBagNdx, pibagEnd = hydra->ibags + pinst[1].instBagNdx; pibag != pibagEnd; pibag++)
					{
						// Generators.
						struct tsf_region zoneRegion = instRegion;
						int hadSampleID = 0;
						il = instBase; // FatzAudio: the zone's modulators over the global zone's and the defaults
						tsf_modlist_add_ibag(&il, hydra, pibag, 2);
						for (pigen = hydra->igens + pibag->instGenNdx, pigenEnd = hydra->igens + pibag[1].instGenNdx; pigen != pigenEnd; pigen++)
						{
							if (pigen->genOper == GenSampleID)
							{
								struct tsf_hydra_shdr* pshdr;

								// FatzAudio: a sample that does not exist, or more zones than counted
								if (pigen->genAmount.wordAmount >= hydra->shdrNum || region_index >= preset->regionNum) continue;

								//preset region key and vel ranges are a filter for the zone regions
								if (zoneRegion.hikey < presetRegion.lokey || zoneRegion.lokey > presetRegion.hikey) continue;
								if (zoneRegion.hivel < presetRegion.lovel || zoneRegion.lovel > presetRegion.hivel) continue;
								if (presetRegion.lokey > zoneRegion.lokey) zoneRegion.lokey = presetRegion.lokey;
								if (presetRegion.hikey < zoneRegion.hikey) zoneRegion.hikey = presetRegion.hikey;
								if (presetRegion.lovel > zoneRegion.lovel) zoneRegion.lovel = presetRegion.lovel;
								if (presetRegion.hivel < zoneRegion.hivel) zoneRegion.hivel = presetRegion.hivel;

								//sum regions
								tsf_region_operator(&zoneRegion, 0, TSF_NULL, &presetRegion);

								// EG times need to be converted from timecents to seconds.
								tsf_region_envtosecs(&zoneRegion.ampenv, TSF_TRUE);
								tsf_region_envtosecs(&zoneRegion.modenv, TSF_FALSE);

								// LFO times need to be converted from timecents to seconds.
								zoneRegion.delayModLFO = (zoneRegion.delayModLFO < -11950.0f ? 0.0f : tsf_timecents2Secsf(zoneRegion.delayModLFO));
								zoneRegion.delayVibLFO = (zoneRegion.delayVibLFO < -11950.0f ? 0.0f : tsf_timecents2Secsf(zoneRegion.delayVibLFO));

								// Fixup sample positions
								pshdr = &hydra->shdrs[pigen->genAmount.wordAmount];
								zoneRegion.offset += pshdr->start;
								zoneRegion.end += pshdr->end;
								zoneRegion.loop_start += pshdr->startLoop;
								zoneRegion.loop_end += pshdr->endLoop;
								if (pshdr->endLoop > 0) zoneRegion.loop_end -= 1;
								if (zoneRegion.loop_end > fontSampleCount) zoneRegion.loop_end = fontSampleCount;
								if (zoneRegion.pitch_keycenter == -1) zoneRegion.pitch_keycenter = pshdr->originalPitch;
								zoneRegion.tune += pshdr->pitchCorrection;
								zoneRegion.sample_rate = pshdr->sampleRate;
								if (zoneRegion.end && zoneRegion.end < fontSampleCount) zoneRegion.end++;
								else zoneRegion.end = fontSampleCount;
								// FatzAudio: keep every read inside the sample data (it has padding
								// after its end for the interpolation's next sample)
								if (zoneRegion.offset > zoneRegion.end) zoneRegion.offset = zoneRegion.end;
								if (zoneRegion.loop_start > zoneRegion.loop_end) zoneRegion.loop_start = zoneRegion.loop_end;

								if (!tsf_store_mods(res, &zoneRegion, &il, &pl, &modCapacity, &lastModStart)) // FatzAudio
								{
									int i; for (i = 0; i != res->presetNum; i++) TSF_FREE(res->presets[i].regions);
									TSF_FREE(res->presets); res->presets = TSF_NULL;
									TSF_FREE(sortedPhdrs);
									TSF_FREE(res->modulators); res->modulators = TSF_NULL;
									return 0;
								}
								preset->regions[region_index] = zoneRegion;
								region_index++;
								hadSampleID = 1;
							}
							else tsf_region_operator(&zoneRegion, pigen->genOper, &pigen->genAmount, TSF_NULL);
						}

						// Handle instrument's global zone.
						if (pibag == hydra->ibags + pinst->instBagNdx && !hadSampleID)
						{
							int i;
							instRegion = zoneRegion;
							instBase = il; // FatzAudio
							for (i = 0; i != instBase.num; i++) if (instBase.zone[i] == 2) instBase.zone[i] = 1;
						}
					}
					hadGenInstrument = 1;
				}
				else tsf_region_operator(&presetRegion, ppgen->genOper, &ppgen->genAmount, TSF_NULL);
			}

			// Handle preset's global zone.
			if (ppbag == hydra->pbags + pphdr->presetBagNdx && !hadGenInstrument)
			{
				int i;
				globalRegion = presetRegion;
				presetGlobal = pl; // FatzAudio
				for (i = 0; i != presetGlobal.num; i++) presetGlobal.zone[i] = 1;
			}
		}
		preset->regionNum = region_index; // FatzAudio: only the regions actually filled in
	}
	TSF_FREE(sortedPhdrs);
	// FatzAudio: which controllers the modulators read, so a change of others costs nothing
	for (phdrIndex = 0; phdrIndex != res->modulatorNum; phdrIndex++)
	{
		tsf_u16 srcs[2]; int k;
		srcs[0] = res->modulators[phdrIndex].src; srcs[1] = res->modulators[phdrIndex].amtSrc;
		for (k = 0; k != 2; k++)
			if (srcs[k] & 0x80) res->modCC[(srcs[k] & 127) >> 5] |= 1u << (srcs[k] & 31);
			else res->modGC |= 1u << (srcs[k] & 31);
	}
	return 1;
}

#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
static int tsf_decode_ogg(const tsf_u8 *pSmpl, const tsf_u8 *pSmplEnd, float** pRes, tsf_u32* pResNum, tsf_u32* pResMax, tsf_u32 resInitial)
{
	float *res = *pRes, *oldres; tsf_u32 resNum = *pResNum; tsf_u32 resMax = *pResMax; stb_vorbis *v;

	// Use whatever stb_vorbis API that is available (either pull or push)
	#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
	v = stb_vorbis_open_memory(pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, TSF_NULL);
	#else
	{ int use, err; v = stb_vorbis_open_pushdata(pSmpl, (int)(pSmplEnd - pSmpl), &use, &err, TSF_NULL); pSmpl += use; }
	#endif
	if (v == TSF_NULL) return 0;

	for (;;)
	{
		float** outputs; int n_samples;

		// Decode one frame of vorbis samples with whatever stb_vorbis API that is available
		#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
		n_samples = stb_vorbis_get_frame_float(v, TSF_NULL, &outputs);
		if (!n_samples) break;
		#else
		if (pSmpl >= pSmplEnd) break;
		{ int use = stb_vorbis_decode_frame_pushdata(v, pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, &outputs, &n_samples); pSmpl += use; }
		if (!n_samples) continue;
		#endif

		// Expand our output buffer if necessary then copy over the decoded frame samples
		resNum += n_samples;
		if (resNum > resMax)
		{
			do { resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial); } while (resNum > resMax);
			oldres = res;
			res = (float*)TSF_REALLOC(res, resMax * sizeof(float));
			if (!res) { TSF_FREE(oldres); stb_vorbis_close(v); return 0; }
		}
		TSF_MEMCPY(res + resNum - n_samples, outputs[0], n_samples * sizeof(float));
	}
	stb_vorbis_close(v);
	*pRes = res; *pResNum = resNum; *pResMax = resMax;
	return 1;
}

static int tsf_decode_sf3_samples(const void* rawBuffer, float** pFloatBuffer, unsigned int* pSmplCount, struct tsf_hydra *hydra)
{
	const tsf_u8* smplBuffer = (const tsf_u8*)rawBuffer;
	tsf_u32 smplLength = *pSmplCount, resNum = 0, resMax = 0, resInitial = (smplLength > 0x100000 ? (smplLength & ~0xFFFFF) : 65536);
	float *res = TSF_NULL, *oldres;
	int i, shdrLast = hydra->shdrNum - 1, is_sf3 = 0;
	for (i = 0; i <= shdrLast; i++)
	{
		struct tsf_hydra_shdr *shdr = &hydra->shdrs[i];
		if (shdr->sampleType & 0x30) // compression flags (sometimes Vorbis flag)
		{
			const tsf_u8 *pSmpl = smplBuffer + shdr->start, *pSmplEnd = smplBuffer + shdr->end;
			if (pSmpl + 4 > pSmplEnd || !TSF_FourCCEquals(pSmpl, "OggS"))
			{
				shdr->start = shdr->end = shdr->startLoop = shdr->endLoop = 0;
				continue;
			}

			// Fix up sample indices in shdr (end index is set after decoding)
			shdr->start = resNum;
			shdr->startLoop += resNum;
			shdr->endLoop += resNum;
			if (!tsf_decode_ogg(pSmpl, pSmplEnd, &res, &resNum, &resMax, resInitial)) { TSF_FREE(res); return 0; }
			shdr->end = resNum;
			is_sf3 = 1;
		}
		else // raw PCM sample
		{
			float *out; short *in = (short*)smplBuffer + resNum, *inEnd; tsf_u32 oldResNum = resNum;
			if (is_sf3) // Fix up sample indices in shdr
			{
				tsf_u32 fix_offset = resNum - shdr->start;
				in -= fix_offset;
				shdr->start = resNum;
				shdr->end += fix_offset;
				shdr->startLoop += fix_offset;
				shdr->endLoop += fix_offset;
			}
			inEnd = in + ((shdr->end >= shdr->endLoop ? shdr->end : shdr->endLoop) - resNum);
			if (i == shdrLast || (tsf_u8*)inEnd > (smplBuffer + smplLength)) inEnd = (short*)(smplBuffer + smplLength);
			if (inEnd <= in) continue;

			// expand our output buffer if necessary then convert the PCM data from short to float
			resNum += (tsf_u32)(inEnd - in);
			if (resNum > resMax)
			{
				do { resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial); } while (resNum > resMax);
				oldres = res;
				res = (float*)TSF_REALLOC(res, resMax * sizeof(float));
				if (!res) { TSF_FREE(oldres); return 0; }
			}

			// Convert the samples from short to float
			for (out = res + oldResNum; in < inEnd;)
				*(out++) = (float)(*(in++) / 32767.0);
		}
	}

	// Trim the sample buffer down then return success (unless out of memory)
	if (!(*pFloatBuffer = (float*)TSF_REALLOC(res, resNum * sizeof(float)))) *pFloatBuffer = res;
	*pSmplCount = resNum;
	return (res ? 1 : 0);
}
#endif

#define TSF_SAMPLE_PADDING 4

static int tsf_load_samples(void** pRawBuffer, float** pFloatBuffer, unsigned int* pSmplCount, struct tsf_riffchunk *chunkSmpl, struct tsf_stream* stream)
{
	#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
	// With OGG Vorbis support we cannot pre-allocate the memory for tsf_decode_sf3_samples
	tsf_u32 resNum, resMax; float* oldres;
	*pSmplCount = chunkSmpl->size;
	*pRawBuffer = (void*)TSF_MALLOC(*pSmplCount);
	if (!*pRawBuffer || !stream->read(stream->data, *pRawBuffer, chunkSmpl->size)) return 0;
	if (chunkSmpl->id[3] != 'o') return 1;

	// Decode custom .sfo 'smpo' format where all samples are in a single ogg stream
	resNum = resMax = 0;
	if (!tsf_decode_ogg((tsf_u8*)*pRawBuffer, (tsf_u8*)*pRawBuffer + chunkSmpl->size, pFloatBuffer, &resNum, &resMax, 65536)) return 0;
	oldres = *pFloatBuffer;
	if (!(*pFloatBuffer = (float*)TSF_REALLOC(*pFloatBuffer, resNum * sizeof(float)))) *pFloatBuffer = oldres;
	*pSmplCount = resNum;
	return (*pFloatBuffer ? 1 : 0);
	#else
	// Inline convert the samples from short to float
	float *res, *out; const short *in;
	(void)pRawBuffer;
	*pSmplCount = chunkSmpl->size / (unsigned int)sizeof(short);
	// FatzAudio: TSF_SAMPLE_PADDING silent samples after the data (the interpolation
	// reads one sample past a region's end), and a short read is an error
	*pFloatBuffer = (float*)TSF_MALLOC(((size_t)*pSmplCount + TSF_SAMPLE_PADDING) * sizeof(float));
	if (!*pFloatBuffer || stream->read(stream->data, *pFloatBuffer, chunkSmpl->size) != (int)chunkSmpl->size) return 0;
	for (res = *pFloatBuffer, out = res + *pSmplCount, in = (short*)res + *pSmplCount; out != res;)
		*(--out) = (float)(*(--in) / 32767.0);
	TSF_MEMSET(*pFloatBuffer + *pSmplCount, 0, TSF_SAMPLE_PADDING * sizeof(float));
	return 1;
	#endif
}

static int tsf_voice_envelope_release_samples(struct tsf_voice_envelope* e, float outSampleRate)
{
	return (int)((e->parameters.release <= 0 ? TSF_FASTRELEASETIME : e->parameters.release) * outSampleRate);
}

static void tsf_voice_envelope_nextsegment(struct tsf_voice_envelope* e, short active_segment, float outSampleRate)
{
	switch (active_segment)
	{
		case TSF_SEGMENT_NONE:
			e->samplesUntilNextSegment = (int)(e->parameters.delay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DELAY;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0;
				e->slope = 0.0;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DELAY:
			e->samplesUntilNextSegment = (int)(e->parameters.attack * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				if (!e->isAmpEnv)
				{
					//mod env attack duration scales with velocity (velocity of 1 is full duration, max velocity is 0.125 times duration)
					e->samplesUntilNextSegment = (int)(e->parameters.attack * ((145 - e->midiVelocity) / 144.0f) * outSampleRate);
				}
				e->segment = TSF_SEGMENT_ATTACK;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0f;
				e->slope = 1.0f / e->samplesUntilNextSegment;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_ATTACK:
			e->samplesUntilNextSegment = (int)(e->parameters.hold * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_HOLD;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 1.0f;
				e->slope = 0.0f;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_HOLD:
			e->samplesUntilNextSegment = (int)(e->parameters.decay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DECAY;
				e->level = 1.0f;
				if (e->isAmpEnv)
				{
					// I don't truly understand this; just following what LinuxSampler does.
					float mysterySlope = -9.226f / e->samplesUntilNextSegment;
					e->slope = TSF_EXPF(mysterySlope);
					e->segmentIsExponential = TSF_TRUE;
					if (e->parameters.sustain > 0.0f)
					{
						// Again, this is following LinuxSampler's example, which is similar to
						// SF2-style decay, where "decay" specifies the time it would take to
						// get to zero, not to the sustain level.  The SFZ spec is not that
						// specific about what "decay" means, so perhaps it's really supposed
						// to specify the time to reach the sustain level.
						e->samplesUntilNextSegment = (int)(TSF_LOG(e->parameters.sustain) / mysterySlope);
					}
				}
				else
				{
					e->slope = -1.0f / e->samplesUntilNextSegment;
					e->samplesUntilNextSegment = (int)(e->parameters.decay * (1.0f - e->parameters.sustain) * outSampleRate);
					e->segmentIsExponential = TSF_FALSE;
				}
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DECAY:
			e->segment = TSF_SEGMENT_SUSTAIN;
			e->level = e->parameters.sustain;
			e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFFF;
			e->segmentIsExponential = TSF_FALSE;
			return;
		case TSF_SEGMENT_SUSTAIN:
			e->segment = TSF_SEGMENT_RELEASE;
			e->samplesUntilNextSegment = tsf_voice_envelope_release_samples(e, outSampleRate);
			if (e->isAmpEnv)
			{
				// I don't truly understand this; just following what LinuxSampler does.
				float mysterySlope = -9.226f / e->samplesUntilNextSegment;
				e->slope = TSF_EXPF(mysterySlope);
				e->segmentIsExponential = TSF_TRUE;
			}
			else
			{
				e->slope = -e->level / e->samplesUntilNextSegment;
				e->segmentIsExponential = TSF_FALSE;
			}
			return;
		case TSF_SEGMENT_RELEASE:
		default:
			e->segment = TSF_SEGMENT_DONE;
			e->segmentIsExponential = TSF_FALSE;
			e->level = e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFF;
	}
}

static void tsf_voice_envelope_setup(struct tsf_voice_envelope* e, struct tsf_envelope* new_parameters, int midiNoteNumber, short midiVelocity, TSF_BOOL isAmpEnv, float outSampleRate)
{
	e->parameters = *new_parameters;
	if (e->parameters.keynumToHold)
	{
		e->parameters.hold += e->parameters.keynumToHold * (60.0f - midiNoteNumber);
		if (e->parameters.hold > 5000.0f) e->parameters.hold = 5000.0f; // FatzAudio: the generator's limit
		e->parameters.hold = (e->parameters.hold < -10000.0f ? 0.0f : tsf_timecents2Secsf(e->parameters.hold));
	}
	if (e->parameters.keynumToDecay)
	{
		e->parameters.decay += e->parameters.keynumToDecay * (60.0f - midiNoteNumber);
		if (e->parameters.decay > 8000.0f) e->parameters.decay = 8000.0f; // FatzAudio: the generator's limit
		e->parameters.decay = (e->parameters.decay < -10000.0f ? 0.0f : tsf_timecents2Secsf(e->parameters.decay));
	}
	e->midiVelocity = midiVelocity;
	e->isAmpEnv = isAmpEnv;
	tsf_voice_envelope_nextsegment(e, TSF_SEGMENT_NONE, outSampleRate);
}

static void tsf_voice_envelope_process(struct tsf_voice_envelope* e, int numSamples, float outSampleRate)
{
	if (e->slope)
	{
		if (e->segmentIsExponential) e->level *= TSF_POWF(e->slope, (float)numSamples);
		else e->level += (e->slope * numSamples);
	}
	if ((e->samplesUntilNextSegment -= numSamples) <= 0)
		tsf_voice_envelope_nextsegment(e, e->segment, outSampleRate);
}

static void tsf_voice_lowpass_setup(struct tsf_voice_lowpass* e, float Fc)
{
	// Lowpass filter from http://www.earlevel.com/main/2012/11/26/biquad-c-source-code/
	double K = TSF_TAN(TSF_PI * Fc), KK = K * K;
	double norm = 1 / (1 + K * e->QInv + KK);
	e->a0 = KK * norm;
	e->a1 = 2 * e->a0;
	e->b1 = 2 * (KK - 1) * norm;
	e->b2 = (1 - K * e->QInv + KK) * norm;
}

static float tsf_voice_lowpass_process(struct tsf_voice_lowpass* e, double In)
{
	double Out = In * e->a0 + e->z1; e->z1 = In * e->a1 + e->z2 - e->b1 * Out; e->z2 = In * e->a0 - e->b2 * Out; return (float)Out;
}

static void tsf_voice_lfo_setup(struct tsf_voice_lfo* e, float delay, int freqCents, float outSampleRate)
{
	e->samplesUntil = (int)(delay * outSampleRate);
	e->delta = (4.0f * tsf_cents2Hertz((float)freqCents) / outSampleRate);
	e->level = 0;
}

static void tsf_voice_lfo_process(struct tsf_voice_lfo* e, int blockSamples)
{
	if (e->samplesUntil > blockSamples) { e->samplesUntil -= blockSamples; return; }
	e->level += e->delta * blockSamples;
	if      (e->level >  1.0f) { e->delta = -e->delta; e->level =  2.0f - e->level; }
	else if (e->level < -1.0f) { e->delta = -e->delta; e->level = -2.0f - e->level; }
}

static void tsf_voice_kill(struct tsf_voice* v)
{
	v->playingPreset = -1;
}

static void tsf_voice_end(tsf* f, struct tsf_voice* v)
{
	// if maxVoiceNum is set, assume that voice rendering and note queuing are on separate threads
	// so to minimize the chance that voice rendering would advance the segment at the same time
	// we just do it twice here and hope that it sticks
	int repeats = (f->maxVoiceNum ? 2 : 1);
	while (repeats--)
	{
		tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		if (v->region->loop_mode == TSF_LOOPMODE_SUSTAIN)
		{
			// Continue playing, but stop looping.
			v->loopEnd = v->loopStart;
		}
	}
}

static void tsf_voice_endquick(tsf* f, struct tsf_voice* v)
{
	// if maxVoiceNum is set, assume that voice rendering and note queuing are on separate threads
	// so to minimize the chance that voice rendering would advance the segment at the same time
	// we just do it twice here and hope that it sticks
	int repeats = (f->maxVoiceNum ? 2 : 1);
	while (repeats--)
	{
		v->ampenv.parameters.release = 0.0f; tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		v->modenv.parameters.release = 0.0f; tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
	}
}

static void tsf_voice_calcpitchratio(struct tsf_voice* v, float pitchShift, float outSampleRate)
{
	double note = v->playingKey + v->region->transpose + v->region->tune / 100.0;
	double adjustedPitch = v->region->pitch_keycenter + (note - v->region->pitch_keycenter) * (v->region->pitch_keytrack / 100.0);
	if (pitchShift) adjustedPitch += pitchShift;
	if (v->modPitch) adjustedPitch += v->modPitch / 100.0; // FatzAudio: tuning modulators
	if (v->scalePitch || v->drumPitch) adjustedPitch += (v->scalePitch + v->drumPitch) / 100.0; // FatzAudio: scale tuning, drum pitch
	v->pitchInputTimecents = adjustedPitch * 100.0;
	v->pitchOutputFactor = v->region->sample_rate / (tsf_timecents2Secsd(v->region->pitch_keycenter * 100.0) * outSampleRate);
}

// FatzAudio: modulators and controllers ------------------------------------------------

// GS/XG sound controllers (CC71-78, and the NRPNs that set the same values) move a value by
// these steps per unit away from 64. Roland and Yamaha publish no curves; these are close
// enough to be heard the same way: CC74 = 0 closes an open filter to about 500 Hz, CC73 =
// 127 turns an instant attack into a 0.6 s fade-in.
#define TSF_SC_CUTOFF    100.0f // cents (CC74, NRPN 01 20)
#define TSF_SC_RESONANCE   3.0f // centibels (CC71, NRPN 01 21)
#define TSF_SC_TIME      100.0f // timecents (CC73 attack, CC75 decay, CC72 release, CC78 vibrato delay)
#define TSF_SC_TIME_FLOOR (-7200.0f) // an instant time lengthened by them starts from 1/64 s
#define TSF_SC_VIBRATE    37.5f // cents of LFO frequency (CC76, NRPN 01 08)
#define TSF_SC_VIBDEPTH    1.0f // cents of vibrato depth (CC77, NRPN 01 09)
#define TSF_SOFT_CUTOFF (-1200.0f) // cents, notes struck with the soft pedal (CC67) down
#define TSF_SOFT_ATTEN    30.0f // centibels, the same

static float tsf_mod_curve(int type, float x)
{
	float v = x * 128.0f, y;
	switch (type)
	{
		case 1: return (v >= 127.0f ? 1.0f : (float)(-(40.0 / 96.0) * TSF_LOG10((127.0f - v) / 127.0f))); // concave
		case 2: if (v <= 0.0f) return 0.0f; y = (float)(1.0 + (40.0 / 96.0) * TSF_LOG10(v / 127.0f)); return (y < 0.0f ? 0.0f : y); // convex
		case 3: return (x >= 0.5f ? 1.0f : 0.0f); // switch
		default: return x;
	}
}

// A modulator source's value (SF2 2.04 8.2): the controller as 0..1 (pitch wheel: 0..16383),
// direction, polarity and curve applied. Without a channel, controllers are at rest.
static float tsf_mod_source(tsf_u16 src, const struct tsf_channel* c, const struct tsf_voice* v)
{
	int index = src & 127, type = src >> 10;
	float x, range = 128.0f;
	if (src & 0x80) x = (c ? c->cc[index] : 0.0f);
	else switch (index)
	{
		case 0: return 1.0f;
		case 2: x = v->ampenv.midiVelocity; break;
		case 3: x = (float)v->playingKey; break;
		case 10: x = (c ? c->keyPressure[v->playingKey & 127] : 0.0f); break;
		case 13: x = (c ? c->chanPressure : 0.0f); break;
		case 14: x = (c ? c->pitchWheel : 8192.0f); range = 16384.0f; break;
		case 16: x = (c ? c->pitchRange : 2.0f); break;
		default: return 0.0f;
	}
	// negative: linear 1..1/128 as FluidSynth; concave and convex from 127 - value, so the
	// controller at its top gives 0 (convex from 128 - value would give 0.12)
	if (src & 0x100) x = ((type == 1 || type == 2) && !(src & 0x200) ? (range - 1.0f - x) / range : 1.0f - x / range);
	else x /= range;
	if (!(src & 0x200)) return tsf_mod_curve(type, x);
	if (type == 0) return 2.0f * x - 1.0f; // bipolar: -1..1, curves mirrored around the middle
	if (type == 3) return (x >= 0.5f ? 1.0f : -1.0f);
	return (x > 0.5f ? tsf_mod_curve(type, 2.0f * x - 1.0f) : -tsf_mod_curve(type, 1.0f - 2.0f * x));
}

// A time in seconds (0: instant) moved by `tc` timecents of modulators, then by `stc` of a
// sound controller, which first lifts a very short time to TSF_SC_TIME_FLOOR so it can lengthen it.
static float tsf_secs_mod(float secs, float tc, float stc)
{
	float t;
	if (!tc && !stc) return secs;
	t = (secs > 0.0f ? (float)(1200.0 * TSF_LOG(secs) / 0.69314718055994530942) : -12000.0f) + tc;
	if (stc > 0.0f && t < TSF_SC_TIME_FLOOR) t = TSF_SC_TIME_FLOOR;
	t += stc;
	if (t > 8000.0f) t = 8000.0f;
	return (t < -11950.0f ? 0.0f : tsf_timecents2Secsf(t));
}

// The same for a hold or decay time still in timecents (scaled by key at note on).
static float tsf_tc_mod(float tc, float off, float stc)
{
	if (!off && !stc) return tc;
	tc += off;
	if (stc > 0.0f && tc < TSF_SC_TIME_FLOOR) tc = TSF_SC_TIME_FLOOR;
	return tc + stc;
}

// An envelope's parameters with the modulators' offsets (delay, attack, hold, decay,
// sustain, release; sustain in centibels or 0.1%) and the sound controllers' times.
static void tsf_envelope_mod(struct tsf_envelope* p, const struct tsf_envelope* region, const float* m, float sAttack, float sDecay, float sRelease, TSF_BOOL isAmp)
{
	*p = *region;
	p->delay = tsf_secs_mod(p->delay, m[0], 0.0f);
	p->attack = tsf_secs_mod(p->attack, m[1], sAttack);
	p->hold = (p->keynumToHold ? tsf_tc_mod(p->hold, m[2], 0.0f) : tsf_secs_mod(p->hold, m[2], 0.0f));
	p->decay = (p->keynumToDecay ? tsf_tc_mod(p->decay, m[3], sDecay) : tsf_secs_mod(p->decay, m[3], sDecay));
	if (m[4])
	{
		if (isAmp) p->sustain *= tsf_decibelsToGain(-m[4] / 10.0f);
		else p->sustain -= m[4] / 1000.0f;
		if (p->sustain < 0.0f) p->sustain = 0.0f; else if (p->sustain > 1.0f) p->sustain = 1.0f;
	}
	p->release = tsf_secs_mod(p->release, m[5], sRelease);
}

static float tsf_clampf(float v, float lo, float hi) { return (v < lo ? lo : (v > hi ? hi : v)); }

static float tsf_channel_pitchshift(tsf* f, struct tsf_channel* c)
{
	return f->globalTuning + (c->pitchWheel == 8192 ? c->tuning : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) - c->pitchRange + c->tuning)); // FatzAudio: globalTuning
}

static void tsf_voice_setpan(struct tsf_voice* v, float newpan)
{
	if      (newpan <= -0.5f) { v->panFactorLeft = 1.0f; v->panFactorRight = 0.0f; }
	else if (newpan >=  0.5f) { v->panFactorLeft = 0.0f; v->panFactorRight = 1.0f; }
	else { v->panFactorLeft = TSF_SQRTF(0.5f - newpan); v->panFactorRight = TSF_SQRTF(0.5f + newpan); }
}

// Evaluates the voice's modulators and its channel's sound controllers and applies what
// changed: at note on (`start`, after the region's own setup) and when a source changes.
// Envelope times change the segments still to come; LFO delays only count at note on.
static void tsf_voice_modulate(tsf* f, struct tsf_voice* v, TSF_BOOL start)
{
	struct tsf_region* r = v->region;
	struct tsf_channel* c = (f->channels && v->playingChannel >= 0 && v->playingChannel < f->channels->channelNum ? &f->channels->channels[v->playingChannel] : TSF_NULL);
	const struct tsf_modulator *mod = f->modulators + r->modStart, *modEnd = mod + r->modNum;
	float m[TSF_MOD_SLOTS], sAttack = 0.0f, sDecay = 0.0f, sRelease = 0.0f, sDelay = 0.0f, depth = 0.0f, val, val2, env[15];
	TSF_BOOL envChanged = TSF_FALSE;
	int i;
	for (i = 0; i != TSF_MOD_SLOTS; i++) m[i] = 0.0f;
	for (; mod != modEnd; mod++)
	{
		float amount = mod->amount;
		if (mod->kind == TSF_MODKIND_MODWHEEL) amount = (c ? c->modRange : 50.0f);
		else if (mod->kind == TSF_MODKIND_PRESSURE) amount = (c ? c->pressureVibrato : 50.0f);
		val = amount * tsf_mod_source(mod->src, c, v);
		if (val == 0.0f) continue;
		val *= tsf_mod_source(mod->amtSrc, c, v);
		m[mod->slot] += (mod->absolute && val < 0.0f ? -val : val);
	}
	if (c)
	{
		m[TSF_MOD_FILTERFC] += (c->cc[74] - 64) * TSF_SC_CUTOFF;
		m[TSF_MOD_FILTERQ] += (c->cc[71] - 64) * TSF_SC_RESONANCE;
		m[TSF_MOD_FREQVIBLFO] += (c->cc[76] - 64) * TSF_SC_VIBRATE;
		depth = (c->cc[77] - 64) * TSF_SC_VIBDEPTH;
		sAttack = (c->cc[73] - 64) * TSF_SC_TIME;
		sDecay = (c->cc[75] - 64) * TSF_SC_TIME;
		sRelease = (c->cc[72] - 64) * TSF_SC_TIME;
		sDelay = (c->cc[78] - 64) * TSF_SC_TIME;
		m[TSF_MOD_FILTERFC] += v->drumFc; // drum key edits
		m[TSF_MOD_FILTERQ] += v->drumQ;
		sAttack += v->drumAttack;
		sDecay += v->drumDecay;
		sRelease += v->drumRelease;
	}
	if (v->soft) { m[TSF_MOD_FILTERFC] += TSF_SOFT_CUTOFF; m[TSF_MOD_ATTENUATION] += TSF_SOFT_ATTEN; }

	// LFO and envelope depths
	val = (float)r->vibLfoToPitch + m[TSF_MOD_VIBLFOTOPITCH];
	if (depth) { val2 = val + depth; val = (val >= 0.0f ? (val2 < 0.0f ? 0.0f : val2) : (val2 > 0.0f ? 0.0f : val2)); } // depth moves toward 0, not past it
	v->vibLfoToPitch = (m[TSF_MOD_VIBLFOTOPITCH] || depth ? tsf_clampf(val, -12000.0f, 12000.0f) : val);
	v->modLfoToPitch = (float)r->modLfoToPitch + m[TSF_MOD_MODLFOTOPITCH];
	v->modEnvToPitch = (float)r->modEnvToPitch + m[TSF_MOD_MODENVTOPITCH];
	v->modLfoToFilterFc = (float)r->modLfoToFilterFc + m[TSF_MOD_MODLFOTOFILTERFC];
	v->modEnvToFilterFc = (float)r->modEnvToFilterFc + m[TSF_MOD_MODENVTOFILTERFC];
	v->modLfoToVolume = (float)r->modLfoToVolume + m[TSF_MOD_MODLFOTOVOLUME];
	if (m[TSF_MOD_MODLFOTOPITCH]) v->modLfoToPitch = tsf_clampf(v->modLfoToPitch, -12000.0f, 12000.0f);
	if (m[TSF_MOD_MODENVTOPITCH]) v->modEnvToPitch = tsf_clampf(v->modEnvToPitch, -12000.0f, 12000.0f);
	if (m[TSF_MOD_MODLFOTOFILTERFC]) v->modLfoToFilterFc = tsf_clampf(v->modLfoToFilterFc, -12000.0f, 12000.0f);
	if (m[TSF_MOD_MODENVTOFILTERFC]) v->modEnvToFilterFc = tsf_clampf(v->modEnvToFilterFc, -12000.0f, 12000.0f);
	if (m[TSF_MOD_MODLFOTOVOLUME]) v->modLfoToVolume = tsf_clampf(v->modLfoToVolume, -960.0f, 960.0f);

	// filter
	val = (float)r->initialFilterFc + m[TSF_MOD_FILTERFC];
	val2 = (float)r->initialFilterQ + m[TSF_MOD_FILTERQ];
	if (m[TSF_MOD_FILTERFC]) val = tsf_clampf(val, 1500.0f, 13500.0f);
	if (m[TSF_MOD_FILTERQ]) val2 = tsf_clampf(val2, 0.0f, 960.0f);
	if (val != v->filterFc || val2 != v->filterQ)
	{
		float lowpassFc = (val <= 13500 ? tsf_cents2Hertz(val) / f->outSampleRate : 1.0f);
		TSF_BOOL wasActive = v->lowpass.active;
		v->filterFc = val;
		v->filterQ = val2;
		v->lowpass.QInv = 1.0 / TSF_POW(10.0, ((val2 / 10.0f) / 20.0));
		v->lowpass.active = (lowpassFc < 0.499f);
		if (v->lowpass.active && !wasActive) v->lowpass.z1 = v->lowpass.z2 = 0;
		if (v->lowpass.active) tsf_voice_lowpass_setup(&v->lowpass, lowpassFc);
	}

	// LFO rates and delays
	val = (float)r->freqVibLFO + m[TSF_MOD_FREQVIBLFO];
	if (m[TSF_MOD_FREQVIBLFO]) val = tsf_clampf(val, -16000.0f, 4500.0f);
	if (val != v->freqVibLFO)
	{
		float delta = 4.0f * tsf_cents2Hertz(val) / f->outSampleRate;
		v->freqVibLFO = val;
		v->viblfo.delta = (v->viblfo.delta < 0.0f ? -delta : delta);
	}
	val = (float)r->freqModLFO + m[TSF_MOD_FREQMODLFO];
	if (m[TSF_MOD_FREQMODLFO]) val = tsf_clampf(val, -16000.0f, 4500.0f);
	if (val != v->freqModLFO)
	{
		float delta = 4.0f * tsf_cents2Hertz(val) / f->outSampleRate;
		v->freqModLFO = val;
		v->modlfo.delta = (v->modlfo.delta < 0.0f ? -delta : delta);
	}
	if (start && (m[TSF_MOD_DELAYVIBLFO] || sDelay)) v->viblfo.samplesUntil = (int)(tsf_secs_mod(r->delayVibLFO, m[TSF_MOD_DELAYVIBLFO], sDelay) * f->outSampleRate);
	if (start && m[TSF_MOD_DELAYMODLFO]) v->modlfo.samplesUntil = (int)(tsf_secs_mod(r->delayModLFO, m[TSF_MOD_DELAYMODLFO], 0.0f) * f->outSampleRate);

	// envelopes, when their offsets changed (only from 0 at note on, where they start out)
	for (i = 0; i != 12; i++) env[i] = m[TSF_MOD_MODENV + i];
	env[12] = sAttack; env[13] = sDecay; env[14] = sRelease;
	for (i = 0; i != 15; i++) if (env[i] != v->envMod[i]) { envChanged = TSF_TRUE; v->envMod[i] = env[i]; }
	if (envChanged && v->ampenv.segment < TSF_SEGMENT_RELEASE)
	{
		struct tsf_envelope p; struct tsf_voice_envelope e;
		tsf_envelope_mod(&p, &r->ampenv, m + TSF_MOD_AMPENV, sAttack, sDecay, sRelease, TSF_TRUE);
		if (start) tsf_voice_envelope_setup(&v->ampenv, &p, v->playingKey, v->ampenv.midiVelocity, TSF_TRUE, f->outSampleRate);
		else { tsf_voice_envelope_setup(&e, &p, v->playingKey, v->ampenv.midiVelocity, TSF_TRUE, f->outSampleRate); v->ampenv.parameters = e.parameters; }
		tsf_envelope_mod(&p, &r->modenv, m + TSF_MOD_MODENV, sAttack, sDecay, sRelease, TSF_FALSE);
		if (start) tsf_voice_envelope_setup(&v->modenv, &p, v->playingKey, v->modenv.midiVelocity, TSF_FALSE, f->outSampleRate);
		else { tsf_voice_envelope_setup(&e, &p, v->playingKey, v->modenv.midiVelocity, TSF_FALSE, f->outSampleRate); v->modenv.parameters = e.parameters; }
	}

	// level (attenuation stays at 0 dB or more), pitch, pan and sends
	val = m[TSF_MOD_ATTENUATION] * 0.1f;
	if (r->attenuation + val < 0.0f) val = -r->attenuation;
	val = -val;
	if (val != v->modGainDB) { v->noteGainDB += val - v->modGainDB; v->modGainDB = val; }
	val = m[TSF_MOD_COARSETUNE] * 100.0f + m[TSF_MOD_FINETUNE];
	if (val != v->modPitch) { v->modPitch = val; tsf_voice_calcpitchratio(v, (c ? tsf_channel_pitchshift(f, c) : 0.0f), f->outSampleRate); }
	if (m[TSF_MOD_PAN] != v->modPan) { v->modPan = m[TSF_MOD_PAN]; tsf_voice_setpan(v, v->basePan + (c ? c->panOffset : 0.0f) + v->modPan * 0.001f); }
	v->modReverb = m[TSF_MOD_REVERB];
	v->modChorus = m[TSF_MOD_CHORUS];
}

static void tsf_voice_render(tsf* f, struct tsf_voice* v, float* outputBuffer, int numSamples)
{
	struct tsf_region* region = v->region;
	float* input = f->fontSamples;
	float* outL = outputBuffer;
	float* outR = (f->outputmode == TSF_STEREO_UNWEAVED ? outL + numSamples : TSF_NULL);

	// Cache some values, to give them at least some chance of ending up in registers.
	// FatzAudio: the voice's modulated values (tsf_voice_modulate) instead of the region's.
	// The envelope and LFOs run whether used or not, so a controller that brings them in
	// later finds them where they would be.
	TSF_BOOL updateModLFO = (v->modlfo.delta != 0);
	TSF_BOOL updateVibLFO = (v->viblfo.delta != 0);
	TSF_BOOL isLooping    = (v->loopStart < v->loopEnd);
	unsigned int tmpLoopStart = v->loopStart, tmpLoopEnd = v->loopEnd;
	double tmpSampleEndDbl = (double)region->end, tmpLoopEndDbl = (double)tmpLoopEnd + 1.0;
	double tmpSourceSamplePosition = v->sourceSamplePosition;
	struct tsf_voice_lowpass tmpLowpass = v->lowpass;

	TSF_BOOL dynamicLowpass = (v->modLfoToFilterFc || v->modEnvToFilterFc);
	float tmpSampleRate = f->outSampleRate, tmpInitialFilterFc, tmpModLfoToFilterFc, tmpModEnvToFilterFc;

	TSF_BOOL dynamicPitchRatio = (v->modLfoToPitch || v->modEnvToPitch || v->vibLfoToPitch || v->portaLeft > 0);
	double pitchRatio;
	float tmpModLfoToPitch, tmpVibLfoToPitch, tmpModEnvToPitch;

	TSF_BOOL dynamicGain = (v->modLfoToVolume != 0);
	float noteGain = 0, tmpModLfoToVolume;

	if (dynamicLowpass) tmpInitialFilterFc = v->filterFc, tmpModLfoToFilterFc = v->modLfoToFilterFc, tmpModEnvToFilterFc = v->modEnvToFilterFc;
	else tmpInitialFilterFc = 0, tmpModLfoToFilterFc = 0, tmpModEnvToFilterFc = 0;

	if (dynamicPitchRatio) pitchRatio = 0, tmpModLfoToPitch = v->modLfoToPitch, tmpVibLfoToPitch = v->vibLfoToPitch, tmpModEnvToPitch = v->modEnvToPitch;
	else pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor, tmpModLfoToPitch = 0, tmpVibLfoToPitch = 0, tmpModEnvToPitch = 0;

	if (dynamicGain) tmpModLfoToVolume = v->modLfoToVolume * 0.1f;
	else noteGain = tsf_decibelsToGain(v->noteGainDB), tmpModLfoToVolume = 0;

	while (numSamples)
	{
		float gainMono, gainLeft, gainRight;
		int blockSamples = (numSamples > TSF_RENDER_EFFECTSAMPLEBLOCK ? TSF_RENDER_EFFECTSAMPLEBLOCK : numSamples);
		numSamples -= blockSamples;

		if (dynamicLowpass)
		{
			float fres = tmpInitialFilterFc + v->modlfo.level * tmpModLfoToFilterFc + v->modenv.level * tmpModEnvToFilterFc;
			float lowpassFc = (fres <= 13500 ? tsf_cents2Hertz(fres) / tmpSampleRate : 1.0f);
			tmpLowpass.active = (lowpassFc < 0.499f);
			if (tmpLowpass.active) tsf_voice_lowpass_setup(&tmpLowpass, lowpassFc);
		}

		if (dynamicPitchRatio)
		{
			// FatzAudio: a portamento glide, from portaCents away to the note's pitch
			float glide = 0.0f;
			if (v->portaLeft > 0)
			{
				glide = v->portaCents * (float)v->portaLeft / (float)v->portaTotal;
				v->portaLeft = (v->portaLeft > blockSamples ? v->portaLeft - blockSamples : 0);
			}
			pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents + (v->modlfo.level * tmpModLfoToPitch + v->viblfo.level * tmpVibLfoToPitch + v->modenv.level * tmpModEnvToPitch + glide)) * v->pitchOutputFactor;
		}

		if (dynamicGain)
			noteGain = tsf_decibelsToGain(v->noteGainDB + (v->modlfo.level * tmpModLfoToVolume));

		gainMono = noteGain * v->ampenv.level;

		// Update EG.
		tsf_voice_envelope_process(&v->ampenv, blockSamples, tmpSampleRate);
		tsf_voice_envelope_process(&v->modenv, blockSamples, tmpSampleRate);

		// Update LFOs.
		if (updateModLFO) tsf_voice_lfo_process(&v->modlfo, blockSamples);
		if (updateVibLFO) tsf_voice_lfo_process(&v->viblfo, blockSamples);

		switch (f->outputmode)
		{
			case TSF_STEREO_INTERLEAVED:
				gainLeft = gainMono * v->panFactorLeft, gainRight = gainMono * v->panFactorRight;
				while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
				{
					unsigned int pos = (unsigned int)tmpSourceSamplePosition, nextPos = (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);

					// Simple linear interpolation.
					float alpha = (float)(tmpSourceSamplePosition - pos), val = (input[pos] * (1.0f - alpha) + input[nextPos] * alpha);

					// Low-pass filter.
					if (tmpLowpass.active) val = tsf_voice_lowpass_process(&tmpLowpass, val);

					*outL++ += val * gainLeft;
					*outL++ += val * gainRight;

					// Next sample.
					tmpSourceSamplePosition += pitchRatio;
					if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
				}
				break;

			case TSF_STEREO_UNWEAVED:
				gainLeft = gainMono * v->panFactorLeft, gainRight = gainMono * v->panFactorRight;
				while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
				{
					unsigned int pos = (unsigned int)tmpSourceSamplePosition, nextPos = (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);

					// Simple linear interpolation.
					float alpha = (float)(tmpSourceSamplePosition - pos), val = (input[pos] * (1.0f - alpha) + input[nextPos] * alpha);

					// Low-pass filter.
					if (tmpLowpass.active) val = tsf_voice_lowpass_process(&tmpLowpass, val);

					*outL++ += val * gainLeft;
					*outR++ += val * gainRight;

					// Next sample.
					tmpSourceSamplePosition += pitchRatio;
					if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
				}
				break;

			case TSF_MONO:
				while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
				{
					unsigned int pos = (unsigned int)tmpSourceSamplePosition, nextPos = (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);

					// Simple linear interpolation.
					float alpha = (float)(tmpSourceSamplePosition - pos), val = (input[pos] * (1.0f - alpha) + input[nextPos] * alpha);

					// Low-pass filter.
					if (tmpLowpass.active) val = tsf_voice_lowpass_process(&tmpLowpass, val);

					*outL++ += val * gainMono;

					// Next sample.
					tmpSourceSamplePosition += pitchRatio;
					if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
				}
				break;
		}

		if (tmpSourceSamplePosition >= tmpSampleEndDbl || v->ampenv.segment == TSF_SEGMENT_DONE)
		{
			tsf_voice_kill(v);
			return;
		}
	}

	v->sourceSamplePosition = tmpSourceSamplePosition;
	if (tmpLowpass.active || dynamicLowpass) v->lowpass = tmpLowpass;
}

TSFDEF tsf* tsf_load(struct tsf_stream* stream)
{
	return tsf_load_sized(stream, 0xFFFFFFFFu);
}

// FatzAudio: `total` is the size of the data (the RIFF chunk is clamped to it).
static tsf* tsf_load_sized(struct tsf_stream* stream, tsf_u32 total)
{
	tsf* res = TSF_NULL;
	struct tsf_riffchunk chunkHead;
	struct tsf_riffchunk chunkList;
	struct tsf_hydra hydra;
	void* rawBuffer = TSF_NULL;
	float* floatBuffer = TSF_NULL;
	tsf_u32 smplCount = 0;

	if (!tsf_riffchunk_read(TSF_NULL, &chunkHead, stream) || !TSF_FourCCEquals(chunkHead.id, "sfbk"))
	{
		//if (e) *e = TSF_INVALID_NOSF2HEADER;
		return res;
	}
	if (total >= 12 && chunkHead.size > total - 12) chunkHead.size = total - 12; // FatzAudio

	// Read hydra and locate sample data.
	TSF_MEMSET(&hydra, 0, sizeof(hydra));
	while (tsf_riffchunk_read(&chunkHead, &chunkList, stream))
	{
		struct tsf_riffchunk chunk;
		if (TSF_FourCCEquals(chunkList.id, "pdta"))
		{
			while (tsf_riffchunk_read(&chunkList, &chunk, stream))
			{
				#define HandleChunk(chunkName) (TSF_FourCCEquals(chunk.id, #chunkName) && !(chunk.size % chunkName##SizeInFile)) \
					{ \
						int num = chunk.size / chunkName##SizeInFile, i; \
						hydra.chunkName##Num = num; \
						if (hydra.chunkName##s) goto out_of_memory; /* FatzAudio: a second chunk of the kind */ \
						hydra.chunkName##s = (struct tsf_hydra_##chunkName*)TSF_MALLOC((num ? num : 1) * sizeof(struct tsf_hydra_##chunkName)); \
						if (!hydra.chunkName##s) goto out_of_memory; \
						TSF_MEMSET(hydra.chunkName##s, 0, (num ? num : 1) * sizeof(struct tsf_hydra_##chunkName)); /* FatzAudio */ \
						for (i = 0; i < num; ++i) tsf_hydra_read_##chunkName(&hydra.chunkName##s[i], stream); \
					}
				enum
				{
					phdrSizeInFile = 38, pbagSizeInFile =  4, pmodSizeInFile = 10,
					pgenSizeInFile =  4, instSizeInFile = 22, ibagSizeInFile =  4,
					imodSizeInFile = 10, igenSizeInFile =  4, shdrSizeInFile = 46
				};
				if      HandleChunk(phdr) else if HandleChunk(pbag) else if HandleChunk(pmod)
				else if HandleChunk(pgen) else if HandleChunk(inst) else if HandleChunk(ibag)
				else if HandleChunk(imod) else if HandleChunk(igen) else if HandleChunk(shdr)
				else stream->skip(stream->data, chunk.size);
				#undef HandleChunk
			}
		}
		else if (TSF_FourCCEquals(chunkList.id, "sdta"))
		{
			while (tsf_riffchunk_read(&chunkList, &chunk, stream))
			{
				if ((TSF_FourCCEquals(chunk.id, "smpl")
						#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
						|| TSF_FourCCEquals(chunk.id, "smpo")
						#endif
					) && !rawBuffer && !floatBuffer && chunk.size >= sizeof(short))
				{
					if (!tsf_load_samples(&rawBuffer, &floatBuffer, &smplCount, &chunk, stream)) goto out_of_memory;
				}
				else stream->skip(stream->data, chunk.size);
			}
		}
		else stream->skip(stream->data, chunkList.size);
	}
	if (!hydra.phdrs || !hydra.pbags || !hydra.pmods || !hydra.pgens || !hydra.insts || !hydra.ibags || !hydra.imods || !hydra.igens || !hydra.shdrs)
	{
		//if (e) *e = TSF_INVALID_INCOMPLETE;
	}
	else if (!rawBuffer && !floatBuffer)
	{
		//if (e) *e = TSF_INVALID_NOSAMPLEDATA;
	}
	else
	{
		#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
		if (!floatBuffer && !tsf_decode_sf3_samples(rawBuffer, &floatBuffer, &smplCount, &hydra)) goto out_of_memory;
		#endif
		res = (tsf*)TSF_MALLOC(sizeof(tsf));
		if (res) TSF_MEMSET(res, 0, sizeof(tsf));
		if (!res || !tsf_load_presets(res, &hydra, smplCount)) goto out_of_memory;
		res->outSampleRate = 44100.0f;
		res->fontSamples = floatBuffer;
		floatBuffer = TSF_NULL; // don't free below
	}
	if (0)
	{
		out_of_memory:
		TSF_FREE(res);
		res = TSF_NULL;
		//if (e) *e = TSF_OUT_OF_MEMORY;
	}
	TSF_FREE(hydra.phdrs); TSF_FREE(hydra.pbags); TSF_FREE(hydra.pmods);
	TSF_FREE(hydra.pgens); TSF_FREE(hydra.insts); TSF_FREE(hydra.ibags);
	TSF_FREE(hydra.imods); TSF_FREE(hydra.igens); TSF_FREE(hydra.shdrs);
	TSF_FREE(rawBuffer);   TSF_FREE(floatBuffer);
	return res;
}

TSFDEF tsf* tsf_copy(tsf* f)
{
	tsf* res;
	if (!f) return TSF_NULL;
	if (!f->refCount)
	{
		f->refCount = (int*)TSF_MALLOC(sizeof(int));
		if (!f->refCount) return TSF_NULL;
		*f->refCount = 1;
	}
	res = (tsf*)TSF_MALLOC(sizeof(tsf));
	if (!res) return TSF_NULL;
	TSF_MEMCPY(res, f, sizeof(tsf));
	res->voices = TSF_NULL;
	res->voiceNum = 0;
	res->channels = TSF_NULL;
	res->drumKeys = TSF_NULL; // FatzAudio
	(*res->refCount)++;
	return res;
}

TSFDEF void tsf_close(tsf* f)
{
	if (!f) return;
	if (!f->refCount || !--(*f->refCount))
	{
		struct tsf_preset *preset = f->presets, *presetEnd = preset + f->presetNum;
		for (; preset != presetEnd; preset++) TSF_FREE(preset->regions);
		TSF_FREE(f->presets);
		TSF_FREE(f->fontSamples);
		TSF_FREE(f->modulators); // FatzAudio
		TSF_FREE(f->refCount);
	}
	TSF_FREE(f->channels);
	TSF_FREE(f->voices);
	TSF_FREE(f->drumKeys); // FatzAudio
	TSF_FREE(f);
}

TSFDEF void tsf_reset(tsf* f)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && (v->ampenv.segment < TSF_SEGMENT_RELEASE || v->ampenv.parameters.release))
			tsf_voice_endquick(f, v);
	if (f->channels) { TSF_FREE(f->channels); f->channels = TSF_NULL; }
}

TSFDEF int tsf_get_presetindex(const tsf* f, int bank, int preset_number)
{
	const struct tsf_preset *presets;
	int i, iMax;
	for (presets = f->presets, i = 0, iMax = f->presetNum; i < iMax; i++)
		if (presets[i].preset == preset_number && presets[i].bank == bank)
			return i;
	return -1;
}

TSFDEF int tsf_get_presetcount(const tsf* f)
{
	return f->presetNum;
}

TSFDEF const char* tsf_get_presetname(const tsf* f, int preset)
{
	return (preset < 0 || preset >= f->presetNum ? TSF_NULL : f->presets[preset].presetName);
}

TSFDEF const char* tsf_bank_get_presetname(const tsf* f, int bank, int preset_number)
{
	return tsf_get_presetname(f, tsf_get_presetindex(f, bank, preset_number));
}

TSFDEF void tsf_set_output(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db)
{
	f->outputmode = outputmode;
	f->outSampleRate = (float)(samplerate >= 1 ? samplerate : 44100.0f);
	f->globalGainDB = global_gain_db;
}

TSFDEF void tsf_set_volume(tsf* f, float global_volume)
{
	f->globalGainDB = (global_volume == 1.0f ? 0 : -tsf_gainToDecibels(1.0f / global_volume));
}

TSFDEF int tsf_set_max_voices(tsf* f, int max_voices)
{
	int i = f->voiceNum;
	int newVoiceNum = (f->voiceNum > max_voices ? f->voiceNum : max_voices);
	struct tsf_voice *newVoices = (struct tsf_voice*)TSF_REALLOC(f->voices, newVoiceNum * sizeof(struct tsf_voice));
	if (!newVoices) return 0;
	f->voices = newVoices;
	f->voiceNum = f->maxVoiceNum = newVoiceNum;
	for (; i < max_voices; i++)
		f->voices[i].playingPreset = -1;
	return 1;
}

// FatzAudio: drum key edits ------------------------------------------------------------

// The edits of `key` in the kit of the channel a note starts on; NULL if none.
static const struct tsf_drumkey* tsf_drumkey_of(tsf* f, int key)
{
	const struct tsf_channel* c;
	if (!f->drumKeys || !f->channels || key < 0 || key > 127) return TSF_NULL;
	c = &f->channels->channels[f->channels->activeChannel];
	if (c->drumKit < 0 || !f->drumKeys[c->drumKit * 128 + key].edited) return TSF_NULL;
	return &f->drumKeys[c->drumKit * 128 + key];
}

static void tsf_voice_drum(struct tsf_voice* v, const struct tsf_drumkey* d)
{
	const unsigned char* p = d->param;
	v->drumPitch = (p[TSF_DRUM_PITCH_COARSE] - 64) * 100.0f + (p[TSF_DRUM_PITCH_FINE] - 64);
	if (p[TSF_DRUM_LEVEL] != 127) v->noteGainDB += 2.0f * tsf_gainToDecibels(p[TSF_DRUM_LEVEL] / 127.0f); // as velocity: 40 log10
	if (p[TSF_DRUM_PAN]) v->basePan = (p[TSF_DRUM_PAN] - 64) / 126.0f;
	v->sendReverb = p[TSF_DRUM_REVERB] / 127.0f;
	v->sendChorus = p[TSF_DRUM_CHORUS] / 127.0f;
	v->sendVariation = p[TSF_DRUM_VARIATION] / 127.0f;
	v->drumFc = (p[TSF_DRUM_CUTOFF] - 64) * TSF_SC_CUTOFF;
	v->drumQ = (p[TSF_DRUM_RESONANCE] - 64) * TSF_SC_RESONANCE;
	v->drumAttack = (p[TSF_DRUM_ATTACK] - 64) * TSF_SC_TIME;
	v->drumDecay = (p[TSF_DRUM_DECAY] - 64) * TSF_SC_TIME;
	v->drumRelease = (p[TSF_DRUM_RELEASE] - 64) * TSF_SC_TIME;
}

TSFDEF int tsf_note_on(tsf* f, int preset_index, int key, float vel)
{
	short midiVelocity = (short)(vel * 127);
	unsigned int voicePlayIndex;
	struct tsf_region *region, *regionEnd;
	const struct tsf_drumkey* drum;

	if (preset_index < 0 || preset_index >= f->presetNum) return 1;
	if (vel <= 0.0f) { tsf_note_off(f, preset_index, key); return 1; }
	drum = tsf_drumkey_of(f, key); // FatzAudio

	// Play all matching regions.
	voicePlayIndex = f->voicePlayIndex++;
	for (region = f->presets[preset_index].regions, regionEnd = region + f->presets[preset_index].regionNum; region != regionEnd; region++)
	{
		struct tsf_voice *voice, *v, *vEnd; TSF_BOOL doLoop; float lowpassFilterQDB, lowpassFc; unsigned int group;
		if (key < region->lokey || key > region->hikey || midiVelocity < region->lovel || midiVelocity > region->hivel) continue;

		// FatzAudio: a drum key's edited exclusive group replaces the region's
		// (kept apart per kit: the same group number in two kits are two groups)
		group = (drum && drum->param[TSF_DRUM_GROUP] != 0xFF ? (drum->param[TSF_DRUM_GROUP] ? 0x10000u | (unsigned int)(drum - f->drumKeys) / 128u << 7 | drum->param[TSF_DRUM_GROUP] : 0) : region->group);
		voice = TSF_NULL, v = f->voices, vEnd = v + f->voiceNum;
		if (group)
		{
			for (; v != vEnd; v++)
				if (v->playingPreset == preset_index && v->group == group) tsf_voice_endquick(f, v);
				else if (v->playingPreset == -1 && !voice) voice = v;
		}
		else for (; v != vEnd; v++) if (v->playingPreset == -1) { voice = v; break; }

		if (!voice)
		{
			if (f->maxVoiceNum)
			{
				// Voices have been pre-allocated and limited to a maximum, try to kill a voice off in its release envelope
				int bestKillReleaseSamplePos = -999999999;
				for (v = f->voices; v != vEnd; v++)
				{
					if (v->ampenv.segment == TSF_SEGMENT_RELEASE)
					{
						// We're looking for the voice furthest into its release
						int releaseSamplesDone = tsf_voice_envelope_release_samples(&v->ampenv, f->outSampleRate) - v->ampenv.samplesUntilNextSegment;
						if (releaseSamplesDone > bestKillReleaseSamplePos)
						{
							bestKillReleaseSamplePos = releaseSamplesDone;
							voice = v;
						}
					}
				}
				if (!voice)
					continue;
				tsf_voice_kill(voice);
			}
			else
			{
				// Allocate more voices so we don't need to kill one off.
				struct tsf_voice* newVoices;
				f->voiceNum += 4;
				newVoices = (struct tsf_voice*)TSF_REALLOC(f->voices, f->voiceNum * sizeof(struct tsf_voice));
				if (!newVoices) return 0;
				f->voices = newVoices;
				voice = &f->voices[f->voiceNum - 4];
				voice[1].playingPreset = voice[2].playingPreset = voice[3].playingPreset = -1;
			}
		}

		voice->region = region;
		voice->playingPreset = preset_index;
		voice->playingKey = key;
		voice->playIndex = voicePlayIndex;
		voice->heldSustain = 0;
		// FatzAudio: the SF2 default velocity-to-attenuation modulator (concave, 96 dB), which is
		// 40*log10(vel) dB, as FluidSynth and DLS synths play it (was 20*log10, half the range)
		voice->noteGainDB = f->globalGainDB - region->attenuation - 2.0f * tsf_gainToDecibels(1.0f / vel);
		// FatzAudio: the region's values until tsf_voice_modulate below
		voice->playingChannel = -1;
		voice->vibLfoToPitch = (float)region->vibLfoToPitch; voice->modLfoToPitch = (float)region->modLfoToPitch;
		voice->modEnvToPitch = (float)region->modEnvToPitch; voice->modLfoToFilterFc = (float)region->modLfoToFilterFc;
		voice->modEnvToFilterFc = (float)region->modEnvToFilterFc; voice->modLfoToVolume = (float)region->modLfoToVolume;
		voice->filterFc = (float)region->initialFilterFc; voice->filterQ = (float)region->initialFilterQ;
		voice->freqVibLFO = (float)region->freqVibLFO; voice->freqModLFO = (float)region->freqModLFO;
		voice->modGainDB = voice->modPitch = voice->modPan = voice->modReverb = voice->modChorus = 0.0f;
		voice->portaCents = 0.0f; voice->portaLeft = voice->portaTotal = 0;
		voice->sostenuto = voice->soft = 0;
		TSF_MEMSET(voice->envMod, 0, sizeof(voice->envMod));
		voice->group = group;
		voice->drumHold = (unsigned char)(drum && !drum->param[TSF_DRUM_NOTE_OFF]);
		voice->scalePitch = voice->drumPitch = 0.0f;
		voice->basePan = region->pan;
		voice->sendReverb = voice->sendChorus = voice->sendVariation = 1.0f;
		voice->drumFc = voice->drumQ = voice->drumAttack = voice->drumDecay = voice->drumRelease = 0.0f;
		if (drum) tsf_voice_drum(voice, drum);

		if (f->channels)
		{
			f->channels->setupVoice(f, voice);
		}
		else
		{
			tsf_voice_calcpitchratio(voice, 0, f->outSampleRate);
			// The SFZ spec is silent about the pan curve, but a 3dB pan law seems common. This sqrt() curve matches what Dimension LE does; Alchemy Free seems closer to sin(adjustedPan * pi/2).
			voice->panFactorLeft  = TSF_SQRTF(0.5f - region->pan);
			voice->panFactorRight = TSF_SQRTF(0.5f + region->pan);
		}

		// Offset/end.
		voice->sourceSamplePosition = region->offset;

		// Loop.
		doLoop = (region->loop_mode != TSF_LOOPMODE_NONE && region->loop_start < region->loop_end);
		voice->loopStart = (doLoop ? region->loop_start : 0);
		voice->loopEnd = (doLoop ? region->loop_end : 0);

		// Setup envelopes.
		tsf_voice_envelope_setup(&voice->ampenv, &region->ampenv, key, midiVelocity, TSF_TRUE, f->outSampleRate);
		tsf_voice_envelope_setup(&voice->modenv, &region->modenv, key, midiVelocity, TSF_FALSE, f->outSampleRate);

		// Setup lowpass filter.
		lowpassFc = (region->initialFilterFc <= 13500 ? tsf_cents2Hertz((float)region->initialFilterFc) / f->outSampleRate : 1.0f);
		lowpassFilterQDB = region->initialFilterQ / 10.0f;
		voice->lowpass.QInv = 1.0 / TSF_POW(10.0, (lowpassFilterQDB / 20.0));
		voice->lowpass.z1 = voice->lowpass.z2 = 0;
		voice->lowpass.active = (lowpassFc < 0.499f);
		if (voice->lowpass.active) tsf_voice_lowpass_setup(&voice->lowpass, lowpassFc);

		// Setup LFO filters.
		tsf_voice_lfo_setup(&voice->modlfo, region->delayModLFO, region->freqModLFO, f->outSampleRate);
		tsf_voice_lfo_setup(&voice->viblfo, region->delayVibLFO, region->freqVibLFO, f->outSampleRate);

		tsf_voice_modulate(f, voice, TSF_TRUE); // FatzAudio
	}
	return 1;
}

TSFDEF int tsf_bank_note_on(tsf* f, int bank, int preset_number, int key, float vel)
{
	int preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	return tsf_note_on(f, preset_index, key, vel);
}

TSFDEF void tsf_note_off(tsf* f, int preset_index, int key)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum, *vMatchFirst = TSF_NULL, *vMatchLast = TSF_NULL;
	for (; v != vEnd; v++)
	{
		//Find the first and last entry in the voices list with matching preset, key and look up the smallest play index
		if (v->playingPreset != preset_index || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE) continue;
		else if (!vMatchFirst || v->playIndex < vMatchFirst->playIndex) vMatchFirst = vMatchLast = v;
		else if (v->playIndex == vMatchFirst->playIndex) vMatchLast = v;
	}
	if (!vMatchFirst) return;
	for (v = vMatchFirst; v <= vMatchLast; v++)
	{
		//Stop all voices with matching preset, key and the smallest play index which was enumerated above
		if (v != vMatchFirst && v != vMatchLast &&
			(v->playIndex != vMatchFirst->playIndex || v->playingPreset != preset_index || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE)) continue;
		tsf_voice_end(f, v);
	}
}

TSFDEF int tsf_bank_note_off(tsf* f, int bank, int preset_number, int key)
{
	int preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	tsf_note_off(f, preset_index, key);
	return 1;
}

TSFDEF void tsf_note_off_all(tsf* f)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++) if (v->playingPreset != -1 && v->ampenv.segment < TSF_SEGMENT_RELEASE)
		tsf_voice_end(f, v);
}

TSFDEF int tsf_active_voice_count(tsf* f)
{
	int count = 0;
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++) if (v->playingPreset != -1) count++;
	return count;
}

TSFDEF void tsf_render_short(tsf* f, short* buffer, int samples, int flag_mixing)
{
	float outputSamples[TSF_RENDER_SHORTBUFFERBLOCK];
	int channels = (f->outputmode == TSF_MONO ? 1 : 2), maxChannelSamples = TSF_RENDER_SHORTBUFFERBLOCK / channels;
	while (samples > 0)
	{
		int channelSamples = (samples > maxChannelSamples ? maxChannelSamples : samples);
		short* bufferEnd = buffer + channelSamples * channels;
		float *floatSamples = outputSamples;
		tsf_render_float(f, floatSamples, channelSamples, TSF_FALSE);
		samples -= channelSamples;

		if (flag_mixing)
			while (buffer != bufferEnd)
			{
				float v = *floatSamples++;
				int vi = *buffer + (v < -1.00004566f ? (int)-32768 : (v > 1.00001514f ? (int)32767 : (int)(v * 32767.5f)));
				*buffer++ = (vi < -32768 ? (short)-32768 : (vi > 32767 ? (short)32767 : (short)vi));
			}
		else
			while (buffer != bufferEnd)
			{
				float v = *floatSamples++;
				*buffer++ = (v < -1.00004566f ? (short)-32768 : (v > 1.00001514f ? (short)32767 : (short)(v * 32767.5f)));
			}
	}
}

TSFDEF void tsf_render_float(tsf* f, float* buffer, int samples, int flag_mixing)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	if (!flag_mixing) TSF_MEMSET(buffer, 0, (f->outputmode == TSF_MONO ? 1 : 2) * sizeof(float) * samples);
	for (; v != vEnd; v++)
		if (v->playingPreset != -1)
			tsf_voice_render(f, v, buffer, samples);
}

// FatzAudio: effect sends
static float tsf_voice_send(float region_send, int cc)
{
	float s = (region_send + cc * (200.0f / 127.0f)) * 0.001f;
	return (s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s));
}

TSFDEF void tsf_render_float_fx(tsf* f, float* buffer, float* reverb, float* chorus, float* variation, float* insert, int insertChannel, int samples, int flag_mixing)
{
	float tmp[TSF_RENDER_EFFECTSAMPLEBLOCK * 2];
	int chans = (f->outputmode == TSF_MONO ? 1 : 2);
	struct tsf_voice *v, *vEnd = f->voices + f->voiceNum;
	if (f->outputmode == TSF_STEREO_UNWEAVED) { tsf_render_float(f, buffer, samples, flag_mixing); return; }
	if (!flag_mixing) TSF_MEMSET(buffer, 0, chans * sizeof(float) * samples);
	while (samples > 0)
	{
		int k = (samples < TSF_RENDER_EFFECTSAMPLEBLOCK ? samples : TSF_RENDER_EFFECTSAMPLEBLOCK), n = k * chans, i;
		for (v = f->voices; v != vEnd; v++)
		{
			float rs = 0.0f, cs = 0.0f, vs = 0.0f;
			if (v->playingPreset == -1) continue;
			if (insert && v->playingChannel == insertChannel) { tsf_voice_render(f, v, insert, k); continue; }
			if (f->channels && v->playingChannel >= 0 && v->playingChannel < f->channels->channelNum)
			{
				struct tsf_channel* c = &f->channels->channels[v->playingChannel];
				rs = tsf_voice_send(v->region->reverbSend + v->modReverb, c->midiReverb) * v->sendReverb;
				cs = tsf_voice_send(v->region->chorusSend + v->modChorus, c->midiChorus) * v->sendChorus;
				if (variation) vs = tsf_voice_send(0.0f, c->cc[94]) * v->sendVariation;
			}
			else
			{
				rs = tsf_voice_send(v->region->reverbSend + v->modReverb, 0);
				cs = tsf_voice_send(v->region->chorusSend + v->modChorus, 0);
			}
			if (rs == 0.0f && cs == 0.0f && vs == 0.0f) { tsf_voice_render(f, v, buffer, k); continue; }
			TSF_MEMSET(tmp, 0, n * sizeof(float));
			tsf_voice_render(f, v, tmp, k);
			for (i = 0; i != n; i++) { buffer[i] += tmp[i]; reverb[i] += tmp[i] * rs; chorus[i] += tmp[i] * cs; }
			if (vs != 0.0f) for (i = 0; i != n; i++) variation[i] += tmp[i] * vs;
		}
		buffer += n; reverb += n; chorus += n; samples -= k;
		if (variation) variation += n;
		if (insert) insert += n;
	}
}

static void tsf_channel_setup_voice(tsf* f, struct tsf_voice* v)
{
	struct tsf_channel* c = &f->channels->channels[f->channels->activeChannel];
	v->playingChannel = f->channels->activeChannel;
	v->noteGainDB += c->gainDB;
	v->scalePitch = c->scaleTune[v->playingKey % 12]; // FatzAudio
	tsf_voice_calcpitchratio(v, tsf_channel_pitchshift(f, c), f->outSampleRate);
	tsf_voice_setpan(v, v->basePan + c->panOffset); // FatzAudio: drum pan
	// FatzAudio: soft pedal; portamento: a glide from the previous key (or CC84's) taking
	// 5 ms at CC5 = 0, 0.18 s at 64 and 5.9 s at 127
	v->soft = (unsigned char)(c->cc[67] >= 64);
	if (c->portaFrom >= 0 && c->portaFrom != v->playingKey)
	{
		v->portaCents = (float)(c->portaFrom - v->playingKey) * 100.0f;
		v->portaTotal = (int)(0.005f * TSF_POWF(2.0f, c->cc[5] * (10.2f / 127.0f)) * f->outSampleRate);
		if (v->portaTotal < 1) v->portaTotal = 1;
		v->portaLeft = v->portaTotal;
	}
}

static struct tsf_channel* tsf_channel_init(tsf* f, int channel)
{
	int i;
	if (f->channels && channel < f->channels->channelNum) return &f->channels->channels[channel];
	if (!f->channels)
	{
		f->channels = (struct tsf_channels*)TSF_MALLOC(sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
		if (!f->channels) return TSF_NULL;
		f->channels->setupVoice = &tsf_channel_setup_voice;
		f->channels->channelNum = 0;
		f->channels->activeChannel = 0;
	}
	else
	{
		struct tsf_channels *newChannels = (struct tsf_channels*)TSF_REALLOC(f->channels, sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
		if (!newChannels) return TSF_NULL;
		f->channels = newChannels;
	}
	i = f->channels->channelNum;
	f->channels->channelNum = channel + 1;
	for (; i <= channel; i++)
	{
		struct tsf_channel* c = &f->channels->channels[i];
		c->presetIndex = c->bank = 0;
		c->pitchWheel = c->midiPan = 8192;
		c->midiVolume = c->midiExpression = 16383;
		c->midiRPN = 0xFFFF;
		c->midiData = c->sustain = 0;
		c->panOffset = 0.0f;
		c->gainDB = 0.0f;
		c->pitchRange = 2.0f;
		c->tuning = 0.0f;
		c->midiReverb = 40; // FatzAudio: the GM2 / GS default
		c->midiChorus = 0;
		TSF_MEMSET(c->cc, 0, sizeof(c->cc)); // FatzAudio: controllers as after a reset
		TSF_MEMSET(c->cc + 70, 64, 10);
		c->cc[7] = c->cc[11] = 127; c->cc[10] = 64; c->cc[91] = 40;
		TSF_MEMSET(c->keyPressure, 0, sizeof(c->keyPressure));
		c->chanPressure = c->sostenuto = 0;
		c->midiNRPN = 0xFFFF;
		c->modRange = c->pressureVibrato = 50.0f;
		c->lastKey = c->portaKey = c->portaFrom = -1;
		TSF_MEMSET(c->scaleTune, 0, sizeof(c->scaleTune));
		c->drumKit = -1;
	}
	return &f->channels->channels[channel];
}

static void tsf_channel_applypitch(tsf* f, int channel, struct tsf_channel* c)
{
	struct tsf_voice *v, *vEnd;
	float pitchShift = tsf_channel_pitchshift(f, c);
	for (v = f->voices, vEnd = v + f->voiceNum; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
			tsf_voice_calcpitchratio(v, pitchShift, f->outSampleRate);
}

TSFDEF int tsf_channel_set_presetindex(tsf* f, int channel, int preset_index)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->presetIndex = (unsigned short)preset_index;
	return 1;
}

TSFDEF int tsf_channel_set_presetnumber(tsf* f, int channel, int preset_number, int flag_mididrums)
{
	int preset_index;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (flag_mididrums)
	{
		preset_index = tsf_get_presetindex(f, 128 | (c->bank & 0x7FFF), preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 128, preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 128, 0);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
	}
	else preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
	if (preset_index == -1) preset_index = tsf_get_presetindex(f, 0, preset_number);
	if (preset_index != -1)
	{
		c->presetIndex = (unsigned short)preset_index;
		return 1;
	}
	return 0;
}

TSFDEF int tsf_channel_set_bank(tsf* f, int channel, int bank)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->bank = (unsigned short)bank;
	return 1;
}

TSFDEF int tsf_channel_set_bank_preset(tsf* f, int channel, int bank, int preset_number)
{
	int preset_index;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	c->presetIndex = (unsigned short)preset_index;
	c->bank = (unsigned short)bank;
	return 1;
}

TSFDEF int tsf_channel_set_pan(tsf* f, int channel, float pan)
{
	struct tsf_voice *v, *vEnd;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	for (v = f->voices, vEnd = v + f->voiceNum; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
			tsf_voice_setpan(v, v->basePan + pan - 0.5f + v->modPan * 0.001f); // FatzAudio: pan modulators, drum pan
	c->panOffset = pan - 0.5f;
	return 1;
}

TSFDEF int tsf_channel_set_volume(tsf* f, int channel, float volume)
{
	float gainDB = tsf_gainToDecibels(volume), gainDBChange;
	struct tsf_voice *v, *vEnd;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (gainDB == c->gainDB) return 1;
	for (v = f->voices, vEnd = v + f->voiceNum, gainDBChange = gainDB - c->gainDB; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
			v->noteGainDB += gainDBChange;
	c->gainDB = gainDB;
	return 1;
}

TSFDEF int tsf_channel_set_pitchwheel(tsf* f, int channel, int pitch_wheel)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pitchWheel == pitch_wheel) return 1;
	c->pitchWheel = (unsigned short)pitch_wheel;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_pitchrange(tsf* f, int channel, float pitch_range)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pitchRange == pitch_range) return 1;
	c->pitchRange = pitch_range;
	if (c->pitchWheel != 8192) tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_tuning(tsf* f, int channel, float tuning)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->tuning == tuning) return 1;
	c->tuning = tuning;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF void tsf_set_tuning(tsf* f, float tuning) // FatzAudio
{
	int i;
	if (f->globalTuning == tuning) return;
	f->globalTuning = tuning;
	if (f->channels)
		for (i = 0; i < f->channels->channelNum; i++) tsf_channel_applypitch(f, i, &f->channels->channels[i]);
}

TSFDEF int tsf_channel_set_sustain(tsf* f, int channel, int flag_sustain)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (!c->sustain == !flag_sustain) return 1;
	c->sustain = (unsigned short)(flag_sustain != 0);
	//Turning on sustain does no action now, just starts note_off behaving differently
	if (flag_sustain) return 1;
	//Turning off sustain, actually end voices that got a note_off and were set to heldSustain status
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE && v->heldSustain &&
			!(c->sostenuto && v->sostenuto)) // FatzAudio: still held by sostenuto
			tsf_voice_end(f, v);
	return 1;
}

// FatzAudio: modulation ---------------------------------------------------------------------

// Re-evaluates the modulators of the channel's voices (of `key` only, when >= 0).
static void tsf_channel_modulate(tsf* f, int channel, int key)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && (key < 0 || v->playingKey == key))
			tsf_voice_modulate(f, v, TSF_FALSE);
}

TSFDEF int tsf_channel_set_sostenuto(tsf* f, int channel, int flag_sostenuto)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (!c->sostenuto == !flag_sostenuto) return 1;
	c->sostenuto = (unsigned char)(flag_sostenuto != 0);
	for (; v != vEnd; v++)
	{
		if (v->playingPreset == -1 || v->playingChannel != channel || v->ampenv.segment >= TSF_SEGMENT_RELEASE) continue;
		if (flag_sostenuto) { if (!v->heldSustain) v->sostenuto = 1; } // the keys down now
		else if (v->sostenuto)
		{
			v->sostenuto = 0;
			if (v->heldSustain && !c->sustain) tsf_voice_end(f, v);
		}
	}
	return 1;
}

TSFDEF int tsf_channel_set_pressure(tsf* f, int channel, int pressure)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->chanPressure == (pressure & 0x7F)) return 1;
	c->chanPressure = (unsigned char)(pressure & 0x7F);
	if (f->modGC & (1u << 13)) tsf_channel_modulate(f, channel, -1);
	return 1;
}

TSFDEF int tsf_channel_set_key_pressure(tsf* f, int channel, int key, int pressure)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->keyPressure[key & 0x7F] == (pressure & 0x7F)) return 1;
	c->keyPressure[key & 0x7F] = (unsigned char)(pressure & 0x7F);
	if (f->modGC & (1u << 10)) tsf_channel_modulate(f, channel, key & 0x7F);
	return 1;
}

TSFDEF int tsf_channel_set_pressure_vibrato(tsf* f, int channel, float cents)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pressureVibrato == cents) return 1;
	c->pressureVibrato = cents;
	tsf_channel_modulate(f, channel, -1);
	return 1;
}

TSFDEF int tsf_channel_set_scale_tuning(tsf* f, int channel, int note, float cents)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c || note < 0 || note > 11) return 0;
	if (c->scaleTune[note] == cents) return 1;
	c->scaleTune[note] = cents;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->playingKey % 12 == note)
		{
			v->scalePitch = cents;
			tsf_voice_calcpitchratio(v, tsf_channel_pitchshift(f, c), f->outSampleRate);
		}
	return 1;
}

TSFDEF int tsf_channel_set_drum_kit(tsf* f, int channel, int kit)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->drumKit = (signed char)(kit >= 0 && kit < TSF_DRUM_KITS ? kit : -1);
	return 1;
}

TSFDEF int tsf_set_drum_param(tsf* f, int kit, int key, int param, int value)
{
	struct tsf_drumkey* d;
	if (kit < 0 || kit >= TSF_DRUM_KITS || key < 0 || key > 127 || param < 0 || param >= TSF_DRUM_PARAMS) return 0;
	if (!f->drumKeys)
	{
		int i;
		f->drumKeys = (struct tsf_drumkey*)TSF_MALLOC(TSF_DRUM_KITS * 128 * sizeof(struct tsf_drumkey));
		if (!f->drumKeys) return 0;
		for (i = 0; i != TSF_DRUM_KITS * 128; i++) { f->drumKeys[i].edited = 0; TSF_MEMCPY(f->drumKeys[i].param, tsf_drumkey_defaults, TSF_DRUM_PARAMS); }
	}
	value &= 0x7F;
	if (param == TSF_DRUM_PAN && !value) value = 64; // "random": the middle
	d = &f->drumKeys[kit * 128 + key];
	d->param[param] = (unsigned char)value;
	d->edited = 1;
	return 1;
}

TSFDEF void tsf_reset_drum_kit(tsf* f, int kit)
{
	int i, first = (kit < 0 ? 0 : kit * 128), end = (kit < 0 ? TSF_DRUM_KITS * 128 : first + 128);
	if (!f->drumKeys || kit >= TSF_DRUM_KITS) return;
	for (i = first; i != end; i++) { f->drumKeys[i].edited = 0; TSF_MEMCPY(f->drumKeys[i].param, tsf_drumkey_defaults, TSF_DRUM_PARAMS); }
}

// GS and XG drum NRPNs (MSB 14-1F, LSB the key), on a channel with a drum kit. Pitch coarse
// and fine are offsets from 64, level, pan and sends absolute; XG's attack and decay are
// rates (higher is faster), so they lengthen the times as they go below 64.
static void tsf_nrpn_drum(tsf* f, int kit, unsigned short nrpn, int value)
{
	static const signed char param[12] = {
		TSF_DRUM_CUTOFF, TSF_DRUM_RESONANCE, TSF_DRUM_ATTACK, TSF_DRUM_DECAY, TSF_DRUM_PITCH_COARSE, TSF_DRUM_PITCH_FINE,
		TSF_DRUM_LEVEL, -1, TSF_DRUM_PAN, TSF_DRUM_REVERB, TSF_DRUM_CHORUS, TSF_DRUM_VARIATION };
	int msb = nrpn >> 7;
	if (msb < 0x14 || msb > 0x1F || param[msb - 0x14] < 0) return;
	if (msb == 0x16 || msb == 0x17) value = (value ? 128 - value : 127);
	tsf_set_drum_param(f, kit, nrpn & 0x7F, param[msb - 0x14], value);
}

// GS and XG part NRPNs (MSB 1) that set what the sound controllers set (BASSMIDI's mapping).
static int tsf_nrpn_controller(unsigned short nrpn)
{
	switch (nrpn)
	{
		case 0x0088: return 76; // 01 08 vibrato rate
		case 0x0089: return 77; // 01 09 vibrato depth
		case 0x008A: return 78; // 01 0A vibrato delay
		case 0x00A0: return 74; // 01 20 filter cutoff
		case 0x00A1: return 71; // 01 21 resonance
		case 0x00E3: return 73; // 01 63 attack
		case 0x00E4: return 75; // 01 64 decay
		case 0x00E6: return 72; // 01 66 release
	}
	return -1;
}

static int tsf_controller_modulates(tsf* f, int controller)
{
	return controller == 1 || (controller >= 71 && controller <= 78) || (f->modCC[controller >> 5] >> (controller & 31) & 1);
}

TSFDEF int tsf_channel_note_on(tsf* f, int channel, int key, float vel)
{
	struct tsf_channel* c;
	int res;
	if (!f->channels || channel >= f->channels->channelNum) return 1;
	f->channels->activeChannel = channel;
	if (!vel)
	{
		tsf_channel_note_off(f, channel, key);
		return 1;
	}
	// FatzAudio: portamento from CC84's key (once), else from the last key while CC65 is on
	c = &f->channels->channels[channel];
	{ const struct tsf_drumkey* d = tsf_drumkey_of(f, key); if (d && !d->param[TSF_DRUM_NOTE_ON]) return 1; } // FatzAudio: a drum key set not to receive note-ons
	c->portaFrom = (c->portaKey >= 0 ? c->portaKey : (c->cc[65] >= 64 ? c->lastKey : -1));
	c->portaKey = -1;
	res = tsf_note_on(f, c->presetIndex, key, vel);
	c->portaFrom = -1;
	c->lastKey = (short)key;
	return res;
}

TSFDEF void tsf_channel_note_off(tsf* f, int channel, int key)
{
	unsigned sustain;
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum, *vMatchFirst = TSF_NULL, *vMatchLast = TSF_NULL;
	for (; v != vEnd; v++)
	{
		//Find the first and last entry in the voices list with matching channel, key and look up the smallest play index
		if (v->playingPreset == -1 || v->playingChannel != channel || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE || v->heldSustain || v->drumHold) continue; // FatzAudio: drumHold
		else if (!vMatchFirst || v->playIndex < vMatchFirst->playIndex) vMatchFirst = vMatchLast = v;
		else if (v->playIndex == vMatchFirst->playIndex) vMatchLast = v;
	}
	if (!vMatchFirst) return;
	for (sustain = f->channels->channels[channel].sustain, v = vMatchFirst; v <= vMatchLast; v++)
	{
		//Stop all voices with matching channel, key and the smallest play index which was enumerated above
		if (v != vMatchFirst && v != vMatchLast &&
			(v->playIndex != vMatchFirst->playIndex || v->playingPreset == -1 || v->playingChannel != channel || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE || v->drumHold)) continue;
		//Don't turn off if sustain is active, just mark as held by sustain so we don't forget it
		if (sustain || (f->channels->channels[channel].sostenuto && v->sostenuto)) // FatzAudio: or sostenuto
			v->heldSustain = 1;
		else
			tsf_voice_end(f, v);
	}
}

TSFDEF void tsf_channel_note_off_all(tsf* f, int channel)
{
	//Ignore sustain channel settings, note_off_all overrides
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE)
			tsf_voice_end(f, v);
}

TSFDEF void tsf_channel_sounds_off_all(tsf* f, int channel)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && (v->ampenv.segment < TSF_SEGMENT_RELEASE || v->ampenv.parameters.release))
			tsf_voice_endquick(f, v);
}

TSFDEF int tsf_channel_midi_control(tsf* f, int channel, int controller, int control_value)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	if (!c) return 0;
	controller &= 0x7F; control_value &= 0x7F; // FatzAudio
	if (c->cc[controller] != control_value) // FatzAudio: modulators and sound controllers
	{
		c->cc[controller] = (unsigned char)control_value;
		if (tsf_controller_modulates(f, controller)) tsf_channel_modulate(f, channel, -1);
	}
	switch (controller)
	{
		case   7 /*VOLUME_MSB*/      : c->midiVolume     = (unsigned short)((c->midiVolume     & 0x7F  ) | (control_value << 7)); goto TCMC_SET_VOLUME;
		case  39 /*VOLUME_LSB*/      : c->midiVolume     = (unsigned short)((c->midiVolume     & 0x3F80) |  control_value);       goto TCMC_SET_VOLUME;
		case  11 /*EXPRESSION_MSB*/  : c->midiExpression = (unsigned short)((c->midiExpression & 0x7F  ) | (control_value << 7)); goto TCMC_SET_VOLUME;
		case  43 /*EXPRESSION_LSB*/  : c->midiExpression = (unsigned short)((c->midiExpression & 0x3F80) |  control_value);       goto TCMC_SET_VOLUME;
		case  10 /*PAN_MSB*/         : c->midiPan        = (unsigned short)((c->midiPan        & 0x7F  ) | (control_value << 7)); goto TCMC_SET_PAN;
		case  42 /*PAN_LSB*/         : c->midiPan        = (unsigned short)((c->midiPan        & 0x3F80) |  control_value);       goto TCMC_SET_PAN;
		case   6 /*DATA_ENTRY_MSB*/  : c->midiData       = (unsigned short)((c->midiData       & 0x7F)   | (control_value << 7)); goto TCMC_SET_DATA;
		case  38 /*DATA_ENTRY_LSB*/  : c->midiData       = (unsigned short)((c->midiData       & 0x3F80) |  control_value);       goto TCMC_SET_DATA;
		case   0 /*BANK_SELECT_MSB*/ : c->bank = (unsigned short)(0x8000 | control_value); return 1; //bank select MSB alone acts like LSB
		case  32 /*BANK_SELECT_LSB*/ : c->bank = (unsigned short)((c->bank & 0x8000 ? ((c->bank & 0x7F) << 7) : 0) | control_value); return 1;
		case 101 /*RPN_MSB*/         : c->midiRPN = (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x7F  ) | (control_value << 7)); c->midiNRPN = 0xFFFF; return 1;
		case 100 /*RPN_LSB*/         : c->midiRPN = (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x3F80) |  control_value); c->midiNRPN = 0xFFFF; return 1;
		case  98 /*NRPN_LSB*/        : c->midiRPN = 0xFFFF; c->midiNRPN = (unsigned short)(((c->midiNRPN == 0xFFFF ? 0 : c->midiNRPN) & 0x3F80) | control_value); return 1; // FatzAudio: NRPN kept
		case  99 /*NRPN_MSB*/        : c->midiRPN = 0xFFFF; c->midiNRPN = (unsigned short)(((c->midiNRPN == 0xFFFF ? 0 : c->midiNRPN) & 0x7F) | (control_value << 7)); return 1;
		case  64 /*SUSTAIN*/         : tsf_channel_set_sustain(f, channel, (int)(control_value >= 64)); return 1;
		case  66 /*SOSTENUTO*/       : tsf_channel_set_sostenuto(f, channel, (int)(control_value >= 64)); return 1; // FatzAudio
		case  84 /*PORTAMENTO_CTRL*/ : c->portaKey = (short)control_value; return 1; // FatzAudio
		case  91 /*REVERB*/          : c->midiReverb = (unsigned char)(control_value & 0x7F); return 1; // FatzAudio
		case  93 /*CHORUS*/          : c->midiChorus = (unsigned char)(control_value & 0x7F); return 1; // FatzAudio
		case 120 /*ALL_SOUND_OFF*/   : tsf_channel_sounds_off_all(f, channel); return 1;
		case 123 /*ALL_NOTES_OFF*/   : tsf_channel_note_off_all(f, channel);   return 1;
		case 121 /*ALL_CTRL_OFF*/    :
			c->midiVolume = c->midiExpression = 16383;
			c->midiPan = 8192;
			c->bank = 0;
			c->midiRPN = 0xFFFF;
			c->midiData = 0;
			tsf_channel_set_volume(f, channel, 1.0f);
			tsf_channel_set_pan(f, channel, 0.5f);
			tsf_channel_set_pitchrange(f, channel, 2.0f);
			tsf_channel_set_tuning(f, channel, 0);
			// FatzAudio: also what RP-015 resets (mod wheel, pressure, pedals but sustain, NRPN) and the
			// modulation depth range; sound controllers (CC70-79) and sends stay
			c->cc[7] = c->cc[11] = 127; c->cc[10] = 64;
			c->cc[1] = c->cc[33] = c->cc[65] = c->cc[66] = c->cc[67] = 0;
			c->chanPressure = 0;
			TSF_MEMSET(c->keyPressure, 0, sizeof(c->keyPressure));
			c->midiNRPN = 0xFFFF;
			c->portaKey = -1;
			c->modRange = 50.0f;
			tsf_channel_set_sostenuto(f, channel, 0);
			tsf_channel_modulate(f, channel, -1);
			return 1;
	}
	return 1;
TCMC_SET_VOLUME:
	//Raising to the power of 3 seems to result in a decent sounding volume curve for MIDI
	tsf_channel_set_volume(f, channel, TSF_POWF((c->midiVolume / 16383.0f) * (c->midiExpression / 16383.0f), 3.0f));
	return 1;
TCMC_SET_PAN:
	tsf_channel_set_pan(f, channel, c->midiPan / 16383.0f);
	return 1;
TCMC_SET_DATA:
	if (c->midiNRPN != 0xFFFF) // FatzAudio: GS/XG part NRPNs, data entry MSB
	{
		int target = tsf_nrpn_controller(c->midiNRPN);
		if (controller == 6 && c->drumKit >= 0) tsf_nrpn_drum(f, c->drumKit, c->midiNRPN, control_value);
		if (target >= 0 && controller == 6 && c->cc[target] != control_value)
		{
			c->cc[target] = (unsigned char)control_value;
			tsf_channel_modulate(f, channel, -1);
		}
		return 1;
	}
	if (c->midiRPN == 5) // FatzAudio: modulation depth range, semitones and 1/128 semitones
	{
		c->modRange = (c->midiData >> 7) * 100.0f + (c->midiData & 0x7F) * (100.0f / 128.0f);
		tsf_channel_modulate(f, channel, -1);
		return 1;
	}
	if      (c->midiRPN == 0) tsf_channel_set_pitchrange(f, channel, (c->midiData >> 7) + 0.01f * (c->midiData & 0x7F));
	else if (c->midiRPN == 1) tsf_channel_set_tuning(f, channel, (int)c->tuning + ((float)c->midiData - 8192.0f) / 8192.0f); //fine tune
	else if (c->midiRPN == 2 && controller == 6) tsf_channel_set_tuning(f, channel, ((float)control_value - 64.0f) + (c->tuning - (int)c->tuning)); //coarse tune
	return 1;
}

TSFDEF int tsf_channel_get_preset_index(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].presetIndex : 0);
}

TSFDEF int tsf_channel_get_preset_bank(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? (f->channels->channels[channel].bank & 0x7FFF) : 0);
}

TSFDEF int tsf_channel_get_preset_number(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->presets[f->channels->channels[channel].presetIndex].preset : 0);
}

TSFDEF float tsf_channel_get_pan(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].panOffset - 0.5f : 0.5f);
}

TSFDEF float tsf_channel_get_volume(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? tsf_decibelsToGain(f->channels->channels[channel].gainDB) : 1.0f);
}

TSFDEF int tsf_channel_get_pitchwheel(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].pitchWheel : 8192);
}

TSFDEF float tsf_channel_get_pitchrange(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].pitchRange : 2.0f);
}

TSFDEF float tsf_channel_get_tuning(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].tuning : 0.0f);
}

TSFDEF int tsf_channel_get_midi_control(tsf* f, int channel, int controller) // FatzAudio
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].cc[controller & 0x7F] : 0);
}

#ifdef __cplusplus
}
#endif

#endif //TSF_IMPLEMENTATION
