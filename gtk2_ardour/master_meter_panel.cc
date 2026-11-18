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
#include <cmath>

#include <pangomm/layout.h>

#include "pbd/compose.h"

#include "ardour/lufs_meter.h"
#include "ardour/meter.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "master_meter_panel.h"
#include "timers.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourWidgets;

namespace {

const float level_lo = -60.f;
const float level_hi = 6.f;
const float lufs_lo  = -50.f;
const float lufs_hi  = 0.f;
const float lufs_target = -14.f;

void
set_color (Cairo::RefPtr<Cairo::Context> const& cr, char const* name, double alpha = 1.0)
{
	Gtkmm2ext::Color c = UIConfiguration::instance ().color (name);
	double r, g, b, a;
	Gtkmm2ext::color_to_rgba (c, r, g, b, a);
	cr->set_source_rgba (r, g, b, alpha);
}

/* draw text; x is left edge, or right edge when right_align, or center when center */
void
text (Gtk::Widget& w, Cairo::RefPtr<Cairo::Context> const& cr, std::string const& s,
      Pango::FontDescription const& font, double x, double y, int align = 0)
{
	Glib::RefPtr<Pango::Layout> l = Pango::Layout::create (w.get_pango_context ());
	l->set_font_description (font);
	l->set_text (s);
	int tw, th;
	l->get_pixel_size (tw, th);
	if (align > 0) {
		x -= tw;
	} else if (align == 0) {
		x -= tw / 2.0;
	}
	cr->move_to (rint (x), rint (y - th / 2.0));
	l->show_in_cairo_context (cr);
}

std::string
fmt1 (float v, float floor_v = -70.f)
{
	if (v <= floor_v || !std::isfinite (v)) {
		return "-∞";
	}
	char buf[32];
	snprintf (buf, sizeof (buf), "%.1f", v);
	return buf;
}

double
frac (float v, float lo, float hi)
{
	if (!std::isfinite (v)) {
		return 0;
	}
	return std::max (0.0, std::min (1.0, (double)(v - lo) / (hi - lo)));
}

} // namespace

MasterMeterPanel::MasterMeterPanel ()
	: _mode (0)
	, _level_tab (_("Level"), ArdourButton::default_elements, true)
	, _loudness_tab (_("Loudness"), ArdourButton::default_elements, true)
	, _reset_button (_("Reset"))
	, _display (*this)
	, _momentary (-200)
	, _short_term (-200)
	, _integrated (-200)
	, _max_momentary (-200)
	, _max_short_term (-200)
	, _range (0)
	, _true_peak (-200)
{
	_level_tab.set_name ("tab button");
	_loudness_tab.set_name ("tab button");
	_reset_button.set_name ("generic button");

	_level_tab.signal_clicked.connect (sigc::bind (sigc::mem_fun (*this, &MasterMeterPanel::set_mode), 0));
	_loudness_tab.signal_clicked.connect (sigc::bind (sigc::mem_fun (*this, &MasterMeterPanel::set_mode), 1));
	_reset_button.signal_clicked.connect (sigc::mem_fun (*this, &MasterMeterPanel::reset));

	set_tooltip (_level_tab, _("Sample peak level of the master output (dBFS)"));
	set_tooltip (_loudness_tab, _("EBU R128 loudness of the master output (LUFS), loudness range and true peak"));
	set_tooltip (_reset_button, _("Reset peak holds and integrated loudness (also reset when playback starts)"));

	Gtk::HBox* tabs = Gtk::manage (new Gtk::HBox (true, 2));
	tabs->pack_start (_level_tab);
	tabs->pack_start (_loudness_tab);

	set_spacing (4);
	set_border_width (2);
	pack_start (*tabs, false, false);
	pack_start (_display, true, true);
	pack_start (_reset_button, false, false);

	set_mode (0);
	show_all ();
}

MasterMeterPanel::~MasterMeterPanel ()
{
	_timer.disconnect ();
}

void
MasterMeterPanel::set_session (Session* s)
{
	SessionHandlePtr::set_session (s);
	_timer.disconnect ();
	if (_session) {
		_timer = Timers::super_rapid_connect (sigc::mem_fun (*this, &MasterMeterPanel::update));
	}
	_display.queue_draw ();
}

void
MasterMeterPanel::session_going_away ()
{
	_timer.disconnect ();
	SessionHandlePtr::session_going_away ();
	_peak.clear ();
	_max_peak.clear ();
	_display.queue_draw ();
}

std::shared_ptr<Route>
MasterMeterPanel::master () const
{
	return _session ? _session->master_out () : std::shared_ptr<Route> ();
}

void
MasterMeterPanel::set_mode (int m)
{
	_mode = m;
	_level_tab.set_active (m == 0);
	_loudness_tab.set_active (m == 1);
	_display.queue_draw ();
}

void
MasterMeterPanel::reset ()
{
	std::shared_ptr<Route> r = master ();
	if (!r) {
		return;
	}
	r->peak_meter ()->reset_max ();
	if (LUFSMeter* lm = r->loudness_meter ()) {
		lm->reset ();
	}
}

void
MasterMeterPanel::update ()
{
	if (!is_mapped ()) {
		return;
	}
	std::shared_ptr<Route> r = master ();
	if (!r) {
		return;
	}
	std::shared_ptr<PeakMeter> pm = r->peak_meter ();
	uint32_t n = pm->input_streams ().n_audio ();
	_peak.resize (n);
	_max_peak.resize (n);
	for (uint32_t i = 0; i < n; ++i) {
		_peak[i]     = pm->meter_level (i, MeterPeak);
		_max_peak[i] = pm->meter_level (i, MeterMaxPeak);
	}
	if (LUFSMeter* lm = r->loudness_meter ()) {
		_momentary      = lm->momentary ();
		_short_term     = lm->short_term ();
		_integrated     = lm->integrated_loudness ();
		_max_momentary  = lm->max_momentary ();
		_max_short_term = lm->max_short_term ();
		_range          = lm->lu_range ();
		_true_peak      = lm->max_dbtp ();
	}
	_display.queue_draw ();
}

/* ****************************************************************************/

MasterMeterPanel::Display::Display (MasterMeterPanel& p)
	: _p (p)
{
}

void
MasterMeterPanel::Display::on_size_request (Gtk::Requisition* req)
{
	float s = UIConfiguration::instance ().get_ui_scale ();
	req->width  = 90 * s;
	req->height = 260 * s;
}

void
MasterMeterPanel::Display::render (Cairo::RefPtr<Cairo::Context> const& cr, cairo_rectangle_t*)
{
	double const w = get_width ();
	double const h = get_height ();

	set_color (cr, "theme:bg1");
	cr->rectangle (0, 0, w, h);
	cr->fill ();

	if (!_p._session) {
		return;
	}
	if (_p._mode == 0) {
		render_level (cr, w, h);
	} else {
		render_loudness (cr, w, h);
	}
}

void
MasterMeterPanel::Display::render_level (Cairo::RefPtr<Cairo::Context> const& cr, double w, double h)
{
	UIConfiguration& uic (UIConfiguration::instance ());
	float const s = uic.get_ui_scale ();
	Pango::FontDescription small = uic.get_SmallFont ();
	Pango::FontDescription tiny  = uic.get_SmallerFont ();

	size_t const n = std::max<size_t> (1, _p._peak.size ());

	double const pad    = 6 * s;
	double const scale_w = 24 * s;
	double const top    = 26 * s;  // room for max-peak readouts
	double const bottom = h - 18 * s;
	double const x0     = pad + scale_w;
	double const bw     = std::max (4.0, (w - x0 - pad - (n - 1) * 2 * s) / n);
	double const mh     = bottom - top;

	for (size_t i = 0; i < n; ++i) {
		double const x = rint (x0 + i * (bw + 2 * s));
		float  const pk = i < _p._peak.size () ? _p._peak[i] : -200;
		float  const mx = i < _p._max_peak.size () ? _p._max_peak[i] : -200;

		set_color (cr, "theme:bg2");
		cr->rectangle (x, top, bw, mh);
		cr->fill ();

		/* segmented colour by level zone */
		struct Zone { float lo, hi; char const* col; };
		static const Zone zones[] = {
			{ level_lo, -18.f, "meter color1" },
			{ -18.f, -6.f, "meter color4" },
			{ -6.f, 0.f, "meter color6" },
			{ 0.f, level_hi, "meter color8" },
		};
		for (Zone const& z : zones) {
			if (pk <= z.lo) {
				break;
			}
			double y1 = bottom - frac (std::min (pk, z.hi), level_lo, level_hi) * mh;
			double y0 = bottom - frac (z.lo, level_lo, level_hi) * mh;
			set_color (cr, z.col);
			cr->rectangle (x, y1, bw, y0 - y1);
			cr->fill ();
		}

		/* max peak hold line */
		if (mx > level_lo) {
			double y = rint (bottom - frac (mx, level_lo, level_hi) * mh) + .5;
			set_color (cr, mx > 0 ? "alert:red" : "neutral:foreground");
			cr->move_to (x, y);
			cr->line_to (x + bw, y);
			cr->stroke ();
		}

		/* max peak readout */
		set_color (cr, mx > 0 ? "alert:red" : "neutral:foreground");
		text (*this, cr, fmt1 (mx), tiny, x + bw / 2, top / 2, 0);

		/* channel label */
		set_color (cr, "neutral:foreground2");
		std::string lbl = n == 2 ? (i == 0 ? "L" : "R") : string_compose ("%1", i + 1);
		text (*this, cr, lbl, small, x + bw / 2, (bottom + h) / 2, 0);
	}

	/* dB scale */
	static const float ticks[] = { 6, 0, -6, -12, -18, -24, -30, -40, -50, -60 };
	for (float t : ticks) {
		double y = rint (bottom - frac (t, level_lo, level_hi) * mh) + .5;
		set_color (cr, "neutral:foreground2", .25);
		cr->move_to (x0 - 3 * s, y);
		cr->line_to (w - pad, y);
		cr->set_line_width (1);
		cr->stroke ();
		set_color (cr, "neutral:foreground2");
		text (*this, cr, string_compose ("%1", (int)t), tiny, x0 - 5 * s, y, 1);
	}

	set_color (cr, "neutral:foreground2");
	text (*this, cr, "dB", tiny, pad, (bottom + h) / 2, -1);
}

void
MasterMeterPanel::Display::render_loudness (Cairo::RefPtr<Cairo::Context> const& cr, double w, double h)
{
	UIConfiguration& uic (UIConfiguration::instance ());
	float const s = uic.get_ui_scale ();
	Pango::FontDescription small = uic.get_SmallFont ();
	Pango::FontDescription tiny  = uic.get_SmallerFont ();
	Pango::FontDescription big   = uic.get_LargeBoldFont ();

	double const pad = 6 * s;
	double const row = 16 * s;

	/* big integrated readout */
	double y = pad + 12 * s;
	set_color (cr, "neutral:foreground2");
	text (*this, cr, _("Integrated"), tiny, w / 2, y, 0);
	y += 16 * s;
	set_color (cr, "neutral:foregroundest");
	text (*this, cr, fmt1 (_p._integrated), big, w / 2, y, 0);
	y += 13 * s;
	set_color (cr, "neutral:foreground2");
	text (*this, cr, "LUFS", tiny, w / 2, y, 0);

	/* value table at the bottom */
	struct Item { char const* label; std::string value; bool warn; };
	std::vector<Item> items = {
		{ _("Momentary"), fmt1 (_p._momentary), false },
		{ _("Short-Term"), fmt1 (_p._short_term), false },
		{ _("Max M"), fmt1 (_p._max_momentary), false },
		{ _("Max S"), fmt1 (_p._max_short_term), false },
		{ _("Range LU"), _p._integrated > -70 ? fmt1 (_p._range, -1000) : "-", false },
		{ _("True Peak"), fmt1 (_p._true_peak), _p._true_peak > -1.f },
	};
	double const table_h = items.size () * row + pad;
	double ty = h - table_h + row / 2;
	for (Item const& it : items) {
		set_color (cr, "neutral:foreground2");
		text (*this, cr, it.label, tiny, pad, ty, -1);
		set_color (cr, it.warn ? "alert:red" : "neutral:foreground");
		text (*this, cr, it.value, small, w - pad, ty, 1);
		ty += row;
	}

	/* M / S / I bars */
	double const top    = y + 14 * s;
	double const bottom = h - table_h - 16 * s;
	double const mh     = bottom - top;
	if (mh < 20 * s) {
		return;
	}
	double const scale_w = 24 * s;
	double const x0      = pad + scale_w;
	double const bw      = std::max (4.0, (w - x0 - pad - 4 * s) / 3);

	struct Bar { char const* label; float v; char const* col; };
	Bar const bars[] = {
		{ "M", _p._momentary,  "widget:blue lighter" },
		{ "S", _p._short_term, "widget:blue" },
		{ "I", _p._integrated, "alert:greenish" },
	};
	for (int i = 0; i < 3; ++i) {
		double const x = rint (x0 + i * (bw + 2 * s));
		set_color (cr, "theme:bg2");
		cr->rectangle (x, top, bw, mh);
		cr->fill ();
		double f = frac (bars[i].v, lufs_lo, lufs_hi);
		if (f > 0) {
			set_color (cr, bars[i].col);
			cr->rectangle (x, bottom - f * mh, bw, f * mh);
			cr->fill ();
		}
		set_color (cr, "neutral:foreground2");
		text (*this, cr, bars[i].label, small, x + bw / 2, bottom + 8 * s, 0);
	}

	static const float ticks[] = { 0, -10, -14, -20, -23, -30, -40, -50 };
	for (float t : ticks) {
		double ty2 = rint (bottom - frac (t, lufs_lo, lufs_hi) * mh) + .5;
		bool tgt = (t == lufs_target);
		set_color (cr, tgt ? "widget:orange" : "neutral:foreground2", tgt ? 1.0 : .25);
		cr->move_to (x0 - 3 * s, ty2);
		cr->line_to (w - pad, ty2);
		cr->set_line_width (1);
		cr->stroke ();
		set_color (cr, tgt ? "widget:orange" : "neutral:foreground2");
		text (*this, cr, string_compose ("%1", (int)t), tiny, x0 - 5 * s, ty2, 1);
	}

}
