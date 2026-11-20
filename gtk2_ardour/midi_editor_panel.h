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
#include <ytkmm/scrolledwindow.h>

#include "widgets/ardour_button.h"

#include "score_view.h"

namespace ARDOUR {
	class MidiRegion;
	class MidiTrack;
	class Session;
}

class Pianoroll;

/** The MIDI editor: Key | Drum | Score views of the same region with their tool bar.
 *  Used by the MIDI editor window and by the editor's bottom pane, so both behave the same. */
class MidiEditorPanel : public Gtk::VBox
{
public:
	/** `expandable`: the pianoroll offers its size button (bottom pane) */
	MidiEditorPanel (std::string const& name, bool with_transport, bool expandable);
	~MidiEditorPanel ();

	enum EditorMode {
		KeyEditor,
		DrumEditor,
		ScoreEditor,
	};

	Pianoroll& pianoroll () { return *_pianoroll; }
	void set_session (ARDOUR::Session*);

	/** show `region` (added first); the view follows the track's note mode unless the score is shown */
	void set_region (std::shared_ptr<ARDOUR::MidiTrack>, std::shared_ptr<ARDOUR::MidiRegion>);
	void add_region (std::shared_ptr<ARDOUR::MidiTrack>, std::shared_ptr<ARDOUR::MidiRegion>);
	void replace_region (std::shared_ptr<ARDOUR::MidiTrack>, std::shared_ptr<ARDOUR::MidiRegion>);
	void remove_regions ();
	void set_show_source (bool);

	void       set_editor_mode (EditorMode);
	EditorMode editor_mode () const { return _mode; }

	/** score editing keys (note values, Delete, arrows, undo); true if used */
	bool handle_key (GdkEventKey*);

private:
	Gtk::HBox           mode_bar;
	Gtk::HBox           hpacker;
	Gtk::VBox           score_placeholder;
	Gtk::ScrolledWindow score_scroller;
	ScoreView           score_view;
	std::shared_ptr<ARDOUR::MidiRegion> _region;
	std::shared_ptr<ARDOUR::MidiTrack>  _track;
	Pianoroll*          _pianoroll;
	EditorMode          _mode;

	ArdourWidgets::ArdourButton key_button;
	ArdourWidgets::ArdourButton drum_button;
	ArdourWidgets::ArdourButton score_button;
	ArdourWidgets::ArdourButton all_drums_button;
	ArdourWidgets::ArdourButton drum_notation_button;

	/* score input: select, or the note value that a click adds */
	Gtk::HBox note_value_box;
	std::vector<std::pair<ArdourWidgets::ArdourButton*, int64_t>> note_value_buttons;
	ArdourWidgets::ArdourButton dot_button;
	ArdourWidgets::ArdourButton triplet_button;
	ArdourWidgets::ArdourButton quint_button;
	int64_t _note_base;    ///< picked value, 0: select
	bool    _note_dot;
	bool    _note_triplet;
	bool    _note_quint;
	/** pick a note value; with notes selected it also sets their length */
	void set_note_value (int64_t ticks);
	void apply_note_value (bool to_selection);

	bool score_focus_in (GdkEventFocus*);
	bool score_focus_out (GdkEventFocus*);
	bool score_key_press (GdkEventKey*);
	bool _magic_focus;
};
