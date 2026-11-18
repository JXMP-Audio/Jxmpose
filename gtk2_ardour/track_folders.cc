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

#include "ardour/route.h"
#include "ardour/route_group.h"
#include "ardour/session.h"
#include "ardour/track.h"

#include "ardour_ui.h"
#include "group_tabs.h"
#include "gui_object.h"
#include "track_folders.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

namespace TrackFolders {

sigc::signal<void> Changed;

namespace {

Session*
session ()
{
	return ARDOUR_UI::instance () ? ARDOUR_UI::instance ()->the_session () : 0;
}

GUIObjectState*
state ()
{
	return ARDOUR_UI::instance () ? ARDOUR_UI::instance ()->gui_object_state : 0;
}

GroupPtr
group_by_id (std::string const& id)
{
	Session* s = session ();
	if (!s || id.empty ()) {
		return GroupPtr ();
	}
	for (auto const& g : s->route_groups ()) {
		if (g->id ().to_s () == id) {
			return g;
		}
	}
	return GroupPtr ();
}

} // namespace

GroupPtr
parent (GroupPtr g)
{
	GUIObjectState* st = state ();
	if (!g || !st) {
		return GroupPtr ();
	}
	GroupPtr p = group_by_id (st->get_string (GroupTabs::group_gui_id (g), X_("folder-parent")));
	return p == g ? GroupPtr () : p;
}

bool
is_descendant (GroupPtr g, GroupPtr ancestor)
{
	if (!g || !ancestor) {
		return false;
	}
	int guard = 0;
	for (GroupPtr p = parent (g); p && guard < 64; p = parent (p), ++guard) {
		if (p == ancestor) {
			return true;
		}
	}
	return false;
}

bool
set_parent (GroupPtr g, GroupPtr p)
{
	GUIObjectState* st = state ();
	if (!g || !st) {
		return false;
	}
	if (p && (p == g || is_descendant (p, g))) {
		return false;
	}
	st->set_property (GroupTabs::group_gui_id (g), X_("folder-parent"), p ? p->id ().to_s () : std::string ());
	Changed (); /* EMIT SIGNAL */
	return true;
}

std::vector<GroupPtr>
chain (GroupPtr g)
{
	std::vector<GroupPtr> c;
	int guard = 0;
	for (GroupPtr p = g; p && guard < 64; p = parent (p), ++guard) {
		c.insert (c.begin (), p);
	}
	return c;
}

int
depth (GroupPtr g)
{
	return std::max (0, (int) chain (g).size () - 1);
}

bool
collapsed (GroupPtr g)
{
	GUIObjectState* st = state ();
	if (!g || !st) {
		return false;
	}
	return st->get_string (GroupTabs::group_gui_id (g), X_("folder-collapsed")) == X_("1");
}

void
set_collapsed (GroupPtr g, bool yn)
{
	GUIObjectState* st = state ();
	if (!g || !st || collapsed (g) == yn) {
		return;
	}
	st->set_property (GroupTabs::group_gui_id (g), X_("folder-collapsed"), std::string (yn ? "1" : "0"));
	Changed (); /* EMIT SIGNAL */
}

bool
hidden_by_fold (std::shared_ptr<Route> r)
{
	if (!r) {
		return false;
	}
	GroupPtr g = r->route_group ();
	if (!g) {
		return false;
	}

	/* the outermost collapsed folder that contains this route */
	GroupPtr folded;
	for (auto const& f : chain (g)) {
		if (collapsed (f)) {
			folded = f;
			break;
		}
	}
	if (!folded) {
		return false;
	}

	/* a collapsed folder keeps its own busses visible */
	bool has_bus = false;
	for (auto const& m : *folded->route_list ()) {
		if (!std::dynamic_pointer_cast<Track> (m)) {
			has_bus = true;
			break;
		}
	}
	if (has_bus) {
		return !(g == folded && !std::dynamic_pointer_cast<Track> (r));
	}

	/* no bus: keep the first route of the folder (incl. subfolders) visible */
	Session* s = session ();
	if (!s) {
		return false;
	}
	std::shared_ptr<Route> first;
	for (auto const& m : *s->get_routes ()) {
		GroupPtr mg = m->route_group ();
		if (!mg || (mg != folded && !is_descendant (mg, folded))) {
			continue;
		}
		if (!first || m->presentation_info ().order () < first->presentation_info ().order ()) {
			first = m;
		}
	}
	return r != first;
}

} // namespace TrackFolders
