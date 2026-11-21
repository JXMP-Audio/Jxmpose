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
#include <string>
#include <vector>

/** The music theory of the Chord Pads, without any GUI (so it can be tested on its own): chords built from
 *  a root, a chord type, a tension and a bass note, the chords of a key, how well one chord follows another
 *  on the circle of fifths, and which chord some notes form. */
namespace ChordTheory {

struct Chord {
	int root;      ///< 0 = C .. 11 = B
	int type;      ///< index into the chord types
	int tension;   ///< index into the tensions, 0 = none
	int bass;      ///< -1 = the root, else 0 .. 11
	int inversion; ///< 0 = root position
	int octave;    ///< offset from the octave of middle C
};

struct ChordType {
	const char* label;
	const char* tip;
	int         iv[3];
	int         n;
};
extern const ChordType types[];
extern const int       n_types;

struct Tension {
	const char* label;
	int         iv[3]; ///< added above the root
	int         n;
};
extern const Tension tensions[];
extern const int     n_tensions;

/** seven-note scales: the chords of a key are built from them by stacking thirds; an empty name is a
 *  separator in the menu */
struct Scale {
	const char* name;
	int         st[7];
};
extern const Scale scales[];
extern const int   n_scales;

/** number of chords of a key (key_chords) */
const int n_key_chords = 16;

/** MIDI notes of a chord, lowest first (root position from middle C + octave, inversions, bass below) */
std::vector<int> chord_pitches (Chord const&);

/** the chords of a key: [0..6] the triads of the seven degrees, [7] and [8] two colours of the tonic
 *  (6 and 6/9 on a major or minor tonic), [9..15] the seventh chords of the degrees */
std::vector<Chord> key_chords (int root, int scale);

/** how well `next` follows `last` by the circle of fifths: 3 best (a step around the circle or its
 *  relative), 2 good (two steps), 1 possible (three), 0 far; -1 the same chord */
int fit (Chord const& last, Chord const& next);

/** the chord some notes form (any voicing, a bass note other than the root becomes a slash chord);
 *  false if they are no known chord */
bool identify (std::vector<int> const& notes, Chord&);

} // namespace ChordTheory
