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

#include "ardour_ui.h"
#include "pianoroll_window.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

PianorollWindow::PianorollWindow (std::string const & name, Session& s)
	: ArdourWindow (string_compose ("%1 - %2", PROGRAM_NAME, name))
	, panel (name, true, false)
{
	panel.set_session (&s);
	set_default_size (1270, 700);
	add (panel);
	panel.show ();
}

PianorollWindow::~PianorollWindow ()
{
}

void
PianorollWindow::set_show_source (bool yn)
{
	panel.set_show_source (yn);
}

void
PianorollWindow::add_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	panel.add_region (track, region);
}

void
PianorollWindow::set_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	panel.set_region (track, region);
	set_title (string_compose (_("%1 Pianoroll: %2"), PROGRAM_NAME, region->name()));
}

void
PianorollWindow::replace_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	panel.replace_region (track, region);
}

void
PianorollWindow::remove_regions ()
{
	panel.remove_regions ();
}

bool
PianorollWindow::on_key_press_event (GdkEventKey* ev)
{
	if (panel.handle_key (ev)) {
		return true;
	}
	return ARDOUR_UI::instance()->key_event_handler (ev, this);
}

bool
PianorollWindow::on_delete_event (GdkEventAny*)
{
	hide ();
	return true;
}
