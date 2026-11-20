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

namespace ARDOUR {
	class MidiTrack;
}

/** Chord pads (like Cubase's): a row of pads, each holding a chord built from a root, a chord type, a
 *  tension and a bass note, with an inversion and an octave. Holding a pad plays the chord on the track's
 *  instrument, dragging it onto the note editor writes it there. The pads are shared by all MIDI editors. */
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

	/** drag and drop target of a pad; the data is the pad number */
	static const char* drag_target () { return "application/x-jxmpose-chord-pad"; }

	static const int n_pads = 8;

private:
	struct Chord {
		int root;      ///< 0 = C .. 11 = B
		int type;      ///< index into the chord types
		int tension;   ///< index into the tensions, 0 = none
		int bass;      ///< -1 = the root, else 0 .. 11
		int inversion; ///< 0 = root position
		int octave;    ///< offset from the octave of middle C
	};
	static std::vector<Chord> _pads;
	static int                _len; ///< index into the lengths

	int                                         _sel;
	std::shared_ptr<ARDOUR::MidiTrack>          _track;
	std::vector<uint8_t>                        _sounding;
	uint8_t                                     _sounding_ch;
	int                                         _channel;
	sigc::connection                            _stop_timer;

	Gtk::HBox                                   _pad_box;
	std::vector<ArdourWidgets::ArdourButton*>   _pad_buttons;
	Gtk::VBox                                   _editor;
	std::vector<ArdourWidgets::ArdourButton*>   _root_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _type_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _tension_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _bass_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _inversion_buttons;
	std::vector<ArdourWidgets::ArdourButton*>   _length_buttons;
	ArdourWidgets::ArdourButton                 _oct_down;
	ArdourWidgets::ArdourButton                 _oct_up;
	Gtk::Label                                  _oct_label;
	ArdourWidgets::ArdourButton                 _insert_button;
	Gtk::Label                                  _notes_label;

	static std::vector<int> chord_pitches (Chord const&);
	static std::string      chord_name (Chord const&);

	void select_pad (int);
	void edit (int what, int value); ///< what: 0 root, 1 type, 2 tension, 3 bass, 4 inversion, 5 octave step
	void update ();

	bool pad_press (GdkEventButton*, int pad);
	bool pad_release (GdkEventButton*);
	void sound (int pad);
	void silence ();
};
