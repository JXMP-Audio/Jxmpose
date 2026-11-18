/*
 * Copyright (C) 2009-2011 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2009-2011 David Robillard <d@drobilla.net>
 * Copyright (C) 2011-2016 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2014-2017 Robin Gareus <robin@gareus.org>
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

#include "ardour/route_group.h"

#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/rgb_macros.h"

#include "editor.h"
#include "editor_group_tabs.h"
#include "editor_route_groups.h"
#include "editor_routes.h"
#include "ardour_message.h"
#include "track_folders.h"
#include "widgets/prompter.h"
#include "pbd/compose.h"
#include "ardour/session.h"
#include "gtkmm2ext/utils.h"
#include "route_time_axis.h"
#include "ui_config.h"
#include "utils.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace ARDOUR_UI_UTILS;

EditorGroupTabs::EditorGroupTabs (Editor& e)
	: EditorComponent (e)
	, _columns (1)
	, _req_columns (1)
{
	TrackFolders::Changed.connect (sigc::mem_fun (*this, &EditorGroupTabs::folders_changed));
}

void
EditorGroupTabs::folders_changed ()
{
	queue_resize ();
	set_dirty ();
}

void
EditorGroupTabs::on_size_request (Gtk::Requisition* req)
{
	Glib::RefPtr<Pango::Layout> layout = Pango::Layout::create (get_pango_context ());
	layout->set_text (X_("Tab Text"));
	int tw, th;
	layout->get_pixel_size (tw, th);
	int const size = (int) ceil (th * 1.25 + 1.0); /* as GroupTabs */

	_req_columns = n_columns ();
	req->width  = size * _req_columns;
	req->height = size;
}

int
EditorGroupTabs::n_columns () const
{
	/* one column per folder nesting level in use */
	int depth = 0;
	if (_editor.session ()) {
		for (auto const& g : _editor.session ()->route_groups ()) {
			depth = std::max (depth, std::min (TrackFolders::depth (g), TrackFolders::max_depth));
		}
	}
	return depth + 1;
}

double
EditorGroupTabs::column_width () const
{
	return get_width () / (double) std::max (1, _columns);
}

int
EditorGroupTabs::column_at (double x, double) const
{
	return std::max (0, std::min (_columns - 1, (int) floor (x / column_width ())));
}

bool
EditorGroupTabs::on_fold_arrow (Tab const& t, double, double y) const
{
	double const p = y + offset ();
	return p >= t.from && p < t.from + column_width ();
}

list<GroupTabs::Tab>
EditorGroupTabs::compute_tabs () const
{
	list<Tab> tabs;

	/* folder chain (outermost first) of each visible track */
	std::vector<std::pair<TimeAxisView*, std::vector<TrackFolders::GroupPtr> > > rows;
	int const ncols = n_columns ();
	if (ncols != _req_columns) {
		/* folder nesting changed (e.g. session state was loaded) */
		const_cast<EditorGroupTabs*> (this)->queue_resize ();
	}
	for (TrackViewList::iterator i = _editor.track_views.begin(); i != _editor.track_views.end(); ++i) {
		if ((*i)->marked_for_display() == false) {
			continue;
		}
		std::vector<TrackFolders::GroupPtr> ch = TrackFolders::chain ((*i)->route_group ());
		if ((int) ch.size () > TrackFolders::max_depth + 1) {
			ch.resize (TrackFolders::max_depth + 1);
		}
		rows.push_back (std::make_pair (*i, ch));
	}
	_columns = ncols;

	for (int d = 0; d < ncols; ++d) {
		Tab tab;
		tab.from  = 0;
		tab.depth = d;

		int32_t y = 0;
		for (auto const& r : rows) {
			std::shared_ptr<RouteGroup> g (d < (int) r.second.size () ? r.second[d] : std::shared_ptr<RouteGroup> ());
			std::shared_ptr<RouteGroup> tab_group (tab.group.lock());

			if (g != tab_group) {
				if (tab_group) {
					tab.to = y;
					tabs.push_back (tab);
				}
				tab.from = y;
				tab.group = g;
				if (g) {
					tab.color = group_color (g);
				}
			}
			y += r.first->effective_height ();
		}

		if (tab.group.lock()) {
			tab.to = y;
			tabs.push_back (tab);
		}
	}

	return tabs;
}

void
EditorGroupTabs::draw_tab (cairo_t* cr, Tab const & tab)
{
	const double from = tab.from - offset ();
	const double to   = tab.to   - offset ();

	if (from > visible_extent () || to < 0) {
		return;
	}

	double const colw       = column_width ();
	double const x0         = tab.depth * colw;
	double const arc_radius = colw;
	double r, g, b, a;

	std::shared_ptr<RouteGroup> tab_group (tab.group.lock());

	if (tab_group && tab_group->is_active()) {
		Gtkmm2ext::color_to_rgba (tab.color, r, g, b, a);
	} else if (!tab_group && _dragging_new_tab) {
		Gdk::Color col = ARDOUR_UI_UTILS::round_robin_palette_color (true);
		color_t ct = Gtkmm2ext::gdk_color_to_rgba (col);
		Gtkmm2ext::color_to_rgba (ct, r, g, b, a);
	} else {
		Gtkmm2ext::color_to_rgba (UIConfiguration::instance().color ("inactive group tab"), r, g, b, a);
	}

	a = 1.0;

	cairo_set_source_rgba (cr, r, g, b, a);
	cairo_move_to (cr, x0, from + arc_radius);
	cairo_arc (cr, x0 + colw, from + arc_radius, arc_radius, M_PI, 3 * M_PI / 2);
	cairo_line_to (cr, x0 + colw, to);
	cairo_arc (cr, x0 + colw, to - arc_radius, arc_radius, M_PI / 2, M_PI);
	cairo_line_to (cr, x0, from + arc_radius);
	cairo_fill (cr);

	if (!tab_group) {
		return;
	}

	Gtkmm2ext::Color c = Gtkmm2ext::contrasting_text_color (Gtkmm2ext::rgba_to_color (r, g, b, a));
	Gtkmm2ext::color_to_rgba (c, r, g, b, a);
	cairo_set_source_rgb (cr, r, g, b);

	/* fold arrow at the top of the tab */
	{
		bool const folded = TrackFolders::collapsed (tab_group);
		double const cx = x0 + colw * .55;
		double const cy = from + colw * .6;
		double const s  = std::max (2.5, colw * .2);
		if (folded) {
			cairo_move_to (cr, cx - s * .6, cy - s);
			cairo_line_to (cr, cx + s * .8, cy);
			cairo_line_to (cr, cx - s * .6, cy + s);
		} else {
			cairo_move_to (cr, cx - s, cy - s * .6);
			cairo_line_to (cr, cx + s, cy - s * .6);
			cairo_line_to (cr, cx, cy + s * .8);
		}
		cairo_close_path (cr);
		cairo_fill (cr);
	}

	double const text_from = from + colw;
	if ((to - text_from) > arc_radius * .5) {
		int text_width, text_height;

		Glib::RefPtr<Pango::Layout> layout;
		layout = Pango::Layout::create (get_pango_context ());
		layout->set_ellipsize (Pango::ELLIPSIZE_MIDDLE);

		layout->set_text (tab_group->name ());
		layout->set_width ((to - text_from - arc_radius * .5) * PANGO_SCALE);
		layout->get_pixel_size (text_width, text_height);

		cairo_move_to (cr, x0 + (colw - text_height) * .5, (text_width + to + text_from) * .5);

		cairo_save (cr);
		cairo_rotate (cr, M_PI * -.5);
		pango_cairo_show_layout (cr, layout->gobj ());
		cairo_restore (cr);
	}
}

double
EditorGroupTabs::primary_coordinate (double, double y) const
{
	return y + offset ();
}

RouteList
EditorGroupTabs::routes_for_tab (Tab const * t) const
{
	RouteList routes;
	int32_t y = 0;

	for (TrackViewList::iterator i = _editor.track_views.begin(); i != _editor.track_views.end(); ++i) {

		if ((*i)->marked_for_display() == false) {
			continue;
		}

		RouteTimeAxisView* rtv = dynamic_cast<RouteTimeAxisView*> (*i);
		if (rtv) {

			if (y >= t->to) {
				/* tab finishes before this track starts */
				break;
			}

			double const h = y + (*i)->effective_height() / 2;

			if (t->from < h && t->to > h) {
				routes.push_back (rtv->route ());
			}
		}

		y += (*i)->effective_height ();
	}

	return routes;
}


void
EditorGroupTabs::add_menu_items (Gtk::Menu* m, std::shared_ptr<RouteGroup> g)
{
	using namespace Gtk::Menu_Helpers;

	if (g) {
		MenuList& items = m->items ();
		std::weak_ptr<RouteGroup> wg (g);

		items.push_back (MenuElem (_("Fit to Window"), [wg,this]() { std::shared_ptr<RouteGroup> g (wg.lock()); if (g) { _editor.fit_route_group (g); }}));

		/* folders */
		items.push_back (SeparatorElem ());
		bool const folded = TrackFolders::collapsed (g);
		items.push_back (MenuElem (folded ? _("Expand Folder") : _("Collapse Folder"), [wg, folded]() {
					std::shared_ptr<RouteGroup> g (wg.lock ());
					if (g) {
						TrackFolders::set_collapsed (g, !folded);
					}
					}));
		items.push_back (MenuElem (_("New Subfolder from Selected Tracks..."), sigc::bind (sigc::mem_fun (*this, &EditorGroupTabs::new_subfolder), wg)));

		Gtk::Menu* pm = manage (new Gtk::Menu);
		MenuList& pitems = pm->items ();
		TrackFolders::GroupPtr cur = TrackFolders::parent (g);
		pitems.push_back (CheckMenuElem (_("(Top Level)")));
		{
			Gtk::CheckMenuItem* ci = dynamic_cast<Gtk::CheckMenuItem*> (&pitems.back ());
			ci->set_active (!cur);
			ci->signal_activate ().connect ([wg]() { std::shared_ptr<RouteGroup> g (wg.lock ()); if (g) { TrackFolders::set_parent (g, TrackFolders::GroupPtr ()); } });
		}
		for (auto const& o : _editor.session ()->route_groups ()) {
			if (o == g || TrackFolders::is_descendant (o, g)) {
				continue;
			}
			std::weak_ptr<RouteGroup> wo (o);
			pitems.push_back (CheckMenuElem (o->name ()));
			Gtk::CheckMenuItem* ci = dynamic_cast<Gtk::CheckMenuItem*> (&pitems.back ());
			ci->set_active (o == cur);
			ci->signal_activate ().connect ([wg, wo]() {
					std::shared_ptr<RouteGroup> g (wg.lock ());
					std::shared_ptr<RouteGroup> o (wo.lock ());
					if (g && o) {
						TrackFolders::set_parent (g, o);
					}
					});
		}
		items.push_back (MenuElem (_("Place Folder Inside"), *pm));
	}
}

void
EditorGroupTabs::new_subfolder (std::weak_ptr<RouteGroup> wp)
{
	std::shared_ptr<RouteGroup> parent (wp.lock ());
	Session* s = _editor.session ();
	if (!parent || !s) {
		return;
	}

	RouteList routes = selected_routes ();
	if (routes.empty ()) {
		ArdourMessageDialog msg (_("Select the tracks for the new subfolder first."));
		msg.run ();
		return;
	}

	ArdourWidgets::Prompter prompter (true);
	prompter.set_title (_("New Subfolder"));
	prompter.set_prompt (_("Name:"));
	prompter.set_initial_text (string_compose (_("%1 Sub"), parent->name ()));
	prompter.add_button (_("Create"), Gtk::RESPONSE_ACCEPT);
	prompter.show_all ();
	if (prompter.run () != Gtk::RESPONSE_ACCEPT) {
		return;
	}
	std::string name;
	prompter.get_result (name);
	if (name.empty ()) {
		return;
	}
	while (s->route_group_by_name (name)) {
		name += "+";
	}

	std::shared_ptr<RouteGroup> g = s->new_route_group (name);
	if (!g) {
		return;
	}
	/* a folder only organises tracks; it does not share their controls */
	g->set_gain (false);
	g->set_mute (false);
	g->set_solo (false);
	g->set_recenable (false);
	g->set_select (false);
	g->set_route_active (false);
	g->set_color (false);
	g->set_monitoring (false);
	/* a paler shade of the parent folder's colour */
	Gtkmm2ext::HSV hsv (group_color (parent));
	hsv.s *= .55;
	hsv = hsv.lighter (.2);
	g->set_rgba (hsv.color ());
	s->add_route_group (g);

	for (auto const& r : routes) {
		g->add (r);
	}
	TrackFolders::set_parent (g, parent);
}

RouteList
EditorGroupTabs::selected_routes () const
{
	RouteList rl;

	for (TrackSelection::iterator i = _editor.get_selection().tracks.begin(); i != _editor.get_selection().tracks.end(); ++i) {
		RouteTimeAxisView* rtv = dynamic_cast<RouteTimeAxisView*> (*i);
		if (rtv) {
			rl.push_back (rtv->route());
		}
	}

	return rl;
}
