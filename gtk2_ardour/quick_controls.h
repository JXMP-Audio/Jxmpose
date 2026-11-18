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

#include <ytkmm/box.h>
#include <ytkmm/label.h>
#include <ytkmm/menu.h>
#include <ytkmm/table.h>

#include "pbd/signals.h"

#include "evoral/Parameter.h"

#include "widgets/ardour_knob.h"

namespace ARDOUR {
	class AutomationControl;
	class Processor;
	class Route;
}

/** Eight knobs per track (studio Inspector) for the plugin parameters
 * used most. Right-click a knob to assign a parameter of any plugin on
 * the track (or clear it); Ctrl+middle-click learns a MIDI controller.
 * Until something is assigned, the knobs follow the first parameters of
 * the track's instrument (or first plugin). Assignments are kept in the
 * session's GUI state.
 */
class QuickControls : public Gtk::VBox
{
public:
	QuickControls ();
	~QuickControls ();

	void set_route (std::shared_ptr<ARDOUR::Route>);

	static const int n_slots = 8;

private:
	struct Slot {
		ArdourWidgets::ArdourKnob*                  knob;
		Gtk::Label*                                 label;
		std::shared_ptr<ARDOUR::AutomationControl> ctrl;
	};
	Slot _slots[n_slots];

	Gtk::Table                     _table;
	Gtk::Menu*                     _menu;
	std::shared_ptr<ARDOUR::Route> _route;
	PBD::ScopedConnectionList      _route_connections;
	bool                           _auto; ///< nothing assigned: following the first plugin

	std::string state_id () const;
	void        rebuild ();
	void        set_slot (int, std::shared_ptr<ARDOUR::Processor>, Evoral::Parameter);
	bool        knob_press (GdkEventButton*, int slot);
	void        assign (int slot, std::weak_ptr<ARDOUR::Processor>, Evoral::Parameter);
	void        clear (int slot);
	void        clear_all ();
};
