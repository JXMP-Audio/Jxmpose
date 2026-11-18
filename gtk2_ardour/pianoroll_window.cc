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
{
	pianoroll->set_session (&s);
	set_default_size (1270, 700);

	hpacker.pack_start (pianoroll->contents(), true, true);
	pianoroll->contents().show ();

	/* editor switch: three views of the same MIDI data */
	struct { ArdourWidgets::ArdourButton* b; EditorMode m; const char* tip; } modes[] = {
		{ &key_button,   KeyEditor,   _("Notes on a piano keyboard grid") },
		{ &drum_button,  DrumEditor,  _("Drum hits: one row per drum sound, notes as fixed-length hits") },
		{ &score_button, ScoreEditor, _("Music notation (in development)") },
	};
	for (auto const& m : modes) {
		m.b->set_name ("tab button");
		m.b->set_corner_radius (2);
		ArdourWidgets::set_tooltip (*m.b, m.tip);
		EditorMode const mode = m.m;
		m.b->signal_clicked.connect ([this, mode] () { set_editor_mode (mode); });
		mode_bar.pack_start (*m.b, false, false);
	}
	mode_bar.set_spacing (2);
	mode_bar.set_border_width (3);

	score_label.set_markup (string_compose ("<span size=\"large\"><b>%1</b></span>\n\n%2",
	                                        _("Score Editor"),
	                                        _("Music notation view of this MIDI region is in development.\nUse the Key or Drum editor for now.")));
	score_label.set_justify (Gtk::JUSTIFY_CENTER);
	score_placeholder.pack_start (score_label, true, true);

	vpacker.pack_start (mode_bar, false, false);
	vpacker.pack_start (hpacker, true, true);
	vpacker.pack_start (score_placeholder, true, true);

	add (vpacker);
	vpacker.show_all ();
	/* visibility of the two views is managed by set_editor_mode () */
	score_placeholder.set_no_show_all ();
	score_placeholder.hide ();
	hpacker.set_no_show_all ();
	key_button.set_active (true);
}

void
PianorollWindow::set_editor_mode (EditorMode m)
{
	_mode = m;
	key_button.set_active (m == KeyEditor);
	drum_button.set_active (m == DrumEditor);
	score_button.set_active (m == ScoreEditor);

	if (m == ScoreEditor) {
		hpacker.hide ();
		score_label.show ();
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

	set_title (string_compose (_("%1 Pianoroll: %2"), PROGRAM_NAME, region->name()));
}

void
PianorollWindow::replace_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	pianoroll->replace_region (region, track);
}

void
PianorollWindow::remove_regions ()
{
	pianoroll->remove_regions ();
}

bool
PianorollWindow::on_key_press_event (GdkEventKey* ev)
{
	return ARDOUR_UI::instance()->key_event_handler (ev, this);
}

bool
PianorollWindow::on_delete_event (GdkEventAny*)
{
	hide ();
	return true;
}
