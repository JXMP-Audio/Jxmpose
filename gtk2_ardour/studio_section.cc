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

#include <sstream>

#include "studio_section.h"
#include "ui_config.h"

using namespace ArdourWidgets;

StudioSection::StudioSection (std::string const& title, std::string const& key, bool expand, bool default_open)
	: _title (title)
	, _key (key)
	, _expand (expand)
	, _default_open (default_open)
{
	_header.set_name ("generic button");
	_header.set_alignment (0.0, 0.5);
	_header.set_corner_radius (3);
	_header.signal_clicked.connect (sigc::mem_fun (*this, &StudioSection::toggle));
	_content.set_spacing (2);
	set_spacing (2);
	pack_start (_header, false, false);
	pack_start (_content, true, true);
	_header.show ();
	_content.show ();
	changed ().connect (sigc::mem_fun (*this, &StudioSection::apply));
	apply (_key);
}

void
StudioSection::add_item (Gtk::Widget& w, bool expand)
{
	_content.pack_start (w, expand, expand);
}

void
StudioSection::show_all_vfunc ()
{
	Gtk::VBox::show_all_vfunc ();
	apply (_key);
}

void
StudioSection::toggle ()
{
	state ()[_key] = !is_open ();
	save_state ();
	changed ().emit (_key);
}

void
StudioSection::apply (std::string const& key)
{
	if (key != _key) {
		return;
	}
	const bool open = is_open ();
	if (open) {
		_content.show ();
	} else {
		_content.hide ();
	}
	_header.set_text ((open ? "▾  " : "▸  ") + _title);
	Gtk::Box* p = dynamic_cast<Gtk::Box*> (get_parent ());
	if (p && _expand) {
		p->set_child_packing (*this, open, open, 0u);
	}
}

/* open/closed state, kept in the UI config as a comma separated list of closed keys */
std::map<std::string, bool>&
StudioSection::state ()
{
	static std::map<std::string, bool> s;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		std::stringstream ss (UIConfiguration::instance ().get_studio_closed_sections ());
		std::string k;
		/* "key" = closed, "+key" = opened (for sections that start closed) */
		while (std::getline (ss, k, ',')) {
			if (k.size () > 1 && k[0] == '+') {
				s[k.substr (1)] = true;
			} else if (!k.empty ()) {
				s[k] = false;
			}
		}
	}
	return s;
}

void
StudioSection::save_state ()
{
	std::string closed;
	for (auto const& i : state ()) {
		closed += std::string (closed.empty () ? "" : ",") + (i.second ? "+" : "") + i.first;
	}
	UIConfiguration::instance ().set_studio_closed_sections (closed);
}

sigc::signal<void, std::string>&
StudioSection::changed ()
{
	static sigc::signal<void, std::string> c;
	return c;
}

bool
StudioSection::is_open () const
{
	std::map<std::string, bool>::const_iterator i = state ().find (_key);
	return i == state ().end () ? _default_open : i->second;
}
