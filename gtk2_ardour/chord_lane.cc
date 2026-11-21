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

#include <glibmm/main.h>
#include <ydkmm/cursor.h>
#include <pango/pangocairo.h>

#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/session.h"

#include "gtkmm2ext/keyboard.h"

#include "temporal/tempo.h"

#include "chord_lane.h"
#include "chord_pads.h"
#include "gui_thread.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace Temporal;

namespace {

void
rgb (cairo_t* cr, uint32_t c, double a = 1.0)
{
	cairo_set_source_rgba (cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, a);
}

void
rounded (cairo_t* cr, double x, double y, double w, double h, double r)
{
	r = std::min (r, std::min (w, h) / 2);
	cairo_new_sub_path (cr);
	cairo_arc (cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc (cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc (cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc (cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path (cr);
}

/* a layout of pango markup, clipped to a width (ellipsized) */
void
show_text (cairo_t* cr, Gtk::Widget& w, std::string const& m, char const* font, double x, double cy, double width, uint32_t c, bool centre)
{
	Glib::RefPtr<Pango::Layout> l = w.create_pango_layout ("");
	l->set_markup (m);
	Pango::FontDescription fd (font);
	l->set_font_description (fd);
	l->set_ellipsize (Pango::ELLIPSIZE_END);
	l->set_width ((int) std::max (1.0, width) * PANGO_SCALE);
	int tw, th;
	l->get_pixel_size (tw, th);
	rgb (cr, c);
	cairo_move_to (cr, centre ? x + std::max (0.0, (width - tw) / 2) : x, cy - th / 2.0);
	pango_cairo_show_layout (cr, l->gobj ());
}

const int64_t chord_slop = Beats::PPQN / 16; /* notes starting this close together are one chord */

} // namespace

std::vector<ChordLane::ClipNote> ChordLane::_clipboard;
int64_t                          ChordLane::_clip_span = 0;

ChordLane::ChordLane (ChordPads& p)
	: _pads (p)
	, _session (0)
	, _history (0)
	, _channel (0)
	, _origin (0)
	, _r_start (0)
	, _r_end (Beats::PPQN * 16)
	, _bar_ticks (Beats::PPQN * 4)
	, _first_bar (1)
	, _ppb (48)
	, _playhead (-1)
	, _sel (-1)
	, _hover (-1)
	, _drag (0)
	, _drag_block (-1)
	, _drag_x0 (0)
	, _drag_delta (0)
	, _dragged (false)
	, _drop_tick (-1)
{
	add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::LEAVE_NOTIFY_MASK
	            | Gdk::SCROLL_MASK | Gdk::KEY_PRESS_MASK | Gdk::FOCUS_CHANGE_MASK);
	set_can_focus (true);
	set_size_request (600, 180);
	std::vector<Gtk::TargetEntry> targets;
	targets.push_back (Gtk::TargetEntry (ChordPads::drag_target (), Gtk::TARGET_SAME_APP));
	drag_dest_set (targets, Gtk::DEST_DEFAULT_DROP | Gtk::DEST_DEFAULT_HIGHLIGHT, Gdk::ACTION_COPY);
	_timer = Glib::signal_timeout ().connect (sigc::mem_fun (*this, &ChordLane::tick_timer), 80);
}

ChordLane::~ChordLane ()
{
	_timer.disconnect ();
}

void
ChordLane::set_region (std::shared_ptr<MidiRegion> r)
{
	_connections.drop_connections ();
	_region = r;
	_sel    = -1;
	_selection.clear ();
	if (_region) {
		std::shared_ptr<MidiModel> m (_region->model ());
		if (m) {
			m->ContentsChanged.connect (_connections, invalidator (*this), std::bind (&ChordLane::rebuild, this), gui_context ());
		}
		_region->PropertyChanged.connect (_connections, invalidator (*this), std::bind (&ChordLane::rebuild, this), gui_context ());
	}
	rebuild ();
}

Beats
ChordLane::source_beats (int64_t tick) const
{
	return _region->absolute_time_to_source_beats (timepos_t (Beats::ticks (_origin + tick)));
}

int64_t
ChordLane::note_tick (NotePtr const& n) const
{
	return _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - _origin;
}

void
ChordLane::rebuild ()
{
	_blocks.clear ();
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model) {
		queue_resize_width ();
		return;
	}

	TempoMap::SharedPtr        tm (TempoMap::use ());
	timepos_t const            pos = _region->position ();
	Temporal::MeterPoint const& meter (tm->meter_at (pos));
	const int64_t              div = (Beats::PPQN * 4) / meter.note_value ();
	_bar_ticks                     = meter.divisions_per_bar () * div;
	BBT_Argument const bbt (tm->bbt_at (pos));
	_first_bar = bbt.bars;
	_origin    = pos.beats ().to_ticks () - ((bbt.beats - 1) * div + bbt.ticks * 4 / meter.note_value ());
	_r_start   = pos.beats ().to_ticks () - _origin;
	_r_end     = _region->end_position ().beats ().to_ticks () - _origin;

	/* the notes in the region, in time order, grouped by their start */
	std::vector<NotePtr> notes;
	{
		MidiModel::ReadLock lm (model->read_lock ());
		for (auto const& n : model->notes ()) {
			if (_channel >= 0 && n->channel () != _channel) {
				continue;
			}
			const int64_t t = note_tick (n);
			if (t >= _r_start && t < _r_end) {
				notes.push_back (n);
			}
		}
	}
	std::sort (notes.begin (), notes.end (), [this] (NotePtr const& a, NotePtr const& b) { return note_tick (a) < note_tick (b); });
	for (auto const& n : notes) {
		const int64_t t = note_tick (n);
		const int64_t e = t + n->length ().to_ticks ();
		if (_blocks.empty () || t - _blocks.back ().start > chord_slop) {
			Block b;
			b.start = t;
			b.end   = e;
			_blocks.push_back (b);
		}
		Block& b = _blocks.back ();
		b.end    = std::max (b.end, e);
		b.notes.push_back (n);
		b.pitches.push_back (n->note ());
	}
	for (auto& b : _blocks) {
		std::sort (b.pitches.begin (), b.pitches.end ());
	}
	if (_sel >= (int) _blocks.size ()) {
		_sel = -1;
	}
	/* chords that are gone are no longer selected */
	std::set<int64_t> keep;
	for (auto const& b : _blocks) {
		if (_selection.count (b.start)) {
			keep.insert (b.start);
		}
	}
	_selection.swap (keep);
	queue_resize_width ();
}

void
ChordLane::queue_resize_width ()
{
	const double w = tick_x (_r_end) + 40;
	set_size_request ((int) std::max (300.0, w), 180);
	queue_draw ();
}

int64_t
ChordLane::snap (int64_t t, bool fine) const
{
	const int64_t g = fine ? Beats::PPQN / 4 : Beats::PPQN; /* a beat, a sixteenth with Shift */
	return ((t + (t >= 0 ? g / 2 : -g / 2)) / g) * g;
}

int
ChordLane::block_at (double x, double y, int& edge) const
{
	edge = 0;
	if (y < ruler_h) {
		return -1;
	}
	for (int i = (int) _blocks.size () - 1; i >= 0; --i) {
		const double x0 = tick_x (_blocks[i].start);
		const double x1 = tick_x (_blocks[i].end);
		if (x >= x0 && x < x1) {
			const double grip = std::min (7.0, (x1 - x0) / 4);
			edge              = x < x0 + grip ? -1 : (x >= x1 - grip ? 1 : 0);
			return i;
		}
	}
	return -1;
}

bool
ChordLane::tick_timer ()
{
	int64_t ph = -1;
	if (_session && _region) {
		const int64_t t = timepos_t (_session->audible_sample ()).beats ().to_ticks () - _origin;
		if (t >= _r_start && t <= _r_end) {
			ph = t;
		}
	}
	if (ph != _playhead) {
		_playhead = ph;
		queue_draw ();
	}
	return true;
}

bool
ChordLane::on_expose_event (GdkEventExpose* ev)
{
	cairo_t* cr = gdk_cairo_create (get_window ()->gobj ());
	cairo_rectangle (cr, ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	cairo_clip (cr);
	const double H = get_height ();

	rgb (cr, 0x1e2023);
	cairo_paint (cr);
	/* the region's span */
	rgb (cr, 0x26292d);
	cairo_rectangle (cr, tick_x (_r_start), ruler_h, tick_x (_r_end) - tick_x (_r_start), H - ruler_h);
	cairo_fill (cr);

	/* ruler: bars and beats */
	rgb (cr, 0x2c2f34);
	cairo_rectangle (cr, 0, 0, get_width (), ruler_h);
	cairo_fill (cr);
	const int64_t beat = Beats::PPQN;
	for (int64_t t = (_r_start / beat) * beat; t <= _r_end; t += beat) {
		const double x   = std::floor (tick_x (t)) + 0.5;
		const bool   bar = t % _bar_ticks == 0;
		rgb (cr, bar ? 0x4c5158 : 0x33363b);
		cairo_move_to (cr, x, bar ? 4 : ruler_h - 6);
		cairo_line_to (cr, x, H);
		cairo_set_line_width (cr, 1);
		cairo_stroke (cr);
		if (bar) {
			show_text (cr, *this, std::to_string (_first_bar + (int) (t / _bar_ticks)), "Sans 8", x + 3, ruler_h / 2.0, 40, 0x9aa3ad, false);
		}
	}

	/* the chords */
	const double top = ruler_h + 8, bh = H - ruler_h - 16;
	for (size_t i = 0; i < _blocks.size (); ++i) {
		Block const& b  = _blocks[i];
		int64_t      s  = b.start, e = b.end;
		if ((int) i == _drag_block && _dragged) {
			if (_drag == 1) {
				s += _drag_delta;
				e += _drag_delta;
			} else if (_drag == 2) {
				s = std::min (e - Beats::PPQN / 4, s + _drag_delta);
			} else if (_drag == 3) {
				e = std::max (s + Beats::PPQN / 4, e + _drag_delta);
			}
		}
		const double x0 = tick_x (s) + 1, x1 = tick_x (e) - 1;
		const bool   sel = is_sel ((int) i);
		cairo_pattern_t* g = cairo_pattern_create_linear (0, top, 0, top + bh);
		const uint32_t   c0 = sel ? 0x6b7fa0 : ((int) i == _hover ? 0x585c63 : 0x4d5056);
		const uint32_t   c1 = sel ? 0x55688a : ((int) i == _hover ? 0x4a4e54 : 0x404348);
		cairo_pattern_add_color_stop_rgb (g, 0, ((c0 >> 16) & 0xff) / 255.0, ((c0 >> 8) & 0xff) / 255.0, (c0 & 0xff) / 255.0);
		cairo_pattern_add_color_stop_rgb (g, 1, ((c1 >> 16) & 0xff) / 255.0, ((c1 >> 8) & 0xff) / 255.0, (c1 & 0xff) / 255.0);
		rounded (cr, x0, top, std::max (4.0, x1 - x0), bh, 5);
		cairo_set_source (cr, g);
		cairo_fill_preserve (cr);
		cairo_pattern_destroy (g);
		rgb (cr, sel ? 0xc9d6ea : 0x6a6f77);
		cairo_set_line_width (cr, sel ? 1.5 : 1);
		cairo_stroke (cr);
		/* grips at the ends */
		rgb (cr, 0xffffff, 0.18);
		cairo_rectangle (cr, x0 + 2, top + bh / 2 - 8, 2, 16);
		cairo_rectangle (cr, x1 - 4, top + bh / 2 - 8, 2, 16);
		cairo_fill (cr);
		show_text (cr, *this, ChordPads::notes_markup (b.pitches), "Sans Bold 12", x0 + 6, top + bh / 2 - 6, x1 - x0 - 12, 0xffffff, true);
		/* the notes, small */
		std::string nn;
		for (int p : b.pitches) {
			nn += (nn.empty () ? "" : " ") + std::string (ChordPads::pc_name (p % 12));
		}
		show_text (cr, *this, Glib::Markup::escape_text (nn), "Sans 8", x0 + 6, top + bh - 12, x1 - x0 - 12, 0xb4bbc4, true);
	}

	/* where a dragged pad would land */
	if (_drop_tick >= 0) {
		const double x = tick_x (_drop_tick);
		rgb (cr, 0x4a90e2, 0.35);
		cairo_rectangle (cr, x, top, tick_x (_drop_tick + _pads.length ().to_ticks ()) - x, bh);
		cairo_fill (cr);
		rgb (cr, 0x4a90e2);
		cairo_rectangle (cr, x - 1, ruler_h, 2, H - ruler_h);
		cairo_fill (cr);
	}

	if (_blocks.empty ()) {
		show_text (cr, *this, std::string ("<i>") + _("Drag pads or chords of the circle here") + "</i>", "Sans 10", tick_x (_r_start) + 10,
		      ruler_h + (H - ruler_h) / 2, 400, 0x7c838c, false);
	}

	/* the playhead */
	if (_playhead >= 0) {
		const double x = std::floor (tick_x (_playhead)) + 0.5;
		rgb (cr, 0xe04040);
		cairo_move_to (cr, x, 0);
		cairo_line_to (cr, x, H);
		cairo_set_line_width (cr, 1.5);
		cairo_stroke (cr);
	}
	cairo_destroy (cr);
	return true;
}

bool
ChordLane::on_button_press_event (GdkEventButton* ev)
{
	grab_focus ();
	if (ev->button != 1 || !_region) {
		return false;
	}
	if (ev->y < ruler_h) {
		/* the ruler moves the playhead */
		if (_session) {
			timepos_t const p (Beats::ticks (std::max<int64_t> (0, _origin + snap (x_tick (ev->x), true))));
			_session->request_locate (p.samples ());
		}
		_drag = 4;
		return true;
	}
	int       edge;
	const int b = block_at (ev->x, ev->y, edge);
	if (ev->type == GDK_2BUTTON_PRESS) {
		if (b < 0) {
			/* double-click on empty space: the selected pad goes there */
			const int pad = _pads.selected ();
			insert_notes (_pads.pitches (pad), _pads.name (pad), snap (x_tick (ev->x), ev->state & GDK_SHIFT_MASK), _pads.length ().to_ticks ());
		}
		return true;
	}
	if (ev->type != GDK_BUTTON_PRESS) {
		return true;
	}
	const bool toggle = ev->state & Gtkmm2ext::Keyboard::PrimaryModifier;
	const bool range  = (ev->state & GDK_SHIFT_MASK) && _sel >= 0 && _sel < (int) _blocks.size ();
	if (b < 0) {
		if (!toggle && !range) {
			_selection.clear ();
			_sel = -1;
		}
	} else if (toggle) {
		if (is_sel (b)) {
			_selection.erase (_blocks[b].start);
		} else {
			_selection.insert (_blocks[b].start);
		}
		_sel = b;
	} else if (range) {
		for (int i = std::min (_sel, b); i <= std::max (_sel, b); ++i) {
			_selection.insert (_blocks[i].start);
		}
	} else {
		select_only (b);
	}
	if (b >= 0) {
		_drag       = edge < 0 ? 2 : (edge > 0 ? 3 : 1);
		_drag_block = b;
		_drag_x0    = ev->x;
		_drag_delta = 0;
		_dragged    = false;
		_pads.play_notes (_blocks[b].pitches);
	}
	queue_draw ();
	return true;
}

bool
ChordLane::on_motion_notify_event (GdkEventMotion* ev)
{
	if (_drag == 4) {
		return true;
	}
	if (_drag && _drag_block >= 0) {
		const bool    fine = ev->state & GDK_SHIFT_MASK;
		const int64_t raw  = (int64_t) ((ev->x - _drag_x0) * Beats::PPQN / _ppb);
		Block const&  b    = _blocks[_drag_block];
		int64_t       d;
		if (_drag == 3) {
			d = snap (b.end + raw, fine) - b.end;
		} else {
			d = snap (b.start + raw, fine) - b.start;
			if (_drag == 1) {
				d = std::max (d, _r_start - b.start);
			}
		}
		if (std::abs (ev->x - _drag_x0) > 3) {
			_dragged = true;
		}
		if (d != _drag_delta || _dragged) {
			_drag_delta = d;
			queue_draw ();
		}
		return true;
	}
	int       edge;
	const int h = block_at (ev->x, ev->y, edge);
	if (h != _hover) {
		_hover = h;
		queue_draw ();
	}
	Glib::RefPtr<Gdk::Window> w = get_window ();
	if (w) {
		if (h >= 0 && edge) {
			w->set_cursor (Gdk::Cursor (Gdk::SB_H_DOUBLE_ARROW));
		} else if (h >= 0) {
			w->set_cursor (Gdk::Cursor (Gdk::FLEUR));
		} else {
			w->set_cursor ();
		}
	}
	return true;
}

bool
ChordLane::on_button_release_event (GdkEventButton* ev)
{
	if (ev->button != 1) {
		return false;
	}
	_pads.silence ();
	if (_drag && _drag != 4 && _dragged && _drag_delta != 0) {
		apply_drag ();
	}
	_drag       = 0;
	_drag_block = -1;
	_dragged    = false;
	queue_draw ();
	return true;
}

bool
ChordLane::on_leave_notify_event (GdkEventCrossing*)
{
	if (_hover >= 0) {
		_hover = -1;
		queue_draw ();
	}
	return false;
}

bool
ChordLane::on_scroll_event (GdkEventScroll* ev)
{
	if (!(ev->state & GDK_CONTROL_MASK)) {
		return false; /* the scrolled window scrolls */
	}
	if (ev->direction == GDK_SCROLL_UP) {
		_ppb = std::min (400.0, _ppb * 1.25);
	} else if (ev->direction == GDK_SCROLL_DOWN) {
		_ppb = std::max (8.0, _ppb / 1.25);
	}
	queue_resize_width ();
	return true;
}

bool
ChordLane::on_key_press_event (GdkEventKey* ev)
{
	return handle_key (ev);
}

bool
ChordLane::handle_key (GdkEventKey* ev)
{
	/* the window passes keys here and so does the widget's own handler: one event acts once */
	static guint32 last_time = 0;
	static guint   last_key  = 0;
	if (ev->time == last_time && ev->keyval == last_key && ev->time != 0) {
		return true;
	}
	const guint key  = gdk_keyval_to_lower (ev->keyval);
	const bool  ctrl = ev->state & Gtkmm2ext::Keyboard::PrimaryModifier;
	bool        done = true;
	if ((key == GDK_KEY_Delete || key == GDK_KEY_BackSpace) && !_selection.empty ()) {
		delete_selected ();
	} else if ((key == GDK_KEY_Left || key == GDK_KEY_Right) && !_blocks.empty ()) {
		const int i = _sel < 0 ? (key == GDK_KEY_Left ? (int) _blocks.size () - 1 : 0)
		                       : std::max (0, std::min ((int) _blocks.size () - 1, _sel + (key == GDK_KEY_Left ? -1 : 1)));
		select_only (i);
		_pads.play_notes (_blocks[i].pitches);
		Glib::signal_timeout ().connect_once ([this] () { _pads.silence (); }, 500);
	} else if (key == GDK_KEY_Escape && !_selection.empty ()) {
		_selection.clear ();
	} else if (ctrl && key == GDK_KEY_a) {
		for (auto const& b : _blocks) {
			_selection.insert (b.start);
		}
	} else if (ctrl && (key == GDK_KEY_c || key == GDK_KEY_x) && !_selection.empty ()) {
		copy_selection (_clipboard, _clip_span);
		if (key == GDK_KEY_x) {
			delete_selected ();
		}
	} else if (ctrl && key == GDK_KEY_v && !_clipboard.empty ()) {
		int64_t at = selection_end ();
		if (at < 0) {
			at = _playhead >= 0 ? snap (_playhead, true) : _r_start;
		}
		paste (_clipboard, _clip_span, at, _("paste chords"));
	} else if (ctrl && key == GDK_KEY_d && !_selection.empty ()) {
		std::vector<ClipNote> c;
		int64_t               span;
		copy_selection (c, span);
		paste (c, span, selection_end (), _("duplicate chords"));
	} else {
		done = false;
	}
	if (done) {
		last_time = ev->time;
		last_key  = ev->keyval;
		queue_draw ();
	}
	return done;
}

void
ChordLane::select_only (int i)
{
	_selection.clear ();
	if (i >= 0 && i < (int) _blocks.size ()) {
		_selection.insert (_blocks[i].start);
	}
	_sel = i;
}

void
ChordLane::select_starting (int64_t from, int64_t to)
{
	_selection.clear ();
	for (size_t i = 0; i < _blocks.size (); ++i) {
		if (_blocks[i].start >= from - chord_slop && _blocks[i].start < to) {
			_selection.insert (_blocks[i].start);
			_sel = (int) i;
		}
	}
}

int64_t
ChordLane::selection_end () const
{
	int64_t e = -1;
	for (size_t i = 0; i < _blocks.size (); ++i) {
		if (is_sel ((int) i)) {
			e = std::max (e, _blocks[i].end);
		}
	}
	return e;
}

void
ChordLane::copy_selection (std::vector<ClipNote>& out, int64_t& span) const
{
	out.clear ();
	int64_t from = -1, to = -1;
	for (size_t i = 0; i < _blocks.size (); ++i) {
		if (is_sel ((int) i)) {
			from = from < 0 ? _blocks[i].start : std::min (from, _blocks[i].start);
			to   = std::max (to, _blocks[i].end);
		}
	}
	span = std::max<int64_t> (0, to - from);
	for (size_t i = 0; i < _blocks.size (); ++i) {
		if (!is_sel ((int) i)) {
			continue;
		}
		for (auto const& n : _blocks[i].notes) {
			ClipNote c;
			c.off      = note_tick (n) - from;
			c.len      = n->length ().to_ticks ();
			c.pitch    = n->note ();
			c.velocity = n->velocity ();
			c.channel  = n->channel ();
			out.push_back (c);
		}
	}
}

void
ChordLane::paste (std::vector<ClipNote> const& clip, int64_t span, int64_t at, std::string const& what)
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || clip.empty ()) {
		return;
	}
	at = std::max (_r_start, at);
	if (at >= _r_end) {
		return;
	}
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (what);
	/* the chords where the pasted ones go are replaced */
	for (auto const& b : _blocks) {
		if (b.start >= at && b.start < at + span) {
			for (auto const& n : b.notes) {
				cmd->remove (n);
			}
		}
	}
	for (auto const& c : clip) {
		const int64_t t = at + c.off;
		if (t >= _r_end) {
			continue;
		}
		cmd->add (NotePtr (new Evoral::Note<Beats> (c.channel, source_beats (t), Beats::ticks (std::min (c.len, _r_end - t)), c.pitch, c.velocity)));
	}
	model->apply_diff_command_as_commit (*_history, cmd);
	select_starting (at, at + span);
	queue_draw ();
}

bool
ChordLane::on_focus_in_event (GdkEventFocus* ev)
{
	Gtkmm2ext::Keyboard::magic_widget_grab_focus ();
	return Gtk::DrawingArea::on_focus_in_event (ev);
}

bool
ChordLane::on_focus_out_event (GdkEventFocus* ev)
{
	Gtkmm2ext::Keyboard::magic_widget_drop_focus ();
	return Gtk::DrawingArea::on_focus_out_event (ev);
}

void
ChordLane::apply_drag ()
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || _drag_block < 0 || _drag_block >= (int) _blocks.size ()) {
		return;
	}
	Block const&                b   = _blocks[_drag_block];
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_drag == 1 ? _("move chord") : _("change chord length"));
	for (auto const& n : b.notes) {
		const int64_t t = note_tick (n);
		const int64_t e = t + n->length ().to_ticks ();
		if (_drag == 1) {
			cmd->change (n, MidiModel::NoteDiffCommand::StartTime, source_beats (t + _drag_delta));
		} else if (_drag == 2) {
			/* the start moves, the end stays */
			const int64_t ns = std::min (e - Beats::PPQN / 16, b.start + _drag_delta);
			cmd->change (n, MidiModel::NoteDiffCommand::StartTime, source_beats (ns));
			cmd->change (n, MidiModel::NoteDiffCommand::Length, Beats::ticks (e - ns));
		} else {
			const int64_t ne = std::max (t + Beats::PPQN / 16, b.end + _drag_delta);
			cmd->change (n, MidiModel::NoteDiffCommand::Length, Beats::ticks (ne - t));
		}
	}
	const int64_t keep = b.start + (_drag == 3 ? 0 : _drag_delta);
	model->apply_diff_command_as_commit (*_history, cmd);
	/* keep the moved chord selected */
	select_starting (keep, keep + 1);
}

void
ChordLane::delete_selected ()
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || _selection.empty ()) {
		return;
	}
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("delete chords"));
	int                         first = -1;
	for (size_t i = 0; i < _blocks.size (); ++i) {
		if (is_sel ((int) i)) {
			first = first < 0 ? (int) i : first;
			for (auto const& n : _blocks[i].notes) {
				cmd->remove (n);
			}
		}
	}
	model->apply_diff_command_as_commit (*_history, cmd);
	/* the next chord gets selected, so Delete can be held */
	select_only (_blocks.empty () ? -1 : std::min (first, (int) _blocks.size () - 1));
	queue_draw ();
}

void
ChordLane::insert_notes (std::vector<uint8_t> const& p, std::string const& name, int64_t tick, int64_t len)
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || p.empty ()) {
		return;
	}
	tick = std::max (_r_start, tick);
	if (tick >= _r_end) {
		return;
	}
	len = std::max<int64_t> (Beats::PPQN / 16, std::min (len, _r_end - tick));
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (string_compose (_("add chord %1"), name));
	/* dropped onto a chord: it takes that one's place and length */
	for (auto const& b : _blocks) {
		if (tick >= b.start && tick < b.end) {
			tick = b.start;
			len  = b.end - b.start;
			for (auto const& n : b.notes) {
				cmd->remove (n);
			}
			break;
		}
	}
	const uint8_t ch = (uint8_t) std::max (0, std::min (15, _channel));
	for (uint8_t n : p) {
		cmd->add (NotePtr (new Evoral::Note<Beats> (ch, source_beats (tick), Beats::ticks (len), n, 100)));
	}
	model->apply_diff_command_as_commit (*_history, cmd);
	select_starting (tick, tick + 1);
	queue_draw ();
}

bool
ChordLane::on_drag_motion (Glib::RefPtr<Gdk::DragContext> const& ctx, int x, int y, guint time)
{
	int64_t t = snap (x_tick (x), false);
	int     edge;
	const int b = block_at (x, std::max (y, ruler_h + 1), edge);
	if (b >= 0) {
		t = _blocks[b].start;
	}
	if (t != _drop_tick) {
		_drop_tick = t;
		queue_draw ();
	}
	ctx->drag_status (Gdk::ACTION_COPY, time);
	return true;
}

void
ChordLane::on_drag_leave (Glib::RefPtr<Gdk::DragContext> const&, guint)
{
	_drop_tick = -1;
	queue_draw ();
}

void
ChordLane::on_drag_data_received (Glib::RefPtr<Gdk::DragContext> const& ctx, int x, int y, Gtk::SelectionData const& data, guint, guint time)
{
	_drop_tick = -1;
	std::vector<uint8_t> notes;
	std::string          name;
	if (data.get_length () <= 0 || !_pads.drag_chord (data.get_data_as_string (), notes, name)) {
		ctx->drag_finish (false, false, time);
		return;
	}
	insert_notes (notes, name, snap (x_tick (x), false), _pads.length ().to_ticks ());
	ctx->drag_finish (true, false, time);
	(void) y;
}
