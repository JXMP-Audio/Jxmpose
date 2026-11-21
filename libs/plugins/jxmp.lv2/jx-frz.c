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

/* JX-FRZ: spectral freeze. Hold takes a snapshot of the spectrum of what
 * plays and sustains it as a phase-vocoder drone (each bin keeps turning at
 * its own frequency, with a little random phase drift) until Hold is
 * switched off. Recapture takes a new snapshot while holding. The dry
 * signal is not delayed. */

#include "jxmp.h"
#include "jxmp_stft.h"

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_HOLD, P_RECAPTURE, P_LEVEL, P_DRY_HOLD, P_DRY, P_DRIFT, P_FADE,
	P_LAST
};

#define MAXBINS 4097

typedef struct {
	float* p[P_LAST];
	float  sr;

	JxStft   stft;
	uint32_t nbins;

	float smag[2][MAXBINS];  /* running magnitude spectrum of the input */
	float mag[2][MAXBINS];   /* held spectrum */
	float phase[2][MAXBINS];
	bool  holding;
	bool  capture;           /* take a snapshot in the next frame */
	float hold_g;            /* per frame fade of the held sound */
	float hold_tgt, fade_step, level, drift;
	uint32_t rnd;
	float* wet[2];
	float  g_dry;
	bool   last_hold, last_recapture;

	float v_live[MAXBINS], v_held[MAXBINS];
	float v_hold_g;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Frz;

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Frz* self = (Frz*)calloc (1, sizeof (Frz));
	if (!self) {
		return NULL;
	}
	const uint32_t n = rate > 50000 ? 8192 : 4096;
	self->wet[0]     = (float*)calloc (8192, sizeof (float));
	self->wet[1]     = (float*)calloc (8192, sizeof (float));
	if (!self->wet[0] || !self->wet[1] || !jx_stft_init (&self->stft, n, n / 4)) {
		free (self->wet[0]);
		free (self->wet[1]);
		free (self);
		return NULL;
	}
	self->sr              = rate;
	self->nbins           = n / 2 + 1;
	self->rnd             = 0x2545F491;
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	self->g_dry           = 1;
	for (uint32_t k = 0; k < MAXBINS; ++k) {
		self->v_live[k] = self->v_held[k] = -120;
	}
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Frz* self = (Frz*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Frz* self = (Frz*)h;
	jx_stft_reset (&self->stft);
	memset (self->smag, 0, sizeof (self->smag));
	self->hold_g = 0;
}

static void
frame (void* ctx, float* re, float* im, uint32_t n)
{
	Frz*           self = (Frz*)ctx;
	const uint32_t nb   = self->nbins;
	const float    norm = 20.f * log10f (2.f / (n * 0.6366f));

	for (uint32_t k = 0; k < nb; ++k) {
		float lr, li, rr, ri;
		jx_fft_split (re, im, n, k, &lr, &li, &rr, &ri);
		const float ml = sqrtf (lr * lr + li * li);
		const float mr = sqrtf (rr * rr + ri * ri);
		self->smag[0][k] = 0.5f * self->smag[0][k] + 0.5f * ml;
		self->smag[1][k] = 0.5f * self->smag[1][k] + 0.5f * mr;
		if (self->capture) {
			self->mag[0][k]   = self->smag[0][k];
			self->mag[1][k]   = self->smag[1][k];
			self->phase[0][k] = atan2f (li, lr);
			self->phase[1][k] = atan2f (ri, rr);
		}
		self->v_live[k] = 20.f * log10f (0.5f * (ml + mr) + 1e-9f) + norm;
	}
	if (self->capture) {
		for (uint32_t k = 0; k < nb; ++k) {
			self->v_held[k] = 20.f * log10f (0.5f * (self->mag[0][k] + self->mag[1][k]) + 1e-9f) + norm;
		}
		self->capture = false;
	}

	/* fade the held sound in / out, one step per frame */
	if (self->hold_g < self->hold_tgt) {
		self->hold_g = fminf (self->hold_tgt, self->hold_g + self->fade_step);
	} else if (self->hold_g > self->hold_tgt) {
		self->hold_g = fmaxf (self->hold_tgt, self->hold_g - self->fade_step);
	}

	if (self->hold_g <= 0.f) {
		memset (re, 0, sizeof (float) * n);
		memset (im, 0, sizeof (float) * n);
		return;
	}

	/* resynthesize: every bin advances by its centre frequency over one hop */
	const float g    = self->hold_g * self->level;
	const float dphi = 2.f * (float)M_PI * self->stft.hop / n;
	for (uint32_t k = 0; k < nb; ++k) {
		float x[2], y[2];
		for (int c = 0; c < 2; ++c) {
			float ph = self->phase[c][k] + dphi * k + self->drift * (float)M_PI * jx_noise (&self->rnd);
			ph       = fmodf (ph, 2.f * (float)M_PI);
			self->phase[c][k] = ph;
			x[c]     = g * self->mag[c][k] * cosf (ph);
			y[c]     = (k == 0 || k == n / 2) ? 0.f : g * self->mag[c][k] * sinf (ph);
		}
		/* pack left + i right (both hermitian) */
		re[k] = x[0] - y[1];
		im[k] = y[0] + x[1];
		if (k > 0 && k < n / 2) {
			re[n - k] = x[0] + y[1];
			im[n - k] = -y[0] + x[1];
		}
	}
}

static void
run (LV2_Handle h, uint32_t n)
{
	Frz*    self = (Frz*)h;
	float** p    = self->p;

	const bool hold = *p[P_HOLD] > 0.5f;
	const bool recapture = *p[P_RECAPTURE] > 0.5f;
	if ((hold && !self->last_hold) || (hold && recapture && !self->last_recapture)) {
		self->capture = true;
	}
	self->last_hold      = hold;
	self->last_recapture = recapture;
	self->holding        = hold;
	self->hold_tgt       = hold ? 1.f : 0.f;
	self->level          = jx_db2g (*p[P_LEVEL]);
	self->drift          = jx_clamp (*p[P_DRIFT], 0, 100) * 0.01f;
	const float frames   = jx_clamp (*p[P_FADE], 10, 5000) * 0.001f * self->sr / self->stft.hop;
	self->fade_step      = 1.f / fmaxf (1.f, frames);

	const float dry = jx_db2g (hold ? *p[P_DRY_HOLD] : *p[P_DRY]);
	const float tc  = jx_tc (jx_clamp (*p[P_FADE], 10, 5000) * 0.3f, self->sr);

	for (uint32_t off = 0; off < n;) {
		const uint32_t ns = n - off > 8192 ? 8192 : n - off;
		jx_stft_run (&self->stft, p[P_IN_L] + off, p[P_IN_R] + off, self->wet[0], self->wet[1], ns, frame, self);
		for (uint32_t i = 0; i < ns; ++i) {
			self->g_dry += tc * (dry - self->g_dry);
			const float xl = p[P_IN_L][off + i], xr = p[P_IN_R][off + i];
			p[P_OUT_L][off + i] = self->wet[0][i] + self->g_dry * xl;
			p[P_OUT_R][off + i] = self->wet[1][i] + self->g_dry * xr;
		}
		off += ns;
	}
	self->v_hold_g = self->hold_g;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Frz* self = (Frz*)h;
	jx_stft_free (&self->stft);
	free (self->wet[0]);
	free (self->wet[1]);
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0xA8D8F0

static void
spectrum_path (cairo_t* cr, const float* db, uint32_t nb, float sr, double x, double y, double w, double h)
{
	const double bin_hz = sr / (2.0 * (nb - 1));
	bool         first  = true;
	for (double px = 0; px <= w; px += 1) {
		uint32_t k0 = (uint32_t)(20 * pow (1000.0, px / w) / bin_hz);
		uint32_t k1 = (uint32_t)(20 * pow (1000.0, (px + 1) / w) / bin_hz);
		if (k0 >= nb) {
			break;
		}
		if (k1 >= nb) {
			k1 = nb - 1;
		}
		float v = -200;
		for (uint32_t k = k0; k <= k1; ++k) {
			v = fmaxf (v, db[k]);
		}
		const double py = y + h * fmin (1.0, fmax (0.0, -v / 90.0));
		if (first) {
			cairo_move_to (cr, x + px, py);
			first = false;
		} else {
			cairo_line_to (cr, x + px, py);
		}
	}
}

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Frz*           self = (Frz*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 44, 0.42);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const bool     held = self->v_hold_g > 0.f;

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-FRZ", "SPECTRAL FREEZE");
	double gx, gy, gw, gh;
	if (full) {
		jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, "LIVE · HELD SPECTRUM");
		gx = 10, gy = y0 + 12, gw = w - 20, gh = hh - y0 - 40;
	} else {
		gx = 3, gy = y0, gw = w - 6, gh = hh - y0 - 3;
	}
	jx_crt (cr, gx, gy, gw, gh, full ? 10 : 4, full ? 6 : 2);
	cairo_save (cr);
	cairo_rectangle (cr, gx + 2, gy + 2, gw - 4, gh - 4);
	cairo_clip (cr);
	spectrum_path (cr, self->v_live, self->nbins, self->sr, gx + 3, gy + 3, gw - 6, gh - 6);
	if (held) {
		jx_set (cr, JX_TRACE_CH1, 0.35);
		cairo_set_line_width (cr, 1);
		cairo_stroke (cr);
		spectrum_path (cr, self->v_held, self->nbins, self->sr, gx + 3, gy + 3, gw - 6, gh - 6);
		jx_trace (cr, JX_TRACE_CH2, !full);
	} else {
		jx_trace (cr, JX_TRACE_CH1, !full);
	}
	cairo_restore (cr);

	if (full) {
		jx_freq_scale (cr, gx + 3, hh - 14, gw - 6);
		jx_annunciator (cr, w - 52, hh - 4, JX_TRACE_CH2, held, "HOLD");
	} else if (held) {
		jx_set (cr, JX_TRACE_CH2, 1);
		jx_font (cr, JX_FONT_LABEL, 8);
		jx_text (cr, gx + gw - 5, gy + 11, 1, "HOLD");
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

const LV2_Descriptor jx_frz_descriptor = {
	JX_URI "jx-frz", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
