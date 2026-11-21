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

#include <ydk/gdkkeysyms.h>

#include "pbd/compose.h"

#include "ardour/midi_region.h"
#include "ardour/midi_track.h"
#include "ardour/session.h"

#include "canvas/canvas.h"

#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/keyboard.h"

#include "ardour_ui.h"
#include "gui_object.h"
#include "midi_editor_panel.h"
#include "pianoroll_midi_view.h"
#include "pianoroll.h"
#include "ui_config.h"

#include "widgets/ardour_icon.h"
#include "widgets/tooltips.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace Temporal;

namespace {

/** a toolbar icon: a SMuFL glyph of the Leland font, or a tie (codepoint 0) */
struct NoteGlyph {
	uint32_t codepoint;
	double   size; ///< font size relative to the button height
};

const NoteGlyph glyph_16th    = { 0xE1D9, 0.80 }; /* note16thUp */
const NoteGlyph glyph_8th     = { 0xE1D7, 0.80 }; /* note8thUp */
const NoteGlyph glyph_quarter = { 0xE1D5, 0.80 }; /* noteQuarterUp */
const NoteGlyph glyph_half    = { 0xE1D3, 0.80 }; /* noteHalfUp */
const NoteGlyph glyph_whole   = { 0xE1D2, 0.80 }; /* noteWhole */
const NoteGlyph glyph_dot     = { 0xE1E7, 1.60 }; /* augmentationDot */
const NoteGlyph glyph_rest    = { 0xE4E5, 0.75 }; /* restQuarter */
const NoteGlyph glyph_flat2   = { 0xE264, 0.75 };
const NoteGlyph glyph_flat    = { 0xE260, 0.75 };
const NoteGlyph glyph_natural = { 0xE261, 0.75 };
const NoteGlyph glyph_sharp   = { 0xE262, 0.75 };
const NoteGlyph glyph_sharp2  = { 0xE263, 1.10 };
const NoteGlyph glyph_tie     = { 0, 0 };
const NoteGlyph glyph_tuplet3 = { 0xE883, 1.10 };
const NoteGlyph glyph_tuplet5 = { 0xE885, 1.10 };

std::string
glyph_utf8 (uint32_t c)
{
	std::string s;
	s += (char) (0xE0 | (c >> 12));
	s += (char) (0x80 | ((c >> 6) & 0x3F));
	s += (char) (0x80 | (c & 0x3F));
	return s;
}

void
render_note_glyph (cairo_t* cr, int w, int h, uint32_t color, void* data)
{
	NoteGlyph const* g = static_cast<NoteGlyph const*> (data);
	Gtkmm2ext::set_source_rgba (cr, color);
	if (g->codepoint == 0) {
		/* tie: a slim crescent */
		double const x1 = w * 0.22, x2 = w * 0.78, y = h * 0.42, hh = h * 0.22;
		cairo_move_to (cr, x1, y);
		cairo_curve_to (cr, x1 + (x2 - x1) / 3, y + hh, x2 - (x2 - x1) / 3, y + hh, x2, y);
		cairo_curve_to (cr, x2 - (x2 - x1) / 3, y + hh * 0.6, x1 + (x2 - x1) / 3, y + hh * 0.6, x1, y);
		cairo_fill (cr);
		return;
	}
	std::string const t = glyph_utf8 (g->codepoint);
	cairo_select_font_face (cr, "Leland", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size (cr, h * g->size);
	cairo_text_extents_t te;
	cairo_text_extents (cr, t.c_str (), &te);
	/* centre the ink */
	cairo_move_to (cr, (w - te.width) / 2 - te.x_bearing, (h - te.height) / 2 - te.y_bearing);
	cairo_show_text (cr, t.c_str ());
}

} // namespace

MidiEditorPanel::MidiEditorPanel (std::string const& name, bool with_transport, bool expandable)
	: _pianoroll (new Pianoroll (name, with_transport, expandable))
	, _mode (KeyEditor)
	, key_button (_("Key Editor"))
	, drum_button (_("Drum Editor"))
	, score_button (_("Score Editor"))
	, chord_button (_("Chord Pads"))
	, chord_lane (chord_pads)
	, all_drums_button (_("All Drums"))
	, drum_notation_button (_("Drum Notation"))
	, input_button (_("Note Input"))
	, dot_button (".")
	, triplet_button ("3")
	, quint_button ("5")
	, zoom_out_button ("\u2212")
	, zoom_in_button ("+")
	, _note_base (1920)
	, _input (false)
	, _note_dot (false)
	, _note_triplet (false)
	, _note_quint (false)
	, _magic_focus (false)
{
	hpacker.pack_start (_pianoroll->contents(), true, true);
	_pianoroll->contents().show ();
	_pianoroll->hide_note_mode_button ();

	/* editor switch: three views of the same MIDI data */
	struct { ArdourWidgets::ArdourButton* b; EditorMode m; const char* tip; } modes[] = {
		{ &key_button,   KeyEditor,   _("Notes on a piano keyboard grid") },
		{ &drum_button,  DrumEditor,  _("Drum hits: one row per drum sound, notes as fixed-length hits") },
		{ &score_button, ScoreEditor, _("Music notation of the region, edited like MuseScore: N starts note input, A-G enter notes; Ctrl+wheel zooms") },
		{ &chord_button, ChordEditor, _("Chord pads: build chords from root, type, tension and bass; hold a pad to hear it, drag it into the notes to write it") },
	};
	for (auto const& m : modes) {
		m.b->set_name ("tab button");
		m.b->set_corner_radius (2);
		ArdourWidgets::set_tooltip (*m.b, m.tip);
		EditorMode const mode = m.m;
		m.b->signal_clicked.connect ([this, mode] () { set_editor_mode (mode); });
		mode_bar.pack_start (*m.b, false, false);
	}
	/* drum view: used sounds only, or the whole drum map */
	all_drums_button.set_name ("tab button");
	all_drums_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (all_drums_button, _("Show every drum sound of the instrument, not only the ones used in this pattern"));
	all_drums_button.signal_clicked.connect ([this] () {
		bool const yn = !all_drums_button.get_active ();
		all_drums_button.set_active (yn);
		_pianoroll->set_show_all_drums (yn);
	});
	mode_bar.pack_end (all_drums_button, false, false);

	/* score: pitched grand staff or drum notation */
	drum_notation_button.set_name ("tab button");
	drum_notation_button.set_corner_radius (2);
	ArdourWidgets::set_tooltip (drum_notation_button, _("Write the region as drum notation (one staff, kick and pedal hi-hat stems down, cymbals with x note heads)"));
	drum_notation_button.signal_clicked.connect ([this] () {
		bool const yn = !drum_notation_button.get_active ();
		drum_notation_button.set_active (yn);
		/* the button says what the region is: switched off, the notes are pitched again, so the key
		 * signature and the chord symbols come back (before, they stayed hidden as drum content
		 * until a detour through the Key Editor) */
		score_view.set_drum_content (yn);
		score_view.set_percussion (yn);
	});
	mode_bar.pack_end (drum_notation_button, false, false);

	/* the note input bar, laid out like MuseScore's: input mode | durations |
	 * dot, rest | accidentals | tie | tuplets; icons are music font glyphs */
	double const ui_scale = UIConfiguration::instance ().get_ui_scale ();
	auto style = [&] (ArdourWidgets::ArdourButton& b, NoteGlyph const* g, const char* tip) {
		b.set_name ("tab button");
		b.set_corner_radius (2);
		if (g) {
			b.set_icon (&render_note_glyph, const_cast<NoteGlyph*> (g));
		}
		b.set_size_request ((int) (30 * ui_scale), (int) (28 * ui_scale));
		ArdourWidgets::set_tooltip (b, tip);
	};
	auto separator = [&] () {
		Gtk::VSeparator* sep = manage (new Gtk::VSeparator);
		note_value_box.pack_start (*sep, false, false, 4);
	};

	style (input_button, 0, _("Note input (key: N, Esc leaves it). A-G enter a note at the cursor (Shift: add to the chord), "
	                          "0 a rest, Alt+2..9 an interval above, T a tied note, Left/Right move the cursor. "
	                          "Click adds a note (to the chord there), Shift+click replaces, right click enters a rest.\n"
	                          "Outside note input: click selects (Ctrl toggles, Shift selects a range, drag lassoes), "
	                          "Left/Right select the next chord, Up/Down transpose (Ctrl: octave, Alt+Shift: diatonic), "
	                          "R repeats, T ties to the next note, Q/W halve/double, Ctrl+C/V copy/paste"));
	input_button.set_icon (ArdourWidgets::ArdourIcon::ToolDraw);
	input_button.signal_clicked.connect ([this] () { set_input (!_input); });
	note_value_box.pack_start (input_button, false, false);
	separator ();

	/* short to long, as in MuseScore */
	struct { NoteGlyph const* glyph; int64_t ticks; const char* tip; } values[] = {
		{ &glyph_16th,    480,  _("16th note (key: 3): the value of entered notes, or the length of the selected notes") },
		{ &glyph_8th,     960,  _("Eighth note (key: 4)") },
		{ &glyph_quarter, 1920, _("Quarter note (key: 5)") },
		{ &glyph_half,    3840, _("Half note (key: 6)") },
		{ &glyph_whole,   7680, _("Whole note (key: 7)") },
	};
	for (auto const& v : values) {
		ArdourWidgets::ArdourButton* b = manage (new ArdourWidgets::ArdourButton);
		style (*b, v.glyph, v.tip);
		int64_t const t = v.ticks;
		b->signal_clicked.connect ([this, t] () { set_note_value (t); score_view.grab_focus (); });
		note_value_box.pack_start (*b, false, false);
		note_value_buttons.push_back (std::make_pair (b, v.ticks));
	}
	separator ();

	style (dot_button, &glyph_dot, _("Augmentation dot (key: .)"));
	dot_button.signal_clicked.connect ([this] () { _note_dot = !_note_dot; if (_note_dot) { _note_triplet = _note_quint = false; } apply_note_value (true); score_view.grab_focus (); });
	note_value_box.pack_start (dot_button, false, false);
	style (rest_button, &glyph_rest, _("Rest (key: 0): in note input a rest at the cursor, else the selected notes are removed"));
	rest_button.signal_clicked.connect ([this] () { score_view.rest_or_delete (); score_view.grab_focus (); });
	note_value_box.pack_start (rest_button, false, false);
	separator ();

	static NoteGlyph const* acc_glyphs[5] = { &glyph_flat2, &glyph_flat, &glyph_natural, &glyph_sharp, &glyph_sharp2 };
	static const char* acc_tips[5] = {
		N_("Double flat"), N_("Flat (key: -)"), N_("Natural (key: =)"), N_("Sharp (key: +)"), N_("Double sharp"),
	};
	for (int k = 0; k < 5; ++k) {
		style (acc_buttons[k], acc_glyphs[k], _(acc_tips[k]));
		int const alter = k - 2;
		acc_buttons[k].signal_clicked.connect ([this, alter] () { score_view.accidental (alter); score_view.grab_focus (); });
		note_value_box.pack_start (acc_buttons[k], false, false);
	}
	separator ();

	style (tie_button, &glyph_tie, _("Tie (key: T): in note input the last notes sound one more value, else the selected notes join the next ones of the same pitch"));
	tie_button.signal_clicked.connect ([this] () { score_view.tie (); score_view.grab_focus (); });
	note_value_box.pack_start (tie_button, false, false);
	separator ();

	style (triplet_button, &glyph_tuplet3, _("Triplets: quarter, eighth and sixteenth notes as triplets (key: Ctrl+3)"));
	triplet_button.signal_clicked.connect ([this] () { _note_triplet = !_note_triplet; if (_note_triplet) { _note_dot = _note_quint = false; } apply_note_value (true); score_view.grab_focus (); });
	note_value_box.pack_start (triplet_button, false, false);
	style (quint_button, &glyph_tuplet5, _("Quintuplets: five sixteenth notes in a beat (key: Ctrl+5)"));
	quint_button.signal_clicked.connect ([this] () { _note_quint = !_note_quint; if (_note_quint) { _note_dot = _note_triplet = false; _note_base = 480; } apply_note_value (true); score_view.grab_focus (); });
	note_value_box.pack_start (quint_button, false, false);
	note_value_box.set_spacing (1);
	note_value_box.set_border_width (2);
	apply_note_value (false);
	apply_note_value (false);
	mode_bar.set_spacing (2);
	mode_bar.set_border_width (3);

	/* score: notation of the region (read-only for now); Ctrl+wheel zooms */
	/* the vertical bar always shows: with an automatic one the width changes with it, the layout gets taller or
	 * shorter and the bar comes and goes in an endless resize loop (seen in the low bottom pane) */
	score_scroller.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_ALWAYS);
	score_scroller.add (score_view);
	score_placeholder.pack_start (note_value_box, false, false);
	score_placeholder.pack_start (score_scroller, true, true);

	/* status bar: what is selected (left), zoom (right) */
	status_label.set_alignment (0, 0.5);
	status_label.set_ellipsize (Pango::ELLIPSIZE_END);
	status_bar.pack_start (status_label, true, true, 6);
	for (ArdourWidgets::ArdourButton* b : { &zoom_out_button, &zoom_in_button, &view_mode_button }) {
		b->set_name ("tab button");
		b->set_corner_radius (2);
	}
	ArdourWidgets::set_tooltip (zoom_out_button, _("Zoom out (Ctrl+wheel)"));
	ArdourWidgets::set_tooltip (zoom_in_button, _("Zoom in (Ctrl+wheel)"));
	zoom_out_button.signal_clicked.connect ([this] () { score_view.zoom_step (-1); });
	zoom_in_button.signal_clicked.connect ([this] () { score_view.zoom_step (1); });
	ArdourWidgets::set_tooltip (view_mode_button, _("Page view (pages of A4 shape) or continuous view (one sheet)"));
	view_mode_button.signal_clicked.connect ([this] () { score_view.set_page_view (!score_view.page_view ()); });
	zoom_label.set_width_chars (5);
	status_bar.pack_end (zoom_in_button, false, false);
	status_bar.pack_end (zoom_label, false, false, 2);
	status_bar.pack_end (zoom_out_button, false, false);
	status_bar.pack_end (view_mode_button, false, false, 6);
	status_bar.set_border_width (2);
	score_placeholder.pack_start (status_bar, false, false);
	score_view.StatusChanged.connect (sigc::mem_fun (*this, &MidiEditorPanel::update_status));
	update_status ();

	/* in the bottom pane the score gets the keys first while it has the focus */
	score_view.signal_focus_in_event ().connect (sigc::mem_fun (*this, &MidiEditorPanel::score_focus_in), false);
	score_view.signal_focus_out_event ().connect (sigc::mem_fun (*this, &MidiEditorPanel::score_focus_out), false);
	score_view.signal_key_press_event ().connect (sigc::mem_fun (*this, &MidiEditorPanel::score_key_press), false);

	/* chord pads: dropped onto the notes they write the chord there */
	chord_pads.InsertRequested.connect (sigc::mem_fun (*this, &MidiEditorPanel::insert_chord_at_playhead));
	chord_pads.KeyChanged.connect (sigc::mem_fun (*this, &MidiEditorPanel::save_region_key));
	{
		std::vector<Gtk::TargetEntry> targets;
		targets.push_back (Gtk::TargetEntry (ChordPads::drag_target (), Gtk::TARGET_SAME_APP));
		Gtk::Widget* cv = _pianoroll->get_canvas ();
		cv->drag_dest_set (targets, Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
		cv->signal_drag_data_received ().connect (sigc::mem_fun (*this, &MidiEditorPanel::chord_dropped));
	}

	chord_lane_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_NEVER);
	chord_lane_scroller.add (chord_lane);
	chord_lane_scroller.set_size_request (-1, ChordLane::lane_h + 16); /* + the scrollbar */

	pack_start (mode_bar, false, false);
	pack_start (chord_pads, true, true);
	pack_start (chord_lane_scroller, false, false);
	pack_start (hpacker, true, true);
	pack_start (score_placeholder, true, true);
	show_all ();
	/* visibility of the two views is managed by set_editor_mode () */
	score_placeholder.set_no_show_all ();
	score_placeholder.hide ();
	hpacker.set_no_show_all ();
	chord_pads.set_no_show_all ();
	chord_pads.hide ();
	chord_lane_scroller.set_no_show_all ();
	chord_lane_scroller.hide ();
	all_drums_button.set_no_show_all ();
	all_drums_button.hide ();
	drum_notation_button.set_no_show_all ();
	drum_notation_button.hide ();
	note_value_box.set_no_show_all ();
	note_value_box.hide ();
	key_button.set_active (true);
}

void
MidiEditorPanel::set_editor_mode (EditorMode m)
{
	_mode = m;
	key_button.set_active (m == KeyEditor);
	drum_button.set_active (m == DrumEditor);
	score_button.set_active (m == ScoreEditor);
	chord_button.set_active (m == ChordEditor);
	if (m == ChordEditor) {
		chord_pads.set_channel (std::max (0, _pianoroll->visible_channel ()));
		chord_pads.show ();
		chord_lane.set_channel (_pianoroll->visible_channel ());
		chord_lane.set_history (&_pianoroll->history ());
		chord_lane.set_region (_region);
		chord_lane_scroller.show_all ();
		chord_lane_scroller.show ();
	} else {
		chord_pads.hide ();
		chord_lane_scroller.hide ();
		chord_lane.set_region (std::shared_ptr<ARDOUR::MidiRegion> ());
	}
	if (m == DrumEditor) {
		all_drums_button.show ();
	} else {
		all_drums_button.hide ();
	}
	if (m == ScoreEditor) {
		/* drums (drum view or MIDI channel 10) start as drum notation */
		bool const drums = _pianoroll->note_mode () == ARDOUR::Percussive || _pianoroll->visible_channel () == 9;
		drum_notation_button.set_active (drums);
		score_view.set_drum_content (drums);
		score_view.set_percussion (drums);
		score_view.set_history (&_pianoroll->history ());
		score_view.set_channel (std::max (0, _pianoroll->visible_channel ()));
		drum_notation_button.show ();
		note_value_box.show ();
	} else {
		drum_notation_button.hide ();
		note_value_box.hide ();
	}

	if (m == ScoreEditor) {
		hpacker.hide ();
		score_view.set_region (_region);
		score_scroller.show_all ();
		score_placeholder.show ();
		return;
	}

	score_placeholder.hide ();
	if (m == ChordEditor) {
		hpacker.hide ();
		return;
	}
	hpacker.show ();

	/* the drum editor shows notes as hits (percussive note mode) */
	ARDOUR::NoteMode const want = (m == DrumEditor) ? ARDOUR::Percussive : ARDOUR::Sustained;
	if (_pianoroll->note_mode () != want) {
		_pianoroll->note_mode_clicked ();
	}
}

MidiEditorPanel::~MidiEditorPanel ()
{
	if (_magic_focus) {
		Gtkmm2ext::Keyboard::magic_widget_drop_focus ();
	}
	delete _pianoroll;
}

void
MidiEditorPanel::set_session (ARDOUR::Session* s)
{
	_pianoroll->set_session (s);
	chord_lane.set_session (s);
}

void
MidiEditorPanel::set_show_source (bool yn)
{
	_pianoroll->set_show_source (yn);
}

/* the key set in the Chord Pads belongs to the region: kept in the session's GUI state, it comes back
 * after a restart and when the region is opened again, and the score writes its key signature */
static std::string
region_key_id (std::shared_ptr<ARDOUR::MidiRegion> r)
{
	return std::string (X_("chord-key ")) + r->id ().to_s ();
}

void
MidiEditorPanel::load_region_key ()
{
	if (!_region || !ARDOUR_UI::instance () || !ARDOUR_UI::instance ()->gui_object_state) {
		score_view.set_key_hint (0);
		return;
	}
	GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
	bool              empty = true;
	std::string const root  = gs.get_string (region_key_id (_region), X_("root"), &empty);
	if (empty || root.empty ()) {
		score_view.set_key_hint (0);
		return;
	}
	std::string const scale = gs.get_string (region_key_id (_region), X_("scale"));
	chord_pads.set_key (atoi (root.c_str ()), atoi (scale.c_str ()));
	score_view.set_key_hint (chord_pads.key_pitch_classes ());
}

void
MidiEditorPanel::save_region_key ()
{
	if (_region && ARDOUR_UI::instance () && ARDOUR_UI::instance ()->gui_object_state) {
		GUIObjectState& gs (*ARDOUR_UI::instance ()->gui_object_state);
		gs.set_property (region_key_id (_region), X_("root"), chord_pads.key_root ());
		gs.set_property (region_key_id (_region), X_("scale"), chord_pads.key_scale ());
		if (ARDOUR::Session* sess = ARDOUR_UI::instance ()->the_session ()) {
			sess->set_dirty ();
		}
	}
	score_view.set_key_hint (_region ? chord_pads.key_pitch_classes () : 0);
}

void
MidiEditorPanel::add_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	_pianoroll->add_region (region, track);
}

void
MidiEditorPanel::set_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	_pianoroll->add_region (region, track);
	_pianoroll->set_region (region);
	_region = region;
	_track  = track;
	load_region_key ();
	score_view.set_track (track);
	chord_pads.set_track (track);
	if (_mode == ScoreEditor) {
		score_view.set_region (region);
	}
	if (_mode == ChordEditor) {
		chord_lane.set_region (region);
	}

	/* follow the track's note mode (drum tracks open in the drum editor) */
	if (_mode != ScoreEditor && _mode != ChordEditor) {
		set_editor_mode (_pianoroll->note_mode () == ARDOUR::Percussive ? DrumEditor : KeyEditor);
	}

}

void
MidiEditorPanel::replace_region (std::shared_ptr<MidiTrack> track, std::shared_ptr<MidiRegion> region)
{
	_pianoroll->replace_region (region, track);
	_region = region;
	load_region_key ();
	if (_mode == ScoreEditor) {
		score_view.set_region (region);
	}
	if (_mode == ChordEditor) {
		chord_lane.set_region (region);
	}
}

void
MidiEditorPanel::remove_regions ()
{
	_pianoroll->remove_regions ();
}

void
MidiEditorPanel::set_note_value (int64_t ticks)
{
	_note_base = ticks;
	if (!_input && !score_view.has_selection ()) {
		/* nothing to change the length of: start entering notes */
		_input = true;
	}
	apply_note_value (true);
}

void
MidiEditorPanel::set_input (bool yn)
{
	_input = yn;
	apply_note_value (false);
	score_view.grab_focus ();
}

void
MidiEditorPanel::apply_note_value (bool to_selection)
{
	int64_t len = _note_base;
	if (len && _note_dot) {
		len = len * 3 / 2;
	} else if (len && _note_quint && len == 480) {
		len = 384;
	} else if (len && _note_triplet && (len == 1920 || len == 960 || len == 480)) {
		len = len * 2 / 3;
	}
	score_view.set_insert_length (_input ? len : 0);
	input_button.set_active (_input);
	for (auto const& b : note_value_buttons) {
		b.first->set_active (b.second == _note_base);
	}
	dot_button.set_active (_note_dot);
	triplet_button.set_active (_note_triplet);
	quint_button.set_active (_note_quint);

	/* MuseScore: outside note input a value changes the selected notes */
	if (to_selection && !_input && score_view.has_selection ()) {
		score_view.set_selection_length (len);
	}
}

bool
MidiEditorPanel::handle_key (GdkEventKey* ev)
{
	/* the chord lane: Delete, arrows, undo / redo of the region edits */
	if (_mode == ChordEditor) {
		if (chord_lane.handle_key (ev)) {
			return true;
		}
		if (ev->state & Gtkmm2ext::Keyboard::PrimaryModifier) {
			if (ev->keyval == GDK_z || ev->keyval == GDK_Z) {
				if (ev->state & Gtkmm2ext::Keyboard::TertiaryModifier) {
					_pianoroll->redo ();
				} else {
					_pianoroll->undo ();
				}
				return true;
			}
			if (ev->keyval == GDK_y || ev->keyval == GDK_Y) {
				_pianoroll->redo ();
				return true;
			}
		}
	}
	if (_mode == ScoreEditor) {
		guint const mods = ev->state & (Gtkmm2ext::Keyboard::PrimaryModifier | Gtkmm2ext::Keyboard::SecondaryModifier);
		/* note input and note values, as in MuseScore: 3 = 16th ... 7 = whole */
		if (!mods) {
			switch (ev->keyval) {
			case GDK_n:
			case GDK_N:
				set_input (!_input);
				return true;
			case GDK_Escape:
				if (_input) {
					set_input (false);
					return true;
				}
				break;
			case GDK_3: set_note_value (480);  return true;
			case GDK_4: set_note_value (960);  return true;
			case GDK_5: set_note_value (1920); return true;
			case GDK_6: set_note_value (3840); return true;
			case GDK_7: set_note_value (7680); return true;
			case GDK_period:
			case GDK_KP_Decimal:
				_note_dot = !_note_dot;
				if (_note_dot) { _note_triplet = _note_quint = false; }
				apply_note_value (true);
				return true;
			default:
				break;
			}
		} else if (mods == Gtkmm2ext::Keyboard::PrimaryModifier) {
			/* tuplets: Ctrl+3 triplet, Ctrl+5 quintuplet (of sixteenths) */
			if (ev->keyval == GDK_3) {
				_note_triplet = !_note_triplet;
				if (_note_triplet) { _note_dot = _note_quint = false; }
				apply_note_value (true);
				return true;
			}
			if (ev->keyval == GDK_5) {
				_note_quint = !_note_quint;
				if (_note_quint) { _note_dot = _note_triplet = false; _note_base = 480; }
				apply_note_value (true);
				return true;
			}
		}
		/* letters, rests, arrows, Delete, copy / paste */
		if (score_view.handle_key (ev)) {
			return true;
		}
		/* undo/redo the region edits here, not in the main editor */
		if (ev->state & Gtkmm2ext::Keyboard::PrimaryModifier) {
			if (ev->keyval == GDK_z || ev->keyval == GDK_Z) {
				if (ev->state & Gtkmm2ext::Keyboard::TertiaryModifier) {
					_pianoroll->redo ();
				} else {
					_pianoroll->undo ();
				}
				return true;
			}
			if (ev->keyval == GDK_y || ev->keyval == GDK_Y) {
				_pianoroll->redo ();
				return true;
			}
		}
	}
	return false;
}

void
MidiEditorPanel::update_status ()
{
	status_label.set_text (score_view.status_text ());
	zoom_label.set_text (string_compose ("%1%%", score_view.zoom_percent ()));
	view_mode_button.set_text (score_view.page_view () ? _("Page view") : _("Continuous view"));
}

bool
MidiEditorPanel::score_focus_in (GdkEventFocus*)
{
	if (!_magic_focus) {
		Gtkmm2ext::Keyboard::magic_widget_grab_focus ();
		_magic_focus = true;
	}
	return false;
}

bool
MidiEditorPanel::score_focus_out (GdkEventFocus*)
{
	if (_magic_focus) {
		Gtkmm2ext::Keyboard::magic_widget_drop_focus ();
		_magic_focus = false;
	}
	return false;
}

bool
MidiEditorPanel::score_key_press (GdkEventKey* ev)
{
	return handle_key (ev);
}


void
MidiEditorPanel::insert_chord (int pad, Temporal::Beats start)
{
	insert_notes (chord_pads.pitches (pad), chord_pads.name (pad), start);
}

void
MidiEditorPanel::insert_notes (std::vector<uint8_t> const& p, std::string const& name, Temporal::Beats start)
{
	MidiView* mv = _pianoroll->midi_view ();
	if (!mv || !mv->midi_region () || p.empty ()) {
		return;
	}
	uint8_t const ch = (uint8_t) std::max (0, std::min (15, _pianoroll->visible_channel ()));
	mv->clear_note_selection ();
	mv->start_note_diff_command (string_compose (_("add chord %1"), name));
	for (uint8_t n : p) {
		mv->note_diff_add_note (std::shared_ptr<MidiView::NoteType> (new MidiView::NoteType (ch, start, chord_pads.length (), n, 100)), true);
	}
	mv->apply_note_diff ();
}

void
MidiEditorPanel::insert_chord_at_playhead (int pad)
{
	if (_mode == ChordEditor) {
		/* the piano roll is hidden: the chord lane writes it */
		chord_lane.insert_at_playhead (chord_pads.pitches (pad), chord_pads.name (pad), chord_pads.length ().to_ticks ());
		return;
	}
	std::shared_ptr<MidiRegion> r = _region;
	MidiView* mv = _pianoroll->midi_view ();
	if (mv && mv->midi_region ()) {
		r = mv->midi_region ();
	}
	if (!r) {
		return;
	}
	timepos_t pos (r->session ().audible_sample ());
	if (pos < r->position () || pos >= r->nt_last ()) {
		/* outside the region: its start */
		pos = r->position ();
	}
	_pianoroll->snap_to (pos, Temporal::RoundNearest, ARDOUR::SnapToGrid_Scaled);
	insert_chord (pad, r->absolute_time_to_source_beats (pos));
}

void
MidiEditorPanel::chord_dropped (Glib::RefPtr<Gdk::DragContext> const& ctx, int x, int y, Gtk::SelectionData const& data, guint, guint time)
{
	MidiView* mv = _pianoroll->midi_view ();
	if (!mv || !mv->midi_region () || data.get_length () <= 0) {
		ctx->drag_finish (false, false, time);
		return;
	}
	std::vector<uint8_t> notes;
	std::string          name;
	if (!chord_pads.drag_chord (data.get_data_as_string (), notes, name)) {
		ctx->drag_finish (false, false, time);
		return;
	}

	/* the same time a click there would add a note at (NoteCreateDrag) */
	GdkEventButton ev;
	memset (&ev, 0, sizeof (ev));
	ev.type = GDK_BUTTON_PRESS;
	ev.x    = x;
	ev.y    = y;
	timepos_t pos (_pianoroll->window_event_sample ((GdkEvent*) &ev));
	_pianoroll->snap_to (pos, Temporal::RoundDownMaybe, ARDOUR::SnapToGrid_Scaled);

	std::shared_ptr<MidiRegion> r = mv->midi_region ();
	Temporal::Beats start;
	if (!mv->on_timeline ()) {
		start = r->absolute_time_to_source_beats (timepos_t (r->source_position ().beats () + pos.beats ()));
	} else {
		start = r->absolute_time_to_source_beats (pos);
	}
	insert_notes (notes, name, start);
	ctx->drag_finish (true, false, time);
}
