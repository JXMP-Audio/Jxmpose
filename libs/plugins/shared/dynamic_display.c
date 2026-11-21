/*
 * Copyright (C) 2018 Johannes Mueller <github@johannes-mueller.org>
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


/* Shared code for a-comp.c and a-exp.c
 *
 * The inline display of the ACE dynamics, drawn after the JXMP design
 * guidelines (the same look as the JX plugins, see jxmp_draw.h):
 *
 *  - compact (mixer strip): the product colour stripe and a VFD with the
 *    input level, the threshold mark and the gain reduction;
 *  - full (plugin window): title band and double stripe, a CRT with the
 *    transfer curve (in -60..+10 dB -> out) and the operating point, LED
 *    bars for the input and output level and a VFD gain reduction readout.
 *
 * It is not meant to be compiled as a individual compilation unit but to be
 * included like
 *
 * #include "dynamic_display.c"
 *
 */

#include "../jxmp.lv2/jxmp_draw.h"

typedef float (*JxDynCurve) (const void* self, float xg);

typedef struct {
	uint32_t    product;
	const char* title;
	const char* subtitle;
	float       thresdb;
	float       gainr;    /* dB, positive */
	float       gr_range; /* dB of gain reduction over the full meter */
	float       lvl_in;   /* dB */
	float       lvl_out;  /* dB */
} JxDynState;

/* a level -60..+10 dB to 0..1 */
static inline double
jx_dyn_pos (float db)
{
	return fmin (1.0, fmax (0.0, (db + 60.f) / 70.f));
}

/* display height for a width: compact in the strip, up to max_h in the plugin window */
static inline uint32_t
jx_dyn_height (uint32_t w, uint32_t max_h)
{
	return jx_display_height (w, max_h, 40, 0.5);
}

static void
jx_dyn_compact (cairo_t* cr, double w, double h, double y0, JxDynState const* s)
{
	const JxRGB  lit = jx_vfd_tint (s->product);
	const double vx = 3, vy = y0, vw = w - 6, vh = h - y0 - 3;
	jx_vfd_window (cr, vx, vy, vw, vh, lit);

	const double lx  = vx + 17, lw = vw - 21;
	const double bh  = fmax (3, floor ((vh - 8) * 0.5));
	const double yin = vy + 3, ygr = vy + vh - 3 - bh;
	jx_vfd_text (cr, vx + 4, yin + bh - 1, -1, 7, lit, true, "IN");
	jx_vfd_text (cr, vx + 4, ygr + bh - 1, -1, 7, lit, s->gainr >= 0.1f, "GR");

	/* unlit segments */
	jx_set_rgb (cr, lit, 0.08);
	cairo_rectangle (cr, lx, yin, lw, bh);
	cairo_rectangle (cr, lx, ygr, lw, bh);
	cairo_fill (cr);

	/* input level, segmented */
	const double xin = lx + lw * jx_dyn_pos (s->lvl_in);
	jx_set_rgb (cr, lit, 0.85);
	for (double sx = lx; sx + 2 <= xin; sx += 3) {
		cairo_rectangle (cr, sx, yin, 2, bh);
	}
	cairo_fill (cr);

	/* threshold mark */
	const double xt = rint (lx + lw * jx_dyn_pos (s->thresdb)) + 0.5;
	jx_set (cr, JX_TRACE_CH2, 0.95);
	cairo_set_line_width (cr, 1.5);
	cairo_move_to (cr, xt, yin - 1);
	cairo_line_to (cr, xt, yin + bh + 1);
	cairo_stroke (cr);

	/* gain reduction from the right */
	const double gw = lw * fmin (1.0, fmax (0.0, s->gainr / s->gr_range));
	jx_set_rgb (cr, lit, 0.95);
	for (double sx = lx + lw - 2; sx >= lx + lw - gw; sx -= 3) {
		cairo_rectangle (cr, sx, ygr, 2, bh);
	}
	cairo_fill (cr);

	jx_vfd_glare (cr, vx, vy, vw, vh);
}

static void
jx_dyn_render (cairo_t* cr, double w, double h, JxDynState const* s, const void* self, JxDynCurve curve)
{
	const double y0 = jx_panel (cr, w, h, s->product, s->title, s->subtitle);

	if (h < JX_FULL_H) {
		jx_dyn_compact (cr, w, h, y0, s);
		return;
	}

	const JxRGB lit = jx_vfd_tint (s->product);
	char        t[32];

	/* ---- transfer curve on a CRT, square, on the left ---- */
	const double cs = fmin (h - y0 - 20, w * 0.55);
	const double cx = 10, cy = y0 + 12;
	jx_section (cr, 8, y0 + 4, cs + 26, s->product, "TRANSFER");
	jx_crt (cr, cx, cy, cs, cs, 7, 7);

	const double px = cx + 3, py = cy + 3, ps = cs - 6;
#define DYN_X(db) (px + ps * jx_dyn_pos (db))
#define DYN_Y(db) (py + ps * (1.0 - jx_dyn_pos (db)))

	cairo_save (cr);
	cairo_rectangle (cr, px, py, ps, ps);
	cairo_clip (cr);

	/* 0 dB in and out, unity */
	cairo_set_line_width (cr, 0.6);
	cairo_set_source_rgba (cr, 0.49, 1.0, 0.66, 0.16);
	cairo_move_to (cr, rint (DYN_X (0)) + 0.5, py);
	cairo_line_to (cr, rint (DYN_X (0)) + 0.5, py + ps);
	cairo_move_to (cr, px, rint (DYN_Y (0)) + 0.5);
	cairo_line_to (cr, px + ps, rint (DYN_Y (0)) + 0.5);
	cairo_stroke (cr);
	const double dash[] = { 1, 2 };
	cairo_set_dash (cr, dash, 2, 0);
	cairo_move_to (cr, DYN_X (-60), DYN_Y (-60));
	cairo_line_to (cr, DYN_X (10), DYN_Y (10));
	cairo_stroke (cr);

	/* threshold */
	jx_set (cr, JX_TRACE_CH2, 0.35);
	cairo_move_to (cr, rint (DYN_X (s->thresdb)) + 0.5, py);
	cairo_line_to (cr, rint (DYN_X (s->thresdb)) + 0.5, py + ps);
	cairo_stroke (cr);
	cairo_set_dash (cr, NULL, 0, 0);

	/* the curve */
	const int n = (int)ps;
	for (int i = 0; i <= n; ++i) {
		const float x_db = -60.f + 70.f * i / (float)n;
		const float y_db = curve (self, x_db);
		if (i == 0) {
			cairo_move_to (cr, DYN_X (x_db), DYN_Y (y_db));
		} else {
			cairo_line_to (cr, DYN_X (x_db), DYN_Y (y_db));
		}
	}
	jx_trace (cr, JX_TRACE_CH1, false);

	/* operating point: the input level on the curve */
	if (s->lvl_in > -60.f) {
		const double ox = DYN_X (s->lvl_in), oy = DYN_Y (curve (self, s->lvl_in));
		jx_led (cr, ox, oy, 2.8, JX_TRACE_CH2, 1);
	}
	cairo_restore (cr);

	/* dB print right of the screen */
	jx_font (cr, JX_FONT_NUMBER, 7);
	jx_set (cr, JX_LEGEND_DIM, 1);
	static const int   marks[]  = { 10, 0, -20, -40, -60 };
	static const char* mtext[]  = { "+10", "0", "−20", "−40", "−60" };
	for (int i = 0; i < 5; ++i) {
		jx_text (cr, cx + cs + 4, DYN_Y (marks[i]) + 2.5, -1, mtext[i]);
	}
#undef DYN_X
#undef DYN_Y

	/* ---- levels and gain reduction on the right ---- */
	const double rx = cx + cs + 34, rw = w - 10 - rx;
	if (rw < 60) {
		return;
	}
	jx_section (cr, rx - 6, y0 + 4, rw + 4, s->product, "LEVELS");

	const double by = cy + 2, bh = cs - 16;
	jx_led_bar (cr, rx, by, 8, bh, s->lvl_in);
	jx_led_bar (cr, rx + 18, by, 8, bh, s->lvl_out);
	jx_font (cr, JX_FONT_LABEL, 7);
	jx_set (cr, JX_LEGEND, 1);
	jx_text (cr, rx + 4, cy + cs - 2, 0, "IN");
	jx_text (cr, rx + 22, cy + cs - 2, 0, "OUT");

	/* the LED bar's own dBFS scale */
	jx_font (cr, JX_FONT_NUMBER, 6);
	jx_set (cr, JX_LEGEND_DIM, 1);
	static const int   lm[] = { 0, -6, -12, -18, -30, -45 };
	static const char* lt[] = { "0", "−6", "−12", "−18", "−30", "−45" };
	for (int i = 0; i < 6; ++i) {
		const double ly = by + bh * (-lm[i] / 45.0);
		jx_text (cr, rx + 30, ly + 2 + (i == 0 ? 2 : 0) - (i == 5 ? 1 : 0), -1, lt[i]);
	}

	/* gain reduction: VFD with the value and a bar hanging from the top */
	const double gx = rx + 52, gw = rw - 52;
	if (gw < 44) {
		return;
	}
	jx_vfd_window (cr, gx, by - 2, gw, bh + 4, lit);
	const bool on = s->gainr >= 0.05f;
	jx_vfd_text (cr, gx + gw * 0.5, by + 12, 0, 8, lit, true, "GAIN REDUCTION");
	if (on) {
		snprintf (t, sizeof (t), "−%.1f", s->gainr);
	} else {
		snprintf (t, sizeof (t), "0.0");
	}
	const double vs = fmin (26, gw * 0.24);
	const double tw = jx_vfd_text (cr, gx + gw * 0.5 - 6, by + bh * 0.5 + vs * 0.36, 0, vs, lit, true, t);
	jx_vfd_text (cr, gx + gw * 0.5 - 6 + tw * 0.5 + 3, by + bh * 0.5 + vs * 0.36, -1, 9, lit, true, "dB");

	/* bar 0 .. gr_range */
	const double mx = gx + 8, mw = gw - 16, my = by + bh - 12;
	jx_set_rgb (cr, lit, 0.08);
	cairo_rectangle (cr, mx, my, mw, 6);
	cairo_fill (cr);
	const double gl = mw * fmin (1.0, fmax (0.0, s->gainr / s->gr_range));
	jx_set_rgb (cr, lit, 0.95);
	for (double sx = mx + mw - 2; sx >= mx + mw - gl; sx -= 3) {
		cairo_rectangle (cr, sx, my, 2, 6);
	}
	cairo_fill (cr);
	/* the bar's scale, printed on the glass */
	snprintf (t, sizeof (t), "−%.0f", s->gr_range);
	jx_font (cr, JX_FONT_NUMBER, 7);
	jx_set_rgb (cr, lit, 0.45);
	jx_text (cr, mx, my - 3, -1, t);
	jx_text (cr, mx + mw, my - 3, 1, "0");
	jx_vfd_glare (cr, gx, by - 2, gw, bh + 4);
}
