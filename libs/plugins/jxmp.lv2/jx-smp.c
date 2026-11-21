/*
 * Copyright (C) 2026 JXMP Audio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* JX-SMP: one-sample player. MIDI notes in a range play the sample,
 * transposed from its root note (or always at its own pitch), with
 * velocity to level, ADSR, loop, start offset, voice limit and trigger
 * probability. The file is loaded by the host's worker thread and its path
 * is saved with the session. */

#include "jxmp.h"

#include <sndfile.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

enum {
	P_CONTROL = 0, P_NOTIFY, P_OUT_L, P_OUT_R,
	P_VOLUME, P_MIN_VOLUME, P_PAN, P_MODE, P_NOTE_LO, P_NOTE_HI, P_ROOT, P_TUNE, P_BEND_RANGE,
	P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_OBEY_OFF, P_LOOP, P_LOOP_START, P_START,
	P_VOICES, P_PROBABILITY, P_CHANNEL,
	P_LAST
};

#define NV     32
#define NPEAKS 256
#define PATHLEN 1024

typedef struct {
	float*   data[2];
	uint32_t frames;
	float    rate;
	float    peaks[NPEAKS];
	char     path[PATHLEN];
} Sample;

enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
	int    note;
	int    stage;
	float  env;
	double pos;
	float  gain;
	uint32_t age;
} Voice;

/* worker messages */
enum { MSG_LOAD = 1, MSG_FREE };
typedef struct {
	int     type;
	Sample* sample;
} Msg;

typedef struct {
	void* p[P_LAST];
	float sr;

	Sample*  sample;
	Voice    v[NV];
	uint32_t clock;
	float    bend; /* semitones */
	uint32_t rnd;

	/* loading */
	char queue_path[PATHLEN];
	bool loading;
	bool queue_load; /* set by restore () */
	bool inform_ui;

	LV2_URID_Map*        map;
	LV2_Worker_Schedule* schedule;
	LV2_Atom_Forge       forge;
	LV2_URID atom_Path, atom_URID, atom_Object, atom_Blank, midi_MidiEvent;
	LV2_URID patch_Get, patch_Set, patch_property, patch_value, smp_sample;

	/* display (copies: the sample itself may be freed by the worker while the GUI draws) */
	bool  v_has;
	float v_peaks[NPEAKS];
	char  v_name[128];
	float v_dur;
	float v_pos[NV];
	int   v_lo, v_hi, v_root;
	int   v_active;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Smp;

#define CTL(x) (*(float*)self->p[x])

static void
free_sample (Sample* s)
{
	if (s) {
		free (s->data[0]);
		free (s->data[1]);
		free (s);
	}
}

static Sample*
load_sample (const char* path)
{
	SF_INFO info;
	memset (&info, 0, sizeof (info));
	SNDFILE* sf = sf_open (path, SFM_READ, &info);
	if (!sf) {
		return NULL;
	}
	/* sane sample rate and length (max 10 min at 768 kHz), checked in 64 bit before narrowing to uint32_t */
	if (info.frames <= 0 || info.channels <= 0 || info.channels > 64
	    || info.samplerate < 1000 || info.samplerate > 768000
	    || info.frames > (sf_count_t)info.samplerate * 600) {
		sf_close (sf);
		return NULL;
	}
	Sample* s = (Sample*)calloc (1, sizeof (Sample));
	float*  buf = (float*)malloc (sizeof (float) * 4096 * info.channels);
	if (!s || !buf) {
		free (s);
		free (buf);
		sf_close (sf);
		return NULL;
	}
	s->frames  = info.frames;
	s->rate    = info.samplerate;
	s->data[0] = (float*)calloc ((size_t)s->frames + 4, sizeof (float));
	s->data[1] = (float*)calloc ((size_t)s->frames + 4, sizeof (float));
	if (!s->data[0] || !s->data[1]) {
		free (buf);
		free_sample (s);
		sf_close (sf);
		return NULL;
	}
	uint32_t at = 0;
	while (at < s->frames) {
		sf_count_t got = sf_readf_float (sf, buf, 4096);
		if (got <= 0) {
			break;
		}
		for (sf_count_t i = 0; i < got && at < s->frames; ++i, ++at) {
			s->data[0][at] = buf[i * info.channels];
			s->data[1][at] = buf[i * info.channels + (info.channels > 1 ? 1 : 0)];
		}
	}
	s->frames = at;
	free (buf);
	sf_close (sf);
	if (s->frames == 0) {
		free_sample (s);
		return NULL;
	}
	for (int k = 0; k < NPEAKS; ++k) {
		const uint32_t a = (uint64_t)s->frames * k / NPEAKS;
		const uint32_t b = (uint64_t)s->frames * (k + 1) / NPEAKS;
		float          pk = 0;
		for (uint32_t i = a; i < b; ++i) {
			pk = fmaxf (pk, fmaxf (fabsf (s->data[0][i]), fabsf (s->data[1][i])));
		}
		s->peaks[k] = pk;
	}
	strncpy (s->path, path, PATHLEN - 1);
	return s;
}

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	LV2_URID_Map*        map      = (LV2_URID_Map*)jx_feature (features, LV2_URID__map);
	LV2_Worker_Schedule* schedule = (LV2_Worker_Schedule*)jx_feature (features, LV2_WORKER__schedule);
	if (!map || !schedule) {
		return NULL;
	}
	Smp* self = (Smp*)calloc (1, sizeof (Smp));
	if (!self) {
		return NULL;
	}
	self->sr       = rate;
	self->map      = map;
	self->schedule = schedule;
	self->rnd      = 0x9E3779B9;
	lv2_atom_forge_init (&self->forge, map);
	self->atom_Path      = map->map (map->handle, LV2_ATOM__Path);
	self->atom_URID      = map->map (map->handle, LV2_ATOM__URID);
	self->atom_Object    = map->map (map->handle, LV2_ATOM__Object);
	self->atom_Blank     = map->map (map->handle, LV2_ATOM__Blank);
	self->midi_MidiEvent = map->map (map->handle, LV2_MIDI__MidiEvent);
	self->patch_Get      = map->map (map->handle, LV2_PATCH__Get);
	self->patch_Set      = map->map (map->handle, LV2_PATCH__Set);
	self->patch_property = map->map (map->handle, LV2_PATCH__property);
	self->patch_value    = map->map (map->handle, LV2_PATCH__value);
	self->smp_sample     = map->map (map->handle, JX_URI "jx-smp#sample");

	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	for (int i = 0; i < NV; ++i) {
		self->v[i].stage = ENV_OFF;
		self->v_pos[i]   = -1;
	}
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Smp* self = (Smp*)h;
	if (port < P_LAST) {
		self->p[port] = data;
	}
}

static void
activate (LV2_Handle h)
{
	Smp* self = (Smp*)h;
	for (int i = 0; i < NV; ++i) {
		self->v[i].stage = ENV_OFF;
	}
	self->bend = 0;
}

static void
schedule_load (Smp* self, const char* path, uint32_t len)
{
	if (self->loading || len == 0 || len >= PATHLEN) {
		return;
	}
	memcpy (self->queue_path, path, len);
	self->queue_path[len] = '\0';
	self->loading         = true;
	Msg m                 = { MSG_LOAD, NULL };
	self->schedule->schedule_work (self->schedule->handle, sizeof (m), &m);
}

static void
note_on (Smp* self, int note, int vel)
{
	if (!self->sample) {
		return;
	}
	const int lo = (int)CTL (P_NOTE_LO), hi = (int)CTL (P_NOTE_HI);
	if (note < lo || note > hi) {
		return;
	}
	const float prob = jx_clamp (CTL (P_PROBABILITY), 0, 100) * 0.01f;
	if (prob < 1.f && 0.5f * (jx_noise (&self->rnd) + 1.f) >= prob) {
		return;
	}
	const int max_voices = (int)jx_clamp (rintf (CTL (P_VOICES)), 1, NV);
	int       active = 0, slot = -1;
	uint32_t  oldest = UINT32_MAX;
	int       old    = -1;
	for (int i = 0; i < NV; ++i) {
		if (self->v[i].stage != ENV_OFF) {
			++active;
			if (self->v[i].age < oldest) {
				oldest = self->v[i].age;
				old    = i;
			}
		} else if (slot < 0) {
			slot = i;
		}
	}
	if (active >= max_voices || slot < 0) {
		slot = old; /* steal the oldest */
	}
	if (slot < 0) {
		return;
	}
	const float vmax = CTL (P_VOLUME), vmin = CTL (P_MIN_VOLUME);
	Voice*      v    = &self->v[slot];
	v->note          = note;
	v->stage         = ENV_ATTACK;
	v->env           = 0;
	v->pos           = jx_clamp (CTL (P_START), 0, 99) * 0.01 * self->sample->frames;
	v->gain          = jx_db2g (vmin + (vmax - vmin) * vel / 127.f);
	v->age           = ++self->clock;
}

static void
note_off (Smp* self, int note)
{
	if (CTL (P_OBEY_OFF) <= 0.5f) {
		return;
	}
	for (int i = 0; i < NV; ++i) {
		if (self->v[i].note == note && self->v[i].stage != ENV_OFF && self->v[i].stage != ENV_RELEASE) {
			self->v[i].stage = ENV_RELEASE;
		}
	}
}

static void
render_voices (Smp* self, uint32_t from, uint32_t to)
{
	float* out[2] = { (float*)self->p[P_OUT_L], (float*)self->p[P_OUT_R] };
	for (uint32_t i = from; i < to; ++i) {
		out[0][i] = out[1][i] = 0;
	}
	const Sample* s = self->sample;
	if (!s) {
		return;
	}
	const bool  shifted = CTL (P_MODE) < 0.5f;
	const int   root    = (int)CTL (P_ROOT);
	const float tune    = CTL (P_TUNE);
	const float base    = s->rate / self->sr;
	const float att     = 1.f / fmaxf (1.f, CTL (P_ATTACK) * 0.001f * self->sr);
	const float dec     = jx_tc (CTL (P_DECAY) / 4.6f, self->sr); /* -40 dB in the given time */
	const float rel     = jx_tc (CTL (P_RELEASE) / 4.6f, self->sr);
	const float sus     = jx_db2g (CTL (P_SUSTAIN));
	const bool  loop    = CTL (P_LOOP) > 0.5f;
	const double lstart = jx_clamp (CTL (P_LOOP_START), 0, 99) * 0.01 * s->frames;
	const float pan     = jx_clamp (CTL (P_PAN), -1, 1);
	const float gl      = fminf (1.f, 1.f - pan), gr = fminf (1.f, 1.f + pan);
	const double end    = s->frames - 1;

	for (int k = 0; k < NV; ++k) {
		Voice* v = &self->v[k];
		if (v->stage == ENV_OFF) {
			continue;
		}
		const double speed = base * exp2 (((shifted ? v->note - root : 0) + tune + self->bend) / 12.0);
		for (uint32_t i = from; i < to; ++i) {
			switch (v->stage) {
				case ENV_ATTACK:
					v->env += att;
					if (v->env >= 1.f) {
						v->env   = 1.f;
						v->stage = ENV_DECAY;
					}
					break;
				case ENV_DECAY:
					v->env += dec * (sus - v->env);
					if (fabsf (v->env - sus) < 1e-4f) {
						v->stage = ENV_SUSTAIN;
					}
					break;
				case ENV_RELEASE:
					v->env -= rel * v->env;
					if (v->env < 1e-4f) {
						v->stage = ENV_OFF;
					}
					break;
				default:
					break;
			}
			if (v->stage == ENV_OFF) {
				break;
			}
			if (v->pos >= end) {
				if (loop && end - lstart > 4) {
					v->pos = lstart + fmod (v->pos - lstart, end - lstart);
				} else {
					v->stage = ENV_OFF;
					break;
				}
			}
			const uint32_t ip = (uint32_t)v->pos;
			const float    fr = v->pos - ip;
			const float    g  = v->gain * v->env;
			out[0][i] += g * gl * (s->data[0][ip] + fr * (s->data[0][ip + 1] - s->data[0][ip]));
			out[1][i] += g * gr * (s->data[1][ip] + fr * (s->data[1][ip + 1] - s->data[1][ip]));
			v->pos += speed;
		}
	}
}

static void
inform_ui (Smp* self)
{
	if (!self->sample) {
		return;
	}
	const char*          path = self->sample->path;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_frame_time (&self->forge, 0);
	lv2_atom_forge_object (&self->forge, &frame, 1, self->patch_Set);
	lv2_atom_forge_key (&self->forge, self->patch_property);
	lv2_atom_forge_urid (&self->forge, self->smp_sample);
	lv2_atom_forge_key (&self->forge, self->patch_value);
	lv2_atom_forge_path (&self->forge, path, strlen (path));
	lv2_atom_forge_pop (&self->forge, &frame);
}

static void
run (LV2_Handle h, uint32_t n)
{
	Smp* self = (Smp*)h;

	/* notify port: prepare the forge */
	const uint32_t capacity = ((LV2_Atom_Sequence*)self->p[P_NOTIFY])->atom.size;
	lv2_atom_forge_set_buffer (&self->forge, (uint8_t*)self->p[P_NOTIFY], capacity);
	LV2_Atom_Forge_Frame seq_frame;
	lv2_atom_forge_sequence_head (&self->forge, &seq_frame, 0);

	if (self->queue_load && !self->loading) {
		self->queue_load = false;
		schedule_load (self, self->queue_path, strlen (self->queue_path));
	}

	const int chn = (int)jx_clamp (rintf (CTL (P_CHANNEL)), 0, 16);
	const float bend_range = jx_clamp (CTL (P_BEND_RANGE), 0, 24);
	uint32_t  pos = 0;

	const LV2_Atom_Sequence* seq = (const LV2_Atom_Sequence*)self->p[P_CONTROL];
	LV2_ATOM_SEQUENCE_FOREACH (seq, ev)
	{
		if (ev->body.type == self->atom_Object || ev->body.type == self->atom_Blank) {
			const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == self->patch_Get) {
				inform_ui (self);
			} else if (obj->body.otype == self->patch_Set) {
				const LV2_Atom* property = NULL;
				const LV2_Atom* value    = NULL;
				lv2_atom_object_get (obj, self->patch_property, &property, self->patch_value, &value, 0);
				if (property && property->type == self->atom_URID && ((const LV2_Atom_URID*)property)->body == self->smp_sample && value && value->type == self->atom_Path) {
					schedule_load (self, (const char*)LV2_ATOM_BODY_CONST (value), strnlen ((const char*)LV2_ATOM_BODY_CONST (value), value->size));
				}
			}
			continue;
		}
		if (ev->body.type != self->midi_MidiEvent || ev->body.size < 2) {
			continue;
		}
		const uint32_t t = ev->time.frames < n ? (uint32_t)ev->time.frames : n;
		if (t > pos) {
			render_voices (self, pos, t);
			/* render_voices writes from `pos', keep what was rendered */
			pos = t;
		}
		const uint8_t* m = (const uint8_t*)(ev + 1);
		if (chn > 0 && (m[0] & 0x0f) != chn - 1) {
			continue;
		}
		switch (m[0] & 0xf0) {
			case 0x90:
				if (ev->body.size > 2 && m[2] > 0) {
					note_on (self, m[1], m[2]);
				} else {
					note_off (self, m[1]);
				}
				break;
			case 0x80:
				note_off (self, m[1]);
				break;
			case 0xe0:
				if (ev->body.size > 2) {
					self->bend = bend_range * (((m[2] << 7) | m[1]) - 8192) / 8192.f;
				}
				break;
			case 0xb0:
				if (ev->body.size > 2 && (m[1] == 120 || m[1] == 123)) {
					for (int i = 0; i < NV; ++i) {
						if (self->v[i].stage != ENV_OFF) {
							self->v[i].stage = m[1] == 120 ? ENV_OFF : ENV_RELEASE;
						}
					}
				}
				break;
			default:
				break;
		}
	}
	if (pos < n) {
		render_voices (self, pos, n);
	}

	if (self->inform_ui) {
		self->inform_ui = false;
		inform_ui (self);
	}
	lv2_atom_forge_pop (&self->forge, &seq_frame);

	int active = 0;
	for (int i = 0; i < NV; ++i) {
		const bool on   = self->v[i].stage != ENV_OFF && self->sample;
		self->v_pos[i]  = on ? self->v[i].pos / self->sample->frames : -1;
		active         += on ? 1 : 0;
	}
	self->v_active = active;
	self->v_lo     = (int)CTL (P_NOTE_LO);
	self->v_hi     = (int)CTL (P_NOTE_HI);
	self->v_root   = (int)CTL (P_ROOT);

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Smp* self = (Smp*)h;
	free_sample (self->sample);
	jx_display_free (&self->disp);
	free (self);
}

/* ---- worker: load and free outside the realtime thread ---- */

static LV2_Worker_Status
work (LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle, uint32_t size, const void* data)
{
	Smp* self = (Smp*)h;
	if (size != sizeof (Msg)) {
		return LV2_WORKER_ERR_UNKNOWN;
	}
	const Msg* m = (const Msg*)data;
	if (m->type == MSG_FREE) {
		free_sample (m->sample);
		return LV2_WORKER_SUCCESS;
	}
	Msg r = { MSG_LOAD, load_sample (self->queue_path) };
	respond (handle, sizeof (r), &r);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status
work_response (LV2_Handle h, uint32_t size, const void* data)
{
	Smp* self = (Smp*)h;
	if (size != sizeof (Msg)) {
		return LV2_WORKER_ERR_UNKNOWN;
	}
	const Msg* m  = (const Msg*)data;
	self->loading = false;
	if (!m->sample) {
		return LV2_WORKER_SUCCESS; /* could not be read: keep the old sample */
	}
	Sample* old  = self->sample;
	self->sample = m->sample;
	for (int i = 0; i < NV; ++i) {
		self->v[i].stage = ENV_OFF;
	}
	if (old) {
		Msg f = { MSG_FREE, old };
		self->schedule->schedule_work (self->schedule->handle, sizeof (f), &f);
	}
	self->inform_ui = true;

	const char* base = strrchr (self->sample->path, '/');
	base             = base ? base + 1 : self->sample->path;
	memcpy (self->v_peaks, self->sample->peaks, sizeof (self->v_peaks));
	snprintf (self->v_name, sizeof (self->v_name), "%.127s", base);
	self->v_dur = self->sample->frames / self->sample->rate;
	self->v_has = true;
	return LV2_WORKER_SUCCESS;
}

/* ---- state: the sample path ---- */

static LV2_State_Status
save (LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t flags, const LV2_Feature* const* features)
{
	Smp* self = (Smp*)h;
	if (!self->sample) {
		return LV2_STATE_SUCCESS;
	}
	LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)jx_feature (features, LV2_STATE__mapPath);
	if (!map_path) {
		return LV2_STATE_ERR_NO_FEATURE;
	}
	LV2_State_Free_Path* free_path = (LV2_State_Free_Path*)jx_feature (features, LV2_STATE__freePath);
	char* apath = map_path->abstract_path (map_path->handle, self->sample->path);
	store (handle, self->smp_sample, apath, strlen (apath) + 1, self->atom_Path, LV2_STATE_IS_POD);
	if (free_path) {
		free_path->free_path (free_path->handle, apath);
	} else {
#ifndef _WIN32
		free (apath);
#endif
	}
	return LV2_STATE_SUCCESS;
}

static LV2_State_Status
restore (LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, uint32_t flags, const LV2_Feature* const* features)
{
	Smp*        self = (Smp*)h;
	size_t      size;
	uint32_t    type, vflags;
	const void* value = retrieve (handle, self->smp_sample, &size, &type, &vflags);
	if (!value || type != self->atom_Path) {
		return LV2_STATE_SUCCESS;
	}
	LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)jx_feature (features, LV2_STATE__mapPath);
	if (!map_path) {
		return LV2_STATE_ERR_NO_FEATURE;
	}
	if (self->loading) {
		return LV2_STATE_ERR_UNKNOWN;
	}
	LV2_State_Free_Path* free_path = (LV2_State_Free_Path*)jx_feature (features, LV2_STATE__freePath);
	char* apath = map_path->absolute_path (map_path->handle, (const char*)value);
	strncpy (self->queue_path, apath, PATHLEN - 1);
	self->queue_path[PATHLEN - 1] = '\0';
	self->queue_load              = true;
	if (free_path) {
		free_path->free_path (free_path->handle, apath);
	} else {
#ifndef _WIN32
		free (apath);
#endif
	}
	return LV2_STATE_SUCCESS;
}

/* ---- display ---- */

#define PRODUCT 0xE8B23A

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Smp*           self = (Smp*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 44, 0.4);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const JxRGB    lit  = jx_vfd_tint (PRODUCT);
	char           t[160];

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-SMP", "SAMPLER");
	double vx, vy, vw, vh;
	if (full) {
		vx = 10, vy = y0 + 4, vw = w - 20, vh = hh - y0 - 30;
	} else {
		vx = 3, vy = y0, vw = w - 6, vh = hh - y0 - 3;
	}
	jx_vfd_window (cr, vx, vy, vw, vh, lit);
	const double wx = vx + 6, ww = vw - 12;
	const double top = vy + (full ? 20 : 4), wh = vh - (full ? 26 : 8), mid = top + wh * 0.5;
	if (self->v_has) {
		jx_set_rgb (cr, lit, 0.75);
		for (int k = 0; k < NPEAKS; ++k) {
			const double x0 = wx + ww * k / NPEAKS;
			const double a  = fmin (1.0, self->v_peaks[k]) * wh * 0.5;
			cairo_rectangle (cr, x0, mid - a, fmax (1, ww / NPEAKS - 0.3), fmax (0.6, 2 * a));
		}
		cairo_fill (cr);
		/* voice playheads */
		jx_set_rgb (cr, lit, 1);
		cairo_set_line_width (cr, 1);
		for (int i = 0; i < NV; ++i) {
			if (self->v_pos[i] >= 0 && self->v_pos[i] <= 1) {
				const double px = rint (wx + ww * self->v_pos[i]) + 0.5;
				cairo_move_to (cr, px, top - 2);
				cairo_line_to (cr, px, top + wh + 2);
			}
		}
		cairo_stroke (cr);
		if (full) {
			snprintf (t, sizeof (t), "%.60s", self->v_name);
			jx_vfd_text (cr, wx, vy + 14, -1, 10, lit, true, t);
			snprintf (t, sizeof (t), "%.2f s", self->v_dur);
			jx_vfd_text (cr, wx + ww, vy + 14, 1, 10, lit, true, t);
		}
	} else {
		jx_vfd_text (cr, vx + vw * 0.5, vy + vh * 0.5 + 4, 0, full ? 12 : 9, lit, true, full ? "NO SAMPLE: CHOOSE A FILE BELOW" : "NO SAMPLE");
	}
	jx_vfd_glare (cr, vx, vy, vw, vh);

	if (full) {
		char lo[16], hi[16];
		snprintf (lo, sizeof (lo), "%s%d", jx_note_name (self->v_lo), self->v_lo / 12 - 1);
		snprintf (hi, sizeof (hi), "%s%d", jx_note_name (self->v_hi), self->v_hi / 12 - 1);
		snprintf (t, sizeof (t), "KEYS %s – %s   ROOT %s%d", lo, hi, jx_note_name (self->v_root), self->v_root / 12 - 1);
		jx_font (cr, JX_FONT_LABEL, 8);
		jx_set (cr, JX_LEGEND, 1);
		jx_text (cr, 10, hh - 10, -1, t);
		snprintf (t, sizeof (t), "VOICES %d", self->v_active);
		jx_set (cr, JX_LEGEND_DIM, 1);
		jx_text (cr, w - 10, hh - 10, 1, t);
	}
	return jx_display_end (&self->disp, cr);
}

static const void*
extension_data (const char* uri)
{
	static const LV2_Inline_Display_Interface display = { render };
	static const LV2_Worker_Interface         worker  = { work, work_response, NULL };
	static const LV2_State_Interface          state   = { save, restore };
	if (!strcmp (uri, LV2_INLINEDISPLAY__interface)) {
		return &display;
	}
	if (!strcmp (uri, LV2_WORKER__interface)) {
		return &worker;
	}
	if (!strcmp (uri, LV2_STATE__interface)) {
		return &state;
	}
	return NULL;
}

const LV2_Descriptor jx_smp_descriptor = {
	JX_URI "jx-smp", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
