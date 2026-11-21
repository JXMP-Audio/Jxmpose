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

#include <cairo.h>

#include <ytkmm/box.h>
#include <ytkmm/drawingarea.h>

#include "plugin_ui.h"

namespace ARDOUR {
	class AutomationControl;
}

struct JxLayout;

/** The JXMP hardware panel of a JX plugin (urn:jxmp:...) or a bundled ACE
 * plugin (urn:ardour:a-...): the plugin's own
 * display on top, then sections with knobs, rubber buttons and selectors
 * drawn after the JXMP design guidelines, bound to the plugin's automation
 * controls (drag / wheel, Shift: fine, double-click: default,
 * Ctrl+middle-click: MIDI learn).
 */
class JxPanel : public Gtk::DrawingArea
{
public:
	JxPanel (std::shared_ptr<ARDOUR::PluginInsert>, JxLayout const&);
	~JxPanel ();

	int panel_width () const { return _width; }
	int panel_height () const { return _height; }

protected:
	bool on_expose_event (GdkEventExpose*);
	bool on_button_press_event (GdkEventButton*);
	bool on_button_release_event (GdkEventButton*);
	bool on_motion_notify_event (GdkEventMotion*);
	bool on_scroll_event (GdkEventScroll*);
	bool on_leave_notify_event (GdkEventCrossing*);
	void on_size_request (Gtk::Requisition*);

private:
	struct Cell;

	std::shared_ptr<ARDOUR::PluginInsert> _pi;
	JxLayout const&                       _layout;
	std::vector<Cell*>                    _cells;
	struct Title {
		double      x, y, w;
		std::string text;
	};
	std::vector<Title> _titles;

	int   _width, _height;
	int   _display_h;
	cairo_surface_t* _display;

	int    _drag;     /* cell being dragged, -1: none */
	int    _hover;
	double _drag_y;
	double _drag_start;

	PBD::ScopedConnectionList _connections;

	void layout ();
	uint32_t display_max () const;
	void paint (cairo_t*);
	bool render_display (cairo_t*);
	void draw_cell (cairo_t*, Cell const&, bool hover);
	int  cell_at (double x, double y, int* sub) const;
	void set_cell_value (Cell&, double internal);
	void set_step (Cell&, double value_or_index);
	std::string loaded_file () const;
	void load_sample (std::vector<const char*> const& patterns);
	void redraw ();
};

class JxPluginUI : public PlugUIBase, public Gtk::VBox
{
public:
	JxPluginUI (std::shared_ptr<ARDOUR::PlugInsertBase>);
	~JxPluginUI ();

	/** is there a JXMP panel for this plugin */
	static bool handles (std::shared_ptr<ARDOUR::PlugInsertBase>);

	gint get_preferred_height ();
	gint get_preferred_width ();
	bool resizable () { return false; }
	bool start_updating (GdkEventAny*) { return false; }
	bool stop_updating (GdkEventAny*) { return false; }

private:
	JxPanel* _panel;
};
