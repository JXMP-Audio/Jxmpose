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

/* JX-MBC: four band compressor. Linkwitz-Riley (24 dB/oct) crossovers with
 * all-pass phase compensation, so the bands sum flat; a stereo-linked peak
 * compressor with soft knee per band. */

#include "jxmp.h"

#define NB 4

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_X1, P_X2, P_X3,
	P_BAND, /* 7 ports per band: thr, ratio, attack, release, makeup, active, solo */
	P_KNEE = P_BAND + 7 * NB,
	P_OUT_GAIN,
	P_GR,   /* one output per band */
	P_LAST = P_GR + NB
};

enum { B_THR = 0, B_RATIO, B_ATT, B_REL, B_MAKEUP, B_ACTIVE, B_SOLO };

#define BP(b, x) (P_BAND + 7 * (b) + (x))

typedef struct {
	JxBiquad lp1[2], hp1[2], lp2[2], hp2[2], lp3[2], hp3[2];
	JxBiquad ap_low2, ap_low3, ap_mid3;
} Split;

typedef struct {
	float* p[P_LAST];
	float  sr;

	Split ch[2];
	float x[3]; /* crossover frequencies in use */
	float env[NB];
	float gain[NB]; /* smoothed linear band gain */
	float out_gain;

	/* display */
	float v_level[NB], v_gr[NB], v_x[3], v_thr[NB];
	bool  v_active[NB], v_solo[NB];

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Mbc;

static void
set_xovers (Mbc* self)
{
	const float sr = self->sr;
	const double q = M_SQRT1_2;
	for (int c = 0; c < 2; ++c) {
		Split* s = &self->ch[c];
		for (int i = 0; i < 2; ++i) {
			jx_bq_set (&s->lp1[i], JX_BQ_LP, self->x[0], q, 0, sr);
			jx_bq_set (&s->hp1[i], JX_BQ_HP, self->x[0], q, 0, sr);
			jx_bq_set (&s->lp2[i], JX_BQ_LP, self->x[1], q, 0, sr);
			jx_bq_set (&s->hp2[i], JX_BQ_HP, self->x[1], q, 0, sr);
			jx_bq_set (&s->lp3[i], JX_BQ_LP, self->x[2], q, 0, sr);
			jx_bq_set (&s->hp3[i], JX_BQ_HP, self->x[2], q, 0, sr);
		}
		jx_bq_set (&s->ap_low2, JX_BQ_AP, self->x[1], q, 0, sr);
		jx_bq_set (&s->ap_low3, JX_BQ_AP, self->x[2], q, 0, sr);
		jx_bq_set (&s->ap_mid3, JX_BQ_AP, self->x[2], q, 0, sr);
	}
}

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Mbc* self = (Mbc*)calloc (1, sizeof (Mbc));
	if (!self) {
		return NULL;
	}
	self->sr              = rate;
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	for (int b = 0; b < NB; ++b) {
		self->gain[b]    = 1.f;
		self->v_level[b] = -120.f;
	}
	self->out_gain = 1.f;
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Mbc* self = (Mbc*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Mbc* self = (Mbc*)h;
	memset (self->ch, 0, sizeof (self->ch));
	self->x[0] = self->x[1] = self->x[2] = 0;
	for (int b = 0; b < NB; ++b) {
		self->env[b]  = 0;
		self->gain[b] = 1.f;
	}
}

/* gain change in dB for a level in dB (soft knee) */
static inline float
gain_computer (float lvl, float thr, float ratio, float knee)
{
	const float over = lvl - thr;
	const float s    = 1.f / ratio - 1.f;
	if (2.f * over < -knee) {
		return 0.f;
	}
	if (knee > 0.f && 2.f * fabsf (over) <= knee) {
		const float t = over + knee * 0.5f;
		return s * t * t / (2.f * knee);
	}
	return s * over;
}

static void
run (LV2_Handle h, uint32_t n)
{
	Mbc* self = (Mbc*)h;
	float** p = self->p;

	/* crossovers: keep them ordered and apart */
	float x1 = jx_clamp (*p[P_X1], 20, 1000);
	float x2 = jx_clamp (*p[P_X2], x1 * 1.25f, 8000);
	float x3 = jx_clamp (*p[P_X3], x2 * 1.25f, 18000);
	if (x1 != self->x[0] || x2 != self->x[1] || x3 != self->x[2]) {
		self->x[0] = x1;
		self->x[1] = x2;
		self->x[2] = x3;
		set_xovers (self);
	}

	float thr[NB], ratio[NB], att[NB], rel[NB], makeup[NB];
	bool  active[NB], audible[NB];
	bool  any_solo = false;
	for (int b = 0; b < NB; ++b) {
		any_solo |= *p[BP (b, B_SOLO)] > 0.5f;
	}
	for (int b = 0; b < NB; ++b) {
		thr[b]     = *p[BP (b, B_THR)];
		ratio[b]   = jx_clamp (*p[BP (b, B_RATIO)], 1.f, 20.f);
		att[b]     = jx_tc (*p[BP (b, B_ATT)], self->sr);
		rel[b]     = jx_tc (*p[BP (b, B_REL)], self->sr);
		makeup[b]  = *p[BP (b, B_MAKEUP)];
		active[b]  = *p[BP (b, B_ACTIVE)] > 0.5f;
		audible[b] = !any_solo || *p[BP (b, B_SOLO)] > 0.5f;
	}
	const float knee     = jx_clamp (*p[P_KNEE], 0, 18);
	const float out_tgt  = jx_db2g (*p[P_OUT_GAIN]);
	const float gain_tc  = jx_tc (2.f, self->sr);
	float       peak[NB] = { 0 }, grmax[NB] = { 0 };

	const float* in[2]  = { p[P_IN_L], p[P_IN_R] };
	float*       out[2] = { p[P_OUT_L], p[P_OUT_R] };

	for (uint32_t i = 0; i < n; ++i) {
		float band[2][NB];
		for (int c = 0; c < 2; ++c) {
			Split*      s  = &self->ch[c];
			const float x  = in[c][i];
			float       lo = jx_bq_run (&s->lp1[1], jx_bq_run (&s->lp1[0], x));
			float       hi = jx_bq_run (&s->hp1[1], jx_bq_run (&s->hp1[0], x));
			float       md = jx_bq_run (&s->lp2[1], jx_bq_run (&s->lp2[0], hi));
			float       hr = jx_bq_run (&s->hp2[1], jx_bq_run (&s->hp2[0], hi));
			band[c][0]     = jx_bq_run (&s->ap_low3, jx_bq_run (&s->ap_low2, lo));
			band[c][1]     = jx_bq_run (&s->ap_mid3, md);
			band[c][2]     = jx_bq_run (&s->lp3[1], jx_bq_run (&s->lp3[0], hr));
			band[c][3]     = jx_bq_run (&s->hp3[1], jx_bq_run (&s->hp3[0], hr));
		}

		float y[2] = { 0, 0 };
		for (int b = 0; b < NB; ++b) {
			const float lvl = fmaxf (fabsf (band[0][b]), fabsf (band[1][b]));
			float*      e   = &self->env[b];
			*e += (lvl > *e ? att[b] : rel[b]) * (lvl - *e);
			if (*e > peak[b]) {
				peak[b] = *e;
			}
			float g_db = 0;
			if (active[b]) {
				const float gr = gain_computer (jx_g2db (*e), thr[b], ratio[b], knee);
				if (-gr > grmax[b]) {
					grmax[b] = -gr;
				}
				g_db = gr + makeup[b];
			}
			const float tgt = audible[b] ? jx_db2g (g_db) : 0.f;
			self->gain[b] += gain_tc * (tgt - self->gain[b]);
			y[0] += band[0][b] * self->gain[b];
			y[1] += band[1][b] * self->gain[b];
		}
		self->out_gain += gain_tc * (out_tgt - self->out_gain);
		out[0][i] = y[0] * self->out_gain;
		out[1][i] = y[1] * self->out_gain;
	}

	for (int b = 0; b < NB; ++b) {
		*p[P_GR + b]       = grmax[b];
		self->v_gr[b]      = grmax[b];
		self->v_level[b]   = jx_g2db (peak[b]);
		self->v_thr[b]     = thr[b];
		self->v_active[b]  = active[b];
		self->v_solo[b]    = any_solo && audible[b];
	}
	self->v_x[0] = x1;
	self->v_x[1] = x2;
	self->v_x[2] = x3;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Mbc* self = (Mbc*)h;
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0xE8743B

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Mbc*           self = (Mbc*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 44, 0.45);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const JxRGB    lit  = jx_vfd_tint (PRODUCT);
	static const char* const names[NB] = { "LOW", "LO MID", "HI MID", "HIGH" };

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-MBC", "MULTIBAND COMPRESSOR");

	if (!full) {
		/* four gain reduction bars hanging from the top */
		double bw = (w - 10) / (double)NB;
		jx_vfd_window (cr, 3, y0, w - 6, hh - y0 - 3, lit);
		for (int b = 0; b < NB; ++b) {
			double bx = 5 + b * bw;
			double gh = (hh - y0 - 16) * fmin (1.0, self->v_gr[b] / 24.0);
			jx_set_rgb (cr, lit, 0.12);
			cairo_rectangle (cr, bx + 2, y0 + 3, bw - 4, hh - y0 - 16);
			cairo_fill (cr);
			jx_set_rgb (cr, lit, self->v_active[b] ? 0.95 : 0.3);
			cairo_rectangle (cr, bx + 2, y0 + 3, bw - 4, gh);
			cairo_fill (cr);
			char t[8];
			snprintf (t, sizeof (t), "%d", b + 1);
			jx_vfd_text (cr, bx + bw * 0.5, hh - 6, 0, 8, lit, self->v_active[b], t);
		}
		jx_vfd_glare (cr, 3, y0, w - 6, hh - y0 - 3);
		return jx_display_end (&self->disp, cr);
	}

	jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, "BANDS · GAIN REDUCTION");
	double gx = 10, gy = y0 + 14, gw = w - 20, gh = hh - y0 - 52;
	jx_vfd_window (cr, gx - 2, gy - 2, gw + 4, gh + 4, lit);

	double edge[NB + 1] = { gx, jx_freq_x (self->v_x[0], gx, gw), jx_freq_x (self->v_x[1], gx, gw), jx_freq_x (self->v_x[2], gx, gw), gx + gw };
	/* dB grid: 0 at the top, -60 at the bottom */
#define DB_Y(db) (gy + gh * fmin (1.0, fmax (0.0, -(db) / 60.0)))
	jx_set_rgb (cr, lit, 0.08);
	cairo_set_line_width (cr, 0.5);
	for (int db = -12; db > -60; db -= 12) {
		cairo_move_to (cr, gx, rint (DB_Y (db)) + 0.5);
		cairo_line_to (cr, gx + gw, rint (DB_Y (db)) + 0.5);
	}
	cairo_stroke (cr);

	for (int b = 0; b < NB; ++b) {
		double bx = edge[b] + 2, bw = edge[b + 1] - edge[b] - 4;
		if (bw < 2) {
			continue;
		}
		bool on = self->v_active[b];
		/* input level: segmented bar from the bottom */
		double ly = DB_Y (self->v_level[b]);
		for (double sy = gy + gh - 3; sy > ly; sy -= 3) {
			jx_set_rgb (cr, lit, 0.22);
			cairo_rectangle (cr, bx, sy - 2, bw, 2);
			cairo_fill (cr);
		}
		/* threshold */
		jx_set_rgb (cr, lit, on ? 0.8 : 0.25);
		cairo_set_line_width (cr, 1);
		double ty = rint (DB_Y (self->v_thr[b])) + 0.5;
		cairo_move_to (cr, bx, ty);
		cairo_line_to (cr, bx + bw, ty);
		cairo_stroke (cr);
		/* gain reduction hanging from the top, 0..24 dB over the full height */
		double grh = gh * fmin (1.0, self->v_gr[b] / 24.0);
		jx_set_rgb (cr, lit, on ? 0.95 : 0.3);
		cairo_rectangle (cr, bx + bw * 0.2, gy, bw * 0.6, grh);
		cairo_fill (cr);
		/* legend */
		char t[16];
		snprintf (t, sizeof (t), "%s", names[b]);
		jx_vfd_text (cr, bx + bw * 0.5, gy + gh - 6, 0, bw > 50 ? 9 : 7, lit, on, t);
		if (self->v_solo[b]) {
			jx_vfd_text (cr, bx + bw * 0.5, gy + gh - 17, 0, 7, lit, true, "SOLO");
		}
		/* gain reduction value under the window */
		snprintf (t, sizeof (t), "%.1f", self->v_gr[b] >= 0.05f ? -self->v_gr[b] : 0.f);
		jx_font (cr, JX_FONT_NUMBER, 8);
		jx_set (cr, JX_LEGEND, 1);
		jx_text (cr, bx + bw * 0.5, gy + gh + 14, 0, t);
	}
	/* crossover lines */
	jx_set_rgb (cr, lit, 0.6);
	cairo_set_line_width (cr, 1);
	for (int i = 1; i < NB; ++i) {
		cairo_move_to (cr, rint (edge[i]) + 0.5, gy);
		cairo_line_to (cr, rint (edge[i]) + 0.5, gy + gh);
	}
	cairo_stroke (cr);
	jx_vfd_glare (cr, gx - 2, gy - 2, gw + 4, gh + 4);
#undef DB_Y

	jx_freq_scale (cr, gx, hh - 8, gw);
	jx_font (cr, JX_FONT_LABEL, 7);
	jx_set (cr, JX_LEGEND_DIM, 1);
	jx_text (cr, gx, gy + gh + 14, -1, "GR dB");
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

const LV2_Descriptor jx_mbc_descriptor = {
	JX_URI "jx-mbc", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
