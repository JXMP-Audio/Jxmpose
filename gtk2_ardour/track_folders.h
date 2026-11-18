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
#include <vector>

#include <sigc++/signal.h>

namespace ARDOUR {
	class Route;
	class RouteGroup;
}

/** Folders in the editor are route groups with two GUI-only extras that are
 * stored with the session (GUIObjectState):
 *
 *  - a parent folder, which makes nested folders (subfolders) possible;
 *  - a collapsed flag: a collapsed folder shows only its busses (e.g. the
 *    instrument bus that its MIDI tracks play into), or its first track when
 *    it has no bus.
 */
namespace TrackFolders {

typedef std::shared_ptr<ARDOUR::RouteGroup> GroupPtr;

/** Deepest nesting level that is drawn (0 = top level only) */
static const int max_depth = 3;

GroupPtr parent (GroupPtr);
/** @return false if the change would create a cycle */
bool     set_parent (GroupPtr, GroupPtr parent);

/** @return the folder and its ancestors, outermost first */
std::vector<GroupPtr> chain (GroupPtr);
int      depth (GroupPtr);
bool     is_descendant (GroupPtr g, GroupPtr ancestor);

bool     collapsed (GroupPtr);
void     set_collapsed (GroupPtr, bool);

/** @return true if a collapsed folder hides this route in the editor */
bool     hidden_by_fold (std::shared_ptr<ARDOUR::Route>);

/** Emitted when folder structure or collapse state changes */
extern sigc::signal<void> Changed;

}
