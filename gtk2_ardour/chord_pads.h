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

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <ytkmm/box.h>
#include <ytkmm/label.h>

#include "temporal/beats.h"

#include "widgets/ardour_button.h"
#include "widgets/ardour_dropdown.h"

namespace ARDOUR {
	class MidiTrack;
}

/** Chord pads (like Cubase's): a grid of pads, each holding a chord built from a root, a chord type, a
 *  tension and a bass note, with an inversion and an octave. Holding a pad plays the chord on the track's
 *  instrument, dragging it onto the note editor writes it there. A key fills the pads with its chords and
 *  names them by degree (major, minor, the modes and more scales). The pads are shared by all MIDI editors. */
class ArdourWindow;

class ChordPads : public Gtk::VBox
{
public:
	ChordPads ();
	~ChordPads ();

	void set_track (std::shared_ptr<ARDOUR::MidiTrack>);
	void set_channel (int c) { _channel = c; } ///< MIDI channel the pads play and write (0 .. 15)
	int  channel () const { return _channel; }

	/** MIDI notes of a pad, lowest first */
	std::vector<uint8_t> pitches (int pad) const;
	std::string          name (int pad) const;
	Temporal::Beats      length () const;
	int                  selected () const { return _sel; }

	/** "Insert at Playhead" was clicked with this pad selected */
	sigc::signal<void, int> InsertRequested;

	/** the key of the pads (root 0 .. 11, scale index); set_key fills the pads when it changes */
	int  key_root () const { return _key_root; }
	int  key_scale () const { return _scale; }
	void set_key (int root, int scale);
	/** the pitch classes of the key's scale, bit n = pitch class n */
	uint16_t key_pitch_classes () const;
	/** the key was changed with the Key or Scale menu */
	sigc::signal<void> KeyChanged;

	/** drag and drop target of a pad or a chord of the circle of fifths; the data is the pad number, or
	 *  "c<root>:<type>:<tension>:<octave>" for a chord of the circle */
	static const char* drag_target () { return "application/x-jxmpose-chord-pad"; }
	/** notes and name of dragged chord data; false if it is not a chord */
	bool drag_chord (std::string const& data, std::vector<uint8_t>& notes, std::string& name) const;

	static const int n_pads = 16; ///< 9 in the lower row, 7 above them (like Cubase)

	struct Chord {
		int root;      ///< 0 = C .. 11 = B
		int type;      ///< index into the chord types
		int tension;   ///< index into the tensions, 0 = none
		int bass;      ///< -1 = the root, else 0 .. 11
		int inversion; ///< 0 = root position
		int octave;    ///< offset from the octave of middle C
	};
	static std::vector<int> chord_pitches (Chord const&);
	static std::string      chord_name (Chord const&);
	/** the name as pango markup, Cubase style (B\u266dmin<sup>7</sup>) */
	static std::string      chord_markup (Chord const&, int spelling = -1); ///< spelling: -1 the key's, 0 sharps, 1 flats
	/** degree in the current key (I, ii, V7 ...), empty outside it */
	static std::string      degree (Chord const&);
	static Chord const&     pad (int i) { return _pads[i]; }
	/** name of a pitch class, with flats in flat keys (Bb, Eb ...) and sharps otherwise */
	static const char*      pc_name (int pc);

	/** how well `next` follows `last` by the circle of fifths: 3 best (a step around the circle or its
	 *  relative), 2 good (two steps), 1 possible (three), 0 far; -1 the same chord */
	static int fit (Chord const& last, Chord const& next);

	/** the chord some notes form (any voicing, a bass note other than the root becomes a slash chord);
	 *  false if they are no known chord */
	static bool identify (std::vector<int> const& notes, Chord&);
	/** a short name of some notes: the chord's markup, else the note names */
	static std::string notes_markup (std::vector<int> const& notes);

	/** sound notes on the track (until silence ()), and let the circle follow the chord they form */
	void play_notes (std::vector<int> const& notes);
	void silence ();

	class PadGrid;
	class KeyPicker;
	class ChordKeys;
	class Circle;

private:
	static std::vector<Chord> _pads;
	static int                _len;       ///< index into the lengths
	static int                _key_root;  ///< key of the pads (for Fill Pads and the degrees)
	static int                _scale;     ///< index into the scales

	int                                         _sel;
	std::shared_ptr<ARDOUR::MidiTrack>          _track;
	std::vector<uint8_t>                        _sounding;
	uint8_t                                     _sounding_ch;
	int                                         _channel;
	sigc::connection                            _stop_timer;

	Chord                                       _last;     ///< the chord heard last (pad or circle)
	bool                                        _has_last;
	void set_last (Chord const&);

	PadGrid*                                    _grid;
	Circle*                                     _circle;
	KeyPicker*                                  _root_picker;
	KeyPicker*                                  _bass_picker;
	ChordKeys*                                  _chord_keys;
	ArdourWidgets::ArdourDropdown               _key_dropdown;
	ArdourWidgets::ArdourDropdown               _scale_dropdown;
	ArdourWidgets::ArdourButton                 _fill_button;
	ArdourWidgets::ArdourButton                 _edit_button;
	Gtk::VBox                                   _editor_box;
	ArdourWindow*                               _editor_window; /* the chord editor floats: under the pads it squeezed them */
	void show_editor (bool);
	ArdourWidgets::ArdourButton                 _no_bass_button;
	std::vector<ArdourWidgets::ArdourButton*>   _type_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _tension_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _inversion_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _length_buttons;
	ArdourWidgets::ArdourButton                 _oct_down;
	ArdourWidgets::ArdourButton                 _oct_up;
	Gtk::Label                                  _oct_label;
	ArdourWidgets::ArdourButton                 _insert_button;
	Gtk::Label                                  _title;

	void select_pad (int);
	void edit (int what, int value); ///< what: 0 root, 1 type, 2 tension, 3 bass, 4 inversion, 5 octave step
	void fill_from_key ();
	void update ();
	void audition (int pad, int ms);

	void pad_pressed (int pad);
	void pad_released ();
	void sound (int pad);
	void play_chord (Chord const&);
};
