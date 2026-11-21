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
#include <string>
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
 * Editing works like MuseScore. In note input mode (set_insert_length > 0)
 * an input cursor shows where the next note goes: A-G enter a note there
 * (the octave nearest the previous note, replacing what was there),
 * Shift+A-G add to the chord, 0 enters a rest, Alt+2..9 add an interval,
 * T ties, clicking adds (Shift+click replaces, right click: rest).
 * Otherwise click selects (Ctrl toggles, Shift selects a range, drag in
 * empty space lassoes), Left/Right step through the chords, Up/Down
 * transpose (Ctrl: octave, Alt+Shift: diatonic), Delete removes. Changes
 * go through the MIDI model's diff commands, so they can be undone.
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
	/** note input mode: the length of new notes in ticks; 0 leaves it */
	void set_insert_length (int64_t len);
	int64_t insert_length () const { return _insert_len; }
	bool in_input () const { return _insert_len > 0; }

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
	/** zoom in percent of the default staff size */
	int  zoom_percent () const;
	/** page view (A4 pages) or continuous view (one sheet as long as the music) */
	void set_page_view (bool);
	bool page_view () const { return _page_view; }
	void zoom_step (int dir);

	/** the selection or input position, as in MuseScore's status bar */
	std::string status_text () const;
	/** status_text () or the zoom changed */
	sigc::signal<void> StatusChanged;

	/* note input toolbar */
	void rest_or_delete ();               ///< note input: rest at the cursor; else delete the selection
	void accidental (int alter) { set_accidental (alter); }
	void tie () { add_tie (); }

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
	bool on_leave_notify_event (GdkEventCrossing*);

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
		size_t page;
		double y;           ///< top of the system
		double left;        ///< where the staff lines start
		double x0;          ///< where the first bar starts
		double stretch;     ///< justification factor for bar widths
	};
	std::vector<System> _systems;
	int _layout_width;

	/* page view: pages of A4 shape stacked on the score background */
	double _page_x, _page_w, _page_h, _margin, _title_h;
	size_t _n_pages;
	bool   _page_view;
	double page_gap () const;
	double page_top (size_t page) const;
	std::string instrument_name () const;

	std::string _last_status;
	void emit_status () { StatusChanged (); }

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

	/* insert preview: where a click would add a note */
	bool    _hover;
	int64_t _hover_tick;     ///< ticks from the first bar
	uint8_t _hover_pitch;
	void    update_hover (double x, double y);
	void    clear_hover ();
	void    draw_hover (Cairo::RefPtr<Cairo::Context>);

	/* note input cursor (ticks from the first bar) and the chord entered last */
	int64_t _cursor;
	int     _cursor_staff;
	int64_t _chord_tick;     ///< -1: none (Shift+letter adds to the chord there)
	int64_t _chord_len;
	int     _chord_top;      ///< highest pitch of that chord
	int     _last_pitch;     ///< previous entered pitch (octave of letter entry), -1: none
	void    draw_cursor (Cairo::RefPtr<Cairo::Context>);
	void    set_cursor (int64_t tick, int staff);
	void    reset_cursor ();

	/* letter, rest and interval entry */
	void enter_letter (int letter, bool add_to_chord);
	void enter_rest (int64_t tick, int staff);
	void enter_interval (int steps);
	void set_accidental (int alter);
	void add_tie ();
	void repeat_selection ();
	void scale_selection_length (bool longer);
	void diatonic_transpose (int steps);

	/* keyboard selection */
	NotePtr _anchor;         ///< first end of a Shift+click range
	void step_chord (int dir, bool extend);
	void step_bar (int dir);
	void step_in_chord (int dir);
	void select_range (NotePtr const& a, NotePtr const& b);
	void select_all ();
	void ensure_visible (int64_t tick);
	void play_selection ();

	/* lasso */
	bool   _lasso;
	bool   _lasso_add;
	double _lasso_x0, _lasso_y0, _lasso_x1, _lasso_y1;

	/* clipboard: notes relative to the earliest one */
	struct Clip {
		int64_t dt, len;
		uint8_t pitch, velocity;
	};
	std::vector<Clip> _clipboard;
	void copy_selection ();
	void paste ();

	int  staff_of (uint8_t pitch) const;
	/** notes inside the region, sorted by time and pitch (ticks from the first bar) */
	std::vector<std::pair<int64_t, NotePtr>> region_notes () const;
	/** add a note of the insert length; replace: notes of its staff inside it go */
	NotePtr write_note (int64_t tick, int64_t len, uint8_t pitch, bool replace);
	/** highest pitch starting at a tick on a staff, -1: none */
	int     top_pitch_at (int64_t tick, int staff) const;
	/** in note input: the next letter goes near the selected (last entered) note */
	void    follow_selection_pitch ();

	int64_t note_tick (NotePtr const&) const;
	bool    position_of (int64_t tick, uint8_t pitch, double& x, double& y) const;
	int64_t snap_step () const;
	int64_t snap_time (int64_t ticks_in_bar) const;
	bool    _drum_content;
	void    play (NotePtr const&);

	bool    pitch_at (double x, double y, size_t& bar, uint8_t& pitch) const;
	int64_t time_in_bar (Bar const&, double x) const;
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

	/* chord symbols (from the sounding notes, per beat), dynamics (from
	 * velocities) and accents; ticks from the first bar */
	struct ChordSym {
		int64_t tick;
		int     root;    ///< pitch class
		int     quality; ///< index into the chord templates
		int     bass;    ///< pitch class of a different bass note, or -1
	};
	struct DynMark {
		int64_t tick;
		int     level;   ///< 0 pp .. 5 ff
	};
	struct Hairpin {
		int64_t start, end;
		bool    cresc;
	};
	std::vector<ChordSym> _chords;
	std::vector<DynMark>  _dyns;
	std::vector<Hairpin>  _hairpins;
	std::set<NotePtr>     _accents;
	void analyze_harmony_and_dynamics (std::vector<std::pair<std::pair<int64_t, int64_t>, NotePtr>> const& notes, size_t n_bars);
	void draw_chord_symbol (Cairo::RefPtr<Cairo::Context>, ChordSym const&, double x, double y, int fifths);

	double system_height () const;
	double staff_y (System const&, int staff) const;

	void draw (Cairo::RefPtr<Cairo::Context>);
	void draw_glyph (Cairo::RefPtr<Cairo::Context>, uint32_t codepoint, double x, double y, double scale = 1.0);
	double glyph_width (Cairo::RefPtr<Cairo::Context>, uint32_t codepoint);
};
