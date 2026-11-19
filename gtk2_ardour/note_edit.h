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
 * on its own with a constant ratio (Rubber Band R3, formants preserved) and crossfaded into the audio.
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

	Note () : start (0), end (0), ostart (0), oend (0), pitch (0), shift (0) {}
	float target () const { return pitch + shift; }
	bool pitch_edited () const { return shift > 0.001f || shift < -0.001f; }
	bool time_edited () const { return ostart != start || oend != end; }
	bool edited () const { return pitch_edited () || time_edited (); }
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

/** Master-source range the region plays (start, length) */
void region_master_range (std::shared_ptr<ARDOUR::AudioRegion>, ARDOUR::samplepos_t& start, ARDOUR::samplecnt_t& len);

/** Reads the region's master sources (all channels) at master-source positions */
bool read_master (std::shared_ptr<ARDOUR::AudioRegion>, ARDOUR::samplepos_t start, ARDOUR::samplecnt_t len, std::vector<std::vector<float> >& out);

/** pYIN analysis of the region's range (mono sum). Returns 0 on success. */
int analyse (std::shared_ptr<ARDOUR::AudioRegion>, Analysis&, PBD::Progress* progress = 0);

/** Notes saved on the region (empty when it was never edited) */
bool load_notes (std::shared_ptr<ARDOUR::Region const>, std::vector<Note>&);
XMLNode* notes_to_xml (std::vector<Note> const&);

/** Apply the edited notes to `in` (frames of all channels, from master position `origin`). First the timing:
 *  only the stretches of audio around moved / resized notes are time-stretched (Rubber Band, key frames at the
 *  note edges), everything else stays untouched. Then the pitch: each note is shifted with its own constant
 *  ratio and crossfaded in over 10 ms centred on its (output) edges. */
void render (std::vector<std::vector<float> > const& in, ARDOUR::samplepos_t origin, ARDOUR::samplecnt_t sr,
             std::vector<Note> const&, std::vector<std::vector<float> >& out);

/** Makes a new region of the edited audio (new sources, master sources = the originals) */
class Filter : public ARDOUR::Filter
{
public:
	Filter (ARDOUR::Session& s, std::vector<Note> const& notes) : ARDOUR::Filter (s), _notes (notes) {}
	int run (std::shared_ptr<ARDOUR::Region>, PBD::Progress* progress = 0);

private:
	std::vector<Note> _notes;
};

} // namespace NoteEdit
