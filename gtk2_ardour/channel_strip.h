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
#include <ytkmm/drawingarea.h>
#include <ytkmm/label.h>
#include <ytkmm/menu.h>

#include "pbd/signals.h"

#include "widgets/ardour_button.h"
#include "widgets/ardour_knob.h"

namespace ARDOUR {
	class AutomationControl;
	class Processor;
	class Route;
}

/** Built-in channel strip (studio Inspector): gate, four band EQ with a
 * response curve, and compressor with a gain reduction meter, right
 * before the fader. A module is a bundled plugin (ACE Expander, EQ,
 * Compressor) that is inserted the first time its button is switched on
 * and is not listed with the inserts; the button then bypasses it,
 * right-click removes it. The modules of a route are kept in the session
 * GUI state ('channel-strip <route id>').
 */
class ChannelStrip : public Gtk::VBox
{
public:
	ChannelStrip ();
	~ChannelStrip ();

	void set_route (std::shared_ptr<ARDOUR::Route>);

	enum Module { Gate = 0, EQ, Comp, NModules };

	/** a processor that belongs to the channel strip of a route (it is not shown with the inserts) */
	static bool is_strip_processor (std::shared_ptr<ARDOUR::Route>, std::shared_ptr<ARDOUR::Processor>);

private:
	std::shared_ptr<ARDOUR::Route> _route;
	PBD::ScopedConnectionList      _route_connections;
	PBD::ScopedConnectionList      _proc_connections;
	sigc::connection               _meter_connection;

	Gtk::HBox                   _buttons;
	ArdourWidgets::ArdourButton _button[NModules];
	Gtk::VBox                   _body[NModules];
	Gtk::DrawingArea            _curve;
	Gtk::DrawingArea            _gr_meter;
	Gtk::Menu*                  _menu;

	std::shared_ptr<ARDOUR::Processor> _proc[NModules];
	std::vector<ArdourWidgets::ArdourKnob*> _knobs;
	float                               _gr; ///< compressor gain reduction (dB)

	std::shared_ptr<ARDOUR::Processor> find (Module) const;
	std::shared_ptr<ARDOUR::AutomationControl> control (Module, uint32_t param) const;

	void rebuild ();
	void update_buttons ();
	bool button_press (GdkEventButton*, Module);
	void toggle (Module);
	void insert (Module);
	void remove (Module);
	void add_knob (Gtk::Table&, int col, Module, uint32_t param, std::string const& label);

	bool curve_expose (GdkEventExpose*);
	bool gr_expose (GdkEventExpose*);
	void update_meter ();
	bool curve_press (GdkEventButton*);
};
