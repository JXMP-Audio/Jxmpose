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

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <cairomm/context.h>

#include "ytkmm/drawingarea.h"

#include "pbd/signals.h"

#include "evoral/Note.h"
#include "temporal/beats.h"

namespace ARDOUR {
	class MidiRegion;
	class MidiTrack;
}

namespace PBD {
	class HistoryOwner;
}

/** Music notation view and editor of one MIDI region.
 *
 * Editing: pick a note value (set_insert_length) and click into the staff
 * to add a note; click a note to select it (Shift adds), Delete removes,
 * Up/Down transposes by a semitone (Ctrl: octave). Changes go through the
 * MIDI model's diff commands, so they can be undone.
 *
 * Notes are quantized to a 16th grid (or a tuplet grid where the MIDI
 * plays triplets, quintuplets or sixteenth triplets) and laid out on a
 * grand staff (treble for middle C and above, bass below), with up to two
 * voices per staff, the key signature of the track (or one guessed from
 * the notes), rests, ties, dots, accidentals, ledger lines, flags and
 * beams. Glyphs come from a
 * SMuFL music font (Leland, bundled under the SIL OFL).
 */
class ScoreView : public Gtk::DrawingArea
{
public:
	ScoreView ();
	~ScoreView ();

	void set_region (std::shared_ptr<ARDOUR::MidiRegion>);

	/** drum notation: one staff, percussion clef, hands stems up, feet
	 * (kick, pedal hi-hat) stems down, cymbals with x note heads */
	void set_percussion (bool);
	bool percussion () const { return _percussion; }
	/** the region holds drums: no key signature, even on the grand staff */
	void set_drum_content (bool);

	/** undo history for edits (the pianoroll's) */
	void set_history (PBD::HistoryOwner* h) { _history = h; }
	/** MIDI channel (0..15) for new notes */
	void set_channel (uint8_t c) { _channel = c; }
	/** length of notes added by clicking, in ticks; 0: select only */
	void set_insert_length (int64_t len) { _insert_len = len; }
	int64_t insert_length () const { return _insert_len; }

	/** the track that plays notes while editing */
	void set_track (std::shared_ptr<ARDOUR::MidiTrack>);
	bool has_selection () const { return !_selection.empty (); }
	/** give the selected notes a new length (ticks) */
	void set_selection_length (int64_t);

	/** keyboard editing; @return true if handled */
	bool handle_key (GdkEventKey*);

	typedef std::shared_ptr<Evoral::Note<Temporal::Beats>> NotePtr;

	/** staff space in pixels (zoom) */
	void set_staff_space (double);
	double staff_space () const { return _sp; }

	/* a grid step (16th note) in ticks */
	static const int64_t grid = 480;

	struct Piece {
		int64_t              start;   ///< ticks from bar start
		int64_t              len;     ///< ticks
		std::vector<uint8_t> pitches; ///< empty: rest
		std::vector<bool>    tie_out; ///< per pitch: tied into the next piece
		std::vector<NotePtr> notes;   ///< per pitch: the model note
		int                  tuplet = 0;   ///< 0, or the division of its beat (3, 5, 6)
	};

	/** a note clipped to one bar */
	struct Seg {
		int64_t start, end;  ///< ticks from bar start
		uint8_t pitch;
		bool    tie_out;     ///< continues in the next bar
		bool    hit;         ///< shorter than a 32nd (drum hit)
		NotePtr note;
	};

	struct Bar {
		std::vector<Piece> staff[2];   ///< 0: treble, 1: bass (drums: hands, feet)
		std::vector<Piece> voice2[2];  ///< second voice (stems down), if any
		std::vector<int64_t> onsets;   ///< union of piece starts, sorted
		std::vector<double>  onset_x;  ///< x offset of each onset from bar start
		double               width;
		double               x;        ///< left edge (set by layout)
		size_t               system;   ///< system index (set by layout)
		double               key_x;    ///< room for a key change at the bar start (0: none)
	};

protected:
	bool on_expose_event (GdkEventExpose*);
	void on_size_allocate (Gtk::Allocation&);
	bool on_scroll_event (GdkEventScroll*);
	bool on_button_press_event (GdkEventButton*);
	bool on_button_release_event (GdkEventButton*);
	bool on_motion_notify_event (GdkEventMotion*);

private:
	std::shared_ptr<ARDOUR::MidiRegion> _region;
	PBD::ScopedConnectionList           _connections;

	double  _sp;          ///< staff space (px)
	bool    _percussion;
	int64_t _bar_len;     ///< ticks per bar
	int     _beats;       ///< time signature
	int     _beat_value;
	int     _first_bar;   ///< bar number of the first bar
	std::vector<Bar> _bars;

	struct System {
		size_t first_bar;
		size_t n_bars;
		double y;           ///< top of the treble staff
		double x0;          ///< where the first bar starts
		double stretch;     ///< justification factor for bar widths
	};
	std::vector<System> _systems;
	int _layout_width;

	/* editing */
	PBD::HistoryOwner* _history;
	uint8_t            _channel;
	int64_t            _insert_len;
	int64_t            _origin;    ///< absolute ticks of the first bar
	int64_t            _r_start;   ///< region start, ticks from the origin
	int64_t            _r_end;
	std::set<NotePtr>  _selection;

	struct Hit {
		double  x0, y0, x1, y1;
		NotePtr note;
	};
	std::vector<Hit> _hits; ///< note heads as drawn

	std::shared_ptr<ARDOUR::MidiTrack> _track;

	/* dragging selected notes */
	NotePtr _drag_note;      ///< the note that was grabbed
	double  _drag_x0, _drag_y0;
	bool    _dragging;
	int64_t _drag_dt;        ///< ticks
	int     _drag_dp;        ///< semitones

	int64_t note_tick (NotePtr const&) const;
	bool    position_of (int64_t tick, uint8_t pitch, double& x, double& y) const;
	int64_t snap_step () const;
	int64_t snap_time (int64_t ticks_in_bar) const;
	bool    _drum_content;
	void    play (NotePtr const&);

	bool    pitch_at (double x, double y, size_t& bar, uint8_t& pitch) const;
	int64_t time_in_bar (Bar const&, double x) const;
	void    add_note (int64_t tick, uint8_t pitch);
	void    delete_selection ();
	void    transpose_selection (int semitones);

	void model_changed ();
	void rebuild ();
	void layout ();
	void build_bar_pieces (std::vector<Piece>& out, size_t bar_index, int staff, std::vector<Seg> const&);

	/* tuplets: beats (index from the first bar) divided in 3 (eighth
	 * triplets), 5 (quintuplets) or 6 (sixteenth triplets) */
	std::map<int64_t, int> _beat_div[2];
	int64_t beat_ticks () const;
	int     beat_div (int staff, int64_t abs_tick) const;
	/** notatable pieces (start, len, tuplet division) of a duration inside one bar */
	std::vector<std::pair<std::pair<int64_t, int64_t>, int>> split_piece (size_t bar_index, int staff, int64_t start, int64_t len, bool dots) const;
	static void piece_value (Piece const&, int64_t& base, bool& dotted);

	/* key signature: number of sharps (> 0) or flats (< 0) */
	int              _fifths;      ///< key of the first bar
	std::vector<int> _bar_fifths;  ///< key per bar (key changes)
	int    fifths_at (size_t bar) const;
	double key_width (int fifths) const;
	/** width of a key change at the start of a bar (cancelling naturals + new key) */
	double key_change_width (int from, int to) const;

	double system_height () const;
	double staff_y (System const&, int staff) const;

	void draw (Cairo::RefPtr<Cairo::Context>);
	void draw_glyph (Cairo::RefPtr<Cairo::Context>, uint32_t codepoint, double x, double y, double scale = 1.0);
	double glyph_width (Cairo::RefPtr<Cairo::Context>, uint32_t codepoint);
};
