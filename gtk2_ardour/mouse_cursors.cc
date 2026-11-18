/*
 * Copyright (C) 2010-2012 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2012-2015 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2014-2017 Robin Gareus <robin@gareus.org>
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

#include <ydkmm/cursor.h>

#include "gtkmm2ext/cursors.h"

#include <cairo.h>
#include <cmath>

#include "utils.h"
#include "ui_config.h"
#include "mouse_cursors.h"
#include "editor_xpms"

using namespace ARDOUR_UI_UTILS;

Gdk::Cursor* MouseCursors::_invalid = 0;

MouseCursors::MouseCursors ()
	: cross_hair (0)
	, scissors (0)
	, trimmer (0)
	, right_side_trim (0)
	, anchored_right_side_trim (0)
	, left_side_trim (0)
	, anchored_left_side_trim (0)
	, right_side_trim_left_only (0)
	, left_side_trim_right_only (0)
	, fade_in (0)
	, fade_out (0)
	, selector (0)
	, grabber (0)
	, grabber_note (0)
	, zoom_in (0)
	, zoom_out (0)
	, time_fx (0)
	, fader (0)
	, midi_pencil (0)
	, midi_select (0)
	, midi_resize (0)
	, midi_erase (0)
	, up_down (0)
	, wait (0)
	, timebar (0)
	, transparent (0)
	, resize_left (0)
	, resize_top_left (0)
	, resize_top (0)
	, resize_top_right (0)
	, resize_right (0)
	, resize_bottom_right (0)
	, resize_bottom (0)
	, resize_bottom_left (0)
	, move (0)
	, expand_left_right (0)
	, expand_up_down (0)
{
}

MouseCursors::~MouseCursors ()
{
	drop_all ();
}

void
MouseCursors::drop_all ()
{
	cursors.clear ();

	delete cross_hair; cross_hair = 0;
	delete scissors; scissors = 0;
	delete trimmer; trimmer = 0;
	delete right_side_trim; right_side_trim = 0;
	delete anchored_right_side_trim; anchored_right_side_trim = 0;
	delete left_side_trim; left_side_trim = 0;
	delete anchored_left_side_trim; anchored_left_side_trim = 0;
	delete right_side_trim_left_only; right_side_trim_left_only = 0;
	delete left_side_trim_right_only; left_side_trim_right_only = 0;
	delete fade_in; fade_in = 0;
	delete fade_out; fade_out = 0;
	delete selector; selector = 0;
	delete grabber; grabber = 0;
	delete grabber_note; grabber_note = 0;
	delete zoom_in; zoom_in = 0;
	delete zoom_out; zoom_out = 0;
	delete time_fx; time_fx = 0;
	delete fader; fader = 0;
	delete midi_pencil; midi_pencil = 0;
	delete midi_select; midi_select = 0;
	delete midi_resize; midi_resize = 0;
	delete midi_erase; midi_erase = 0;
	delete up_down; up_down = 0;
	delete wait; wait = 0;
	delete timebar; timebar = 0;
	delete transparent; transparent = 0;
	delete resize_left; resize_left = 0;
	delete resize_top_left; resize_top_left = 0;
	delete resize_top; resize_top = 0;
	delete resize_top_right; resize_top_right = 0;
	delete resize_right; resize_right = 0;
	delete resize_bottom_right; resize_bottom_right = 0;
	delete resize_bottom; resize_bottom = 0;
	delete resize_bottom_left; resize_bottom_left = 0;
	delete move; move = 0;
	delete expand_left_right; expand_left_right = 0;
	delete expand_up_down; expand_up_down = 0;
}

/* Pencil with the tip at the top-left hot-spot and the body running down to
 * the bottom-right (the X11 stock pencil points the other way). */
Gdk::Cursor*
MouseCursors::make_pencil_cursor ()
{
	float const scale = std::max (1.f, UIConfiguration::instance ().get_ui_scale ());
	int const   sz    = (int) rint (24 * scale);

	cairo_surface_t* surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sz, sz);
	cairo_t*         cr   = cairo_create (surf);
	cairo_scale (cr, sz / 24.0, sz / 24.0);

	/* local frame: x along the pencil from the tip (0,0), y across it */
	cairo_translate (cr, 1, 1);
	cairo_rotate (cr, M_PI / 4.0);

	double const len = 28.0; // tip .. end (diagonal of the 24px box)
	double const hw  = 3.0;  // half width
	double const cone = 7.0;

	cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width (cr, 1.0);

	/* body */
	cairo_rectangle (cr, cone, -hw, len - cone - 4.5, 2 * hw);
	cairo_set_source_rgb (cr, 0.95, 0.72, 0.20);
	cairo_fill_preserve (cr);
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_stroke (cr);
	/* eraser */
	cairo_rectangle (cr, len - 4.5, -hw, 4.0, 2 * hw);
	cairo_set_source_rgb (cr, 0.90, 0.50, 0.55);
	cairo_fill_preserve (cr);
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_stroke (cr);
	/* wooden cone */
	cairo_move_to (cr, 0, 0);
	cairo_line_to (cr, cone, -hw);
	cairo_line_to (cr, cone, hw);
	cairo_close_path (cr);
	cairo_set_source_rgb (cr, 0.96, 0.87, 0.70);
	cairo_fill_preserve (cr);
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_stroke (cr);
	/* graphite tip */
	cairo_move_to (cr, 0, 0);
	cairo_line_to (cr, 2.6, -1.1);
	cairo_line_to (cr, 2.6, 1.1);
	cairo_close_path (cr);
	cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);
	cairo_fill (cr);

	cairo_destroy (cr);
	cairo_surface_flush (surf);

	/* premultiplied ARGB32 -> RGBA pixbuf */
	Glib::RefPtr<Gdk::Pixbuf> pb = Gdk::Pixbuf::create (Gdk::COLORSPACE_RGB, true, 8, sz, sz);
	unsigned char const* src    = cairo_image_surface_get_data (surf);
	int const            stride = cairo_image_surface_get_stride (surf);
	for (int y = 0; y < sz; ++y) {
		uint32_t const* row = (uint32_t const*) (src + y * stride);
		guint8*         dst = pb->get_pixels () + y * pb->get_rowstride ();
		for (int x = 0; x < sz; ++x) {
			uint32_t const px = row[x];
			unsigned const a  = px >> 24;
			unsigned r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
			if (a > 0 && a < 255) {
				r = r * 255 / a;
				g = g * 255 / a;
				b = b * 255 / a;
			}
			dst[4 * x + 0] = r;
			dst[4 * x + 1] = g;
			dst[4 * x + 2] = b;
			dst[4 * x + 3] = a;
		}
	}
	cairo_surface_destroy (surf);

	int const hot = (int) rint (1 * sz / 24.0);
	return new Gdk::Cursor (Gdk::Display::get_default (), pb, hot, hot);
}

Gdk::Cursor*
MouseCursors::make_cursor (const char* name, int hotspot_x, int hotspot_y)
{
	Gtkmm2ext::CursorInfo* info = Gtkmm2ext::CursorInfo::lookup_cursor_info (name);

	if (info) {
		hotspot_x = info->x;
		hotspot_y = info->y;
	}

	Glib::RefPtr<Gdk::Pixbuf> p (::get_icon (name, _cursor_set));
	Gdk::Cursor* c = new Gdk::Cursor (Gdk::Display::get_default(), p, hotspot_x, hotspot_y);
	cursors.push_back (c);
	return c;
}

void
MouseCursors::set_cursor_set (const std::string& name)
{
	using namespace Glib;
	using namespace Gdk;

	drop_all ();
	Gtkmm2ext::CursorInfo::drop_cursor_info();
	_cursor_set = name;

	std::string hotspot_info_path = get_icon_path ("hotspots", _cursor_set, false);

	if (!hotspot_info_path.empty()) {
		Gtkmm2ext::CursorInfo::load_cursor_info (hotspot_info_path);
	}

	/* these will throw exceptions if their images cannot be found.

	   the default hotspot coordinates will be overridden by any
	   data found by Gtkmm2ext::Cursors::load_cursor_info(). the values
	   here from the set of cursors used by Ardour; new cursor/icon
	   sets should come with a hotspot info file.
	*/

	zoom_in = make_cursor ("zoom_in_cursor", 10, 5);
	zoom_out = make_cursor ("zoom_out_cursor", 5, 5);
	scissors = make_cursor ("scissors", 5, 0);
	grabber = make_cursor ("grabber", 5, 0);
	grabber_note = make_cursor ("grabber_note", 5, 10);
	left_side_trim = make_cursor ("trim_left_cursor", 5, 11);
	anchored_left_side_trim = make_cursor ("anchored_trim_left_cursor", 5, 11);
	right_side_trim = make_cursor ("trim_right_cursor", 23, 11);
	anchored_right_side_trim = make_cursor ("anchored_trim_right_cursor", 23, 11);
	left_side_trim_right_only = make_cursor ("trim_left_cursor_right_only", 5, 11);
	right_side_trim_left_only = make_cursor ("trim_right_cursor_left_only", 23, 11);
	fade_in = make_cursor ("fade_in_cursor", 0, 0);
	fade_out = make_cursor ("fade_out_cursor", 29, 0);
	resize_left = make_cursor ("resize_left_cursor", 3, 10);
	resize_top_left = make_cursor ("resize_top_left_cursor", 3, 3);
	resize_top = make_cursor ("resize_top_cursor", 10, 3);
	resize_top_right = make_cursor ("resize_top_right_cursor", 18, 3);
	resize_right = make_cursor ("resize_right_cursor", 24, 10);
	resize_bottom_right = make_cursor ("resize_bottom_right_cursor", 18, 18);
	resize_bottom = make_cursor ("resize_bottom_cursor", 10, 24);
	resize_bottom_left = make_cursor ("resize_bottom_left_cursor", 3, 18);
	move = make_cursor ("move_cursor", 11, 11);
	expand_left_right = make_cursor ("expand_left_right_cursor", 11, 4);
	expand_up_down = make_cursor ("expand_up_down_cursor", 4, 11);
	selector = make_cursor ("i_beam_cursor", 4, 11);

	Gdk::Color fbg ("#ffffff");
	Gdk::Color ffg ("#000000");

	{
		RefPtr<Bitmap> source = Bitmap::create ((char const *) fader_cursor_bits, fader_cursor_width, fader_cursor_height);
		RefPtr<Bitmap> mask = Bitmap::create ((char const *) fader_cursor_mask_bits, fader_cursor_width, fader_cursor_height);
		fader = new Cursor (source, mask, ffg, fbg, fader_cursor_x_hot, fader_cursor_y_hot);
	}

	{
		char pix[4] = { 0, 0, 0, 0 };
		RefPtr<Bitmap> bits = Bitmap::create (pix, 2, 2);
		Color c;
		transparent = new Cursor (bits, bits, c, c, 0, 0);
	}

	cross_hair = new Cursor (CROSSHAIR);
	cursors.push_back (cross_hair);
	trimmer =  new Cursor (SB_H_DOUBLE_ARROW);
	cursors.push_back (trimmer);
	time_fx = new Cursor (SIZING);
	cursors.push_back (time_fx);
	wait = new Cursor (WATCH);
	cursors.push_back (wait);
	timebar = new Cursor(LEFT_PTR);
	cursors.push_back (timebar);
	midi_pencil = make_pencil_cursor ();
	cursors.push_back (midi_pencil);
	midi_select = new Cursor (CENTER_PTR);
	cursors.push_back (midi_select);
	midi_resize = new Cursor (SIZING);
	cursors.push_back (midi_resize);
	midi_erase = new Cursor (DRAPED_BOX);
	cursors.push_back (midi_erase);
	up_down = new Cursor (SB_V_DOUBLE_ARROW);
	cursors.push_back (up_down);
}

void
MouseCursors::create_invalid()
{
	char pix[4] = { 0, 0, 0, 0 };
	Glib::RefPtr<Gdk::Bitmap> bits = Gdk::Bitmap::create (pix, 2, 2);
	Gdk::Color c;
	_invalid = new Gdk::Cursor (bits, bits, c, c, 0, 0);
}

Gdk::Cursor*
MouseCursors::from_gdk_cursor (GdkCursor* gc)
{
	for (auto & c : cursors) {
		if (c->gobj() == gc) {
			return c;
		}
	}

	return nullptr;
}

