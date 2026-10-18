/* TinyMidiLoader - v0.7 - Minimalistic midi parsing library - https://github.com/schellingb/TinySoundFont
                                     no warranty implied; use at your own risk

   ALTERED SOURCE VERSION (FatzAudio): this is not the original tml.h. tml_load
   was rewritten for untrusted input and exact timing, and text meta events are
   kept; every change is marked "FatzAudio" and listed in README.md next to
   this file.
   Do this:
      #define TML_IMPLEMENTATION
   before you include this file in *one* C or C++ file to create the implementation.
   // i.e. it should look like this:
   #include ...
   #include ...
   #define TML_IMPLEMENTATION
   #include "tml.h"

   [OPTIONAL] #define TML_NO_STDIO to remove stdio dependency
   [OPTIONAL] #define TML_MALLOC, TML_REALLOC, and TML_FREE to avoid stdlib.h
   [OPTIONAL] #define TML_MEMCPY to avoid string.h

   LICENSE (ZLIB)

   Copyright (C) 2017, 2018, 2020 Bernhard Schelling

   This software is provided 'as-is', without any express or implied
   warranty.  In no event will the authors be held liable for any damages
   arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
      claim that you wrote the original software. If you use this software
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be
      misrepresented as being the original software.
   3. This notice may not be removed or altered from any source distribution.

*/

#ifndef TML_INCLUDE_TML_INL
#define TML_INCLUDE_TML_INL

#ifdef __cplusplus
extern "C" {
#endif

// Define this if you want the API functions to be static
#ifdef TML_STATIC
#define TMLDEF static
#else
#define TMLDEF extern
#endif

// Channel message type
enum TMLMessageType
{
	TML_NOTE_OFF = 0x80, TML_NOTE_ON = 0x90, TML_KEY_PRESSURE = 0xA0, TML_CONTROL_CHANGE = 0xB0, TML_PROGRAM_CHANGE = 0xC0, TML_CHANNEL_PRESSURE = 0xD0, TML_PITCH_BEND = 0xE0, TML_SET_TEMPO = 0x51
};

// FatzAudio: meta event types, public so text messages (types TML_TEXT to
// TML_CUE_POINT) can be told apart. Messages of these types carry `text`.
enum TMLSystemType
{
	TML_TEXT  = 0x01, TML_COPYRIGHT    = 0x02, TML_TRACK_NAME     = 0x03, TML_INST_NAME     = 0x04, TML_LYRIC           = 0x05, TML_MARKER       = 0x06, TML_CUE_POINT = 0x07,
	TML_EOT   = 0x2f, TML_SMPTE_OFFSET = 0x54, TML_TIME_SIGNATURE = 0x58, TML_KEY_SIGNATURE = 0x59, TML_SEQUENCER_EVENT = 0x7f,
	TML_SYSEX = 0xf0, TML_TIME_CODE    = 0xf1, TML_SONG_POSITION  = 0xf2, TML_SONG_SELECT   = 0xf3, TML_TUNE_REQUEST    = 0xf6, TML_EOX          = 0xf7, TML_SYNC      = 0xf8,
	TML_TICK  = 0xf9, TML_START        = 0xfa, TML_CONTINUE       = 0xfb, TML_STOP          = 0xfc, TML_ACTIVE_SENSING  = 0xfe, TML_SYSTEM_RESET = 0xff
};

// Midi controller numbers
enum TMLController
{
	TML_BANK_SELECT_MSB,      TML_MODULATIONWHEEL_MSB, TML_BREATH_MSB,       TML_FOOT_MSB = 4,      TML_PORTAMENTO_TIME_MSB,   TML_DATA_ENTRY_MSB, TML_VOLUME_MSB,
	TML_BALANCE_MSB,          TML_PAN_MSB = 10,        TML_EXPRESSION_MSB,   TML_EFFECTS1_MSB,      TML_EFFECTS2_MSB,          TML_GPC1_MSB = 16, TML_GPC2_MSB, TML_GPC3_MSB, TML_GPC4_MSB,
	TML_BANK_SELECT_LSB = 32, TML_MODULATIONWHEEL_LSB, TML_BREATH_LSB,       TML_FOOT_LSB = 36,     TML_PORTAMENTO_TIME_LSB,   TML_DATA_ENTRY_LSB, TML_VOLUME_LSB,
	TML_BALANCE_LSB,          TML_PAN_LSB = 42,        TML_EXPRESSION_LSB,   TML_EFFECTS1_LSB,      TML_EFFECTS2_LSB,          TML_GPC1_LSB = 48, TML_GPC2_LSB, TML_GPC3_LSB, TML_GPC4_LSB,
	TML_SUSTAIN_SWITCH = 64,  TML_PORTAMENTO_SWITCH,   TML_SOSTENUTO_SWITCH, TML_SOFT_PEDAL_SWITCH, TML_LEGATO_SWITCH,         TML_HOLD2_SWITCH,
	TML_SOUND_CTRL1,          TML_SOUND_CTRL2,         TML_SOUND_CTRL3,      TML_SOUND_CTRL4,       TML_SOUND_CTRL5,           TML_SOUND_CTRL6,
	TML_SOUND_CTRL7,          TML_SOUND_CTRL8,         TML_SOUND_CTRL9,      TML_SOUND_CTRL10,      TML_GPC5, TML_GPC6,        TML_GPC7, TML_GPC8,
	TML_PORTAMENTO_CTRL,      TML_FX_REVERB = 91,      TML_FX_TREMOLO,       TML_FX_CHORUS,         TML_FX_CELESTE_DETUNE,     TML_FX_PHASER,
	TML_DATA_ENTRY_INCR,      TML_DATA_ENTRY_DECR,     TML_NRPN_LSB,         TML_NRPN_MSB,          TML_RPN_LSB,               TML_RPN_MSB,
	TML_ALL_SOUND_OFF = 120,  TML_ALL_CTRL_OFF,        TML_LOCAL_CONTROL,    TML_ALL_NOTES_OFF,     TML_OMNI_OFF, TML_OMNI_ON, TML_POLY_OFF, TML_POLY_ON
};

// A single MIDI message linked to the next message in time
typedef struct tml_message
{
	// Time of the message in milliseconds
	unsigned int time;

	// Type (see TMLMessageType) and channel number
	unsigned char type, channel;

	// 2 byte of parameter data based on the type:
	// - key, velocity for TML_NOTE_ON and TML_NOTE_OFF messages
	// - key, key_pressure for TML_KEY_PRESSURE messages
	// - control, control_value for TML_CONTROL_CHANGE messages (see TMLController)
	// - program for TML_PROGRAM_CHANGE messages
	// - channel_pressure for TML_CHANNEL_PRESSURE messages
	// - pitch_bend for TML_PITCH_BEND messages
	union
	{
		#ifdef _MSC_VER
		#pragma warning(push)
		#pragma warning(disable:4201) //nonstandard extension used: nameless struct/union
		#elif defined(__GNUC__)
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wpedantic" //ISO C++ prohibits anonymous structs
		#endif

		struct { union { char key, control, program, channel_pressure; }; union { char velocity, key_pressure, control_value; }; };
		struct { unsigned short pitch_bend; };

		#ifdef _MSC_VER
		#pragma warning( pop )
		#elif defined(__GNUC__)
		#pragma GCC diagnostic pop
		#endif
	};

	// The pointer to the next message in time following this event
	struct tml_message* next;

	// FatzAudio: exact time of the message in seconds (`time` is truncated to ms)
	double seconds;

	// FatzAudio: text of TML_TEXT .. TML_CUE_POINT messages, NUL-terminated (the
	// raw bytes of the file, any encoding), and its length; the data bytes of
	// TML_TIME_SIGNATURE, TML_KEY_SIGNATURE and TML_SYSEX (up to 64 bytes, without
	// the F0) the same way; NULL for other types
	const char* text;
	unsigned int text_length;

	// FatzAudio: track (0-based MTrk chunk) the message came from
	unsigned short track;

	// FatzAudio: internal, offset of the text while loading
	unsigned int text_offset;
} tml_message;

// The load functions will return a pointer to a struct tml_message.
// Normally the linked list gets traversed by following the next pointers.
// Make sure to keep the pointer to the first message to free the memory.
// On error the tml_load* functions will return NULL most likely due to an
// invalid MIDI stream (or if the file did not exist in tml_load_filename).

#ifndef TML_NO_STDIO
// Directly load a MIDI file from a .mid file path
TMLDEF tml_message* tml_load_filename(const char* filename);
#endif

// Load a MIDI file from a block of memory
// (FatzAudio: a negative size is refused)
TMLDEF tml_message* tml_load_memory(const void* buffer, int size);

// Get infos about this loaded MIDI file, returns the note count
// NULL can be passed for any output value pointer if not needed.
//   used_channels:   Will be set to how many channels play notes
//                    (i.e. 1 if channel 15 is used but no other)
//   used_programs:   Will be set to how many different programs are used
//   total_notes:     Will be set to the total number of note on messages
//   time_first_note: Will be set to the time of the first note on message
//   time_length:     Will be set to the total time in milliseconds
TMLDEF int tml_get_info(tml_message* first_message, int* used_channels, int* used_programs, int* total_notes, unsigned int* time_first_note, unsigned int* time_length);

// Read the tempo (microseconds per quarter note) value from a message with the type TML_SET_TEMPO
TMLDEF int tml_get_tempo_value(tml_message* set_tempo_message);

// Free all the memory of the linked message list (can also call free() manually)
TMLDEF void tml_free(tml_message* f);

// Stream structure for the generic loading
struct tml_stream
{
	// Custom data given to the functions as the first parameter
	void* data;

	// Function pointer will be called to read 'size' bytes into ptr (returns number of read bytes)
	int (*read)(void* data, void* ptr, unsigned int size);
};

// Generic Midi loading method using the stream structure above
TMLDEF tml_message* tml_load(struct tml_stream* stream);

// If this library is used together with TinySoundFont, tsf_stream (equivalent to tml_stream) can also be used
struct tsf_stream;
TMLDEF tml_message* tml_load_tsf_stream(struct tsf_stream* stream);

#ifdef __cplusplus
}
#endif

// end header
// ---------------------------------------------------------------------------------------------------------
#endif //TML_INCLUDE_TML_INL

#ifdef TML_IMPLEMENTATION

#if !defined(TML_MALLOC) || !defined(TML_FREE) || !defined(TML_REALLOC)
#  include <stdlib.h>
#  define TML_MALLOC  malloc
#  define TML_FREE    free
#  define TML_REALLOC realloc
#endif

#if !defined(TML_MEMCPY) || !defined(TML_MEMSET)
#  include <string.h>
#  define TML_MEMCPY  memcpy
#  define TML_MEMSET  memset
#endif

#ifndef TML_NO_STDIO
#  include <stdio.h>
#endif

#define TML_NULL 0

////crash on errors and warnings to find broken midi files while debugging
//#define TML_ERROR(msg) *(int*)0 = 0xbad;
//#define TML_WARN(msg)  *(int*)0 = 0xf00d;

////print errors and warnings
//#define TML_ERROR(msg) printf("ERROR: %s\n", msg);
//#define TML_WARN(msg)  printf("WARNING: %s\n", msg);

#ifndef TML_ERROR
#define TML_ERROR(msg)
#endif

#ifndef TML_WARN
#define TML_WARN(msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TML_NO_STDIO
static int tml_stream_stdio_read(FILE* f, void* ptr, unsigned int size) { return (int)fread(ptr, 1, size, f); }
TMLDEF tml_message* tml_load_filename(const char* filename)
{
	struct tml_message* res;
	struct tml_stream stream = { TML_NULL, (int(*)(void*,void*,unsigned int))&tml_stream_stdio_read };
	#if __STDC_WANT_SECURE_LIB__
	FILE* f = TML_NULL; fopen_s(&f, filename, "rb");
	#else
	FILE* f = fopen(filename, "rb");
	#endif
	if (!f) { TML_ERROR("File not found"); return 0; }
	stream.data = f;
	res = tml_load(&stream);
	fclose(f);
	return res;
}
#endif

struct tml_stream_memory { const char* buffer; unsigned int total, pos; };
static int tml_stream_memory_read(struct tml_stream_memory* m, void* ptr, unsigned int size) { if (size > m->total - m->pos) size = m->total - m->pos; TML_MEMCPY(ptr, m->buffer+m->pos, size); m->pos += size; return size; }
TMLDEF struct tml_message* tml_load_memory(const void* buffer, int size)
{
	struct tml_stream stream = { TML_NULL, (int(*)(void*,void*,unsigned int))&tml_stream_memory_read };
	struct tml_stream_memory f = { 0, 0, 0 };
	if (!buffer || size < 0) return TML_NULL; // FatzAudio
	f.buffer = (const char*)buffer;
	f.total = size;
	stream.data = &f;
	return tml_load(&stream);
}

// FatzAudio: limits for untrusted files. A track larger than this, or a file with
// more messages, is cut there (what was read so far still plays).
#ifndef TML_MAX_TRACK_BYTES
#define TML_MAX_TRACK_BYTES 0x4000000
#endif
#ifndef TML_MAX_MESSAGES
#define TML_MAX_MESSAGES 0x400000
#endif

struct tml_track
{
	unsigned int Idx, End;
	unsigned long long Ticks; // FatzAudio: 64-bit absolute ticks of the message at Idx
};

struct tml_tempomsg
{
	unsigned int time;
	unsigned char type, Tempo[3];
	tml_message* next;
};

struct tml_parser
{
	unsigned char *buf, *buf_end;
	int last_status, message_array_size, message_count;
	// FatzAudio: text of meta events, and the track being parsed
	char* text;
	unsigned int text_size, text_cap;
	unsigned short track;
};

static int tml_readbyte(struct tml_parser* p)
{
	return (p->buf == p->buf_end ? -1 : *(p->buf++));
}

static int tml_readvariablelength(struct tml_parser* p)
{
	unsigned int res = 0, i = 0;
	unsigned char c;
	for (; i != 4; i++)
	{
		if (p->buf == p->buf_end) { TML_WARN("Unexpected end of file"); return -1; }
		c = *(p->buf++);
		if (c & 0x80) res = ((res | (c & 0x7F)) << 7);
		else return (int)(res | c);
	}
	TML_WARN("Invalid variable length byte count"); return -1;
}

// FatzAudio: keeps `len` bytes of text (plus a NUL) in the parser's pool.
static int tml_keeptext(struct tml_parser* p, tml_message* evt, const unsigned char* data, int len)
{
	if (p->text_cap - p->text_size < (unsigned int)len + 1)
	{
		unsigned int cap = p->text_cap ? p->text_cap : 4096;
		char* grown;
		while (cap - p->text_size < (unsigned int)len + 1) { if (cap > 0x10000000) return 0; cap *= 2; }
		grown = (char*)TML_REALLOC(p->text, cap);
		if (!grown) return 0;
		p->text = grown;
		p->text_cap = cap;
	}
	evt->text_offset = p->text_size;
	evt->text_length = (unsigned int)len;
	if (len) TML_MEMCPY(p->text + p->text_size, data, len);
	p->text[p->text_size + len] = '\0';
	p->text_size += (unsigned int)len + 1;
	return 1;
}

static int tml_parsemessage(tml_message** f, struct tml_parser* p)
{
	int deltatime = tml_readvariablelength(p), status = tml_readbyte(p);
	tml_message* evt;

	if (deltatime < 0) { TML_WARN("Unexpected end of file"); return -1; } // FatzAudio
	if (deltatime & 0xFFF00000) deltatime = 0; //throw away delays that are insanely high for malformatted midis
	if (status < 0) { TML_WARN("Unexpected end of file"); return -1; }
	if ((status & 0x80) == 0)
	{
		// Invalid, use same status as before
		if ((p->last_status & 0x80) == 0) { TML_WARN("Undefined status and invalid running status"); return -1; }
		p->buf--;
		status = p->last_status;
	}
	else if (status < 0xF0) p->last_status = status; // FatzAudio: only channel messages set the running status

	if (p->message_array_size == p->message_count)
	{
		//start allocated memory size of message array at 64, double each time until 8192, then add 1024 entries until done
		tml_message* grown;
		if (p->message_count >= TML_MAX_MESSAGES) { TML_WARN("Too many messages"); return -1; } // FatzAudio
		p->message_array_size += (!p->message_array_size ? 64 : (p->message_array_size > 4096 ? 1024 : p->message_array_size));
		grown = (tml_message*)TML_REALLOC(*f, p->message_array_size * sizeof(tml_message));
		if (!grown) { TML_ERROR("Out of memory"); return -1; } // FatzAudio: *f stays valid for the caller to free
		*f = grown;
	}
	evt = *f + p->message_count;
	TML_MEMSET(evt, 0, sizeof(*evt)); // FatzAudio
	evt->track = p->track;

	//check what message we have
	if ((status == TML_SYSEX) || (status == TML_EOX)) //sysex
	{
		int len = tml_readvariablelength(p);
		if (len < 0 || len > p->buf_end - p->buf) { TML_WARN("Unexpected end of file"); p->buf = p->buf_end; return -1; } // FatzAudio
		// FatzAudio: short sysex messages (GM/GS/XG resets, drum parts, master volume) are kept, without the F0
		if (status == TML_SYSEX && len <= 64)
		{
			if (!tml_keeptext(p, evt, p->buf, len)) { TML_ERROR("Out of memory"); return -1; }
			evt->type = TML_SYSEX;
		}
		else evt->type = 0;
		p->buf += len;
	}
	else if (status == 0xFF) //meta events
	{
		int meta_type = tml_readbyte(p), buflen = tml_readvariablelength(p);
		unsigned char* metadata = p->buf;
		if (meta_type < 0 || buflen < 0) { TML_WARN("Unexpected end of file"); return -1; }
		if (buflen > p->buf_end - p->buf) { TML_WARN("Unexpected end of file"); p->buf = p->buf_end; return -1; } // FatzAudio
		p->buf += buflen;

		switch (meta_type)
		{
			case TML_EOT:
				if (buflen != 0) { TML_WARN("Invalid length for EndOfTrack event"); return -1; }
				if (!deltatime) return TML_EOT; //no need to store this message
				evt->type = TML_EOT;
				break;

			case TML_SET_TEMPO:
				if (buflen != 3) { TML_WARN("Invalid length for SetTempo meta event"); return -1; }
				evt->type = TML_SET_TEMPO;
				((struct tml_tempomsg*)evt)->Tempo[0] = metadata[0];
				((struct tml_tempomsg*)evt)->Tempo[1] = metadata[1];
				((struct tml_tempomsg*)evt)->Tempo[2] = metadata[2];
				break;

			// FatzAudio: text events are kept (lyrics, markers, names, copyright)
			case TML_TEXT: case TML_COPYRIGHT: case TML_TRACK_NAME: case TML_INST_NAME:
			case TML_LYRIC: case TML_MARKER: case TML_CUE_POINT:
			case TML_TIME_SIGNATURE: case TML_KEY_SIGNATURE: // FatzAudio: their bytes, as text
				if (!tml_keeptext(p, evt, metadata, buflen)) { TML_ERROR("Out of memory"); return -1; }
				evt->type = (unsigned char)meta_type;
				break;

			default:
				evt->type = 0;
		}
	}
	else if (status >= 0xF0) //FatzAudio: system common and real-time messages do not belong in files; skip their data bytes
	{
		while (p->buf != p->buf_end && !(*p->buf & 0x80)) p->buf++;
		evt->type = 0;
	}
	else //channel message
	{
		int param;
		if ((param = tml_readbyte(p)) < 0) { TML_WARN("Unexpected end of file"); return -1; }
		evt->key = (param & 0x7f);
		evt->channel = (status & 0x0f);
		switch (evt->type = (status & 0xf0))
		{
			case TML_NOTE_OFF:
			case TML_NOTE_ON:
			case TML_KEY_PRESSURE:
			case TML_CONTROL_CHANGE:
				if ((param = tml_readbyte(p)) < 0) { TML_WARN("Unexpected end of file"); return -1; }
				evt->velocity = (param & 0x7f);
				break;

			case TML_PITCH_BEND:
				if ((param = tml_readbyte(p)) < 0) { TML_WARN("Unexpected end of file"); return -1; }
				evt->pitch_bend = ((param & 0x7f) << 7) | evt->key;
				break;

			case TML_PROGRAM_CHANGE:
			case TML_CHANNEL_PRESSURE:
				evt->velocity = 0;
				break;

			default: //ignore system/manufacture messages
				evt->type = 0;
				break;
		}
	}

	if (deltatime || evt->type)
	{
		evt->time = deltatime;
		p->message_count++;
	}
	return evt->type;
}

// FatzAudio: min-heap of tracks by (Ticks, track index), for merging the tracks
// in time order in O(messages * log tracks).
static int tml_track_before(const struct tml_track* tracks, unsigned short a, unsigned short b)
{
	return tracks[a].Ticks < tracks[b].Ticks || (tracks[a].Ticks == tracks[b].Ticks && a < b);
}

static void tml_heap_down(const struct tml_track* tracks, unsigned short* heap, int n, int i)
{
	for (;;)
	{
		int l = 2 * i + 1, r = l + 1, m = i;
		unsigned short swap;
		if (l < n && tml_track_before(tracks, heap[l], heap[m])) m = l;
		if (r < n && tml_track_before(tracks, heap[r], heap[m])) m = r;
		if (m == i) return;
		swap = heap[i]; heap[i] = heap[m]; heap[m] = swap;
		i = m;
	}
}

TMLDEF tml_message* tml_load(struct tml_stream* stream)
{
	int num_tracks, division, header_length, trackbufsize = 0, truncated = 0;
	unsigned char midi_header[14], *trackbuf = TML_NULL;
	struct tml_message* messages = TML_NULL;
	struct tml_message* sorted = TML_NULL;
	struct tml_track *tracks, *t, *tracksEnd;
	struct tml_parser p;
	double smpte_seconds_per_tick = 0;

	TML_MEMSET(&p, 0, sizeof(p));

	// Parse MIDI header
	if (stream->read(stream->data, midi_header, 14) != 14) { TML_ERROR("Unexpected end of file"); return messages; }
	// FatzAudio: the header length is 32 bits and may exceed 6 (extra bytes are skipped)
	header_length = (int)(((unsigned int)midi_header[4] << 24) | (midi_header[5] << 16) | (midi_header[6] << 8) | midi_header[7]);
	if (midi_header[0] != 'M' || midi_header[1] != 'T' || midi_header[2] != 'h' || midi_header[3] != 'd' ||
	    header_length < 6 || header_length > 256 || midi_header[8] != 0 || midi_header[9] > 2) { TML_ERROR("Doesn't look like a MIDI file: invalid MThd header"); return messages; }
	num_tracks = (int)(midi_header[10] << 8) | midi_header[11];
	division = (int)(midi_header[12] << 8) | midi_header[13]; //division is ticks per beat (quarter-note)
	if (midi_header[12] & 0x80)
	{
		// FatzAudio: SMPTE timing, frames per second (negative) and ticks per frame
		int fps = -(signed char)midi_header[12], tpf = midi_header[13];
		if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) || tpf == 0) { TML_ERROR("Invalid SMPTE timing"); return messages; }
		smpte_seconds_per_tick = 1.0 / ((fps == 29 ? 29.97 : (double)fps) * tpf);
	}
	if (num_tracks <= 0 || division <= 0) { TML_ERROR("Doesn't look like a MIDI file: invalid track or division values"); return messages; } // FatzAudio: || (was &&)
	if (header_length > 6)
	{
		unsigned char skip[256];
		if (stream->read(stream->data, skip, (unsigned int)(header_length - 6)) != header_length - 6) { TML_ERROR("Unexpected end of file"); return messages; }
	}

	// Allocate temporary tracks array for parsing
	tracks = (struct tml_track*)TML_MALLOC(sizeof(struct tml_track) * num_tracks);
	if (!tracks) { TML_ERROR("Out of memory"); return messages; } // FatzAudio
	tracksEnd = &tracks[num_tracks];
	for (t = tracks; t != tracksEnd; t++) t->Idx = t->End = 0, t->Ticks = 0;

	// Read all messages for all tracks
	for (t = tracks; t != tracksEnd;)
	{
		unsigned char track_header[8];
		unsigned int track_length;
		int is_track;
		if (stream->read(stream->data, track_header, 8) != 8) { TML_WARN("Unexpected end of file"); break; }
		// FatzAudio: chunks other than MTrk (vendor extensions) are skipped, not the end
		is_track = (track_header[0] == 'M' && track_header[1] == 'T' && track_header[2] == 'r' && track_header[3] == 'k');

		// Get size of track data and read into buffer (allocate bigger buffer if needed)
		track_length = track_header[7] | (track_header[6] << 8) | (track_header[5] << 16) | ((unsigned int)track_header[4] << 24);
		if (track_length > TML_MAX_TRACK_BYTES) { TML_WARN("Invalid MTrk header"); break; }
		if (trackbufsize < (int)track_length)
		{
			TML_FREE(trackbuf);
			trackbuf = (unsigned char*)TML_MALLOC(track_length);
			trackbufsize = trackbuf ? (int)track_length : 0;
			if (!trackbuf) { TML_ERROR("Out of memory"); break; } // FatzAudio
		}
		if (track_length)
		{
			// FatzAudio: a truncated last chunk still plays what it holds
			int got = stream->read(stream->data, trackbuf, track_length);
			if (got < 0) got = 0;
			if ((unsigned int)got != track_length) { TML_WARN("Unexpected end of file"); truncated = 1; track_length = (unsigned int)got; }
		}
		if (!is_track) { if (truncated) break; continue; }

		t->Idx = p.message_count;
		p.track = (unsigned short)(t - tracks);
		p.last_status = 0;
		for (p.buf_end = (p.buf = trackbuf) + track_length; p.buf != p.buf_end;)
		{
			int type = tml_parsemessage(&messages, &p);
			if (type == TML_EOT || type < 0) break; //file end or illegal data encountered
		}
		if (p.buf != p.buf_end) { TML_WARN( "Track length did not match data length"); }
		t->End = p.message_count;
		t++;
		if (truncated) break;
	}
	tracksEnd = t; // FatzAudio: only the tracks that were read
	TML_FREE(trackbuf);

	// FatzAudio: merge the tracks in time order into a new array (followed by the
	// text), with exact times from 64-bit tick counts. Equal ticks keep the order of
	// the tracks, then of the file. Placeholder messages (type 0) are dropped.
	if (p.message_count)
	{
		int heap_n = 0, out = 0, i;
		unsigned short* heap = (unsigned short*)TML_MALLOC(sizeof(unsigned short) * (num_tracks ? num_tracks : 1));
		unsigned long long tempo_ticks = 0; //tick count at the last tempo change
		double tempo_seconds = 0.0, seconds_per_tick = (smpte_seconds_per_tick > 0 ? smpte_seconds_per_tick : 0.5 / division);
		sorted = heap ? (tml_message*)TML_MALLOC(sizeof(tml_message) * p.message_count + p.text_size + 1) : TML_NULL;
		if (!sorted) { TML_ERROR("Out of memory"); p.message_count = 0; }
		else
		{
			// FatzAudio: format 2 tracks are independent patterns played one after another
			unsigned long long base = 0;
			for (t = tracks; t != tracksEnd; t++)
				if (t->Idx != t->End)
				{
					t->Ticks = base + messages[t->Idx].time;
					heap[heap_n++] = (unsigned short)(t - tracks);
					if (midi_header[9] == 2) { unsigned int k; for (k = t->Idx; k != t->End; k++) base += messages[k].time; }
				}
			for (i = heap_n / 2 - 1; i >= 0; i--) tml_heap_down(tracks, heap, heap_n, i);
			while (heap_n)
			{
				tml_message* msg;
				double seconds;
				t = &tracks[heap[0]];
				msg = &messages[t->Idx];
				seconds = tempo_seconds + (double)(t->Ticks - tempo_ticks) * seconds_per_tick;
				if (msg->type == TML_SET_TEMPO && smpte_seconds_per_tick <= 0)
				{
					unsigned char* Tempo = ((struct tml_tempomsg*)msg)->Tempo;
					int tempo = (Tempo[0]<<16)|(Tempo[1]<<8)|Tempo[2];
					if (tempo > 0)
					{
						seconds_per_tick = tempo / (1000000.0 * division);
						tempo_seconds = seconds;
						tempo_ticks = t->Ticks;
					}
				}
				if (msg->type)
				{
					tml_message* o = &sorted[out++];
					*o = *msg;
					o->seconds = seconds;
					o->time = (seconds * 1000.0 < 4294967295.0 ? (unsigned int)(seconds * 1000.0) : 0xFFFFFFFFu);
				}
				if (++t->Idx != t->End) t->Ticks += messages[t->Idx].time;
				else heap[0] = heap[--heap_n];
				tml_heap_down(tracks, heap, heap_n, 0);
			}
			p.message_count = out;
			if (p.text_size) TML_MEMCPY((char*)(sorted + p.message_count), p.text, p.text_size);
			for (i = 0; i < out; i++)
			{
				sorted[i].next = (i + 1 < out ? &sorted[i + 1] : TML_NULL);
				if ((sorted[i].type >= TML_TEXT && sorted[i].type <= TML_CUE_POINT) || sorted[i].type == TML_TIME_SIGNATURE || sorted[i].type == TML_KEY_SIGNATURE || sorted[i].type == TML_SYSEX) sorted[i].text = (const char*)(sorted + p.message_count) + sorted[i].text_offset;
				else sorted[i].text = TML_NULL;
			}
		}
		TML_FREE(heap);
	}
	TML_FREE(tracks);
	TML_FREE(messages);
	TML_FREE(p.text);

	if (p.message_count == 0)
	{
		TML_FREE(sorted);
		sorted = TML_NULL;
	}

	return sorted;
}

TMLDEF tml_message* tml_load_tsf_stream(struct tsf_stream* stream)
{
	return tml_load((struct tml_stream*)stream);
}

TMLDEF int tml_get_info(tml_message* Msg, int* out_used_channels, int* out_used_programs, int* out_total_notes, unsigned int* out_time_first_note, unsigned int* out_time_length)
{
	int used_programs = 0, used_channels = 0, total_notes = 0;
	unsigned int time_first_note = 0xffffffff, time_length = 0;
	unsigned char channels[16] = { 0 }, programs[128] = { 0 };
	for (;Msg; Msg = Msg->next)
	{
		time_length = Msg->time;
		if (Msg->type == TML_PROGRAM_CHANGE && !programs[(int)Msg->program]) { programs[(int)Msg->program] = 1; used_programs++; }
		if (Msg->type != TML_NOTE_ON) continue;
		if (time_first_note == 0xffffffff) time_first_note = time_length;
		if (!channels[Msg->channel]) { channels[Msg->channel] = 1; used_channels++; }
		total_notes++;
	}
	if (time_first_note == 0xffffffff) time_first_note = 0;
	if (out_used_channels  ) *out_used_channels   = used_channels;
	if (out_used_programs  ) *out_used_programs   = used_programs;
	if (out_total_notes    ) *out_total_notes     = total_notes;
	if (out_time_first_note) *out_time_first_note = time_first_note;
	if (out_time_length    ) *out_time_length     = time_length;
	return total_notes;
}

TMLDEF int tml_get_tempo_value(tml_message* msg)
{
	unsigned char* Tempo;
	if (!msg || msg->type != TML_SET_TEMPO) return 0;
	Tempo = ((struct tml_tempomsg*)msg)->Tempo;
	return ((Tempo[0]<<16)|(Tempo[1]<<8)|Tempo[2]);
}

TMLDEF void tml_free(tml_message* f)
{
	TML_FREE(f);
}

#ifdef __cplusplus
}
#endif

#endif //TML_IMPLEMENTATION
