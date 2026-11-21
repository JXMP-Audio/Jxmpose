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

/* JX-LOOP: eight synchronised loop slots. The first recording sets the
 * loop length for all slots (switch Rec on, play, switch Rec off). Later
 * recordings run in sync: on an empty slot the first pass is recorded and
 * further passes are overdubbed, on a slot with a loop Rec overdubs. Play
 * (on by default) makes a slot heard, Reverse plays it backwards, Clear
 * empties it. Every control can be MIDI learned. Up to 30 s per loop. */

#include "jxmp.h"

#define NSLOT 8
#define MAXSEC 30

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R,
	P_MONITOR, P_LEVEL,
	P_SLOT, /* 5 per slot: rec, play, reverse, level, clear */
	P_LENGTH = P_SLOT + 5 * NSLOT,
	P_POS,
	P_LAST
};

enum { S_REC = 0, S_PLAY, S_REV, S_LEVEL, S_CLEAR };
#define SP(s, x) (P_SLOT + 5 * (s) + (x))

typedef struct {
	float*   buf[2];
	bool     has;      /* holds a complete loop */
	bool     rec;      /* recording (first pass) or overdubbing */
	bool     dub;
	uint32_t start;    /* first pass: where it started */
	uint32_t written;  /* first pass: samples recorded so far */
	bool     last_clear;
	float    g;        /* smoothed playback gain */
	float    peak;
} Slot;

typedef struct {
	float* p[P_LAST];
	float  sr;

	Slot     s[NSLOT];
	uint32_t maxlen;
	uint32_t len;       /* loop length, 0: not set */
	uint32_t pos;
	int      free_rec;  /* slot that records the first loop (sets the length), -1: none */
	uint32_t free_len;
	float    g_mon, g_out;

	/* display */
	int   v_state[NSLOT]; /* 0 empty, 1 stopped, 2 play, 3 rec, 4 overdub */
	bool  v_rev[NSLOT];
	float v_peak[NSLOT];
	float v_pos, v_len;
	int   v_free;

	JxDisplay           disp;
	LV2_Inline_Display* queue_draw;
	JxRedraw            redraw;
} Loop;

static LV2_Handle
instantiate (const LV2_Descriptor* d, double rate, const char* path, const LV2_Feature* const* features)
{
	Loop* self = (Loop*)calloc (1, sizeof (Loop));
	if (!self) {
		return NULL;
	}
	self->sr     = rate;
	self->maxlen = (uint32_t)(rate * MAXSEC);
	/* calloc'ed memory is mapped lazily: pages are only used once recorded into */
	for (int i = 0; i < NSLOT; ++i) {
		for (int c = 0; c < 2; ++c) {
			self->s[i].buf[c] = (float*)calloc (self->maxlen, sizeof (float));
			if (!self->s[i].buf[c]) {
				for (int j = 0; j <= i; ++j) {
					free (self->s[j].buf[0]);
					free (self->s[j].buf[1]);
				}
				free (self);
				return NULL;
			}
		}
	}
	self->free_rec        = -1;
	self->queue_draw      = jx_queue_draw_feature (features);
	self->redraw.interval = rate / 25;
	return self;
}

static void
connect_port (LV2_Handle h, uint32_t port, void* data)
{
	Loop* self = (Loop*)h;
	if (port < P_LAST) {
		self->p[port] = (float*)data;
	}
}

static void
clear_slot (Loop* self, int i)
{
	Slot* s    = &self->s[i];
	s->has     = false;
	s->rec     = false;
	s->dub     = false;
	s->written = 0;
	if (self->free_rec == i) {
		self->free_rec = -1;
	}
	bool any = false;
	for (int k = 0; k < NSLOT; ++k) {
		any |= self->s[k].has || self->s[k].rec;
	}
	if (!any) {
		self->len = 0;
		self->pos = 0;
	}
}

/* switch recording of slot i according to its Rec control */
static void
update_rec (Loop* self, int i, bool want)
{
	Slot* s = &self->s[i];
	if (want && !s->rec) {
		if (self->len == 0) {
			if (self->free_rec < 0) {
				/* the first loop: record freely, the length is set when Rec goes off */
				self->free_rec = i;
				self->free_len = 0;
				s->rec         = true;
				s->dub         = false;
				s->has         = false;
			}
			/* else wait until the first loop is set */
		} else if (s->has) {
			s->rec = true;
			s->dub = true;
		} else {
			s->rec     = true;
			s->dub     = false;
			s->start   = self->pos;
			s->written = 0;
		}
	} else if (!want && s->rec) {
		if (self->free_rec == i) {
			if (self->free_len < self->sr * 0.05f) {
				clear_slot (self, i); /* too short: forget it */
				return;
			}
			self->len      = self->free_len;
			self->pos      = 0;
			self->free_rec = -1;
			s->has         = true;
		} else if (!s->dub) {
			/* first pass stopped early: the recorded part is kept, the rest is silence */
			for (uint32_t k = s->written; k < self->len; ++k) {
				const uint32_t idx = (s->start + k) % self->len;
				s->buf[0][idx]     = 0;
				s->buf[1][idx]     = 0;
			}
			s->has = s->written > 0;
		}
		s->rec = false;
		s->dub = false;
	}
}

static void
run (LV2_Handle h, uint32_t n)
{
	Loop*   self = (Loop*)h;
	float** p    = self->p;

	bool any_rec = false;
	for (int i = 0; i < NSLOT; ++i) {
		const bool clr = *p[SP (i, S_CLEAR)] > 0.5f;
		if (clr && !self->s[i].last_clear) {
			clear_slot (self, i);
		}
		self->s[i].last_clear = clr;
		update_rec (self, i, *p[SP (i, S_REC)] > 0.5f && !clr);
		any_rec |= self->s[i].rec;
	}

	const int   mon     = (int)rintf (*p[P_MONITOR]);
	const float mon_tgt = (mon == 1 || (mon == 0 && any_rec)) ? 1.f : 0.f;
	const float out_tgt = jx_db2g (*p[P_LEVEL]);
	const float tc      = jx_tc (10.f, self->sr);
	float       g_tgt[NSLOT];
	bool        rev[NSLOT];
	for (int i = 0; i < NSLOT; ++i) {
		g_tgt[i]          = *p[SP (i, S_PLAY)] > 0.5f ? jx_db2g (*p[SP (i, S_LEVEL)]) : 0.f;
		rev[i]            = *p[SP (i, S_REV)] > 0.5f;
		self->s[i].peak   = 0;
	}

	for (uint32_t k = 0; k < n; ++k) {
		const float x[2] = { p[P_IN_L][k], p[P_IN_R][k] };
		float       y[2] = { 0, 0 };

		if (self->free_rec >= 0) {
			Slot* s = &self->s[self->free_rec];
			s->buf[0][self->free_len] = x[0];
			s->buf[1][self->free_len] = x[1];
			if (++self->free_len >= self->maxlen) {
				/* out of memory for the loop: close it here */
				self->len      = self->free_len;
				self->pos      = 0;
				self->free_rec = -1;
				s->has         = true;
				s->rec         = false;
			}
		} else if (self->len > 0) {
			const uint32_t pos = self->pos;
			for (int i = 0; i < NSLOT; ++i) {
				Slot* s = &self->s[i];
				s->g += tc * (g_tgt[i] - s->g);
				if (s->has && s->g > 1e-5f) {
					const uint32_t idx = rev[i] ? self->len - 1 - pos : pos;
					const float    l   = s->buf[0][idx] * s->g;
					const float    r   = s->buf[1][idx] * s->g;
					y[0] += l;
					y[1] += r;
					s->peak = fmaxf (s->peak, fmaxf (fabsf (l), fabsf (r)));
				}
				if (s->rec) {
					if (s->dub) {
						s->buf[0][pos] += x[0];
						s->buf[1][pos] += x[1];
					} else {
						s->buf[0][pos] = x[0];
						s->buf[1][pos] = x[1];
						++s->written;
					}
				}
			}
			if (++self->pos >= self->len) {
				self->pos = 0;
			}
			/* a first pass that went all the way round becomes a loop and overdubs from now on */
			for (int i = 0; i < NSLOT; ++i) {
				Slot* s = &self->s[i];
				if (s->rec && !s->dub && s->written >= self->len) {
					s->has = true;
					s->dub = true;
				}
			}
		}

		self->g_mon += tc * (mon_tgt - self->g_mon);
		self->g_out += tc * (out_tgt - self->g_out);
		p[P_OUT_L][k] = self->g_mon * x[0] + self->g_out * y[0];
		p[P_OUT_R][k] = self->g_mon * x[1] + self->g_out * y[1];
	}

	*p[P_LENGTH] = (self->free_rec >= 0 ? self->free_len : self->len) / self->sr;
	*p[P_POS]    = self->len > 0 ? self->pos / (float)self->len : 0.f;

	for (int i = 0; i < NSLOT; ++i) {
		const Slot* s = &self->s[i];
		int         st;
		if (s->rec) {
			st = s->dub ? 4 : 3;
		} else if (s->has) {
			st = g_tgt[i] > 0 ? 2 : 1;
		} else {
			st = 0;
		}
		self->v_state[i] = st;
		self->v_rev[i]   = rev[i];
		self->v_peak[i]  = s->peak;
	}
	self->v_pos  = *p[P_POS];
	self->v_len  = *p[P_LENGTH];
	self->v_free = self->free_rec;

	if (self->queue_draw && jx_redraw_due (&self->redraw, n)) {
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
cleanup (LV2_Handle h)
{
	Loop* self = (Loop*)h;
	for (int i = 0; i < NSLOT; ++i) {
		free (self->s[i].buf[0]);
		free (self->s[i].buf[1]);
	}
	jx_display_free (&self->disp);
	free (self);
}

#define PRODUCT  0xFF6F91
#define C_DUB    0xFF8A1C

static uint32_t
state_color (int st)
{
	switch (st) {
		case 2: return JX_LED_GREEN;
		case 3: return JX_LED_RED;
		case 4: return C_DUB;
		default: return JX_LED_GREEN;
	}
}

static LV2_Inline_Display_Image_Surface*
render (LV2_Handle h, uint32_t w, uint32_t max_h)
{
	Loop*          self = (Loop*)h;
	const uint32_t hh   = jx_display_height (w, max_h, 36, 0.62);
	cairo_t*       cr   = jx_display_begin (&self->disp, w, hh);
	const bool     full = hh >= JX_FULL_H;
	const JxRGB    lit  = jx_vfd_tint (PRODUCT);
	static const char* const sname[] = { "EMPTY", "STOP", "PLAY", "REC", "DUB" };
	char t[32];

	double y0 = jx_panel (cr, w, hh, PRODUCT, "JX-LOOP", "LOOPER · 8 SLOTS");

	if (!full) {
		const double pitch = (w - 8) / (double)NSLOT;
		for (int i = 0; i < NSLOT; ++i) {
			const int st = self->v_state[i];
			jx_led (cr, 4 + pitch * (i + 0.5), y0 + 9, fmin (4, pitch * 0.25), state_color (st), st >= 2 ? 1 : (st == 1 ? 0.35 : 0));
		}
		/* position */
		jx_set (cr, JX_WELL, 1);
		cairo_rectangle (cr, 4, hh - 9, w - 8, 5);
		cairo_fill (cr);
		jx_set_rgb (cr, lit, 0.9);
		cairo_rectangle (cr, 4, hh - 9, (w - 8) * (self->v_free >= 0 ? 1.0 : self->v_pos), 5);
		cairo_fill (cr);
		return jx_display_end (&self->disp, cr);
	}

	/* loop length and position on a VFD */
	double vx = 10, vy = y0 + 4, vw = w - 20, vh = 30;
	jx_vfd_window (cr, vx, vy, vw, vh, lit);
	if (self->v_free >= 0) {
		snprintf (t, sizeof (t), "REC %.2f s", self->v_len);
	} else if (self->v_len > 0) {
		snprintf (t, sizeof (t), "%.2f s", self->v_len);
	} else {
		snprintf (t, sizeof (t), "NO LOOP");
	}
	jx_vfd_text (cr, vx + 8, vy + 21, -1, 16, lit, true, t);
	const double bx = vx + vw * 0.42, bw = vw * 0.55;
	for (int i = 0; i < 32; ++i) {
		const bool on = self->v_len > 0 && i <= (int)(self->v_pos * 32);
		jx_set_rgb (cr, lit, on ? 0.9 : 0.07);
		cairo_rectangle (cr, bx + i * bw / 32, vy + 10, bw / 32 - 1.5, 10);
		cairo_fill (cr);
	}
	jx_vfd_glare (cr, vx, vy, vw, vh);

	if (hh < 150) {
		/* short (above the controls of the plugin window): one LED and state per slot */
		const double pitch = (w - 20) / (double)NSLOT;
		const double ly    = vy + vh + 22;
		for (int i = 0; i < NSLOT; ++i) {
			const int    st = self->v_state[i];
			const double lx = 10 + pitch * i;
			jx_led (cr, lx + 6, ly, 3.5, state_color (st), st >= 2 ? 1 : (st == 1 ? 0.35 : 0));
			snprintf (t, sizeof (t), "%d %s%s", i + 1, sname[st], self->v_rev[i] && st ? " R" : "");
			jx_font (cr, JX_FONT_LABEL, 8);
			jx_set (cr, st ? JX_LEGEND : JX_LEGEND_DIM, 1);
			jx_text (cr, lx + 14, ly + 3, -1, t);
		}
		return jx_display_end (&self->disp, cr);
	}

	jx_section (cr, 8, vy + vh + 8, w - 16, PRODUCT, "SLOTS");
	const double top = vy + vh + 16;
	const double rh  = (hh - top - 6) / (NSLOT / 2);
	const double cw  = (w - 20) / 2;
	for (int i = 0; i < NSLOT; ++i) {
		const int    st = self->v_state[i];
		const double cx = 10 + (i / 4) * (cw + 0);
		const double cy = top + (i % 4) * rh;
		snprintf (t, sizeof (t), "%d", i + 1);
		jx_font (cr, JX_FONT_LABEL, 11);
		jx_set (cr, PRODUCT, 1);
		jx_text (cr, cx + 4, cy + rh * 0.5 + 4, -1, t);
		jx_led (cr, cx + 30, cy + rh * 0.5, 3, state_color (st), st >= 2 ? 1 : (st == 1 ? 0.35 : 0));
		jx_font (cr, JX_FONT_LABEL, 9);
		jx_set (cr, st ? JX_LEGEND : JX_LEGEND_DIM, 1);
		jx_text (cr, cx + 40, cy + rh * 0.5 + 3, -1, sname[st]);
		if (self->v_rev[i] && st) {
			jx_set (cr, JX_LEGEND_DIM, 1);
			jx_text (cr, cx + 80, cy + rh * 0.5 + 3, -1, "REV");
		}
		/* level */
		const double mx = cx + 108, mw = cw - 124;
		if (mw > 10) {
			jx_set (cr, JX_WELL, 1);
			cairo_rectangle (cr, mx, cy + rh * 0.5 - 2, mw, 4);
			cairo_fill (cr);
			const double lv = fmax (0, fmin (1, (jx_g2db (self->v_peak[i]) + 48) / 48));
			jx_set (cr, JX_LED_GREEN, 0.9);
			cairo_rectangle (cr, mx, cy + rh * 0.5 - 2, mw * lv, 4);
			cairo_fill (cr);
		}
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

const LV2_Descriptor jx_loop_descriptor = {
	JX_URI "jx-loop", instantiate, connect_port, NULL, run, NULL, cleanup, extension_data
};
