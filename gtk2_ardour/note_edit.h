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

/* Per-note pitch and timing editing of monophonic audio (vocals, bass, solo instruments).
 *
 * The audio of a region is analysed once (pYIN: pitch track + note segmentation), every detected note
 * becomes an editable object with a pitch offset and an output position / length. Rendering never touches
 * the original recording: the region always reads from its master (original) sources, the stretches around
 * moved notes are time-stretched with key frames at the note edges, then every re-pitched note is shifted
 * on its own with a constant ratio (Rubber Band R3, formants preserved or shifted), its slow pitch drift is
 * flattened by reading it at a varying rate (windowed sinc), and it is crossfaded into the audio with its level.
 * The edits live in the region's extra XML ("JXNoteEdit"), in master-source sample positions, so they
 * survive trims and can be reopened.
 */

#include <memory>
#include <vector>

#include "ardour/filter.h"
#include "ardour/types.h"

class XMLNode;

namespace ARDOUR {
	class AudioRegion;
	class Region;
	class Session;
}
namespace PBD {
	class Progress;
}

namespace NoteEdit {

struct Note {
	ARDOUR::samplepos_t start;  ///< where it was sung: master-source sample position
	ARDOUR::samplepos_t end;    ///< exclusive
	ARDOUR::samplepos_t ostart; ///< where it plays after a time edit (same coordinates)
	ARDOUR::samplepos_t oend;
	float pitch;                ///< detected pitch centre, MIDI note number (fractional)
	float shift;                ///< edit, semitones
	float drift;                ///< 0..1: how much of the slow pitch drift inside the note is flattened (vibrato stays)
	float gain;                 ///< dB
	float formant;              ///< formant shift, semitones (0 = formants stay where they were sung)
	float trans;                ///< transition from the previous note: its length in seconds, 0 = as it was sung

	Note () : start (0), end (0), ostart (0), oend (0), pitch (0), shift (0), drift (0), gain (0), formant (0), trans (0) {}
	float target () const { return pitch + shift; }
	bool pitch_edited () const { return shift > 0.001f || shift < -0.001f || drift > 0.001f || formant > 0.001f || formant < -0.001f; }
	bool time_edited () const { return ostart != start || oend != end; }
	bool gain_edited () const { return gain > 0.01f || gain < -0.01f; }
	bool trans_edited () const { return trans > 0.0005f; }
	bool edited () const { return pitch_edited () || time_edited () || gain_edited () || trans_edited (); }
	/** output position of a source position inside the note */
	double to_out (double src) const { return end > start ? ostart + (src - start) * (double) (oend - ostart) / (end - start) : ostart; }
	double to_src (double out) const { return oend > ostart ? start + (out - ostart) * (double) (end - start) / (oend - ostart) : start; }
};


/** What the analysis found in one region's range of its master sources */
struct Analysis {
	ARDOUR::samplecnt_t sr;
	ARDOUR::samplecnt_t hop;    ///< samples per frame of midi / rms
	ARDOUR::samplepos_t origin; ///< master-source position of frame 0
	ARDOUR::samplecnt_t length; ///< samples analysed
	std::vector<float> midi;    ///< pitch per frame (MIDI note), 0 = unvoiced
	std::vector<float> rms;     ///< level per frame, 0..1 (normalised to the loudest frame)
	std::vector<Note> notes;

	Analysis () : sr (48000), hop (256), origin (0), length (0) {}
};

/** Pitch centre (median of the pitch track) between two master-source positions; `fallback` without voiced frames */
float median_pitch (Analysis const&, ARDOUR::samplepos_t start, ARDOUR::samplepos_t end, float fallback);

/** Slow pitch deviation from the note's centre per analysis frame (semitones, frames f0 .. f1 of the note;
 *  unvoiced frames are bridged). Vibrato is averaged out, this is what drift correction removes. */
std::vector<float> drift_curve (Analysis const&, Note const&, ARDOUR::samplepos_t& f0);

/** Master-source range the region plays (start, length) */
void region_master_range (std::shared_ptr<ARDOUR::AudioRegion>, ARDOUR::samplepos_t& start, ARDOUR::samplecnt_t& len);

/** What a worker thread reads: the region's master sources (one per channel) and the range of them the region
 *  plays. Taken in the GUI thread, so the worker never touches the region itself (it may be trimmed, moved or
 *  removed meanwhile, and its positions may be in music time, which needs the tempo map of the GUI thread). */
struct SourceRange {
	std::vector<std::shared_ptr<ARDOUR::AudioSource> > sources;
	ARDOUR::samplepos_t start;
	ARDOUR::samplecnt_t len;
	ARDOUR::samplecnt_t sr;
	SourceRange () : start (0), len (0), sr (0) {}
	/** the same audio is read from both */
	bool same_audio (SourceRange const& o) const { return sources == o.sources && start == o.start && len == o.len; }
};
/** GUI thread only. False when the region has no audio master sources. */
bool source_range (std::shared_ptr<ARDOUR::AudioRegion>, SourceRange&);

/** Reads the master sources (all channels) of the range; safe in a worker thread */
bool read_master (SourceRange const&, std::vector<std::vector<float> >& out);

/** pYIN analysis of the range (mono sum). Returns 0 on success. Safe in a worker thread. */
int analyse (SourceRange const&, Analysis&, PBD::Progress* progress = 0);

/** Notes saved on the region (empty when it was never edited) */
bool load_notes (std::shared_ptr<ARDOUR::Region const>, std::vector<Note>&);
XMLNode* notes_to_xml (std::vector<Note> const&);

/** The audio between the notes follows their pitch edits (output positions, semitones):
 *  - two notes less than 300 ms apart are joined by a glide from the shift of the first to the shift of the
 *    second; it reaches into both notes (up to 80 ms, a quarter of the note), because the slide from one
 *    note to the next usually lies inside the notes, and replaces them there. With a transition length set
 *    on the second note (Note::trans) the sung slide is replaced by an S-curve of that length from the
 *    first note's target pitch to the second one's, centred between them,
 *  - before / after a note without such a neighbour (a scoop into it, its tail) the note's shift is kept for
 *    150 ms and fades back to the untouched audio over the outer half (`open` = -1 at a, +1 at b, 0 joined).
 *  Only where a neighbouring note has a new pitch, formant or transition. */
struct Glide {
	ARDOUR::samplepos_t a, b;
	float               s0, s1;
	float               formant;
	int                 open;
	float               trans;   ///< seconds, 0 = keep the sung slide
	ARDOUR::samplepos_t c;       ///< centre of the transition (output position)
	Note                na, nb;  ///< the joined notes (open: both the note itself)
};
std::vector<Glide> glides (std::vector<Note> const&, ARDOUR::samplecnt_t sr);
/** 0..1: how much of the glided audio is used at output position `pos` (10 ms crossfades at joined ends) */
double glide_weight (Glide const&, double pos, ARDOUR::samplecnt_t sr);
/** Semitones the glide moves the audio at output position `pos` by; `an` (the pitch track) is needed for a
 *  set transition length, without it the plain glide is returned */
double glide_offset (Glide const&, double pos, Analysis const* an);
/** The glide covering output position `pos`: its offset there (semitones) and weight; false if there is none */
bool glide_at (std::vector<Glide> const&, double pos, ARDOUR::samplecnt_t sr, double& shift, double& weight,
               Analysis const* an = 0);

/** Apply the edited notes to `in` (frames of all channels, from master position `origin`). First the timing:
 *  only the stretches of audio around moved / resized notes are time-stretched (Rubber Band, key frames at the
 *  note edges), everything else stays untouched. Then the pitch: each note is shifted with its own constant
 *  ratio and crossfaded in over 10 ms centred on its (output) edges. */
void render (std::vector<std::vector<float> > const& in, ARDOUR::samplepos_t origin, ARDOUR::samplecnt_t sr,
             std::vector<Note> const&, Analysis const&, std::vector<std::vector<float> >& out,
             PBD::Progress* progress = 0);

/** Reads the original audio of the range and renders the notes into it (nothing but the sources is touched,
 *  so this can run in a worker thread). Returns 0 on success, -1 on an error or when `progress` was cancelled. */
int render_range (SourceRange const&, std::vector<Note> const&, Analysis const&,
                  std::vector<std::vector<float> >& out, PBD::Progress* progress = 0);

/** True when the region can be rendered (no time stretch / pitch shift of its own); logs why not */
bool can_render (std::shared_ptr<ARDOUR::AudioRegion>);

/** render_range () of the region's range, GUI thread */
int render_region (std::shared_ptr<ARDOUR::AudioRegion>, std::vector<Note> const&, Analysis const&,
                   std::vector<std::vector<float> >& out, PBD::Progress* progress = 0);

/** Makes a new region of the edited audio (new sources, master sources = the originals) */
class Filter : public ARDOUR::Filter
{
public:
	Filter (ARDOUR::Session& s, std::vector<Note> const& notes, Analysis const& an) : ARDOUR::Filter (s), _notes (notes), _an (an) {}
	int run (std::shared_ptr<ARDOUR::Region>, PBD::Progress* progress = 0);
	/** use audio already made by render_region() (taken over) instead of rendering in run() */
	void set_rendered (std::vector<std::vector<float> >& out) { _rendered.swap (out); }

private:
	std::vector<Note> _notes;
	Analysis          _an;
	std::vector<std::vector<float> > _rendered;
};

} // namespace NoteEdit
