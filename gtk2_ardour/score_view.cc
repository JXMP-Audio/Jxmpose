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
#include <map>
#include <set>

#include "pbd/compose.h"

#include "temporal/tempo.h"

#include "ardour/midi_model.h"
#include "ardour/midi_region.h"

#include "pbd/history_owner.h"

#include "gtkmm2ext/keyboard.h"

#include "gui_thread.h"
#include "note_player.h"
#include "score_view.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace Temporal;

namespace {

/* SMuFL code points */
enum : uint32_t {
	Brace          = 0xE000,
	GClef          = 0xE050,
	FClef          = 0xE062,
	PercClef       = 0xE069,
	NoteheadX      = 0xE0A9,
	NoteheadCircleX = 0xE0B3,
	NoteheadTriangle = 0xE0BE,
	TimeSig0       = 0xE080,
	Tuplet3        = 0xE883,
	NoteheadWhole  = 0xE0A2,
	NoteheadHalf   = 0xE0A3,
	NoteheadBlack  = 0xE0A4,
	AugDot         = 0xE1E7,
	Flag8thUp      = 0xE240,
	Flag8thDown    = 0xE241,
	Flag16thUp     = 0xE242,
	Flag16thDown   = 0xE243,
	AccFlat        = 0xE260,
	AccNatural     = 0xE261,
	AccSharp       = 0xE262,
	RestWhole      = 0xE4E3,
	RestHalf       = 0xE4E4,
	RestQuarter    = 0xE4E5,
	Rest8th        = 0xE4E6,
	Rest16th       = 0xE4E7,
};

const int64_t whole   = 7680;
const int64_t half    = 3840;
const int64_t quarter = 1920;
const int64_t eighth  = 960;

std::string
utf8 (uint32_t c)
{
	std::string s;
	if (c < 0x80) {
		s += (char) c;
	} else if (c < 0x800) {
		s += (char) (0xC0 | (c >> 6));
		s += (char) (0x80 | (c & 0x3F));
	} else {
		s += (char) (0xE0 | (c >> 12));
		s += (char) (0x80 | ((c >> 6) & 0x3F));
		s += (char) (0x80 | (c & 0x3F));
	}
	return s;
}

/** spelling with sharps: letter (0 = C .. 6 = B) and alteration */
void
spell (uint8_t pitch, int& diatonic, int& alter)
{
	static const int letter[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
	static const int acc[12]    = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	int const octave = pitch / 12 - 1;
	diatonic = octave * 7 + letter[pitch % 12];
	alter    = acc[pitch % 12];
}

/** drum staff position (diatonic step, treble positions) and note head */
void
drum_position (uint8_t pitch, int& d, uint32_t& head)
{
	head = NoteheadBlack;
	switch (pitch) {
	case 35: case 36: d = 31; break;                         /* kick: F4 */
	case 37: d = 35; head = NoteheadX; break;                /* side stick */
	case 38: case 40: d = 35; break;                         /* snare: C5 */
	case 39: d = 35; head = NoteheadX; break;                /* clap */
	case 41: d = 32; break;                                  /* floor toms */
	case 43: d = 33; break;
	case 45: d = 34; break;                                  /* toms */
	case 47: case 48: d = 36; break;
	case 50: d = 37; break;
	case 42: d = 39; head = NoteheadX; break;                /* closed hi-hat: G5 */
	case 46: d = 39; head = NoteheadCircleX; break;          /* open hi-hat */
	case 44: d = 29; head = NoteheadX; break;                /* pedal hi-hat: D4 */
	case 49: case 52: case 55: case 57: d = 40; head = NoteheadX; break; /* crashes: A5 */
	case 51: case 53: case 59: d = 38; head = NoteheadX; break;          /* rides: F5 */
	case 56: d = 37; head = NoteheadTriangle; break;         /* cowbell */
	case 54: d = 37; head = NoteheadX; break;                /* tambourine */
	default: d = 37; break;
	}
}

bool
is_foot (uint8_t pitch)
{
	return pitch == 35 || pitch == 36 || pitch == 44;
}

/* diatonic step of the bottom staff line: E4 (treble), G2 (bass) */
int
bottom_line (int staff)
{
	return staff == 0 ? 4 * 7 + 2 : 2 * 7 + 4;
}

/** split a duration into notatable values (16th grid), aligned to the beat */
std::vector<std::pair<int64_t, int64_t>>
split_duration (int64_t start, int64_t len, int64_t bar_len, bool dots = true)
{
	struct V { int64_t len, align; };
	static const V values[] = {
		{ whole, whole }, { half * 3 / 2 * 1, half }, { half, half },
		{ quarter * 3 / 2, quarter }, { quarter, quarter },
		{ eighth * 3 / 2, eighth }, { eighth, eighth }, { ScoreView::grid, ScoreView::grid },
	};
	std::vector<std::pair<int64_t, int64_t>> r;
	while (len > 0) {
		bool found = false;
		for (auto const& v : values) {
			if (v.len == whole && bar_len < whole) {
				continue;
			}
			bool const dotted = v.len != v.align;
			if (dotted) {
				/* rests are written without dots; a dotted note must
				 * not run over the next boundary of twice its base
				 * value (beat or half bar), to keep the beats visible */
				int64_t const span = 2 * v.align;
				if (!dots || start + v.len > (start / span + 1) * span) {
					continue;
				}
			}
			if (v.len <= len && start % v.align == 0) {
				r.push_back (std::make_pair (start, v.len));
				start += v.len;
				len -= v.len;
				found = true;
				break;
			}
		}
		if (!found) {
			/* off the 16th grid: should not happen, swallow the rest */
			r.push_back (std::make_pair (start, len));
			break;
		}
	}
	return r;
}

/** undotted value and dot flag */
void
note_value (int64_t len, int64_t& base, bool& dotted)
{
	dotted = false;
	base   = len;
	for (int64_t b : { whole, half, quarter, eighth, ScoreView::grid }) {
		if (len == b) {
			return;
		}
		if (len == b * 3 / 2) {
			base   = b;
			dotted = true;
			return;
		}
	}
}

} // namespace

ScoreView::ScoreView ()
	: _sp (9. * UIConfiguration::instance ().get_ui_scale ())
	, _percussion (false)
	, _bar_len (whole)
	, _beats (4)
	, _beat_value (4)
	, _first_bar (1)
	, _layout_width (0)
	, _history (0)
	, _channel (0)
	, _insert_len (0)
	, _origin (0)
	, _r_start (0)
	, _r_end (0)
	, _drag_x0 (0)
	, _drag_y0 (0)
	, _dragging (false)
	, _drag_dt (0)
	, _drag_dp (0)
{
	add_events (Gdk::SCROLL_MASK | Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::BUTTON1_MOTION_MASK | Gdk::KEY_PRESS_MASK);
	set_can_focus (true);
}

ScoreView::~ScoreView ()
{
}

void
ScoreView::set_region (std::shared_ptr<MidiRegion> r)
{
	_connections.drop_connections ();
	_region = r;

	if (_region) {
		std::shared_ptr<MidiModel> m (_region->model ());
		if (m) {
			m->ContentsChanged.connect (_connections, invalidator (*this), std::bind (&ScoreView::model_changed, this), gui_context ());
		}
		_region->PropertyChanged.connect (_connections, invalidator (*this), std::bind (&ScoreView::model_changed, this), gui_context ());
	}
	rebuild ();
}

void
ScoreView::set_staff_space (double sp)
{
	_sp = std::max (4., std::min (30., sp));
	rebuild ();
}

void
ScoreView::set_percussion (bool yn)
{
	if (_percussion != yn) {
		_percussion = yn;
		rebuild ();
	}
}

void
ScoreView::model_changed ()
{
	rebuild ();
}

void
ScoreView::rebuild ()
{
	_bars.clear ();

	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());

	if (!model) {
		layout ();
		return;
	}

	TempoMap::SharedPtr tm (TempoMap::use ());
	timepos_t const pos = _region->position ();
	timepos_t const end = _region->end_position ();

	Temporal::MeterPoint const& meter (tm->meter_at (pos));
	_beats      = meter.divisions_per_bar ();
	_beat_value = meter.note_value ();
	int64_t const div_ticks = (Beats::PPQN * 4) / _beat_value;
	_bar_len    = _beats * div_ticks;

	BBT_Argument const bbt (tm->bbt_at (pos));
	_first_bar = bbt.bars;

	/* origin: start of the bar that contains the region start */
	int64_t const origin  = pos.beats ().to_ticks () - ((bbt.beats - 1) * div_ticks + bbt.ticks * 4 / _beat_value);
	int64_t const r_start = pos.beats ().to_ticks () - origin;
	int64_t const r_end   = end.beats ().to_ticks () - origin;
	_origin  = origin;
	_r_start = r_start;
	_r_end   = r_end;

	size_t const n_bars = std::max<int64_t> (1, (r_end + _bar_len - 1) / _bar_len);

	/* segments per bar and staff */
	std::vector<std::vector<Seg>> segs[2];
	segs[0].resize (n_bars);
	segs[1].resize (n_bars);

	{
		MidiModel::ReadLock lm (model->read_lock ());

		/* forget selected notes that are gone */
		std::set<NotePtr> still;
		for (auto const& n : model->notes ()) {
			if (_selection.count (n)) {
				still.insert (n);
			}
		}
		_selection.swap (still);

		/* triplet beats: a note starts clearly nearer to the eighth-triplet
		 * grid (1/3, 2/3 of the beat) than to the 16th grid, or ends
		 * exactly on it */
		_triplet_beats[0].clear ();
		_triplet_beats[1].clear ();
		int64_t const bt = beat_ticks ();
		auto staff_of = [&] (uint8_t pitch) {
			/* staff, or voice in drum notation (0: hands, 1: feet) */
			return _percussion ? (is_foot (pitch) ? 1 : 0) : (pitch >= 60 ? 0 : 1);
		};
		if (bt == quarter) {
			int64_t const tg = quarter / 3;
			for (auto const& n : model->notes ()) {
				int64_t const s = _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - origin;
				int64_t const e = _region->source_beats_to_absolute_beats (n->end_time ()).to_ticks () - origin;
				if (e <= r_start || s >= r_end || s < 0) {
					continue;
				}
				int64_t const off = s % bt;
				int64_t const n3  = ((off + tg / 2) / tg) * tg;
				int64_t const d3  = std::abs (off - n3);
				int64_t const d4  = std::abs (off - ((off + grid / 2) / grid) * grid);
				if ((n3 == tg || n3 == 2 * tg) && d3 * 2 < d4) {
					_triplet_beats[staff_of (n->note ())].insert (s / bt);
				}
				int64_t const eoff = e % bt;
				if (e > 0 && (eoff == tg || eoff == 2 * tg)) {
					_triplet_beats[staff_of (n->note ())].insert (e / bt);
				}
			}
		}

		auto quantize = [&] (int staff, int64_t t) {
			int64_t const b   = t / bt;
			int64_t const g   = (bt == quarter && _triplet_beats[staff].count (b)) ? quarter / 3 : grid;
			int64_t const off = t - b * bt;
			return b * bt + ((off + g / 2) / g) * g;
		};

		for (auto const& n : model->notes ()) {
			int64_t s = _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - origin;
			int64_t e = _region->source_beats_to_absolute_beats (n->end_time ()).to_ticks () - origin;
			if (e <= r_start || s >= r_end) {
				continue;
			}
			s = std::max (s, r_start);
			e = std::min (e, r_end);
			bool const hit = (e - s) < grid;

			int const staff = staff_of (n->note ());

			/* quantize to the 16th (or triplet) grid */
			s = quantize (staff, s);
			e = quantize (staff, e);
			if (e <= s) {
				e = s + (_triplet_beats[staff].count (s / bt) ? quarter / 3 : grid);
			}

			/* split at bar lines, tied */
			while (s < e) {
				size_t const b = s / _bar_len;
				if (b >= n_bars) {
					break;
				}
				int64_t const bar_end = (b + 1) * _bar_len;
				Seg sg;
				sg.start   = s - b * _bar_len;
				sg.end     = std::min (e, bar_end) - b * _bar_len;
				sg.pitch   = n->note ();
				sg.tie_out = e > bar_end && b + 1 < n_bars;
				sg.note    = n;
				sg.hit     = hit;
				segs[staff][b].push_back (sg);
				s = bar_end;
			}
		}
	}

	_bars.resize (n_bars);
	for (size_t b = 0; b < n_bars; ++b) {
		Bar& bar (_bars[b]);
		build_bar_pieces (bar, b, 0, segs[0][b]);
		build_bar_pieces (bar, b, 1, segs[1][b]);

		std::set<int64_t> on;
		for (int st = 0; st < 2; ++st) {
			for (auto const& p : bar.staff[st]) {
				on.insert (p.start);
			}
		}
		bar.onsets.assign (on.begin (), on.end ());
		bar.onset_x.clear ();

		/* spacing: grows with the square root of the duration */
		double x = 0;
		for (size_t k = 0; k < bar.onsets.size (); ++k) {
			int64_t const next = (k + 1 < bar.onsets.size ()) ? bar.onsets[k + 1] : _bar_len;
			int64_t const gap  = next - bar.onsets[k];
			bool accidental = false;
			for (int st = 0; st < 2; ++st) {
				for (auto const& p : bar.staff[st]) {
					if (p.start != bar.onsets[k]) {
						continue;
					}
					for (auto pi : p.pitches) {
						int const pc = pi % 12;
						accidental |= (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
					}
				}
			}
			x += accidental ? _sp * 1.2 : 0;
			bar.onset_x.push_back (x);
			x += _sp * (1.8 + 1.5 * std::sqrt ((double) gap / grid));
		}
		bar.width = _sp * 1.4 + x + _sp * 0.4;
	}

	layout ();
}

void
ScoreView::build_bar_pieces (Bar& bar, size_t bar_index, int staff, std::vector<Seg> const& in)
{
	std::vector<Seg> segs (in);
	std::sort (segs.begin (), segs.end (), [] (Seg const& a, Seg const& b) {
		return a.start < b.start || (a.start == b.start && a.pitch < b.pitch);
	});

	std::vector<Piece>& out (bar.staff[staff]);
	out.clear ();

	auto add_rest = [&] (int64_t s, int64_t len) {
		if (s == 0 && len == _bar_len) {
			/* whole bar rest */
			Piece p;
			p.start = 0;
			p.len   = _bar_len;
			out.push_back (p);
			return;
		}
		for (auto const& v : split_piece (bar_index, staff, s, len, false)) {
			Piece p;
			p.start  = v.first.first;
			p.len    = v.first.second;
			p.tuplet = v.second;
			out.push_back (p);
		}
	};

	int64_t t = 0;
	size_t  i = 0;
	while (i < segs.size ()) {
		int64_t const st = segs[i].start;
		size_t j = i;
		int64_t en = _bar_len;
		while (j < segs.size () && segs[j].start == st) {
			en = std::min (en, segs[j].end);
			++j;
		}
		int64_t const next = (j < segs.size ()) ? segs[j].start : _bar_len;
		en = std::min (en, next);

		/* readability: a hit (shorter than a 16th) followed by a rest in
		 * the same beat takes the rest's time (16th + rest -> 8th or
		 * quarter), like drum hits are usually written. Notes with a
		 * real length are shown as they are. */
		{
			int64_t const beat     = (_beat_value == 8 && _beats % 3 == 0) ? eighth * 3 : quarter;
			int64_t const beat_end = (st / beat + 1) * beat;
			bool tied = false;
			bool hits = true;
			for (size_t k = i; k < j; ++k) {
				tied |= segs[k].tie_out;
				hits &= segs[k].hit;
			}
			if (!tied && hits && en < next && en < beat_end) {
				en = std::min (next, beat_end);
			}
		}

		if (st < t) {
			/* overlaps the previous chord (single voice): skip */
			i = j;
			continue;
		}
		if (st > t) {
			add_rest (t, st - t);
		}

		std::vector<uint8_t> pitches;
		std::vector<bool>    ties;
		std::vector<NotePtr> notes;
		for (size_t k = i; k < j; ++k) {
			if (std::find (pitches.begin (), pitches.end (), segs[k].pitch) != pitches.end ()) {
				continue;
			}
			pitches.push_back (segs[k].pitch);
			ties.push_back (segs[k].tie_out && en == _bar_len);
			notes.push_back (segs[k].note);
		}

		auto const parts = split_piece (bar_index, staff, st, en - st, true);
		for (size_t k = 0; k < parts.size (); ++k) {
			Piece p;
			p.start   = parts[k].first.first;
			p.len     = parts[k].first.second;
			p.tuplet  = parts[k].second;
			p.pitches = pitches;
			p.notes   = notes;
			p.tie_out = (k + 1 < parts.size ()) ? std::vector<bool> (pitches.size (), true) : ties;
			out.push_back (p);
		}
		t = en;
		i = j;
	}
	if (t < _bar_len) {
		add_rest (t, _bar_len - t);
	}
}

int64_t
ScoreView::beat_ticks () const
{
	return (_beat_value == 8 && _beats % 3 == 0) ? eighth * 3 : (Beats::PPQN * 4) / _beat_value;
}

bool
ScoreView::triplet_beat (int staff, int64_t abs_tick) const
{
	return beat_ticks () == quarter && _triplet_beats[staff].count (abs_tick / quarter);
}

std::vector<std::pair<std::pair<int64_t, int64_t>, bool>>
ScoreView::split_piece (size_t bar_index, int staff, int64_t start, int64_t len, bool dots) const
{
	std::vector<std::pair<std::pair<int64_t, int64_t>, bool>> r;
	int64_t const bar0 = bar_index * _bar_len;
	int64_t const tg   = quarter / 3;

	while (len > 0) {
		int64_t const abs = bar0 + start;
		if (triplet_beat (staff, abs)) {
			/* inside a triplet beat: quarter (2/3) and eighth (1/3) values */
			int64_t const beat_end = (abs / quarter + 1) * quarter - bar0;
			int64_t const chunk    = std::min (len, beat_end - start);
			int64_t const off      = start % quarter;
			int64_t v;
			if (off == 0 && chunk >= quarter) {
				v = quarter;
				r.push_back (std::make_pair (std::make_pair (start, v), false));
			} else {
				v = (off == 0 && chunk >= 2 * tg) ? 2 * tg : std::min (chunk, tg);
				r.push_back (std::make_pair (std::make_pair (start, v), true));
			}
			start += v;
			len   -= v;
			continue;
		}

		/* plain values, up to the next triplet beat */
		int64_t limit = len;
		for (int64_t t = (abs / quarter + 1) * quarter; t < bar0 + start + len; t += quarter) {
			if (triplet_beat (staff, t)) {
				limit = t - abs;
				break;
			}
		}
		for (auto const& v : split_duration (start, limit, _bar_len, dots)) {
			r.push_back (std::make_pair (v, false));
		}
		start += limit;
		len   -= limit;
	}
	return r;
}

void
ScoreView::piece_value (Piece const& p, int64_t& base, bool& dotted)
{
	if (p.tuplet) {
		dotted = false;
		base   = (p.len >= 2 * quarter / 3) ? quarter : eighth;
		return;
	}
	note_value (p.len, base, dotted);
}

double
ScoreView::system_height () const
{
	if (_percussion) {
		return _sp * (6 + 4 + 6);
	}
	/* treble + gap + bass, with room for ledger lines */
	return _sp * (6 + 4 + 7 + 4 + 6);
}

double
ScoreView::staff_y (System const& s, int staff) const
{
	if (_percussion) {
		return s.y + _sp * 6;
	}
	return s.y + _sp * 6 + (staff == 0 ? 0 : _sp * 11);
}

void
ScoreView::layout ()
{
	_systems.clear ();

	Gtk::Allocation const a (get_allocation ());
	double const margin = _sp * 2;
	double const avail  = std::max (200., (double) a.get_width () - 2 * margin);
	_layout_width       = a.get_width ();

	size_t b = 0;
	double y = _sp * 2;
	while (b < _bars.size ()) {
		System s;
		s.first_bar = b;
		s.y         = y;
		s.x0        = margin + _sp * 4.5 + (_systems.empty () ? _sp * 3 : 0);

		double w = 0;
		size_t n = 0;
		while (b + n < _bars.size () && (n == 0 || s.x0 + w + _bars[b + n].width <= margin + avail)) {
			w += _bars[b + n].width;
			++n;
		}
		s.n_bars = n;
		bool const last = (b + n >= _bars.size ());
		double const room = margin + avail - s.x0;
		s.stretch = (!last && w > 0) ? std::max (1., room / w) : std::min (1.5, std::max (1., room / std::max (1., w)));

		double bx = s.x0;
		for (size_t k = 0; k < n; ++k) {
			_bars[b + k].x      = bx;
			_bars[b + k].system = _systems.size ();
			bx += _bars[b + k].width * s.stretch;
		}
		_systems.push_back (s);

		b += n;
		y += system_height ();
	}

	set_size_request (-1, (int) std::max (y + _sp * 2, _sp * 30));
	queue_draw ();
}

void
ScoreView::on_size_allocate (Gtk::Allocation& a)
{
	Gtk::DrawingArea::on_size_allocate (a);
	if (a.get_width () != _layout_width) {
		layout ();
	}
}

bool
ScoreView::on_scroll_event (GdkEventScroll* ev)
{
	if (!(ev->state & Gtkmm2ext::Keyboard::PrimaryModifier)) {
		return false;
	}
	if (ev->direction == GDK_SCROLL_UP) {
		set_staff_space (_sp * 1.1);
		return true;
	} else if (ev->direction == GDK_SCROLL_DOWN) {
		set_staff_space (_sp / 1.1);
		return true;
	}
	return false;
}

bool
ScoreView::on_expose_event (GdkEventExpose* ev)
{
	Cairo::RefPtr<Cairo::Context> cr = get_window ()->create_cairo_context ();
	cr->rectangle (ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	cr->clip ();
	draw (cr);
	return true;
}

void
ScoreView::draw_glyph (Cairo::RefPtr<Cairo::Context> cr, uint32_t c, double x, double y, double scale)
{
	cr->select_font_face ("Leland", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
	cr->set_font_size (4 * _sp * scale);
	cr->move_to (x, y);
	cr->show_text (utf8 (c));
}

double
ScoreView::glyph_width (Cairo::RefPtr<Cairo::Context> cr, uint32_t c)
{
	cr->select_font_face ("Leland", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
	cr->set_font_size (4 * _sp);
	Cairo::TextExtents te;
	cr->get_text_extents (utf8 (c), te);
	return te.x_advance;
}

void
ScoreView::draw (Cairo::RefPtr<Cairo::Context> cr)
{
	Gtk::Allocation const a (get_allocation ());

	_hits.clear ();

	/* paper */
	cr->set_source_rgb (0.97, 0.96, 0.94);
	cr->paint ();

	cr->set_source_rgb (0.1, 0.1, 0.1);

	if (_bars.empty ()) {
		cr->select_font_face ("Sans", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_NORMAL);
		cr->set_font_size (14);
		cr->move_to (20, 30);
		cr->show_text (_("No MIDI region"));
		return;
	}

	double const sp     = _sp;
	double const line_w = std::max (1., sp * 0.13);
	double const stem_w = std::max (1., sp * 0.12);
	double const headw  = glyph_width (cr, NoteheadBlack);
	double const margin = sp * 2;

	/* staff position: pitch spelling, or the drum map */
	auto pos = [&] (uint8_t pitch, int& d, int& alt) {
		if (_percussion) {
			uint32_t head;
			drum_position (pitch, d, head);
			alt = 0;
		} else {
			spell (pitch, d, alt);
		}
	};

	struct Pending { double x, y; size_t system; bool below; };
	std::map<uint8_t, Pending> pending_tie[2];

	auto draw_tie = [&] (double x1, double x2, double y, bool below) {
		double const h  = (below ? 1 : -1) * std::min (sp * 1.2, std::max (sp * 0.5, (x2 - x1) * 0.12));
		double const yy = y + (below ? 0.6 : -0.6) * sp;
		double const dx = (x2 - x1) / 3;
		cr->move_to (x1, yy);
		cr->curve_to (x1 + dx, yy + h, x2 - dx, yy + h, x2, yy);
		cr->curve_to (x2 - dx, yy + h * 0.55, x1 + dx, yy + h * 0.55, x1, yy);
		cr->fill ();
	};

	for (size_t si = 0; si < _systems.size (); ++si) {
		System const& sys (_systems[si]);

		double x_end = sys.x0;
		for (size_t b = 0; b < sys.n_bars; ++b) {
			x_end += _bars[sys.first_bar + b].width * sys.stretch;
		}

		double const top0 = staff_y (sys, 0);
		double const top1 = staff_y (sys, 1);

		/* staff lines */
		cr->set_line_width (line_w);
		for (int st = 0; st < (_percussion ? 1 : 2); ++st) {
			double const top = st == 0 ? top0 : top1;
			for (int l = 0; l < 5; ++l) {
				cr->move_to (margin, top + l * sp);
				cr->line_to (x_end, top + l * sp);
			}
		}
		cr->stroke ();

		/* system bar line and brace */
		cr->set_line_width (line_w * 1.5);
		cr->move_to (margin, top0);
		cr->line_to (margin, top1 + 4 * sp);
		cr->stroke ();
		if (!_percussion) {
			double const span = top1 + 4 * sp - top0;
			cr->save ();
			cr->translate (margin - sp * 1.6, top1 + 4 * sp);
			cr->scale (2., span / (4 * sp));
			draw_glyph (cr, Brace, 0, 0);
			cr->restore ();
		}

		/* clefs */
		if (_percussion) {
			draw_glyph (cr, PercClef, margin + sp * 1.2, top0 + 2 * sp);
		} else {
			draw_glyph (cr, GClef, margin + sp * 0.7, top0 + 3 * sp);
			draw_glyph (cr, FClef, margin + sp * 0.7, top1 + 1 * sp);
		}

		/* time signature on the first system */
		if (si == 0) {
			for (int st = 0; st < (_percussion ? 1 : 2); ++st) {
				double const top = st == 0 ? top0 : top1;
				std::string const n = string_compose ("%1", _beats);
				std::string const d = string_compose ("%1", _beat_value);
				double x = margin + sp * 4.6;
				for (char c : n) {
					draw_glyph (cr, TimeSig0 + (c - '0'), x, top + sp);
					x += glyph_width (cr, TimeSig0 + (c - '0'));
				}
				x = margin + sp * 4.6;
				for (char c : d) {
					draw_glyph (cr, TimeSig0 + (c - '0'), x, top + 3 * sp);
					x += glyph_width (cr, TimeSig0 + (c - '0'));
				}
			}
		}

		/* bar number */
		cr->select_font_face ("Sans", Cairo::FONT_SLANT_ITALIC, Cairo::FONT_WEIGHT_NORMAL);
		cr->set_font_size (sp * 1.2);
		cr->move_to (margin, top0 - sp * 2.2);
		cr->show_text (string_compose ("%1", _first_bar + (int) sys.first_bar));

		double bx = sys.x0;

		for (size_t bi = 0; bi < sys.n_bars; ++bi) {
			Bar const& bar (_bars[sys.first_bar + bi]);
			double const bw = bar.width * sys.stretch;

			auto onset_x = [&] (int64_t start) {
				size_t k = std::lower_bound (bar.onsets.begin (), bar.onsets.end (), start) - bar.onsets.begin ();
				double const ox = k < bar.onset_x.size () ? bar.onset_x[k] : 0;
				return bx + (sp * 1.4 + ox) * sys.stretch;
			};

			for (int st = 0; st < 2; ++st) {
				double const top    = st == 0 ? top0 : top1;
				int const    ref    = _percussion ? bottom_line (0) : bottom_line (st);
				double const bottom = top + 4 * sp;
				double const middle = top + 2 * sp;
				auto y_of = [&] (int d) { return bottom - (d - ref) * sp / 2.; };

				std::vector<Piece> const& pieces (bar.staff[st]);

				/* beam groups: 8ths/16ths inside one beat */
				int64_t const beat = (_beat_value == 8 && _beats % 3 == 0) ? eighth * 3 : quarter;
				std::vector<int> group (pieces.size (), -1);
				{
					int g = 0;
					size_t k = 0;
					while (k < pieces.size ()) {
						auto beamable = [&] (Piece const& p) {
							int64_t base; bool dotted;
							piece_value (p, base, dotted);
							return !p.pitches.empty () && base <= eighth;
						};
						if (!beamable (pieces[k])) {
							++k;
							continue;
						}
						int64_t const bt = pieces[k].start / beat;
						size_t e = k;
						while (e < pieces.size () && beamable (pieces[e]) && pieces[e].start / beat == bt && pieces[e].start + pieces[e].len <= (bt + 1) * beat) {
							++e;
						}
						if (e - k >= 2) {
							for (size_t m = k; m < e; ++m) {
								group[m] = g;
							}
							++g;
						}
						k = std::max (e, k + 1);
					}
				}

				/* stem direction per group and beam height */
				std::map<int, bool>   group_up;
				std::map<int, double> group_y;
				for (size_t k = 0; k < pieces.size (); ++k) {
					if (group[k] < 0) {
						continue;
					}
					int dmin = 1000, dmax = -1000;
					for (size_t m = 0; m < pieces.size (); ++m) {
						if (group[m] != group[k]) {
							continue;
						}
						for (auto pi : pieces[m].pitches) {
							int d, alt;
							pos (pi, d, alt);
							dmin = std::min (dmin, d);
							dmax = std::max (dmax, d);
						}
					}
					bool const up = _percussion ? (st == 0) : ((dmin + dmax) / 2. < ref + 4);
					group_up[group[k]] = up;
					group_y[group[k]]  = up ? std::min (y_of (dmax) - 3.25 * sp, middle) : std::max (y_of (dmin) + 3.25 * sp, middle);
				}

				std::map<int, int> altered; /* diatonic step -> alteration in this bar */
				std::vector<double> stem_x (pieces.size (), 0);
				std::vector<double> piece_x (pieces.size (), 0);
				std::vector<double> piece_top (pieces.size (), 1e9); /* highest ink, for tuplet numbers */

				for (size_t k = 0; k < pieces.size (); ++k) {
					Piece const& p (pieces[k]);
					double const x = onset_x (p.start);

					int64_t base;
					bool    dotted;
					piece_value (p, base, dotted);

					if (p.pitches.empty ()) {
						/* rest */
						if (_percussion && st == 1) {
							/* the feet voice only shows rests in bars where it plays */
							bool plays = false;
							for (auto const& q : pieces) {
								plays |= !q.pitches.empty ();
							}
							if (!plays) {
								continue;
							}
						}
						/* two voices on one staff: rests move out of the way */
						double const voice_dy = _percussion ? (st == 0 ? -sp : 2 * sp) : 0;
						if (p.len == _bar_len) {
							double const w = glyph_width (cr, RestWhole);
							draw_glyph (cr, RestWhole, bx + (bw - w) / 2, top + sp + (st == 1 ? voice_dy : 0));
							continue;
						}
						uint32_t g = Rest16th;
						if (base >= whole) {
							g = RestWhole;
						} else if (base >= half) {
							g = RestHalf;
						} else if (base >= quarter) {
							g = RestQuarter;
						} else if (base >= eighth) {
							g = Rest8th;
						}
						double const ry = ((g == RestWhole) ? top + sp : middle) + voice_dy;
						draw_glyph (cr, g, x, ry);
						piece_x[k]   = x;
						piece_top[k] = ry - 1.5 * sp;
						if (dotted) {
							draw_glyph (cr, AugDot, x + glyph_width (cr, g) + sp * 0.3, middle - sp / 2 + voice_dy);
						}
						continue;
					}

					/* chord */
					std::vector<std::pair<int, int>> ds; /* diatonic, alteration (drums: pitch) */
					for (auto pi : p.pitches) {
						int d, alt;
						pos (pi, d, alt);
						ds.push_back (std::make_pair (d, _percussion ? (int) pi : alt));
					}
					std::sort (ds.begin (), ds.end ());
					int const dmin = ds.front ().first;
					int const dmax = ds.back ().first;

					bool const beamed = group[k] >= 0;
					bool const up     = beamed ? group_up[group[k]] : (_percussion ? (st == 0) : ((dmin + dmax) / 2. < ref + 4));

					/* seconds: displace every other head to the other side of the stem */
					std::vector<double> head_dx (ds.size (), 0);
					bool any_displaced = false;
					if (up) {
						for (size_t m = 1; m < ds.size (); ++m) {
							if (ds[m].first - ds[m - 1].first == 1 && head_dx[m - 1] == 0) {
								head_dx[m] = headw - stem_w;
								any_displaced = true;
							}
						}
					} else {
						for (size_t m = ds.size () - 1; m-- > 0;) {
							if (ds[m + 1].first - ds[m].first == 1 && head_dx[m + 1] == 0) {
								head_dx[m] = -(headw - stem_w);
								any_displaced = true;
							}
						}
					}

					/* ledger lines */
					cr->set_line_width (line_w);
					double const lx0 = x - sp * 0.4 + (up ? 0 : (any_displaced ? -(headw - stem_w) : 0));
					double const lx1 = x + headw + sp * 0.4 + (up && any_displaced ? headw - stem_w : 0);
					for (int d = ref - 2; d >= dmin; d -= 2) {
						cr->move_to (lx0, y_of (d));
						cr->line_to (lx1, y_of (d));
					}
					for (int d = ref + 10; d <= dmax; d += 2) {
						cr->move_to (lx0, y_of (d));
						cr->line_to (lx1, y_of (d));
					}
					cr->stroke ();

					/* accidentals (not on tied continuation notes) */
					double acc_x = x - sp * 0.3 + (up ? 0 : (any_displaced ? -(headw - stem_w) : 0));
					int last_acc_d = 1000;
					for (size_t m = (_percussion ? 0 : ds.size ()); m-- > 0;) {
						int const d   = ds[m].first;
						int const alt = ds[m].second;
						uint8_t pitch = 0;
						for (auto pi : p.pitches) {
							int dd, aa;
							spell (pi, dd, aa);
							if (dd == d && aa == alt) {
								pitch = pi;
							}
						}
						bool const tied_in = pending_tie[st].count (pitch) > 0;
						int const cur = altered.count (d) ? altered[d] : 0;
						altered[d] = alt;
						if (tied_in || cur == alt) {
							continue;
						}
						uint32_t const g = alt ? AccSharp : AccNatural;
						double const gw  = glyph_width (cr, g);
						if (last_acc_d - d < 6) {
							acc_x -= gw + sp * 0.15;
						}
						draw_glyph (cr, g, acc_x - gw, y_of (d));
						last_acc_d = d;
					}

					/* note heads */
					uint32_t const head = base >= whole ? NoteheadWhole : (base >= half ? NoteheadHalf : NoteheadBlack);
					for (size_t m = 0; m < ds.size (); ++m) {
						uint32_t h = head;
						if (_percussion) {
							int dd;
							drum_position (ds[m].second, dd, h);
							if (h == NoteheadBlack) {
								h = head;
							}
						}
						/* the model note behind this head */
						NotePtr note;
						for (size_t j = 0; j < p.pitches.size () && j < p.notes.size (); ++j) {
							int dd, aa;
							pos (p.pitches[j], dd, aa);
							if (dd == ds[m].first && (_percussion ? (int) p.pitches[j] : aa) == ds[m].second) {
								note = p.notes[j];
							}
						}
						bool const sel = note && _selection.count (note);
						if (sel) {
							cr->set_source_rgb (0.15, 0.45, 0.95);
						}
						draw_glyph (cr, h, x + head_dx[m], y_of (ds[m].first));
						if (sel) {
							cr->set_source_rgb (0.1, 0.1, 0.1);
						}
						if (note) {
							Hit ht;
							ht.x0   = x + head_dx[m] - sp * 0.2;
							ht.x1   = x + head_dx[m] + headw + sp * 0.2;
							ht.y0   = y_of (ds[m].first) - sp * 0.6;
							ht.y1   = y_of (ds[m].first) + sp * 0.6;
							ht.note = note;
							_hits.push_back (ht);
						}
					}

					/* dots */
					if (dotted) {
						double const dx = x + headw + sp * 0.35 + (up && any_displaced ? headw - stem_w : 0);
						for (auto const& dd : ds) {
							bool const on_line = ((dd.first - ref) % 2) == 0;
							draw_glyph (cr, AugDot, dx, y_of (dd.first) - (on_line ? sp / 2 : 0));
						}
					}

					piece_x[k]   = x;
					piece_top[k] = y_of (dmax) - sp * 0.6;

					/* stem and flags */
					if (base < whole) {
						double sx, y0, y1;
						if (up) {
							sx = x + headw - stem_w / 2;
							y0 = y_of (dmin);
							y1 = beamed ? group_y[group[k]] : std::min (y_of (dmax) - 3.5 * sp, middle);
						} else {
							sx = x + stem_w / 2;
							y0 = y_of (dmax);
							y1 = beamed ? group_y[group[k]] : std::max (y_of (dmin) + 3.5 * sp, middle);
						}
						stem_x[k] = sx;
						if (up) {
							piece_top[k] = std::min (piece_top[k], y1);
						}
						cr->set_line_width (stem_w);
						cr->move_to (sx, y0);
						cr->line_to (sx, y1);
						cr->stroke ();

						if (!beamed && base <= eighth) {
							uint32_t const flag = base == eighth ? (up ? Flag8thUp : Flag8thDown) : (up ? Flag16thUp : Flag16thDown);
							draw_glyph (cr, flag, sx - stem_w / 2, y1);
						}
					}

					/* ties */
					for (size_t m = 0; m < p.pitches.size (); ++m) {
						uint8_t const pitch = p.pitches[m];
						int d, alt;
						pos (pitch, d, alt);
						auto pt = pending_tie[st].find (pitch);
						if (pt != pending_tie[st].end ()) {
							if (pt->second.system == si) {
								draw_tie (pt->second.x, x - sp * 0.2, y_of (d), pt->second.below);
							} else {
								draw_tie (sys.x0 - sp, x - sp * 0.2, y_of (d), pt->second.below);
							}
							pending_tie[st].erase (pt);
						}
						if (m < p.tie_out.size () && p.tie_out[m]) {
							Pending pd;
							pd.x      = x + headw + sp * 0.2;
							pd.y      = y_of (d);
							pd.system = si;
							pd.below  = up;
							pending_tie[st][pitch] = pd;
						}
					}
				}

				/* beams */
				for (auto const& gu : group_up) {
					int const g   = gu.first;
					bool const up = gu.second;
					double const by = group_y[g];
					double const th = sp * 0.5;
					std::vector<size_t> members;
					for (size_t k = 0; k < pieces.size (); ++k) {
						if (group[k] == g) {
							members.push_back (k);
						}
					}
					double const x0 = stem_x[members.front ()] - stem_w / 2;
					double const x1 = stem_x[members.back ()] + stem_w / 2;
					double const y0 = up ? by : by - th;
					cr->rectangle (x0, y0, x1 - x0, th);
					cr->fill ();

					/* secondary beams for 16ths */
					double const y2 = up ? by + sp * 0.75 : by - sp * 0.75 - th;
					for (size_t m = 0; m < members.size (); ++m) {
						int64_t base;
						bool dotted;
						piece_value (pieces[members[m]], base, dotted);
						if (base != grid) {
							continue;
						}
						auto is16 = [&] (size_t mm) {
							int64_t b2; bool d2;
							piece_value (pieces[members[mm]], b2, d2);
							return b2 == grid;
						};
						double const sx = stem_x[members[m]];
						if (m + 1 < members.size () && is16 (m + 1)) {
							cr->rectangle (sx - stem_w / 2, y2, stem_x[members[m + 1]] - sx + stem_w, th);
						} else if (!(m > 0 && is16 (m - 1))) {
							double const stub = sp * 1.1;
							if (m > 0) {
								cr->rectangle (sx - stub, y2, stub + stem_w / 2, th);
							} else {
								cr->rectangle (sx - stem_w / 2, y2, stub, th);
							}
						}
						cr->fill ();
					}
				}

				/* triplet numbers (and a bracket unless the beat is one beam) */
				for (size_t k = 0; k < pieces.size ();) {
					if (!pieces[k].tuplet) {
						++k;
						continue;
					}
					int64_t const beat_no = pieces[k].start / quarter;
					size_t e = k;
					double x0 = piece_x[k], x1 = piece_x[k], ytop = top;
					bool one_beam = group[k] >= 0;
					while (e < pieces.size () && pieces[e].tuplet && pieces[e].start / quarter == beat_no) {
						x1   = piece_x[e];
						ytop = std::min (ytop, piece_top[e]);
						one_beam &= (group[e] >= 0 && group[e] == group[k]);
						++e;
					}
					x1 += headw;
					double const ny = ytop - sp * 0.8;
					double const nw = glyph_width (cr, Tuplet3) * 0.7;
					draw_glyph (cr, Tuplet3, (x0 + x1) / 2 - nw / 2, ny, 0.7);
					if (!one_beam) {
						cr->set_line_width (line_w);
						cr->move_to (x0, ny);
						cr->line_to (x0, ny - sp * 0.6);
						cr->line_to ((x0 + x1) / 2 - nw * 0.8, ny - sp * 0.6);
						cr->move_to ((x0 + x1) / 2 + nw * 0.8, ny - sp * 0.6);
						cr->line_to (x1, ny - sp * 0.6);
						cr->line_to (x1, ny);
						cr->stroke ();
					}
					k = e;
				}
			}

			bx += bw;

			/* bar line through both staves */
			cr->set_line_width (line_w * 1.2);
			cr->move_to (bx, top0);
			cr->line_to (bx, top1 + 4 * sp);
			cr->stroke ();
		}

		/* ties running into the next system: draw the first half here */
		for (int st = 0; st < 2; ++st) {
			for (auto const& pt : pending_tie[st]) {
				if (pt.second.system == si) {
					draw_tie (pt.second.x, x_end + sp * 0.5, pt.second.y, pt.second.below);
				}
			}
		}
	}

	/* final bar line */
	if (!_systems.empty ()) {
		System const& sys (_systems.back ());
		double x_end = sys.x0;
		for (size_t b = 0; b < sys.n_bars; ++b) {
			x_end += _bars[sys.first_bar + b].width * sys.stretch;
		}
		double const top0 = staff_y (sys, 0);
		double const top1 = staff_y (sys, 1);
		cr->rectangle (x_end - sp * 0.5, top0, sp * 0.5, top1 + 4 * sp - top0);
		cr->fill ();
		cr->set_line_width (line_w * 1.2);
		cr->move_to (x_end - sp * 1.0, top0);
		cr->line_to (x_end - sp * 1.0, top1 + 4 * sp);
		cr->stroke ();
	}

	/* drag preview: the selected notes at their new place */
	if (_dragging && (_drag_dt || _drag_dp)) {
		cr->set_source_rgba (0.15, 0.45, 0.95, 0.55);
		for (auto const& n : _selection) {
			int const p = n->note () + _drag_dp;
			double gx, gy;
			if (p >= 0 && p <= 127 && position_of (note_tick (n) + _drag_dt, p, gx, gy)) {
				draw_glyph (cr, NoteheadBlack, gx, gy);
			}
		}
	}

	(void) a;
}

/* ---- editing ---- */

int64_t
ScoreView::time_in_bar (Bar const& bar, double x) const
{
	System const& sys (_systems[bar.system]);
	double const ox = (x - bar.x) / sys.stretch - _sp * 1.4;

	/* piecewise linear between the onsets, the last one runs to the bar end */
	std::vector<double>  xs (bar.onset_x);
	std::vector<int64_t> ts (bar.onsets);
	if (xs.empty () || ts.front () != 0) {
		xs.insert (xs.begin (), 0.);
		ts.insert (ts.begin (), 0);
	}
	xs.push_back (bar.width - _sp * 1.8);
	ts.push_back (_bar_len);

	if (ox <= xs.front ()) {
		return 0;
	}
	for (size_t k = 0; k + 1 < xs.size (); ++k) {
		if (ox < xs[k + 1]) {
			double const f = (xs[k + 1] > xs[k]) ? (ox - xs[k]) / (xs[k + 1] - xs[k]) : 0;
			return ts[k] + (int64_t) (f * (ts[k + 1] - ts[k]));
		}
	}
	return _bar_len - grid;
}

bool
ScoreView::pitch_at (double x, double y, size_t& bar_index, uint8_t& pitch) const
{
	if (_systems.empty ()) {
		return false;
	}
	int const si = std::max (0, std::min ((int) _systems.size () - 1, (int) floor ((y - _systems.front ().y) / system_height ())));
	System const& sys (_systems[si]);

	/* which bar */
	bool found = false;
	for (size_t b = sys.first_bar; b < sys.first_bar + sys.n_bars; ++b) {
		if (x >= _bars[b].x && x < _bars[b].x + _bars[b].width * sys.stretch) {
			bar_index = b;
			found     = true;
			break;
		}
	}
	if (!found) {
		return false;
	}

	/* which staff: the nearer middle line */
	int st = 0;
	if (!_percussion) {
		double const m0 = staff_y (sys, 0) + 2 * _sp;
		double const m1 = staff_y (sys, 1) + 2 * _sp;
		st = std::fabs (y - m0) <= std::fabs (y - m1) ? 0 : 1;
	}
	double const top    = staff_y (sys, st);
	double const bottom = top + 4 * _sp;
	if (y < top - 6 * _sp || y > bottom + 6 * _sp) {
		return false;
	}
	int const ref = _percussion ? bottom_line (0) : bottom_line (st);
	int const d   = ref + (int) lrint ((bottom - y) / (_sp / 2));

	if (_percussion) {
		/* the drum on that staff position */
		static const int drum_at[][2] = {
			{ 29, 44 }, { 31, 36 }, { 32, 41 }, { 33, 43 }, { 34, 45 }, { 35, 38 },
			{ 36, 48 }, { 37, 50 }, { 38, 51 }, { 39, 42 }, { 40, 49 },
		};
		int best = 38, dist = 1000;
		for (auto const& da : drum_at) {
			if (std::abs (da[0] - d) < dist) {
				dist = std::abs (da[0] - d);
				best = da[1];
			}
		}
		pitch = best;
		return true;
	}

	static const int semis[7] = { 0, 2, 4, 5, 7, 9, 11 };
	int const octave = (int) floor (d / 7.);
	int const letter = d - octave * 7;
	int const p      = (octave + 1) * 12 + semis[letter];
	if (p < 0 || p > 127) {
		return false;
	}
	pitch = p;
	return true;
}

void
ScoreView::add_note (int64_t tick, uint8_t pitch)
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history) {
		return;
	}
	int64_t len = std::min (_insert_len, _r_end - tick);
	if (len <= 0) {
		return;
	}
	Beats const start = _region->absolute_time_to_source_beats (timepos_t (Beats::ticks (_origin + tick)));

	/* the same note is already there: select it instead of stacking a copy */
	{
		MidiModel::ReadLock lm (model->read_lock ());
		for (auto const& e : model->notes ()) {
			if (e->note () == pitch && e->time () == start) {
				_selection.clear ();
				_selection.insert (e);
				queue_draw ();
				return;
			}
		}
	}

	NotePtr n (new Evoral::Note<Beats> (_channel, start, Beats::ticks (len), pitch, 100));

	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("add note"));
	cmd->add (n);
	model->apply_diff_command_as_commit (*_history, cmd);

	_selection.clear ();
	_selection.insert (n);
	play (n);
	rebuild ();
}

void
ScoreView::delete_selection ()
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || _selection.empty ()) {
		return;
	}
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("delete notes"));
	for (auto const& n : _selection) {
		cmd->remove (n);
	}
	_selection.clear ();
	model->apply_diff_command_as_commit (*_history, cmd);
	rebuild ();
}

void
ScoreView::transpose_selection (int semitones)
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || _selection.empty ()) {
		return;
	}
	for (auto const& n : _selection) {
		int const p = n->note () + semitones;
		if (p < 0 || p > 127) {
			return;
		}
	}
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("transpose"));
	for (auto const& n : _selection) {
		cmd->change (n, MidiModel::NoteDiffCommand::NoteNumber, (uint8_t) (n->note () + semitones));
	}
	model->apply_diff_command_as_commit (*_history, cmd);
	play (*_selection.begin ());
	rebuild ();
}

bool
ScoreView::on_button_press_event (GdkEventButton* ev)
{
	grab_focus ();

	if (ev->button != 1) {
		return false;
	}

	bool const add = ev->state & Gtkmm2ext::Keyboard::TertiaryModifier;

	/* a note head: select it */
	for (auto h = _hits.rbegin (); h != _hits.rend (); ++h) {
		if (ev->x >= h->x0 && ev->x <= h->x1 && ev->y >= h->y0 && ev->y <= h->y1) {
			if (add) {
				if (_selection.count (h->note)) {
					_selection.erase (h->note);
				} else {
					_selection.insert (h->note);
				}
			} else if (!_selection.count (h->note)) {
				_selection.clear ();
				_selection.insert (h->note);
			}
			if (_selection.count (h->note)) {
				/* may become a drag */
				_drag_note = h->note;
				_drag_x0   = ev->x;
				_drag_y0   = ev->y;
				_dragging  = false;
				_drag_dt   = 0;
				_drag_dp   = 0;
				play (h->note);
			}
			queue_draw ();
			return true;
		}
	}

	/* empty place: add a note when a note value is picked */
	size_t  b;
	uint8_t pitch;
	if (_insert_len > 0 && pitch_at (ev->x, ev->y, b, pitch)) {
		int64_t const step = snap_step ();
		int64_t const t    = (time_in_bar (_bars[b], ev->x) / step) * step;
		int64_t const tick = b * _bar_len + t;
		if (tick >= _r_start && tick < _r_end) {
			add_note (tick, pitch);
			return true;
		}
	}

	if (!add && !_selection.empty ()) {
		_selection.clear ();
		queue_draw ();
	}
	return true;
}

bool
ScoreView::handle_key (GdkEventKey* ev)
{
	bool const octave = ev->state & Gtkmm2ext::Keyboard::PrimaryModifier;

	switch (ev->keyval) {
	case GDK_Delete:
	case GDK_BackSpace:
		delete_selection ();
		return true;
	case GDK_Up:
		transpose_selection (octave ? 12 : 1);
		return !_selection.empty ();
	case GDK_Down:
		transpose_selection (octave ? -12 : -1);
		return !_selection.empty ();
	case GDK_Escape:
		_selection.clear ();
		queue_draw ();
		return true;
	default:
		break;
	}
	return false;
}

int64_t
ScoreView::snap_step () const
{
	return _insert_len > 0 ? std::min (_insert_len, quarter) : grid;
}

int64_t
ScoreView::note_tick (NotePtr const& n) const
{
	return _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - _origin;
}

bool
ScoreView::position_of (int64_t tick, uint8_t pitch, double& x, double& y) const
{
	if (tick < 0 || _bars.empty ()) {
		return false;
	}
	size_t const b = tick / _bar_len;
	if (b >= _bars.size ()) {
		return false;
	}
	Bar const&    bar (_bars[b]);
	System const& sys (_systems[bar.system]);
	int64_t const t = tick - b * _bar_len;

	/* inverse of time_in_bar () */
	std::vector<double>  xs (bar.onset_x);
	std::vector<int64_t> ts (bar.onsets);
	if (xs.empty () || ts.front () != 0) {
		xs.insert (xs.begin (), 0.);
		ts.insert (ts.begin (), 0);
	}
	xs.push_back (bar.width - _sp * 1.8);
	ts.push_back (_bar_len);
	double ox = xs.front ();
	for (size_t k = 0; k + 1 < ts.size (); ++k) {
		if (t >= ts[k] && t < ts[k + 1]) {
			ox = xs[k] + (xs[k + 1] - xs[k]) * (double) (t - ts[k]) / (ts[k + 1] - ts[k]);
			break;
		}
	}
	x = bar.x + (_sp * 1.4 + ox) * sys.stretch;

	int d, alt;
	int st;
	if (_percussion) {
		uint32_t head;
		drum_position (pitch, d, head);
		st = 0;
	} else {
		spell (pitch, d, alt);
		st = pitch >= 60 ? 0 : 1;
	}
	int const ref = _percussion ? bottom_line (0) : bottom_line (st);
	y = staff_y (sys, st) + 4 * _sp - (d - ref) * _sp / 2.;
	return true;
}

void
ScoreView::play (NotePtr const& n)
{
	if (!_track || !n || !UIConfiguration::instance ().get_sound_midi_notes ()) {
		return;
	}
	NotePlayer* np = new NotePlayer (_track);
	np->add (n);
	np->play ();
	/* NotePlayer deletes itself */
}

bool
ScoreView::on_motion_notify_event (GdkEventMotion* ev)
{
	if (!_drag_note) {
		return false;
	}
	if (!_dragging && std::fabs (ev->x - _drag_x0) < 4 && std::fabs (ev->y - _drag_y0) < 4) {
		return true;
	}
	_dragging = true;

	size_t  b;
	uint8_t pitch;
	if (pitch_at (ev->x, ev->y, b, pitch)) {
		int64_t const step = snap_step ();
		int64_t const t    = b * _bar_len + (time_in_bar (_bars[b], ev->x) / step) * step;
		int64_t const t0   = note_tick (_drag_note);
		/* keep the grabbed note's offset from the snap grid */
		int64_t const dt   = t - (t0 / step) * step;
		int const     dp   = (int) pitch - (int) _drag_note->note ();
		if (dt != _drag_dt || dp != _drag_dp) {
			_drag_dt = dt;
			_drag_dp = dp;
			queue_draw ();
		}
	}
	return true;
}

bool
ScoreView::on_button_release_event (GdkEventButton* ev)
{
	if (ev->button != 1 || !_drag_note) {
		return false;
	}
	NotePtr const grabbed = _drag_note;
	_drag_note.reset ();

	if (!_dragging || (_drag_dt == 0 && _drag_dp == 0)) {
		_dragging = false;
		return true;
	}
	_dragging = false;

	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history) {
		return true;
	}

	/* all moved notes must stay inside the region and the MIDI range */
	for (auto const& n : _selection) {
		int64_t const t = note_tick (n) + _drag_dt;
		int const     p = n->note () + _drag_dp;
		if (t < _r_start || t >= _r_end || p < 0 || p > 127) {
			queue_draw ();
			return true;
		}
	}

	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("move notes"));
	for (auto const& n : _selection) {
		if (_drag_dt) {
			Beats const start = _region->absolute_time_to_source_beats (timepos_t (Beats::ticks (_origin + note_tick (n) + _drag_dt)));
			cmd->change (n, MidiModel::NoteDiffCommand::StartTime, start);
		}
		if (_drag_dp) {
			cmd->change (n, MidiModel::NoteDiffCommand::NoteNumber, (uint8_t) (n->note () + _drag_dp));
		}
	}
	_drag_dt = 0;
	_drag_dp = 0;
	model->apply_diff_command_as_commit (*_history, cmd);
	play (grabbed);
	rebuild ();
	return true;
}

void
ScoreView::set_selection_length (int64_t len)
{
	std::shared_ptr<MidiModel> model (_region ? _region->model () : std::shared_ptr<MidiModel> ());
	if (!model || !_history || _selection.empty () || len <= 0) {
		return;
	}
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("change note length"));
	for (auto const& n : _selection) {
		int64_t const l = std::min (len, std::max<int64_t> (grid / 4, _r_end - note_tick (n)));
		cmd->change (n, MidiModel::NoteDiffCommand::Length, Beats::ticks (l));
	}
	model->apply_diff_command_as_commit (*_history, cmd);
	rebuild ();
}
