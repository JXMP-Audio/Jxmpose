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

/* JX-VOC: channel vocoder. The main input (voice) is the modulator, the
 * sidechain (a synth) or internal noise is the carrier. Both go through a
 * bank of band-pass filters (4th order, log spaced 80 Hz .. 12 kHz); each
 * carrier band is flattened by its own envelope and given the envelope of
 * the matching voice band. */

#include "jxmp.h"

#define MAXB 32

enum {
	P_IN_L = 0, P_IN_R, P_SC_L, P_SC_R, P_OUT_L, P_OUT_R,
	P_BANDS, P_SOURCE, P_WET, P_DRY, P_CAR_DRY, P_INVERT, P_ATTACK, P_RELEASE, P_SHIFT, P_SIBILANCE,
	P_LAST
};

typedef struct {
	float* p[P_LAST];
	float  sr;

	int   nb;      /* bands in use */
	float shift;   /* carrier shift in use (semitones) */
	JxBiquad mod[MAXB][2];
	JxBiquad car[2][MAXB][2];
	JxBiquad sib;  /* sibilance high-pass on the voice */
	float    menv[MAXB];
	float    cenv[2][MAXB];
	float    g_wet, g_dry, g_cdry, g_sib;
	uint32_t rnd;

	float v_env[MAXB];
	int   v_nb;
	int   v_source;
	bool  v_invert;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Voc;

static void
setup_bank (Voc* self, int nb, float shift)
{
	const double lo = 80, hi = 12000;
	const double r  = pow (hi / lo, 1.0 / (nb - 1));
	const double q  = 1.3 * sqrt (r) / (r - 1); /* a little narrower than the spacing, two stages */
	const double fs = pow (2.0, shift / 12.0);
	for (int b = 0; b < nb; ++b) {
		const double f = lo * pow (r, b);
		for (int k = 0; k < 2; ++k) {
			jx_bq_set (&self->mod[b][k], JX_BQ_BP, f, q, 0, self->sr);
			jx_bq_reset (&self->mod[b][k]);
			for (int c = 0; c < 2; ++c) {
				jx_bq_set (&self->car[c][b][k], JX_BQ_BP, f * fs, q, 0, self->sr);
				jx_bq_reset (&self->car[c][b][k]);
			}
		}
		self->menv[b] = self->cenv[0][b] = self->cenv[1][b] = 0;
	}
	self->nb    = nb;
	self->shift = shift;
}

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Voc* self = (Voc*)calloc (1, sizeof (Voc));
	if (!self) {
		return NULL;
	}
	self->sr              = rate;
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	self->rnd             = 0x1234567;
	jx_bq_set (&self->sib, JX_BQ_HP, 5000, M_SQRT1_2, 0, rate);
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Voc* self = (Voc*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Voc* self = (Voc*)h;
	self->nb  = 0;
	jx_bq_reset (&self->sib);
}

static void
run (LV2_Handle h, uint32_t n)
{
	Voc*    self = (Voc*)h;
	float** p    = self->p;

	const int   nb    = (int)jx_clamp (rintf (*p[P_BANDS]), 4, MAXB);
	const float shift = jx_clamp (*p[P_SHIFT], -12, 12);
	if (nb != self->nb || shift != self->shift) {
		setup_bank (self, nb, shift);
	}

	const bool  invert = *p[P_INVERT] > 0.5f;
	const bool  noise  = !invert && *p[P_SOURCE] > 0.5f;
	const float att    = jx_tc (*p[P_ATTACK], self->sr);
	const float rel    = jx_tc (*p[P_RELEASE], self->sr);
	const float ctc    = jx_tc (20.f, self->sr); /* carrier flattening */
	const float gtc    = jx_tc (10.f, self->sr);
	const float wet    = jx_db2g (*p[P_WET]);
	const float dry    = jx_db2g (*p[P_DRY]);
	const float cdry   = jx_db2g (*p[P_CAR_DRY]);
	const float sib    = jx_db2g (*p[P_SIBILANCE]);

	const float* voice[2] = { p[P_IN_L], p[P_IN_R] };
	const float* car[2]   = { p[P_SC_L], p[P_SC_R] };
	if (invert) {
		voice[0] = p[P_SC_L];
		voice[1] = p[P_SC_R];
		car[0]   = p[P_IN_L];
		car[1]   = p[P_IN_R];
	}
	/* a mono carrier (right side silent) feeds both sides */
	if (!noise) {
		float e = 0;
		for (uint32_t i = 0; i < n; i += 4) {
			e += fabsf (car[1][i]);
		}
		if (e < 1e-7f) {
			car[1] = car[0];
		}
	}

	float vmax[MAXB] = { 0 };

	for (uint32_t i = 0; i < n; ++i) {
		const float v = 0.5f * (voice[0][i] + voice[1][i]);
		float       c[2];
		if (noise) {
			c[0] = 0.3f * jx_noise (&self->rnd);
			c[1] = 0.3f * jx_noise (&self->rnd);
		} else {
			c[0] = car[0][i];
			c[1] = car[1][i];
		}
		float y[2] = { 0, 0 };
		for (int b = 0; b < nb; ++b) {
			const float m = fabsf (jx_bq_run (&self->mod[b][1], jx_bq_run (&self->mod[b][0], v)));
			float*      e = &self->menv[b];
			*e += (m > *e ? att : rel) * (m - *e);
			for (int k = 0; k < 2; ++k) {
				const float cb = jx_bq_run (&self->car[k][b][1], jx_bq_run (&self->car[k][b][0], c[k]));
				float*      ce = &self->cenv[k][b];
				*ce += ctc * (fabsf (cb) - *ce);
				y[k] += cb * *e / (*ce + 1e-3f);
			}
			if (*e > vmax[b]) {
				vmax[b] = *e;
			}
		}
		const float s = jx_bq_run (&self->sib, v);
		self->g_wet += gtc * (wet - self->g_wet);
		self->g_dry += gtc * (dry - self->g_dry);
		self->g_cdry += gtc * (cdry - self->g_cdry);
		self->g_sib += gtc * (sib - self->g_sib);
		for (int k = 0; k < 2; ++k) {
			/* the bank sums to about the voice level */
			p[P_OUT_L + k][i] = self->g_wet * y[k] + self->g_dry * voice[k][i] + self->g_cdry * c[k] + self->g_sib * s;
		}
	}

	for (int b = 0; b < MAXB; ++b) {
		self->v_env[b] = b < nb ? vmax[b] : 0;
	}
	self->v_nb     = nb;
	self->v_source = noise ? 1 : 0;
	self->v_invert = invert;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Voc* self = (Voc*)h;
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0xA35BD6

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Voc*           self = (Voc*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 44, 0.42);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const JxRGB    lit  = jx_vfd_tint (PRODUCT);
	const int      nb   = self->v_nb > 0 ? self->v_nb : 16;

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-VOC", "VOCODER");
	double gx, gy, gw, gh;
	if (full) {
		jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, "BANDS");
		gx = 10;
		gy = y0 + 14;
		gw = w - 20;
		gh = hh - y0 - 48;
	} else {
		gx = 5;
		gy = y0 + 2;
		gw = w - 10;
		gh = hh - y0 - 6;
	}
	jx_vfd_window (cr, gx - 2, gy - 2, gw + 4, gh + 4, lit);
	const double bw  = gw / nb;
	const double seg = full ? 3 : 2;
	for (int b = 0; b < nb; ++b) {
		/* envelope in dB, -60..0 */
		double lv = (jx_g2db (self->v_env[b] * 2.f) + 60.0) / 60.0;
		lv        = fmax (0, fmin (1, lv));
		for (double sy = 0; sy < gh - 4; sy += seg) {
			bool on = sy < lv * (gh - 4);
			jx_set_rgb (cr, lit, on ? 0.9 : 0.06);
			cairo_rectangle (cr, gx + b * bw + bw * 0.15, gy + gh - 2 - sy - seg + 1, fmax (1, bw * 0.7), seg - 1);
			cairo_fill (cr);
		}
	}
	jx_vfd_glare (cr, gx - 2, gy - 2, gw + 4, gh + 4);

	if (full) {
		char t[32];
		double ty = hh - 12;
		snprintf (t, sizeof (t), "%d BANDS", nb);
		double ax = 10;
		ax += jx_annunciator (cr, ax, ty + 2, PRODUCT, true, t);
		ax += jx_annunciator (cr, ax, ty + 2, PRODUCT, self->v_source == 0 && !self->v_invert, "SIDECHAIN");
		ax += jx_annunciator (cr, ax, ty + 2, PRODUCT, self->v_source == 1, "NOISE");
		jx_annunciator (cr, ax, ty + 2, PRODUCT, self->v_invert, "SWAPPED");
		jx_font (cr, JX_FONT_NUMBER, 7);
		jx_set (cr, JX_LEGEND_DIM, 1);
		jx_text (cr, w - 10, ty + 2, 1, "80 Hz – 12 kHz");
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

const LV2_Descriptor jx_voc_descriptor = {
	JX_URI "jx-voc", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
