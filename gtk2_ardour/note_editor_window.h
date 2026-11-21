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

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <ytkmm/adjustment.h>
#include <ytkmm/box.h>
#include <ytkmm/button.h>
#include <ytkmm/drawingarea.h>
#include <ytkmm/label.h>
#include <ytkmm/menu.h>
#include <ytkmm/scrollbar.h>
#include <ytkmm/spinbutton.h>
#include <ytkmm/table.h>

#include "pbd/progress.h"

#include "ardour_window.h"
#include "note_edit.h"

namespace ARDOUR {
	class AudioRegion;
	class Playlist;
}
class RegionSelection;

/** Per-note editor of one audio region (vocal, bass, any single voice): every note of the recording is a blob
 *  on a keyboard grid. Drag a blob up / down to move its pitch (whole semitones, Alt = free), sideways to move
 *  it in time, drag its left / right edge to change its length (notes sung legato stretch with it);
 *  double-click puts it exactly on the nearest semitone, S splits a note at the mouse, M merges the selected
 *  ones. Every change renders the region again (from the original audio) as one undo step. */
class NoteEditorWindow : public ArdourWindow
{
public:
	static void open (RegionSelection const&);

	NoteEditorWindow (std::shared_ptr<ARDOUR::AudioRegion>);
	~NoteEditorWindow ();

	std::shared_ptr<ARDOUR::AudioRegion> region () const { return _region; }

private:
	class Canvas : public Gtk::DrawingArea
	{
	public:
		Canvas (NoteEditorWindow& w) : _w (w) {}
	protected:
		bool on_expose_event (GdkEventExpose*);
		bool on_button_press_event (GdkEventButton*);
		bool on_button_release_event (GdkEventButton*);
		bool on_motion_notify_event (GdkEventMotion*);
		bool on_scroll_event (GdkEventScroll*);
	private:
		NoteEditorWindow& _w;
	};

	std::shared_ptr<ARDOUR::AudioRegion> _region;
	std::weak_ptr<ARDOUR::Playlist>      _playlist;
	NoteEdit::Analysis                   _an;
	std::vector<NoteEdit::Note>          _notes;
	std::vector<bool>                    _sel;

	Gtk::VBox        _vbox;
	Gtk::HBox        _toolbar;
	Gtk::Button      _snap_button;
	Gtk::Button      _reset_button;
	Gtk::Button      _reset_time_button;
	Gtk::Button      _correct_button;
	Gtk::Label       _gain_label;
	Gtk::Adjustment  _gain_adj;
	Gtk::SpinButton  _gain_spin;
	Gtk::Label       _formant_label;
	Gtk::Adjustment  _formant_adj;
	Gtk::SpinButton  _formant_spin;
	bool             _syncing;
	std::vector<bool> _synced_sel;
	sigc::connection _spin_timeout;
	Gtk::Menu        _menu;
	Gtk::Button      _zoom_in;
	Gtk::Button      _zoom_out;
	Gtk::Label       _status;
	Gtk::Table       _table;
	Canvas           _canvas;
	Gtk::Adjustment  _hadj;
	Gtk::Adjustment  _vadj;
	Gtk::HScrollbar  _hscroll;
	Gtk::VScrollbar  _vscroll;

	double _pps;    ///< pixels per second
	double _lane;   ///< pixels per semitone
	float  _hi;     ///< MIDI note at the top of the grid
	float  _lo;     ///< MIDI note at the bottom of the grid

	/* mouse */
	enum Drag { NoDrag, Undecided, PitchDrag, TimeDrag, StartDrag, EndDrag, RectDrag, TransDrag };
	Drag   _drag;
	int    _grab;
	double _x0, _y0, _x1, _y1;
	std::vector<float> _base;
	std::vector<NoteEdit::Note> _base_notes;
	bool   _moved;
	double _mouse_x, _mouse_y;
	int    _hover_trans; ///< note whose transition handle is under the mouse, -1 none
	float  _trans_base;

	/* transition handles: one between two notes less than 300 ms apart, on the second note */
	struct TransHandle {
		int    note; ///< the second note (it holds the transition length)
		double x, y;
	};
	std::vector<TransHandle> transition_handles () const;
	int  trans_handle_at (double x, double y) const;
	void draw_trans_handles (cairo_t*);

	sigc::connection _timer;
	ARDOUR::samplepos_t _last_playhead;

	/* work that takes a while runs in a worker thread: the analysis when the window opens (the canvas shows
	 * a ring with the percentage and takes no edits meanwhile) and rendering an edit (a small ring in the
	 * corner, editing goes on; a new edit made meanwhile is rendered after it) */
	class JobProgress : public PBD::Progress
	{
	public:
		JobProgress () : value (0) {}
		std::atomic<float> value;
		void stop () { cancel (); }
	private:
		void set_overall_progress (float p) { value = p; }
	};
	enum Busy { NotBusy, Analysing, Rendering };
	Busy                                 _busy;
	bool                                 _analysis_failed;
	std::thread                          _worker;
	std::atomic<bool>                    _job_done;
	int                                  _job_rv;
	std::unique_ptr<JobProgress>         _job_progress;
	std::shared_ptr<ARDOUR::AudioRegion> _job_region;
	NoteEdit::SourceRange                _job_range;    ///< what the worker reads (it never touches the region)
	NoteEdit::Analysis                   _job_an;
	std::vector<NoteEdit::Note>          _job_notes;
	std::vector<std::vector<float> >     _job_out;
	std::string                          _job_what;
	std::string                          _render_again; ///< an edit made while rendering: its undo name
	int64_t                              _job_started;  ///< g_get_monotonic_time () at the start
	double                               _spin_phase;
	sigc::connection                     _busy_timer;

	void start_job (Busy);
	bool busy_tick ();
	void stop_job ();
	void analysis_done ();
	void render_done ();
	bool region_unchanged () const; ///< the region still plays the audio the job read
	void discard_render ();
	void draw_busy (cairo_t*, int w, int h);

	void apply (std::string const& what);
	void session_going_away ();

	/* geometry */
	double key_width () const { return 46; }
	double ruler_height () const { return 20; }
	double x_of (ARDOUR::samplepos_t master_pos) const;
	double y_of (float midi) const;
	float  midi_at (double y) const;
	int    note_at (double x, double y) const;
	int    edge_at (double x, double y, int& note) const; ///< -1 left edge, 1 right edge, 0 none
	ARDOUR::samplepos_t pos_at (double x) const;
	void   update_adjustments ();
	void   zoom (double factor, double around_x);

	void draw (cairo_t*, int w, int h);
	void draw_transitions (cairo_t*, double kw, double w);
	std::vector<NoteEdit::Glide> _gl; ///< glides of the notes as drawn (refreshed per draw)
	void snap_selected ();
	void reset_selected ();
	void shift_selected (float semitones);
	bool time_edit (Drag, ARDOUR::samplecnt_t delta);
	void split_at (int note, ARDOUR::samplepos_t out_pos);
	void merge_selected ();
	void reset_time ();
	void correct_pitch_dialog ();
	void sync_note_controls ();
	void note_controls_changed ();
	void popup_menu (int note, GdkEventButton*);
	void update_status (std::string const& hint = std::string ());
	bool any_selected () const;
	bool on_key_press_event (GdkEventKey*);
	void tick ();
	void follow_undo ();
	void load_region_notes ();

	friend class Canvas;
};
