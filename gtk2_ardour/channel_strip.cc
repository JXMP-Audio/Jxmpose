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

#include <cmath>
#include <complex>

#include <ytkmm/table.h>

#include "pbd/compose.h"

#include "ardour/amp.h"
#include "ardour/automation_control.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "ardour_ui.h"
#include "channel_strip.h"
#include "gui_object.h"
#include "gui_thread.h"
#include "timers.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourWidgets;

namespace {

char const* const module_key[ChannelStrip::NModules]  = { "gate", "eq", "comp" };
char const* const mono_uri[ChannelStrip::NModules]    = { "urn:ardour:a-exp", "urn:ardour:a-eq", "urn:ardour:a-comp" };
char const* const stereo_uri[ChannelStrip::NModules]  = { "urn:ardour:a-exp#stereo", "urn:ardour:a-eq", "urn:ardour:a-comp#stereo" };

/* ACE plugin ports */
enum {
	ExpThreshold = 4, ExpRelease = 1,
	CompRatio = 3, CompThreshold = 4, CompMakeup = 5, CompGR = 6,
	EqFreqL = 0, EqGainL = 1, EqFreq1 = 2, EqGain1 = 3, EqBw1 = 4,
	EqFreq2 = 5, EqGain2 = 6, EqBw2 = 7, EqFreq3 = 8, EqGain3 = 9, EqBw3 = 10,
	EqFreq4 = 11, EqGain4 = 12, EqBw4 = 13, EqFreqH = 14, EqGainH = 15, EqMaster = 16,
	EqTogL = 17, EqTog1 = 18, EqTogH = 22,
};

std::string
state_id (std::shared_ptr<Route> r)
{
	return string_compose (X_("channel-strip %1"), r->id ().to_s ());
}

/* RBJ biquads: magnitude in dB at f */
double
biquad_db (double b0, double b1, double b2, double a0, double a1, double a2, double f, double fs)
{
	std::complex<double> const z  = std::polar (1.0, -2.0 * M_PI * f / fs);
	std::complex<double> const z2 = z * z;
	std::complex<double> const h  = (b0 + b1 * z + b2 * z2) / (a0 + a1 * z + a2 * z2);
	return 20.0 * log10 (std::max (1e-9, std::abs (h)));
}

double
peak_db (double f0, double g, double bw, double f, double fs)
{
	double const A     = pow (10.0, g / 40.0);
	double const w0    = 2.0 * M_PI * f0 / fs;
	double const alpha = sin (w0) * sinh (log (2.0) / 2.0 * bw * w0 / sin (w0));
	return biquad_db (1 + alpha * A, -2 * cos (w0), 1 - alpha * A, 1 + alpha / A, -2 * cos (w0), 1 - alpha / A, f, fs);
}

double
shelf_db (bool low, double f0, double g, double f, double fs)
{
	double const A     = pow (10.0, g / 40.0);
	double const w0    = 2.0 * M_PI * f0 / fs;
	double const alpha = sin (w0) / 2.0 * sqrt (2.0);
	double const c = cos (w0), sA = 2 * sqrt (A) * alpha;
	if (low) {
		return biquad_db (A * ((A + 1) - (A - 1) * c + sA), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sA),
		                  (A + 1) + (A - 1) * c + sA, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sA, f, fs);
	}
	return biquad_db (A * ((A + 1) + (A - 1) * c + sA), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sA),
	                  (A + 1) - (A - 1) * c + sA, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sA, f, fs);
}

} // namespace

bool
ChannelStrip::is_strip_processor (std::shared_ptr<Route> r, std::shared_ptr<Processor> p)
{
	if (!r || !p || !ARDOUR_UI::instance () || !ARDOUR_UI::instance ()->gui_object_state) {
		return false;
	}
	GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
	std::string const id  = p->id ().to_s ();
	std::string const sid = state_id (r);
	for (int m = 0; m < NModules; ++m) {
		if (gs.get_string (sid, module_key[m]) == id) {
			return true;
		}
	}
	return false;
}

ChannelStrip::ChannelStrip ()
	: _menu (0)
	, _gr (0)
{
	set_spacing (3);

	static char const* const names[NModules] = { N_("Gate"), N_("EQ"), N_("Comp") };
	static char const* const tips[NModules]  = {
		N_("Gate (expander) before the EQ. Click: on/off (the first time it is added), right-click: remove or edit"),
		N_("Four band EQ with low and high shelf. Click: on/off (the first time it is added), right-click: remove or edit; double-click the curve for all bands"),
		N_("Compressor after the EQ, with a gain reduction meter. Click: on/off (the first time it is added), right-click: remove or edit"),
	};
	_buttons.set_homogeneous (true);
	_buttons.set_spacing (2);
	for (int m = 0; m < NModules; ++m) {
		_button[m].set_text (_(names[m]));
		_button[m].set_name (X_("generic button"));
		_button[m].signal_button_press_event ().connect (sigc::bind (sigc::mem_fun (*this, &ChannelStrip::button_press), (Module) m), false);
		set_tooltip (_button[m], _(tips[m]));
		_buttons.pack_start (_button[m], true, true);
	}
	pack_start (_buttons, false, false);

	float const scale = UIConfiguration::instance ().get_ui_scale ();
	_curve.set_size_request (-1, rintf (44.f * scale));
	_curve.add_events (Gdk::BUTTON_PRESS_MASK);
	_curve.signal_expose_event ().connect (sigc::mem_fun (*this, &ChannelStrip::curve_expose));
	_curve.signal_button_press_event ().connect (sigc::mem_fun (*this, &ChannelStrip::curve_press));
	_gr_meter.set_size_request (-1, rintf (5.f * scale));
	_gr_meter.signal_expose_event ().connect (sigc::mem_fun (*this, &ChannelStrip::gr_expose));
	set_tooltip (_gr_meter, _("Gain reduction"));

	for (int m = 0; m < NModules; ++m) {
		_body[m].set_spacing (1);
		_body[m].set_no_show_all ();
		pack_start (_body[m], false, false);
	}

	_buttons.show_all ();
	rebuild ();
	show ();
}

ChannelStrip::~ChannelStrip ()
{
	_meter_connection.disconnect ();
	delete _menu;
}

void
ChannelStrip::set_route (std::shared_ptr<Route> r)
{
	_route_connections.drop_connections ();
	_route = r;
	if (_route) {
		_route->processors_changed.connect (_route_connections, invalidator (*this), std::bind (&ChannelStrip::rebuild, this), gui_context ());
		_route->DropReferences.connect (_route_connections, invalidator (*this), std::bind (&ChannelStrip::set_route, this, std::shared_ptr<Route> ()), gui_context ());
	}
	rebuild ();
}

std::shared_ptr<Processor>
ChannelStrip::find (Module m) const
{
	if (!_route) {
		return std::shared_ptr<Processor> ();
	}
	std::string const id = ARDOUR_UI::instance ()->gui_object_state->get_string (state_id (_route), module_key[m]);
	if (id.empty ()) {
		return std::shared_ptr<Processor> ();
	}
	return _route->processor_by_id (PBD::ID (id));
}

std::shared_ptr<AutomationControl>
ChannelStrip::control (Module m, uint32_t param) const
{
	if (!_proc[m]) {
		return std::shared_ptr<AutomationControl> ();
	}
	return _proc[m]->automation_control (Evoral::Parameter (PluginAutomation, 0, param));
}

void
ChannelStrip::add_knob (Gtk::Table& t, int col, Module m, uint32_t param, std::string const& label)
{
	std::shared_ptr<AutomationControl> c = control (m, param);
	ArdourKnob* k = manage (new ArdourKnob (ArdourKnob::default_elements, ArdourKnob::Detent));
	float const scale = UIConfiguration::instance ().get_ui_scale ();
	k->set_size_request (rintf (22.f * scale), rintf (22.f * scale));
	if (c) {
		k->set_controllable (c);
		k->set_tooltip_prefix (label + ": ");
		if (m == EQ) {
			c->Changed.connect (_proc_connections, invalidator (*this), std::bind (&Gtk::Widget::queue_draw, &_curve), gui_context ());
		}
	}
	Gtk::Label* l = manage (new Gtk::Label (label));
	l->modify_font (UIConfiguration::instance ().get_SmallFont ());
	t.attach (*k, col, col + 1, 0, 1, Gtk::SHRINK, Gtk::SHRINK);
	t.attach (*l, col, col + 1, 1, 2, Gtk::FILL, Gtk::SHRINK);
}

void
ChannelStrip::rebuild ()
{
	_proc_connections.drop_connections ();
	_meter_connection.disconnect ();

	for (int m = 0; m < NModules; ++m) {
		_proc[m] = find ((Module) m);
		if (_proc[m]) {
			_proc[m]->ActiveChanged.connect (_proc_connections, invalidator (*this), std::bind (&ChannelStrip::update_buttons, this), gui_context ());
		}
		/* empty the module body (keep the shared curve and meter widgets) */
		std::vector<Gtk::Widget*> ch = _body[m].get_children ();
		for (auto w : ch) {
			_body[m].remove (*w);
		}
		_body[m].hide ();
	}

	if (_proc[Gate]) {
		Gtk::Table* t = manage (new Gtk::Table (2, 4, true));
		add_knob (*t, 0, Gate, ExpThreshold, _("Thr"));
		add_knob (*t, 1, Gate, ExpRelease, _("Rel"));
		_body[Gate].pack_start (*t, false, false);
	}
	if (_proc[EQ]) {
		_body[EQ].pack_start (_curve, false, false);
		Gtk::Table* t = manage (new Gtk::Table (2, 4, true));
		add_knob (*t, 0, EQ, EqGainL, _("Low"));
		add_knob (*t, 1, EQ, EqGain1, _("LoMid"));
		add_knob (*t, 2, EQ, EqGain3, _("HiMid"));
		add_knob (*t, 3, EQ, EqGainH, _("High"));
		_body[EQ].pack_start (*t, false, false);
		/* the curve follows every band */
		for (uint32_t p = EqFreqL; p <= EqTogH; ++p) {
			std::shared_ptr<AutomationControl> c = control (EQ, p);
			if (c) {
				c->Changed.connect (_proc_connections, invalidator (*this), std::bind (&Gtk::Widget::queue_draw, &_curve), gui_context ());
			}
		}
	}
	if (_proc[Comp]) {
		Gtk::Table* t = manage (new Gtk::Table (2, 4, true));
		add_knob (*t, 0, Comp, CompThreshold, _("Thr"));
		add_knob (*t, 1, Comp, CompRatio, _("Ratio"));
		add_knob (*t, 2, Comp, CompMakeup, _("Gain"));
		_body[Comp].pack_start (_gr_meter, false, false);
		_body[Comp].pack_start (*t, false, false);
		_meter_connection = Timers::rapid_connect (sigc::mem_fun (*this, &ChannelStrip::update_meter));
	}

	for (int m = 0; m < NModules; ++m) {
		if (_proc[m]) {
			for (auto w : _body[m].get_children ()) {
				w->show_all ();
			}
			_body[m].show ();
		}
	}
	update_buttons ();
	_curve.queue_draw ();
}

void
ChannelStrip::update_buttons ()
{
	for (int m = 0; m < NModules; ++m) {
		_button[m].set_active (_proc[m] && _proc[m]->enabled ());
		_body[m].set_sensitive (_proc[m] && _proc[m]->enabled ());
		_button[m].set_sensitive ((bool) _route);
	}
}

bool
ChannelStrip::button_press (GdkEventButton* ev, Module m)
{
	if (!_route || ev->type != GDK_BUTTON_PRESS) {
		return true;
	}
	if (ev->button == 1) {
		toggle (m);
		return true;
	}
	if (ev->button == 3 && _proc[m]) {
		delete _menu;
		_menu = new Gtk::Menu;
		std::weak_ptr<Processor> wp (_proc[m]);
		_menu->items ().push_back (Gtk::Menu_Helpers::MenuElem (_("Edit..."), [wp] () {
			std::shared_ptr<Processor> p = wp.lock ();
			if (p) {
				p->ShowUI (); /* EMIT SIGNAL */
			}
		}));
		_menu->items ().push_back (Gtk::Menu_Helpers::MenuElem (_("Remove"), sigc::bind (sigc::mem_fun (*this, &ChannelStrip::remove), m)));
		_menu->popup (ev->button, ev->time);
		return true;
	}
	return true;
}

void
ChannelStrip::toggle (Module m)
{
	if (!_proc[m]) {
		insert (m);
		return;
	}
	_proc[m]->enable (!_proc[m]->enabled ());
	_route->session ().set_dirty ();
}

void
ChannelStrip::insert (Module m)
{
	/* gate, EQ, compressor in this order right before the fader */
	std::shared_ptr<Processor> before;
	for (int n = m + 1; n < NModules && !before; ++n) {
		before = _proc[n];
	}
	if (!before) {
		before = _route->amp ();
	}

	bool const stereo = _route->amp ()->input_streams ().n_audio () >= 2;
	PluginPtr  pp     = find_plugin (_route->session (), stereo ? stereo_uri[m] : mono_uri[m], ARDOUR::LV2);
	if (!pp) {
		pp = find_plugin (_route->session (), mono_uri[m], ARDOUR::LV2);
	}
	if (!pp) {
		_button[m].set_sensitive (false);
		set_tooltip (_button[m], _("The bundled plugin for this module is not installed"));
		return;
	}

	std::shared_ptr<Processor> p (new PluginInsert (_route->session (), *_route, pp));

	/* remember it before the route announces the change, so that no
	 * insert list shows it */
	ARDOUR_UI::instance ()->gui_object_state->set_property (state_id (_route), module_key[m], p->id ().to_s ());

	if (_route->add_processor (p, before)) {
		ARDOUR_UI::instance ()->gui_object_state->set_property (state_id (_route), module_key[m], std::string ());
		return;
	}
	_route->session ().set_dirty ();
	rebuild ();
}

void
ChannelStrip::remove (Module m)
{
	std::shared_ptr<Processor> p = _proc[m];
	if (!p) {
		return;
	}
	ARDOUR_UI::instance ()->gui_object_state->set_property (state_id (_route), module_key[m], std::string ());
	_route->remove_processor (p);
	_route->session ().set_dirty ();
	rebuild ();
}

bool
ChannelStrip::curve_press (GdkEventButton* ev)
{
	if (ev->type == GDK_2BUTTON_PRESS && _proc[EQ]) {
		_proc[EQ]->ShowUI (); /* EMIT SIGNAL */
		return true;
	}
	return false;
}

bool
ChannelStrip::curve_expose (GdkEventExpose*)
{
	Cairo::RefPtr<Cairo::Context> cr = _curve.get_window ()->create_cairo_context ();
	Gtk::Allocation const a = _curve.get_allocation ();
	double const w = a.get_width (), h = a.get_height ();

	cr->set_source_rgb (0.08, 0.09, 0.10);
	cr->rectangle (0, 0, w, h);
	cr->fill ();

	/* grid: 100 Hz, 1 kHz, 10 kHz; 0 dB */
	double const fmin = 20, fmax = 20000, range = 18;
	auto x_of = [&] (double f) { return w * log (f / fmin) / log (fmax / fmin); };
	auto y_of = [&] (double db) { return h / 2 - db / range * (h / 2 - 2); };
	cr->set_source_rgba (1, 1, 1, 0.12);
	cr->set_line_width (1);
	for (double f : { 100.0, 1000.0, 10000.0 }) {
		cr->move_to (rint (x_of (f)) + 0.5, 0);
		cr->line_to (rint (x_of (f)) + 0.5, h);
	}
	cr->move_to (0, rint (h / 2) + 0.5);
	cr->line_to (w, rint (h / 2) + 0.5);
	cr->stroke ();

	if (!_proc[EQ]) {
		return true;
	}

	auto val = [&] (uint32_t p, double dflt) {
		std::shared_ptr<AutomationControl> c = control (EQ, p);
		return c ? c->get_value () : dflt;
	};
	double const fs = _route ? _route->session ().nominal_sample_rate () : 48000;
	bool const   on = _proc[EQ]->enabled ();

	cr->set_line_width (1.5);
	cr->move_to (0, y_of (0));
	for (int px = 0; px <= (int) w; ++px) {
		double const f = fmin * pow (fmax / fmin, px / w);
		double       db = val (EqMaster, 0);
		if (val (EqTogL, 1) > 0.5) {
			db += shelf_db (true, val (EqFreqL, 160), val (EqGainL, 0), f, fs);
		}
		for (int b = 0; b < 4; ++b) {
			if (val (EqTog1 + b, 1) > 0.5) {
				db += peak_db (val (EqFreq1 + 3 * b, 300), val (EqGain1 + 3 * b, 0), val (EqBw1 + 3 * b, 1), f, fs);
			}
		}
		if (val (EqTogH, 1) > 0.5) {
			db += shelf_db (false, val (EqFreqH, 9000), val (EqGainH, 0), f, fs);
		}
		cr->line_to (px, std::max (1.0, std::min (h - 1, y_of (on ? db : 0))));
	}
	Gtkmm2ext::set_source_rgba (cr, UIConfiguration::instance ().color (X_("theme:contrasting alt")));
	if (!on) {
		cr->set_source_rgba (0.5, 0.5, 0.5, 0.8);
	}
	cr->stroke ();
	return true;
}

void
ChannelStrip::update_meter ()
{
	if (!_proc[Comp]) {
		return;
	}
	std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (_proc[Comp]);
	if (!pi) {
		return;
	}
	float const gr = _proc[Comp]->enabled () ? pi->plugin ()->get_parameter (CompGR) : 0;
	if (fabsf (gr - _gr) > 0.1f) {
		_gr = gr;
		_gr_meter.queue_draw ();
	}
}

bool
ChannelStrip::gr_expose (GdkEventExpose*)
{
	Cairo::RefPtr<Cairo::Context> cr = _gr_meter.get_window ()->create_cairo_context ();
	Gtk::Allocation const a = _gr_meter.get_allocation ();
	double const w = a.get_width (), h = a.get_height ();
	cr->set_source_rgb (0.08, 0.09, 0.10);
	cr->rectangle (0, 0, w, h);
	cr->fill ();
	/* reduction grows from the right, 0..20 dB */
	double const len = w * std::min (1.0f, _gr / 20.f);
	cr->set_source_rgb (0.95, 0.6, 0.15);
	cr->rectangle (w - len, 0, len, h);
	cr->fill ();
	return true;
}
