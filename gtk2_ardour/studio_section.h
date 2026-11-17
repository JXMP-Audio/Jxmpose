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

#include <map>
#include <string>

#include <ytkmm/box.h>

#include "widgets/ardour_button.h"

/* A titled, collapsible group of widgets (studio look). Sections that share
 * a key open and close together (e.g. the Inserts rack of every mixer strip);
 * closed keys are remembered in the UI configuration (studio-closed-sections).
 */
class StudioSection : public Gtk::VBox
{
public:
	StudioSection (std::string const& title, std::string const& key, bool expand);

	void add_item (Gtk::Widget& w, bool expand = false);
	void toggle ();
	bool is_open () const;
	/* re-apply open/closed state, call once the section is packed */
	void refresh () { apply (_key); }

private:
	void apply (std::string const& key);

	static std::map<std::string, bool>& state ();
	static void save_state ();
	static sigc::signal<void, std::string>& changed ();

	ArdourWidgets::ArdourButton _header;
	Gtk::VBox                   _content;
	std::string                 _title;
	std::string                 _key;
	bool                        _expand;
};
