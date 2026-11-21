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

/* JX-DNR: noise reduction by spectral subtraction. Switch on Learn Noise
 * while only the noise plays, switch it off: the averaged spectrum is the
 * noise profile (saved with the session). Every frequency bin is then
 * reduced by how much of it is noise, down to the Reduction floor, with
 * smoothing over time and frequency against "musical noise". */

#include "jxmp.h"
#include "jxmp_stft.h"

#include <lv2/atom/atom.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_LEARN, P_REDUCTION, P_THRESHOLD, P_SMOOTH, P_GAIN, P_LISTEN, P_LATENCY,
	P_LAST
};

#define MAXBINS 2049

typedef struct {
	float* p[P_LAST];
	float  sr;

	JxStft stft;
	uint32_t nbins;

	/* noise profile (power per bin) */
	float  noise[MAXBINS];
	bool   has_profile;
	double learn_sum[MAXBINS];
	uint32_t learn_frames;
	bool   learning;

	/* profile handed over by restore () */
	float          pending[MAXBINS];
	uint32_t       pending_bins;
	volatile int   pending_ready;

	float gain[MAXBINS]; /* smoothed gain per bin */
	float tmp[MAXBINS];

	/* parameters for the frame callback */
	float floor_g, thr, smooth;
	bool  listen;
	float out_gain;

	/* display: spectra in dB */
	float v_in[MAXBINS], v_out[MAXBINS], v_noise[MAXBINS]; /* v_noise: GUI thread only */
	float v_reduction;
	bool  v_learning;

	LV2_URID_Map* map;
	LV2_URID      atom_Vector, atom_Float, dnr_profile;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Dnr;

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	LV2_URID_Map* map = (LV2_URID_Map*)jx_feature (features, LV2_URID__map);
	if (!map) {
		return NULL;
	}
	Dnr* self = (Dnr*)calloc (1, sizeof (Dnr));
	if (!self) {
		return NULL;
	}
	const uint32_t n = rate > 50000 ? 4096 : 2048;
	if (!jx_stft_init (&self->stft, n, n / 4)) {
		free (self);
		return NULL;
	}
	self->sr              = rate;
	self->nbins           = n / 2 + 1;
	self->map             = map;
	self->atom_Vector     = map->map (map->handle, LV2_ATOM__Vector);
	self->atom_Float      = map->map (map->handle, LV2_ATOM__Float);
	self->dnr_profile     = map->map (map->handle, JX_URI "jx-dnr#profile");
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	self->out_gain        = 1;
	for (uint32_t k = 0; k < MAXBINS; ++k) {
		self->gain[k] = 1;
		self->v_in[k] = self->v_out[k] = -120;
	}
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Dnr* self = (Dnr*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
activate (LV2_Handle h)
{
	Dnr* self = (Dnr*)h;
	jx_stft_reset (&self->stft);
	for (uint32_t k = 0; k < MAXBINS; ++k) {
		self->gain[k] = 1;
	}
}

static void
frame (void* ctx, float* re, float* im, uint32_t n)
{
	Dnr*           self  = (Dnr*)ctx;
	const uint32_t nb    = self->nbins;
	/* full scale sine -> 0 dB (sqrt hann window: coherent gain 2 / pi) */
	const float    norm  = 1.f / (n * 0.3183f * n * 0.3183f);
	const float    sm    = self->smooth;

	for (uint32_t k = 0; k < nb; ++k) {
		float pl, pr;
		jx_fft_split_power (re, im, n, k, &pl, &pr);
		const float pw = 0.5f * (pl + pr);
		self->tmp[k]   = pw;
		self->v_in[k]  = 0.7f * self->v_in[k] + 0.3f * (10.f * log10f (pw * norm + 1e-12f));
		if (self->learning) {
			self->learn_sum[k] += pw;
		}
	}
	if (self->learning) {
		++self->learn_frames;
	}

	if (!self->has_profile || self->learning) {
		for (uint32_t k = 0; k < nb; ++k) {
			self->v_out[k] = self->v_in[k];
		}
		return;
	}

	/* gain per bin: power subtraction, limited by the floor */
	for (uint32_t k = 0; k < nb; ++k) {
		const float nz = self->noise[k] * self->thr;
		const float pw = self->tmp[k];
		float       g  = pw > 1e-20f ? 1.f - nz / pw : 0.f;
		g              = g > 0.f ? sqrtf (g) : 0.f;
		if (g < self->floor_g) {
			g = self->floor_g;
		}
		/* fast release of the reduction (speech onsets), smoothed attack */
		self->gain[k] = g > self->gain[k] ? g : sm * self->gain[k] + (1.f - sm) * g;
	}
	/* smooth across neighbouring bins */
	self->tmp[0] = self->gain[0];
	for (uint32_t k = 1; k + 1 < nb; ++k) {
		self->tmp[k] = 0.25f * self->gain[k - 1] + 0.5f * self->gain[k] + 0.25f * self->gain[k + 1];
	}
	self->tmp[nb - 1] = self->gain[nb - 1];

	for (uint32_t k = 0; k < nb; ++k) {
		float g = self->tmp[k];
		if (self->listen) {
			g = 1.f - g;
		}
		const uint32_t j = (n - k) & (n - 1);
		re[k] *= g;
		im[k] *= g;
		if (j != k) {
			re[j] *= g;
			im[j] *= g;
		}
		self->v_out[k] = self->v_in[k] + 20.f * log10f (g + 1e-6f);
	}
}

static void
run (LV2_Handle h, uint32_t n)
{
	Dnr*    self = (Dnr*)h;
	float** p    = self->p;

	if (self->pending_ready) {
		const uint32_t nb = self->pending_bins < self->nbins ? self->pending_bins : self->nbins;
		memcpy (self->noise, self->pending, sizeof (float) * nb);
		self->has_profile   = nb == self->nbins;
		self->pending_ready = 0;
	}

	const bool learn = *p[P_LEARN] > 0.5f;
	if (learn && !self->learning) {
		memset (self->learn_sum, 0, sizeof (self->learn_sum));
		self->learn_frames = 0;
	} else if (!learn && self->learning && self->learn_frames > 0) {
		for (uint32_t k = 0; k < self->nbins; ++k) {
			self->noise[k] = self->learn_sum[k] / self->learn_frames;
		}
		self->has_profile = true;
	}
	self->learning = learn;

	self->floor_g = jx_db2g (-jx_clamp (*p[P_REDUCTION], 0, 60));
	self->thr     = powf (10.f, 0.1f * jx_clamp (*p[P_THRESHOLD], -12, 24));
	/* per frame smoothing coefficient from 0..100 % */
	self->smooth  = 0.9f * jx_clamp (*p[P_SMOOTH], 0, 100) * 0.01f;
	self->listen  = *p[P_LISTEN] > 0.5f;

	jx_stft_run (&self->stft, p[P_IN_L], p[P_IN_R], p[P_OUT_L], p[P_OUT_R], n, frame, self);

	const float g   = jx_db2g (*p[P_GAIN]);
	const float gtc = jx_tc (10.f, self->sr);
	for (uint32_t i = 0; i < n; ++i) {
		self->out_gain += gtc * (g - self->out_gain);
		p[P_OUT_L][i] *= self->out_gain;
		p[P_OUT_R][i] *= self->out_gain;
	}

	*p[P_LATENCY]     = self->stft.lat;
	self->v_reduction = *p[P_REDUCTION];
	self->v_learning  = learn;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Dnr* self = (Dnr*)h;
	jx_stft_free (&self->stft);
	jx_display_free (&self->disp);
	free (self);
}

/* ---- state: the noise profile ---- */

static LV2_State_Status
save (LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t flags, const LV2_Feature* const* features)
{
	Dnr* self = (Dnr*)h;
	if (!self->has_profile) {
		return LV2_STATE_SUCCESS;
	}
	const size_t sz  = sizeof (LV2_Atom_Vector_Body) + sizeof (float) * self->nbins;
	uint8_t*     buf = (uint8_t*)malloc (sz);
	if (!buf) {
		return LV2_STATE_ERR_UNKNOWN;
	}
	LV2_Atom_Vector_Body* vb = (LV2_Atom_Vector_Body*)buf;
	vb->child_size           = sizeof (float);
	vb->child_type           = self->atom_Float;
	memcpy (buf + sizeof (LV2_Atom_Vector_Body), self->noise, sizeof (float) * self->nbins);
	store (handle, self->dnr_profile, buf, sz, self->atom_Vector, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
	free (buf);
	return LV2_STATE_SUCCESS;
}

static LV2_State_Status
restore (LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, uint32_t flags, const LV2_Feature* const* features)
{
	Dnr*        self = (Dnr*)h;
	size_t      size;
	uint32_t    type, vflags;
	const void* value = retrieve (handle, self->dnr_profile, &size, &type, &vflags);
	if (!value || type != self->atom_Vector || size < sizeof (LV2_Atom_Vector_Body)) {
		return LV2_STATE_SUCCESS;
	}
	const LV2_Atom_Vector_Body* vb = (const LV2_Atom_Vector_Body*)value;
	if (vb->child_type != self->atom_Float || vb->child_size != sizeof (float)) {
		return LV2_STATE_ERR_BAD_TYPE;
	}
	uint32_t nb = (size - sizeof (LV2_Atom_Vector_Body)) / sizeof (float);
	if (nb < 2) {
		return LV2_STATE_ERR_BAD_TYPE;
	}
	if (nb > MAXBINS) {
		nb = MAXBINS;
	}
	/* a profile taken at another FFT size is resampled to ours */
	const float* src = (const float*)(vb + 1);
	for (uint32_t k = 0; k < self->nbins; ++k) {
		const uint32_t s = (uint32_t)((uint64_t)k * (nb - 1) / (self->nbins - 1));
		self->pending[k] = src[s];
	}
	self->pending_bins  = self->nbins;
	self->pending_ready = 1;
	return LV2_STATE_SUCCESS;
}

/* ---- display ---- */

#define PRODUCT 0x7FA7C9

/* path of a spectrum (dB per bin) on a log frequency axis, -100..0 dB */
static void
spectrum_path (cairo_t* cr, const float* db, uint32_t nb, float sr, double x, double y, double w, double h, bool fill)
{
	const double bin_hz = sr / (2.0 * (nb - 1));
	bool         first  = true;
	for (double px = 0; px <= w; px += 1) {
		const double f0 = 20 * pow (1000.0, px / w);
		const double f1 = 20 * pow (1000.0, (px + 1) / w);
		uint32_t     k0 = (uint32_t)(f0 / bin_hz);
		uint32_t     k1 = (uint32_t)(f1 / bin_hz);
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
		const double py = y + h * fmin (1.0, fmax (0.0, -v / 100.0));
		if (first) {
			if (fill) {
				cairo_move_to (cr, x + px, y + h);
				cairo_line_to (cr, x + px, py);
			} else {
				cairo_move_to (cr, x + px, py);
			}
			first = false;
		} else {
			cairo_line_to (cr, x + px, py);
		}
	}
	if (fill && !first) {
		cairo_line_to (cr, x + w, y + h);
		cairo_close_path (cr);
	}
}

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Dnr*           self = (Dnr*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 50, 0.45);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const uint32_t nb   = self->nbins;

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-DNR", "NOISE REDUCTION");
	double gx, gy, gw, gh;
	if (full) {
		jx_section (cr, 8, y0 + 4, w - 16, PRODUCT, "SPECTRUM · NOISE PROFILE");
		gx = 10, gy = y0 + 12, gw = w - 20, gh = hh - y0 - 44;
	} else {
		gx = 3, gy = y0, gw = w - 6, gh = hh - y0 - 3;
	}
	jx_crt (cr, gx, gy, gw, gh, full ? 10 : 4, full ? 5 : 2);
	cairo_save (cr);
	cairo_rectangle (cr, gx + 2, gy + 2, gw - 4, gh - 4);
	cairo_clip (cr);
	double sx = gx + 3, sy = gy + 3, sw = gw - 6, sh = gh - 6;

	/* noise profile: amber */
	if (self->has_profile) {
		const float norm = 1.f / (self->stft.n * 0.3183f * self->stft.n * 0.3183f);
		for (uint32_t k = 0; k < nb; ++k) {
			self->v_noise[k] = 10.f * log10f (self->noise[k] * norm + 1e-12f);
		}
		spectrum_path (cr, self->v_noise, nb, self->sr, sx, sy, sw, sh, true);
		jx_set (cr, JX_TRACE_CH2, 0.15);
		cairo_fill_preserve (cr);
		cairo_new_path (cr);
		spectrum_path (cr, self->v_noise, nb, self->sr, sx, sy, sw, sh, false);
		jx_trace (cr, JX_TRACE_CH2, !full);
	}
	/* input dim, output bright */
	spectrum_path (cr, self->v_in, nb, self->sr, sx, sy, sw, sh, false);
	jx_set (cr, JX_TRACE_CH1, 0.3);
	cairo_set_line_width (cr, 1);
	cairo_stroke (cr);
	spectrum_path (cr, self->v_out, nb, self->sr, sx, sy, sw, sh, false);
	jx_trace (cr, JX_TRACE_CH1, !full);
	cairo_restore (cr);

	if (self->v_learning) {
		jx_set (cr, JX_TRACE_CH2, 1);
		jx_font (cr, JX_FONT_LABEL, full ? 11 : 8);
		jx_text (cr, gx + gw - 6, gy + (full ? 16 : 11), 1, "LEARNING");
	} else if (!self->has_profile && full) {
		jx_set (cr, JX_TRACE_CH2, 0.8);
		jx_font (cr, JX_FONT_LABEL, 10);
		jx_text (cr, gx + gw * 0.5, gy + gh * 0.5, 0, "NO PROFILE: PLAY ONLY THE NOISE WITH LEARN NOISE ON");
	}

	if (full) {
		jx_freq_scale (cr, sx, hh - 18, sw);
		char t[32];
		jx_annunciator (cr, 10, hh - 5, JX_TRACE_CH2, self->v_learning, "LEARN");
		snprintf (t, sizeof (t), "REDUCTION %.0f dB", self->v_reduction);
		jx_font (cr, JX_FONT_NUMBER, 8);
		jx_set (cr, JX_LEGEND_DIM, 1);
		jx_text (cr, w - 10, hh - 5, 1, t);
	}
	return jx_display_end (&self->disp, cr);
}

static const void*
extension_data (const char* uri)
{
	static const LV2_Inline_Display_Interface display = { render };
	static const LV2_State_Interface          state   = { save, restore };
	if (!strcmp (uri, LV2_INLINEDISPLAY__interface)) {
		return &display;
	}
	if (!strcmp (uri, LV2_STATE__interface)) {
		return &state;
	}
	return NULL;
}

const LV2_Descriptor jx_dnr_descriptor = {
	JX_URI "jx-dnr", instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};
