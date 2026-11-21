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

const int major_scale[7] = { 0, 2, 4, 5, 7, 9, 11 };
const int minor_scale[7] = { 0, 2, 3, 5, 7, 8, 10 };

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

} // namespace

/* ------------------------------------------------------------------------------------------------------ */

/** The pads: 4 x 2 tiles with the chord name, its degree in the key and its notes */
class ChordPads::PadGrid : public Gtk::DrawingArea
{
public:
	PadGrid (ChordPads& o) : _o (o), _hover (-1), _press (-1)
	{
		add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK);
		set_size_request (420, 104);
		std::vector<Gtk::TargetEntry> targets;
		targets.push_back (Gtk::TargetEntry (drag_target (), Gtk::TARGET_SAME_APP));
		drag_source_set (targets, Gdk::BUTTON1_MASK, Gdk::ACTION_COPY);
	}

	int pad_at (double x, double y) const
	{
		const double pw = (get_width () - gap * 3) / 4.0;
		const double ph = (get_height () - gap) / 2.0;
		const int    c  = (int) std::floor (x / (pw + gap));
		const int    r  = (int) std::floor (y / (ph + gap));
		if (c < 0 || c > 3 || r < 0 || r > 1 || x - c * (pw + gap) > pw || y - r * (ph + gap) > ph) {
			return -1;
		}
		return r * 4 + c;
	}

protected:
	bool on_expose_event (GdkEventExpose*)
	{
		cairo_t*     cr = gdk_cairo_create (get_window ()->gobj ());
		const double pw = (get_width () - gap * 3) / 4.0;
		const double ph = (get_height () - gap) / 2.0;
		for (int i = 0; i < n_pads; ++i) {
			const double x   = (i % 4) * (pw + gap);
			const double y   = (i / 4) * (ph + gap);
			const bool   sel = i == _o._sel;
			Chord const& c   = _o._pads[i];
			rounded (cr, x, y, pw, ph, 6);
			set_rgb (cr, sel ? 0x3d7fc4 : (i == _hover ? 0x454a52 : 0x383c43));
			cairo_fill_preserve (cr);
			set_rgb (cr, sel ? 0x8ab8ea : 0x4b5058);
			cairo_set_line_width (cr, 1);
			cairo_stroke (cr);
			if (i == _press) {
				rounded (cr, x + 2, y + 2, pw - 4, ph - 4, 5);
				set_rgb (cr, 0xffffff, 0.12);
				cairo_fill (cr);
			}

			text (cr, *this, chord_name (c), "Sans Bold 13", x + pw / 2, y + ph / 2 - 3, 0xffffff);
			std::string const deg = degree (c);
			if (!deg.empty ()) {
				text (cr, *this, deg, "Sans 8", x + 6, y + 9, sel ? 0xdbe9f8 : 0x9aa3ad, 0.0);
			}
			text (cr, *this, std::to_string (i + 1), "Sans 7", x + pw - 6, y + 9, sel ? 0xdbe9f8 : 0x6c737c, 1.0);
			std::string notes;
			for (int n : chord_pitches (c)) {
				notes += (notes.empty () ? "" : " ") + std::string (pc_name (n % 12));
			}
			text (cr, *this, notes, "Sans 7", x + pw / 2, y + ph - 9, sel ? 0xdbe9f8 : 0x9aa3ad);
		}
		cairo_destroy (cr);
		return true;
	}

	bool on_button_press_event (GdkEventButton* ev)
	{
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
		/* a small picture of the pad under the pointer */
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

private:
	ChordPads&       _o;
	int              _hover;
	int              _press;
	static const int gap = 6;
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
	ChordKeys () { set_size_request (-1, 30); }
	void set (std::vector<int> const& notes, bool with_bass)
	{
		_notes = notes;
		_bass  = with_bass;
		queue_draw ();
	}

protected:
	bool on_expose_event (GdkEventExpose*)
	{
		cairo_t* cr = gdk_cairo_create (get_window ()->gobj ());
		int      lo = 36, hi = 84; /* C2 .. B5 */
		if (!_notes.empty ()) {
			lo = std::min (lo, _notes.front () / 12 * 12);
			hi = std::max (hi, (_notes.back () / 12 + 1) * 12);
		}
		draw_keys (cr, 0, 0, get_width (), get_height () - 1, lo, hi, [this] (int n) -> uint32_t {
			for (size_t i = 0; i < _notes.size (); ++i) {
				if (_notes[i] == n) {
					return (i == 0 && _bass) ? orange : blue;
				}
			}
			return 0;
		});
		/* where middle C is */
		int whites = 0;
		for (int n = lo; n < 60; ++n) {
			whites += is_black (n) ? 0 : 1;
		}
		int all = 0;
		for (int n = lo; n < hi; ++n) {
			all += is_black (n) ? 0 : 1;
		}
		const double kw = get_width () / (double) std::max (1, all);
		text (cr, *this, "C4", "Sans 6", whites * kw + kw / 2, get_height () - 7, 0x6c737c);
		cairo_destroy (cr);
		return true;
	}

private:
	std::vector<int> _notes;
	bool             _bass = false;
};

/* ------------------------------------------------------------------------------------------------------ */

/* a diatonic set in C to start with: I ii iii IV V vi V7 vii-half-diminished */
std::vector<ChordPads::Chord> ChordPads::_pads = {
	{ 0, 0, 0, -1, 0, 0 }, { 2, 1, 0, -1, 0, 0 }, { 4, 1, 0, -1, 0, 0 }, { 5, 0, 0, -1, 0, 0 },
	{ 7, 0, 0, -1, 0, -1 }, { 9, 1, 0, -1, 0, -1 }, { 7, 0, 2, -1, 0, -1 }, { 11, 2, 2, -1, 0, -1 },
};
int  ChordPads::_len       = 0;
int  ChordPads::_key_root  = 0;
bool ChordPads::_key_minor = false;

const char*
ChordPads::pc_name (int pc)
{
	/* keys written with flats: F Bb Eb Ab Db (major), D G C F Bb (minor) */
	static const bool flat_major[12] = { false, true, false, true, false, true, false, false, true, false, true, false };
	static const bool flat_minor[12] = { true, false, true, false, false, true, false, true, false, false, true, false };
	const bool flats = _key_minor ? flat_minor[_key_root] : flat_major[_key_root];
	pc = ((pc % 12) + 12) % 12;
	return flats ? flat_names[pc] : note_names[pc];
}

ChordPads::ChordPads ()
	: _sel (0)
	, _sounding_ch (0)
	, _channel (0)
	, _grid (manage (new PadGrid (*this)))
	, _root_picker (manage (new KeyPicker (blue)))
	, _bass_picker (manage (new KeyPicker (orange)))
	, _chord_keys (manage (new ChordKeys ()))
	, _fill_button (_("Fill Pads"))
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
			update ();
		}));
	}
	_key_dropdown.set_name ("generic button");
	_key_dropdown.set_size_request (52, -1);
	top->pack_start (_key_dropdown, false, false);
	_scale_dropdown.add_menu_elem (Gtk::Menu_Helpers::MenuElem (_("Major"), [this] () { _key_minor = false; update (); }));
	_scale_dropdown.add_menu_elem (Gtk::Menu_Helpers::MenuElem (_("Minor"), [this] () { _key_minor = true; update (); }));
	_scale_dropdown.set_name ("generic button");
	_scale_dropdown.set_size_request (72, -1);
	top->pack_start (_scale_dropdown, false, false);
	_fill_button.set_name ("generic button");
	_fill_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_fill_button, _("Put the chords of the key on the pads: I ii iii IV V vi vii and V7 (minor: i ii III iv v VI VII V7)"));
	_fill_button.signal_clicked.connect (sigc::mem_fun (*this, &ChordPads::fill_from_key));
	top->pack_start (_fill_button, false, false, 4);
	Gtk::Label* hint = manage (new Gtk::Label ());
	hint->set_markup (std::string ("<span size='small' foreground='#9aa3ad'>")
	                  + _("Click a pad to hear and edit it · drag it onto the notes to write it there") + "</span>");
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
	_insert_button.set_name ("generic button");
	_insert_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_insert_button, _("Write the selected chord at the playhead"));
	_insert_button.signal_clicked.connect ([this] () { InsertRequested (_sel); });
	bottom->pack_start (_insert_button, false, false, 10);
	right->pack_start (*bottom, false, false);

	Gtk::HBox* main = manage (new Gtk::HBox ());
	main->set_spacing (14);
	main->pack_start (*left, true, true);
	main->pack_start (*right, false, false);

	pack_start (*top, false, false);
	pack_start (*main, false, false);
	set_spacing (4);
	set_border_width (4);
	show_all ();
	update ();
}

ChordPads::~ChordPads ()
{
	silence ();
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
ChordPads::degree (Chord const& c)
{
	static const char* const roman[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };
	int const* scale = _key_minor ? minor_scale : major_scale;
	const int  rel   = (c.root - _key_root + 12) % 12;
	int        d     = -1;
	for (int i = 0; i < 7; ++i) {
		if (scale[i] == rel) {
			d = i;
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
	return r;
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
	audition (_sel, 700);
}

void
ChordPads::fill_from_key ()
{
	/* type and tension per degree; the eighth pad is the dominant seventh (harmonic minor's V7 in minor) */
	static const int maj_type[7] = { 0, 1, 1, 0, 0, 1, 2 };
	static const int min_type[7] = { 1, 2, 0, 1, 1, 0, 0 };
	int const* scale = _key_minor ? minor_scale : major_scale;
	int const* type  = _key_minor ? min_type : maj_type;
	for (int d = 0; d < 7; ++d) {
		Chord& c    = _pads[d];
		c.root      = (_key_root + scale[d]) % 12;
		c.type      = type[d];
		c.tension   = type[d] == 2 ? 2 : 0; /* the diminished one as m7b5 */
		c.bass      = -1;
		c.inversion = 0;
		c.octave    = c.root >= 7 ? -1 : 0; /* roots between G3 and F#4 */
	}
	Chord& v7    = _pads[7];
	v7.root      = (_key_root + 7) % 12;
	v7.type      = 0;
	v7.tension   = 2;
	v7.bass      = -1;
	v7.inversion = 0;
	v7.octave    = v7.root >= 7 ? -1 : 0;
	update ();
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
	_scale_dropdown.set_text (_key_minor ? _("Minor") : _("Major"));
	_chord_keys->set (chord_pitches (c), c.bass >= 0);
	_grid->queue_draw ();
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
	/* sounds while held; the timer is for a release that never comes (a drag took it) */
	audition (pad, 8000);
}

void
ChordPads::pad_released ()
{
	silence ();
}

void
ChordPads::sound (int pad)
{
	silence ();
	if (!_track) {
		return;
	}
	_sounding_ch = (uint8_t) std::max (0, std::min (15, _channel));
	for (uint8_t n : pitches (pad)) {
		uint8_t ev[3] = { (uint8_t) (0x90 | _sounding_ch), n, 100 };
		_track->write_immediate_event (Evoral::MIDI_EVENT, 3, ev);
		_sounding.push_back (n);
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
