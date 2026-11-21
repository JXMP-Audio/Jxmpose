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

#include <cstring>
#include <sstream>

#include "pbd/compose.h"

#include "ardour/automation_control.h"
#include "ardour/plugin_insert.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "ardour_ui.h"
#include "gui_object.h"
#include "gui_thread.h"
#include "quick_controls.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourWidgets;

namespace {

/** the plugin parameters of a processor that can be put on a knob */
std::vector<Evoral::Parameter>
knob_parameters (std::shared_ptr<Processor> p)
{
	std::vector<Evoral::Parameter> r;
	if (!std::dynamic_pointer_cast<PluginInsert> (p)) {
		return r;
	}
	for (auto const& param : p->what_can_be_automated ()) {
		if (param.type () == PluginAutomation && p->describe_parameter (param) != X_("hidden")) {
			r.push_back (param);
		}
	}
	return r;
}

/** a word in at most 4 letters: "Level" -> "Lvl", "Decay" -> "Dcy" */
std::string
short_word (std::string const& w)
{
	if (w.size () <= 4) {
		return w;
	}
	std::string r (1, w[0]);
	for (size_t i = 1; i < w.size () && r.size () < 4; ++i) {
		if (!strchr ("aeiouAEIOU", w[i])) {
			r += w[i];
		}
	}
	return r;
}

/** a knob label in two short lines: "BD Level" -> "BD\nLvl" */
std::string
short_name (std::string const& name)
{
	std::istringstream is (name);
	std::string        w0, w1;
	is >> w0 >> w1;
	if (w1.empty ()) {
		return w0.size () <= 6 ? w0 : short_word (w0);
	}
	return short_word (w0) + "\n" + short_word (w1);
}

} // namespace

QuickControls::QuickControls ()
	: _table (4, 4, true)
	, _menu (0)
	, _auto (true)
{
	_table.set_spacings (1);
	pack_start (_table, false, false);

	for (int i = 0; i < n_slots; ++i) {
		_slots[i].knob  = 0;
		_slots[i].label = manage (new Gtk::Label);
		_slots[i].label->modify_font (UIConfiguration::instance ().get_SmallFont ());
		_slots[i].label->set_ellipsize (Pango::ELLIPSIZE_END);
		_slots[i].label->set_max_width_chars (5);
		_slots[i].label->set_justify (Gtk::JUSTIFY_CENTER);
		_slots[i].label->set_size_request (1, -1);
		_table.attach (*_slots[i].label, i % 4, i % 4 + 1, (i / 4) * 2 + 1, (i / 4) * 2 + 2, Gtk::FILL | Gtk::EXPAND, Gtk::SHRINK);
	}
	rebuild ();
	show_all ();
}

QuickControls::~QuickControls ()
{
	/* the knobs are not managed (rebuild () replaces them): they go with us */
	for (int i = 0; i < n_slots; ++i) {
		delete _slots[i].knob;
	}
	delete _menu;
}

void
QuickControls::set_route (std::shared_ptr<Route> r)
{
	_route_connections.drop_connections ();
	_route = r;
	if (_route) {
		_route->processors_changed.connect (_route_connections, invalidator (*this), std::bind (&QuickControls::rebuild, this), gui_context ());
		_route->DropReferences.connect (_route_connections, invalidator (*this), std::bind (&QuickControls::set_route, this, std::shared_ptr<Route> ()), gui_context ());
	}
	rebuild ();
}

std::string
QuickControls::state_id () const
{
	return _route ? string_compose (X_("quick-controls %1"), _route->id ().to_s ()) : std::string ();
}

void
QuickControls::rebuild ()
{
	/* fresh knobs: a knob cannot drop its controllable */
	for (int i = 0; i < n_slots; ++i) {
		Slot& s (_slots[i]);
		if (s.knob) {
			_table.remove (*s.knob);
			delete s.knob;
		}
		s.knob = new ArdourKnob (ArdourKnob::default_elements, ArdourKnob::NoFlags);
		float const scale = UIConfiguration::instance ().get_ui_scale ();
		s.knob->set_size_request (rintf (22.f * scale), rintf (22.f * scale));
		s.knob->signal_button_press_event ().connect (sigc::bind (sigc::mem_fun (*this, &QuickControls::knob_press), i), false);
		_table.attach (*s.knob, i % 4, i % 4 + 1, (i / 4) * 2, (i / 4) * 2 + 1, Gtk::SHRINK, Gtk::SHRINK);
		s.knob->show ();
		s.ctrl.reset ();
		s.label->set_text (u8"–");
		set_tooltip (*s.knob, _("Quick Control: right-click to assign a plugin parameter, Ctrl+middle-click to learn a MIDI controller"));
	}

	if (!_route) {
		return;
	}

	GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
	std::string const sid = state_id ();
	_auto = gs.get_string (sid, X_("assigned")) != X_("1");

	if (_auto) {
		/* the instrument's (or the first plugin's) first parameters */
		std::shared_ptr<Processor> p = _route->the_instrument ();
		if (!p) {
			p = _route->nth_plugin (0);
		}
		if (p) {
			int i = 0;
			for (auto const& param : knob_parameters (p)) {
				if (i >= n_slots) {
					break;
				}
				set_slot (i++, p, param);
			}
		}
		return;
	}

	for (int i = 0; i < n_slots; ++i) {
		std::string const v = gs.get_string (sid, string_compose (X_("slot%1"), i));
		if (v.empty ()) {
			continue;
		}
		std::istringstream is (v);
		std::string        pid;
		uint32_t           param_id;
		if (!(is >> pid >> param_id)) {
			continue;
		}
		std::shared_ptr<Processor> p = _route->processor_by_id (PBD::ID (pid));
		if (p) {
			set_slot (i, p, Evoral::Parameter (PluginAutomation, 0, param_id));
		}
	}
}

void
QuickControls::set_slot (int i, std::shared_ptr<Processor> p, Evoral::Parameter param)
{
	std::shared_ptr<AutomationControl> c = p->automation_control (param);
	if (!c) {
		return;
	}
	Slot& s (_slots[i]);
	s.ctrl = c;
	s.knob->set_controllable (c);
	std::string const name = p->describe_parameter (param);
	s.knob->set_tooltip_prefix (string_compose ("%1: %2: ", p->name (), name));
	s.label->set_text (short_name (name));
}

bool
QuickControls::knob_press (GdkEventButton* ev, int slot)
{
	if (ev->button != 3 || !_route) {
		return false;
	}

	delete _menu;
	_menu = new Gtk::Menu;
	Gtk::Menu_Helpers::MenuList& items (_menu->items ());

	bool any = false;
	for (uint32_t n = 0;; ++n) {
		std::shared_ptr<Processor> p = _route->nth_plugin (n);
		if (!p) {
			break;
		}
		std::vector<Evoral::Parameter> const params = knob_parameters (p);
		if (params.empty ()) {
			continue;
		}
		Gtk::Menu* sub = manage (new Gtk::Menu);
		for (auto const& param : params) {
			sub->items ().push_back (Gtk::Menu_Helpers::MenuElem (p->describe_parameter (param),
			                                                       sigc::bind (sigc::mem_fun (*this, &QuickControls::assign), slot, std::weak_ptr<Processor> (p), param)));
		}
		items.push_back (Gtk::Menu_Helpers::MenuElem (p->name (), *sub));
		any = true;
	}
	if (!any) {
		items.push_back (Gtk::Menu_Helpers::MenuElem (_("No plugins on this track")));
		items.back ().set_sensitive (false);
	}

	items.push_back (Gtk::Menu_Helpers::SeparatorElem ());
	items.push_back (Gtk::Menu_Helpers::MenuElem (_("Clear"), sigc::bind (sigc::mem_fun (*this, &QuickControls::clear), slot)));
	items.back ().set_sensitive (_slots[slot].ctrl != 0);
	items.push_back (Gtk::Menu_Helpers::MenuElem (_("Follow the Instrument"), sigc::mem_fun (*this, &QuickControls::clear_all)));
	items.back ().set_sensitive (!_auto);

	_menu->popup (ev->button, ev->time);
	return true;
}

void
QuickControls::assign (int slot, std::weak_ptr<Processor> wp, Evoral::Parameter param)
{
	std::shared_ptr<Processor> p = wp.lock ();
	if (!p || !_route) {
		return;
	}
	GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
	std::string const sid = state_id ();

	if (_auto) {
		/* keep what the other knobs show now */
		for (int i = 0; i < n_slots; ++i) {
			std::string v;
			if (_slots[i].ctrl) {
				for (uint32_t n = 0;; ++n) {
					std::shared_ptr<Processor> q = _route->nth_plugin (n);
					if (!q) {
						break;
					}
					for (auto const& prm : knob_parameters (q)) {
						if (q->automation_control (prm) == _slots[i].ctrl) {
							v = string_compose ("%1 %2", q->id ().to_s (), prm.id ());
						}
					}
				}
			}
			gs.set_property (sid, string_compose (X_("slot%1"), i), v);
		}
		gs.set_property (sid, X_("assigned"), X_("1"));
	}

	gs.set_property (sid, string_compose (X_("slot%1"), slot), string_compose ("%1 %2", p->id ().to_s (), param.id ()));
	_route->session ().set_dirty ();
	rebuild ();
}

void
QuickControls::clear (int slot)
{
	if (!_route) {
		return;
	}
	if (_auto) {
		/* materialize the current knobs, then drop this one */
		std::shared_ptr<Processor> p = _route->the_instrument ();
		if (!p) {
			p = _route->nth_plugin (0);
		}
		if (p) {
			std::vector<Evoral::Parameter> const params = knob_parameters (p);
			if (slot < (int) params.size ()) {
				assign (slot, p, params[slot]);
			}
		}
	}
	GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
	gs.set_property (state_id (), string_compose (X_("slot%1"), slot), std::string ());
	_route->session ().set_dirty ();
	rebuild ();
}

void
QuickControls::clear_all ()
{
	if (!_route) {
		return;
	}
	GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
	gs.remove_node (state_id ());
	_route->session ().set_dirty ();
	rebuild ();
}
