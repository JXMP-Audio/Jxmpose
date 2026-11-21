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

/* Tests of the GUI-free logic of JXMPOSE's own editors: the Chord Pads' music theory (chord_theory.cc) and
 * Edit Notes (note_edit.cc: glides, rendering, the analysis cache). No test framework is needed: every
 * failed CHECK is printed and the exit status is the number of failures.
 *
 * Run: gtk2_ardour/jxmp-tests (after ./waf) */

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#include "chord_theory.h"
#include "note_edit.h"

using namespace ChordTheory;
using std::vector;

static int failures = 0;
static int checks   = 0;

#define CHECK(cond)                                                                \
	do {                                                                           \
		++checks;                                                                  \
		if (!(cond)) {                                                             \
			++failures;                                                            \
			fprintf (stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                          \
	} while (0)

static Chord
chord (int root, int type, int tension = 0, int bass = -1, int inversion = 0, int octave = 0)
{
	Chord c = { root, type, tension, bass, inversion, octave };
	return c;
}

static vector<int>
v (std::initializer_list<int> l)
{
	return vector<int> (l);
}

/* ---- Chord Pads ---- */

static int
tension_index (const char* label)
{
	for (int i = 0; i < n_tensions; ++i) {
		if (std::string (tensions[i].label) == label) {
			return i;
		}
	}
	return -1;
}

static void
test_chord_pitches ()
{
	CHECK (chord_pitches (chord (0, 0)) == v ({ 60, 64, 67 }));                         /* C */
	CHECK (chord_pitches (chord (9, 1, tension_index ("7"), -1, 0, -1)) == v ({ 57, 60, 64, 67 })); /* Am7 below C4 */
	CHECK (chord_pitches (chord (0, 0, 0, -1, 1)) == v ({ 64, 67, 72 }));               /* C, first inversion */
	CHECK (chord_pitches (chord (0, 0, 0, -1, 2)) == v ({ 67, 72, 76 }));               /* second inversion */
	CHECK (chord_pitches (chord (0, 0, 0, 4)) == v ({ 52, 60, 64, 67 }));               /* C/E: the bass below */
	CHECK (chord_pitches (chord (7, 0, tension_index ("7"))) == v ({ 67, 71, 74, 77 })); /* G7 */
	CHECK (chord_pitches (chord (0, 6)) == v ({ 60, 67 }));                             /* power chord */
	/* out of range values are clamped, never read outside the tables */
	CHECK (!chord_pitches (chord (0, 99, 99)).empty ());
	CHECK (!chord_pitches (chord (0, -5, -5)).empty ());
	/* nothing outside MIDI */
	for (int n : chord_pitches (chord (11, 0, tension_index ("13"), -1, 0, 6))) {
		CHECK (n >= 0 && n <= 127);
	}
}

static void
test_key_chords ()
{
	const int maj7 = tension_index ("maj7"), dom7 = tension_index ("7");

	/* C major: C Dm Em F G Am Bdim (as m7b5), sevenths Cmaj7 Dm7 Em7 Fmaj7 G7 Am7 Bm7b5 */
	vector<Chord> k = key_chords (0, 0);
	CHECK ((int) k.size () == n_key_chords);
	const int roots[7] = { 0, 2, 4, 5, 7, 9, 11 };
	const int typ[7]   = { 0, 1, 1, 0, 0, 1, 2 };
	const int sev[7]   = { maj7, dom7, dom7, maj7, dom7, dom7, dom7 };
	for (int d = 0; d < 7; ++d) {
		CHECK (k[d].root == roots[d]);
		CHECK (k[d].type == typ[d]);
		CHECK (k[9 + d].root == roots[d]);
		CHECK (k[9 + d].tension == sev[d]);
	}
	CHECK (k[6].tension == dom7); /* the diminished triad comes as m7b5 */
	CHECK (k[0].tension == 0);
	CHECK (k[7].root == 0 && k[7].tension == tension_index ("6"));
	CHECK (k[8].root == 0 && k[8].tension == tension_index ("6/9"));

	/* A minor (Aeolian): Am Bdim C Dm Em F G */
	k                  = key_chords (9, 5);
	const int mroot[7] = { 9, 11, 0, 2, 4, 5, 7 };
	const int mtyp[7]  = { 1, 2, 0, 1, 1, 0, 0 };
	for (int d = 0; d < 7; ++d) {
		CHECK (k[d].root == mroot[d]);
		CHECK (k[d].type == mtyp[d]);
	}

	/* every scale and key: where the scale has a regular triad on a degree (major, minor, diminished,
	 * augmented), the chord is that triad and all its notes (with the seventh of a diminished one) lie in the
	 * scale. Degrees without one (exotic scales) get the nearest triad on purpose and are left out. */
	for (int s = 0; s < n_scales; ++s) {
		if (!scales[s].name[0]) {
			continue;
		}
		int const* st    = scales[s].st;
		auto       above = [st] (int d, int k) { return (st[(d + k) % 7] - st[d] + 12) % 12; };
		for (int root = 0; root < 12; ++root) {
			std::set<int> pcs;
			for (int i = 0; i < 7; ++i) {
				pcs.insert ((root + st[i]) % 12);
			}
			vector<Chord> kc = key_chords (root, s);
			for (int d = 0; d < 7; ++d) {
				CHECK (kc[d].root == (root + st[d]) % 12);
				const int third = above (d, 2), fifth = above (d, 4);
				int       want  = -1;
				if (third == 4 && fifth == 7) {
					want = 0;
				} else if (third == 3 && fifth == 7) {
					want = 1;
				} else if (third == 3 && fifth == 6) {
					want = 2;
				} else if (third == 4 && fifth == 8) {
					want = 3;
				}
				if (want < 0) {
					continue;
				}
				CHECK (kc[d].type == want);
				for (int n : chord_pitches (kc[d])) {
					CHECK (pcs.count (n % 12));
				}
			}
		}
	}

	/* an invalid key falls back to C major instead of reading outside the tables */
	CHECK (key_chords (-1, 0)[0].root == 0);
	CHECK (key_chords (0, 999)[0].root == 0);
	CHECK ((int) key_chords (0, 999).size () == n_key_chords);
}

static void
test_identify ()
{
	Chord c;
	CHECK (identify (v ({ 60, 64, 67 }), c) && c.root == 0 && c.type == 0 && c.tension == 0 && c.bass == -1);
	CHECK (identify (v ({ 57, 60, 64 }), c) && c.root == 9 && c.type == 1 && c.bass == -1); /* Am */
	CHECK (identify (v ({ 64, 67, 72 }), c) && c.root == 0 && c.type == 0 && c.bass == 4);  /* C/E */
	CHECK (identify (v ({ 55, 59, 62, 65 }), c) && c.root == 7 && c.tension == tension_index ("7")); /* G7 */
	CHECK (!identify (v ({ 60 }), c));
	CHECK (!identify (v ({ 60, 61, 62, 63 }), c)); /* a cluster is no chord */

	/* every chord type and tension, in every root, is recognised as the same notes */
	for (int root = 0; root < 12; ++root) {
		for (int ty = 0; ty < n_types; ++ty) {
			for (int tn = 0; tn < n_tensions; ++tn) {
				const vector<int> p = chord_pitches (chord (root, ty, tn));
				Chord             r;
				CHECK (identify (p, r));
				std::set<int> a, b;
				for (int n : p) {
					a.insert (n % 12);
				}
				for (int n : chord_pitches (r)) {
					b.insert (n % 12);
				}
				CHECK (a == b);
			}
		}
	}
}

static void
test_fit ()
{
	const Chord C = chord (0, 0), G = chord (7, 0), F = chord (5, 0), Am = chord (9, 1), D = chord (2, 0), Fs = chord (6, 0);
	CHECK (fit (C, C) == -1);
	CHECK (fit (C, G) == 3);  /* one step on the circle */
	CHECK (fit (C, F) == 3);
	CHECK (fit (C, Am) == 3); /* the relative minor */
	CHECK (fit (C, D) == 2);  /* two steps */
	CHECK (fit (C, Fs) == 0); /* opposite side */
	/* G7 wants to resolve down a fifth (to C), not up to D */
	const Chord G7 = chord (7, 0, tension_index ("7"));
	CHECK (fit (G7, C) == 3);
	CHECK (fit (G7, D) == 2);
	/* symmetric for plain triads */
	for (int a = 0; a < 12; ++a) {
		for (int b = 0; b < 12; ++b) {
			CHECK (fit (chord (a, 0), chord (b, 0)) == fit (chord (b, 0), chord (a, 0)));
		}
	}
}

/* ---- Edit Notes ---- */

static NoteEdit::Note
note (ARDOUR::samplepos_t s, ARDOUR::samplepos_t e, float pitch, float shift = 0)
{
	NoteEdit::Note n;
	n.start = n.ostart = s;
	n.end = n.oend = e;
	n.pitch        = pitch;
	n.shift        = shift;
	return n;
}

static void
test_glides ()
{
	const ARDOUR::samplecnt_t sr = 48000;
	vector<NoteEdit::Note>    ns;
	ns.push_back (note (0, sr, 60));
	ns.push_back (note (sr + sr / 10, 2 * sr, 62)); /* 100 ms after the first */
	ns.push_back (note (4 * sr, 5 * sr, 64));       /* far from the others */

	/* nothing edited: no glides */
	CHECK (NoteEdit::glides (ns, sr).empty ());

	/* the second note re-pitched: joined to the first (close), open towards the third (far) */
	ns[1].shift                            = 2;
	vector<NoteEdit::Glide> g              = NoteEdit::glides (ns, sr);
	bool                    joined_to_prev = false;
	for (auto const& x : g) {
		CHECK (x.a < x.b);
		if (x.open == 0) {
			joined_to_prev = x.s0 == 0 && x.s1 == 2 && x.a < ns[0].oend && x.b > ns[1].ostart;
		}
		/* the far note is never touched */
		CHECK (x.b <= ns[2].ostart);
	}
	CHECK (joined_to_prev);
}

static double
zero_crossing_hz (vector<float> const& x, size_t a, size_t b, double sr)
{
	int n = 0;
	for (size_t i = a + 1; i < b; ++i) {
		n += (x[i - 1] < 0) != (x[i] < 0);
	}
	return n * sr / (2.0 * (b - a));
}

static void
test_render ()
{
	const ARDOUR::samplecnt_t sr  = 48000;
	const size_t              len = 2 * sr;
	vector<vector<float> >    in (1, vector<float> (len));
	for (size_t i = 0; i < len; ++i) {
		in[0][i] = 0.5f * sinf (2 * M_PI * 220.0 * i / sr); /* A3 = MIDI 57 */
	}
	NoteEdit::Analysis an;
	an.sr     = sr;
	an.hop    = 256;
	an.origin = 0;
	an.length = len;
	an.midi.assign (len / an.hop, 57.f);
	an.rms.assign (len / an.hop, 1.f);

	vector<NoteEdit::Note> ns;
	ns.push_back (note (sr / 2, 3 * sr / 2, 57));

	/* nothing edited: the audio comes back unchanged */
	vector<vector<float> > out;
	NoteEdit::render (in, 0, sr, ns, an, out);
	CHECK (out.size () == 1 && out[0].size () == len);
	double maxdiff = 0;
	for (size_t i = 0; i < len && out.size () == 1 && out[0].size () == len; ++i) {
		maxdiff = std::max (maxdiff, (double) fabsf (out[0][i] - in[0][i]));
	}
	CHECK (maxdiff < 1e-6);

	/* a fifth up: the middle of the note plays at 330 Hz, outside it the audio stays */
	ns[0].shift = 7;
	NoteEdit::render (in, 0, sr, ns, an, out);
	CHECK (out.size () == 1 && out[0].size () == len);
	if (out.size () == 1 && out[0].size () == len) {
		const double mid = zero_crossing_hz (out[0], 3 * sr / 4, 5 * sr / 4, sr);
		const double pre = zero_crossing_hz (out[0], 0, sr * 3 / 10, sr); /* before the note and its 150 ms tail */
		CHECK (fabs (mid - 220.0 * pow (2.0, 7 / 12.0)) < 4.0);
		CHECK (fabs (pre - 220.0) < 3.0); /* one zero crossing = 1.7 Hz here */
		if (fabs (mid - 329.6) >= 4.0 || fabs (pre - 220.0) >= 3.0) {
			fprintf (stderr, "  render: %.1f Hz in the note (want 329.6), %.1f Hz before it (want 220)\n", mid, pre);
		}
	}
}

static void
test_analysis_cache ()
{
	gchar* dir = g_dir_make_tmp ("jxmp-tests-XXXXXX", 0);
	CHECK (dir != 0);
	if (!dir) {
		return;
	}
	const std::string file = std::string (dir) + "/a.bin";

	NoteEdit::Analysis a;
	a.sr     = 44100;
	a.hop    = 256;
	a.origin = 12345;
	a.length = 99999;
	for (int i = 0; i < 300; ++i) {
		a.midi.push_back (i % 7 ? 60.f + i * 0.01f : 0.f);
		a.rms.push_back (i / 300.f);
	}
	NoteEdit::Note n = note (12345, 20000, 61.5f, 1.25f);
	n.ostart         = 12400;
	n.drift          = 0.5f;
	n.gain           = -3;
	n.formant        = 2;
	n.trans          = 0.05f;
	a.notes.push_back (n);
	a.notes.push_back (note (30000, 40000, 64));

	CHECK (NoteEdit::save_analysis (file, "key 1", a));
	NoteEdit::Analysis b;
	CHECK (NoteEdit::load_analysis (file, "key 1", b));
	CHECK (b.sr == a.sr && b.hop == a.hop && b.origin == a.origin && b.length == a.length);
	CHECK (b.midi == a.midi && b.rms == a.rms);
	CHECK (b.notes.size () == 2);
	if (b.notes.size () == 2) {
		NoteEdit::Note const& m = b.notes[0];
		CHECK (m.start == n.start && m.end == n.end && m.ostart == n.ostart && m.oend == n.oend);
		CHECK (m.pitch == n.pitch && m.shift == n.shift && m.drift == n.drift && m.gain == n.gain && m.formant == n.formant && m.trans == n.trans);
	}

	/* made from other audio: not used */
	NoteEdit::Analysis c;
	CHECK (!NoteEdit::load_analysis (file, "key 2", c));
	CHECK (c.midi.empty ());

	/* damaged (cut short): not used */
	gchar* data = 0;
	gsize  size = 0;
	CHECK (g_file_get_contents (file.c_str (), &data, &size, 0));
	CHECK (g_file_set_contents (file.c_str (), data, size / 2, 0));
	g_free (data);
	CHECK (!NoteEdit::load_analysis (file, "key 1", c));

	/* missing */
	CHECK (!NoteEdit::load_analysis (std::string (dir) + "/none.bin", "key 1", c));

	g_unlink (file.c_str ());
	g_rmdir (dir);
	g_free (dir);
}

int
main ()
{
	test_chord_pitches ();
	test_key_chords ();
	test_identify ();
	test_fit ();
	test_glides ();
	test_render ();
	test_analysis_cache ();
	printf ("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
