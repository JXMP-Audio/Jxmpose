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

/* JX-PIT: two pitch shifters (semitones + cents, level and pan each) on
 * the same input, mixed with the dry signal. Granular (two crossfading
 * taps) shifting: no latency, suits doubling, harmonies and octaves.
 * Keep Formants switches to PSOLA (jxmp_psola.h): the timbre of a single
 * voice stays, at 15 ms latency (the dry signal is delayed with it). */

#include "jxmp.h"
#include "jxmp_psola.h"

#define NS 2

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_WINDOW, P_WET, P_DRY,
	P_SHIFTER, /* 5 per shifter: enable, semitones, cents, level, pan */
	P_FORMANT = P_SHIFTER + 5 * NS,
	P_LATENCY,
	P_LAST
};

enum { S_ON = 0, S_SEMI, S_CENT, S_LEVEL, S_PAN };
#define SP(s, x) (P_SHIFTER + 5 * (s) + (x))

typedef struct {
	float* p[P_LAST];
	float  sr;

	JxDelay   dl[2];
	JxShifter sh[NS];
	JxPsola   ps[NS];
	JxYin     yin;
	float     g[NS][2]; /* smoothed left/right gain of each shifter */
	float     g_dry;

	float v_shift[NS], v_level[NS], v_pan[NS];
	bool  v_on[NS];
	float v_peak;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Pit;

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Pit* self = (Pit*)calloc (1, sizeof (Pit));
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
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Pit* self = (Pit*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Pit* self = (Pit*)h;
	for (int c = 0; c < 2; ++c) {
		memset (self->dl[c].buf, 0, sizeof (float) * (self->dl[c].mask + 1));
	}
	for (int s = 0; s < NS; ++s) {
		self->sh[s].phase = 0;
		jx_psola_reset (&self->ps[s]);
	}
	jx_yin_reset (&self->yin);
}

static void
run (LV2_Handle h, uint32_t n)
{
	Pit*    self = (Pit*)h;
	float** p    = self->p;

	const float win = jx_clamp (*p[P_WINDOW], 20, 120) * 0.001f * self->sr;
	const float wet = jx_db2g (*p[P_WET]);
	const float tc  = jx_tc (10.f, self->sr);
	float       ratio[NS], tgt[NS][2];
	bool        on[NS];
	for (int s = 0; s < NS; ++s) {
		const float st  = rintf (*p[SP (s, S_SEMI)]) + *p[SP (s, S_CENT)] * 0.01f;
		const float lvl = jx_db2g (*p[SP (s, S_LEVEL)]) * wet;
		const float pan = jx_clamp (*p[SP (s, S_PAN)], -1, 1);
		on[s]           = *p[SP (s, S_ON)] > 0.5f;
		ratio[s]        = powf (2.f, st / 12.f);
		/* balance: centre = both sides at full level */
		tgt[s][0] = on[s] ? lvl * fminf (1.f, 1.f - pan) : 0.f;
		tgt[s][1] = on[s] ? lvl * fminf (1.f, 1.f + pan) : 0.f;

		self->v_shift[s] = st;
		self->v_level[s] = *p[SP (s, S_LEVEL)];
		self->v_pan[s]   = pan;
		self->v_on[s]    = on[s];
	}
	const float dry     = jx_db2g (*p[P_DRY]);
	const bool  formant = p[P_FORMANT] && *p[P_FORMANT] > 0.5f;
	const float lat     = formant ? (float)jx_psola_latency (self->sr) : 0.f;
	float       pk      = 0;
	if (p[P_LATENCY]) {
		*p[P_LATENCY] = lat;
	}

	for (uint32_t i = 0; i < n; ++i) {
		const float x[2] = { p[P_IN_L][i], p[P_IN_R][i] };
		jx_delay_write (&self->dl[0], x[0]);
		jx_delay_write (&self->dl[1], x[1]);
		float y[2] = { 0, 0 };
		if (formant) {
			jx_yin_feed (&self->yin, 0.5f * (x[0] + x[1]));
		}
		for (int s = 0; s < NS; ++s) {
			self->g[s][0] += tc * (tgt[s][0] - self->g[s][0]);
			self->g[s][1] += tc * (tgt[s][1] - self->g[s][1]);
			if (!on[s] && self->g[s][0] < 1e-5f && self->g[s][1] < 1e-5f) {
				continue;
			}
			if (formant) {
				float l, r;
				jx_psola (&self->ps[s], self->dl, self->sr, ratio[s], self->yin.period, &l, &r);
				y[0] += self->g[s][0] * l;
				y[1] += self->g[s][1] * r;
				continue;
			}
			/* both channels read at the same phase */
			JxShifter r = self->sh[s];
			y[0] += self->g[s][0] * jx_shift (&r, &self->dl[0], ratio[s], win);
			y[1] += self->g[s][1] * jx_shift (&self->sh[s], &self->dl[1], ratio[s], win);
		}
		self->g_dry += tc * (dry - self->g_dry);
		/* with formants kept the shifted voices are late by the latency: the dry signal waits for them */
		const float dl = formant ? jx_delay_read (&self->dl[0], lat) : x[0];
		const float dr = formant ? jx_delay_read (&self->dl[1], lat) : x[1];
		p[P_OUT_L][i] = y[0] + self->g_dry * dl;
		p[P_OUT_R][i] = y[1] + self->g_dry * dr;
		pk            = fmaxf (pk, fmaxf (fabsf (y[0]), fabsf (y[1])));
	}
	self->v_peak = pk;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Pit* self = (Pit*)h;
	jx_delay_free (&self->dl[0]);
	jx_delay_free (&self->dl[1]);
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0x3FB6E0

static void
fmt_shift (char* t, size_t len, float st, bool full)
{
	const float a = fabsf (st);
	const char* sg = st > 0.004f ? "+" : (st < -0.004f ? "−" : " ");
	if (full) {
		snprintf (t, len, "%s%d.%02d", sg, (int)a, (int)rintf ((a - (int)a) * 100) % 100);
	} else {
		snprintf (t, len, "%s%.1f", sg, a);
	}
}

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Pit*           self = (Pit*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 40, 0.36);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const JxRGB    lit  = jx_vfd_tint (PRODUCT);
	char           t[32];

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-PIT", "PITCH SHIFTER");

	if (!full) {
		jx_vfd_window (cr, 3, y0, w - 6, hh - y0 - 3, lit);
		for (int s = 0; s < NS; ++s) {
			fmt_shift (t, sizeof (t), self->v_shift[s], false);
			double cx = 3 + (w - 6) * (s + 0.5) / NS;
			jx_vfd_text (cr, cx, y0 + (hh - y0) * 0.5 + 4, 0, 12, lit, self->v_on[s], t);
		}
		jx_vfd_glare (cr, 3, y0, w - 6, hh - y0 - 3);
		return jx_display_end (&self->disp, cr);
	}

	jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, "SHIFTERS");
	const double rh = (hh - y0 - 20) / NS;
	for (int s = 0; s < NS; ++s) {
		double ry = y0 + 12 + s * rh;
		double vx = 40, vw = w - 50, vh = rh - 6;
		jx_annunciator (cr, 12, ry + vh * 0.5 + 4, PRODUCT, self->v_on[s], s ? "B" : "A");
		jx_vfd_window (cr, vx, ry, vw, vh, lit);
		fmt_shift (t, sizeof (t), self->v_shift[s], true);
		double tw = jx_vfd_text (cr, vx + 10, ry + vh * 0.5 + 9, -1, 26, lit, self->v_on[s], t);
		jx_vfd_text (cr, vx + 16 + tw, ry + vh * 0.5 + 9, -1, 10, lit, self->v_on[s], "ST");
		/* level and pan annunciators */
		snprintf (t, sizeof (t), "%+.1f dB", self->v_level[s]);
		jx_vfd_text (cr, vx + vw - 10, ry + vh * 0.5 - 2, 1, 10, lit, self->v_on[s], t);
		double px = vx + vw - 70, py = ry + vh * 0.5 + 8, pw = 60;
		jx_set_rgb (cr, lit, self->v_on[s] ? 0.25 : 0.08);
		cairo_rectangle (cr, px, py, pw, 2);
		cairo_fill (cr);
		jx_set_rgb (cr, lit, self->v_on[s] ? 1 : 0.15);
		cairo_rectangle (cr, px + pw * 0.5 * (1 + self->v_pan[s]) - 2, py - 3, 4, 8);
		cairo_fill (cr);
		jx_vfd_glare (cr, vx, ry, vw, vh);
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

const LV2_Descriptor jx_pit_descriptor = {
	JX_URI "jx-pit", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
