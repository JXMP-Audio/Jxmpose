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
#include <cstdlib>
#include <set>

#include "chord_theory.h"

namespace ChordTheory {


/* the triad (or what stands for it) */
const ChordType types[] = {
	{ "Maj",  "Major: root, major third, fifth",           { 0, 4, 7 }, 3 },
	{ "Min",  "Minor: root, minor third, fifth",           { 0, 3, 7 }, 3 },
	{ "Dim",  "Diminished: minor third, diminished fifth (with 7: half diminished)", { 0, 3, 6 }, 3 },
	{ "Aug",  "Augmented: major third, augmented fifth",   { 0, 4, 8 }, 3 },
	{ "Sus2", "Suspended second: the third replaced by the second", { 0, 2, 7 }, 3 },
	{ "Sus4", "Suspended fourth: the third replaced by the fourth", { 0, 5, 7 }, 3 },
	{ "5",    "Power chord: root and fifth",               { 0, 7, 0 }, 2 },
};
const int n_types = sizeof (types) / sizeof (types[0]);


/* added notes: sevenths and the colours on top of them */
const Tension tensions[] = {
	{ "—", { 0, 0, 0 },    0 },
	{ "6",      { 9, 0, 0 },    1 },
	{ "7",      { 10, 0, 0 },   1 },
	{ "maj7",   { 11, 0, 0 },   1 },
	{ "dim7",   { 9, 0, 0 },    1 },
	{ "9",      { 10, 14, 0 },  2 },
	{ "maj9",   { 11, 14, 0 },  2 },
	{ "add9",   { 14, 0, 0 },   1 },
	{ "11",     { 10, 14, 17 }, 3 },
	{ "13",     { 10, 14, 21 }, 3 },
	{ "7b9",    { 10, 13, 0 },  2 },
	{ "7#9",    { 10, 15, 0 },  2 },
	{ "7#11",   { 10, 18, 0 },  2 },
	{ "7b13",   { 10, 20, 0 },  2 },
	{ "6/9",    { 9, 14, 0 },   2 },
};
const int n_tensions = sizeof (tensions) / sizeof (tensions[0]);

/* seven-note scales: the pads are built from them by stacking thirds */
const Scale scales[] = {
	{ "Major (Ionian)",     { 0, 2, 4, 5, 7, 9, 11 } },
	{ "Dorian",             { 0, 2, 3, 5, 7, 9, 10 } },
	{ "Phrygian",           { 0, 1, 3, 5, 7, 8, 10 } },
	{ "Lydian",             { 0, 2, 4, 6, 7, 9, 11 } },
	{ "Mixolydian",         { 0, 2, 4, 5, 7, 9, 10 } },
	{ "Minor (Aeolian)",    { 0, 2, 3, 5, 7, 8, 10 } },
	{ "Locrian",            { 0, 1, 3, 5, 6, 8, 10 } },
	{ "",                   { 0 } }, /* separator */
	{ "Harmonic Minor",     { 0, 2, 3, 5, 7, 8, 11 } },
	{ "Melodic Minor",      { 0, 2, 3, 5, 7, 9, 11 } },
	{ "Harmonic Major",     { 0, 2, 4, 5, 7, 8, 11 } },
	{ "Phrygian Dominant",  { 0, 1, 4, 5, 7, 8, 10 } },
	{ "Lydian Dominant",    { 0, 2, 4, 6, 7, 9, 10 } },
	{ "Altered",            { 0, 1, 3, 4, 6, 8, 10 } },
	{ "Dorian #4",          { 0, 2, 3, 6, 7, 9, 10 } },
	{ "Hungarian Minor",    { 0, 2, 3, 6, 7, 8, 11 } },
	{ "Neapolitan Minor",   { 0, 1, 3, 5, 7, 8, 11 } },
	{ "Neapolitan Major",   { 0, 1, 3, 5, 7, 9, 11 } },
	{ "Double Harmonic",    { 0, 1, 4, 5, 7, 8, 11 } },
	{ "Persian",            { 0, 1, 4, 5, 6, 8, 11 } },
	{ "Enigmatic",          { 0, 1, 4, 6, 8, 10, 11 } },
};
const int n_scales = sizeof (scales) / sizeof (scales[0]);


std::vector<int>
chord_pitches (Chord const& c)
{
	ChordType const& t    = types[std::max (0, std::min (n_types - 1, c.type))];
	Tension const&   tn   = tensions[std::max (0, std::min (n_tensions - 1, c.tension))];
	const int        base = 60 + c.root + 12 * c.octave;

	std::set<int> iv;
	for (int i = 0; i < t.n; ++i) {
		iv.insert (t.iv[i]);
	}
	for (int i = 0; i < tn.n; ++i) {
		iv.insert (tn.iv[i]);
	}
	std::vector<int> p;
	for (int i : iv) {
		p.push_back (base + i);
	}
	/* inversions: the lowest notes go up an octave */
	for (int k = 0; k < c.inversion && k + 1 < (int) p.size (); ++k) {
		const int lo = p.front ();
		p.erase (p.begin ());
		p.push_back (lo + 12);
		std::sort (p.begin (), p.end ());
	}
	/* a bass note below the chord */
	if (c.bass >= 0) {
		int b = 48 + c.bass + 12 * c.octave;
		while (b >= p.front ()) {
			b -= 12;
		}
		p.insert (p.begin (), b);
	}
	std::vector<int> out;
	for (int n : p) {
		if (n >= 0 && n <= 127) {
			out.push_back (n);
		}
	}
	return out;
}

std::vector<Chord>
key_chords (int root, int scale)
{
	/* stack thirds inside the scale: the third and fifth give the triad, the seventh the seventh chord */
	if (root < 0 || root > 11 || scale < 0 || scale >= n_scales || !scales[scale].name[0]) {
		root  = 0;
		scale = 0;
	}
	int const* st = scales[scale].st;
	auto above = [st] (int d, int k) { return (st[(d + k) % 7] - st[d] + 12) % 12; };
	auto build = [&, root] (int d, bool seventh) {
		Chord c;
		const int third = above (d, 2), fifth = above (d, 4), sev = above (d, 6);
		c.root    = (root + st[d]) % 12;
		c.tension = 0;
		if (third == 4 && fifth == 7) {
			c.type = 0;
		} else if (third == 3 && fifth == 7) {
			c.type = 1;
		} else if (third == 3 && fifth == 6) {
			c.type = 2;
			seventh = true; /* the diminished triad as m7b5 / dim7, as it is mostly used */
		} else if (third == 4 && fifth == 8) {
			c.type = 3;
		} else if (fifth == 7) {
			c.type = third == 2 ? 4 : 5; /* no third in the scale there: sus */
		} else {
			c.type = third >= 4 ? 0 : 1; /* odd fifths (exotic scales): the nearest triad */
		}
		if (seventh) {
			c.tension = sev == 11 ? 3 : (sev == 10 ? 2 : (sev == 9 && c.type == 2 ? 4 : 0));
		}
		c.bass      = -1;
		c.inversion = 0;
		c.octave    = c.root >= 7 ? -1 : 0; /* roots between G3 and F#4 */
		return c;
	};
	/* lower row: the triads of the seven degrees and two colours of the tonic (6, 6/9);
	 * upper row: the seventh chords of the seven degrees */
	std::vector<Chord> pads (n_key_chords, Chord { 0, 0, 0, -1, 0, 0 });
	for (int d = 0; d < 7; ++d) {
		pads[d]     = build (d, false);
		pads[9 + d] = build (d, true);
	}
	pads[7] = pads[0];
	pads[8] = pads[0];
	if (pads[0].type <= 1) {
		pads[7].tension = 1;  /* 6 */
		pads[8].tension = 14; /* 6/9 */
	} else {
		pads[7].tension = 2;
		pads[8].tension = 7; /* add9 */
	}
	return pads;
}

int
fit (Chord const& last, Chord const& next)
{
	/* the place on the circle: the major key the chord belongs to as I, vi (minor) or vii (diminished) */
	auto place = [] (Chord const& c, int& ring) {
		int key;
		if (c.type == 1) {
			ring = 1;
			key  = c.root + 3;
		} else if (c.type == 2) {
			ring = 2;
			key  = c.root + 1;
		} else {
			ring = 0;
			key  = c.root;
		}
		return ((key % 12) * 7) % 12;
	};
	int       ra, rb;
	const int ia = place (last, ra);
	const int ib = place (next, rb);
	if (ra == rb && last.root == next.root) {
		return -1;
	}
	const int d  = ((ib - ia) % 12 + 18) % 12 - 6; /* -6 .. 5, < 0: anticlockwise (down a fifth) */
	const int ad = std::abs (d);
	/* a dominant seventh wants to go down a fifth, not further up */
	const bool dominant = last.type == 0 && (last.tension == 2 || last.tension == 5 || (last.tension >= 8 && last.tension <= 13));
	if (dominant && d == 1) {
		return 2;
	}
	return ad <= 1 ? 3 : (ad == 2 ? 2 : (ad == 3 ? 1 : 0));
}

bool
identify (std::vector<int> const& notes, Chord& out)
{
	if (notes.size () < 2) {
		return false;
	}
	std::set<int> pcs;
	int           lo = 128;
	for (int n : notes) {
		pcs.insert (((n % 12) + 12) % 12);
		lo = std::min (lo, n);
	}
	const int bass = lo % 12;
	/* fewest notes added first: plain triads before tensions; the bass as root before slash chords */
	int best = -1;
	for (int pass = 0; pass < 2; ++pass) {
		for (int tn = 0; tn < n_tensions; ++tn) {
			for (int ty = 0; ty < n_types; ++ty) {
				for (int r = 0; r < 12; ++r) {
					if ((pass == 0) != (r == bass)) {
						continue;
					}
					Chord         c { r, ty, tn, -1, 0, 0 };
					std::set<int> cp;
					for (int n : chord_pitches (c)) {
						cp.insert (n % 12);
					}
					if (cp == pcs) {
						c.bass = r == bass ? -1 : bass;
						c.octave = lo / 12 - 5;
						out      = c;
						return true;
					}
				}
			}
		}
	}
	(void) best;
	return false;
}

} // namespace ChordTheory
