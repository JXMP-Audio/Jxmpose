/*
 * Copyright (C) 2026 xGlck
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

#include <algorithm>
#include <cmath>

#include "gtkmm2ext/utils.h"

#include "mixer_strip_decor.h"

namespace {

/* small deterministic PRNG (splitmix32 style) */
class Rand
{
public:
	Rand (uint32_t seed) : _s (seed ? seed : 0x9E3779B9u) {}

	/* [0, 1) */
	double operator() ()
	{
		_s += 0x9E3779B9u;
		uint32_t z = _s;
		z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
		z = (z ^ (z >> 13)) * 0xC2B2AE35u;
		z ^= z >> 16;
		return z / 4294967296.0;
	}

	double range (double lo, double hi) { return lo + (hi - lo) * (*this) (); }
	int    irange (int lo, int hi) { return lo + (int) floor ((hi - lo + 1) * (*this) ()); }

private:
	uint32_t _s;
};

void
radial_smudge (cairo_t* cr, double x, double y, double r, double v, double a)
{
	cairo_pattern_t* p = cairo_pattern_create_radial (x, y, 0, x, y, r);
	cairo_pattern_add_color_stop_rgba (p, 0, v, v, v, a);
	cairo_pattern_add_color_stop_rgba (p, 1, v, v, v, 0);
	cairo_set_source (cr, p);
	cairo_arc (cr, x, y, r, 0, 2 * M_PI);
	cairo_fill (cr);
	cairo_pattern_destroy (p);
}

} // namespace

uint32_t
MixerStripDecor::seed_for (std::string const& identity)
{
	uint32_t h = 0x811C9DC5u;
	for (unsigned char c : identity) {
		h ^= c;
		h *= 0x01000193u;
	}
	return h;
}

void
MixerStripDecor::draw_stripe (cairo_t* cr, double x, double y, double w, double h, Gtkmm2ext::Color c)
{
	/* 4 : 1 : 1  (band, gap, line), as on the JXMP panels (2.4 mm, 0.6 mm, 0.6 mm) */
	const double unit = std::max (1.0, floor (h / 6.0));
	Gtkmm2ext::set_source_rgba (cr, c);
	cairo_rectangle (cr, x, y, w, 4 * unit);
	cairo_rectangle (cr, x, y + 5 * unit, w, unit);
	cairo_fill (cr);
}

void
MixerStripDecor::draw_patina (cairo_t* cr, double w, double h, uint32_t seed, double scale)
{
	if (w < 4 || h < 4) {
		return;
	}

	Rand rnd (seed);
	const double s = std::max (1.0, scale);

	cairo_save (cr);
	cairo_rectangle (cr, 0, 0, w, h);
	cairo_clip (cr);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

	/* worn edges: a faint light band along both sides */
	const double edge = 7 * s;
	cairo_pattern_t* p = cairo_pattern_create_linear (0, 0, edge, 0);
	cairo_pattern_add_color_stop_rgba (p, 0, 1, 1, 1, 0.06);
	cairo_pattern_add_color_stop_rgba (p, 1, 1, 1, 1, 0);
	cairo_set_source (cr, p);
	cairo_rectangle (cr, 0, 0, edge, h);
	cairo_fill (cr);
	cairo_pattern_destroy (p);
	p = cairo_pattern_create_linear (w, 0, w - edge, 0);
	cairo_pattern_add_color_stop_rgba (p, 0, 1, 1, 1, 0.04);
	cairo_pattern_add_color_stop_rgba (p, 1, 1, 1, 1, 0);
	cairo_set_source (cr, p);
	cairo_rectangle (cr, w - edge, 0, edge, h);
	cairo_fill (cr);
	cairo_pattern_destroy (p);

	/* grime: dark smudges, more towards the bottom (hands on faders) */
	const int n_grime = rnd.irange (5, 8);
	for (int i = 0; i < n_grime; ++i) {
		const double y = h * (1.0 - pow (rnd (), 2.0) * 0.8);
		radial_smudge (cr, rnd () * w, y, rnd.range (9, 28) * s, 0, rnd.range (0.06, 0.11));
	}

	/* rubs: light smudges */
	for (int i = 0; i < 4; ++i) {
		radial_smudge (cr, rnd () * w, rnd () * h, rnd.range (10, 30) * s, 1, 0.025);
	}

	/* light scratches, density scaled to the strip area */
	const int n_scratch = std::min (70, std::max (8, (int) (w * h / (1600.0 * s * s))));
	for (int i = 0; i < n_scratch; ++i) {
		const double x0  = rnd () * w;
		const double y0  = rnd () * h;
		const double len = rnd.range (2.5, 22) * s;
		const double a   = rnd () * M_PI;
		const double bend = rnd.range (-0.25, 0.25);
		const double x1  = x0 + cos (a) * len;
		const double y1  = y0 + sin (a) * len;
		const double xm  = (x0 + x1) / 2 - sin (a) * len * bend;
		const double ym  = (y0 + y1) / 2 + cos (a) * len * bend;
		cairo_move_to (cr, x0, y0);
		cairo_curve_to (cr, xm, ym, xm, ym, x1, y1);
		cairo_set_line_width (cr, rnd.range (0.4, 0.9) * s);
		cairo_set_source_rgba (cr, 1, 1, 1, rnd.range (0.03, 0.09));
		cairo_stroke (cr);
	}

	/* chipped paint on the side edges: bare aluminium flakes */
	const int n_chips = std::max (3, (int) (h / (90.0 * s)));
	for (int i = 0; i < n_chips; ++i) {
		const bool   left = rnd () < 0.5;
		const double cx   = left ? rnd () * 2.5 * s : w - rnd () * 2.5 * s;
		const double cy   = rnd () * h;
		const double r    = rnd.range (1.0, 2.6) * s;
		const int    nv   = rnd.irange (7, 10);
		for (int k = 0; k < nv; ++k) {
			const double ang = k * 2 * M_PI / nv;
			const double rr  = r * rnd.range (0.6, 1.1);
			/* stretched along the edge */
			const double vx = cx + cos (ang) * rr * 0.6;
			const double vy = cy + sin (ang) * rr * 1.7;
			if (k == 0) {
				cairo_move_to (cr, vx, vy);
			} else {
				cairo_line_to (cr, vx, vy);
			}
		}
		cairo_close_path (cr);
		p = cairo_pattern_create_linear (0, cy - r, 0, cy + r);
		cairo_pattern_add_color_stop_rgb (p, 0, 0xC4 / 255.0, 0xC7 / 255.0, 0xCC / 255.0);
		cairo_pattern_add_color_stop_rgb (p, 1, 0x7E / 255.0, 0x82 / 255.0, 0x89 / 255.0);
		cairo_set_source (cr, p);
		cairo_fill_preserve (cr);
		cairo_pattern_destroy (p);
		cairo_set_source_rgba (cr, 0, 0, 0, 0.45);
		cairo_set_line_width (cr, 0.5 * s);
		cairo_stroke (cr);
	}

	/* deep scratches: a dark groove with a light lip */
	const int n_deep = rnd.irange (1, 3);
	for (int i = 0; i < n_deep; ++i) {
		const double x0  = rnd () * w;
		const double y0  = rnd () * h;
		const double len = rnd.range (3, 12) * s;
		const double a   = rnd () * M_PI;
		const double x1  = x0 + cos (a) * len;
		const double y1  = y0 + sin (a) * len;
		cairo_set_line_width (cr, 1.0 * s);
		cairo_move_to (cr, x0, y0);
		cairo_line_to (cr, x1, y1);
		cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
		cairo_stroke (cr);
		cairo_set_line_width (cr, 0.6 * s);
		cairo_move_to (cr, x0, y0 + 0.7 * s);
		cairo_line_to (cr, x1, y1 + 0.7 * s);
		cairo_set_source_rgba (cr, 1, 1, 1, 0.10);
		cairo_stroke (cr);
	}

	/* paint dots */
	const int n_dots = rnd.irange (3, 7);
	for (int i = 0; i < n_dots; ++i) {
		const double x = rnd () * w;
		const double y = rnd () * h;
		const double r = rnd.range (0.5, 1.2) * s;
		cairo_save (cr);
		cairo_translate (cr, x, y);
		cairo_scale (cr, 1.0, rnd.range (0.6, 1.0));
		cairo_arc (cr, 0, 0, r, 0, 2 * M_PI);
		cairo_restore (cr);
		cairo_set_source_rgba (cr, 0xA8 / 255.0, 0xAC / 255.0, 0xB2 / 255.0, 0.55);
		cairo_fill_preserve (cr);
		cairo_set_source_rgba (cr, 0, 0, 0, 0.3);
		cairo_set_line_width (cr, 0.4 * s);
		cairo_stroke (cr);
	}

	cairo_restore (cr);
}
