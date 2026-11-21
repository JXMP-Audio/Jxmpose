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

/* JX-SPA: spectrum analyzer on a CRT. The audio passes unchanged. FFT size
 * 1024..8192, window, integration time, floor, slope (dB/octave around
 * 1 kHz) and a peak hold trace. */

#include "jxmp.h"

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_SIZE, P_WINDOW, P_INTEGRATE, P_FLOOR, P_PEAK, P_SLOPE, P_CHANNEL,
	P_LAST
};

#define MAXN    8192
#define MAXBINS (MAXN / 2 + 1)
#define NSIZES  4

typedef struct {
	float* p[P_LAST];
	float  sr;

	JxFFT    fft[NSIZES];
	float    ring[MAXN];
	uint32_t rpos, since;
	float    win[MAXN];
	float    win_sum;
	int      size_idx, win_type;
	float    re[MAXN], im[MAXN];
	float    db[MAXBINS], peak[MAXBINS];

	/* display */
	uint32_t v_nbins;
	float    v_floor;
	bool     v_peak;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Spa;

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Spa* self = (Spa*)calloc (1, sizeof (Spa));
	if (!self) {
		return NULL;
	}
	for (int i = 0; i < NSIZES; ++i) {
		if (!jx_fft_init (&self->fft[i], 1024 << i)) {
			for (int j = 0; j <= i; ++j) {
				jx_fft_free (&self->fft[j]);
			}
			free (self);
			return NULL;
		}
	}
	self->sr              = rate;
	self->size_idx        = -1;
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	for (int k = 0; k < MAXBINS; ++k) {
		self->db[k] = self->peak[k] = -200;
	}
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Spa* self = (Spa*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Spa* self = (Spa*)h;
	memset (self->ring, 0, sizeof (self->ring));
	self->rpos = self->since = 0;
}

static void
make_window (Spa* self, uint32_t n, int type)
{
	double sum = 0;
	for (uint32_t i = 0; i < n; ++i) {
		const double x = 2.0 * M_PI * i / n;
		double       w;
		switch (type) {
			case 0: w = 0.35875 - 0.48829 * cos (x) + 0.14128 * cos (2 * x) - 0.01168 * cos (3 * x); break; /* blackman-harris */
			case 1: w = 0.42 - 0.5 * cos (x) + 0.08 * cos (2 * x); break;                                     /* blackman */
			case 2: w = 0.5 - 0.5 * cos (x); break;                                                             /* hann */
			default: w = 1; break;
		}
		self->win[i] = w;
		sum += w;
	}
	self->win_sum = sum;
}

static void
analyse (Spa* self, uint32_t n, float integrate_ms, float slope)
{
	const JxFFT* fft = &self->fft[self->size_idx];
	for (uint32_t i = 0; i < n; ++i) {
		self->re[i] = self->ring[(self->rpos + MAXN - n + i) & (MAXN - 1)] * self->win[i];
		self->im[i] = 0;
	}
	jx_fft (fft, self->re, self->im, false);

	const uint32_t nb    = n / 2 + 1;
	const float    hop_s = (n / 4) / self->sr;
	const float    a     = integrate_ms > 0 ? expf (-hop_s / (integrate_ms * 0.001f)) : 0.f;
	const float    norm  = 4.f / (self->win_sum * self->win_sum); /* full scale sine = 0 dB */
	const float    decay = 12.f * hop_s;                           /* peak falls 12 dB/s */
	for (uint32_t k = 0; k < nb; ++k) {
		const float pw = (self->re[k] * self->re[k] + self->im[k] * self->im[k]) * norm;
		float       d  = 10.f * log10f (pw + 1e-20f);
		if (k > 0 && slope != 0) {
			d += slope * log2f (k * self->sr / n / 1000.f);
		}
		self->db[k] = a * self->db[k] + (1.f - a) * d;
		self->peak[k] = fmaxf (self->db[k], self->peak[k] - decay);
	}
	self->v_nbins = nb;
}

static void
run (LV2_Handle h, uint32_t n)
{
	Spa*    self = (Spa*)h;
	float** p    = self->p;

	const int size_idx = (int)jx_clamp (rintf (*p[P_SIZE]), 0, NSIZES - 1);
	const int win_type = (int)jx_clamp (rintf (*p[P_WINDOW]), 0, 3);
	const uint32_t fn  = 1024u << size_idx;
	if (size_idx != self->size_idx || win_type != self->win_type) {
		self->size_idx = size_idx;
		self->win_type = win_type;
		make_window (self, fn, win_type);
		for (int k = 0; k < MAXBINS; ++k) {
			self->db[k] = self->peak[k] = -200;
		}
	}
	const int   chn   = (int)jx_clamp (rintf (*p[P_CHANNEL]), 0, 3);
	const float integ = jx_clamp (*p[P_INTEGRATE], 0, 5000);
	const float slope = jx_clamp (*p[P_SLOPE], -6, 6);

	for (uint32_t i = 0; i < n; ++i) {
		const float l = p[P_IN_L][i], r = p[P_IN_R][i];
		float       x;
		switch (chn) {
			case 1: x = l; break;
			case 2: x = r; break;
			case 3: x = 0.5f * (l - r); break;
			default: x = 0.5f * (l + r); break;
		}
		self->ring[self->rpos] = x;
		self->rpos             = (self->rpos + 1) & (MAXN - 1);
		if (++self->since >= fn / 4) {
			self->since = 0;
			analyse (self, fn, integ, slope);
		}
	}
	if (p[P_OUT_L] != p[P_IN_L]) {
		memcpy (p[P_OUT_L], p[P_IN_L], sizeof (float) * n);
	}
	if (p[P_OUT_R] != p[P_IN_R]) {
		memcpy (p[P_OUT_R], p[P_IN_R], sizeof (float) * n);
	}
	self->v_floor = jx_clamp (*p[P_FLOOR], -160, -30);
	self->v_peak  = *p[P_PEAK] > 0.5f;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Spa* self = (Spa*)h;
	for (int i = 0; i < NSIZES; ++i) {
		jx_fft_free (&self->fft[i]);
	}
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT 0x5FF0A0

static void
trace_path (cairo_t* cr, const float* db, uint32_t nb, float sr, float floor_db, double x, double y, double w, double h)
{
	const double bin_hz = sr / (2.0 * (nb - 1));
	bool         first  = true;
	for (double px = 0; px <= w; px += 1) {
		const double f0 = 20 * pow (1000.0, px / w);
		const double f1 = 20 * pow (1000.0, (px + 1) / w);
		double       v;
		uint32_t     k0 = (uint32_t)(f0 / bin_hz), k1 = (uint32_t)(f1 / bin_hz);
		if (k0 + 1 >= nb) {
			break;
		}
		if (k1 > k0) {
			v = -200;
			for (uint32_t k = k0; k <= k1 && k < nb; ++k) {
				v = fmax (v, db[k]);
			}
		} else {
			/* fewer bins than pixels: interpolate */
			const double fk = f0 / bin_hz - k0;
			v               = db[k0] * (1 - fk) + db[k0 + 1] * fk;
		}
		const double py = y + h * fmin (1.0, fmax (0.0, v / floor_db));
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
	Spa*           self = (Spa*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 60, 0.5);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const float    fl   = self->v_floor < -30 ? self->v_floor : -96;
	const uint32_t nb   = self->v_nbins;

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-SPA", "SPECTRUM ANALYZER");
	double gx, gy, gw, gh;
	if (full) {
		gx = 26, gy = y0 + 4, gw = w - 36, gh = hh - y0 - 22;
	} else {
		gx = 3, gy = y0, gw = w - 6, gh = hh - y0 - 3;
	}
	jx_crt (cr, gx, gy, gw, gh, 1, 1);
	const double sx = gx + 3, sy = gy + 3, sw = gw - 6, sh = gh - 6;

	/* grid: decades and 12 dB steps */
	cairo_set_line_width (cr, 0.5);
	cairo_set_source_rgba (cr, 0.49, 1.0, 0.66, 0.1);
	static const double fg[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
	for (int i = 0; i < 8; ++i) {
		const double fx = rint (jx_freq_x (fg[i], sx, sw)) + 0.5;
		cairo_move_to (cr, fx, sy);
		cairo_line_to (cr, fx, sy + sh);
	}
	for (int d = -12; d > fl; d -= 12) {
		const double ly = rint (sy + sh * d / fl) + 0.5;
		cairo_move_to (cr, sx, ly);
		cairo_line_to (cr, sx + sw, ly);
	}
	cairo_stroke (cr);
	if (full) {
		char t[16];
		jx_font (cr, JX_FONT_NUMBER, 7);
		jx_set (cr, JX_LEGEND_DIM, 1);
		for (int d = 0; d > fl; d -= 24) {
			snprintf (t, sizeof (t), "%d", d);
			jx_text (cr, gx - 4, sy + sh * d / fl + 3, 1, t);
		}
		jx_freq_scale (cr, sx, hh - 5, sw);
	}

	if (nb > 2) {
		cairo_save (cr);
		cairo_rectangle (cr, gx + 2, gy + 2, gw - 4, gh - 4);
		cairo_clip (cr);
		if (self->v_peak) {
			trace_path (cr, self->peak, nb, self->sr, fl, sx, sy, sw, sh);
			jx_set (cr, JX_TRACE_CH2, 0.7);
			cairo_set_line_width (cr, 1);
			cairo_stroke (cr);
		}
		trace_path (cr, self->db, nb, self->sr, fl, sx, sy, sw, sh);
		jx_trace (cr, JX_TRACE_CH1, !full);
		cairo_restore (cr);
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

const LV2_Descriptor jx_spa_descriptor = {
	JX_URI "jx-spa", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
