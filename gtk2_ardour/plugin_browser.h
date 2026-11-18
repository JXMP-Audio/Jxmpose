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

#include <ytkmm/box.h>
#include <ytkmm/label.h>
#include <ytkmm/entry.h>
#include <ytkmm/liststore.h>
#include <ytkmm/scrolledwindow.h>

#include "pbd/signals.h"

#include "ardour/plugin.h"
#include "ardour/session_handle.h"

#include "gtkmm2ext/dndtreeview.h"
#include "widgets/ardour_dropdown.h"

/** Plugin & instrument browser for the editor's right zone.
 *
 * Search + filter (all, instruments, effects, favorites). Double-click inserts
 * the plugin into the selected tracks (instruments at the top, effects
 * pre-fader); rows can also be dragged onto any processor box.
 */
class PluginBrowser : public Gtk::VBox, public ARDOUR::SessionHandlePtr
{
public:
	PluginBrowser ();
	~PluginBrowser ();

private:
	enum Filter {
		All,
		Instruments,
		Effects,
		Favorites,
	};

	struct Columns : public Gtk::TreeModel::ColumnRecord {
		Columns () {
			add (name);
			add (tip);
			add (plugin);
		}
		Gtk::TreeModelColumn<std::string>             name;
		Gtk::TreeModelColumn<std::string>             tip;
		Gtk::TreeModelColumn<ARDOUR::PluginPresetPtr> plugin;
	};

	void set_filter (Filter);
	void refill ();
	void row_activated (Gtk::TreeModel::Path const&, Gtk::TreeViewColumn*);
	bool button_press (GdkEventButton*);
	void insert_plugin (ARDOUR::PluginInfoPtr);

	Filter                         _filter;
	ArdourWidgets::ArdourDropdown  _filter_dd;
	Gtk::Entry                     _search;
	Columns                        _columns;
	Glib::RefPtr<Gtk::ListStore>   _model;
	Gtkmm2ext::DnDTreeView<ARDOUR::PluginPresetPtr> _display;
	Gtk::ScrolledWindow            _scroller;

	PBD::ScopedConnectionList _manager_connections;
};
