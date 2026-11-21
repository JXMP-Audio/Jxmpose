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

/* small DSP toolbox shared by the JX plugins: gain conversions, one-pole
 * smoothing, RBJ biquads, a radix-2 FFT, delay lines and a granular
 * (two-tap crossfading delay) pitch shifter. Everything is allocated in
 * instantiate (), nothing in run (). */

#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float
jx_db2g (float db)
{
	return db <= -120.f ? 0.f : powf (10.f, 0.05f * db);
}

static inline float
jx_g2db (float g)
{
	return g > 1e-6f ? 20.f * log10f (g) : -120.f;
}

static inline float
jx_clamp (float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* one-pole coefficient for a time constant in ms */
static inline float
jx_tc (float ms, float sr)
{
	if (ms <= 0.01f) {
		return 1.f;
	}
	return 1.f - expf (-1000.f / (ms * sr));
}

/* host calls queue_draw at most ~25 times per second */
typedef struct {
	uint32_t interval;
	uint32_t count;
} JxRedraw;

static inline bool
jx_redraw_due (JxRedraw* r, uint32_t n_samples)
{
	r->count += n_samples;
	if (r->count >= r->interval) {
		r->count = 0;
		return true;
	}
	return false;
}

/* ---- biquad (transposed direct form II) ---- */

typedef struct {
	float b0, b1, b2, a1, a2;
	float z1, z2;
} JxBiquad;

enum { JX_BQ_LP = 0, JX_BQ_HP, JX_BQ_BP, JX_BQ_AP, JX_BQ_PEAK, JX_BQ_LSHELF, JX_BQ_HSHELF };

static inline void
jx_bq_set (JxBiquad* f, int type, double fc, double q, double gain_db, double sr)
{
	if (fc > sr * 0.48) {
		fc = sr * 0.48;
	}
	if (fc < 5) {
		fc = 5;
	}
	const double w0 = 2.0 * M_PI * fc / sr;
	const double cw = cos (w0);
	const double al = sin (w0) / (2.0 * q);
	const double A  = pow (10.0, gain_db / 40.0);
	double b0, b1, b2, a0, a1, a2;
	switch (type) {
		case JX_BQ_LP:
			b0 = (1 - cw) * 0.5; b1 = 1 - cw; b2 = b0;
			a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
			break;
		case JX_BQ_HP:
			b0 = (1 + cw) * 0.5; b1 = -(1 + cw); b2 = b0;
			a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
			break;
		case JX_BQ_BP: /* constant 0 dB peak gain */
			b0 = al; b1 = 0; b2 = -al;
			a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
			break;
		case JX_BQ_AP:
			b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al;
			a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
			break;
		case JX_BQ_PEAK:
			b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A;
			a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
			break;
		case JX_BQ_LSHELF: {
			const double sa = 2 * sqrt (A) * al;
			b0 = A * ((A + 1) - (A - 1) * cw + sa);
			b1 = 2 * A * ((A - 1) - (A + 1) * cw);
			b2 = A * ((A + 1) - (A - 1) * cw - sa);
			a0 = (A + 1) + (A - 1) * cw + sa;
			a1 = -2 * ((A - 1) + (A + 1) * cw);
			a2 = (A + 1) + (A - 1) * cw - sa;
		} break;
		default: { /* high shelf */
			const double sa = 2 * sqrt (A) * al;
			b0 = A * ((A + 1) + (A - 1) * cw + sa);
			b1 = -2 * A * ((A - 1) + (A + 1) * cw);
			b2 = A * ((A + 1) + (A - 1) * cw - sa);
			a0 = (A + 1) - (A - 1) * cw + sa;
			a1 = 2 * ((A - 1) - (A + 1) * cw);
			a2 = (A + 1) - (A - 1) * cw - sa;
		} break;
	}
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
}

static inline float
jx_bq_run (JxBiquad* f, float x)
{
	const float y = f->b0 * x + f->z1;
	f->z1         = f->b1 * x - f->a1 * y + f->z2;
	f->z2         = f->b2 * x - f->a2 * y;
	return y;
}

static inline void
jx_bq_reset (JxBiquad* f)
{
	f->z1 = f->z2 = 0;
}

/* ---- FFT: in-place complex radix-2 ---- */

typedef struct {
	uint32_t  n;
	float*    cs;
	float*    sn;
	uint32_t* rev;
} JxFFT;

static inline bool
jx_fft_init (JxFFT* f, uint32_t n)
{
	f->n   = n;
	f->cs  = (float*)malloc (sizeof (float) * n / 2);
	f->sn  = (float*)malloc (sizeof (float) * n / 2);
	f->rev = (uint32_t*)malloc (sizeof (uint32_t) * n);
	if (!f->cs || !f->sn || !f->rev) {
		return false;
	}
	for (uint32_t i = 0; i < n / 2; ++i) {
		f->cs[i] = cos (2.0 * M_PI * i / n);
		f->sn[i] = -sin (2.0 * M_PI * i / n);
	}
	uint32_t bits = 0;
	while ((1u << bits) < n) {
		++bits;
	}
	for (uint32_t i = 0; i < n; ++i) {
		uint32_t r = 0;
		for (uint32_t b = 0; b < bits; ++b) {
			if (i & (1u << b)) {
				r |= 1u << (bits - 1 - b);
			}
		}
		f->rev[i] = r;
	}
	return true;
}

static inline void
jx_fft_free (JxFFT* f)
{
	free (f->cs);
	free (f->sn);
	free (f->rev);
	f->cs = f->sn = NULL;
	f->rev        = NULL;
}

/* forward (inverse = false) or inverse (unscaled) transform */
static inline void
jx_fft (const JxFFT* f, float* re, float* im, bool inverse)
{
	const uint32_t n = f->n;
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t j = f->rev[i];
		if (j > i) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	const float sgn = inverse ? -1.f : 1.f;
	for (uint32_t len = 2; len <= n; len <<= 1) {
		const uint32_t half = len >> 1;
		const uint32_t step = n / len;
		for (uint32_t i = 0; i < n; i += len) {
			for (uint32_t k = 0; k < half; ++k) {
				const float wr = f->cs[k * step];
				const float wi = sgn * f->sn[k * step];
				const uint32_t a = i + k, b = a + half;
				const float tr = re[b] * wr - im[b] * wi;
				const float ti = re[b] * wi + im[b] * wr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
			}
		}
	}
}

/* two real signals were packed as re = L, im = R: power of bin k (0..n/2) of each */
static inline void
jx_fft_split_power (const float* re, const float* im, uint32_t n, uint32_t k, float* pl, float* pr)
{
	const uint32_t j  = (n - k) & (n - 1);
	const float    lr = 0.5f * (re[k] + re[j]);
	const float    li = 0.5f * (im[k] - im[j]);
	const float    rr = 0.5f * (im[k] + im[j]);
	const float    ri = -0.5f * (re[k] - re[j]);
	*pl               = lr * lr + li * li;
	*pr               = rr * rr + ri * ri;
}

/* split a packed transform into the complex spectra of both channels for bin k */
static inline void
jx_fft_split (const float* re, const float* im, uint32_t n, uint32_t k, float* lr, float* li, float* rr, float* ri)
{
	const uint32_t j = (n - k) & (n - 1);
	*lr              = 0.5f * (re[k] + re[j]);
	*li              = 0.5f * (im[k] - im[j]);
	*rr              = 0.5f * (im[k] + im[j]);
	*ri              = -0.5f * (re[k] - re[j]);
}

/* ---- delay line (power of two) ---- */

typedef struct {
	float*   buf;
	uint32_t mask;
	uint32_t w; /* next write position */
} JxDelay;

static inline bool
jx_delay_init (JxDelay* d, uint32_t min_len)
{
	uint32_t n = 1;
	while (n < min_len) {
		n <<= 1;
	}
	d->buf  = (float*)calloc (n, sizeof (float));
	d->mask = n - 1;
	d->w    = 0;
	return d->buf != NULL;
}

static inline void
jx_delay_free (JxDelay* d)
{
	free (d->buf);
	d->buf = NULL;
}

static inline void
jx_delay_write (JxDelay* d, float x)
{
	d->buf[d->w] = x;
	d->w         = (d->w + 1) & d->mask;
}

/* read `delay' samples behind the last written sample (cubic interpolation, delay >= 1) */
static inline float
jx_delay_read (const JxDelay* d, float delay)
{
	const float    pos = (float)d->w - 1.f - delay;
	const float    fl  = floorf (pos);
	const float    fr  = pos - fl;
	const int32_t  i   = (int32_t)fl;
	const uint32_t m   = d->mask;
	const float    y0  = d->buf[(uint32_t)(i - 1) & m];
	const float    y1  = d->buf[(uint32_t)i & m];
	const float    y2  = d->buf[(uint32_t)(i + 1) & m];
	const float    y3  = d->buf[(uint32_t)(i + 2) & m];
	const float    c1  = 0.5f * (y2 - y0);
	const float    c2  = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
	const float    c3  = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
	return ((c3 * fr + c2) * fr + c1) * fr + y1;
}

/* ---- granular pitch shifter ----
 * Two read taps move through a delay line at `ratio' speed, half a window
 * apart, and crossfade (sin^2 / cos^2, constant sum) so each tap fades out
 * before it jumps. Several shifters can read the same delay line. */

typedef struct {
	float phase; /* 0..1 position in the window */
} JxShifter;

/* one sample of a shifter reading from delay line d */
static inline float
jx_shift (JxShifter* s, const JxDelay* d, float ratio, float win)
{
	float p = s->phase;
	float q = p + 0.5f;
	if (q >= 1.f) {
		q -= 1.f;
	}
	const float g1 = sinf ((float)M_PI * p);
	const float g2 = sinf ((float)M_PI * q);
	const float y  = jx_delay_read (d, 1.f + p * win) * g1 * g1 + jx_delay_read (d, 1.f + q * win) * g2 * g2;
	p += (1.f - ratio) / win;
	if (p >= 1.f) {
		p -= 1.f;
	} else if (p < 0.f) {
		p += 1.f;
	}
	s->phase = p;
	return y;
}

/* xorshift noise, -1..1 */
static inline float
jx_noise (uint32_t* s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return (float)(*s) / 2147483648.f - 1.f;
}
