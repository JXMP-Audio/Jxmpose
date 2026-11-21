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

/* stereo short-time Fourier transform with overlap-add resynthesis.
 * Both channels share one complex FFT (left = real part, right =
 * imaginary part); the frame callback gets the packed spectrum and may
 * change it. Latency: n - hop samples. */

#pragma once

#include "jxmp_dsp.h"

typedef void (*JxStftFrame) (void* ctx, float* re, float* im, uint32_t n);

typedef struct {
	uint32_t n, hop, lat, rover;
	float    scale;
	float*   win;
	float*   in[2];
	float*   out[2];
	float*   acc[2];
	float*   re;
	float*   im;
	JxFFT    fft;
} JxStft;

static inline void
jx_stft_free (JxStft* s)
{
	free (s->win);
	for (int c = 0; c < 2; ++c) {
		free (s->in[c]);
		free (s->out[c]);
		free (s->acc[c]);
	}
	free (s->re);
	free (s->im);
	jx_fft_free (&s->fft);
	memset (s, 0, sizeof (JxStft));
}

static inline void
jx_stft_reset (JxStft* s)
{
	for (int c = 0; c < 2; ++c) {
		memset (s->in[c], 0, sizeof (float) * s->n);
		memset (s->out[c], 0, sizeof (float) * s->n);
		memset (s->acc[c], 0, sizeof (float) * 2 * s->n);
	}
	s->rover = s->lat;
}

static inline bool
jx_stft_init (JxStft* s, uint32_t n, uint32_t hop)
{
	memset (s, 0, sizeof (JxStft));
	s->n   = n;
	s->hop = hop;
	s->lat = n - hop;
	s->win = (float*)malloc (sizeof (float) * n);
	s->re  = (float*)malloc (sizeof (float) * n);
	s->im  = (float*)malloc (sizeof (float) * n);
	bool ok = s->win && s->re && s->im && jx_fft_init (&s->fft, n);
	for (int c = 0; c < 2; ++c) {
		s->in[c]  = (float*)calloc (n, sizeof (float));
		s->out[c] = (float*)calloc (n, sizeof (float));
		s->acc[c] = (float*)calloc (2 * n, sizeof (float));
		ok        = ok && s->in[c] && s->out[c] && s->acc[c];
	}
	if (!ok) {
		jx_stft_free (s);
		return false;
	}
	/* sqrt (periodic hann) for analysis and synthesis: the products sum to n / hop / 2 */
	for (uint32_t i = 0; i < n; ++i) {
		s->win[i] = sqrt (0.5 - 0.5 * cos (2.0 * M_PI * i / n));
	}
	s->scale = 1.f / (n * (n / (float)hop) * 0.5f);
	jx_stft_reset (s);
	return true;
}

static inline void
jx_stft_run (JxStft* s, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t nframes, JxStftFrame cb, void* ctx)
{
	const uint32_t n = s->n, hop = s->hop, lat = s->lat;
	for (uint32_t i = 0; i < nframes; ++i) {
		s->in[0][s->rover] = in_l[i];
		s->in[1][s->rover] = in_r[i];
		out_l[i]           = s->out[0][s->rover - lat];
		out_r[i]           = s->out[1][s->rover - lat];
		if (++s->rover < n) {
			continue;
		}
		s->rover = lat;
		for (uint32_t k = 0; k < n; ++k) {
			s->re[k] = s->in[0][k] * s->win[k];
			s->im[k] = s->in[1][k] * s->win[k];
		}
		jx_fft (&s->fft, s->re, s->im, false);
		cb (ctx, s->re, s->im, n);
		jx_fft (&s->fft, s->re, s->im, true);
		for (uint32_t k = 0; k < n; ++k) {
			s->acc[0][k] += s->re[k] * s->win[k] * s->scale;
			s->acc[1][k] += s->im[k] * s->win[k] * s->scale;
		}
		for (int c = 0; c < 2; ++c) {
			memcpy (s->out[c], s->acc[c], sizeof (float) * hop);
			memmove (s->acc[c], s->acc[c] + hop, sizeof (float) * n);
			memset (s->acc[c] + n, 0, sizeof (float) * hop);
			memmove (s->in[c], s->in[c] + hop, sizeof (float) * lat);
		}
	}
}
