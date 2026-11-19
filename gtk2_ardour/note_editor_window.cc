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
#include <list>
#include <cstdlib>

#include <ydk/gdkkeysyms.h>

#include "pbd/stateful_diff_command.h"

#include "gtkmm2ext/doi.h"

#include "ardour/audioregion.h"
#include "ardour/playlist.h"
#include "ardour/session.h"

#include "audio_region_view.h"
#include "note_editor_window.h"
#include "public_editor.h"
#include "region_selection.h"
#include "timers.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using std::vector;

static std::list<NoteEditorWindow*> windows;

void
NoteEditorWindow::open (RegionSelection const& rs)
{
	for (RegionView* rv : rs) {
		AudioRegionView* arv = dynamic_cast<AudioRegionView*> (rv);
		if (!arv) {
			continue;
		}
		std::shared_ptr<AudioRegion> r = arv->audio_region ();
		for (NoteEditorWindow* w : windows) {
			if (w->region () == r) {
				w->present ();
				return;
			}
		}
		NoteEditorWindow* w = new NoteEditorWindow (r);
		if (w->_notes.empty () && w->_an.length == 0) {
			delete w;
			return;
		}
		windows.push_back (w);
		w->present ();
		return; /* one region at a time */
	}
}

NoteEditorWindow::NoteEditorWindow (std::shared_ptr<AudioRegion> r)
	: ArdourWindow (string_compose (_("Edit Notes: %1"), r->name ()))
	, _region (r)
	, _snap_button (_("Snap to Semitone"))
	, _reset_button (_("Reset Pitch"))
	, _reset_time_button (_("Reset Timing"))
	, _zoom_in ("+")
	, _zoom_out ("−")
	, _table (2, 2)
	, _canvas (*this)
	, _hadj (0, 0, 1)
	, _vadj (0, 0, 1)
	, _hscroll (_hadj)
	, _vscroll (_vadj)
	, _pps (120)
	, _lane (14)
	, _hi (84)
	, _lo (36)
	, _drag (NoDrag)
	, _grab (-1)
	, _x0 (0), _y0 (0), _x1 (0), _y1 (0)
	, _moved (false)
	, _mouse_x (-1), _mouse_y (-1)
	, _last_playhead (-1)
{
	set_session (&r->session ());

	_snap_button.set_tooltip_text (_("Put the selected notes (all when none is selected) exactly on the nearest semitone"));
	_reset_button.set_tooltip_text (_("Back to the pitch they were sung at"));
	_snap_button.signal_clicked ().connect (sigc::mem_fun (*this, &NoteEditorWindow::snap_selected));
	_reset_button.signal_clicked ().connect (sigc::mem_fun (*this, &NoteEditorWindow::reset_selected));
	_reset_time_button.set_tooltip_text (_("Put the selected notes (all when none is selected) back where they were played, with their original length"));
	_reset_time_button.signal_clicked ().connect (sigc::mem_fun (*this, &NoteEditorWindow::reset_time));
	_zoom_in.signal_clicked ().connect ([this] () { zoom (1.5, -1); });
	_zoom_out.signal_clicked ().connect ([this] () { zoom (1 / 1.5, -1); });

	_toolbar.set_spacing (4);
	_toolbar.set_border_width (4);
	_toolbar.pack_start (_snap_button, false, false);
	_toolbar.pack_start (_reset_button, false, false);
	_toolbar.pack_start (_reset_time_button, false, false);
	_toolbar.pack_start (_status, true, true);
	_toolbar.pack_end (_zoom_in, false, false);
	_toolbar.pack_end (_zoom_out, false, false);
	_status.set_alignment (0.0, 0.5);

	_canvas.add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::SCROLL_MASK);
	_canvas.set_can_focus (true);
	_canvas.set_size_request (600, 300);
	_canvas.signal_size_allocate ().connect ([this] (Gtk::Allocation&) { update_adjustments (); });
	_hadj.signal_value_changed ().connect ([this] () { _canvas.queue_draw (); });
	_vadj.signal_value_changed ().connect ([this] () { _canvas.queue_draw (); });

	_table.attach (_canvas, 0, 1, 0, 1, Gtk::EXPAND | Gtk::FILL, Gtk::EXPAND | Gtk::FILL);
	_table.attach (_vscroll, 1, 2, 0, 1, Gtk::SHRINK, Gtk::EXPAND | Gtk::FILL);
	_table.attach (_hscroll, 0, 1, 1, 2, Gtk::EXPAND | Gtk::FILL, Gtk::SHRINK);

	_vbox.pack_start (_toolbar, false, false);
	_vbox.pack_start (_table, true, true);
	add (_vbox);
	set_default_size (1000, 520);

	if (!analyse ()) {
		return;
	}
	_vbox.show_all ();

	_timer = Timers::rapid_connect (sigc::mem_fun (*this, &NoteEditorWindow::tick));
}

NoteEditorWindow::~NoteEditorWindow ()
{
	_timer.disconnect ();
	windows.remove (this);
}

void
NoteEditorWindow::session_going_away ()
{
	ArdourWindow::session_going_away ();
	hide ();
	_region.reset ();
	windows.remove (this);
	delete_when_idle (this);
}

bool
NoteEditorWindow::analyse ()
{
	Gdk::Cursor wait (Gdk::WATCH);
	if (get_window ()) {
		get_window ()->set_cursor (wait);
	}
	const int rv = NoteEdit::analyse (_region, _an);
	if (get_window ()) {
		get_window ()->set_cursor ();
	}
	if (rv) {
		_an.length = 0;
		return false;
	}

	_playlist = _region->playlist ();
	load_region_notes ();

	if (!_notes.empty ()) {
		float lo = 127, hi = 0;
		for (auto const& n : _notes) {
			lo = std::min (lo, std::min (n.pitch, n.target ()));
			hi = std::max (hi, std::max (n.pitch, n.target ()));
		}
		_lo = std::max (0.f, std::floor (lo) - 6);
		_hi = std::min (127.f, std::ceil (hi) + 6);
		update_status (_("Drag a note up / down for pitch, sideways to move it, its edge to change the length. S splits, M merges."));
	} else {
		_status.set_text (_("No notes found: this works on a single voice (vocal, solo instrument)."));
	}
	_pps = std::max (40.0, std::min (400.0, 900.0 / std::max (0.5, _an.length / (double) _an.sr)));
	update_adjustments ();
	return true;
}

void
NoteEditorWindow::load_region_notes ()
{
	/* a region edited before keeps its notes (boundaries, detected pitch and the edits) */
	vector<NoteEdit::Note> saved;
	if (NoteEdit::load_notes (_region, saved)) {
		_notes = saved;
	} else {
		_notes = _an.notes;
	}
	_sel.assign (_notes.size (), false);
}

void
NoteEditorWindow::follow_undo ()
{
	/* undo / redo swapped our region for another version of the same recording: follow it */
	std::shared_ptr<Playlist> pl = _playlist.lock ();
	if (!pl || !_region || _region->playlist ()) {
		return;
	}
	std::shared_ptr<RegionList> rl = pl->region_list ();
	for (auto const& r : *rl) {
		std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (r);
		if (ar && !ar->master_sources ().empty () && !_region->master_sources ().empty () &&
		    ar->master_sources ().front () == _region->master_sources ().front () &&
		    ar->position () == _region->position ()) {
			_region = ar;
			load_region_notes ();
			update_status ();
			_canvas.queue_draw ();
			return;
		}
	}
}

void
NoteEditorWindow::update_adjustments ()
{
	const double w  = std::max (1, _canvas.get_width ()) - key_width ();
	const double h  = std::max (1, _canvas.get_height ()) - ruler_height ();
	const double tw = _an.length / (double) _an.sr * _pps;
	const double th = (_hi - _lo + 1) * _lane;
	_hadj.set_upper (std::max (tw, w));
	_hadj.set_page_size (w);
	_hadj.set_step_increment (20);
	_hadj.set_page_increment (w * 0.8);
	_hadj.set_value (std::min (_hadj.get_value (), std::max (0.0, tw - w)));
	_vadj.set_upper (std::max (th, h));
	_vadj.set_page_size (h);
	_vadj.set_step_increment (_lane);
	_vadj.set_page_increment (h * 0.8);
	if (_vadj.get_value () == 0 && th > h && !_notes.empty ()) {
		/* start centred on the notes */
		float mid = 0;
		for (auto const& n : _notes) {
			mid += n.target () / _notes.size ();
		}
		_vadj.set_value (std::max (0.0, std::min (th - h, (_hi - mid) * _lane - h / 2)));
	}
	_vadj.set_value (std::min (_vadj.get_value (), std::max (0.0, th - h)));
	_canvas.queue_draw ();
}

void
NoteEditorWindow::zoom (double factor, double around_x)
{
	const double w = std::max (1, _canvas.get_width ()) - key_width ();
	if (around_x < 0) {
		around_x = key_width () + w / 2;
	}
	const double t = (around_x - key_width () + _hadj.get_value ()) / _pps;
	_pps = std::max (10.0, std::min (2000.0, _pps * factor));
	update_adjustments ();
	_hadj.set_value (std::max (0.0, std::min (_hadj.get_upper () - w, t * _pps - (around_x - key_width ()))));
}

double
NoteEditorWindow::x_of (samplepos_t pos) const
{
	return key_width () + (pos - _an.origin) / (double) _an.sr * _pps - _hadj.get_value ();
}

double
NoteEditorWindow::y_of (float midi) const
{
	return ruler_height () + (_hi + 0.5 - midi) * _lane - _vadj.get_value ();
}

float
NoteEditorWindow::midi_at (double y) const
{
	return _hi + 0.5 - (y - ruler_height () + _vadj.get_value ()) / _lane;
}

int
NoteEditorWindow::note_at (double x, double y) const
{
	for (size_t i = 0; i < _notes.size (); ++i) {
		NoteEdit::Note const& n = _notes[i];
		const double yc = y_of (n.target ());
		if (x >= x_of (n.ostart) && x <= x_of (n.oend) && std::fabs (y - yc) <= _lane * 0.75) {
			return i;
		}
	}
	return -1;
}

int
NoteEditorWindow::edge_at (double x, double y, int& note) const
{
	/* the edges of a note: 5 px wide (less on tiny notes, so the middle stays grabbable) */
	for (size_t i = 0; i < _notes.size (); ++i) {
		NoteEdit::Note const& n = _notes[i];
		if (std::fabs (y - y_of (n.target ())) > _lane * 0.75) {
			continue;
		}
		const double xs = x_of (n.ostart), xe = x_of (n.oend);
		const double gw = std::min (5.0, (xe - xs) / 4);
		if (x >= xs - 2 && x <= xs + gw) {
			note = i;
			return -1;
		}
		if (x >= xe - gw && x <= xe + 2) {
			note = i;
			return 1;
		}
	}
	note = -1;
	return 0;
}

samplepos_t
NoteEditorWindow::pos_at (double x) const
{
	return _an.origin + (samplepos_t) llrint ((x - key_width () + _hadj.get_value ()) / _pps * _an.sr);
}

bool
NoteEditorWindow::any_selected () const
{
	return std::find (_sel.begin (), _sel.end (), true) != _sel.end ();
}

void
NoteEditorWindow::update_status (std::string const& hint)
{
	size_t pe = 0, te = 0;
	for (auto const& n : _notes) {
		pe += n.pitch_edited () ? 1 : 0;
		te += n.time_edited () ? 1 : 0;
	}
	std::string t = string_compose (_("%1 notes, %2 with new pitch, %3 with new timing."), _notes.size (), pe, te);
	if (!hint.empty ()) {
		t += "  " + hint;
	}
	_status.set_text (t);
}

static void
set_rgb (cairo_t* cr, uint32_t rgb, double a = 1.0)
{
	cairo_set_source_rgba (cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, a);
}

void
NoteEditorWindow::draw (cairo_t* cr, int w, int h)
{
	static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	const double kw = key_width ();
	const double rh = ruler_height ();

	set_rgb (cr, 0x1e2024);
	cairo_paint (cr);

	/* lanes */
	cairo_save (cr);
	cairo_rectangle (cr, 0, rh, w, h - rh);
	cairo_clip (cr);
	cairo_select_font_face (cr, "Barlow", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, 10);
	for (int k = (int) _lo; k <= (int) _hi; ++k) {
		const double y0 = y_of (k + 0.5);
		if (y0 > h || y0 + _lane < rh) {
			continue;
		}
		const int  pc    = k % 12;
		const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
		set_rgb (cr, black ? 0x1a1c1f : 0x24272c);
		cairo_rectangle (cr, kw, y0, w - kw, _lane);
		cairo_fill (cr);
		set_rgb (cr, pc == 0 ? 0x3a3e45 : 0x2b2e33);
		cairo_rectangle (cr, kw, y0 + _lane - 1, w - kw, 1);
		cairo_fill (cr);
		/* keyboard */
		set_rgb (cr, black ? 0x2a2c30 : 0xc8cacd);
		cairo_rectangle (cr, 0, y0, kw - 1, _lane - 1);
		cairo_fill (cr);
		if (pc == 0 || _lane >= 14) {
			set_rgb (cr, black ? 0xa0a3a8 : 0x303235);
			cairo_move_to (cr, 4, y0 + _lane - 4);
			cairo_show_text (cr, string_compose ("%1%2", names[pc], k / 12 - 1).c_str ());
		}
	}

	/* blobs */
	cairo_rectangle (cr, kw, rh, w - kw, h - rh);
	cairo_clip (cr);
	for (size_t i = 0; i < _notes.size (); ++i) {
		NoteEdit::Note const& n = _notes[i];
		const double xs = x_of (n.ostart), xe = x_of (n.oend);
		if (xe < kw || xs > w) {
			continue;
		}
		/* source frame -> x where it plays now */
		auto fx = [&] (samplepos_t f) { return std::max (xs, std::min (xe, x_of ((samplepos_t) llrint (n.to_out (_an.origin + f * _an.hop))))); };
		const double yc = y_of (n.target ());
		const samplepos_t f0 = std::max ((samplepos_t) 0, (n.start - _an.origin) / _an.hop);
		const samplepos_t f1 = std::min ((samplepos_t) _an.rms.size () - 1, (n.end - _an.origin) / _an.hop);
		/* outline: the level of the note above and below its pitch line */
		cairo_new_path (cr);
		cairo_move_to (cr, xs, yc);
		for (samplepos_t f = f0; f <= f1; ++f) {
			const double x = fx (f);
			cairo_line_to (cr, x, yc - std::max (2.0, _an.rms[f] * _lane * 0.8));
		}
		cairo_line_to (cr, xe, yc);
		for (samplepos_t f = f1; f >= f0; --f) {
			const double x = fx (f);
			cairo_line_to (cr, x, yc + std::max (2.0, _an.rms[f] * _lane * 0.8));
			if (f == 0) {
				break;
			}
		}
		cairo_close_path (cr);
		const bool     sel  = _sel[i];
		const uint32_t fill = sel ? 0xf0a040 : (n.edited () ? 0x5fb0e8 : 0x4a8fc8);
		set_rgb (cr, fill, 0.85);
		cairo_fill_preserve (cr);
		set_rgb (cr, sel ? 0xffd090 : 0x9fd0f5, 0.9);
		cairo_set_line_width (cr, 1);
		cairo_stroke (cr);

		/* the sung pitch inside the note, moved with the edit */
		cairo_new_path (cr);
		bool pen = false;
		for (samplepos_t f = f0; f <= f1; ++f) {
			if (_an.midi[f] <= 0) {
				pen = false;
				continue;
			}
			const double x = fx (f);
			const double y = y_of (_an.midi[f] + n.shift);
			if (pen) {
				cairo_line_to (cr, x, y);
			} else {
				cairo_move_to (cr, x, y);
				pen = true;
			}
		}
		set_rgb (cr, 0xffffff, 0.75);
		cairo_set_line_width (cr, 1.2);
		cairo_stroke (cr);

		/* where it was played: a faint bracket under the note */
		if (n.time_edited ()) {
			const double xo0 = x_of (n.start), xo1 = x_of (n.end), yb = yc + _lane * 0.7;
			set_rgb (cr, 0xffffff, 0.3);
			cairo_set_line_width (cr, 1);
			cairo_move_to (cr, xo0 + 0.5, yb - 3);
			cairo_line_to (cr, xo0 + 0.5, yb + 0.5);
			cairo_line_to (cr, xo1 - 0.5, yb + 0.5);
			cairo_line_to (cr, xo1 - 0.5, yb - 3);
			cairo_stroke (cr);
		}

		/* where it was sung */
		if (n.pitch_edited ()) {
			const double yo = y_of (n.pitch);
			set_rgb (cr, 0xffffff, 0.25);
			cairo_set_dash (cr, (double[]) { 3, 3 }, 2, 0);
			cairo_move_to (cr, xs, yo);
			cairo_line_to (cr, xe, yo);
			cairo_stroke (cr);
			cairo_set_dash (cr, 0, 0, 0);
		}
	}

	/* rubber band */
	if (_drag == RectDrag) {
		set_rgb (cr, 0x9fd0f5, 0.15);
		cairo_rectangle (cr, std::min (_x0, _x1), std::min (_y0, _y1), std::fabs (_x1 - _x0), std::fabs (_y1 - _y0));
		cairo_fill_preserve (cr);
		set_rgb (cr, 0x9fd0f5, 0.7);
		cairo_stroke (cr);
	}
	cairo_restore (cr);

	/* ruler: seconds from the region start */
	set_rgb (cr, 0x2b2e33);
	cairo_rectangle (cr, 0, 0, w, rh);
	cairo_fill (cr);
	double step = 1;
	while (step * _pps < 60) {
		step *= 2;
	}
	while (step * _pps > 160 && step > 0.125) {
		step /= 2;
	}
	cairo_select_font_face (cr, "Barlow", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, 10);
	for (double t = 0; t <= _an.length / (double) _an.sr; t += step) {
		const double x = x_of (_an.origin + t * _an.sr);
		if (x < kw || x > w) {
			continue;
		}
		set_rgb (cr, 0x8a8d92);
		cairo_rectangle (cr, x, rh - 6, 1, 6);
		cairo_fill (cr);
		cairo_move_to (cr, x + 3, rh - 7);
		char buf[32];
		snprintf (buf, sizeof (buf), step < 1 ? "%.2f" : "%.0f", t);
		cairo_show_text (cr, buf);
	}

	/* playhead */
	if (_session && _region) {
		const samplepos_t ph = _session->audible_sample ();
		const samplepos_t rs = _region->position_sample ();
		if (ph >= rs && ph < rs + _region->length_samples ()) {
			const double x = x_of (_an.origin + (ph - rs));
			if (x >= kw && x <= w) {
				set_rgb (cr, 0xff4040);
				cairo_rectangle (cr, x, 0, 1, h);
				cairo_fill (cr);
			}
		}
	}
}

bool
NoteEditorWindow::Canvas::on_expose_event (GdkEventExpose* ev)
{
	cairo_t* cr = gdk_cairo_create (get_window ()->gobj ());
	cairo_rectangle (cr, ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	cairo_clip (cr);
	_w.draw (cr, get_width (), get_height ());
	cairo_destroy (cr);
	return true;
}

bool
NoteEditorWindow::Canvas::on_button_press_event (GdkEventButton* ev)
{
	grab_focus ();
	if (ev->x < _w.key_width () || ev->y < _w.ruler_height ()) {
		return false;
	}
	int        ei     = -1;
	const int  edge   = _w.edge_at (ev->x, ev->y, ei);
	const int  i      = edge ? ei : _w.note_at (ev->x, ev->y);
	if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS) {
		if (i >= 0 && !_w._sel[i]) {
			std::fill (_w._sel.begin (), _w._sel.end (), false);
			_w._sel[i] = true;
			queue_draw ();
		}
		_w._mouse_x = ev->x;
		_w._mouse_y = ev->y;
		_w.popup_menu (i, ev);
		return true;
	}
	if (ev->button != 1) {
		return false;
	}
	const bool toggle = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK);

	if (ev->type == GDK_2BUTTON_PRESS) {
		_w._drag = NoDrag;
		if (i >= 0) {
			if (!_w._sel[i]) {
				std::fill (_w._sel.begin (), _w._sel.end (), false);
				_w._sel[i] = true;
			}
			_w.snap_selected ();
		}
		return true;
	}
	if (ev->type != GDK_BUTTON_PRESS) {
		return true;
	}

	_w._x0 = _w._x1 = ev->x;
	_w._y0 = _w._y1 = ev->y;
	_w._moved = false;

	if (i >= 0) {
		if (toggle) {
			_w._sel[i] = !_w._sel[i];
		} else if (!_w._sel[i]) {
			std::fill (_w._sel.begin (), _w._sel.end (), false);
			_w._sel[i] = true;
		}
		if (edge && !toggle) {
			/* resize this one note */
			std::fill (_w._sel.begin (), _w._sel.end (), false);
			_w._sel[i] = true;
		}
		if (_w._sel[i]) {
			_w._drag = edge < 0 ? StartDrag : (edge > 0 ? EndDrag : Undecided);
			_w._grab = i;
			_w._base.resize (_w._notes.size ());
			for (size_t k = 0; k < _w._notes.size (); ++k) {
				_w._base[k] = _w._notes[k].shift;
			}
			_w._base_notes = _w._notes;
		}
	} else {
		if (!toggle) {
			std::fill (_w._sel.begin (), _w._sel.end (), false);
		}
		_w._drag = RectDrag;
	}
	queue_draw ();
	return true;
}

bool
NoteEditorWindow::Canvas::on_motion_notify_event (GdkEventMotion* ev)
{
	_w._mouse_x = ev->x;
	_w._mouse_y = ev->y;

	if (_w._drag == NoDrag && get_window ()) {
		/* show what a drag would do here */
		int ei;
		if (_w.edge_at (ev->x, ev->y, ei)) {
			get_window ()->set_cursor (Gdk::Cursor (Gdk::SB_H_DOUBLE_ARROW));
		} else if (_w.note_at (ev->x, ev->y) >= 0) {
			get_window ()->set_cursor (Gdk::Cursor (Gdk::FLEUR));
		} else {
			get_window ()->set_cursor ();
		}
		return false;
	}

	if (_w._drag == Undecided) {
		/* the first few pixels decide: up / down = pitch, sideways = time */
		const double dx = ev->x - _w._x0, dy = ev->y - _w._y0;
		if (std::fabs (dx) < 4 && std::fabs (dy) < 4) {
			return true;
		}
		_w._drag = std::fabs (dx) > std::fabs (dy) ? TimeDrag : PitchDrag;
	}

	if (_w._drag == TimeDrag || _w._drag == StartDrag || _w._drag == EndDrag) {
		const ARDOUR::samplecnt_t d = (ARDOUR::samplecnt_t) llrint ((ev->x - _w._x0) / _w._pps * _w._an.sr);
		_w._notes = _w._base_notes;
		_w._moved = _w.time_edit (_w._drag, d);
		queue_draw ();
		return true;
	}

	if (_w._drag == PitchDrag) {
		NoteEdit::Note const& g  = _w._notes[_w._grab];
		const float           dy = (_w._y0 - ev->y) / _w._lane;
		float                 delta;
		if (ev->state & GDK_MOD1_MASK) {
			delta = dy; /* free */
		} else {
			/* the grabbed note lands on whole semitones, the others move with it */
			const float base_target = g.pitch + _w._base[_w._grab];
			delta = std::round (base_target + dy) - base_target;
		}
		for (size_t k = 0; k < _w._notes.size (); ++k) {
			if (_w._sel[k]) {
				const float s = _w._base[k] + delta;
				if (s != _w._notes[k].shift) {
					_w._notes[k].shift = s;
					_w._moved = true;
				}
			}
		}
		queue_draw ();
		return true;
	}
	if (_w._drag == RectDrag) {
		_w._x1 = ev->x;
		_w._y1 = ev->y;
		const double xa = std::min (_w._x0, _w._x1), xb = std::max (_w._x0, _w._x1);
		const double ya = std::min (_w._y0, _w._y1), yb = std::max (_w._y0, _w._y1);
		for (size_t k = 0; k < _w._notes.size (); ++k) {
			NoteEdit::Note const& n  = _w._notes[k];
			const double          yc = _w.y_of (n.target ());
			if (_w.x_of (n.oend) >= xa && _w.x_of (n.ostart) <= xb && yc + _w._lane / 2 >= ya && yc - _w._lane / 2 <= yb) {
				_w._sel[k] = true;
			} else if (!(ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
				_w._sel[k] = false;
			}
		}
		queue_draw ();
		return true;
	}
	return false;
}

bool
NoteEditorWindow::Canvas::on_button_release_event (GdkEventButton* ev)
{
	if (ev->button != 1) {
		return false;
	}
	const Drag d = _w._drag;
	_w._drag     = NoDrag;
	if (d == PitchDrag && _w._moved) {
		_w.apply (_("move note pitch"));
	} else if (d == TimeDrag && _w._moved) {
		_w.apply (_("move note in time"));
	} else if ((d == StartDrag || d == EndDrag) && _w._moved) {
		_w.apply (_("change note length"));
	}
	queue_draw ();
	return true;
}

bool
NoteEditorWindow::Canvas::on_scroll_event (GdkEventScroll* ev)
{
	const bool up = ev->direction == GDK_SCROLL_UP;
	if (ev->direction != GDK_SCROLL_UP && ev->direction != GDK_SCROLL_DOWN) {
		return false;
	}
	if (ev->state & GDK_CONTROL_MASK) {
		_w.zoom (up ? 1.25 : 0.8, ev->x);
	} else if (ev->state & GDK_SHIFT_MASK) {
		Gtk::Adjustment& a = _w._hadj;
		a.set_value (std::max (a.get_lower (), std::min (a.get_upper () - a.get_page_size (), a.get_value () + (up ? -60 : 60))));
	} else {
		Gtk::Adjustment& a = _w._vadj;
		a.set_value (std::max (a.get_lower (), std::min (a.get_upper () - a.get_page_size (), a.get_value () + (up ? -_w._lane * 2 : _w._lane * 2))));
	}
	return true;
}

void
NoteEditorWindow::snap_selected ()
{
	const bool any = std::find (_sel.begin (), _sel.end (), true) != _sel.end ();
	bool changed   = false;
	for (size_t k = 0; k < _notes.size (); ++k) {
		if (any && !_sel[k]) {
			continue;
		}
		const float s = std::round (_notes[k].target ()) - _notes[k].pitch;
		if (std::fabs (s - _notes[k].shift) > 1e-4f) {
			_notes[k].shift = s;
			changed         = true;
		}
	}
	if (changed) {
		apply (_("snap notes to semitones"));
	}
}

void
NoteEditorWindow::reset_selected ()
{
	const bool any = std::find (_sel.begin (), _sel.end (), true) != _sel.end ();
	bool changed   = false;
	for (size_t k = 0; k < _notes.size (); ++k) {
		if ((!any || _sel[k]) && _notes[k].shift != 0) {
			_notes[k].shift = 0;
			changed         = true;
		}
	}
	if (changed) {
		apply (_("reset note pitch"));
	}
}

void
NoteEditorWindow::shift_selected (float semitones)
{
	bool changed = false;
	for (size_t k = 0; k < _notes.size (); ++k) {
		if (_sel[k]) {
			_notes[k].shift += semitones;
			changed = true;
		}
	}
	if (changed) {
		apply (_("move note pitch"));
	}
}

bool
NoteEditorWindow::on_key_press_event (GdkEventKey* ev)
{
	const bool alt = ev->state & GDK_MOD1_MASK;
	switch (ev->keyval) {
		case GDK_Up:
			shift_selected (alt ? 0.1f : 1.f);
			return true;
		case GDK_Down:
			shift_selected (alt ? -0.1f : -1.f);
			return true;
		case GDK_Left:
		case GDK_Right:
			/* nudge the selection by 10 ms (Alt: 1 ms) */
			if (any_selected ()) {
				const samplecnt_t d = (alt ? _an.sr / 1000 : _an.sr / 100) * (ev->keyval == GDK_Left ? -1 : 1);
				_base_notes         = _notes;
				if (time_edit (TimeDrag, d)) {
					apply (_("move note in time"));
				}
			}
			return true;
		case GDK_s:
		case GDK_S:
			if (!(ev->state & GDK_CONTROL_MASK)) {
				const int i = note_at (_mouse_x, _mouse_y);
				if (i >= 0) {
					split_at (i, pos_at (_mouse_x));
				}
				return true;
			}
			break;
		case GDK_z:
		case GDK_Z:
			/* the edits are undone in the editor; this window follows */
			if (ev->state & GDK_CONTROL_MASK) {
				if (ev->state & GDK_SHIFT_MASK) {
					PublicEditor::instance ().redo (1);
				} else {
					PublicEditor::instance ().undo (1);
				}
				follow_undo ();
				return true;
			}
			break;
		case GDK_r:
		case GDK_R:
			if (ev->state & GDK_CONTROL_MASK) {
				PublicEditor::instance ().redo (1);
				follow_undo ();
				return true;
			}
			break;
		case GDK_m:
		case GDK_M:
			if (!(ev->state & GDK_CONTROL_MASK)) {
				merge_selected ();
				return true;
			}
			break;
		case GDK_Escape:
			std::fill (_sel.begin (), _sel.end (), false);
			_canvas.queue_draw ();
			return true;
		case GDK_a:
			if (ev->state & GDK_CONTROL_MASK) {
				std::fill (_sel.begin (), _sel.end (), true);
				_canvas.queue_draw ();
				return true;
			}
			break;
		default:
			break;
	}
	return ArdourWindow::on_key_press_event (ev);
}

/** the output timing is usable: every note long enough, in the analysed range, in order and not overlapping */
static bool
timing_ok (vector<NoteEdit::Note> const& notes, samplepos_t lo, samplepos_t hi, samplecnt_t min_len)
{
	for (size_t k = 0; k < notes.size (); ++k) {
		NoteEdit::Note const& n = notes[k];
		if (n.oend - n.ostart < min_len || n.ostart < lo || n.oend > hi) {
			return false;
		}
		if (k > 0 && n.ostart < notes[k - 1].oend) {
			return false;
		}
	}
	return true;
}

bool
NoteEditorWindow::time_edit (Drag what, samplecnt_t delta)
{
	/* starts from _base_notes, the timing when the drag began. Notes played legato with an edited note
	 * (no gap between them) follow it: the neighbour stretches or shrinks instead of being overlapped. */
	const vector<NoteEdit::Note> base = _base_notes;
	const samplecnt_t legato  = _an.sr * 3 / 20; /* neighbours closer than 150 ms follow (a gap of silence absorbs the move) */
	const samplecnt_t min_len = _an.sr / 40; /* a note keeps at least 25 ms */
	const samplepos_t lo = _an.origin, hi = _an.origin + _an.length;

	auto attempt = [&] (samplecnt_t d) -> bool {
		_notes = base;
		const size_t n = _notes.size ();
		vector<bool> moves_start (n, false), moves_end (n, false);
		for (size_t k = 0; k < n; ++k) {
			if (what == TimeDrag) {
				moves_start[k] = moves_end[k] = _sel[k];
			} else if ((int) k == _grab) {
				moves_start[k] = what == StartDrag;
				moves_end[k]   = what == EndDrag;
			}
		}
		for (size_t k = 0; k < n; ++k) {
			if (moves_start[k]) {
				_notes[k].ostart += d;
				/* the legato neighbour before ends where this one starts now */
				if (k > 0 && !moves_end[k - 1] && base[k].ostart - base[k - 1].oend <= legato) {
					_notes[k - 1].oend = _notes[k].ostart - (base[k].ostart - base[k - 1].oend);
				}
			}
			if (moves_end[k]) {
				_notes[k].oend += d;
				if (k + 1 < n && !moves_start[k + 1] && base[k + 1].ostart - base[k].oend <= legato) {
					_notes[k + 1].ostart = _notes[k].oend + (base[k + 1].ostart - base[k].oend);
				}
			}
		}
		return timing_ok (_notes, lo, hi, min_len);
	};

	if (delta == 0) {
		_notes = base;
		return false;
	}
	if (attempt (delta)) {
		return true;
	}
	/* blocked (a neighbour, the region edge, a note getting too short): go as far as possible */
	samplecnt_t good = 0, bad = delta;
	while (std::llabs (bad - good) > 1) {
		const samplecnt_t mid = good + (bad - good) / 2;
		if (attempt (mid)) {
			good = mid;
		} else {
			bad = mid;
		}
	}
	if (good == 0) {
		_notes = base;
		return false;
	}
	attempt (good);
	return true;
}

void
NoteEditorWindow::split_at (int i, samplepos_t out_pos)
{
	if (i < 0 || i >= (int) _notes.size ()) {
		return;
	}
	NoteEdit::Note const  n       = _notes[i];
	const samplecnt_t     min_len = _an.sr / 40;
	if (out_pos - n.ostart < min_len || n.oend - out_pos < min_len) {
		update_status (_("Too close to the edge of the note to split there."));
		return;
	}
	const samplepos_t src = (samplepos_t) llrint (n.to_src (out_pos));
	NoteEdit::Note a = n, b = n;
	a.end    = src;
	a.oend   = out_pos;
	b.start  = src;
	b.ostart = out_pos;
	/* each part gets its own pitch centre; the edit (shift) stays the same, so nothing sounds different yet */
	a.pitch = NoteEdit::median_pitch (_an, a.start, a.end, n.pitch);
	b.pitch = NoteEdit::median_pitch (_an, b.start, b.end, n.pitch);
	_notes[i] = a;
	_notes.insert (_notes.begin () + i + 1, b);
	_sel.assign (_notes.size (), false);
	_sel[i + 1] = true;
	apply (_("split note"));
}

void
NoteEditorWindow::merge_selected ()
{
	size_t first = _notes.size (), last = 0;
	for (size_t k = 0; k < _notes.size (); ++k) {
		if (_sel[k]) {
			first = std::min (first, k);
			last  = std::max (last, k);
		}
	}
	if (first >= last) {
		update_status (_("Select two or more neighbouring notes to merge them."));
		return;
	}
	/* one note from the first start to the last end; it keeps the edit of its longest part */
	size_t longest = first;
	for (size_t k = first; k <= last; ++k) {
		if (_notes[k].end - _notes[k].start > _notes[longest].end - _notes[longest].start) {
			longest = k;
		}
	}
	NoteEdit::Note m = _notes[first];
	m.end   = _notes[last].end;
	m.oend  = _notes[last].oend;
	m.pitch = NoteEdit::median_pitch (_an, m.start, m.end, _notes[longest].pitch);
	m.shift = _notes[longest].shift;
	_notes.erase (_notes.begin () + first + 1, _notes.begin () + last + 1);
	_notes[first] = m;
	_sel.assign (_notes.size (), false);
	_sel[first] = true;
	apply (_("merge notes"));
}

void
NoteEditorWindow::reset_time ()
{
	const bool   any = any_selected ();
	vector<bool> reset (_notes.size (), false);
	for (size_t k = 0; k < _notes.size (); ++k) {
		reset[k] = !any || _sel[k];
	}
	/* a neighbour that was stretched along would now overlap: it goes back as well */
	bool again = true;
	while (again) {
		again                       = false;
		vector<NoteEdit::Note> test = _notes;
		for (size_t k = 0; k < test.size (); ++k) {
			if (reset[k]) {
				test[k].ostart = test[k].start;
				test[k].oend   = test[k].end;
			}
		}
		for (size_t k = 1; k < test.size (); ++k) {
			if (test[k].ostart < test[k - 1].oend) {
				again = again || !reset[k] || !reset[k - 1];
				reset[k] = reset[k - 1] = true;
			}
		}
	}
	bool changed = false;
	for (size_t k = 0; k < _notes.size (); ++k) {
		if (reset[k] && _notes[k].time_edited ()) {
			_notes[k].ostart = _notes[k].start;
			_notes[k].oend   = _notes[k].end;
			changed          = true;
		}
	}
	if (changed) {
		apply (_("reset note timing"));
	}
}

void
NoteEditorWindow::popup_menu (int i, GdkEventButton* ev)
{
	using namespace Gtk::Menu_Helpers;
	MenuList& items = _menu.items ();
	items.clear ();
	const samplepos_t at = pos_at (ev->x);
	if (i >= 0) {
		items.push_back (MenuElem (_("Split Here (S)"), [this, i, at] () { split_at (i, at); }));
	}
	items.push_back (MenuElem (_("Merge Selected (M)"), sigc::mem_fun (*this, &NoteEditorWindow::merge_selected)));
	items.push_back (SeparatorElem ());
	items.push_back (MenuElem (_("Snap to Semitone"), sigc::mem_fun (*this, &NoteEditorWindow::snap_selected)));
	items.push_back (MenuElem (_("Reset Pitch"), sigc::mem_fun (*this, &NoteEditorWindow::reset_selected)));
	items.push_back (MenuElem (_("Reset Timing"), sigc::mem_fun (*this, &NoteEditorWindow::reset_time)));
	_menu.popup (ev->button, ev->time);
}

void
NoteEditorWindow::apply (std::string const& what)
{
	if (!_session || !_region) {
		return;
	}
	std::shared_ptr<Playlist> pl = _region->playlist ();
	if (!pl) {
		follow_undo ();
		pl = _region->playlist ();
	}
	if (!pl) {
		/* the region left its playlist (deleted): nothing to edit any more */
		_status.set_text (_("This region is no longer in the project; open Edit Notes on it again."));
		return;
	}

	Gdk::Cursor wait (Gdk::WATCH);
	get_window ()->set_cursor (wait);
	NoteEdit::Filter f (*_session, _notes);
	const int rv = _region->apply (f, 0);
	get_window ()->set_cursor ();
	if (rv != 0 || f.results.empty ()) {
		_status.set_text (_("The edit could not be rendered (see the log)."));
		return;
	}

	std::shared_ptr<Region> nr = f.results.front ();
	PublicEditor& ed = PublicEditor::instance ();
	ed.begin_reversible_command (what);
	pl->clear_changes ();
	pl->clear_owned_changes ();
	pl->freeze ();
	pl->replace_region (_region, nr, _region->position ());
	pl->thaw ();
	vector<PBD::Command*> cmds;
	pl->rdiff (cmds);
	_session->add_commands (cmds);
	_session->add_command (new PBD::StatefulDiffCommand (pl));
	ed.commit_reversible_command ();

	/* a whole-file region is copied into the playlist: follow the copy (same new sources) */
	std::shared_ptr<RegionList> rl = pl->region_list ();
	for (auto const& r : *rl) {
		if (r->source (0) == nr->source (0)) {
			if (r != nr) {
				/* the copy does not carry the notes: they belong to the region that plays */
				r->add_extra_xml (*NoteEdit::notes_to_xml (_notes));
			}
			nr = r;
			break;
		}
	}
	_region = std::dynamic_pointer_cast<AudioRegion> (nr);
	update_status ();
	_canvas.queue_draw ();
}

void
NoteEditorWindow::tick ()
{
	if (!_session || !is_visible ()) {
		return;
	}
	follow_undo ();
	const samplepos_t ph = _session->audible_sample ();
	if (ph != _last_playhead) {
		_last_playhead = ph;
		_canvas.queue_draw ();
	}
}
