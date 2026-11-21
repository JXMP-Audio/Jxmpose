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

/* JX-TUN: tuner and automatic pitch correction for a single voice or
 * instrument. Pitch is detected with YIN on a decimated copy of the input
 * (~12 kHz, 85 ms window, every ~10 ms); in Correct mode the signal is
 * shifted towards the nearest note of the chosen key and scale. */

#include "jxmp.h"

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_MODE, P_REF, P_KEY, P_SCALE, P_SPEED, P_AMOUNT, P_MUTE,
	P_FREQ, P_NOTE, P_CENTS,
	P_LAST
};

#define YW   1024 /* analysis window (decimated samples) */
#define HOP  128
#define NSCALES 13

static const uint16_t scale_mask[NSCALES] = {
	0xfff,                                                        /* chromatic */
	(1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11), /* major */
	(1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10), /* minor */
	(1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10), /* dorian */
	(1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10), /* phrygian */
	(1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11), /* lydian */
	(1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10), /* mixolydian */
	(1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10), /* locrian */
	(1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11), /* harmonic minor */
	(1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                        /* major pentatonic */
	(1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                       /* minor pentatonic */
	(1 << 0) | (1 << 1) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11), /* arabian (double harmonic) */
	(1 << 0) | (1 << 2) | (1 << 5) | (1 << 7) | (1 << 10),                       /* egyptian */
};

static const char* const scale_name[NSCALES] = {
	"CHROMATIC", "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN", "LOCRIAN",
	"HARM MINOR", "MAJ PENTA", "MIN PENTA", "ARABIAN", "EGYPTIAN"
};

typedef struct {
	float* p[P_LAST];
	float  sr;

	/* detection */
	uint32_t decim, dcount;
	float    dacc;
	JxBiquad aa[2];
	float    ring[YW];
	uint32_t rpos, hop;
	float    diff[YW / 2];
	float    pitch;   /* detected note (fractional MIDI note), < 0: none */
	float    period;  /* detected period in samples */

	/* correction */
	JxDelay   dl[2];
	JxShifter sh;
	float     corr;   /* current correction, semitones */
	float     win;
	float     g_out;

	float v_freq, v_pitch, v_target, v_ref;
	int   v_mode, v_key, v_scale;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Tun;

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Tun* self = (Tun*)calloc (1, sizeof (Tun));
	if (!self) {
		return NULL;
	}
	self->sr    = rate;
	self->decim = rate > 13000 ? (uint32_t)floor (rate / 12000.0) : 1;
	jx_bq_set (&self->aa[0], JX_BQ_LP, 0.4 * rate / self->decim, 0.54, 0, rate);
	jx_bq_set (&self->aa[1], JX_BQ_LP, 0.4 * rate / self->decim, 1.31, 0, rate);
	if (!jx_delay_init (&self->dl[0], rate * 0.1) || !jx_delay_init (&self->dl[1], rate * 0.1)) {
		jx_delay_free (&self->dl[0]);
		free (self);
		return NULL;
	}
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	self->pitch           = -1;
	self->win             = 0.03f * rate;
	self->v_pitch         = -1;
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Tun* self = (Tun*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Tun* self = (Tun*)h;
	memset (self->ring, 0, sizeof (self->ring));
	self->rpos = self->hop = self->dcount = 0;
	self->dacc  = 0;
	self->pitch = -1;
	self->corr  = 0;
	self->g_out = 1;
	jx_bq_reset (&self->aa[0]);
	jx_bq_reset (&self->aa[1]);
}

/* YIN on the last YW decimated samples; sets pitch (MIDI note relative to ref) or -1 */
static void
detect (Tun* self, float ref)
{
	const float    srd    = self->sr / self->decim;
	const uint32_t tmin   = (uint32_t)(srd / 1400.f);
	const uint32_t tmax   = YW / 2 - 2;
	const uint32_t len    = YW - tmax - 2;
	float          x[YW];
	double         energy = 0;
	for (uint32_t i = 0; i < YW; ++i) {
		x[i] = self->ring[(self->rpos + i) & (YW - 1)];
		energy += x[i] * x[i];
	}
	if (energy / YW < 1e-6) { /* below -60 dBFS */
		self->pitch = -1;
		return;
	}
	float* d   = self->diff;
	double sum = 0;
	d[0]       = 1;
	for (uint32_t tau = 1; tau <= tmax; ++tau) {
		double s = 0;
		for (uint32_t i = 0; i < len; ++i) {
			const float df = x[i] - x[i + tau];
			s += df * df;
		}
		sum += s;
		d[tau] = sum > 0 ? s * tau / sum : 1;
	}
	uint32_t best = 0;
	for (uint32_t tau = tmin > 2 ? tmin : 2; tau < tmax; ++tau) {
		if (d[tau] < 0.15f) {
			while (tau + 1 < tmax && d[tau + 1] < d[tau]) {
				++tau;
			}
			best = tau;
			break;
		}
	}
	if (!best) {
		self->pitch = -1;
		return;
	}
	/* parabolic interpolation */
	const float a = d[best - 1], b = d[best], c = d[best + 1];
	float       t = best;
	const float den = a - 2 * b + c;
	if (fabsf (den) > 1e-9f) {
		t += 0.5f * (a - c) / den;
	}
	const float f = srd / t;
	if (f < 40 || f > 1400) {
		self->pitch = -1;
		return;
	}
	self->period = t * self->decim;
	self->pitch  = 69.f + 12.f * log2f (f / ref);
}

/* nearest allowed note to a pitch */
static float
target_note (float pitch, int key, int scale)
{
	const uint16_t mask = scale_mask[scale];
	const int      base = (int)floorf (pitch);
	float          best = pitch, bd = 99;
	for (int nn = base - 6; nn <= base + 6; ++nn) {
		if (!(mask & (1 << (((nn - key) % 12 + 12) % 12)))) {
			continue;
		}
		const float dd = fabsf (pitch - nn);
		if (dd < bd) {
			bd   = dd;
			best = nn;
		}
	}
	return best;
}

static void
run (LV2_Handle h, uint32_t n)
{
	Tun*    self = (Tun*)h;
	float** p    = self->p;

	const int   mode   = *p[P_MODE] > 0.5f ? 1 : 0;
	const float ref    = jx_clamp (*p[P_REF], 400, 480);
	const int   key    = (int)jx_clamp (rintf (*p[P_KEY]), 0, 11);
	const int   scale  = (int)jx_clamp (rintf (*p[P_SCALE]), 0, NSCALES - 1);
	const float amount = jx_clamp (*p[P_AMOUNT], 0, 100) * 0.01f;
	const float speed  = jx_tc (*p[P_SPEED], self->sr / 64.f); /* updated every 64 samples */
	const float mute   = (mode == 0 && *p[P_MUTE] > 0.5f) ? 0.f : 1.f;
	const float gtc    = jx_tc (5.f, self->sr);

	float target = -1;
	for (uint32_t i = 0; i < n; ++i) {
		const float xl = p[P_IN_L][i], xr = p[P_IN_R][i];
		const float m  = jx_bq_run (&self->aa[1], jx_bq_run (&self->aa[0], 0.5f * (xl + xr)));
		self->dacc += m;
		if (++self->dcount >= self->decim) {
			self->ring[self->rpos] = self->dacc / self->decim;
			self->rpos             = (self->rpos + 1) & (YW - 1);
			self->dacc             = 0;
			self->dcount           = 0;
			if (++self->hop >= HOP) {
				self->hop = 0;
				detect (self, ref);
			}
		}

		float y[2] = { xl, xr };
		if (mode == 1) {
			if ((i & 63) == 0) {
				float want = 0;
				if (self->pitch >= 0) {
					target = target_note (self->pitch, key, scale);
					want   = (target - self->pitch) * amount;
					/* window: two periods of the voice, 10..40 ms */
					const float wt = jx_clamp (2.f * self->period, 0.01f * self->sr, 0.04f * self->sr);
					self->win += 0.2f * (wt - self->win);
				}
				self->corr += speed * (want - self->corr);
			}
			jx_delay_write (&self->dl[0], xl);
			jx_delay_write (&self->dl[1], xr);
			const float ratio = powf (2.f, self->corr / 12.f);
			JxShifter   r     = self->sh;
			y[0]              = jx_shift (&r, &self->dl[0], ratio, self->win);
			y[1]              = jx_shift (&self->sh, &self->dl[1], ratio, self->win);
		}
		self->g_out += gtc * (mute - self->g_out);
		p[P_OUT_L][i] = y[0] * self->g_out;
		p[P_OUT_R][i] = y[1] * self->g_out;
	}

	if (self->pitch >= 0) {
		const float note = rintf (self->pitch);
		*p[P_FREQ]       = ref * powf (2.f, (self->pitch - 69.f) / 12.f);
		*p[P_NOTE]       = note;
		*p[P_CENTS]      = 100.f * (self->pitch - note);
		if (target < 0) {
			target = target_note (self->pitch, key, scale);
		}
	} else {
		*p[P_FREQ]  = 0;
		*p[P_CENTS] = 0;
	}
	self->v_freq   = *p[P_FREQ];
	self->v_pitch  = self->pitch;
	self->v_target = target;
	self->v_ref    = ref;
	self->v_mode   = mode;
	self->v_key    = key;
	self->v_scale  = scale;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Tun* self = (Tun*)h;
	jx_delay_free (&self->dl[0]);
	jx_delay_free (&self->dl[1]);
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0x9CD64A

/* cents scale with a needle (-50..+50) and the in-tune LED */
static void
draw_cents (cairo_t* cr, double x, double y, double w, double h, JxRGB lit, bool valid, float cents, bool full)
{
	const double cx = x + w * 0.5;
	jx_set_rgb (cr, lit, 0.35);
	cairo_set_line_width (cr, 1);
	for (int c = -50; c <= 50; c += full ? 5 : 10) {
		const double tx  = rint (cx + w * 0.5 * c / 50.0) + 0.5;
		const double len = (c % 25 == 0) ? h * 0.45 : h * 0.25;
		cairo_move_to (cr, tx, y + h - len);
		cairo_line_to (cr, tx, y + h);
	}
	cairo_stroke (cr);
	if (full) {
		jx_font (cr, JX_FONT_NUMBER, 7);
		jx_set_rgb (cr, lit, 0.5);
		jx_text (cr, x, y + h + 9, -1, "−50");
		jx_text (cr, cx, y + h + 9, 0, "0");
		jx_text (cr, x + w, y + h + 9, 1, "+50");
	}
	if (!valid) {
		return;
	}
	const bool   in_tune = fabsf (cents) < 3;
	const double nx      = cx + w * 0.5 * jx_clamp (cents, -50, 50) / 50.0;
	jx_set_rgb (cr, lit, 1);
	cairo_set_line_width (cr, full ? 3 : 2);
	cairo_move_to (cr, nx, y);
	cairo_line_to (cr, nx, y + h);
	cairo_stroke (cr);
	if (in_tune) {
		jx_set_rgb (cr, lit, 0.3);
		cairo_rectangle (cr, cx - w * 0.03, y, w * 0.06, h);
		cairo_fill (cr);
	}
}

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Tun*           self  = (Tun*)h;
	const uint32_t hh    = jx_display_height (w, max_h, 48, 0.4);
	cairo_t*       cr    = jx_display_begin (&self->disp, w, hh);
	const bool     full  = hh >= JX_FULL_H;
	const JxRGB    lit   = jx_vfd_tint (PRODUCT);
	const bool     valid = self->v_pitch >= 0;
	const float    note  = rintf (self->v_pitch);
	const float    cents = 100.f * (self->v_pitch - note);
	char           t[48];

	if (valid) {
		snprintf (t, sizeof (t), "%s%d", jx_note_name ((int)note), (int)note / 12 - 1);
	} else {
		snprintf (t, sizeof (t), "--");
	}

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-TUN", "TUNER · PITCH CORRECTION");

	if (!full) {
		jx_vfd_window (cr, 3, y0, w - 6, hh - y0 - 3, lit);
		jx_vfd_text (cr, 8, y0 + 17, -1, 15, lit, valid, t);
		if (self->v_mode == 1) {
			jx_vfd_text (cr, w - 8, y0 + 13, 1, 7, lit, true, "CORR");
		}
		draw_cents (cr, 8, hh - 14, w - 16, 8, lit, valid, cents, false);
		jx_vfd_glare (cr, 3, y0, w - 6, hh - y0 - 3);
		return jx_display_end (&self->disp, cr);
	}

	jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, self->v_mode ? "CORRECT" : "TUNER");
	double vx = 10, vy = y0 + 14, vw = w - 20, vh = hh - y0 - 46;
	jx_vfd_window (cr, vx, vy, vw, vh, lit);
	jx_vfd_text (cr, vx + 14, vy + vh * 0.5 + 6, -1, fmin (40, vh * 0.55), lit, valid, t);
	if (valid) {
		snprintf (t, sizeof (t), "%.1f Hz", self->v_freq);
		jx_vfd_text (cr, vx + vw - 10, vy + 16, 1, 11, lit, true, t);
		snprintf (t, sizeof (t), "%+.0f ct", cents);
		jx_vfd_text (cr, vx + vw - 10, vy + 30, 1, 11, lit, true, t);
	}
	if (self->v_mode == 1 && valid && self->v_target >= 0) {
		snprintf (t, sizeof (t), "TO %s%d", jx_note_name ((int)self->v_target), (int)self->v_target / 12 - 1);
		jx_vfd_text (cr, vx + vw * 0.42, vy + vh * 0.5 + 6, -1, fmin (22, vh * 0.3), lit, true, t);
	}
	draw_cents (cr, vx + vw * 0.08, vy + vh - 22, vw * 0.84, 12, lit, valid, cents, false);
	jx_vfd_glare (cr, vx, vy, vw, vh);

	/* key / scale / reference under the window */
	double ty = hh - 14;
	snprintf (t, sizeof (t), "KEY %s", jx_note_name (self->v_key));
	double ax = 10;
	ax += jx_annunciator (cr, ax, ty + 3, PRODUCT, self->v_mode == 1, t);
	jx_annunciator (cr, ax, ty + 3, PRODUCT, self->v_mode == 1, scale_name[self->v_scale]);
	snprintf (t, sizeof (t), "A = %.1f Hz", self->v_ref);
	jx_font (cr, JX_FONT_NUMBER, 8);
	jx_set (cr, JX_LEGEND_DIM, 1);
	jx_text (cr, w - 10, ty + 3, 1, t);
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

const LV2_Descriptor jx_tun_descriptor = {
	JX_URI "jx-tun", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
