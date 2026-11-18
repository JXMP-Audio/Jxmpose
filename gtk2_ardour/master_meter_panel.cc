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
#include <cstdio>

#include <pangomm/layout.h>
#include <ytkmm/adjustment.h>
#include <ytkmm/label.h>
#include <ytkmm/spinbutton.h>
#include <ytkmm/stock.h>

#include "pbd/compose.h"

#include "ardour/lufs_meter.h"
#include "ardour/meter.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/menu_elems.h"
#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "ardour_dialog.h"
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

void
set_color (Cairo::RefPtr<Cairo::Context> const& cr, char const* name, double alpha = 1.0)
{
	Gtkmm2ext::Color c = UIConfiguration::instance ().color (name);
	double r, g, b, a;
	Gtkmm2ext::color_to_rgba (c, r, g, b, a);
	cr->set_source_rgba (r, g, b, alpha);
}

Glib::RefPtr<Pango::Layout>
layout (Gtk::Widget& w, std::string const& s, Pango::FontDescription const& font)
{
	Glib::RefPtr<Pango::Layout> l = Pango::Layout::create (w.get_pango_context ());
	l->set_font_description (font);
	l->set_text (s);
	return l;
}

int
text_height (Gtk::Widget& w, Pango::FontDescription const& font)
{
	int tw, th;
	layout (w, "-0.9", font)->get_pixel_size (tw, th);
	return th;
}

int
text_width (Gtk::Widget& w, std::string const& s, Pango::FontDescription const& font)
{
	int tw, th;
	layout (w, s, font)->get_pixel_size (tw, th);
	return tw;
}

/* draw text vertically centred on y; align: -1 left edge at x, 0 centred, 1 right edge at x */
void
text (Gtk::Widget& w, Cairo::RefPtr<Cairo::Context> const& cr, std::string const& s,
      Pango::FontDescription const& font, double x, double y, int align = 0)
{
	Glib::RefPtr<Pango::Layout> l = layout (w, s, font);
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

/* draw a dB / LUFS grid with labels; labels that would overlap (or would
 * collide with the `avoid` value's label) are skipped */
void
draw_scale (Gtk::Widget& w, Cairo::RefPtr<Cairo::Context> const& cr, std::vector<float> const& ticks,
            float lo, float hi, double x_label, double x0, double x1, double top, double bottom,
            Pango::FontDescription const& font, float avoid = 1000.f)
{
	double const mh = bottom - top;
	double const th = text_height (w, font);
	double const ya = avoid <= hi ? bottom - frac (avoid, lo, hi) * mh : -1e9;
	double last_y   = -1e9;
	for (float t : ticks) {
		double y = rint (bottom - frac (t, lo, hi) * mh) + .5;
		if (fabs (y - last_y) < th || fabs (y - ya) < th) {
			continue;
		}
		set_color (cr, "neutral:foreground2", .22);
		cr->move_to (x0, y);
		cr->line_to (x1, y);
		cr->set_line_width (1);
		cr->stroke ();
		set_color (cr, "neutral:foreground2");
		text (w, cr, string_compose ("%1", (int)t), font, x_label, y, 1);
		last_y = y;
	}
}

struct TargetPreset {
	float       lufs;
	char const* name;
};

/* commonly published loudness targets (integrated LUFS) */
TargetPreset const target_presets[] = {
	{ -14.f, "Streaming (Spotify, YouTube, Tidal, Amazon)" },
	{ -13.f, "Loud streaming master" },
	{ -15.f, "Deezer" },
	{ -16.f, "Apple Music, podcasts" },
	{ -23.f, "EBU R128 broadcast" },
	{ -24.f, "ATSC A/85 broadcast (US)" },
};

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
	_target_dd.set_name ("generic button");

	_level_tab.signal_clicked.connect (sigc::bind (sigc::mem_fun (*this, &MasterMeterPanel::set_mode), 0));
	_loudness_tab.signal_clicked.connect (sigc::bind (sigc::mem_fun (*this, &MasterMeterPanel::set_mode), 1));
	_reset_button.signal_clicked.connect (sigc::mem_fun (*this, &MasterMeterPanel::reset));

	using namespace Gtk::Menu_Helpers;
	for (TargetPreset const& p : target_presets) {
		std::string label = string_compose ("%1 LUFS   %2", p.lufs, p.name);
		_target_dd.add_menu_elem (Gtkmm2ext::MenuElemNoMnemonic (label, sigc::bind (sigc::mem_fun (*this, &MasterMeterPanel::set_target), p.lufs)));
	}
	_target_dd.add_menu_elem (SeparatorElem ());
	_target_dd.add_menu_elem (MenuElem (_("Custom..."), sigc::mem_fun (*this, &MasterMeterPanel::custom_target)));
	update_target_label ();

	set_tooltip (_level_tab, _("Sample peak level of the master output (dBFS)"));
	set_tooltip (_loudness_tab, _("EBU R128 loudness of the master output (LUFS), loudness range and true peak"));
	set_tooltip (_reset_button, _("Reset peak holds and integrated loudness (also reset when playback starts)"));
	set_tooltip (_target_dd, _("Loudness target (integrated LUFS), drawn as the orange line"));

	Gtk::HBox* tabs = Gtk::manage (new Gtk::HBox (true, 2));
	tabs->pack_start (_level_tab);
	tabs->pack_start (_loudness_tab);

	set_spacing (4);
	set_border_width (2);
	pack_start (*tabs, false, false);
	pack_start (_target_dd, false, false);
	pack_start (_display, true, true);
	pack_start (_reset_button, false, false);

	show_all ();
	_target_dd.set_no_show_all ();
	set_mode (0);
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
	if (m == 1) {
		_target_dd.show ();
	} else {
		_target_dd.hide ();
	}
	_display.queue_draw ();
}

void
MasterMeterPanel::set_target (float t)
{
	UIConfiguration::instance ().set_master_meter_loudness_target (t);
	update_target_label ();
	_display.queue_draw ();
}

void
MasterMeterPanel::update_target_label ()
{
	char buf[64];
	snprintf (buf, sizeof (buf), "%.1f LUFS", UIConfiguration::instance ().get_master_meter_loudness_target ());
	_target_dd.set_text (buf);
}

void
MasterMeterPanel::custom_target ()
{
	ArdourDialog d (_("Loudness Target"), true);
	Gtk::Adjustment adj (UIConfiguration::instance ().get_master_meter_loudness_target (), -40, 0, 0.5, 1);
	Gtk::SpinButton sb (adj, 0.5, 1);
	Gtk::Label      l (_("Integrated loudness target (LUFS):"));
	Gtk::HBox       hb;
	hb.set_spacing (6);
	hb.pack_start (l, false, false);
	hb.pack_start (sb, false, false);
	d.get_vbox ()->pack_start (hb, false, false, 6);
	d.add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
	d.add_button (Gtk::Stock::OK, Gtk::RESPONSE_OK);
	d.set_default_response (Gtk::RESPONSE_OK);
	sb.set_activates_default ();
	d.show_all ();
	if (d.run () == Gtk::RESPONSE_OK) {
		sb.update ();
		set_target (sb.get_value ());
	}
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
	req->width  = 100 * s;
	req->height = 320 * s;
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
	Pango::FontDescription label_font = uic.get_NormalFont ();
	Pango::FontDescription value_font = uic.get_NormalBoldFont ();

	size_t const n = std::max<size_t> (1, _p._peak.size ());

	double const pad     = 8 * s;
	double const vh      = text_height (*this, value_font);
	double const lh      = text_height (*this, label_font);
	double const scale_w = text_width (*this, "-60", label_font) + 8 * s;
	double const top     = pad + vh + 6 * s;  // room for max-peak readouts
	double const bottom  = h - pad - lh - 4 * s;
	/* bars fill the frame with the same margin on both sides */
	double const x0      = rint (pad + scale_w);
	double const x1      = rint (w - pad);
	double const gap     = rint (6 * s);
	double const bw      = std::max (4.0, (x1 - x0 - (n - 1) * gap) / n);
	double const mh      = bottom - top;

	for (size_t i = 0; i < n; ++i) {
		double const x  = rint (x0 + i * (bw + gap));
		float  const pk = i < _p._peak.size () ? _p._peak[i] : -200;
		float  const mx = i < _p._max_peak.size () ? _p._max_peak[i] : -200;

		set_color (cr, "theme:bg2");
		cr->rectangle (x, top, bw, mh);
		cr->fill ();

		/* colour by level zone */
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
			cr->set_line_width (1);
			cr->stroke ();
		}

		/* max peak readout */
		set_color (cr, mx > 0 ? "alert:red" : "neutral:foregroundest");
		text (*this, cr, fmt1 (mx), value_font, x + bw / 2, pad + vh / 2, 0);

		/* channel label */
		set_color (cr, "neutral:foreground2");
		std::string lbl = n == 2 ? (i == 0 ? "L" : "R") : string_compose ("%1", i + 1);
		text (*this, cr, lbl, label_font, x + bw / 2, bottom + 4 * s + lh / 2, 0);
	}

	static const std::vector<float> ticks = { 6, 0, -3, -6, -12, -18, -24, -30, -40, -50, -60 };
	draw_scale (*this, cr, ticks, level_lo, level_hi, x0 - 5 * s, x0, x1, top, bottom, label_font);

	set_color (cr, "neutral:foreground2");
	text (*this, cr, "dBFS", label_font, x0 - 5 * s, bottom + 4 * s + lh / 2, 1);
}

void
MasterMeterPanel::Display::render_loudness (Cairo::RefPtr<Cairo::Context> const& cr, double w, double h)
{
	UIConfiguration& uic (UIConfiguration::instance ());
	float const s      = uic.get_ui_scale ();
	float const target = uic.get_master_meter_loudness_target ();

	Pango::FontDescription label_font = uic.get_NormalFont ();
	Pango::FontDescription value_font = uic.get_NormalBoldFont ();
	Pango::FontDescription big_font   = uic.get_LargerBoldFont ();

	double const pad = 8 * s;
	double const lh  = text_height (*this, label_font);
	double const vh  = text_height (*this, value_font);
	double const bh  = text_height (*this, big_font);

	/* big integrated readout: title / value / difference to target */
	double y = pad + lh / 2;
	set_color (cr, "neutral:foreground2");
	text (*this, cr, _("Integrated LUFS"), label_font, w / 2, y, 0);
	y += lh / 2 + 2 * s + bh / 2;
	bool const have_i = _p._integrated > -70;
	set_color (cr, "neutral:foregroundest");
	text (*this, cr, fmt1 (_p._integrated), big_font, w / 2, y, 0);
	y += bh / 2 + 2 * s + lh / 2;
	if (have_i) {
		float d = _p._integrated - target;
		char buf[32];
		snprintf (buf, sizeof (buf), "%+.1f LU", d);
		set_color (cr, d > 1.f ? "alert:red" : (d < -1.f ? "widget:orange" : "alert:green"));
		text (*this, cr, buf, label_font, w / 2, y, 0);
	}
	y += lh / 2;

	/* value table at the bottom */
	struct Item { std::string label; std::string value; bool warn; };
	std::vector<Item> items = {
		{ _("Momentary"), fmt1 (_p._momentary), false },
		{ _("Short-term"), fmt1 (_p._short_term), false },
		{ _("Max momentary"), fmt1 (_p._max_momentary), false },
		{ _("Max short-term"), fmt1 (_p._max_short_term), false },
		{ _("Range (LU)"), have_i ? fmt1 (_p._range, -1000) : "-", false },
		{ _("True peak (dBTP)"), fmt1 (_p._true_peak), _p._true_peak > -1.f },
	};
	double const row     = std::max (lh, vh) + 5 * s;
	double const table_h = items.size () * row;
	double ty = h - pad - table_h + row / 2;

	set_color (cr, "neutral:foreground2", .22);
	cr->move_to (pad, rint (h - pad - table_h - 3 * s) + .5);
	cr->line_to (w - pad, rint (h - pad - table_h - 3 * s) + .5);
	cr->set_line_width (1);
	cr->stroke ();

	for (Item const& it : items) {
		set_color (cr, "neutral:foreground2");
		text (*this, cr, it.label, label_font, pad, ty, -1);
		set_color (cr, it.warn ? "alert:red" : "neutral:foregroundest");
		text (*this, cr, it.value, value_font, w - pad, ty, 1);
		ty += row;
	}

	/* M / S / I bars */
	double const top     = y + 8 * s;
	double const bottom  = h - pad - table_h - 10 * s - lh;
	double const mh      = bottom - top;
	if (mh < 40 * s) {
		return;
	}
	double const scale_w = text_width (*this, "-50", label_font) + 8 * s;
	double const x0      = rint (pad + scale_w);
	double const x1      = rint (w - pad);
	double const gap     = rint (6 * s);
	double const bw      = std::max (4.0, (x1 - x0 - 2 * gap) / 3);

	struct Bar { char const* label; float v; char const* col; };
	Bar const bars[] = {
		{ "M", _p._momentary,  "widget:blue lighter" },
		{ "S", _p._short_term, "widget:blue" },
		{ "I", _p._integrated, "alert:greenish" },
	};
	for (int i = 0; i < 3; ++i) {
		double const x = rint (x0 + i * (bw + gap));
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
		text (*this, cr, bars[i].label, label_font, x + bw / 2, bottom + 4 * s + lh / 2, 0);
	}

	static const std::vector<float> ticks = { 0, -6, -10, -20, -30, -40, -50 };
	draw_scale (*this, cr, ticks, lufs_lo, lufs_hi, x0 - 5 * s, x0, x1, top, bottom, label_font, target);

	/* target line on top */
	double const yt = rint (bottom - frac (target, lufs_lo, lufs_hi) * mh) + .5;
	set_color (cr, "widget:orange");
	cr->set_line_width (2 * s);
	cr->move_to (x0 - 3 * s, yt);
	cr->line_to (x1, yt);
	cr->stroke ();
	text (*this, cr, fmt1 (target), label_font, x0 - 5 * s, yt, 1);
}
