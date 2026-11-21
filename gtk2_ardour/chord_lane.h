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

#include <ytkmm/drawingarea.h>

#include "pbd/signals.h"

#include "evoral/Note.h"
#include "temporal/beats.h"

namespace ARDOUR {
	class MidiRegion;
	class Session;
}
namespace PBD {
	class HistoryOwner;
}

class ChordPads;

/** The region's notes as a row of chords (Chord Pads mode, instead of the piano roll): notes starting
 *  together form one block named by the chord they make. Blocks are moved and resized like notes (their
 *  notes go with them), Delete removes one, pads and chords of the circle dropped here are written at the
 *  drop position (onto a block: replace it). Ctrl+wheel zooms, a click on the ruler moves the playhead. */
class ChordLane : public Gtk::DrawingArea
{
public:
	ChordLane (ChordPads&);
	~ChordLane ();

	void set_session (ARDOUR::Session* s) { _session = s; }
	void set_region (std::shared_ptr<ARDOUR::MidiRegion>);
	void set_history (PBD::HistoryOwner* h) { _history = h; }
	void set_channel (int c) { _channel = c; }

	/** write notes at a position of the region's timeline (ticks from the start of its first bar) */
	void insert_notes (std::vector<uint8_t> const&, std::string const& name, int64_t tick, int64_t len);
	/** Delete removes the selected chord, Left/Right select the neighbours */
	bool handle_key (GdkEventKey*);

protected:
	bool on_expose_event (GdkEventExpose*);
	bool on_button_press_event (GdkEventButton*);
	bool on_button_release_event (GdkEventButton*);
	bool on_motion_notify_event (GdkEventMotion*);
	bool on_scroll_event (GdkEventScroll*);
	bool on_key_press_event (GdkEventKey*);
	bool on_focus_in_event (GdkEventFocus*);
	bool on_focus_out_event (GdkEventFocus*);
	bool on_leave_notify_event (GdkEventCrossing*);
	void on_drag_data_received (Glib::RefPtr<Gdk::DragContext> const&, int, int, Gtk::SelectionData const&, guint, guint);
	bool on_drag_motion (Glib::RefPtr<Gdk::DragContext> const&, int, int, guint);
	void on_drag_leave (Glib::RefPtr<Gdk::DragContext> const&, guint);

private:
	typedef std::shared_ptr<Evoral::Note<Temporal::Beats> > NotePtr;

	struct Block {
		int64_t              start, end; ///< ticks from _origin
		std::vector<NotePtr> notes;
		std::vector<int>     pitches;    ///< sorted
	};

	ChordPads&                          _pads;
	ARDOUR::Session*                    _session;
	std::shared_ptr<ARDOUR::MidiRegion> _region;
	PBD::HistoryOwner*                  _history;
	PBD::ScopedConnectionList           _connections;
	sigc::connection                    _timer;
	int                                 _channel;

	std::vector<Block> _blocks;
	int64_t            _origin;              ///< absolute ticks of the first bar line
	int64_t            _r_start, _r_end;     ///< the region, ticks from _origin
	int                _bar_ticks;
	int                _first_bar;
	double             _ppb;                 ///< pixels per beat
	int64_t            _playhead;            ///< ticks from _origin, -1 outside

	int     _sel;
	int     _hover;
	int     _drag;       ///< 0 none, 1 move, 2 left edge, 3 right edge, 4 ruler
	int     _drag_block;
	double  _drag_x0;
	int64_t _drag_delta;
	bool    _dragged;
	int64_t _drop_tick;  ///< where a dragged chord would land, -1 none

	static const int ruler_h = 20;

	void    rebuild ();
	void    queue_resize_width ();
	double  tick_x (int64_t t) const { return 8 + (double) (t - _r_start) * _ppb / Temporal::Beats::PPQN; }
	int64_t x_tick (double x) const { return _r_start + (int64_t) ((x - 8) * Temporal::Beats::PPQN / _ppb); }
	int64_t snap (int64_t t, bool fine) const;
	int     block_at (double x, double y, int& edge) const;
	bool    tick_timer ();

	Temporal::Beats source_beats (int64_t tick) const;
	int64_t         note_tick (NotePtr const&) const;

	void apply_drag ();
	void delete_selected ();
};
