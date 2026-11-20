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
#include <set>

#include <glibmm/main.h>

#include "ardour/midi_track.h"

#include "widgets/tooltips.h"

#include "chord_pads.h"

#include "pbd/i18n.h"

using namespace ArdourWidgets;

namespace {

const char* const note_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

struct ChordType {
	const char* label;
	int         iv[3];
	int         n;
};

/* the triad (or what stands for it) */
const ChordType types[] = {
	{ "Maj",  { 0, 4, 7 }, 3 },
	{ "Min",  { 0, 3, 7 }, 3 },
	{ "Dim",  { 0, 3, 6 }, 3 },
	{ "Aug",  { 0, 4, 8 }, 3 },
	{ "Sus2", { 0, 2, 7 }, 3 },
	{ "Sus4", { 0, 5, 7 }, 3 },
	{ "5",    { 0, 7, 0 }, 2 },
};
const int n_types = sizeof (types) / sizeof (types[0]);

struct Tension {
	const char* label;
	int         iv[3]; ///< added above the root
	int         n;
};

/* added notes: sevenths and the colours on top of them */
const Tension tensions[] = {
	{ "—", { 0, 0, 0 },   0 },
	{ "6",      { 9, 0, 0 },   1 },
	{ "7",      { 10, 0, 0 },  1 },
	{ "maj7",   { 11, 0, 0 },  1 },
	{ "dim7",   { 9, 0, 0 },   1 },
	{ "9",      { 10, 14, 0 }, 2 },
	{ "maj9",   { 11, 14, 0 }, 2 },
	{ "add9",   { 14, 0, 0 },  1 },
	{ "11",     { 10, 14, 17 },3 },
	{ "13",     { 10, 14, 21 },3 },
	{ "7b9",    { 10, 13, 0 }, 2 },
	{ "7#9",    { 10, 15, 0 }, 2 },
	{ "7#11",   { 10, 18, 0 }, 2 },
	{ "7b13",   { 10, 20, 0 }, 2 },
	{ "6/9",    { 9, 14, 0 },  2 },
};
const int n_tensions = sizeof (tensions) / sizeof (tensions[0]);

const struct { const char* label; int64_t ticks; } lengths[] = {
	{ "1/1", 7680 }, { "1/2", 3840 }, { "1/4", 1920 }, { "1/8", 960 },
};
const int n_lengths = sizeof (lengths) / sizeof (lengths[0]);

std::string
note_name (int n)
{
	return std::string (note_names[n % 12]) + std::to_string (n / 12 - 1);
}

ArdourButton*
small_button (std::string const& label)
{
	ArdourButton* b = manage (new ArdourButton (label));
	b->set_name ("tab button");
	b->set_corner_radius (2);
	return b;
}

} // namespace

/* a diatonic set in C to start with: I ii iii IV V vi V7 vii-half-diminished */
std::vector<ChordPads::Chord> ChordPads::_pads = {
	{ 0, 0, 0, -1, 0, 0 }, { 2, 1, 0, -1, 0, 0 }, { 4, 1, 0, -1, 0, 0 }, { 5, 0, 0, -1, 0, 0 },
	{ 7, 0, 0, -1, 0, -1 }, { 9, 1, 0, -1, 0, -1 }, { 7, 0, 2, -1, 0, -1 }, { 11, 2, 2, -1, 0, -1 },
};
int ChordPads::_len = 0;

ChordPads::ChordPads ()
	: _sel (0)
	, _sounding_ch (0)
	, _channel (0)
	, _oct_down ("−")
	, _oct_up ("+")
	, _insert_button (_("Insert at Playhead"))
{
	std::vector<Gtk::TargetEntry> targets;
	targets.push_back (Gtk::TargetEntry (drag_target (), Gtk::TARGET_SAME_APP));

	/* the pads */
	for (int i = 0; i < n_pads; ++i) {
		ArdourButton* b = manage (new ArdourButton ());
		b->set_name ("generic button");
		b->set_corner_radius (4);
		b->set_size_request (92, 42);
		b->set_fixed_colors (0x3d7fc4ff, 0x34373cff); /* selected: blue */
		ArdourWidgets::set_tooltip (*b, _("Hold to hear the chord, drag it into the notes to write it there"));
		b->signal_button_press_event ().connect (sigc::bind (sigc::mem_fun (*this, &ChordPads::pad_press), i), false);
		b->signal_button_release_event ().connect (sigc::mem_fun (*this, &ChordPads::pad_release), false);
		b->drag_source_set (targets, Gdk::BUTTON1_MASK, Gdk::ACTION_COPY);
		b->signal_drag_begin ().connect ([this, i] (Glib::RefPtr<Gdk::DragContext> const&) {
			silence ();
			select_pad (i);
		});
		b->signal_drag_data_get ().connect ([i] (Glib::RefPtr<Gdk::DragContext> const&, Gtk::SelectionData& data, guint, guint) {
			std::string const s = std::to_string (i);
			data.set (drag_target (), 8, (const guint8*) s.c_str (), s.size ());
		});
		_pad_box.pack_start (*b, true, true);
		_pad_buttons.push_back (b);
	}
	_pad_box.set_spacing (4);

	/* the chord editor of the selected pad: one row per property */
	auto row = [this] (const char* text) {
		Gtk::HBox*  h = manage (new Gtk::HBox ());
		Gtk::Label* l = manage (new Gtk::Label (text));
		l->set_alignment (1.0, 0.5);
		l->set_size_request (58, -1);
		h->pack_start (*l, false, false, 4);
		h->set_spacing (2);
		_editor.pack_start (*h, false, false);
		return h;
	};
	auto add = [] (Gtk::HBox* h, ArdourButton* b) {
		b->set_size_request (44, -1);
		h->pack_start (*b, false, false);
	};

	Gtk::HBox* h = row (_("Root"));
	for (int i = 0; i < 12; ++i) {
		ArdourButton* b = small_button (note_names[i]);
		b->signal_clicked.connect ([this, i] () { edit (0, i); });
		add (h, b);
		_root_buttons.push_back (b);
	}
	h = row (_("Type"));
	for (int i = 0; i < n_types; ++i) {
		ArdourButton* b = small_button (types[i].label);
		b->signal_clicked.connect ([this, i] () { edit (1, i); });
		add (h, b);
		_type_buttons.push_back (b);
	}
	h = row (_("Tension"));
	for (int i = 0; i < n_tensions; ++i) {
		ArdourButton* b = small_button (tensions[i].label);
		b->signal_clicked.connect ([this, i] () { edit (2, i); });
		add (h, b);
		_tension_buttons.push_back (b);
	}
	h = row (_("Bass"));
	for (int i = -1; i < 12; ++i) {
		ArdourButton* b = small_button (i < 0 ? std::string ("\u2014") : std::string (note_names[i]));
		if (i < 0) {
			ArdourWidgets::set_tooltip (*b, _("No separate bass note"));
		}
		b->signal_clicked.connect ([this, i] () { edit (3, i); });
		add (h, b);
		_bass_buttons.push_back (b);
	}
	h = row (_("Voicing"));
	{
		const char* inv[] = { _("Root"), _("1st Inv"), _("2nd Inv"), _("3rd Inv") };
		for (int i = 0; i < 4; ++i) {
			ArdourButton* b = small_button (inv[i]);
			b->signal_clicked.connect ([this, i] () { edit (4, i); });
			add (h, b);
			b->set_size_request (56, -1);
			_inversion_buttons.push_back (b);
		}
		Gtk::Label* l = manage (new Gtk::Label (_("Octave")));
		h->pack_start (*l, false, false, 8);
		for (ArdourButton* b : { &_oct_down, &_oct_up }) {
			b->set_name ("tab button");
			b->set_corner_radius (2);
		}
		_oct_down.signal_clicked.connect ([this] () { edit (5, -1); });
		_oct_up.signal_clicked.connect ([this] () { edit (5, 1); });
		_oct_label.set_size_request (28, -1);
		add (h, &_oct_down);
		h->pack_start (_oct_label, false, false);
		add (h, &_oct_up);
	}
	h = row (_("Length"));
	for (int i = 0; i < n_lengths; ++i) {
		ArdourButton* b = small_button (lengths[i].label);
		b->signal_clicked.connect ([this, i] () {
			_len = i;
			update ();
		});
		add (h, b);
		_length_buttons.push_back (b);
	}
	_insert_button.set_name ("generic button");
	_insert_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (_insert_button, _("Write the selected chord at the playhead"));
	_insert_button.signal_clicked.connect ([this] () { InsertRequested (_sel); });
	h->pack_start (_insert_button, false, false, 12);
	_notes_label.set_alignment (0.0, 0.5);
	h->pack_start (_notes_label, false, false, 4);
	_editor.set_spacing (2);

	pack_start (_pad_box, false, false);
	pack_start (_editor, false, false);
	set_spacing (6);
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
	ChordType const& t  = types[std::max (0, std::min (n_types - 1, c.type))];
	Tension const&   tn = tensions[std::max (0, std::min (n_tensions - 1, c.tension))];
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
	std::string s    = note_names[c.root];

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
		s += std::string ("/") + note_names[c.bass];
	}
	return s;
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
	sound (_sel);
	_stop_timer.disconnect ();
	_stop_timer = Glib::signal_timeout ().connect ([this] () { silence (); return false; }, 700);
}

void
ChordPads::update ()
{
	for (int i = 0; i < (int) _pad_buttons.size (); ++i) {
		_pad_buttons[i]->set_text (chord_name (_pads[i]));
		_pad_buttons[i]->set_active (i == _sel);
	}
	Chord const& c = _pads[_sel];
	for (int i = 0; i < (int) _root_buttons.size (); ++i) {
		_root_buttons[i]->set_active (i == c.root);
	}
	for (int i = 0; i < (int) _type_buttons.size (); ++i) {
		_type_buttons[i]->set_active (i == c.type);
	}
	for (int i = 0; i < (int) _tension_buttons.size (); ++i) {
		_tension_buttons[i]->set_active (i == c.tension);
	}
	for (int i = 0; i < (int) _bass_buttons.size (); ++i) {
		_bass_buttons[i]->set_active (i - 1 == c.bass);
	}
	for (int i = 0; i < (int) _inversion_buttons.size (); ++i) {
		_inversion_buttons[i]->set_active (i == c.inversion);
	}
	for (int i = 0; i < (int) _length_buttons.size (); ++i) {
		_length_buttons[i]->set_active (i == _len);
	}
	_oct_label.set_text (c.octave > 0 ? "+" + std::to_string (c.octave) : std::to_string (c.octave));

	std::string notes;
	for (int n : chord_pitches (c)) {
		notes += (notes.empty () ? "" : "  ") + note_name (n);
	}
	_notes_label.set_text (notes);
}

bool
ChordPads::pad_press (GdkEventButton* ev, int pad)
{
	if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) {
		return false;
	}
	select_pad (pad);
	sound (pad);
	_stop_timer.disconnect ();
	/* in case the release never comes (a drag took it) */
	_stop_timer = Glib::signal_timeout ().connect ([this] () { silence (); return false; }, 8000);
	return false;
}

bool
ChordPads::pad_release (GdkEventButton* ev)
{
	if (ev->button == 1) {
		silence ();
	}
	return false;
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
