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

/* JXMP look for the inline displays of the JX plugins (see DesignGuidelines:
 * anthracite panel, double stripe in the product colour, section lines with
 * the title cut in, VFD windows, CRT screens, LED bars). The same display is
 * drawn small in the mixer strip and up to 300 px tall in the generic
 * plugin window. */

#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cairo/cairo.h>

#include "ardour/lv2_extensions.h"

#define JX_PANEL      0x2A2B2F
#define JX_PANEL_DARK 0x1E1F22
#define JX_LINE       0x5C5F66
#define JX_LEGEND     0xE6E7EA
#define JX_LEGEND_DIM 0x9A9DA4
#define JX_WELL       0x050506
#define JX_BEZEL      0x0E0E10
#define JX_LED_GREEN  0x3CD06E
#define JX_LED_YELLOW 0xF0C030
#define JX_LED_RED    0xE83A30
#define JX_TRACE_CH1  0x7CFFA8
#define JX_TRACE_CH2  0xFFBE55

/* a display is "full" (generic plugin window) from this height on, else compact (mixer strip) */
#define JX_FULL_H 120

typedef struct {
	double r, g, b;
} JxRGB;

static inline JxRGB
jx_hex (uint32_t c)
{
	JxRGB rgb = { ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0 };
	return rgb;
}

static inline void
jx_set (cairo_t* cr, uint32_t c, double a)
{
	JxRGB k = jx_hex (c);
	cairo_set_source_rgba (cr, k.r, k.g, k.b, a);
}

static inline void
jx_set_rgb (cairo_t* cr, JxRGB k, double a)
{
	cairo_set_source_rgba (cr, k.r, k.g, k.b, a);
}

/* VFD glow of a product colour: largest channel to 1.0, then ch * 0.8 + 0.2 */
static inline JxRGB
jx_vfd_tint (uint32_t c)
{
	JxRGB  k = jx_hex (c);
	double m = fmax (k.r, fmax (k.g, k.b));
	if (m <= 0) {
		m = 1;
	}
	JxRGB t = { k.r / m * 0.8 + 0.2, k.g / m * 0.8 + 0.2, k.b / m * 0.8 + 0.2 };
	return t;
}

static inline void
jx_rrect (cairo_t* cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path (cr);
	cairo_arc (cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc (cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc (cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc (cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_close_path (cr);
}

enum { JX_FONT_TITLE, JX_FONT_LABEL, JX_FONT_NUMBER, JX_FONT_DENSE };

static inline void
jx_font (cairo_t* cr, int which, double size)
{
	switch (which) {
		case JX_FONT_TITLE:
			cairo_select_font_face (cr, "Barlow", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
			break;
		case JX_FONT_DENSE:
			cairo_select_font_face (cr, "Barlow Condensed", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
			break;
		case JX_FONT_NUMBER:
			cairo_select_font_face (cr, "Barlow", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
			break;
		default:
			cairo_select_font_face (cr, "Barlow", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
			break;
	}
	cairo_set_font_size (cr, size);
}

/* text at (x, baseline y); align -1 left, 0 centre, 1 right. Returns the width */
static inline double
jx_text (cairo_t* cr, double x, double y, int align, const char* txt)
{
	cairo_text_extents_t te;
	cairo_text_extents (cr, txt, &te);
	double tx = x;
	if (align == 0) {
		tx = x - te.x_advance * 0.5;
	} else if (align > 0) {
		tx = x - te.x_advance;
	}
	cairo_move_to (cr, tx, y);
	cairo_show_text (cr, txt);
	cairo_new_path (cr);
	return te.x_advance;
}

/* The panel: anthracite plate; in the full view a dark top band with the
 * title and the double stripe in the product colour, compact: the stripe
 * only. Returns the y where content starts. */
static inline double
jx_panel (cairo_t* cr, double w, double h, uint32_t product, const char* title, const char* subtitle)
{
	jx_set (cr, JX_PANEL, 1);
	cairo_rectangle (cr, 0, 0, w, h);
	cairo_fill (cr);

	if (h < JX_FULL_H) {
		jx_set (cr, product, 1);
		cairo_rectangle (cr, 0, 0, w, 2);
		cairo_fill (cr);
		cairo_rectangle (cr, 0, 3, w, 1);
		cairo_fill (cr);
		return 6;
	}

	jx_set (cr, JX_PANEL_DARK, 1);
	cairo_rectangle (cr, 0, 0, w, 22);
	cairo_fill (cr);

	/* screws */
	for (int i = 0; i < 2; ++i) {
		double sx = i ? w - 9 : 9;
		cairo_arc (cr, sx, 11, 3.2, 0, 2 * M_PI);
		jx_set (cr, 0x0C0C0E, 1);
		cairo_fill_preserve (cr);
		jx_set (cr, 0x3A3D44, 1);
		cairo_set_line_width (cr, 0.6);
		cairo_stroke (cr);
		cairo_move_to (cr, sx - 2, 11);
		cairo_line_to (cr, sx + 2, 11);
		cairo_stroke (cr);
	}

	jx_font (cr, JX_FONT_TITLE, 15);
	jx_set (cr, JX_LEGEND, 1);
	double tw = jx_text (cr, w * 0.5, 16, 0, title);
	if (subtitle && w > 220) {
		jx_font (cr, JX_FONT_LABEL, 8.5);
		jx_set (cr, JX_LEGEND_DIM, 1);
		jx_text (cr, w * 0.5 + tw * 0.5 + 8, 16, -1, subtitle);
	}

	jx_set (cr, product, 1);
	cairo_rectangle (cr, 0, 22, w, 3);
	cairo_fill (cr);
	cairo_rectangle (cr, 0, 26, w, 1);
	cairo_fill (cr);
	return 32;
}

/* A section: one horizontal line with the title cut in, title in the product colour */
static inline void
jx_section (cairo_t* cr, double x, double y, double w, uint32_t product, const char* title)
{
	jx_font (cr, JX_FONT_LABEL, 8.5);
	cairo_text_extents_t te;
	cairo_text_extents (cr, title, &te);
	double tx = x + 6;
	jx_set (cr, JX_LINE, 1);
	cairo_set_line_width (cr, 0.9);
	cairo_move_to (cr, x, y + 0.5);
	cairo_line_to (cr, tx - 3, y + 0.5);
	cairo_move_to (cr, tx + te.x_advance + 3, y + 0.5);
	cairo_line_to (cr, x + w, y + 0.5);
	cairo_stroke (cr);
	jx_set (cr, product, 1);
	jx_text (cr, tx, y + 3.5, -1, title);
}

/* VFD window: bezel set into the panel, tinted glass, grid mesh. Content is
 * drawn by the caller, then jx_vfd_glare () */
static inline void
jx_vfd_window (cairo_t* cr, double x, double y, double w, double h, JxRGB lit)
{
	double bz = h < 30 ? fmax (1.5, h * 0.12) : 3;
	cairo_pattern_t* p = cairo_pattern_create_linear (0, y, 0, y + h);
	cairo_pattern_add_color_stop_rgb (p, 0, 0.02, 0.02, 0.024);
	cairo_pattern_add_color_stop_rgb (p, 1, 0.14, 0.145, 0.16);
	jx_rrect (cr, x, y, w, h, 2.5);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);

	double gx = x + bz, gy = y + bz, gw = w - 2 * bz, gh = h - 2 * bz;
	p = cairo_pattern_create_linear (0, gy, 0, gy + gh);
	cairo_pattern_add_color_stop_rgb (p, 0, 0.02 + lit.r * 0.045, 0.02 + lit.g * 0.045, 0.02 + lit.b * 0.045);
	cairo_pattern_add_color_stop_rgb (p, 1, 0.02 + lit.r * 0.08, 0.02 + lit.g * 0.08, 0.02 + lit.b * 0.08);
	jx_rrect (cr, gx, gy, gw, gh, 1.5);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);

	if (gh > 16) {
		cairo_save (cr);
		cairo_rectangle (cr, gx, gy, gw, gh);
		cairo_clip (cr);
		jx_set_rgb (cr, lit, 0.035);
		cairo_set_line_width (cr, 0.4);
		for (double mx = gx; mx < gx + gw; mx += 1.6) {
			cairo_move_to (cr, mx, gy);
			cairo_line_to (cr, mx, gy + gh);
		}
		for (double my = gy; my < gy + gh; my += 1.6) {
			cairo_move_to (cr, gx, my);
			cairo_line_to (cr, gx + gw, my);
		}
		cairo_stroke (cr);
		/* filament wires */
		cairo_set_source_rgba (cr, 0, 0, 0, 0.45);
		for (int i = 1; i <= 3; ++i) {
			double fy = gy + gh * i / 4.0;
			cairo_move_to (cr, gx, fy);
			cairo_line_to (cr, gx + gw, fy + 0.6);
		}
		cairo_stroke (cr);
		cairo_restore (cr);
	}
}

static inline void
jx_vfd_glare (cairo_t* cr, double x, double y, double w, double h)
{
	cairo_pattern_t* p = cairo_pattern_create_linear (0, y, 0, y + h * 0.42);
	cairo_pattern_add_color_stop_rgba (p, 0, 1, 1, 1, 0.07);
	cairo_pattern_add_color_stop_rgba (p, 1, 1, 1, 1, 0);
	cairo_rectangle (cr, x + 2, y + 2, w - 4, h * 0.42);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);
}

/* VFD text: a soft glow under the sharp glyphs. on = false draws the 0.09 ghost of an annunciator */
static inline double
jx_vfd_text (cairo_t* cr, double x, double y, int align, double size, JxRGB lit, bool on, const char* txt)
{
	jx_font (cr, JX_FONT_LABEL, size);
	cairo_text_extents_t te;
	cairo_text_extents (cr, txt, &te);
	double tx = x;
	if (align == 0) {
		tx = x - te.x_advance * 0.5;
	} else if (align > 0) {
		tx = x - te.x_advance;
	}
	if (!on) {
		jx_set_rgb (cr, lit, 0.09);
		cairo_move_to (cr, tx, y);
		cairo_show_text (cr, txt);
		cairo_new_path (cr);
		return te.x_advance;
	}
	cairo_move_to (cr, tx, y);
	cairo_text_path (cr, txt);
	jx_set_rgb (cr, lit, 0.25);
	cairo_set_line_width (cr, fmax (1, size * 0.18));
	cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke_preserve (cr);
	jx_set_rgb (cr, lit, 1);
	cairo_fill (cr);
	return te.x_advance;
}

/* CRT screen (rectangular tube face) with a thin bezel and a division grid */
static inline void
jx_crt (cairo_t* cr, double x, double y, double w, double h, int xdiv, int ydiv)
{
	jx_set (cr, JX_BEZEL, 1);
	jx_rrect (cr, x, y, w, h, 4);
	cairo_fill (cr);
	cairo_pattern_t* p = cairo_pattern_create_radial (x + w * 0.5, y + h * 0.5, 0, x + w * 0.5, y + h * 0.5, fmax (w, h) * 0.7);
	cairo_pattern_add_color_stop_rgb (p, 0, 0.035, 0.07, 0.055);
	cairo_pattern_add_color_stop_rgb (p, 1, 0.01, 0.02, 0.018);
	jx_rrect (cr, x + 2, y + 2, w - 4, h - 4, 3);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);

	cairo_set_line_width (cr, 0.5);
	cairo_set_source_rgba (cr, 0.49, 1.0, 0.66, 0.08);
	for (int i = 1; i < xdiv; ++i) {
		double gx = rint (x + 2 + (w - 4) * i / (double)xdiv) + 0.5;
		cairo_move_to (cr, gx, y + 2);
		cairo_line_to (cr, gx, y + h - 2);
	}
	for (int i = 1; i < ydiv; ++i) {
		double gy = rint (y + 2 + (h - 4) * i / (double)ydiv) + 0.5;
		cairo_move_to (cr, x + 2, gy);
		cairo_line_to (cr, x + w - 2, gy);
	}
	cairo_stroke (cr);
}

/* phosphor trace: the current path drawn as a wide soft glow and a bright core */
static inline void
jx_trace (cairo_t* cr, uint32_t color, bool compact)
{
	cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	jx_set (cr, color, 0.18);
	cairo_set_line_width (cr, compact ? 3 : 4.5);
	cairo_stroke_preserve (cr);
	jx_set (cr, color, 0.95);
	cairo_set_line_width (cr, compact ? 1 : 1.4);
	cairo_stroke (cr);
}

/* one LED: on = brightness 0..1 */
static inline void
jx_led (cairo_t* cr, double cx, double cy, double r, uint32_t color, double on)
{
	JxRGB k = jx_hex (color);
	if (on > 0.05) {
		cairo_pattern_t* p = cairo_pattern_create_radial (cx, cy, 0, cx, cy, r * 2.6);
		cairo_pattern_add_color_stop_rgba (p, 0, k.r, k.g, k.b, 0.35 * on);
		cairo_pattern_add_color_stop_rgba (p, 1, k.r, k.g, k.b, 0);
		cairo_arc (cr, cx, cy, r * 2.6, 0, 2 * M_PI);
		cairo_set_source (cr, p);
		cairo_fill (cr);
		cairo_pattern_destroy (p);
	}
	double f = 0.18 + 0.82 * on;
	cairo_arc (cr, cx, cy, r, 0, 2 * M_PI);
	cairo_set_source_rgb (cr, k.r * f, k.g * f, k.b * f);
	cairo_fill (cr);
}

/* vertical LED bar, 3 dB per LED from -45 to 0 dBFS (green, yellow from -6, only the top LED red) */
static inline void
jx_led_bar (cairo_t* cr, double x, double y, double w, double h, float db)
{
	const int n   = 16;
	double    gap = h > 80 ? 1.5 : 1;
	double    lh  = (h - gap * (n - 1)) / n;
	jx_set (cr, JX_BEZEL, 1);
	cairo_rectangle (cr, x - 1, y - 1, w + 2, h + 2);
	cairo_fill (cr);
	for (int i = 0; i < n; ++i) {
		double   thr = -45 + 3 * i;
		uint32_t c   = i == n - 1 ? JX_LED_RED : (thr >= -6 ? JX_LED_YELLOW : JX_LED_GREEN);
		JxRGB    k   = jx_hex (c);
		double   f   = db > thr ? 1.0 : 0.18;
		cairo_set_source_rgb (cr, k.r * f, k.g * f, k.b * f);
		cairo_rectangle (cr, x, y + h - (i + 1) * lh - i * gap, w, lh);
		cairo_fill (cr);
	}
}

/* small rubber-button-like status tag (lit or off) with a legend */
static inline void
jx_tag (cairo_t* cr, double x, double y, double w, double h, uint32_t lit, bool on, const char* txt)
{
	cairo_set_source_rgba (cr, 0, 0, 0, 0.5);
	jx_rrect (cr, x + 0.8, y + 1.6, w, h, 3);
	cairo_fill (cr);
	cairo_pattern_t* p = cairo_pattern_create_linear (0, y, 0, y + h);
	if (on) {
		JxRGB k = jx_hex (lit);
		cairo_pattern_add_color_stop_rgb (p, 0, fmin (1, k.r + 0.12), fmin (1, k.g + 0.12), fmin (1, k.b + 0.12));
		cairo_pattern_add_color_stop_rgb (p, 1, k.r * 0.7, k.g * 0.7, k.b * 0.7);
	} else {
		cairo_pattern_add_color_stop_rgb (p, 0, 0.235, 0.243, 0.263);
		cairo_pattern_add_color_stop_rgb (p, 1, 0.118, 0.122, 0.133);
	}
	jx_rrect (cr, x, y, w, h, 3);
	cairo_set_source (cr, p);
	cairo_fill_preserve (cr);
	cairo_pattern_destroy (p);
	jx_set (cr, 0x0A0A0B, 1);
	cairo_set_line_width (cr, 1);
	cairo_stroke (cr);
	jx_font (cr, JX_FONT_LABEL, fmin (9, 0.55 * h));
	jx_set (cr, on ? 0x1A1210 : JX_LEGEND, 1);
	jx_text (cr, x + w * 0.5, y + h * 0.5 + fmin (9, 0.55 * h) * 0.36, 0, txt);
}

/* annunciator: an LED and its legend (an indicator, not a control). y is the baseline, returns the width taken */
static inline double
jx_annunciator (cairo_t* cr, double x, double y, uint32_t color, bool on, const char* txt)
{
	jx_led (cr, x + 3, y - 3, 2.6, color, on ? 1 : 0);
	jx_font (cr, JX_FONT_LABEL, 8);
	jx_set (cr, on ? JX_LEGEND : JX_LEGEND_DIM, 1);
	return 9 + jx_text (cr, x + 9, y, -1, txt) + 12;
}

/* frequency (Hz) to x on a log axis 20 Hz .. 20 kHz */
static inline double
jx_freq_x (double f, double x, double w)
{
	if (f < 20) {
		f = 20;
	}
	return x + w * log10 (f / 20.0) / 3.0;
}

/* print the frequency scale under a log axis */
static inline void
jx_freq_scale (cairo_t* cr, double x, double y, double w)
{
	static const double      f[]  = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
	static const char* const t[] = { "50", "100", "200", "500", "1K", "2K", "5K", "10K" };
	jx_font (cr, JX_FONT_NUMBER, 7);
	jx_set (cr, JX_LEGEND_DIM, 1);
	for (int i = 0; i < 8; ++i) {
		if (w < 200 && (i & 1)) {
			continue;
		}
		jx_text (cr, jx_freq_x (f[i], x, w), y, 0, t[i]);
	}
}

static inline const char*
jx_note_name (int note)
{
	static const char* const n[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	return n[((note % 12) + 12) % 12];
}

/* the cairo surface handed to the host */
typedef struct {
	cairo_surface_t*                 s;
	LV2_Inline_Display_Image_Surface surf;
	uint32_t                         w, h;
} JxDisplay;

static inline cairo_t*
jx_display_begin (JxDisplay* d, uint32_t w, uint32_t h)
{
	if (!d->s || d->w != w || d->h != h) {
		if (d->s) {
			cairo_surface_destroy (d->s);
		}
		d->s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
		d->w = w;
		d->h = h;
	}
	cairo_t* cr = cairo_create (d->s);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
	return cr;
}

static inline LV2_Inline_Display_Image_Surface*
jx_display_end (JxDisplay* d, cairo_t* cr)
{
	cairo_destroy (cr);
	cairo_surface_flush (d->s);
	d->surf.width  = cairo_image_surface_get_width (d->s);
	d->surf.height = cairo_image_surface_get_height (d->s);
	d->surf.stride = cairo_image_surface_get_stride (d->s);
	d->surf.data   = cairo_image_surface_get_data (d->s);
	return &d->surf;
}

static inline void
jx_display_free (JxDisplay* d)
{
	if (d->s) {
		cairo_surface_destroy (d->s);
		d->s = NULL;
	}
}

/* display height for a width: compact in the strip, up to max_h in the plugin window */
static inline uint32_t
jx_display_height (uint32_t w, uint32_t max_h, uint32_t compact_h, double full_aspect)
{
	if (max_h >= JX_FULL_H && w >= 240) {
		uint32_t h = (uint32_t)(w * full_aspect);
		if (h > max_h) {
			h = max_h;
		}
		if (h < JX_FULL_H) {
			h = JX_FULL_H;
		}
		return h;
	}
	return compact_h < max_h ? compact_h : max_h;
}
