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

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include <glibmm/main.h>
#include <pango/pangocairo.h>
#include <ytkmm/drawingarea.h>
#include <ytkmm/table.h>

#include "ardour/midi_track.h"

#include "widgets/tooltips.h"

#include "ardour_window.h"
#include "chord_pads.h"

#include "pbd/i18n.h"

using namespace ArdourWidgets;

namespace {

const char* const note_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const flat_names[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
const char* const key_menu_names[12] = { "C", "C# / Db", "D", "D# / Eb", "E", "F", "F# / Gb", "G", "G# / Ab", "A", "A# / Bb", "B" };

struct ChordType {
	const char* label;
	const char* tip;
	int         iv[3];
	int         n;
};

/* the triad (or what stands for it) */
const ChordType types[] = {
	{ "Maj",  "Major: root, major third, fifth",           { 0, 4, 7 }, 3 },
	{ "Min",  "Minor: root, minor third, fifth",           { 0, 3, 7 }, 3 },
	{ "Dim",  "Diminished: minor third, diminished fifth (with 7: half diminished)", { 0, 3, 6 }, 3 },
	{ "Aug",  "Augmented: major third, augmented fifth",   { 0, 4, 8 }, 3 },
	{ "Sus2", "Suspended second: the third replaced by the second", { 0, 2, 7 }, 3 },
	{ "Sus4", "Suspended fourth: the third replaced by the fourth", { 0, 5, 7 }, 3 },
	{ "5",    "Power chord: root and fifth",               { 0, 7, 0 }, 2 },
};
const int n_types = sizeof (types) / sizeof (types[0]);

struct Tension {
	const char* label;
	int         iv[3]; ///< added above the root
	int         n;
};

/* added notes: sevenths and the colours on top of them */
const Tension tensions[] = {
	{ "—", { 0, 0, 0 },    0 },
	{ "6",      { 9, 0, 0 },    1 },
	{ "7",      { 10, 0, 0 },   1 },
	{ "maj7",   { 11, 0, 0 },   1 },
	{ "dim7",   { 9, 0, 0 },    1 },
	{ "9",      { 10, 14, 0 },  2 },
	{ "maj9",   { 11, 14, 0 },  2 },
	{ "add9",   { 14, 0, 0 },   1 },
	{ "11",     { 10, 14, 17 }, 3 },
	{ "13",     { 10, 14, 21 }, 3 },
	{ "7b9",    { 10, 13, 0 },  2 },
	{ "7#9",    { 10, 15, 0 },  2 },
	{ "7#11",   { 10, 18, 0 },  2 },
	{ "7b13",   { 10, 20, 0 },  2 },
	{ "6/9",    { 9, 14, 0 },   2 },
};
const int n_tensions = sizeof (tensions) / sizeof (tensions[0]);

const struct { const char* label; int64_t ticks; } lengths[] = {
	{ "1/1", 7680 }, { "1/2", 3840 }, { "1/4", 1920 }, { "1/8", 960 },
};
const int n_lengths = sizeof (lengths) / sizeof (lengths[0]);

/* seven-note scales: the pads are built from them by stacking thirds */
struct Scale {
	const char* name;
	int         st[7];
};
const Scale scales[] = {
	{ "Major (Ionian)",     { 0, 2, 4, 5, 7, 9, 11 } },
	{ "Dorian",             { 0, 2, 3, 5, 7, 9, 10 } },
	{ "Phrygian",           { 0, 1, 3, 5, 7, 8, 10 } },
	{ "Lydian",             { 0, 2, 4, 6, 7, 9, 11 } },
	{ "Mixolydian",         { 0, 2, 4, 5, 7, 9, 10 } },
	{ "Minor (Aeolian)",    { 0, 2, 3, 5, 7, 8, 10 } },
	{ "Locrian",            { 0, 1, 3, 5, 6, 8, 10 } },
	{ "",                   { 0 } }, /* separator */
	{ "Harmonic Minor",     { 0, 2, 3, 5, 7, 8, 11 } },
	{ "Melodic Minor",      { 0, 2, 3, 5, 7, 9, 11 } },
	{ "Harmonic Major",     { 0, 2, 4, 5, 7, 8, 11 } },
	{ "Phrygian Dominant",  { 0, 1, 4, 5, 7, 8, 10 } },
	{ "Lydian Dominant",    { 0, 2, 4, 6, 7, 9, 10 } },
	{ "Altered",            { 0, 1, 3, 4, 6, 8, 10 } },
	{ "Dorian #4",          { 0, 2, 3, 6, 7, 9, 10 } },
	{ "Hungarian Minor",    { 0, 2, 3, 6, 7, 8, 11 } },
	{ "Neapolitan Minor",   { 0, 1, 3, 5, 7, 8, 11 } },
	{ "Neapolitan Major",   { 0, 1, 3, 5, 7, 9, 11 } },
	{ "Double Harmonic",    { 0, 1, 4, 5, 7, 8, 11 } },
	{ "Persian",            { 0, 1, 4, 5, 6, 8, 11 } },
	{ "Enigmatic",          { 0, 1, 4, 6, 8, 10, 11 } },
};
const int n_scales = sizeof (scales) / sizeof (scales[0]);

ArdourButton*
small_button (std::string const& label, int width = 40)
{
	ArdourButton* b = manage (new ArdourButton (label));
	b->set_name ("tab button");
	b->set_corner_radius (2);
	b->set_size_request (width, -1);
	return b;
}

Gtk::Label*
header (std::string const& text, float yalign = 1.0)
{
	Gtk::Label* l = manage (new Gtk::Label ());
	l->set_markup ("<span size='small' foreground='#9aa3ad'>" + text + "</span>");
	l->set_alignment (0.0, yalign);
	return l;
}

void
set_rgb (cairo_t* cr, uint32_t rgb, double a = 1.0)
{
	cairo_set_source_rgba (cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, a);
}

void
rounded (cairo_t* cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path (cr);
	cairo_arc (cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc (cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc (cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc (cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path (cr);
}

/* centred text with pango */
void
text (cairo_t* cr, Gtk::Widget& w, std::string const& s, char const* font, double cx, double cy, uint32_t rgb, double align = 0.5)
{
	Glib::RefPtr<Pango::Layout> l = w.create_pango_layout ("");
	l->set_text (s);
	Pango::FontDescription fd (font);
	l->set_font_description (fd);
	int tw, th;
	l->get_pixel_size (tw, th);
	set_rgb (cr, rgb);
	cairo_move_to (cr, cx - tw * align, cy - th / 2.0);
	pango_cairo_show_layout (cr, l->gobj ());
}

/* centred pango markup */
void
markup (cairo_t* cr, Gtk::Widget& w, std::string const& m, char const* font, double cx, double cy, uint32_t rgb)
{
	Glib::RefPtr<Pango::Layout> l = w.create_pango_layout ("");
	l->set_markup (m);
	Pango::FontDescription fd (font);
	l->set_font_description (fd);
	int tw, th;
	l->get_pixel_size (tw, th);
	set_rgb (cr, rgb);
	cairo_move_to (cr, cx - tw / 2.0, cy - th / 2.0);
	pango_cairo_show_layout (cr, l->gobj ());
}

/* sharps and flats as the music signs */
std::string
signs (std::string s)
{
	std::string out;
	for (size_t i = 0; i < s.size (); ++i) {
		if (s[i] == '#') {
			out += "\u266f";
		} else if (s[i] == 'b') { /* lower case b is always a flat here (the note is B) */
			out += "\u266d";
		} else {
			out += s[i];
		}
	}
	return out;
}

bool
is_black (int n)
{
	const int pc = n % 12;
	return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

/* a piano keyboard from `lo` (a C) to `hi` (exclusive) in the box; `fill (note)` gives a key colour or 0,
 * `hit` (optional) receives the key under (px, py) */
int
draw_keys (cairo_t* cr, double x, double y, double w, double h, int lo, int hi, std::function<uint32_t (int)> fill,
           double px = -1, double py = -1)
{
	int whites = 0;
	for (int n = lo; n < hi; ++n) {
		whites += is_black (n) ? 0 : 1;
	}
	const double kw = w / std::max (1, whites);
	int          hit = -1;

	/* white keys */
	int wi = 0;
	for (int n = lo; n < hi; ++n) {
		if (is_black (n)) {
			continue;
		}
		const double kx = x + wi * kw;
		if (cr) {
			const uint32_t c = fill (n);
			cairo_rectangle (cr, kx + 0.5, y, kw - 1, h);
			set_rgb (cr, c ? c : 0xdfe2e6);
			cairo_fill (cr);
		}
		if (px >= kx && px < kx + kw && py >= y && py < y + h) {
			hit = n;
		}
		++wi;
	}
	/* black keys on top */
	wi = 0;
	for (int n = lo; n < hi; ++n) {
		if (!is_black (n)) {
			++wi;
			continue;
		}
		const double kx = x + wi * kw - kw * 0.3;
		const double bw = kw * 0.6;
		const double bh = h * 0.6;
		if (cr) {
			const uint32_t c = fill (n);
			cairo_rectangle (cr, kx, y, bw, bh);
			set_rgb (cr, c ? c : 0x24262a);
			cairo_fill (cr);
		}
		if (px >= kx && px < kx + bw && py >= y && py < y + bh) {
			hit = n;
		}
	}
	return hit;
}

const uint32_t blue   = 0x4a90e2;
const uint32_t orange = 0xf0a040;

/* how well a chord follows the last one (ChordPads::fit): best, good, possible */
uint32_t
fit_colour (int level)
{
	switch (level) {
	case 3: return 0x34b360;
	case 2: return 0xd2b33a;
	case 1: return 0xd77a34;
	default: return 0;
	}
}

std::string
chord_data (ChordPads::Chord const& c)
{
	return "c" + std::to_string (c.root) + ":" + std::to_string (c.type) + ":" + std::to_string (c.tension) + ":" + std::to_string (c.octave);
}

bool
parse_chord_data (std::string const& s, ChordPads::Chord& c)
{
	c = ChordPads::Chord { 0, 0, 0, -1, 0, 0 };
	return s.size () > 1 && s[0] == 'c' && sscanf (s.c_str () + 1, "%d:%d:%d:%d", &c.root, &c.type, &c.tension, &c.octave) == 4
	       && c.root >= 0 && c.root < 12;
}

} // namespace

/* ------------------------------------------------------------------------------------------------------ */

/** The pads, laid out like Cubase's: nine in the lower row like white keys, seven above them between
 *  the lower ones like black keys. Grey tiles with the chord name; a coloured strip at the bottom shows
 *  how well the chord would follow the one heard last. */
class ChordPads::PadGrid : public Gtk::DrawingArea
{
public:
	PadGrid (ChordPads& o) : _o (o), _hover (-1), _press (-1)
	{
		add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
		set_size_request (420, 150);
		std::vector<Gtk::TargetEntry> targets;
		targets.push_back (Gtk::TargetEntry (drag_target (), Gtk::TARGET_SAME_APP));
		drag_source_set (targets, Gdk::BUTTON1_MASK, Gdk::ACTION_COPY);
		/* a chord of the circle (or another pad) dropped on a pad goes there */
		drag_dest_set (targets, Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
	}

	/* the box of a pad: 0 .. 8 lower row, 9 .. 15 upper row */
	void pad_rect (int i, double& x, double& y, double& w, double& h) const
	{
		static const double upper[7] = { 1, 2, 4, 5, 6, 8, 9 };
		const double slot = get_width () / 9.5;
		w = slot - gap;
		h = (get_height () - gap) / 2.0;
		if (i < 9) {
			x = i * slot;
			y = h + gap;
		} else {
			x = upper[i - 9] * slot - slot / 2;
			y = 0;
		}
	}

	int pad_at (double px, double py) const
	{
		for (int i = 0; i < n_pads; ++i) {
			double x, y, w, h;
			pad_rect (i, x, y, w, h);
			if (px >= x && px < x + w && py >= y && py < y + h) {
				return i;
			}
		}
		return -1;
	}

protected:
	bool on_expose_event (GdkEventExpose*)
	{
		cairo_t* cr = gdk_cairo_create (get_window ()->gobj ());
		for (int i = 0; i < n_pads; ++i) {
			double x, y, w, h;
			pad_rect (i, x, y, w, h);
			const bool   sel = i == _o._sel;
			Chord const& c   = _o._pads[i];
			const int    lvl = _o._has_last ? fit (_o._last, c) : 0;

			/* the tile: a slight gradient, lighter when selected or under the mouse, darker while held */
			const uint32_t top = i == _press ? 0x37393d : (sel ? 0x63676e : (i == _hover ? 0x51545a : 0x4a4d52));
			const uint32_t bot = i == _press ? 0x2f3134 : (sel ? 0x54585e : (i == _hover ? 0x44474c : 0x3d4045));
			cairo_pattern_t* g = cairo_pattern_create_linear (0, y, 0, y + h);
			cairo_pattern_add_color_stop_rgb (g, 0, ((top >> 16) & 0xff) / 255.0, ((top >> 8) & 0xff) / 255.0, (top & 0xff) / 255.0);
			cairo_pattern_add_color_stop_rgb (g, 1, ((bot >> 16) & 0xff) / 255.0, ((bot >> 8) & 0xff) / 255.0, (bot & 0xff) / 255.0);
			rounded (cr, x, y, w, h, 2);
			cairo_set_source (cr, g);
			cairo_fill_preserve (cr);
			cairo_pattern_destroy (g);
			set_rgb (cr, sel ? 0xd6dae0 : 0x232528);
			cairo_set_line_width (cr, sel ? 1.5 : 1);
			cairo_stroke (cr);

			if (fit_colour (lvl)) {
				cairo_rectangle (cr, x + 1, y + h - 4, w - 2, 3);
				set_rgb (cr, fit_colour (lvl));
				cairo_fill (cr);
			}
			markup (cr, *this, chord_markup (c), "Sans Bold 12", x + w / 2, y + h / 2, 0xffffff);
			std::string const deg = degree (c);
			if (!deg.empty ()) {
				text (cr, *this, deg, "Sans 8", x + 5, y + 10, sel ? 0xffffff : 0xaab1ba, 0.0);
			}
		}
		cairo_destroy (cr);
		return true;
	}

	bool on_button_press_event (GdkEventButton* ev)
	{
		if (ev->button == 1 && ev->type == GDK_2BUTTON_PRESS && pad_at (ev->x, ev->y) >= 0) {
			_o.show_editor (true);
			return true;
		}
		if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) {
			return false;
		}
		_press = pad_at (ev->x, ev->y);
		if (_press >= 0) {
			_o.pad_pressed (_press);
		}
		queue_draw ();
		return false; /* the drag source needs it too */
	}

	bool on_button_release_event (GdkEventButton* ev)
	{
		if (ev->button == 1) {
			_press = -1;
			_o.pad_released ();
			queue_draw ();
		}
		return false;
	}

	bool on_motion_notify_event (GdkEventMotion* ev)
	{
		const int h = pad_at (ev->x, ev->y);
		if (h != _hover) {
			_hover = h;
			queue_draw ();
		}
		return false;
	}

	bool on_leave_notify_event (GdkEventCrossing*)
	{
		_hover = -1;
		queue_draw ();
		return false;
	}

	void on_drag_begin (Glib::RefPtr<Gdk::DragContext> const& ctx)
	{
		_o.silence ();
		Gtk::DrawingArea::on_drag_begin (ctx);
	}

	void on_drag_end (Glib::RefPtr<Gdk::DragContext> const&)
	{
		_press = -1;
		queue_draw ();
	}

	void on_drag_data_get (Glib::RefPtr<Gdk::DragContext> const&, Gtk::SelectionData& data, guint, guint)
	{
		std::string const s = std::to_string (std::max (0, _o._sel));
		data.set (drag_target (), 8, (const guint8*) s.c_str (), s.size ());
	}

	void on_drag_data_received (Glib::RefPtr<Gdk::DragContext> const& ctx, int x, int y, Gtk::SelectionData const& data, guint, guint time)
	{
		const int   pad = pad_at (x, y);
		std::string d   = data.get_length () > 0 ? data.get_data_as_string () : std::string ();
		Chord       c;
		bool        ok  = false;
		if (pad >= 0 && parse_chord_data (d, c)) {
			_o._pads[pad] = c;
			ok            = true;
		} else if (pad >= 0 && !d.empty () && isdigit (d[0])) {
			const int src = atoi (d.c_str ());
			if (src >= 0 && src < n_pads && src != pad) {
				_o._pads[pad] = _o._pads[src]; /* a pad dropped on another one: copied there */
				ok            = true;
			}
		}
		if (ok) {
			_o.select_pad (pad);
		}
		ctx->drag_finish (ok, false, time);
	}

private:
	ChordPads&       _o;
	int              _hover;
	int              _press;
	static const int gap = 5;
};

/** One octave to pick a note class (root, bass) */
class ChordPads::KeyPicker : public Gtk::DrawingArea
{
public:
	KeyPicker (uint32_t colour) : _colour (colour), _pc (-1)
	{
		add_events (Gdk::BUTTON_PRESS_MASK);
		set_size_request (126, 46);
	}
	void set (int pc)
	{
		_pc = pc;
		queue_draw ();
	}
	sigc::signal<void, int> Picked;

protected:
	bool on_expose_event (GdkEventExpose*)
	{
		cairo_t* cr = gdk_cairo_create (get_window ()->gobj ());
		draw_keys (cr, 0, 0, get_width (), get_height () - 1, 60, 72, [this] (int n) { return n % 12 == _pc ? _colour : 0; });
		/* name of the picked note on its key */
		if (_pc >= 0) {
			int whites = 0;
			for (int n = 60; n < 60 + _pc; ++n) {
				whites += is_black (n) ? 0 : 1;
			}
			const double kw = get_width () / 7.0;
			const double cx = is_black (_pc) ? whites * kw : whites * kw + kw / 2;
			text (cr, *this, pc_name (_pc), "Sans Bold 7", cx, is_black (_pc) ? get_height () * 0.42 : get_height () - 9,
			      0xffffff);
		}
		cairo_destroy (cr);
		return true;
	}
	bool on_button_press_event (GdkEventButton* ev)
	{
		if (ev->button != 1) {
			return false;
		}
		const int n = draw_keys (0, 0, 0, get_width (), get_height () - 1, 60, 72, [] (int) { return 0u; }, ev->x, ev->y);
		if (n >= 0) {
			Picked (n % 12);
		}
		return true;
	}

private:
	uint32_t _colour;
	int      _pc;
};

/** The notes of the selected chord on a keyboard (the bass note in its own colour) */
class ChordPads::ChordKeys : public Gtk::DrawingArea
{
public:
	ChordKeys () { set_size_request (-1, 62); }
	void set (std::vector<int> const& notes, bool with_bass)
	{
		_notes = notes;
		_bass  = with_bass;
		queue_draw ();
	}

protected:
	bool on_expose_event (GdkEventExpose*)
	{
		cairo_t*     cr    = gdk_cairo_create (get_window ()->gobj ());
		const double names = 18; /* the row of note names under the keys */
		const double kh    = get_height () - names;
		int          lo = 36, hi = 84; /* C2 .. B5 */
		if (!_notes.empty ()) {
			lo = std::min (lo, _notes.front () / 12 * 12);
			hi = std::max (hi, (_notes.back () / 12 + 1) * 12);
		}
		draw_keys (cr, 0, 0, get_width (), kh, lo, hi, [this] (int n) -> uint32_t {
			for (size_t i = 0; i < _notes.size (); ++i) {
				if (_notes[i] == n) {
					return (i == 0 && _bass) ? orange : blue;
				}
			}
			return 0;
		});
		int all = 0;
		for (int n = lo; n < hi; ++n) {
			all += is_black (n) ? 0 : 1;
		}
		const double kw = get_width () / (double) std::max (1, all);
		/* the centre of a key */
		auto key_x = [&] (int note) {
			int wi = 0;
			for (int n = lo; n < note; ++n) {
				wi += is_black (n) ? 0 : 1;
			}
			return is_black (note) ? wi * kw : wi * kw + kw / 2;
		};
		/* the octaves, on their C keys */
		for (int n = lo; n < hi; n += 12) {
			if (n % 12 == 0) {
				text (cr, *this, "C" + std::to_string (n / 12 - 1), "Sans 7", key_x (n), kh - 7, n == 60 ? 0x30343a : 0x7c838c);
			}
		}
		/* the notes of the chord, named under the keys */
		for (size_t i = 0; i < _notes.size (); ++i) {
			const int   n  = _notes[i];
			std::string nm = std::string (pc_name (n % 12)) + std::to_string (n / 12 - 1);
			const double x = key_x (n);
			set_rgb (cr, (i == 0 && _bass) ? orange : blue);
			cairo_move_to (cr, x, kh + 1);
			cairo_line_to (cr, x, kh + 4);
			cairo_set_line_width (cr, 1.5);
			cairo_stroke (cr);
			text (cr, *this, nm, "Sans Bold 9", x, kh + names / 2 + 2, (i == 0 && _bass) ? orange : 0xe6eaef);
		}
		cairo_destroy (cr);
		return true;
	}

private:
	std::vector<int> _notes;
	bool             _bass = false;
};

/* ------------------------------------------------------------------------------------------------------ */

/** The circle of fifths as Cubase's chord assistant draws it: the reference chord (the one heard last, or
 *  the key's tonic) at the top, the majors around it on the outer ellipse (IV to the left, V to the right),
 *  their relative minors on the inner one, the leading-tone chords near the centre. A triangle under each
 *  chord shows how well it follows the reference, the key's chords carry their degree. */
class ChordPads::Circle : public Gtk::DrawingArea
{
public:
	Circle (ChordPads& o) : _o (o), _hover (-1), _press (-1)
	{
		add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
		set_size_request (360, 250);
		std::vector<Gtk::TargetEntry> targets;
		targets.push_back (Gtk::TargetEntry (drag_target (), Gtk::TARGET_SAME_APP));
		drag_source_set (targets, Gdk::BUTTON1_MASK, Gdk::ACTION_COPY);
	}

protected:
	struct Box {
		double x, y, w, h;
		Chord  c;
		int    spelling = -1;
	};
	std::vector<Box> _boxes;

	Chord ref_chord () const
	{
		if (_o._has_last) {
			return _o._last;
		}
		int const* st = scales[_scale].st;
		return Chord { _key_root, st[2] == 3 ? 1 : 0, 0, -1, 0, 0 };
	}

	void layout ()
	{
		_boxes.clear ();
		const Chord  ref = ref_chord ();
		const int    key = ref.type == 1 ? (ref.root + 3) % 12 : (ref.type == 2 ? (ref.root + 1) % 12 : ref.root);
		const double cx = get_width () / 2.0, cy = get_height () / 2.0 + 4;
		const double W = get_width (), H = get_height ();
		auto add = [&] (Chord c, double ex, double ey, double a, int spelling) {
			c.octave = c.root >= 7 ? -1 : 0;
			Box b;
			Glib::RefPtr<Pango::Layout> l = create_pango_layout ("");
			l->set_markup (chord_markup (c, spelling));
			Pango::FontDescription fd ("Sans Bold 8");
			l->set_font_description (fd);
			int tw, th;
			l->get_pixel_size (tw, th);
			b.w = std::max (30, tw + 10);
			b.h = 17;
			b.x = cx + ex * std::cos (a) - b.w / 2;
			b.y = cy + ey * std::sin (a) - b.h / 2;
			b.c = c;
			b.spelling = spelling;
			_boxes.push_back (b);
		};
		for (int k = -6; k < 6; ++k) {
			const double a  = -M_PI / 2 + k * M_PI / 6;
			const int    pc = ((key + 7 * k) % 12 + 12) % 12;
			/* the conventional names: the flat side of the circle (F Bb Eb Ab Db Gb, Dm Gm Cm Fm Bbm Ebm) with flats */
			const int fi = (pc * 7) % 12; /* position of the major from C */
			add (Chord { pc, 0, 0, -1, 0, 0 }, W * 0.43, H * 0.41, a, fi >= 6 ? 1 : 0);
			add (Chord { (pc + 9) % 12, 1, 0, -1, 0, 0 }, W * 0.29, H * 0.25, a, fi >= 7 ? 1 : 0);
			if (k == 0) {
				/* the leading-tone chord of the reference's key, near the centre as in Cubase */
				add (Chord { (pc + 11) % 12, 2, 0, -1, 0, 0 }, W * 0.12, H * 0.17, M_PI / 3, 0);
			}
		}
	}

	int box_at (double x, double y) const
	{
		for (size_t i = 0; i < _boxes.size (); ++i) {
			Box const& b = _boxes[i];
			if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
				return (int) i;
			}
		}
		return -1;
	}

	bool on_expose_event (GdkEventExpose*)
	{
		layout ();
		cairo_t*     cr = gdk_cairo_create (get_window ()->gobj ());
		const double W = get_width (), H = get_height ();
		const double cx = W / 2.0, cy = H / 2.0 + 4;

		/* the web: ellipses and spokes */
		set_rgb (cr, 0x5c6068, 0.55);
		cairo_set_line_width (cr, 1);
		for (double f : { 1.0, 0.65, 0.33 }) {
			cairo_save (cr);
			cairo_translate (cr, cx, cy);
			cairo_scale (cr, W * 0.43 * f, H * 0.41 * f);
			cairo_arc (cr, 0, 0, 1, 0, 2 * M_PI);
			cairo_restore (cr);
			cairo_stroke (cr);
		}
		for (int k = 0; k < 12; ++k) {
			const double a = -M_PI / 2 + k * M_PI / 6;
			cairo_move_to (cr, cx, cy);
			cairo_line_to (cr, cx + W * 0.48 * std::cos (a), cy + H * 0.47 * std::sin (a));
		}
		cairo_stroke (cr);

		const Chord ref = ref_chord ();
		for (size_t i = 0; i < _boxes.size (); ++i) {
			Box const& b   = _boxes[i];
			const int  lvl = fit (ref, b.c);
			draw_box (cr, b, lvl, (int) i == _hover);
		}
		/* the reference in the middle */
		Box mid;
		mid.w = 52;
		mid.h = 18;
		mid.x = cx - mid.w / 2;
		mid.y = cy - mid.h / 2;
		mid.c = ref;
		draw_box (cr, mid, -2, false);
		cairo_destroy (cr);
		return true;
	}

	void draw_box (cairo_t* cr, Box const& b, int lvl, bool hover)
	{
		rounded (cr, b.x, b.y, b.w, b.h, 2);
		set_rgb (cr, lvl == -2 ? 0x5d7fa8 : (lvl == -1 ? 0x4f6f96 : (hover ? 0x575b62 : 0x45484e)));
		cairo_fill_preserve (cr);
		set_rgb (cr, 0x7a7f87, 0.8);
		cairo_set_line_width (cr, 1);
		cairo_stroke (cr);
		markup (cr, *this, chord_markup (b.c, b.spelling), "Sans Bold 8", b.x + b.w / 2, b.y + b.h / 2, 0xffffff);
		/* the triangle: how well it follows; the degree in the key next to it */
		const double tx = b.x + b.w / 2, ty = b.y + b.h + 1;
		cairo_move_to (cr, tx - 4, ty);
		cairo_line_to (cr, tx + 4, ty);
		cairo_line_to (cr, tx, ty + 5);
		cairo_close_path (cr);
		set_rgb (cr, lvl == -2 || lvl == -1 ? 0x4a90e2 : (fit_colour (lvl) ? fit_colour (lvl) : 0x6c7179));
		cairo_fill (cr);
		std::string const deg = degree (b.c);
		if (!deg.empty () && deg[0] != 'b' && deg[0] != '#') {
			text (cr, *this, deg, "Sans 7", tx + 6, ty + 3, 0xd8dce2, 0.0);
		}
	}

	bool on_button_press_event (GdkEventButton* ev)
	{
		if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) {
			return false;
		}
		_press = box_at (ev->x, ev->y);
		if (_press >= 0) {
			_pressed = _boxes[_press].c;
			_o.play_chord (_pressed);
		}
		return false;
	}

	bool on_button_release_event (GdkEventButton* ev)
	{
		if (ev->button == 1) {
			_o.silence ();
			if (_press >= 0) {
				/* the circle turns to the chord on release, so a drag still takes the pressed one */
				_o.set_last (_pressed);
			}
			_press = -1;
		}
		return false;
	}

	bool on_motion_notify_event (GdkEventMotion* ev)
	{
		const int h = box_at (ev->x, ev->y);
		if (h != _hover) {
			_hover = h;
			queue_draw ();
		}
		return false;
	}

	bool on_leave_notify_event (GdkEventCrossing*)
	{
		_hover = -1;
		queue_draw ();
		return false;
	}

	void on_drag_begin (Glib::RefPtr<Gdk::DragContext> const& ctx)
	{
		_o.silence ();
		Gtk::DrawingArea::on_drag_begin (ctx);
	}

	void on_drag_end (Glib::RefPtr<Gdk::DragContext> const&)
	{
		_press = -1;
	}

	void on_drag_data_get (Glib::RefPtr<Gdk::DragContext> const&, Gtk::SelectionData& data, guint, guint)
	{
		std::string const s = chord_data (_pressed);
		data.set (drag_target (), 8, (const guint8*) s.c_str (), s.size ());
	}

private:
	ChordPads& _o;
	int        _hover;
	int        _press;
	Chord      _pressed;
};

/* ------------------------------------------------------------------------------------------------------ */

/* filled from the key (C major) when the first chord pads open */
std::vector<ChordPads::Chord> ChordPads::_pads;
int  ChordPads::_len       = 0;
int  ChordPads::_key_root  = 0;
int  ChordPads::_scale     = 0;

const char*
ChordPads::pc_name (int pc)
{
	/* the spelling (sharps or flats) that names the seven notes of the scale with seven different letters;
	 * when both do (or neither), flats for the flat keys */
	static int cache_root = -1, cache_scale = -1;
	static bool flats = false;
	if (cache_root != _key_root || cache_scale != _scale) {
		cache_root  = _key_root;
		cache_scale = _scale;
		auto letters = [] (const char* const* names) {
			std::set<char> l;
			for (int i = 0; i < 7; ++i) {
				l.insert (names[(_key_root + scales[_scale].st[i]) % 12][0]);
			}
			return (int) l.size ();
		};
		const int sharp_letters = letters (note_names);
		const int flat_letters  = letters (flat_names);
		if (sharp_letters != flat_letters) {
			flats = flat_letters > sharp_letters;
		} else {
			/* F and the black keys read as flat keys (F, Bb, Eb, Ab, Db, Gb) */
			flats = _key_root == 5 || _key_root == 10 || _key_root == 3 || _key_root == 8 || _key_root == 1 || _key_root == 6;
		}
	}
	pc = ((pc % 12) + 12) % 12;
	return flats ? flat_names[pc] : note_names[pc];
}

ChordPads::ChordPads ()
	: _sel (0)
	, _sounding_ch (0)
	, _channel (0)
	, _has_last (false)
	, _grid (manage (new PadGrid (*this)))
	, _circle (manage (new Circle (*this)))
	, _root_picker (manage (new KeyPicker (blue)))
	, _bass_picker (manage (new KeyPicker (orange)))
	, _chord_keys (manage (new ChordKeys ()))
	, _fill_button (_("Fill Pads"))
	, _edit_button (_("Edit Chord"))
	, _editor_window (0)
	, _no_bass_button (_("None"))
	, _oct_down ("−")
	, _oct_up ("+")
	, _insert_button (_("Insert at Playhead"))
{
	/* top: the key and how to use the pads */
	Gtk::HBox* top = manage (new Gtk::HBox ());
	top->set_spacing (4);
	Gtk::Label* kl = manage (new Gtk::Label (_("Key")));
	top->pack_start (*kl, false, false, 2);
	for (int i = 0; i < 12; ++i) {
		_key_dropdown.add_menu_elem (Gtk::Menu_Helpers::MenuElem (key_menu_names[i], [this, i] () {
			_key_root = i;
			fill_from_key (); /* like Cubase: the pads follow the key */
		}));
	}
	_key_dropdown.set_name ("generic button");
	_key_dropdown.set_size_request (52, -1);
	top->pack_start (_key_dropdown, false, false);
	for (int i = 0; i < n_scales; ++i) {
		if (!scales[i].name[0]) {
			_scale_dropdown.add_separator ();
			continue;
		}
		_scale_dropdown.add_menu_elem (Gtk::Menu_Helpers::MenuElem (_(scales[i].name), [this, i] () {
			_scale = i;
			fill_from_key ();
		}));
	}
	_scale_dropdown.set_name ("generic button");
	_scale_dropdown.set_size_request (130, -1);
	ArdourWidgets::set_tooltip (_scale_dropdown, _("Scale of the key: picking it puts its chords on the pads, they show their degree in it"));
	top->pack_start (_scale_dropdown, false, false);
	_fill_button.set_name ("generic button");
	_fill_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_fill_button, _("Put the chords of the scale on the pads: a triad on each of its seven degrees, and the seventh chord on the fifth degree"));
	_fill_button.signal_clicked.connect (sigc::mem_fun (*this, &ChordPads::fill_from_key));
	(void) _fill_button; /* the pads are filled when the key or the scale is picked */
	_edit_button.set_name ("generic button");
	_edit_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_edit_button, _("Show the chord editor of the selected pad: root, type, tension, bass, voicing (also: double-click a pad)"));
	_edit_button.signal_clicked.connect ([this] () { show_editor (!_edit_button.get_active ()); });
	top->pack_start (_edit_button, false, false, 4);
	_insert_button.set_name ("generic button");
	_insert_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_insert_button, _("Write the selected chord at the playhead"));
	_insert_button.signal_clicked.connect ([this] () { InsertRequested (_sel); });
	top->pack_start (_insert_button, false, false);
	Gtk::Label* hint = manage (new Gtk::Label ());
	hint->set_markup (std::string ("<span foreground='#b4bbc4'>")
	                  + _("Hold a pad to hear it · drag it onto the notes to write it, or a chord of the circle onto a pad") + "</span>");
	top->pack_end (*hint, false, false, 4);

	/* left: the pads and the notes of the selected one */
	Gtk::VBox* left = manage (new Gtk::VBox ());
	left->set_spacing (4);
	left->pack_start (*_grid, true, true);
	left->pack_start (*_chord_keys, false, false);
	ArdourWidgets::set_tooltip (*_grid, _("Hold a pad to hear the chord; drag it onto the notes to write it there"));

	/* right: the chord editor of the selected pad */
	Gtk::VBox* right = manage (new Gtk::VBox ());
	right->set_spacing (4);
	Gtk::HBox* cols = manage (new Gtk::HBox ());
	cols->set_spacing (12);

	Gtk::VBox* c = manage (new Gtk::VBox ());
	c->set_spacing (2);
	c->pack_start (*header (_("Root")), false, false);
	c->pack_start (*_root_picker, false, false);
	_root_picker->Picked.connect ([this] (int pc) { edit (0, pc); });
	cols->pack_start (*c, false, false);

	c = manage (new Gtk::VBox ());
	c->set_spacing (2);
	c->pack_start (*header (_("Type")), false, false);
	Gtk::Table* t = manage (new Gtk::Table (4, 2, true));
	t->set_spacings (2);
	for (int i = 0; i < n_types; ++i) {
		ArdourButton* b = small_button (types[i].label, 44);
		ArdourWidgets::set_tooltip (*b, _(types[i].tip));
		b->signal_clicked.connect ([this, i] () { edit (1, i); });
		t->attach (*b, i % 2, i % 2 + 1, i / 2, i / 2 + 1, Gtk::FILL, Gtk::SHRINK);
		_type_buttons.push_back (b);
	}
	c->pack_start (*t, false, false);
	cols->pack_start (*c, false, false);

	c = manage (new Gtk::VBox ());
	c->set_spacing (2);
	c->pack_start (*header (_("Tension")), false, false);
	t = manage (new Gtk::Table (5, 3, true));
	t->set_spacings (2);
	for (int i = 0; i < n_tensions; ++i) {
		ArdourButton* b = small_button (tensions[i].label, 44);
		if (i == 0) {
			ArdourWidgets::set_tooltip (*b, _("No added notes"));
		}
		b->signal_clicked.connect ([this, i] () { edit (2, i); });
		t->attach (*b, i % 3, i % 3 + 1, i / 3, i / 3 + 1, Gtk::FILL, Gtk::SHRINK);
		_tension_buttons.push_back (b);
	}
	c->pack_start (*t, false, false);
	cols->pack_start (*c, false, false);

	c = manage (new Gtk::VBox ());
	c->set_spacing (2);
	c->pack_start (*header (_("Bass")), false, false);
	c->pack_start (*_bass_picker, false, false);
	_bass_picker->Picked.connect ([this] (int pc) { edit (3, pc); });
	_no_bass_button.set_name ("tab button");
	_no_bass_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_no_bass_button, _("No separate bass note (the chord's own lowest note is the bass)"));
	_no_bass_button.signal_clicked.connect ([this] () { edit (3, -1); });
	c->pack_start (_no_bass_button, false, false);
	cols->pack_start (*c, false, false);
	right->pack_start (*cols, false, false);

	/* voicing, octave, length, insert */
	Gtk::HBox* bottom = manage (new Gtk::HBox ());
	bottom->set_spacing (2);
	bottom->pack_start (*header (_("Voicing"), 0.5), false, false, 2);
	{
		const char* inv[] = { _("Root"), _("1st"), _("2nd"), _("3rd") };
		const char* tip[] = { _("Root position"), _("First inversion: the lowest note goes up an octave"),
		                      _("Second inversion"), _("Third inversion (chords with four or more notes)") };
		for (int i = 0; i < 4; ++i) {
			ArdourButton* b = small_button (inv[i], 38);
			ArdourWidgets::set_tooltip (*b, tip[i]);
			b->signal_clicked.connect ([this, i] () { edit (4, i); });
			bottom->pack_start (*b, false, false);
			_inversion_buttons.push_back (b);
		}
	}
	bottom->pack_start (*header (_("Octave"), 0.5), false, false, 6);
	for (ArdourButton* b : { &_oct_down, &_oct_up }) {
		b->set_name ("tab button");
		b->set_corner_radius (2);
		b->set_size_request (24, -1);
	}
	_oct_down.signal_clicked.connect ([this] () { edit (5, -1); });
	_oct_up.signal_clicked.connect ([this] () { edit (5, 1); });
	_oct_label.set_size_request (22, -1);
	bottom->pack_start (_oct_down, false, false);
	bottom->pack_start (_oct_label, false, false);
	bottom->pack_start (_oct_up, false, false);
	bottom->pack_start (*header (_("Length"), 0.5), false, false, 6);
	for (int i = 0; i < n_lengths; ++i) {
		ArdourButton* b = small_button (lengths[i].label, 34);
		ArdourWidgets::set_tooltip (*b, _("Length of the written chord"));
		b->signal_clicked.connect ([this, i] () {
			_len = i;
			update ();
		});
		bottom->pack_start (*b, false, false);
		_length_buttons.push_back (b);
	}
	right->pack_start (*bottom, false, false);

	Gtk::HBox* main = manage (new Gtk::HBox ());
	main->set_spacing (14);
	main->pack_start (*left, true, true);
	{
		Gtk::VBox* cv = manage (new Gtk::VBox ());
		cv->set_spacing (2);
		Gtk::Label* ct = manage (new Gtk::Label ());
		ct->set_markup (std::string ("<span weight='bold' foreground='#d8dce2'>") + _("Circle of Fifths") + "</span>");
		cv->pack_start (*ct, false, false);
		cv->pack_start (*_circle, true, true);
		Gtk::Label* legend = manage (new Gtk::Label ());
		legend->set_markup (std::string ("<span size='small'><span foreground='#34b360'>\u25cf</span> ") + _("best")
		                    + "  <span foreground='#d2b33a'>\u25cf</span> " + _("good")
		                    + "  <span foreground='#d77a34'>\u25cf</span> " + _("possible") + "</span>");
		cv->pack_start (*legend, false, false);
		ArdourWidgets::set_tooltip (*_circle, _("Circle of fifths: majors outside, their relative minors and the diminished chords inside. "
		                                        "Click to hear a chord, drag it onto a pad or into the notes. The colours show what "
		                                        "follows the chord heard last best: neighbours on the circle (IV, V and the relatives), "
		                                        "then two and three steps away. The white edge is the key of the pads."));
		main->pack_start (*cv, false, false);
	}

	/* the chord editor, shown on demand in a small window of its own */
	_editor_box.pack_start (*right, false, false);
	_editor_box.set_border_width (8);
	_editor_box.show_all ();

	pack_start (*top, false, false);
	pack_start (*main, true, true);
	set_spacing (4);
	set_border_width (4);
	show_all ();
	if ((int) _pads.size () != n_pads) {
		fill_from_key (); /* the first time: the chords of C major */
	} else {
		update ();
	}
}

ChordPads::~ChordPads ()
{
	silence ();
	if (_editor_window) {
		_editor_window->remove ();
		delete _editor_window;
	}
}

void
ChordPads::set_track (std::shared_ptr<ARDOUR::MidiTrack> t)
{
	silence ();
	_track = t;
}

std::vector<int>
ChordPads::chord_pitches (Chord const& c)
{
	ChordType const& t    = types[std::max (0, std::min (n_types - 1, c.type))];
	Tension const&   tn   = tensions[std::max (0, std::min (n_tensions - 1, c.tension))];
	const int        base = 60 + c.root + 12 * c.octave;

	std::set<int> iv;
	for (int i = 0; i < t.n; ++i) {
		iv.insert (t.iv[i]);
	}
	for (int i = 0; i < tn.n; ++i) {
		iv.insert (tn.iv[i]);
	}
	std::vector<int> p;
	for (int i : iv) {
		p.push_back (base + i);
	}
	/* inversions: the lowest notes go up an octave */
	for (int k = 0; k < c.inversion && k + 1 < (int) p.size (); ++k) {
		const int lo = p.front ();
		p.erase (p.begin ());
		p.push_back (lo + 12);
		std::sort (p.begin (), p.end ());
	}
	/* a bass note below the chord */
	if (c.bass >= 0) {
		int b = 48 + c.bass + 12 * c.octave;
		while (b >= p.front ()) {
			b -= 12;
		}
		p.insert (p.begin (), b);
	}
	std::vector<int> out;
	for (int n : p) {
		if (n >= 0 && n <= 127) {
			out.push_back (n);
		}
	}
	return out;
}

std::string
ChordPads::chord_name (Chord const& c)
{
	const int   type = std::max (0, std::min (n_types - 1, c.type));
	std::string tn   = c.tension > 0 && c.tension < n_tensions ? tensions[c.tension].label : "";
	std::string s    = pc_name (c.root);

	switch (type) {
	case 0: /* major */
		s += tn;
		break;
	case 1: /* minor */
		s += tn == "maj7" ? std::string ("m(maj7)") : "m" + tn;
		break;
	case 2: /* diminished: with a minor seventh it is half diminished */
		if (tn == "7") {
			s += "m7b5";
		} else if (tn == "dim7") {
			s += "dim7";
		} else {
			s += tn.empty () ? std::string ("dim") : "dim(" + tn + ")";
		}
		break;
	case 3:
		s += "+" + tn;
		break;
	case 4:
	case 5: {
		std::string const sus = type == 4 ? "sus2" : "sus4";
		if (tn.empty ()) {
			s += sus;
		} else if (tn == "add9" || tn == "6" || tn == "6/9") {
			s += sus + "(" + tn + ")";
		} else {
			s += tn + sus;
		}
		break;
	}
	default:
		s += tn.empty () ? std::string ("5") : "5(" + tn + ")";
		break;
	}
	if (c.bass >= 0 && c.bass != c.root) {
		s += std::string ("/") + pc_name (c.bass);
	}
	return s;
}

std::string
ChordPads::chord_markup (Chord const& c, int spelling)
{
	auto nm = [spelling] (int pc) -> std::string {
		return spelling < 0 ? pc_name (pc) : (spelling ? flat_names[((pc % 12) + 12) % 12] : note_names[((pc % 12) + 12) % 12]);
	};
	/* Cubase-like: B♭min<sup>7</sup>, D♭maj<sup>7</sup>, Gdim, Cmin<sup>7♭5</sup> */
	const int   type = std::max (0, std::min (n_types - 1, c.type));
	std::string tn   = c.tension > 0 && c.tension < n_tensions ? tensions[c.tension].label : "";
	std::string q, sup;
	switch (type) {
	case 0:
		if (tn == "maj7" || tn == "maj9") {
			q   = "maj";
			sup = tn.substr (3);
		} else {
			sup = tn;
		}
		break;
	case 1:
		q   = "min";
		sup = tn;
		break;
	case 2:
		if (tn == "7") {
			q   = "min";
			sup = "7b5";
		} else if (tn == "dim7") {
			q   = "dim";
			sup = "7";
		} else {
			q   = "dim";
			sup = tn;
		}
		break;
	case 3: q = "aug"; sup = tn; break;
	case 4: q = "sus2"; sup = tn; break;
	case 5: q = "sus4"; sup = tn; break;
	default: q = "5"; sup = tn; break;
	}
	std::string m = signs (nm (c.root)) + q;
	if (!sup.empty ()) {
		m += "<sup>" + signs (sup) + "</sup>";
	}
	if (c.bass >= 0 && c.bass != c.root) {
		m += "/" + signs (nm (c.bass));
	}
	return m;
}

std::string
ChordPads::degree (Chord const& c)
{
	static const char* const roman[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };
	int const*  scale = scales[_scale].st;
	const int   rel   = (c.root - _key_root + 12) % 12;
	int         d     = -1;
	std::string acc;
	for (int i = 0; i < 7; ++i) {
		if (scale[i] == rel) {
			d = i;
		}
	}
	/* a root outside the scale: flat of the degree above it (bVII), else sharp of the one below */
	for (int i = 0; d < 0 && i < 7; ++i) {
		if (scale[i] == (rel + 1) % 12) {
			d   = i;
			acc = "b";
		}
	}
	for (int i = 0; d < 0 && i < 7; ++i) {
		if (scale[i] == (rel + 11) % 12) {
			d   = i;
			acc = "#";
		}
	}
	if (d < 0) {
		return std::string ();
	}
	std::string r = roman[d];
	if (c.type == 1 || c.type == 2) {
		std::transform (r.begin (), r.end (), r.begin (), ::tolower);
	}
	std::string const tn = c.tension > 0 && c.tension < n_tensions ? tensions[c.tension].label : "";
	if (c.type == 2) {
		r += tn == "7" ? "ø7" : "°" + (tn == "dim7" ? std::string ("7") : tn);
	} else if (c.type == 3) {
		r += "+" + tn;
	} else {
		r += tn;
	}
	return acc + r;
}

std::vector<uint8_t>
ChordPads::pitches (int pad) const
{
	std::vector<uint8_t> v;
	if (pad >= 0 && pad < (int) _pads.size ()) {
		for (int n : chord_pitches (_pads[pad])) {
			v.push_back ((uint8_t) n);
		}
	}
	return v;
}

std::string
ChordPads::name (int pad) const
{
	return pad >= 0 && pad < (int) _pads.size () ? chord_name (_pads[pad]) : std::string ();
}

Temporal::Beats
ChordPads::length () const
{
	return Temporal::Beats::ticks (lengths[std::max (0, std::min (n_lengths - 1, _len))].ticks);
}

void
ChordPads::select_pad (int i)
{
	_sel = i;
	update ();
}

void
ChordPads::edit (int what, int value)
{
	Chord& c = _pads[_sel];
	switch (what) {
	case 0: c.root = value; break;
	case 1: c.type = value; break;
	case 2: c.tension = value; break;
	case 3: c.bass = value; break;
	case 4: c.inversion = value; break;
	case 5: c.octave = std::max (-3, std::min (3, c.octave + value)); break;
	}
	update ();
	/* let the change be heard, like a pad press */
	set_last (_pads[_sel]);
	audition (_sel, 700);
}

void
ChordPads::fill_from_key ()
{
	/* stack thirds inside the scale: the third and fifth give the triad, the seventh the seventh chord */
	int const* st = scales[_scale].st;
	auto above = [st] (int d, int k) { return (st[(d + k) % 7] - st[d] + 12) % 12; };
	auto build = [&] (int d, bool seventh) {
		Chord c;
		const int third = above (d, 2), fifth = above (d, 4), sev = above (d, 6);
		c.root    = (_key_root + st[d]) % 12;
		c.tension = 0;
		if (third == 4 && fifth == 7) {
			c.type = 0;
		} else if (third == 3 && fifth == 7) {
			c.type = 1;
		} else if (third == 3 && fifth == 6) {
			c.type = 2;
			seventh = true; /* the diminished triad as m7b5 / dim7, as it is mostly used */
		} else if (third == 4 && fifth == 8) {
			c.type = 3;
		} else if (fifth == 7) {
			c.type = third == 2 ? 4 : 5; /* no third in the scale there: sus */
		} else {
			c.type = third >= 4 ? 0 : 1; /* odd fifths (exotic scales): the nearest triad */
		}
		if (seventh) {
			c.tension = sev == 11 ? 3 : (sev == 10 ? 2 : (sev == 9 && c.type == 2 ? 4 : 0));
		}
		c.bass      = -1;
		c.inversion = 0;
		c.octave    = c.root >= 7 ? -1 : 0; /* roots between G3 and F#4 */
		return c;
	};
	/* lower row: the triads of the seven degrees and two colours of the tonic (6, 6/9);
	 * upper row: the seventh chords of the seven degrees */
	_pads.assign (n_pads, Chord { 0, 0, 0, -1, 0, 0 });
	for (int d = 0; d < 7; ++d) {
		_pads[d]     = build (d, false);
		_pads[9 + d] = build (d, true);
	}
	_pads[7] = _pads[0];
	_pads[8] = _pads[0];
	if (_pads[0].type <= 1) {
		_pads[7].tension = 1;  /* 6 */
		_pads[8].tension = 14; /* 6/9 */
	} else {
		_pads[7].tension = 2;
		_pads[8].tension = 7; /* add9 */
	}
	update ();
}

void
ChordPads::show_editor (bool yn)
{
	_edit_button.set_active (yn);
	if (!yn) {
		if (_editor_window) {
			_editor_window->hide ();
		}
		return;
	}
	if (!_editor_window) {
		_editor_window = new ArdourWindow (_("Chord Editor"));
		_editor_window->add (_editor_box);
		_editor_window->set_resizable (false);
		_editor_window->signal_hide ().connect ([this] () { _edit_button.set_active (false); });
		/* closing the window only hides it (it is reused) */
		_editor_window->signal_delete_event ().connect ([this] (GdkEventAny*) { _editor_window->hide (); return true; }, false);
	}
	if (Gtk::Window* top = dynamic_cast<Gtk::Window*> (get_toplevel ())) {
		_editor_window->set_transient_for (*top);
	}
	if (!_editor_window->get_visible ()) {
		/* open above the button (over the tracks), so the pads stay visible */
		int x, y;
		_edit_button.get_window ()->get_origin (x, y);
		Gtk::Allocation const a   = _edit_button.get_allocation ();
		Gtk::Requisition const r  = _editor_window->size_request ();
		_editor_window->move (x + a.get_x (), std::max (0, y + a.get_y () - r.height - 40)); /* 40: title bar */
	}
	_editor_window->present ();
}

void
ChordPads::update ()
{
	Chord const& c = _pads[_sel];
	_root_picker->set (c.root);
	_bass_picker->set (c.bass);
	_no_bass_button.set_active (c.bass < 0);
	for (int i = 0; i < (int) _type_buttons.size (); ++i) {
		_type_buttons[i]->set_active (i == c.type);
	}
	for (int i = 0; i < (int) _tension_buttons.size (); ++i) {
		_tension_buttons[i]->set_active (i == c.tension);
	}
	for (int i = 0; i < (int) _inversion_buttons.size (); ++i) {
		_inversion_buttons[i]->set_active (i == c.inversion);
	}
	for (int i = 0; i < (int) _length_buttons.size (); ++i) {
		_length_buttons[i]->set_active (i == _len);
	}
	_oct_label.set_text (c.octave > 0 ? "+" + std::to_string (c.octave) : std::to_string (c.octave));
	_key_dropdown.set_text (pc_name (_key_root));
	_scale_dropdown.set_text (_(scales[_scale].name));
	_chord_keys->set (chord_pitches (c), c.bass >= 0);
	_grid->queue_draw ();
	_circle->queue_draw ();
}

void
ChordPads::audition (int pad, int ms)
{
	sound (pad);
	_stop_timer = Glib::signal_timeout ().connect ([this] () { silence (); return false; }, ms);
}

void
ChordPads::pad_pressed (int pad)
{
	select_pad (pad);
	set_last (_pads[pad]);
	/* sounds while held; the timer is for a release that never comes (a drag took it) */
	audition (pad, 8000);
}

void
ChordPads::pad_released ()
{
	silence ();
}

int
ChordPads::fit (Chord const& last, Chord const& next)
{
	/* the place on the circle: the major key the chord belongs to as I, vi (minor) or vii (diminished) */
	auto place = [] (Chord const& c, int& ring) {
		int key;
		if (c.type == 1) {
			ring = 1;
			key  = c.root + 3;
		} else if (c.type == 2) {
			ring = 2;
			key  = c.root + 1;
		} else {
			ring = 0;
			key  = c.root;
		}
		return ((key % 12) * 7) % 12;
	};
	int       ra, rb;
	const int ia = place (last, ra);
	const int ib = place (next, rb);
	if (ra == rb && last.root == next.root) {
		return -1;
	}
	const int d  = ((ib - ia) % 12 + 18) % 12 - 6; /* -6 .. 5, < 0: anticlockwise (down a fifth) */
	const int ad = std::abs (d);
	/* a dominant seventh wants to go down a fifth, not further up */
	const bool dominant = last.type == 0 && (last.tension == 2 || last.tension == 5 || (last.tension >= 8 && last.tension <= 13));
	if (dominant && d == 1) {
		return 2;
	}
	return ad <= 1 ? 3 : (ad == 2 ? 2 : (ad == 3 ? 1 : 0));
}

void
ChordPads::set_last (Chord const& c)
{
	_last     = c;
	_has_last = true;
	_grid->queue_draw ();
	_circle->queue_draw ();
}

bool
ChordPads::drag_chord (std::string const& data, std::vector<uint8_t>& notes, std::string& name) const
{
	notes.clear ();
	Chord c;
	if (parse_chord_data (data, c)) {
		for (int n : chord_pitches (c)) {
			notes.push_back ((uint8_t) n);
		}
		name = chord_name (c);
	} else if (!data.empty () && isdigit (data[0])) {
		const int pad = atoi (data.c_str ());
		notes         = pitches (pad);
		name          = this->name (pad);
	}
	return !notes.empty ();
}

void
ChordPads::sound (int pad)
{
	if (pad >= 0 && pad < (int) _pads.size ()) {
		play_chord (_pads[pad]);
	}
}

void
ChordPads::play_chord (Chord const& c)
{
	silence ();
	if (!_track) {
		return;
	}
	_sounding_ch = (uint8_t) std::max (0, std::min (15, _channel));
	for (int n : chord_pitches (c)) {
		uint8_t ev[3] = { (uint8_t) (0x90 | _sounding_ch), (uint8_t) n, 100 };
		_track->write_immediate_event (Evoral::MIDI_EVENT, 3, ev);
		_sounding.push_back ((uint8_t) n);
	}
}

bool
ChordPads::identify (std::vector<int> const& notes, Chord& out)
{
	if (notes.size () < 2) {
		return false;
	}
	std::set<int> pcs;
	int           lo = 128;
	for (int n : notes) {
		pcs.insert (((n % 12) + 12) % 12);
		lo = std::min (lo, n);
	}
	const int bass = lo % 12;
	/* fewest notes added first: plain triads before tensions; the bass as root before slash chords */
	int best = -1;
	for (int pass = 0; pass < 2; ++pass) {
		for (int tn = 0; tn < n_tensions; ++tn) {
			for (int ty = 0; ty < n_types; ++ty) {
				for (int r = 0; r < 12; ++r) {
					if ((pass == 0) != (r == bass)) {
						continue;
					}
					Chord         c { r, ty, tn, -1, 0, 0 };
					std::set<int> cp;
					for (int n : chord_pitches (c)) {
						cp.insert (n % 12);
					}
					if (cp == pcs) {
						c.bass = r == bass ? -1 : bass;
						c.octave = lo / 12 - 5;
						out      = c;
						return true;
					}
				}
			}
		}
	}
	(void) best;
	return false;
}

std::string
ChordPads::notes_markup (std::vector<int> const& notes)
{
	Chord c;
	if (identify (notes, c)) {
		return chord_markup (c);
	}
	std::string s;
	std::set<int> seen;
	for (int n : notes) {
		if (seen.insert (n % 12).second) {
			s += (s.empty () ? "" : " ") + signs (pc_name (n % 12));
		}
	}
	return s;
}

void
ChordPads::play_notes (std::vector<int> const& notes)
{
	silence ();
	if (!_track) {
		return;
	}
	_sounding_ch = (uint8_t) std::max (0, std::min (15, _channel));
	for (int n : notes) {
		uint8_t ev[3] = { (uint8_t) (0x90 | _sounding_ch), (uint8_t) n, 100 };
		_track->write_immediate_event (Evoral::MIDI_EVENT, 3, ev);
		_sounding.push_back ((uint8_t) n);
	}
	Chord c;
	if (identify (notes, c)) {
		set_last (c);
	}
}

void
ChordPads::silence ()
{
	_stop_timer.disconnect ();
	if (_track) {
		for (uint8_t n : _sounding) {
			uint8_t ev[3] = { (uint8_t) (0x80 | _sounding_ch), n, 0 };
			_track->write_immediate_event (Evoral::MIDI_EVENT, 3, ev);
		}
	}
	_sounding.clear ();
}
