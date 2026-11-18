/*
 * Copyright (C) 2025 Paul Davis <paul@linuxaudiosystems.com>
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

#include "pbd/compose.h"

#include "ardour/midi_region.h"
#include "ardour/midi_track.h"

#include "gtkmm2ext/doi.h"
#include "gtkmm2ext/keyboard.h"

#include "ardour_ui.h"
#include "chord_box.h"
#include "pianoroll.h"
#include "pianoroll_window.h"
#include "region_editor.h"

#include "widgets/tooltips.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

PianorollWindow::PianorollWindow (std::string const & name, Session& s)
	: ArdourWindow (string_compose ("%1 - %2", PROGRAM_NAME, name))
	, pianoroll (new Pianoroll (name, true))
	, region_editor (nullptr)
	, _mode (KeyEditor)
	, key_button (_("Key Editor"))
	, drum_button (_("Drum Editor"))
	, score_button (_("Score Editor"))
	, all_drums_button (_("All Drums"))
	, drum_notation_button (_("Drum Notation"))
	, dot_button (".")
	, triplet_button ("3")
	, quint_button ("5")
	, _note_base (0)
	, _note_dot (false)
	, _note_triplet (false)
	, _note_quint (false)
{
	pianoroll->set_session (&s);
	set_default_size (1270, 700);

	hpacker.pack_start (pianoroll->contents(), true, true);
	pianoroll->contents().show ();
	pianoroll->hide_note_mode_button ();

	/* editor switch: three views of the same MIDI data */
	struct { ArdourWidgets::ArdourButton* b; EditorMode m; const char* tip; } modes[] = {
		{ &key_button,   KeyEditor,   _("Notes on a piano keyboard grid") },
		{ &drum_button,  DrumEditor,  _("Drum hits: one row per drum sound, notes as fixed-length hits") },
		{ &score_button, ScoreEditor, _("Music notation of the region (view only for now; Ctrl+wheel to zoom)") },
	};
	for (auto const& m : modes) {
		m.b->set_name ("tab button");
		m.b->set_corner_radius (2);
		ArdourWidgets::set_tooltip (*m.b, m.tip);
		EditorMode const mode = m.m;
		m.b->signal_clicked.connect ([this, mode] () { set_editor_mode (mode); });
		mode_bar.pack_start (*m.b, false, false);
	}
	/* drum view: used sounds only, or the whole drum map */
	all_drums_button.set_name ("tab button");
	all_drums_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (all_drums_button, _("Show every drum sound of the instrument, not only the ones used in this pattern"));
	all_drums_button.signal_clicked.connect ([this] () {
		bool const yn = !all_drums_button.get_active ();
		all_drums_button.set_active (yn);
		pianoroll->set_show_all_drums (yn);
	});
	mode_bar.pack_end (all_drums_button, false, false);

	/* score: pitched grand staff or drum notation */
	drum_notation_button.set_name ("tab button");
	drum_notation_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (drum_notation_button, _("Write the region as drum notation (one staff, kick and pedal hi-hat stems down, cymbals with x note heads)"));
	drum_notation_button.signal_clicked.connect ([this] () {
		bool const yn = !drum_notation_button.get_active ();
		drum_notation_button.set_active (yn);
		score_view.set_percussion (yn);
	});
	mode_bar.pack_end (drum_notation_button, false, false);

	/* score input */
	struct { const char* label; int64_t ticks; const char* tip; } values[] = {
		{ _("Select"), 0,    _("Select and drag notes; Delete removes, Up/Down transposes (Ctrl: octave) (key: 0)") },
		{ "1/1",       7680, _("Whole notes: click into the staff to add, or set the length of the selected notes (key: 8)") },
		{ "1/2",       3840, _("Half notes (key: 7)") },
		{ "1/4",       1920, _("Quarter notes (key: 6)") },
		{ "1/8",       960,  _("Eighth notes (key: 5)") },
		{ "1/16",      480,  _("Sixteenth notes (key: 4)") },
	};
	for (auto const& v : values) {
		ArdourWidgets::ArdourButton* b = manage (new ArdourWidgets::ArdourButton (v.label));
		b->set_name ("tab button");
		b->set_corner_radius (2);
		ArdourWidgets::set_tooltip (*b, v.tip);
		int64_t const t = v.ticks;
		b->signal_clicked.connect ([this, t] () { set_note_value (t); });
		note_value_box.pack_start (*b, false, false);
		note_value_buttons.push_back (std::make_pair (b, v.ticks));
	}
	dot_button.set_name ("tab button");
	dot_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (dot_button, _("Dotted note value (key: .)"));
	dot_button.signal_clicked.connect ([this] () { _note_dot = !_note_dot; if (_note_dot) { _note_triplet = _note_quint = false; } apply_note_value (true); });
	note_value_box.pack_start (dot_button, false, false, 4);
	triplet_button.set_name ("tab button");
	triplet_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (triplet_button, _("Triplets: quarter, eighth and sixteenth notes as triplets (key: ;)"));
	triplet_button.signal_clicked.connect ([this] () { _note_triplet = !_note_triplet; if (_note_triplet) { _note_dot = _note_quint = false; } apply_note_value (true); });
	note_value_box.pack_start (triplet_button, false, false);
	quint_button.set_name ("tab button");
	quint_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (quint_button, _("Quintuplets: five sixteenth notes in a beat"));
	quint_button.signal_clicked.connect ([this] () { _note_quint = !_note_quint; if (_note_quint) { _note_dot = _note_triplet = false; } apply_note_value (true); });
	note_value_box.pack_start (quint_button, false, false);
	note_value_box.set_spacing (2);
	mode_bar.pack_start (note_value_box, false, false, 12);
	apply_note_value (false);
	mode_bar.set_spacing (2);
	mode_bar.set_border_width (3);

	/* score: notation of the region (read-only for now); Ctrl+wheel zooms */
	score_scroller.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	score_scroller.add (score_view);
	score_placeholder.pack_start (score_scroller, true, true);

	vpacker.pack_start (mode_bar, false, false);
	vpacker.pack_start (hpacker, true, true);
	vpacker.pack_start (score_placeholder, true, true);

	add (vpacker);
	vpacker.show_all ();
	/* visibility of the two views is managed by set_editor_mode () */
	score_placeholder.set_no_show_all ();
	score_placeholder.hide ();
	hpacker.set_no_show_all ();
	all_drums_button.set_no_show_all ();
	all_drums_button.hide ();
	drum_notation_button.set_no_show_all ();
	drum_notation_button.hide ();
	note_value_box.set_no_show_all ();
	note_value_box.hide ();
	key_button.set_active (true);
}

void
PianorollWindow::set_editor_mode (EditorMode m)
{
	_mode = m;
	key_button.set_active (m == KeyEditor);
	drum_button.set_active (m == DrumEditor);
	score_button.set_active (m == ScoreEditor);
	if (m == DrumEditor) {
		all_drums_button.show ();
	} else {
		all_drums_button.hide ();
	}
	if (m == ScoreEditor) {
		/* drums (drum view or MIDI channel 10) start as drum notation */
		bool const drums = pianoroll->note_mode () == ARDOUR::Percussive || pianoroll->visible_channel () == 9;
		drum_notation_button.set_active (drums);
		score_view.set_percussion (drums);
		score_view.set_history (&pianoroll->history ());
		score_view.set_channel (pianoroll->visible_channel ());
		drum_notation_button.show ();
		note_value_box.show ();
	} else {
		drum_notation_button.hide ();
		note_value_box.hide ();
	}

	if (m == ScoreEditor) {
		hpacker.hide ();
		score_view.set_region (_region);
		score_scroller.show_all ();
		score_placeholder.show ();
		return;
	}

	score_placeholder.hide ();
	hpacker.show ();

	/* the drum editor shows notes as hits (percussive note mode) */
	ARDOUR::NoteMode const want = (m == DrumEditor) ? ARDOUR::Percussive : ARDOUR::Sustained;
	if (pianoroll->note_mode () != want) {
		pianoroll->note_mode_clicked ();
	}
}

PianorollWindow::~PianorollWindow ()
{
	delete pianoroll;
	delete region_editor;
}

void
PianorollWindow::set_show_source (bool yn)
{
	pianoroll->set_show_source (yn);
}

void
PianorollWindow::add_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	pianoroll->add_region (region, track);
}

void
PianorollWindow::set_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	pianoroll->add_region (region, track);
	pianoroll->set_region (region);
	_region = region;
	_track  = track;
	score_view.set_track (track);
	if (_mode == ScoreEditor) {
		score_view.set_region (region);
	}

	/* follow the track's note mode (drum tracks open in the drum editor) */
	if (_mode != ScoreEditor) {
		set_editor_mode (pianoroll->note_mode () == ARDOUR::Percussive ? DrumEditor : KeyEditor);
	}

	set_title (string_compose (_("%1 Pianoroll: %2"), PROGRAM_NAME, region->name()));
}

void
PianorollWindow::replace_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	pianoroll->replace_region (region, track);
	_region = region;
	if (_mode == ScoreEditor) {
		score_view.set_region (region);
	}
}

void
PianorollWindow::remove_regions ()
{
	pianoroll->remove_regions ();
}

void
PianorollWindow::set_note_value (int64_t ticks)
{
	_note_base = ticks;
	apply_note_value (true);
}

void
PianorollWindow::apply_note_value (bool to_selection)
{
	int64_t len = _note_base;
	if (len && _note_dot) {
		len = len * 3 / 2;
	} else if (len && _note_quint && len == 480) {
		len = 384;
	} else if (len && _note_triplet && (len == 1920 || len == 960 || len == 480)) {
		len = len * 2 / 3;
	}
	score_view.set_insert_length (len);
	for (auto const& b : note_value_buttons) {
		b.first->set_active (b.second == _note_base);
	}
	dot_button.set_active (_note_dot);
	triplet_button.set_active (_note_triplet);
	quint_button.set_active (_note_quint);

	/* Dorico-style: picking a value with notes selected changes their length */
	if (to_selection && len > 0 && score_view.has_selection ()) {
		score_view.set_selection_length (len);
	}
}

bool
PianorollWindow::on_key_press_event (GdkEventKey* ev)
{
	/* score editing keys first (Delete, arrows, Escape) */
	if (_mode == ScoreEditor) {
		if (score_view.handle_key (ev)) {
			return true;
		}
		/* note values like in notation programs: 4 = 16th ... 8 = whole */
		if (!(ev->state & (Gtkmm2ext::Keyboard::PrimaryModifier | Gtkmm2ext::Keyboard::SecondaryModifier))) {
			switch (ev->keyval) {
			case GDK_4: set_note_value (480);  return true;
			case GDK_5: set_note_value (960);  return true;
			case GDK_6: set_note_value (1920); return true;
			case GDK_7: set_note_value (3840); return true;
			case GDK_8: set_note_value (7680); return true;
			case GDK_0: set_note_value (0);    return true;
			case GDK_period:
				_note_dot = !_note_dot;
				if (_note_dot) { _note_triplet = _note_quint = false; }
				apply_note_value (true);
				return true;
			case GDK_semicolon:
				_note_triplet = !_note_triplet;
				if (_note_triplet) { _note_dot = _note_quint = false; }
				apply_note_value (true);
				return true;
			default:
				break;
			}
		}
		/* undo/redo the region edits here, not in the main editor */
		if (ev->state & Gtkmm2ext::Keyboard::PrimaryModifier) {
			if (ev->keyval == GDK_z || ev->keyval == GDK_Z) {
				if (ev->state & Gtkmm2ext::Keyboard::TertiaryModifier) {
					pianoroll->redo ();
				} else {
					pianoroll->undo ();
				}
				return true;
			}
			if (ev->keyval == GDK_y || ev->keyval == GDK_Y) {
				pianoroll->redo ();
				return true;
			}
		}
	}
	return ARDOUR_UI::instance()->key_event_handler (ev, this);
}

bool
PianorollWindow::on_delete_event (GdkEventAny*)
{
	hide ();
	return true;
}
