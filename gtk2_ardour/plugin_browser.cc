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

#include <algorithm>

#include "pbd/compose.h"
#include "pbd/string_convert.h"

#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/rc_configuration.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/menu_elems.h"

#include "widgets/tooltips.h"

#include "plugin_browser.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourWidgets;

namespace {

void
collect (PluginInfoList& dst, PluginInfoList const& src)
{
	PluginManager& mgr (PluginManager::instance ());
	for (auto const& p : src) {
		PluginManager::PluginStatusType st = mgr.get_status (p);
		if (st == PluginManager::Hidden || st == PluginManager::Concealed) {
			continue;
		}
		dst.push_back (p);
	}
}

std::string
lower (std::string s)
{
	std::transform (s.begin (), s.end (), s.begin (), ::tolower);
	return s;
}

} // namespace

PluginBrowser::PluginBrowser ()
	: _filter (All)
{
	using namespace Gtk::Menu_Helpers;
	_filter_dd.add_menu_elem (MenuElem (_("All Plugins"), sigc::bind (sigc::mem_fun (*this, &PluginBrowser::set_filter), All)));
	_filter_dd.add_menu_elem (MenuElem (_("Instruments"), sigc::bind (sigc::mem_fun (*this, &PluginBrowser::set_filter), Instruments)));
	_filter_dd.add_menu_elem (MenuElem (_("Effects"), sigc::bind (sigc::mem_fun (*this, &PluginBrowser::set_filter), Effects)));
	_filter_dd.add_menu_elem (MenuElem (_("Favorites"), sigc::bind (sigc::mem_fun (*this, &PluginBrowser::set_filter), Favorites)));
	_filter_dd.set_name ("generic button");
	_filter_dd.set_text (_("All Plugins"));
	_filter_dd.set_sizing_texts ({ _("All Plugins"), _("Instruments"), _("Favorites") });
	set_tooltip (_filter_dd, _("Which plugins to list"));

	_search.set_width_chars (8);
	_search.signal_changed ().connect (sigc::mem_fun (*this, &PluginBrowser::refill));
	set_tooltip (_search, _("Search by name, vendor or category"));

	_model = Gtk::ListStore::create (_columns);
	_display.set_model (_model);
	_display.append_column (_("Plugin"), _columns.name);
	_display.set_name ("EditGroupList");
	_display.set_headers_visible (false);
	_display.set_rules_hint (true);
	_display.set_can_focus (false);
	_display.get_selection ()->set_mode (Gtk::SELECTION_SINGLE);
	_display.add_object_drag (_columns.plugin.index (), "x-ardour/plugin.favorite", Gtk::TARGET_SAME_APP);
	_display.set_drag_column (_columns.name.index ());
	/* handle the double-click ourselves; row-activated is unreliable without focus */
	_display.signal_button_press_event ().connect (sigc::mem_fun (*this, &PluginBrowser::button_press), false);
	if (UIConfiguration::instance ().get_use_tooltips ()) {
		_display.set_tooltip_column (_columns.tip.index ());
	}

	_scroller.add (_display);
	_scroller.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

	Gtk::Label* hint = Gtk::manage (new Gtk::Label (_("Double-click: insert\non selected tracks.\nDrag: onto an insert slot.")));
	hint->set_name ("EditorInfoLine");
	hint->set_alignment (0, 0.5);
	hint->set_line_wrap (true);

	set_spacing (3);
	set_border_width (2);
	pack_start (_filter_dd, false, false);
	pack_start (_search, false, false);
	pack_start (_scroller, true, true);
	pack_start (*hint, false, false);

	PluginManager::instance ().PluginListChanged.connect (_manager_connections, invalidator (*this), std::bind (&PluginBrowser::refill, this), gui_context ());
	PluginManager::instance ().PluginStatusChanged.connect (_manager_connections, invalidator (*this), std::bind (&PluginBrowser::refill, this), gui_context ());

	refill ();
	show_all ();
}

PluginBrowser::~PluginBrowser ()
{
}

void
PluginBrowser::set_filter (Filter f)
{
	_filter = f;
	refill ();
}

void
PluginBrowser::refill ()
{
	PluginInfoList plugs;
	PluginManager& mgr (PluginManager::instance ());
#ifdef VST3_SUPPORT
	collect (plugs, mgr.vst3_plugin_info ());
#endif
#ifdef LXVST_SUPPORT
	collect (plugs, mgr.lxvst_plugin_info ());
#endif
#ifdef WINDOWS_VST_SUPPORT
	collect (plugs, mgr.windows_vst_plugin_info ());
#endif
#ifdef MACVST_SUPPORT
	collect (plugs, mgr.mac_vst_plugin_info ());
#endif
#ifdef AUDIOUNIT_SUPPORT
	collect (plugs, mgr.au_plugin_info ());
#endif
	collect (plugs, mgr.lv2_plugin_info ());
	collect (plugs, mgr.ladspa_plugin_info ());
	collect (plugs, mgr.lua_plugin_info ());

	plugs.sort ([] (PluginInfoPtr const& a, PluginInfoPtr const& b) {
			return lower (a->name) < lower (b->name);
			});

	std::string const needle = lower (_search.get_text ());

	_model->clear ();
	for (auto const& p : plugs) {
		switch (_filter) {
			case Instruments:
				if (!p->is_instrument ()) {
					continue;
				}
				break;
			case Effects:
				if (p->is_instrument ()) {
					continue;
				}
				break;
			case Favorites:
				if (mgr.get_status (p) != PluginManager::Favorite) {
					continue;
				}
				break;
			default:
				break;
		}
		if (!needle.empty ()) {
			std::string hay = lower (p->name + " " + p->creator + " " + p->category);
			if (hay.find (needle) == std::string::npos) {
				continue;
			}
		}
		Gtk::TreeModel::Row row = *_model->append ();
		row[_columns.name]   = p->name;
		row[_columns.tip]    = Gtkmm2ext::markup_escape_text (string_compose ("%1\n%2 • %3 • %4", p->name, p->creator, p->category, PluginManager::plugin_type_name (p->type)));
		row[_columns.plugin] = PluginPresetPtr (new PluginPreset (p));
	}
}

bool
PluginBrowser::button_press (GdkEventButton* ev)
{
	if (ev->type != GDK_2BUTTON_PRESS || ev->button != 1) {
		return false;
	}
	Gtk::TreeModel::Path path;
	Gtk::TreeViewColumn* col;
	int cx, cy;
	if (_display.get_path_at_pos ((int) ev->x, (int) ev->y, path, col, cx, cy)) {
		row_activated (path, col);
	}
	return true;
}

void
PluginBrowser::row_activated (Gtk::TreeModel::Path const& path, Gtk::TreeViewColumn*)
{
	Gtk::TreeModel::iterator i = _model->get_iter (path);
	if (!i) {
		return;
	}
	PluginPresetPtr ppp = (*i)[_columns.plugin];
	if (ppp) {
		insert_plugin (ppp->_pip);
	}
}

void
PluginBrowser::insert_plugin (PluginInfoPtr pip)
{
	if (!_session) {
		return;
	}
	std::shared_ptr<RouteList const> routes = _session->get_routes ();
	for (auto const& rt : *routes) {
		if (!rt->is_selected () || rt->is_monitor () || rt->is_auditioner ()) {
			continue;
		}
		PluginPtr p = pip->load (*_session);
		if (!p) {
			continue;
		}
		Route::ProcessorStreams err;
		std::shared_ptr<Processor> processor (new PluginInsert (*_session, *rt, p));
		if (pip->is_instrument ()) {
			rt->add_processor_by_index (processor, 0, &err, Config->get_new_plugins_active ());
		} else {
			rt->add_processor (processor, PreFader, &err, Config->get_new_plugins_active ());
		}
	}
}
