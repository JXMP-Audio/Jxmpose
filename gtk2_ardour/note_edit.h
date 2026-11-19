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

/* Per-note pitch editing of monophonic audio (vocals).
 *
 * The audio of a region is analysed once (pYIN: pitch track + note segmentation), every detected note
 * becomes an editable object with a pitch offset. Rendering never touches the original recording: the
 * region always reads from its master (original) sources, every edited note is pitch shifted on its own
 * with a constant ratio (Rubber Band R3, formants preserved) and crossfaded into the untouched audio.
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
	ARDOUR::samplepos_t start; ///< master-source sample position
	ARDOUR::samplepos_t end;   ///< exclusive
	float pitch;               ///< detected pitch centre, MIDI note number (fractional)
	float shift;               ///< edit, semitones

	Note () : start (0), end (0), pitch (0), shift (0) {}
	float target () const { return pitch + shift; }
	bool edited () const { return shift > 0.001f || shift < -0.001f; }
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

/** Master-source range the region plays (start, length) */
void region_master_range (std::shared_ptr<ARDOUR::AudioRegion>, ARDOUR::samplepos_t& start, ARDOUR::samplecnt_t& len);

/** Reads the region's master sources (all channels) at master-source positions */
bool read_master (std::shared_ptr<ARDOUR::AudioRegion>, ARDOUR::samplepos_t start, ARDOUR::samplecnt_t len, std::vector<std::vector<float> >& out);

/** pYIN analysis of the region's range (mono sum). Returns 0 on success. */
int analyse (std::shared_ptr<ARDOUR::AudioRegion>, Analysis&, PBD::Progress* progress = 0);

/** Notes saved on the region (empty when it was never edited) */
bool load_notes (std::shared_ptr<ARDOUR::Region const>, std::vector<Note>&);
XMLNode* notes_to_xml (std::vector<Note> const&);

/** Shift `in` (frames of all channels, from master position `origin`) by the edited notes: each note is
 *  rendered with its own constant ratio and crossfaded in over `xfade` samples centred on its edges. */
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
