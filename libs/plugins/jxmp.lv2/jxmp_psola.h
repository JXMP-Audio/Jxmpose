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

/* Formant preserving pitch shift for one voice (PSOLA).
 *
 * The granular shifter (jx_shift) plays the audio faster or slower, so the
 * whole spectrum moves: a voice shifted up sounds small ("chipmunk"), down
 * it sounds big and hollow. PSOLA keeps the speed: it cuts the input into
 * grains of two periods of the voice (a Hann window around a period) and
 * lays them out again closer together (higher) or further apart (lower).
 * The grains are not resampled, so the resonances of the voice (formants)
 * stay where they were and only the pitch changes. Grains are taken whole
 * periods apart, so a grain that repeats or skips a period still lines up
 * with its neighbours.
 *
 * It needs the period of the voice (JxYin, YIN on a decimated copy) and
 * JX_PS_DELAY seconds of latency. Without a detected pitch (noise,
 * breath, silence) the same runs with a fixed 10 ms grain, which leaves
 * the sound unshifted, as it should be for consonants. */

#pragma once

#include "jxmp_dsp.h"

#define JX_YW       1024   /* YIN window (decimated samples) */
#define JX_YHOP     128    /* YIN every so many decimated samples */
#define JX_PS_DELAY 0.015f /* latency (s): half the longest period (33 Hz) */
#define JX_PS_NG    12     /* grains at once: two per period up to +24 semitones */

/* ---- pitch detection ---- */

typedef struct {
	float    sr;
	uint32_t decim, dcount, hop, rpos;
	float    dacc;
	JxBiquad aa[2];
	float    ring[JX_YW];
	float    diff[JX_YW / 2];
	float    period; /* samples at the full rate, 0: no pitch */
} JxYin;

static inline void
jx_yin_init (JxYin* y, float sr)
{
	memset (y, 0, sizeof (JxYin));
	y->sr    = sr;
	y->decim = sr > 13000 ? (uint32_t)floor (sr / 12000.0) : 1;
	jx_bq_set (&y->aa[0], JX_BQ_LP, 0.4 * sr / y->decim, 0.54, 0, sr);
	jx_bq_set (&y->aa[1], JX_BQ_LP, 0.4 * sr / y->decim, 1.31, 0, sr);
}

static inline void
jx_yin_reset (JxYin* y)
{
	memset (y->ring, 0, sizeof (y->ring));
	y->rpos = y->hop = y->dcount = 0;
	y->dacc   = 0;
	y->period = 0;
	jx_bq_reset (&y->aa[0]);
	jx_bq_reset (&y->aa[1]);
}

/* YIN on the last JX_YW decimated samples */
static inline void
jx_yin_detect (JxYin* y)
{
	const float    srd  = y->sr / y->decim;
	const uint32_t tmin = (uint32_t)(srd / 1400.f);
	const uint32_t tmax = JX_YW / 2 - 2;
	const uint32_t len  = JX_YW - tmax - 2;
	float          x[JX_YW];
	double         energy = 0;
	for (uint32_t i = 0; i < JX_YW; ++i) {
		x[i] = y->ring[(y->rpos + i) & (JX_YW - 1)];
		energy += x[i] * x[i];
	}
	if (energy / JX_YW < 1e-6) { /* below -60 dBFS */
		y->period = 0;
		return;
	}
	float* d   = y->diff;
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
		y->period = 0;
		return;
	}
	const float a = d[best - 1], b = d[best], c = d[best + 1];
	float       t   = best;
	const float den = a - 2 * b + c;
	if (fabsf (den) > 1e-9f) {
		t += 0.5f * (a - c) / den;
	}
	const float f = srd / t;
	y->period     = (f < 40 || f > 1400) ? 0 : t * y->decim;
}

/* feed one (mono) sample */
static inline void
jx_yin_feed (JxYin* y, float x)
{
	y->dacc += jx_bq_run (&y->aa[1], jx_bq_run (&y->aa[0], x));
	if (++y->dcount < y->decim) {
		return;
	}
	y->ring[y->rpos] = y->dacc / y->decim;
	y->rpos          = (y->rpos + 1) & (JX_YW - 1);
	y->dacc          = 0;
	y->dcount        = 0;
	if (++y->hop >= JX_YHOP) {
		y->hop = 0;
		jx_yin_detect (y);
	}
}

/* ---- PSOLA ---- */

typedef struct {
	float    delay; /* constant while it plays: the grain is not resampled */
	float    gain;
	uint32_t k, len;
} JxGrain;

typedef struct {
	JxGrain g[JX_PS_NG];
	float   next;  /* samples until the next grain */
	float   lag;   /* how far behind now the centre of the last grain was taken (ages by 1 per sample) */
} JxPsola;

static inline void
jx_psola_reset (JxPsola* s)
{
	memset (s, 0, sizeof (JxPsola));
}

static inline uint32_t
jx_psola_latency (float sr)
{
	return (uint32_t)(JX_PS_DELAY * sr);
}

/* one stereo output sample of the shifter from delay lines d[2] (the sample of `now' already written).
 * `period' from JxYin (0: none). */
static inline void
jx_psola (JxPsola* s, const JxDelay* d, float sr, float ratio, float period, float* yl, float* yr)
{
	const float D = (float)jx_psola_latency (sr);

	s->lag += 1.f;
	if (s->next <= 0.f) {
		/* the period the grains are taken with: the voice's, else 10 ms; at most what the latency allows */
		float T = period > 0.f ? period : 0.01f * sr;
		T       = jx_clamp (T, 32.f, 2.f * D - 8.f);
		ratio   = jx_clamp (ratio, 0.25f, 4.f);
		/* grains two periods long; lower than sung they are spaced further apart than a period and get
		 * longer so that they still overlap by half (Hann windows sum to a constant then) */
		const float h = ratio >= 1.f ? T : T / ratio;

		/* the next period of the input; the grain's centre plays h from now and should be D behind it */
		float       lag  = s->lag - T;
		const float want = D - h;
		while (lag < want - 0.5f * T) {
			lag += T; /* higher than sung: a period is used twice */
		}
		while (lag > want + 0.5f * T) {
			lag -= T; /* lower: a period is skipped */
		}
		s->lag = lag;

		for (int i = 0; i < JX_PS_NG; ++i) {
			JxGrain* g = &s->g[i];
			if (g->k >= g->len) {
				g->len   = (uint32_t)(2.f * h);
				g->k     = 0;
				g->delay = jx_clamp (lag + h, 1.f, (float)d[0].mask - 4.f);
				g->gain  = ratio > 1.f ? 1.f / ratio : 1.f;
				break;
			}
		}
		s->next += T / ratio;
	}
	s->next -= 1.f;

	float l = 0, r = 0;
	for (int i = 0; i < JX_PS_NG; ++i) {
		JxGrain* g = &s->g[i];
		if (g->k >= g->len) {
			continue;
		}
		const float w = g->gain * (0.5f - 0.5f * cosf (2.f * (float)M_PI * g->k / g->len));
		l += w * jx_delay_read (&d[0], g->delay);
		r += w * jx_delay_read (&d[1], g->delay);
		++g->k;
	}
	*yl = l;
	*yr = r;
}
