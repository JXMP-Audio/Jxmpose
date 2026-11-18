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
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "pbd/compose.h"

#include "temporal/tempo.h"

#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/midi_track.h"
#include "ardour/scale.h"

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
	Tuplet0        = 0xE880,
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
	ArticAccentAbove   = 0xE4A0,
	ArticAccentBelow   = 0xE4A1,
	ArticStaccatoAbove = 0xE4A2,
	ArticStaccatoBelow = 0xE4A3,
	DynamicPP      = 0xE52B,
	DynamicP       = 0xE520,
	DynamicMP      = 0xE52C,
	DynamicMF      = 0xE52D,
	DynamicF       = 0xE522,
	DynamicFF      = 0xE52F,
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

const int natural_pc[7] = { 0, 2, 4, 5, 7, 9, 11 };

/** alteration of a letter (0 = C .. 6 = B) in a key signature */
int
key_alter (int letter, int fifths)
{
	static const int sharps[7] = { 3, 0, 4, 1, 5, 2, 6 }; /* F C G D A E B */
	static const int flats[7]  = { 6, 2, 5, 1, 4, 0, 3 }; /* B E A D G C F */
	int const n = std::min (7, std::abs (fifths));
	for (int i = 0; i < n; ++i) {
		if ((fifths > 0 ? sharps[i] : flats[i]) == letter) {
			return fifths > 0 ? 1 : -1;
		}
	}
	return 0;
}

/** pitch class of a major key's scale degree set */
bool
in_major (int pc, int fifths)
{
	for (int l = 0; l < 7; ++l) {
		if (((natural_pc[l] + key_alter (l, fifths)) % 12 + 12) % 12 == pc) {
			return true;
		}
	}
	return false;
}

/** spelling in a key: diatonic step (octave * 7 + letter) and alteration.
 * Notes of the key are spelled as in its scale, others as naturals, or
 * with sharps in sharp keys and flats in flat keys. */
void
spell (uint8_t pitch, int& diatonic, int& alter, int fifths = 0)
{
	int const pc = pitch % 12;
	int l = -1, a = 0;
	for (int k = 0; k < 7 && l < 0; ++k) {
		int const ka = key_alter (k, fifths);
		if (((natural_pc[k] + ka) % 12 + 12) % 12 == pc) {
			l = k;
			a = ka;
		}
	}
	for (int k = 0; k < 7 && l < 0; ++k) {
		if (natural_pc[k] == pc) {
			l = k;
		}
	}
	for (int k = 0; k < 7 && l < 0; ++k) {
		if (fifths >= 0 && natural_pc[k] + 1 == pc) {
			l = k;
			a = 1;
		} else if (fifths < 0 && natural_pc[k] - 1 == pc) {
			l = k;
			a = -1;
		}
	}
	int const octave = ((int) pitch - a) / 12 - 1;
	diatonic = octave * 7 + l;
	alter    = a;
}

/** chord templates: intervals above the root and the symbol suffix
 * ('b' and '#' are drawn as accidentals) */
struct ChordTemplate {
	std::vector<int> iv;
	char const*      name;
};

std::vector<ChordTemplate> const&
chord_templates ()
{
	static const std::vector<ChordTemplate> t = {
		{ { 0, 4, 7 }, "" },
		{ { 0, 3, 7 }, "m" },
		{ { 0, 3, 6 }, "dim" },
		{ { 0, 4, 8 }, "+" },
		{ { 0, 5, 7 }, "sus4" },
		{ { 0, 2, 7 }, "sus2" },
		{ { 0, 4, 7, 9 }, "6" },
		{ { 0, 3, 7, 9 }, "m6" },
		{ { 0, 4, 7, 10 }, "7" },
		{ { 0, 4, 7, 11 }, "maj7" },
		{ { 0, 3, 7, 10 }, "m7" },
		{ { 0, 3, 7, 11 }, "m(maj7)" },
		{ { 0, 3, 6, 10 }, "m7b5" },
		{ { 0, 3, 6, 9 }, "dim7" },
		{ { 0, 5, 7, 10 }, "7sus4" },
		{ { 0, 2, 4, 7 }, "add9" },
		{ { 0, 2, 3, 7 }, "m(add9)" },
		{ { 0, 2, 4, 7, 9 }, "6/9" },
		{ { 0, 2, 4, 7, 10 }, "9" },
		{ { 0, 2, 4, 7, 11 }, "maj9" },
		{ { 0, 2, 3, 7, 10 }, "m9" },
		{ { 0, 1, 4, 7, 10 }, "7b9" },
		{ { 0, 3, 4, 7, 10 }, "7#9" },
		{ { 0, 2, 3, 5, 7, 10 }, "m11" },
		{ { 0, 2, 4, 7, 9, 10 }, "13" },
	};
	return t;
}

/** chord root names: letter (0 = C .. 6 = B) and alteration for a pitch class in a key */
void
chord_root_name (int pc, int fifths, int& letter, int& alter)
{
	/* sharps in sharp keys (from D major on), flats in flat keys; in
	 * between the usual names: C# Eb F# Ab Bb */
	static const int sharp_l[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
	static const int sharp_a[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	static const int flat_l[12]  = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };
	static const int flat_a[12]  = { 0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0 };
	static const int mid_l[12]   = { 0, 0, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6 };
	static const int mid_a[12]   = { 0, 1, 0, -1, 0, 0, 1, 0, -1, 0, -1, 0 };
	pc = ((pc % 12) + 12) % 12;
	if (fifths >= 2) {
		letter = sharp_l[pc];
		alter  = sharp_a[pc];
	} else if (fifths <= -2) {
		letter = flat_l[pc];
		alter  = flat_a[pc];
	} else {
		letter = mid_l[pc];
		alter  = mid_a[pc];
	}
}

/** dynamic level (0 pp .. 5 ff) of a velocity */
int
dyn_level (double vel)
{
	static const double upper[5] = { 44, 59, 73, 88, 104 };
	int l = 0;
	while (l < 5 && vel >= upper[l]) {
		++l;
	}
	return l;
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

const int64_t ScoreView::grid;

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
	, _fifths (0)
	, _drum_content (false)
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
ScoreView::set_drum_content (bool yn)
{
	if (_drum_content != yn) {
		_drum_content = yn;
		rebuild ();
	}
}

void
ScoreView::set_track (std::shared_ptr<MidiTrack> t)
{
	_track = t;
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

		int64_t const bt = beat_ticks ();
		auto staff_of = [&] (uint8_t pitch) {
			/* staff, or voice in drum notation (0: hands, 1: feet) */
			return _percussion ? (is_foot (pitch) ? 1 : 0) : (pitch >= 60 ? 0 : 1);
		};

		/* key signature: the major key (relative minor alike) that leaves
		 * the least note time outside its scale; the track's key when the
		 * notes fit it about as well */
		_fifths = 0;
		if (!_percussion && !_drum_content) {
			double w[12] = { 0 };
			double total = 0;
			for (auto const& n : model->notes ()) {
				double const l = std::max<int64_t> (grid, n->length ().to_ticks ());
				w[n->note () % 12] += l;
				total += l;
			}
			auto outside = [&] (int f) {
				double out = 0;
				for (int pc = 0; pc < 12; ++pc) {
					out += in_major (pc, f) ? 0 : w[pc];
				}
				return out;
			};
			double best = 1e18;
			for (int f = -6; f <= 6; ++f) {
				double const out = outside (f) + total * 0.01 * std::abs (f);
				if (out < best) {
					best    = out;
					_fifths = f;
				}
			}
			MusicalKey const* key = _track ? _track->key () : 0;
			for (int f = -7; key && f <= 7; ++f) {
				bool same = true;
				for (int pc = 0; pc < 12; ++pc) {
					same &= key->in_key (60 + pc) == in_major (pc, f);
				}
				if (same) {
					if (outside (f) <= outside (_fifths) + total * 0.05) {
						_fifths = f;
					}
					break;
				}
			}
		}

		/* key changes: a 4-bar window that clearly does not fit the
		 * current key (more than 15% of its note time outside) and fits
		 * another one at least twice as well starts a new key */
		_bar_fifths.assign (n_bars, _fifths);
		if (!_percussion && !_drum_content && n_bars > 2) {
			std::vector<std::array<double, 12>> wb (n_bars);
			for (auto& a : wb) {
				a.fill (0);
			}
			for (auto const& n : model->notes ()) {
				int64_t const s = _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - origin;
				if (s < r_start || s >= r_end) {
					continue;
				}
				size_t const b = std::min<size_t> (n_bars - 1, s / _bar_len);
				wb[b][n->note () % 12] += std::max<int64_t> (grid, n->length ().to_ticks ());
			}
			auto win_out = [&] (int f, size_t b0, double& tot) {
				double out = 0;
				tot        = 0;
				for (size_t b = b0; b < std::min (n_bars, b0 + 4); ++b) {
					for (int pc = 0; pc < 12; ++pc) {
						tot += wb[b][pc];
						out += in_major (pc, f) ? 0 : wb[b][pc];
					}
				}
				return out;
			};
			int cur = _fifths;
			for (size_t b = 0; b < n_bars; ++b) {
				double bar_tot = 0;
				for (int pc = 0; pc < 12; ++pc) {
					bar_tot += wb[b][pc];
				}
				double tot;
				double const oc = win_out (cur, b, tot);
				if (bar_tot > 0 && tot > 0 && oc > 0.15 * tot) {
					int    best   = cur;
					double best_o = 1e18;
					for (int f = -6; f <= 6; ++f) {
						double t2;
						double const o = win_out (f, b, t2) + tot * 0.01 * std::abs (f);
						if (o < best_o) {
							best_o = o;
							best   = f;
						}
					}
					double t3;
					if (best != cur && win_out (best, b, t3) * 2 < oc) {
						cur = best;
					}
				}
				_bar_fifths[b] = cur;
			}
			_fifths = _bar_fifths.front ();
		}

		/* tuplets: per beat, the grid that fits the note starts (and
		 * entered note ends) clearly better than the 16th grid */
		_beat_div[0].clear ();
		_beat_div[1].clear ();
		if (bt == quarter) {
			std::map<int64_t, std::vector<int64_t>> offs[2];
			for (auto const& n : model->notes ()) {
				int64_t const s = _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - origin;
				int64_t const e = _region->source_beats_to_absolute_beats (n->end_time ()).to_ticks () - origin;
				if (e <= r_start || s >= r_end || s < 0) {
					continue;
				}
				int const st = staff_of (n->note ());
				offs[st][s / bt].push_back (s % bt);
				/* ends count when they are exactly on a tuplet grid
				 * (entered tuplets), not when played */
				int64_t const eo = e % bt;
				if (e - s >= grid / 2 && eo % grid && (eo % 320 == 0 || eo % 384 == 0)) {
					offs[st][e / bt].push_back (eo);
				}
			}
			for (int st = 0; st < 2; ++st) {
				std::set<int64_t> beats;
				for (auto const& o : offs[st]) {
					beats.insert (o.first);
				}
				for (int64_t beat : beats) {
					std::vector<int64_t> const& os (offs[st][beat]);
					auto maxdist = [&] (int dv) {
						int64_t const g = quarter / dv;
						int64_t m = 0;
						for (int64_t o : os) {
							int64_t const q = ((o + g / 2) / g) * g;
							m = std::max (m, std::abs (o - q));
						}
						return m;
					};
					int64_t const m4 = maxdist (4);
					int dv = 4;
					int64_t md = m4;
					for (int cand : { 3, 6, 5 }) {
						int64_t const m = maxdist (cand);
						if (m * 2 < m4 && m < md) {
							dv = cand;
							md = m;
						}
					}
					if (dv != 4) {
						_beat_div[st][beat] = dv;
					}
				}
			}
		}

		auto quantize = [&] (int staff, int64_t t) {
			int64_t const b   = t / bt;
			int64_t const g   = quarter / beat_div (staff, t);
			int64_t const off = t - b * bt;
			return b * bt + ((off + g / 2) / g) * g;
		};

		std::vector<std::pair<std::pair<int64_t, int64_t>, NotePtr>> qnotes; /* quantized start, end */

		std::vector<int64_t> next_on[2]; /* quantized note starts per staff */
		for (auto const& n : model->notes ()) {
			int64_t const s = _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - origin;
			if (s >= r_start && s < r_end) {
				int const st = staff_of (n->note ());
				next_on[st].push_back (quantize (st, s));
			}
		}
		for (auto& v : next_on) {
			std::sort (v.begin (), v.end ());
			v.erase (std::unique (v.begin (), v.end ()), v.end ());
		}

		for (auto const& n : model->notes ()) {
			int64_t s = _region->source_beats_to_absolute_beats (n->time ()).to_ticks () - origin;
			int64_t e = _region->source_beats_to_absolute_beats (n->end_time ()).to_ticks () - origin;
			if (e <= r_start || s >= r_end) {
				continue;
			}
			s = std::max (s, r_start);
			e = std::min (e, r_end);
			qnotes.push_back (std::make_pair (std::make_pair (quantize (staff_of (n->note ()), s), quantize (staff_of (n->note ()), e)), n));
			bool const hit = (e - s) < grid / 2; /* drum hits, very short notes */

			int const staff = staff_of (n->note ());

			/* quantize to the 16th (or triplet) grid */
			s = quantize (staff, s);
			e = quantize (staff, e);
			if (e <= s) {
				e = s + quarter / beat_div (staff, s);
			}

			/* detached notes are written up to the next note on their
			 * staff when that is near (staccato instead of rests) */
			if (!_percussion && !_drum_content) {
				std::vector<int64_t> const& ns (next_on[staff]);
				auto const it = std::upper_bound (ns.begin (), ns.end (), s);
				int64_t const next = it == ns.end () ? -1 : *it;
				if (next > e && next - s <= quarter && (next / _bar_len) == (s / _bar_len)) {
					e = next;
				} else if ((next < 0 || (next / _bar_len) != (s / _bar_len)) && e - s < eighth && s % eighth == 0 && beat_div (staff, s) == 4) {
					/* the last one in the bar: an eighth */
					e = std::min (s + eighth, (s / _bar_len + 1) * _bar_len);
				}
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

		analyze_harmony_and_dynamics (qnotes, n_bars);
	}

	/* two voices per staff: notes that overlap others (by more than a
	 * 16th) and are not part of their chord go to the second voice; the
	 * higher voice is the first one */
	auto split_voices = [&] (std::vector<Seg>& in, std::vector<Seg>& v2) {
		std::sort (in.begin (), in.end (), [] (Seg const& a, Seg const& b) {
			return a.start < b.start || (a.start == b.start && a.pitch > b.pitch);
		});
		std::vector<Seg> v[2];
		int64_t vend[2] = { 0, 0 }, cs[2] = { -1, -1 }, ce[2] = { -1, -1 };
		auto joins = [&] (Seg const& s, int k) {
			return s.start == cs[k] && std::abs (s.end - ce[k]) <= std::max (grid, (ce[k] - cs[k]) / 4);
		};
		int last[2] = { -1, -1 }; /* last (lowest) pitch per voice */
		for (auto const& s : in) {
			int k = -1;
			/* a free second voice keeps its line: a note below the first
			 * voice's chord that is nearer to the second voice's last note */
			if (!v[1].empty () && s.start >= vend[1] - grid && last[0] >= 0 && s.pitch < last[0]
			    && std::abs ((int) s.pitch - last[1]) < std::abs ((int) s.pitch - last[0])) {
				k     = 1;
				cs[1] = s.start;
				ce[1] = s.end;
				vend[1] = s.end;
			}
			for (int c = 0; c < 2 && k < 0; ++c) {
				if (joins (s, c)) {
					k = c;
				} else if (s.start >= vend[c] - grid) {
					k      = c;
					cs[c]  = s.start;
					ce[c]  = s.end;
					vend[c] = s.end;
				}
			}
			if (k < 0) {
				k = 0;
			}
			vend[k] = std::max (vend[k], s.end);
			last[k] = s.pitch;
			v[k].push_back (s);
		}
		if (!v[1].empty ()) {
			double m0 = 0, m1 = 0;
			for (auto const& s : v[0]) { m0 += s.pitch; }
			for (auto const& s : v[1]) { m1 += s.pitch; }
			if (m1 / v[1].size () > m0 / std::max<size_t> (1, v[0].size ())) {
				std::swap (v[0], v[1]);
			}
		}
		in.swap (v[0]);
		v2.swap (v[1]);
	};

	_bars.resize (n_bars);
	for (size_t b = 0; b < n_bars; ++b) {
		Bar& bar (_bars[b]);
		for (int st = 0; st < 2; ++st) {
			std::vector<Seg> v2;
			if (!_percussion) {
				split_voices (segs[st][b], v2);
			}
			build_bar_pieces (bar.staff[st], b, st, segs[st][b]);
			bar.voice2[st].clear ();
			if (!v2.empty ()) {
				build_bar_pieces (bar.voice2[st], b, st, v2);
			}
		}

		std::set<int64_t> on;
		for (int st = 0; st < 2; ++st) {
			for (auto const& p : bar.staff[st]) {
				on.insert (p.start);
			}
			for (auto const& p : bar.voice2[st]) {
				on.insert (p.start);
			}
		}
		bar.onsets.assign (on.begin (), on.end ());
		bar.onset_x.clear ();

		/* a key change takes room at the bar start */
		bar.key_x = (b > 0 && fifths_at (b) != fifths_at (b - 1)) ? key_change_width (fifths_at (b - 1), fifths_at (b)) : 0;

		/* spacing: grows with the square root of the duration */
		double x = bar.key_x;
		for (size_t k = 0; k < bar.onsets.size (); ++k) {
			int64_t const next = (k + 1 < bar.onsets.size ()) ? bar.onsets[k + 1] : _bar_len;
			int64_t const gap  = next - bar.onsets[k];
			bool accidental = false;
			for (int li = 0; li < 4 && !_percussion; ++li) {
				for (auto const& p : (li < 2 ? bar.staff[li] : bar.voice2[li - 2])) {
					if (p.start != bar.onsets[k]) {
						continue;
					}
					for (auto pi : p.pitches) {
						accidental |= !in_major (pi % 12, fifths_at (b));
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
ScoreView::analyze_harmony_and_dynamics (std::vector<std::pair<std::pair<int64_t, int64_t>, NotePtr>> const& notes, size_t n_bars)
{
	_chords.clear ();
	_dyns.clear ();
	_hairpins.clear ();
	_accents.clear ();

	if (notes.empty ()) {
		return;
	}

	/* chord symbols: per beat, the template that explains the most note
	 * time (notes struck in the beat count more); a beat keeps the chord
	 * before it unless the new chord fits clearly better (passing notes) */
	if (!_percussion && !_drum_content) {
		int64_t const bt     = beat_ticks ();
		int64_t const n_beat = (int64_t) n_bars * _bar_len / bt;
		auto const&   tpl    = chord_templates ();

		struct Cand { int root, q, bass; double score; };
		auto score = [&] (double const* w, int bass_pc, int root, int q) {
			std::vector<int> const& iv (tpl[q].iv);
			bool in[12] = { false };
			for (int i : iv) {
				in[(root + i) % 12] = true;
			}
			double s = 0;
			for (int pc = 0; pc < 12; ++pc) {
				s += in[pc] ? w[pc] : -w[pc];
			}
			for (int i : iv) {
				if (w[(root + i) % 12] < 0.03) {
					/* missing tones: the fifth may go, the root when it is not the bass, extensions may not */
					s -= (i == 7) ? 0.02 : (i == 0 ? 0.05 : ((i == 3 || i == 4) ? 0.12 : 0.1));
				}
			}
			s += (bass_pc == root) ? 0.08 : 0;
			s -= 0.012 * iv.size ();
			return s;
		};

		ChordSym cur;
		cur.tick    = -1;
		cur.quality = -1;
		for (int64_t b = 0; b < n_beat; ++b) {
			int64_t const b0 = b * bt, b1 = b0 + bt;
			if (b1 <= _r_start || b0 >= _r_end) {
				continue;
			}
			double w[12] = { 0 };
			int    low = 128;
			for (auto const& qn : notes) {
				int64_t const s = qn.first.first, e = qn.first.second;
				int64_t const ov = std::min (e, b1) - std::max (s, b0);
				if (ov <= 0) {
					continue;
				}
				int const p = qn.second->note ();
				w[p % 12] += ov + ((s >= b0) ? bt / 2 : 0);
				if (ov >= bt / 4 || s >= b0) {
					low = std::min (low, p);
				}
			}
			double tot = 0;
			for (double v : w) {
				tot += v;
			}
			if (tot < bt * 0.75) {
				/* (nearly) nothing sounds: the previous chord goes on */
				continue;
			}
			for (double& v : w) {
				v /= tot;
			}
			int const bass_pc = low < 128 ? low % 12 : -1;

			Cand best = { 0, 0, -1, -1e9 };
			for (int r = 0; r < 12; ++r) {
				for (size_t q = 0; q < tpl.size (); ++q) {
					double const s = score (w, bass_pc, r, q);
					if (s > best.score) {
						best = { r, (int) q, -1, s };
					}
				}
			}
			if (best.score < 0.2) {
				continue;
			}
			best.bass = (bass_pc >= 0 && bass_pc != best.root) ? bass_pc : -1;
			if (cur.quality >= 0) {
				if (best.root == cur.root && best.q == cur.quality && best.bass == cur.bass) {
					continue;
				}
				double const keep = score (w, bass_pc, cur.root, cur.quality);
				if (keep >= best.score - 0.15 && (b % (_bar_len / bt)) != 0) {
					/* passing notes inside the bar */
					continue;
				}
				if (keep >= best.score - 0.04) {
					continue;
				}
			}
			cur.tick    = b0;
			cur.root    = best.root;
			cur.quality = best.q;
			cur.bass    = best.bass;
			_chords.push_back (cur);
		}
	}

	/* dynamics: onsets (both staves) with their mean velocity, smoothed
	 * over the neighbours; marks where the level changes for good,
	 * hairpins over steady rises and falls, accents on notes clearly
	 * louder than their surroundings */
	std::map<int64_t, std::vector<NotePtr>> by_onset;
	for (auto const& qn : notes) {
		by_onset[qn.first.first].push_back (qn.second);
	}
	/* accents: notes clearly louder than the notes of the same line (the
	 * nearest pitch on their staff) before and after them */
	{
		std::vector<std::vector<NotePtr>> groups[2];
		for (auto const& o : by_onset) {
			std::vector<NotePtr> g[2];
			for (auto const& nt : o.second) {
				int const st = _percussion ? (is_foot (nt->note ()) ? 1 : 0) : (nt->note () >= 60 ? 0 : 1);
				g[st].push_back (nt);
			}
			for (int st = 0; st < 2; ++st) {
				if (!g[st].empty ()) {
					groups[st].push_back (g[st]);
				}
			}
		}
		for (int st = 0; st < 2; ++st) {
			std::vector<std::vector<NotePtr>> const& gs (groups[st]);
			auto nearest = [&] (size_t gi, int pitch, double& vel) {
				int best = 128;
				for (auto const& nt : gs[gi]) {
					int const d = std::abs ((int) nt->note () - pitch);
					if (d < best && d <= 12) {
						best = d;
						vel  = nt->velocity ();
					}
				}
				return best < 128;
			};
			for (size_t i = 0; i < gs.size (); ++i) {
				for (auto const& nt : gs[i]) {
					double sum = 0, v = 0;
					int    cnt = 0;
					if (i > 0 && nearest (i - 1, nt->note (), v)) {
						sum += v;
						++cnt;
					}
					if (i + 1 < gs.size () && nearest (i + 1, nt->note (), v)) {
						sum += v;
						++cnt;
					}
					if (cnt && nt->velocity () >= sum / cnt + ((_percussion || _drum_content) ? 22 : 18) && nt->velocity () >= 70) {
						_accents.insert (nt);
					}
				}
			}
		}
	}

	if (_percussion || _drum_content) {
		return;
	}

	/* the level of an onset leaves its accents out */
	std::vector<int64_t> ticks;
	std::vector<double>  mean;
	for (auto const& o : by_onset) {
		double v = 0;
		int    c = 0;
		for (auto const& nt : o.second) {
			if (!_accents.count (nt)) {
				v += nt->velocity ();
				++c;
			}
		}
		if (c) {
			ticks.push_back (o.first);
			mean.push_back (v / c);
		}
	}
	size_t const n = ticks.size ();
	std::vector<double> sm (n);
	for (size_t i = 0; i < n; ++i) {
		double a[3] = { mean[i > 0 ? i - 1 : i], mean[i], mean[i + 1 < n ? i + 1 : i] };
		std::sort (a, a + 3);
		sm[i] = a[1];
	}

	/* hairpins: at least 4 onsets over a beat or more that keep rising (falling) by 18 or more */
	std::vector<bool> in_pin (n, false);
	for (size_t i = 0; i + 3 < n;) {
		size_t j = i;
		int    dir = 0;
		while (j + 1 < n) {
			double const d = sm[j + 1] - sm[j];
			int const    sd = d > 2 ? 1 : (d < -2 ? -1 : 0);
			if (dir == 0 && sd != 0) {
				dir = sd;
			}
			if ((dir > 0 && d < -2) || (dir < 0 && d > 2)) {
				break;
			}
			++j;
		}
		/* flat ends are not part of it */
		size_t a = i;
		while (a < j && std::abs (sm[a + 1] - sm[a]) <= 2) {
			++a;
		}
		size_t z = j;
		while (z > a && std::abs (sm[z] - sm[z - 1]) <= 2) {
			--z;
		}
		int steps = 0;
		for (size_t k = a; k < z; ++k) {
			steps += std::abs (sm[k + 1] - sm[k]) > 2;
		}
		if (dir != 0 && steps >= 3 && std::abs (sm[z] - sm[a]) >= 18 && ticks[z] - ticks[a] >= quarter) {
			i = a;
			j = z;
			Hairpin hp;
			hp.start = ticks[i];
			hp.end   = ticks[j];
			hp.cresc = dir > 0;
			_hairpins.push_back (hp);
			for (size_t k = i + 1; k < j; ++k) {
				in_pin[k] = true;
			}
			i = j;
		} else {
			++i;
		}
	}

	/* marks with hysteresis: a level change needs the smoothed velocity a
	 * few steps past the boundary */
	static const double upper[5] = { 44, 59, 73, 88, 104 };
	int cur = -1;
	for (size_t i = 0; i < n; ++i) {
		if (in_pin[i]) {
			continue;
		}
		int const l = dyn_level (sm[i]);
		if (cur >= 0) {
			double const lo = cur > 0 ? upper[cur - 1] - 5 : -1e9;
			double const hi = cur < 5 ? upper[cur] + 5 : 1e9;
			if (sm[i] >= lo && sm[i] < hi) {
				continue;
			}
		}
		if (cur >= 0 && i + 1 < n && !in_pin[i + 1] && dyn_level (sm[i + 1]) == cur) {
			/* only one onset: not a new level */
			continue;
		}
		if (l != cur) {
			DynMark m;
			m.tick  = ticks[i];
			m.level = l;
			_dyns.push_back (m);
			cur = l;
		}
	}
}

void
ScoreView::draw_chord_symbol (Cairo::RefPtr<Cairo::Context> cr, ChordSym const& c, double x, double y, int fifths)
{
	double const fs = _sp * 1.7;
	auto text = [&] (std::string const& s, double size, double dy) {
		cr->select_font_face ("Barlow", Cairo::FONT_SLANT_NORMAL, Cairo::FONT_WEIGHT_BOLD);
		cr->set_font_size (size);
		cr->move_to (x, y + dy);
		cr->show_text (s);
		Cairo::TextExtents te;
		cr->get_text_extents (s, te);
		x += te.x_advance;
	};
	auto acc = [&] (int alter, double size) {
		if (!alter) {
			return;
		}
		uint32_t const g = alter > 0 ? AccSharp : AccFlat;
		double const   sc = size / (_sp * 4) * 1.0;
		draw_glyph (cr, g, x + _sp * 0.08, y - size * 0.36, sc);
		x += glyph_width (cr, g) * sc + _sp * 0.15;
	};
	auto note_name = [&] (int pc, double size) {
		int l, a;
		chord_root_name (pc, fifths, l, a);
		text (std::string (1, "CDEFGAB"[l]), size, 0);
		acc (a, size);
	};

	note_name (c.root, fs);
	/* the suffix is smaller and raised; b/# before a number are accidentals */
	std::string const suffix (chord_templates ()[c.quality].name);
	double const ss = fs * 0.72;
	std::string run;
	for (size_t i = 0; i < suffix.size (); ++i) {
		char const ch = suffix[i];
		if ((ch == 'b' || ch == '#') && i + 1 < suffix.size () && isdigit (suffix[i + 1])) {
			text (run, ss, -fs * 0.28);
			run.clear ();
			acc (ch == '#' ? 1 : -1, ss);
		} else {
			run += ch;
		}
	}
	text (run, ss, -fs * 0.28);
	if (c.bass >= 0) {
		text ("/", fs, 0);
		note_name (c.bass, fs);
	}
}

void
ScoreView::build_bar_pieces (std::vector<Piece>& out, size_t bar_index, int staff, std::vector<Seg> const& in)
{
	std::vector<Seg> segs (in);
	std::sort (segs.begin (), segs.end (), [] (Seg const& a, Seg const& b) {
		return a.start < b.start || (a.start == b.start && a.pitch < b.pitch);
	});

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

		/* readability: a hit (shorter than a 32nd) followed by a rest in
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

int
ScoreView::beat_div (int staff, int64_t abs_tick) const
{
	if (beat_ticks () != quarter) {
		return 4;
	}
	auto i = _beat_div[staff].find (abs_tick / quarter);
	return i == _beat_div[staff].end () ? 4 : i->second;
}

std::vector<std::pair<std::pair<int64_t, int64_t>, int>>
ScoreView::split_piece (size_t bar_index, int staff, int64_t start, int64_t len, bool dots) const
{
	std::vector<std::pair<std::pair<int64_t, int64_t>, int>> r;
	int64_t const bar0 = bar_index * _bar_len;

	while (len > 0) {
		int64_t const abs = bar0 + start;
		int const     dv  = beat_div (staff, abs);
		if (dv != 4) {
			/* inside a tuplet beat: multiples of the tuplet unit */
			int64_t const u        = quarter / dv;
			int64_t const beat_end = (abs / quarter + 1) * quarter - bar0;
			int64_t const chunk    = std::min (len, beat_end - start);
			int64_t const off      = start % quarter;
			int64_t v;
			if (off == 0 && chunk >= quarter) {
				v = quarter;
				r.push_back (std::make_pair (std::make_pair (start, v), 0));
			} else {
				int64_t const units = chunk / u;
				int64_t n = 1;
				/* rests are written without dots */
				for (int64_t c : (dv == 3 ? std::vector<int64_t> { 2, 1 } : (dots ? std::vector<int64_t> { 4, 3, 2, 1 } : std::vector<int64_t> { 2, 1 }))) {
					if (c <= units) {
						n = c;
						break;
					}
				}
				v = units ? n * u : chunk;
				r.push_back (std::make_pair (std::make_pair (start, v), dv));
			}
			start += v;
			len   -= v;
			continue;
		}

		/* plain values, up to the next tuplet beat */
		int64_t limit = len;
		for (int64_t t = (abs / quarter + 1) * quarter; t < bar0 + start + len; t += quarter) {
			if (beat_div (staff, t) != 4) {
				limit = t - abs;
				break;
			}
		}
		for (auto const& v : split_duration (start, limit, _bar_len, dots)) {
			r.push_back (std::make_pair (v, 0));
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
		/* written value: units of an eighth (triplets) or a 16th */
		int64_t const u    = quarter / p.tuplet;
		int64_t const unit = p.tuplet == 3 ? eighth : grid;
		note_value (std::max<int64_t> (1, p.len / u) * unit, base, dotted);
		return;
	}
	note_value (p.len, base, dotted);
}

int
ScoreView::fifths_at (size_t bar) const
{
	return bar < _bar_fifths.size () ? _bar_fifths[bar] : _fifths;
}

double
ScoreView::key_width (int fifths) const
{
	return std::abs (fifths) * _sp;
}

double
ScoreView::key_change_width (int from, int to) const
{
	int const cancel = ((from > 0) != (to > 0) || to == 0) ? std::abs (from) : std::max (0, std::abs (from) - std::abs (to));
	return (cancel + std::abs (to)) * _sp + _sp * 1.2;
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
		s.x0        = margin + _sp * 4.5 + key_width (fifths_at (b)) + (_systems.empty () ? _sp * 3 : 0);

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

	int cur_fifths = _fifths; /* key of the bar being drawn */

	/* staff position: pitch spelling, or the drum map */
	auto pos = [&] (uint8_t pitch, int& d, int& alt) {
		if (_percussion) {
			uint32_t head;
			drum_position (pitch, d, head);
			alt = 0;
		} else {
			spell (pitch, d, alt, cur_fifths);
		}
	};

	/* key signature (with naturals cancelling the previous one) at x on both staves */
	auto draw_key = [&] (double x, double top0, double top1, int from, int to) {
		static const int sharp_d[7] = { 38, 35, 39, 36, 33, 37, 34 };
		static const int flat_d[7]  = { 34, 37, 33, 36, 32, 35, 31 };
		int const cancel = ((from > 0) != (to > 0) || to == 0) ? std::abs (from) : std::max (0, std::abs (from) - std::abs (to));
		int const keep   = std::abs (from) - cancel; /* the cancelled ones are the last of the old key */
		for (int st = 0; st < 2; ++st) {
			double const bottom = (st == 0 ? top0 : top1) + 4 * sp;
			int const    ref    = bottom_line (st);
			double       xx     = x;
			for (int i = keep; i < keep + cancel; ++i) {
				int const d = (from > 0 ? sharp_d[i] : flat_d[i]) - (st == 0 ? 0 : 14);
				draw_glyph (cr, AccNatural, xx, bottom - (d - ref) * sp / 2.);
				xx += sp;
			}
			for (int i = 0; i < std::abs (to); ++i) {
				int const d = (to > 0 ? sharp_d[i] : flat_d[i]) - (st == 0 ? 0 : 14);
				draw_glyph (cr, to > 0 ? AccSharp : AccFlat, xx, bottom - (d - ref) * sp / 2.);
				xx += sp;
			}
		}
	};

	struct Pending { double x, y; size_t system; bool below; };
	std::map<uint8_t, Pending> pending_tie[2]; /* per staff */

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

		/* key signature of the system's first bar */
		int const sys_key = fifths_at (sys.first_bar);
		if (!_percussion && sys_key) {
			draw_key (margin + sp * 3.8, top0, top1, 0, sys_key);
		}

		/* time signature on the first system */
		if (si == 0) {
			for (int st = 0; st < (_percussion ? 1 : 2); ++st) {
				double const top = st == 0 ? top0 : top1;
				std::string const n = string_compose ("%1", _beats);
				std::string const d = string_compose ("%1", _beat_value);
				double x = margin + sp * 4.6 + key_width (sys_key);
				for (char c : n) {
					draw_glyph (cr, TimeSig0 + (c - '0'), x, top + sp);
					x += glyph_width (cr, TimeSig0 + (c - '0'));
				}
				x = margin + sp * 4.6 + key_width (sys_key);
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

			cur_fifths = fifths_at (sys.first_bar + bi);
			if (bar.key_x > 0 && !_percussion) {
				/* key change: after a thin double bar line (the system start already shows the key) */
				if (bi > 0) {
					cr->set_line_width (line_w * 1.2);
					cr->move_to (bx - sp * 0.4, top0);
					cr->line_to (bx - sp * 0.4, top1 + 4 * sp);
					cr->stroke ();
					draw_key (bx + sp * 0.6, top0, top1, fifths_at (sys.first_bar + bi - 1), cur_fifths);
				}
			}

			auto onset_x = [&] (int64_t start) {
				size_t k = std::lower_bound (bar.onsets.begin (), bar.onsets.end (), start) - bar.onsets.begin ();
				double const ox = k < bar.onset_x.size () ? bar.onset_x[k] : 0;
				return bx + (sp * 1.4 + ox) * sys.stretch;
			};

			std::map<int, int> altered_bar[2]; /* per staff: diatonic step -> alteration in this bar */

			/* layers: staff 0, staff 1, then their second voices */
			for (int li = 0; li < 4; ++li) {
				int const  st  = li % 2;
				bool const v2  = li >= 2;
				bool const two = !_percussion && !bar.voice2[st].empty ();
				if (v2 && !two) {
					continue;
				}
				double const top    = st == 0 ? top0 : top1;
				int const    ref    = _percussion ? bottom_line (0) : bottom_line (st);
				double const bottom = top + 4 * sp;
				double const middle = top + 2 * sp;
				auto y_of = [&] (int d) { return bottom - (d - ref) * sp / 2.; };

				std::vector<Piece> const& pieces (v2 ? bar.voice2[st] : bar.staff[st]);

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
					bool const up = _percussion ? (st == 0) : (two ? !v2 : ((dmin + dmax) / 2. < ref + 4));
					group_up[group[k]] = up;
					group_y[group[k]]  = up ? std::min (y_of (dmax) - 3.25 * sp, middle) : std::max (y_of (dmin) + 3.25 * sp, middle);
				}

				std::map<int, int>& altered (altered_bar[st]);
				std::vector<double> stem_x (pieces.size (), 0);
				std::vector<double> piece_x (pieces.size (), 0);
				std::vector<double> piece_top (pieces.size (), 1e9); /* highest ink, for tuplet numbers */
				std::vector<std::pair<size_t, double>> accent_above, accent_below; /* piece, x */
				std::vector<double> piece_bottom (pieces.size (), -1e9);

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
						double const voice_dy = _percussion ? (st == 0 ? -sp : 2 * sp) : (two ? (v2 ? 2 * sp : -sp) : 0);
						if (p.len == _bar_len) {
							double const w = glyph_width (cr, RestWhole);
							draw_glyph (cr, RestWhole, bx + (bw - w) / 2, top + sp + ((_percussion && st == 0) ? 0 : voice_dy));
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
					bool const up     = beamed ? group_up[group[k]] : (_percussion ? (st == 0) : (two ? !v2 : ((dmin + dmax) / 2. < ref + 4)));

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
							spell (pi, dd, aa, cur_fifths);
							if (dd == d && aa == alt) {
								pitch = pi;
							}
						}
						bool const tied_in = pending_tie[st].count (pitch) > 0;
						int const cur = altered.count (d) ? altered[d] : key_alter (((d % 7) + 7) % 7, cur_fifths);
						altered[d] = alt;
						if (tied_in || cur == alt) {
							continue;
						}
						uint32_t const g = alt > 0 ? AccSharp : (alt < 0 ? AccFlat : AccNatural);
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

					/* articulations, opposite the stem: staccato for notes
					 * played much shorter than written, accents */
					{
						bool tied_in = true;
						bool tie_out = false;
						bool accent  = false;
						int64_t played = 0;
						for (size_t m = 0; m < p.pitches.size (); ++m) {
							tied_in &= pending_tie[st].count (p.pitches[m]) > 0;
							tie_out |= m < p.tie_out.size () && p.tie_out[m];
						}
						for (auto const& nt : p.notes) {
							if (nt) {
								accent |= _accents.count (nt) > 0;
								played = std::max<int64_t> (played, nt->length ().to_ticks ());
							}
						}
						bool const staccato = !_percussion && !tied_in && !tie_out && p.len <= quarter && played > 0 && played * 2 <= p.len;
						if (!tied_in && accent && _percussion) {
							/* drums: outside the stems and beams */
							(up ? accent_above : accent_below).push_back (std::make_pair (k, x + headw / 2));
							accent = false;
						}
						if (!tied_in && (staccato || accent)) {
							double const cx = x + headw / 2 + (any_displaced ? (up ? (headw - stem_w) / 2 : -(headw - stem_w) / 2) : 0);
							double       ay = up ? y_of (dmin) + sp * 1.1 : y_of (dmax) - sp * 1.1;
							if (staccato) {
								uint32_t const g = up ? ArticStaccatoBelow : ArticStaccatoAbove;
								draw_glyph (cr, g, cx - glyph_width (cr, g) / 2, ay);
								ay += up ? sp * 0.9 : -sp * 0.9;
							}
							if (accent) {
								uint32_t const g = up ? ArticAccentBelow : ArticAccentAbove;
								draw_glyph (cr, g, cx - glyph_width (cr, g) / 2, ay + (up ? sp * 0.3 : 0));
							}
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
						} else {
							piece_bottom[k] = y1;
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

				for (auto const& aa : accent_above) {
					double const w = glyph_width (cr, ArticAccentAbove);
					draw_glyph (cr, ArticAccentAbove, aa.second - w / 2, std::min (piece_top[aa.first], top) - sp * 0.8);
				}
				for (auto const& aa : accent_below) {
					double const w = glyph_width (cr, ArticAccentBelow);
					draw_glyph (cr, ArticAccentBelow, aa.second - w / 2, std::max (piece_bottom[aa.first], bottom) + sp * 1.3);
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
					while (e < pieces.size () && pieces[e].tuplet == pieces[k].tuplet && pieces[e].start / quarter == beat_no) {
						x1   = piece_x[e];
						ytop = std::min (ytop, piece_top[e]);
						one_beam &= (group[e] >= 0 && group[e] == group[k]);
						++e;
					}
					x1 += headw;
					double const ny = ytop - sp * 0.8;
					uint32_t const num = Tuplet0 + pieces[k].tuplet;
					double const   nw  = glyph_width (cr, num) * 0.7;
					draw_glyph (cr, num, (x0 + x1) / 2 - nw / 2, ny, 0.7);
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

			/* x of any tick in the bar: between the onsets */
			{
				int64_t const bar0 = (int64_t) (sys.first_bar + bi) * _bar_len;
				std::vector<std::pair<int64_t, double>> pts;
				if (bar.onsets.empty () || bar.onsets.front () > 0) {
					pts.push_back (std::make_pair ((int64_t) 0, bx + (sp * 1.4 + bar.key_x) * sys.stretch));
				}
				for (auto t : bar.onsets) {
					pts.push_back (std::make_pair (t, onset_x (t)));
				}
				pts.push_back (std::make_pair (_bar_len, bx + bw));
				auto tick_x = [&] (int64_t t) {
					for (size_t k = 0; k + 1 < pts.size (); ++k) {
						if (t >= pts[k].first && t <= pts[k + 1].first) {
							double const f = (double) (t - pts[k].first) / std::max<int64_t> (1, pts[k + 1].first - pts[k].first);
							return pts[k].second + f * (pts[k + 1].second - pts[k].second);
						}
					}
					return pts.back ().second;
				};

				/* chord symbols above the treble staff */
				for (auto const& ch : _chords) {
					if (ch.tick >= bar0 && ch.tick < bar0 + _bar_len) {
						draw_chord_symbol (cr, ch, tick_x (ch.tick - bar0), top0 - sp * 2.6, cur_fifths);
					}
				}

				/* dynamics and hairpins between the staves (below the drum staff) */
				double const dyn_y = top0 + sp * 8.1;
				for (auto const& d : _dyns) {
					if (d.tick >= bar0 && d.tick < bar0 + _bar_len) {
						static const uint32_t glyph[6] = { DynamicPP, DynamicP, DynamicMP, DynamicMF, DynamicF, DynamicFF };
						draw_glyph (cr, glyph[d.level], tick_x (d.tick - bar0) - sp * 0.4, dyn_y);
					}
				}
				for (auto const& hp : _hairpins) {
					int64_t const s = std::max (hp.start, bar0);
					int64_t const e = std::min (hp.end, bar0 + _bar_len);
					if (e <= s) {
						continue;
					}
					double const len  = std::max<int64_t> (1, hp.end - hp.start);
					double const open = sp * 0.75;
					auto half = [&] (int64_t t) {
						double const f = (t - hp.start) / len;
						return open * (hp.cresc ? f : 1 - f);
					};
					double const x0 = tick_x (s - bar0) + (s == hp.start ? sp * 1.8 : 0);
					double const x1 = tick_x (e - bar0) - (e == hp.end ? sp * 0.9 : 0);
					if (x1 - x0 < sp) {
						continue;
					}
					double const yc = dyn_y - sp * 0.45;
					cr->set_line_width (line_w);
					cr->move_to (x0, yc - half (s));
					cr->line_to (x1, yc - half (e));
					cr->move_to (x0, yc + half (s));
					cr->line_to (x1, yc + half (e));
					cr->stroke ();
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
	int const p      = (octave + 1) * 12 + semis[letter] + key_alter (letter, fifths_at (bar_index));
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

	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (_("add note"));

	{
		MidiModel::ReadLock lm (model->read_lock ());
		for (auto const& e : model->notes ()) {
			/* the same note is already there: select it instead of stacking a copy */
			if (e->note () == pitch && e->time () == start) {
				delete cmd;
				_selection.clear ();
				_selection.insert (e);
				queue_draw ();
				return;
			}
		}
		/* notes on the same staff do not overlap the new one (that would
		 * make a second voice): it ends where the next one starts, and a
		 * note sounding into it ends there; same start makes a chord */
		if (!_percussion) {
			for (auto const& e : model->notes ()) {
				if ((e->note () >= 60) != (pitch >= 60)) {
					continue;
				}
				int64_t const et = note_tick (e);
				int64_t const ee = et + e->length ().to_ticks ();
				if (et > tick && et < tick + len) {
					len = et - tick;
				} else if (et < tick && ee > tick) {
					cmd->change (e, MidiModel::NoteDiffCommand::Length, Beats::ticks (tick - et));
				}
			}
		}
	}

	NotePtr n (new Evoral::Note<Beats> (_channel, start, Beats::ticks (len), pitch, 100));
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
		int64_t const tick = b * _bar_len + snap_time (time_in_bar (_bars[b], ev->x));
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
ScoreView::snap_time (int64_t t) const
{
	/* the grid restarts on every beat (tuplet steps do not add up
	 * across beats), or every half note for quarter triplets */
	int64_t const step   = snap_step ();
	int64_t const period = (quarter % step == 0) ? quarter : ((2 * quarter) % step == 0 ? 2 * quarter : _bar_len);
	int64_t const p0     = (t / period) * period;
	return std::min (_bar_len - 1, p0 + ((t - p0) / step) * step);
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
		spell (pitch, d, alt, fifths_at (b));
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
		int64_t const t    = b * _bar_len + snap_time (time_in_bar (_bars[b], ev->x));
		int64_t const t0   = note_tick (_drag_note);
		int64_t const b0   = (t0 / _bar_len) * _bar_len;
		/* keep the grabbed note's offset from the snap grid */
		int64_t const dt   = t - (b0 + snap_time (t0 - b0));
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
