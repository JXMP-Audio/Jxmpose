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

#include <ytkmm/box.h>

#include "ardour/session_handle.h"

#include "gtkmm2ext/cairo_widget.h"
#include "widgets/ardour_button.h"
#include "widgets/ardour_dropdown.h"

namespace ARDOUR {
	class Route;
}

/** Master output meter for the editor's right-hand column.
 *
 * Two views: "Level" (sample peak per channel in dBFS, with max-peak
 * readouts) and "Loudness" (EBU R128 momentary / short-term / integrated
 * LUFS, loudness range and max true peak).
 */
class MasterMeterPanel : public Gtk::VBox, public ARDOUR::SessionHandlePtr
{
public:
	MasterMeterPanel ();
	~MasterMeterPanel ();

	void set_session (ARDOUR::Session*);

private:
	class Display : public CairoWidget
	{
	public:
		Display (MasterMeterPanel&);
		void render (Cairo::RefPtr<Cairo::Context> const&, cairo_rectangle_t*);
	private:
		void on_size_request (Gtk::Requisition*);
		void render_level (Cairo::RefPtr<Cairo::Context> const&, double w, double h);
		void render_loudness (Cairo::RefPtr<Cairo::Context> const&, double w, double h);
		MasterMeterPanel& _p;
	};

	void session_going_away ();

	void set_mode (int);
	void set_target (float);
	void custom_target ();
	void update_target_label ();
	void reset ();
	void update ();

	std::shared_ptr<ARDOUR::Route> master () const;

	int _mode; // 0: level, 1: loudness

	ArdourWidgets::ArdourButton _level_tab;
	ArdourWidgets::ArdourButton _loudness_tab;
	ArdourWidgets::ArdourButton _reset_button;
	ArdourWidgets::ArdourDropdown _target_dd;
	Display                     _display;

	/* cached values, refreshed by the timer */
	std::vector<float> _peak;
	std::vector<float> _max_peak;
	float _momentary;
	float _short_term;
	float _integrated;
	float _max_momentary;
	float _max_short_term;
	float _range;
	float _true_peak;

	sigc::connection _timer;
};
