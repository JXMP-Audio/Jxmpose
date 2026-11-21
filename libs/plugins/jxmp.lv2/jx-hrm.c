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

/* JX-HRM: MIDI played harmonizer. Every held MIDI note adds a copy of the
 * audio input shifted by (note - Center Note) semitones, with its own
 * attack / release, up to eight voices. Higher voices are panned right,
 * lower ones left (Spread). */

#include "jxmp.h"
#include "jxmp_psola.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

enum {
	P_MIDI = 0, P_IN_L, P_IN_R, P_OUT_L, P_OUT_R,
	P_WET, P_DRY, P_GAIN, P_VOICES, P_ATTACK, P_RELEASE, P_CHANNEL, P_CENTER, P_WINDOW, P_SPREAD, P_GLIDE,
	P_FORMANT, P_LATENCY, /* appended later: older sessions keep their port indices */
	P_LAST
};

#define NV 8

typedef struct {
	int       note;   /* -1: unused */
	bool      gate;
	float     env;
	float     st;     /* current shift in semitones (glides to the target) */
	float     vel;
	uint32_t  age;
	JxShifter sh;
	JxPsola   ps;
} Voice;

typedef struct {
	void*  p[P_LAST];
	float  sr;

	JxDelay  dl[2];
	JxYin    yin;
	Voice    v[NV];
	uint32_t clock;
	float    g_wet, g_dry, g_in;

	LV2_URID midi_MidiEvent;

	int   v_note[NV];
	float v_env[NV];
	int   v_center;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Hrm;

#define CTL(x) (*(float*)self->p[x])

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	LV2_URID_Map* map = (LV2_URID_Map*)jx_feature (features, LV2_URID__map);
	if (!map) {
		return NULL;
	}
	Hrm* self = (Hrm*)calloc (1, sizeof (Hrm));
	if (!self) {
		return NULL;
	}
	self->sr = rate;
	if (!jx_delay_init (&self->dl[0], rate * 0.25) || !jx_delay_init (&self->dl[1], rate * 0.25)) {
		jx_delay_free (&self->dl[0]);
		free (self);
		return NULL;
	}
	jx_yin_init (&self->yin, rate);
	self->midi_MidiEvent  = map->map (map->handle, LV2_MIDI__MidiEvent);
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	for (int i = 0; i < NV; ++i) {
		self->v[i].note = -1;
		self->v_note[i] = -1;
	}
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Hrm* self = (Hrm*)h;
	if (port < P_LAST) {
		self->p[port] = data;
	}
}

static void
activate (LV2_Handle h)
{
	Hrm* self = (Hrm*)h;
	for (int c = 0; c < 2; ++c) {
		memset (self->dl[c].buf, 0, sizeof (float) * (self->dl[c].mask + 1));
	}
	for (int i = 0; i < NV; ++i) {
		self->v[i].note = -1;
		self->v[i].gate = false;
		self->v[i].env  = 0;
		jx_psola_reset (&self->v[i].ps);
	}
	jx_yin_reset (&self->yin);
}

static void
note_on (Hrm* self, int note, int vel, int max_voices, int center)
{
	/* retrigger the same note, else a free voice, else steal the oldest */
	int slot = -1, gated = 0;
	for (int i = 0; i < NV; ++i) {
		if (self->v[i].note == note) {
			slot = i;
		}
		gated += self->v[i].gate ? 1 : 0;
	}
	if (slot < 0 && gated < max_voices) {
		for (int i = 0; i < NV && slot < 0; ++i) {
			if (!self->v[i].gate && self->v[i].env < 1e-4f) {
				slot = i;
			}
		}
		for (int i = 0; i < NV && slot < 0; ++i) {
			if (!self->v[i].gate) {
				slot = i;
			}
		}
	}
	if (slot < 0) {
		uint32_t oldest = UINT32_MAX;
		for (int i = 0; i < NV; ++i) {
			if (self->v[i].gate && self->v[i].age < oldest) {
				oldest = self->v[i].age;
				slot   = i;
			}
		}
	}
	if (slot < 0) {
		return;
	}
	Voice* v = &self->v[slot];
	if (v->env < 1e-4f) {
		v->st       = note - center; /* no glide from silence */
		v->sh.phase = 0;
		jx_psola_reset (&v->ps);
	}
	v->note = note;
	v->gate = true;
	v->vel  = vel / 127.f;
	v->age  = ++self->clock;
}

static void
note_off (Hrm* self, int note)
{
	for (int i = 0; i < NV; ++i) {
		if (self->v[i].note == note && self->v[i].gate) {
			self->v[i].gate = false;
		}
	}
}

static void
process (Hrm* self, uint32_t from, uint32_t to)
{
	const float* in[2]  = { (const float*)self->p[P_IN_L], (const float*)self->p[P_IN_R] };
	float*       out[2] = { (float*)self->p[P_OUT_L], (float*)self->p[P_OUT_R] };

	const float att    = jx_tc (CTL (P_ATTACK), self->sr);
	const float rel    = jx_tc (CTL (P_RELEASE), self->sr);
	const float glide  = jx_tc (CTL (P_GLIDE), self->sr);
	const float win    = jx_clamp (CTL (P_WINDOW), 20, 100) * 0.001f * self->sr;
	const int   center = (int)rintf (CTL (P_CENTER));
	const float spread = jx_clamp (CTL (P_SPREAD), 0, 100) * 0.01f;
	const float tc     = jx_tc (10.f, self->sr);
	const float wet    = jx_db2g (CTL (P_WET));
	const float dry    = jx_db2g (CTL (P_DRY));
	const float gin    = jx_db2g (CTL (P_GAIN));
	const bool  formant = self->p[P_FORMANT] && CTL (P_FORMANT) > 0.5f;
	const float lat     = formant ? (float)jx_psola_latency (self->sr) : 0.f;

	float pan_l[NV], pan_r[NV], tgt[NV];
	for (int i = 0; i < NV; ++i) {
		const Voice* v = &self->v[i];
		if (v->note < 0) {
			pan_l[i] = pan_r[i] = 0;
			tgt[i]              = 0;
			continue;
		}
		tgt[i]        = v->note - center;
		const float p = spread * jx_clamp (tgt[i] / 12.f, -1, 1);
		pan_l[i]      = fminf (1.f, 1.f - p) * v->vel;
		pan_r[i]      = fminf (1.f, 1.f + p) * v->vel;
	}

	for (uint32_t s = from; s < to; ++s) {
		self->g_in += tc * (gin - self->g_in);
		self->g_wet += tc * (wet - self->g_wet);
		self->g_dry += tc * (dry - self->g_dry);
		const float x[2] = { in[0][s] * self->g_in, in[1][s] * self->g_in };
		jx_delay_write (&self->dl[0], x[0]);
		jx_delay_write (&self->dl[1], x[1]);
		if (formant) {
			jx_yin_feed (&self->yin, 0.5f * (x[0] + x[1]));
		}
		float y[2] = { 0, 0 };
		for (int i = 0; i < NV; ++i) {
			Voice* v = &self->v[i];
			if (v->note < 0) {
				continue;
			}
			v->env += (v->gate ? att : rel) * ((v->gate ? 1.f : 0.f) - v->env);
			if (!v->gate && v->env < 1e-5f) {
				v->env  = 0;
				v->note = -1;
				continue;
			}
			v->st += glide * (tgt[i] - v->st);
			const float ratio = exp2f (v->st / 12.f);
			if (formant) {
				float l, r;
				jx_psola (&v->ps, self->dl, self->sr, ratio, self->yin.period, &l, &r);
				y[0] += v->env * pan_l[i] * l;
				y[1] += v->env * pan_r[i] * r;
				continue;
			}
			JxShifter   r     = v->sh;
			y[0] += v->env * pan_l[i] * jx_shift (&r, &self->dl[0], ratio, win);
			y[1] += v->env * pan_r[i] * jx_shift (&v->sh, &self->dl[1], ratio, win);
		}
		/* with formants kept the voices are late by the latency: the dry signal waits for them */
		const float dl = formant ? jx_delay_read (&self->dl[0], lat) : x[0];
		const float dr = formant ? jx_delay_read (&self->dl[1], lat) : x[1];
		out[0][s] = self->g_wet * y[0] + self->g_dry * dl;
		out[1][s] = self->g_wet * y[1] + self->g_dry * dr;
	}
}

static void
run (LV2_Handle h, uint32_t n)
{
	Hrm* self = (Hrm*)h;

	const int max_voices = (int)jx_clamp (rintf (CTL (P_VOICES)), 1, NV);
	const int chn        = (int)jx_clamp (rintf (CTL (P_CHANNEL)), 0, 16);
	const int center     = (int)rintf (CTL (P_CENTER));
	uint32_t  pos        = 0;
	if (self->p[P_LATENCY]) {
		*(float*)self->p[P_LATENCY] = (self->p[P_FORMANT] && CTL (P_FORMANT) > 0.5f) ? (float)jx_psola_latency (self->sr) : 0.f;
	}

	const LV2_Atom_Sequence* seq = (const LV2_Atom_Sequence*)self->p[P_MIDI];
	LV2_ATOM_SEQUENCE_FOREACH (seq, ev)
	{
		if (ev->body.type != self->midi_MidiEvent || ev->body.size < 3) {
			continue;
		}
		uint32_t t = ev->time.frames < n ? (uint32_t)ev->time.frames : n;
		if (t > pos) {
			process (self, pos, t);
			pos = t;
		}
		const uint8_t* m = (const uint8_t*)(ev + 1);
		if (chn > 0 && (m[0] & 0x0f) != chn - 1) {
			continue;
		}
		switch (m[0] & 0xf0) {
			case 0x90:
				if (m[2] > 0) {
					note_on (self, m[1], m[2], max_voices, center);
				} else {
					note_off (self, m[1]);
				}
				break;
			case 0x80:
				note_off (self, m[1]);
				break;
			case 0xb0:
				if (m[1] == 123 || m[1] == 120) { /* all notes / sound off */
					for (int i = 0; i < NV; ++i) {
						self->v[i].gate = false;
					}
				}
				break;
			default:
				break;
		}
	}
	if (pos < n) {
		process (self, pos, n);
	}

	for (int i = 0; i < NV; ++i) {
		self->v_note[i] = self->v[i].note;
		self->v_env[i]  = self->v[i].env;
	}
	self->v_center = center;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Hrm* self = (Hrm*)h;
	jx_delay_free (&self->dl[0]);
	jx_delay_free (&self->dl[1]);
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0xD65BB5

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Hrm*           self = (Hrm*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 40, 0.36);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const JxRGB    lit  = jx_vfd_tint (PRODUCT);
	char           t[16];

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-HRM", "HARMONIZER");
	double vx, vy, vw, vh;
	if (full) {
		jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, "VOICES");
		vx = 10, vy = y0 + 12, vw = w - 20, vh = hh - y0 - 40;
	} else {
		vx = 3, vy = y0, vw = w - 6, vh = hh - y0 - 3;
	}
	jx_vfd_window (cr, vx, vy, vw, vh, lit);
	const int    cols = full ? NV : NV / 2;
	const int    rows = full ? 1 : 2;
	const double cw   = (vw - 8) / cols;
	const double ch   = (vh - 6) / rows;
	for (int i = 0; i < NV; ++i) {
		const int    col = i % cols, row = i / cols;
		const double cx  = vx + 4 + col * cw + cw * 0.5;
		const double cy  = vy + 3 + row * ch;
		const bool   on  = self->v_note[i] >= 0;
		if (on) {
			snprintf (t, sizeof (t), "%s%d", jx_note_name (self->v_note[i]), self->v_note[i] / 12 - 1);
		} else {
			snprintf (t, sizeof (t), "%d", i + 1);
		}
		JxRGB l = lit;
		if (on) {
			const float e = 0.3f + 0.7f * self->v_env[i];
			l.r *= e, l.g *= e, l.b *= e;
		}
		jx_vfd_text (cr, cx, cy + ch * 0.5 + (full ? 2 : 4), 0, full ? fmin (18, cw * 0.32) : 10, l, on, t);
		if (full && on) {
			const int st = self->v_note[i] - self->v_center;
			snprintf (t, sizeof (t), "%+d", st);
			jx_vfd_text (cr, cx, cy + ch * 0.5 + 18, 0, 9, lit, true, t);
		}
	}
	jx_vfd_glare (cr, vx, vy, vw, vh);
	if (full) {
		snprintf (t, sizeof (t), "%s%d", jx_note_name (self->v_center), self->v_center / 12 - 1);
		jx_font (cr, JX_FONT_LABEL, 8);
		jx_set (cr, JX_LEGEND_DIM, 1);
		double tw = jx_text (cr, 10, hh - 10, -1, "CENTER NOTE ");
		jx_set (cr, JX_LEGEND, 1);
		jx_text (cr, 10 + tw, hh - 10, -1, t);
		jx_set (cr, JX_LEGEND_DIM, 1);
		jx_text (cr, w - 10, hh - 10, 1, "MIDI NOTES PLAY THE VOICES");
	}
	return jx_display_end (&self->disp, cr);
}

static const void*
extension_data (const char* uri)
{
	static const LV2_Inline_Display_Interface display = { render };
	if (!strcmp (uri, LV2_INLINEDISPLAY__interface)) {
		return &display;
	}
	return NULL;
}

const LV2_Descriptor jx_hrm_descriptor = {
	JX_URI "jx-hrm", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
