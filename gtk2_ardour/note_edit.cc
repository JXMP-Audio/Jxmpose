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
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/miscutils.h>

#include <rubberband/RubberBandStretcher.h>
#include <vamp-hostsdk/PluginLoader.h>

#include "pbd/compose.h"
#include "pbd/error.h"
#include "pbd/progress.h"
#include "pbd/xml++.h"

#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/session_directory.h"

#include "note_edit.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace PBD;
using std::vector;

namespace NoteEdit {

static float
hz_to_midi (float hz)
{
	return 69.f + 12.f * std::log2 (hz / 440.f);
}

void
region_master_range (std::shared_ptr<AudioRegion> r, samplepos_t& start, samplecnt_t& len)
{
	start = r->ancestral_start_sample () + r->start_sample ();
	len   = r->length_samples ();
}

static std::string
cache_name (PBD::ID const& source, samplepos_t start, samplecnt_t len)
{
	return string_compose ("editnotes-%1-%2-%3.bin", source.to_s (), start, len);
}

void
cleanup_analysis_cache (Session& s, CleanupReport& rep)
{
	/* the files still used: one per range of a source some audio region plays (also regions only in the
	 * region list: Edit Notes may be opened on them again) */
	std::set<std::string> used;
	for (auto const& i : RegionFactory::all_regions ()) {
		std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (i.second);
		if (!ar || ar->master_sources ().empty ()) {
			continue;
		}
		samplepos_t start;
		samplecnt_t len;
		region_master_range (ar, start, len);
		used.insert (cache_name (ar->master_sources ().front ()->id (), start, len));
	}

	const std::string dir = s.analysis_dir ();
	GDir*             d   = g_dir_open (dir.c_str (), 0, 0);
	if (!d) {
		return;
	}
	vector<std::string> gone;
	while (const gchar* n = g_dir_read_name (d)) {
		const std::string name (n);
		if (name.compare (0, 10, "editnotes-") == 0 && name.size () > 14 && name.compare (name.size () - 4, 4, ".bin") == 0
		    && !used.count (name)) {
			gone.push_back (name);
		}
	}
	g_dir_close (d);

	/* moved to the "dead" folder like the other cleaned files: Flush Wastebasket deletes them */
	const std::string dead = s.session_directory ().dead_path ();
	for (auto const& name : gone) {
		const std::string path = Glib::build_filename (dir, name);
		GStatBuf          st;
		const size_t      size = g_stat (path.c_str (), &st) == 0 ? st.st_size : 0;
		if (g_rename (path.c_str (), Glib::build_filename (dead, name).c_str ()) == 0) {
			rep.paths.push_back (path);
			rep.space += size;
		}
	}
}

bool
source_range (std::shared_ptr<AudioRegion> r, SourceRange& sr)
{
	sr = SourceRange ();
	region_master_range (r, sr.start, sr.len);
	sr.sr = r->session ().sample_rate ();
	std::stringstream key;
	key << "v1 " << sr.start << " " << sr.len << " " << sr.sr;
	for (auto const& s : r->master_sources ()) {
		std::shared_ptr<AudioSource> as = std::dynamic_pointer_cast<AudioSource> (s);
		if (!as) {
			return false;
		}
		sr.sources.push_back (as);
		key << " " << as->id ().to_s () << ":" << as->length ().samples ();
	}
	if (sr.sources.empty ()) {
		return false;
	}
	sr.cache_key  = key.str ();
	sr.cache_file = Glib::build_filename (r->session ().analysis_dir (), cache_name (sr.sources.front ()->id (), sr.start, sr.len));
	return true;
}

bool
read_master (SourceRange const& sr, vector<vector<float> >& out)
{
	out.assign (sr.sources.size (), vector<float> (sr.len, 0.f));
	const samplecnt_t chunk = 65536;
	for (size_t c = 0; c < sr.sources.size (); ++c) {
		/* AudioSource::read () takes the source's own lock: fine next to the butler */
		for (samplecnt_t pos = 0; pos < sr.len; pos += chunk) {
			const samplecnt_t n = std::min (chunk, sr.len - pos);
			if (sr.sources[c]->read (&out[c][pos], sr.start + pos, n) != n) {
				return false;
			}
		}
	}
	return true;
}

float
median_pitch (Analysis const& a, samplepos_t start, samplepos_t end, float fallback)
{
	vector<float> v;
	const samplepos_t f0 = std::max ((samplepos_t) 0, (start - a.origin) / a.hop);
	const samplepos_t f1 = std::min ((samplepos_t) a.midi.size () - 1, (end - a.origin) / a.hop);
	for (samplepos_t fr = f0; fr <= f1; ++fr) {
		if (a.midi[fr] > 0.f) {
			v.push_back (a.midi[fr]);
		}
	}
	if (v.empty ()) {
		return fallback;
	}
	std::nth_element (v.begin (), v.begin () + v.size () / 2, v.end ());
	return v[v.size () / 2];
}

vector<float>
drift_curve (Analysis const& a, Note const& n, samplepos_t& f0)
{
	vector<float> dev;
	if (a.midi.empty ()) {
		f0 = 0;
		return dev;
	}
	f0 = std::max ((samplepos_t) 0, (n.start - a.origin) / a.hop);
	const samplepos_t f1 = std::min ((samplepos_t) a.midi.size () - 1, (n.end - a.origin) / a.hop);
	if (f1 < f0) {
		return dev;
	}
	const size_t  len = f1 - f0 + 1;
	vector<float> p (len, 0.f);
	/* bridge unvoiced frames: hold the last voiced value, before the first one use the first one */
	float last = -1;
	for (size_t k = 0; k < len; ++k) {
		const float m = a.midi[f0 + k];
		if (m > 0 && std::fabs (m - n.pitch) < 3) { /* octave errors are not drift */
			last = m;
		}
		p[k] = last;
	}
	float first = n.pitch;
	for (size_t k = 0; k < len; ++k) {
		if (p[k] >= 0) {
			first = p[k];
			break;
		}
	}
	for (size_t k = 0; k < len && p[k] < 0; ++k) {
		p[k] = first;
	}
	/* moving average over ~200 ms: vibrato (5..7 Hz) averages out, the slow drift stays */
	const int half = std::max (1, (int) (0.1 * a.sr / a.hop));
	dev.resize (len);
	double sum = 0;
	int    cnt = 0;
	int    lo = 0, hi = -1;
	for (int k = 0; k < (int) len; ++k) {
		while (hi < std::min ((int) len - 1, k + half)) {
			sum += p[++hi];
			++cnt;
		}
		while (lo < k - half) {
			sum -= p[lo++];
			--cnt;
		}
		dev[k] = sum / cnt;
	}
	/* drift is the wandering around the note's own centre: the centre itself is left to the pitch edit */
	double mean = 0;
	for (float d : dev) {
		mean += d;
	}
	mean /= len;
	for (float& d : dev) {
		d -= mean;
	}
	return dev;
}

/* ---- the analysis cache: a small binary file, "JXNE" + version, the key, the analysis ---- */

namespace {
const char     cache_magic[4] = { 'J', 'X', 'N', 'E' };
const uint32_t cache_version  = 1;

template <typename T>
void
put (std::string& b, T v)
{
	b.append ((const char*) &v, sizeof (T));
}

struct Reader {
	const char* p;
	const char* e;
	template <typename T>
	bool get (T& v)
	{
		if ((size_t) (e - p) < sizeof (T)) {
			return false;
		}
		memcpy (&v, p, sizeof (T));
		p += sizeof (T);
		return true;
	}
	bool get_floats (vector<float>& v, uint64_t n)
	{
		if (n > (uint64_t) (e - p) / sizeof (float)) {
			return false;
		}
		v.resize (n);
		if (n) {
			memcpy (&v[0], p, n * sizeof (float));
		}
		p += n * sizeof (float);
		return true;
	}
};
} // namespace

bool
save_analysis (std::string const& file, std::string const& key, Analysis const& a)
{
	std::string b;
	b.append (cache_magic, 4);
	put<uint32_t> (b, cache_version);
	put<uint32_t> (b, key.size ());
	b.append (key);
	put<int64_t> (b, a.sr);
	put<int64_t> (b, a.hop);
	put<int64_t> (b, a.origin);
	put<int64_t> (b, a.length);
	put<uint64_t> (b, a.midi.size ());
	b.append ((const char*) a.midi.data (), a.midi.size () * sizeof (float));
	put<uint64_t> (b, a.rms.size ());
	b.append ((const char*) a.rms.data (), a.rms.size () * sizeof (float));
	put<uint64_t> (b, a.notes.size ());
	for (auto const& n : a.notes) {
		put<int64_t> (b, n.start);
		put<int64_t> (b, n.end);
		put<int64_t> (b, n.ostart);
		put<int64_t> (b, n.oend);
		put<float> (b, n.pitch);
		put<float> (b, n.shift);
		put<float> (b, n.drift);
		put<float> (b, n.gain);
		put<float> (b, n.formant);
		put<float> (b, n.trans);
	}
	/* g_file_set_contents () writes a temporary file and renames it: never a half written cache */
	GError* err = 0;
	if (!g_file_set_contents (file.c_str (), b.data (), b.size (), &err)) {
		if (err) {
			g_error_free (err);
		}
		return false;
	}
	return true;
}

bool
load_analysis (std::string const& file, std::string const& key, Analysis& a)
{
	gchar* data = 0;
	gsize  size = 0;
	if (!g_file_get_contents (file.c_str (), &data, &size, 0)) {
		return false;
	}
	Reader   r = { data, data + size };
	bool     ok = size >= 4 && !memcmp (data, cache_magic, 4);
	uint32_t ver = 0, klen = 0;
	r.p += 4;
	ok = ok && r.get (ver) && ver == cache_version && r.get (klen) && klen == key.size () && (size_t) (r.e - r.p) >= klen
	     && !memcmp (r.p, key.data (), klen);
	Analysis an;
	if (ok) {
		r.p += klen;
		int64_t  sr, hop, origin, length;
		uint64_t n;
		ok = r.get (sr) && r.get (hop) && r.get (origin) && r.get (length) && hop > 0 && length >= 0;
		an.sr     = sr;
		an.hop    = hop;
		an.origin = origin;
		an.length = length;
		ok = ok && r.get (n) && r.get_floats (an.midi, n) && r.get (n) && r.get_floats (an.rms, n) && r.get (n);
		for (uint64_t i = 0; ok && i < n; ++i) {
			Note    note;
			int64_t s, e, os, oe;
			ok = r.get (s) && r.get (e) && r.get (os) && r.get (oe) && r.get (note.pitch) && r.get (note.shift)
			     && r.get (note.drift) && r.get (note.gain) && r.get (note.formant) && r.get (note.trans);
			note.start  = s;
			note.end    = e;
			note.ostart = os;
			note.oend   = oe;
			if (ok) {
				an.notes.push_back (note);
			}
		}
		ok = ok && r.p == r.e;
	}
	g_free (data);
	if (ok) {
		a = an;
	}
	return ok;
}

static int analyse_audio (SourceRange const& range, Analysis& a, Progress* progress);

int
analyse (SourceRange const& range, Analysis& a, Progress* progress)
{
	if (!range.cache_file.empty () && load_analysis (range.cache_file, range.cache_key, a)) {
		if (progress) {
			progress->set_progress (1.0);
		}
		return 0;
	}
	const int rv = analyse_audio (range, a, progress);
	if (rv == 0 && !range.cache_file.empty () && !(progress && progress->cancelled ())) {
		if (!save_analysis (range.cache_file, range.cache_key, a)) {
			warning << string_compose (_("Edit Notes: could not save the analysis to %1"), range.cache_file) << endmsg;
		}
	}
	return rv;
}

static int
analyse_audio (SourceRange const& range, Analysis& a, Progress* progress)
{
	using namespace Vamp;
	using namespace Vamp::HostExt;

	const float sr = range.sr;
	a = Analysis ();
	a.sr     = sr;
	a.origin = range.start;
	a.length = range.len;

	vector<vector<float> > ch;
	if (!read_master (range, ch) || ch.empty ()) {
		error << _("Edit Notes: could not read the audio of the region") << endmsg;
		return -1;
	}
	vector<float> mono (a.length, 0.f);
	for (auto const& c : ch) {
		for (samplecnt_t i = 0; i < a.length; ++i) {
			mono[i] += c[i] / ch.size ();
		}
	}
	/* progress: reading 0 .. 5 %, pitch tracking .. 85 %, the note search (getRemainingFeatures) .. 97 %
	 * (the Edit Notes window names the step from these values) */
	if (progress) {
		progress->set_progress (0.05);
	}

	Vamp::Plugin* p = PluginLoader::getInstance ()->loadPlugin ("libardourvamppyin:pyin", sr, PluginLoader::ADAPT_ALL_SAFE);
	if (!p) {
		error << _("Edit Notes: the pitch analysis plugin (pYIN) could not be loaded") << endmsg;
		return -1;
	}
	size_t step  = p->getPreferredStepSize ();
	size_t block = p->getPreferredBlockSize ();
	if (step == 0) {
		step = 256;
	}
	if (block == 0) {
		block = 2048;
	}
	if (!p->initialise (1, step, block)) {
		delete p;
		error << _("Edit Notes: the pitch analysis could not start") << endmsg;
		return -1;
	}
	a.hop = step;

	int track_out = -1, notes_out = -1;
	Vamp::Plugin::OutputList outs = p->getOutputDescriptors ();
	for (size_t i = 0; i < outs.size (); ++i) {
		if (outs[i].identifier == "smoothedpitchtrack") {
			track_out = i;
		} else if (outs[i].identifier == "notes") {
			notes_out = i;
		}
	}

	const size_t frames = a.length / step + 1;
	a.midi.assign (frames, 0.f);
	a.rms.assign (frames, 0.f);

	vector<float> buf (block);
	Vamp::Plugin::FeatureSet all;
	for (samplecnt_t pos = 0; pos < a.length; pos += step) {
		const samplecnt_t n = std::min ((samplecnt_t) block, a.length - pos);
		std::fill (buf.begin (), buf.end (), 0.f);
		std::copy (mono.begin () + pos, mono.begin () + pos + n, buf.begin ());
		const float* bp = &buf[0];
		Vamp::Plugin::FeatureSet fs = p->process (&bp, RealTime::frame2RealTime (pos, sr));
		for (auto& f : fs) {
			all[f.first].insert (all[f.first].end (), f.second.begin (), f.second.end ());
		}
		if (progress && (pos / step) % 64 == 0) {
			if (progress->cancelled ()) {
				delete p;
				return -1;
			}
			progress->set_progress (0.05 + 0.8 * pos / (double) a.length);
		}
	}
	if (progress) {
		progress->set_progress (0.85);
	}
	Vamp::Plugin::FeatureSet rest = p->getRemainingFeatures ();
	if (progress) {
		progress->set_progress (0.97);
	}
	for (auto& f : rest) {
		all[f.first].insert (all[f.first].end (), f.second.begin (), f.second.end ());
	}
	delete p;

	/* level per frame (window of one hop, centred like pYIN's frames) */
	float peak = 1e-9f;
	for (size_t f = 0; f < frames; ++f) {
		const samplepos_t s = f * step;
		const samplepos_t e = std::min ((samplepos_t) a.length, s + (samplepos_t) step * 4);
		double sum = 0;
		for (samplepos_t i = s; i < e; ++i) {
			sum += mono[i] * mono[i];
		}
		a.rms[f] = e > s ? std::sqrt (sum / (e - s)) : 0.f;
		peak     = std::max (peak, a.rms[f]);
	}
	for (auto& v : a.rms) {
		v /= peak;
	}

	if (track_out >= 0) {
		for (auto const& f : all[track_out]) {
			if (f.values.empty () || f.values[0] <= 0.f) {
				continue;
			}
			const long fr = lrint (RealTime::realTime2Frame (f.timestamp, sr) / (double) step);
			if (fr >= 0 && fr < (long) frames) {
				a.midi[fr] = hz_to_midi (f.values[0]);
			}
		}
	}

	if (notes_out >= 0) {
		for (auto const& f : all[notes_out]) {
			if (f.values.empty () || !f.hasDuration) {
				continue;
			}
			Note n;
			/* pYIN's frames are centred, so the first note may start a little before 0 */
			const samplepos_t s = std::max ((samplepos_t) 0, (samplepos_t) RealTime::realTime2Frame (f.timestamp, sr));
			const samplepos_t e = std::min ((samplepos_t) a.length, (samplepos_t) (RealTime::realTime2Frame (f.timestamp, sr) + RealTime::realTime2Frame (f.duration, sr)));
			if (e - s < sr * 0.04) {
				continue; /* blips */
			}
			n.start = a.origin + s;
			n.end   = a.origin + e;
			n.ostart = n.start;
			n.oend   = n.end;
			/* the pitch centre: the median of the pitch track inside the note (pYIN's note value is rounded) */
			n.pitch = median_pitch (a, n.start, n.end, hz_to_midi (f.values[0]));
			a.notes.push_back (n);
		}
	}

	if (progress) {
		progress->set_progress (1.0);
	}
	return 0;
}

static const char* const kNode = "JXNoteEdit";

XMLNode*
notes_to_xml (vector<Note> const& notes)
{
	XMLNode* node = new XMLNode (kNode);
	for (auto const& n : notes) {
		XMLNode* c = new XMLNode ("Note");
		c->set_property ("start", n.start);
		c->set_property ("end", n.end);
		c->set_property ("drift", n.drift);
		c->set_property ("gain", n.gain);
		c->set_property ("formant", n.formant);
		c->set_property ("ostart", n.ostart);
		c->set_property ("oend", n.oend);
		c->set_property ("pitch", n.pitch);
		c->set_property ("shift", n.shift);
		if (n.trans_edited ()) {
			c->set_property ("trans", n.trans);
		}
		node->add_child_nocopy (*c);
	}
	return node;
}

bool
load_notes (std::shared_ptr<Region const> r, vector<Note>& notes)
{
	notes.clear ();
	XMLNode* node = std::const_pointer_cast<Region> (r)->extra_xml (kNode);
	if (!node) {
		return false;
	}
	for (auto const& c : node->children ("Note")) {
		Note n;
		if (c->get_property ("start", n.start) && c->get_property ("end", n.end) && c->get_property ("pitch", n.pitch)) {
			c->get_property ("shift", n.shift);
			c->get_property ("drift", n.drift);
			c->get_property ("gain", n.gain);
			c->get_property ("formant", n.formant);
			c->get_property ("trans", n.trans);
			if (!c->get_property ("ostart", n.ostart) || !c->get_property ("oend", n.oend)) {
				n.ostart = n.start;
				n.oend   = n.end;
			}
			notes.push_back (n);
		}
	}
	return !notes.empty ();
}

/** One span of all channels pitch-shifted; same length and timing as the input. `ratio_at` gives the ratio at a
 *  position inside the span (0 .. len); it is set per block (realtime mode follows changes), 256 samples when it
 *  varies, so a glide moves in ~5 ms steps that the stretcher smooths. */
static void
shift_span (vector<vector<float> > const& in, samplepos_t from, samplecnt_t len, samplecnt_t sr,
            std::function<double (samplecnt_t)> ratio_at, bool varying, double formant_semitones, vector<vector<float> >& out)
{
	using namespace RubberBand;
	const size_t nch = in.size ();
	out.assign (nch, vector<float> ());

	RubberBandStretcher rb (sr, nch,
	                        RubberBandStretcher::OptionProcessRealTime | RubberBandStretcher::OptionEngineFiner |
	                        RubberBandStretcher::OptionFormantPreserved | RubberBandStretcher::OptionChannelsTogether);
	auto set_ratio = [&] (double ratio) {
		rb.setPitchScale (ratio);
		if (formant_semitones != 0) {
			/* preserved formants would be scaled by 1 / ratio; move them on top of that */
			rb.setFormantScale (std::pow (2.0, formant_semitones / 12.0) / ratio);
		}
	};
	set_ratio (ratio_at (0));

	const int        block = varying ? 256 : 1024;
	vector<float*>   ptrs (nch);
	vector<vector<float> > tmp (nch, vector<float> (std::max (block, (int) rb.getPreferredStartPad ())));
	vector<const float*> cptrs (nch);

	const int pad = rb.getPreferredStartPad ();
	if (pad > 0) {
		for (size_t c = 0; c < nch; ++c) {
			std::fill (tmp[c].begin (), tmp[c].begin () + pad, 0.f);
			cptrs[c] = &tmp[c][0];
		}
		rb.process (&cptrs[0], pad, false);
	}
	samplecnt_t skip = rb.getStartDelay ();

	vector<vector<float> > got (nch, vector<float> (8192));
	auto pull = [&] () {
		int av;
		while ((av = rb.available ()) > 0) {
			const int n = std::min (av, 8192);
			for (size_t c = 0; c < nch; ++c) {
				ptrs[c] = &got[c][0];
			}
			rb.retrieve (&ptrs[0], n);
			int s = 0;
			if (skip > 0) {
				s = std::min ((samplecnt_t) n, skip);
				skip -= s;
			}
			for (size_t c = 0; c < nch; ++c) {
				out[c].insert (out[c].end (), got[c].begin () + s, got[c].begin () + n);
			}
		}
	};

	for (samplecnt_t pos = 0; pos < len; pos += block) {
		const samplecnt_t n = std::min ((samplecnt_t) block, len - pos);
		if (varying) {
			set_ratio (ratio_at (pos + n / 2));
		}
		for (size_t c = 0; c < nch; ++c) {
			cptrs[c] = &in[c][from + pos];
		}
		rb.process (&cptrs[0], n, pos + n >= len);
		pull ();
	}
	/* the tail: R3 keeps a little back until the final block was processed; feed silence if it is still short */
	for (int guard = 0; (samplecnt_t) out[0].size () < len && guard < 64; ++guard) {
		for (size_t c = 0; c < nch; ++c) {
			std::fill (tmp[c].begin (), tmp[c].begin () + block, 0.f);
			cptrs[c] = &tmp[c][0];
		}
		if (rb.available () < 0) {
			break;
		}
		pull ();
	}
	for (size_t c = 0; c < nch; ++c) {
		out[c].resize (len, 0.f);
	}
}

static void
shift_span (vector<vector<float> > const& in, samplepos_t from, samplecnt_t len, samplecnt_t sr,
            double ratio, double formant_semitones, vector<vector<float> >& out)
{
	shift_span (in, from, len, sr, [ratio] (samplecnt_t) { return ratio; }, false, formant_semitones, out);
}

/** Windowed-sinc read of `x` at a fractional position */
static inline float
read_frac (vector<float> const& x, double pos)
{
	static const int taps = 8; /* per side */
	const long       i0   = (long) std::floor (pos);
	const double     fr   = pos - i0;
	double           acc  = 0;
	for (int k = -taps + 1; k <= taps; ++k) {
		const long j = i0 + k;
		if (j < 0 || j >= (long) x.size ()) {
			continue;
		}
		const double d = k - fr;
		const double w = d == 0 ? 1.0 : std::sin (M_PI * d) / (M_PI * d) * std::sin (M_PI * d / taps) / (M_PI * d / taps);
		acc += x[j] * w;
	}
	return acc;
}

/** Flatten the drift of one note inside `buf` (samples a .. b): read it at a varying rate, so the pitch follows
 *  `cents (i)`. The rates are normalised to keep a .. b in place; the time error inside stays at a few ms. */
static void
bend_span (vector<vector<float> >& buf, samplepos_t a, samplepos_t b, std::function<double (samplepos_t)> cents)
{
	const samplecnt_t len = b - a;
	if (len < 2) {
		return;
	}
	vector<double> tau (len + 1, 0.0);
	for (samplecnt_t i = 0; i < len; ++i) {
		tau[i + 1] = tau[i] + std::pow (2.0, cents (i) / 1200.0);
	}
	const double norm = len / tau[len];
	for (size_t c = 0; c < buf.size (); ++c) {
		const vector<float> src (buf[c]);
		for (samplecnt_t i = 0; i < len; ++i) {
			buf[c][a + i] = read_frac (src, a + tau[i] * norm);
		}
	}
}

/** Time-stretch [a, b) of `in` so the key points (src -> dst, relative to `in`, both ends identity) are met;
 *  written into `out` with 10 ms crossfades at a and b (the audio there is not moved, so it lines up) */
static void
warp_span (vector<vector<float> > const& in, samplecnt_t sr, vector<std::pair<samplepos_t, samplepos_t> > const& pts,
           vector<vector<float> >& out)
{
	using namespace RubberBand;
	const size_t      nch    = in.size ();
	const samplecnt_t len    = in[0].size ();
	const samplepos_t a      = pts.front ().first;
	const samplepos_t b      = pts.back ().first;
	const samplecnt_t margin = sr / 10;
	const samplepos_t s      = std::max ((samplepos_t) 0, a - margin);
	const samplepos_t e      = std::min (len, b + margin);
	const samplecnt_t n      = e - s;

	RubberBandStretcher rb (sr, nch,
	                        RubberBandStretcher::OptionProcessOffline | RubberBandStretcher::OptionEngineFiner |
	                        RubberBandStretcher::OptionFormantPreserved | RubberBandStretcher::OptionChannelsTogether);
	rb.setExpectedInputDuration (n);
	rb.setTimeRatio (1.0);
	std::map<size_t, size_t> km;
	for (auto const& p : pts) {
		if (p.first > s && p.first < e) {
			km[p.first - s] = p.second - s;
		}
	}
	rb.setKeyFrameMap (km);

	const samplecnt_t      block = 4096;
	vector<const float*>   cp (nch);
	for (samplepos_t pos = 0; pos < n; pos += block) {
		const samplecnt_t k = std::min (block, n - pos);
		for (size_t c = 0; c < nch; ++c) {
			cp[c] = &in[c][s + pos];
		}
		rb.study (&cp[0], k, pos + k >= n);
	}
	vector<vector<float> > got (nch, vector<float> (8192)), res (nch);
	vector<float*>         op (nch);
	auto pull = [&] () {
		int av;
		while ((av = rb.available ()) > 0) {
			const int m = std::min (av, 8192);
			for (size_t c = 0; c < nch; ++c) {
				op[c] = &got[c][0];
			}
			const size_t r = rb.retrieve (&op[0], m);
			for (size_t c = 0; c < nch; ++c) {
				res[c].insert (res[c].end (), got[c].begin (), got[c].begin () + r);
			}
		}
	};
	for (samplepos_t pos = 0; pos < n; pos += block) {
		const samplecnt_t k = std::min (block, n - pos);
		for (size_t c = 0; c < nch; ++c) {
			cp[c] = &in[c][s + pos];
		}
		rb.process (&cp[0], k, pos + k >= n);
		pull ();
	}
	pull ();
	for (size_t c = 0; c < nch; ++c) {
		res[c].resize (n, 0.f);
	}

	const samplecnt_t xfade = std::max ((samplecnt_t) 32, sr / 100);
	const double      a0    = a - xfade / 2.0;
	const double      b1    = b + xfade / 2.0;
	const samplepos_t ws    = std::max (s, (samplepos_t) std::floor (a0));
	const samplepos_t we    = std::min (e, (samplepos_t) std::ceil (b1));
	for (samplepos_t i = ws; i < we; ++i) {
		const double w = std::max (0.0, std::min (1.0, std::min ((i - a0) / xfade, (b1 - i) / xfade)));
		for (size_t c = 0; c < nch; ++c) {
			out[c][i] = out[c][i] * (1.0 - w) + res[c][i - s] * w;
		}
	}
}

/** Move the notes in time: stretch only where the source -> output map is not the identity */
static void
time_warp (vector<vector<float> > const& in, samplepos_t origin, samplecnt_t sr, vector<Note> const& notes,
           vector<vector<float> >& out)
{
	out = in;
	const samplecnt_t len = in[0].size ();

	typedef std::pair<samplepos_t, samplepos_t> Pt;
	vector<Pt> raw;
	raw.push_back (Pt (0, 0));
	for (auto const& n : notes) {
		raw.push_back (Pt (n.start - origin, n.ostart - origin));
		raw.push_back (Pt (n.end - origin, n.oend - origin));
	}
	raw.push_back (Pt (len, len));
	std::sort (raw.begin (), raw.end ());
	vector<Pt> pts;
	for (auto const& p : raw) {
		if (p.first < 0 || p.first > len || p.second < 0 || p.second > len) {
			continue;
		}
		if (pts.empty () || (p.first > pts.back ().first && p.second > pts.back ().second)) {
			pts.push_back (p);
		}
	}

	/* spans between two identity points with moved points in between */
	size_t i = 0;
	while (i + 1 < pts.size ()) {
		size_t j = i + 1;
		while (j < pts.size () && pts[j].first != pts[j].second) {
			++j;
		}
		if (j >= pts.size ()) {
			break;
		}
		if (j > i + 1) {
			warp_span (in, sr, vector<Pt> (pts.begin () + i, pts.begin () + j + 1), out);
		}
		i = j;
	}
}

static inline double
smoothstep (double t)
{
	t = std::max (0.0, std::min (1.0, t));
	return t * t * (3 - 2 * t);
}

vector<Glide>
glides (vector<Note> const& notes, samplecnt_t sr)
{
	const samplecnt_t near = sr * 3 / 10; /* closer notes are joined by a glide */
	const samplecnt_t tail = sr * 15 / 100;

	vector<Note const*> v;
	for (auto const& n : notes) {
		v.push_back (&n);
	}
	std::sort (v.begin (), v.end (), [] (Note const* a, Note const* b) { return a->ostart < b->ostart; });

	auto moved = [] (Note const* n) { return n->shift > 0.001f || n->shift < -0.001f || n->formant > 0.001f || n->formant < -0.001f; };
	auto mid   = [] (Note const* n) { return (n->ostart + n->oend) / 2; };

	vector<Glide> g;
	for (size_t i = 0; i < v.size (); ++i) {
		Note const* n    = v[i];
		Note const* prev = i > 0 ? v[i - 1] : 0;
		Note const* next = i + 1 < v.size () ? v[i + 1] : 0;
		const bool  join_prev = prev && n->ostart >= prev->oend && n->ostart - prev->oend < near;
		const bool  join_next = next && next->ostart >= n->oend && next->ostart - n->oend < near;

		if (join_next) {
			if (moved (n) || moved (next) || next->trans_edited ()) {
				const samplecnt_t in_n    = std::min (sr * 8 / 100, (n->oend - n->ostart) / 4);
				const samplecnt_t in_next = std::min (sr * 8 / 100, (next->oend - next->ostart) / 4);
				Glide x;
				x.a       = n->oend - in_n;
				x.b       = next->ostart + in_next;
				x.s0      = n->shift;
				x.s1      = next->shift;
				x.formant = (n->formant + next->formant) / 2;
				x.open    = 0;
				x.trans   = next->trans_edited () ? next->trans : 0.f;
				x.c       = (n->oend + next->ostart) / 2;
				x.na      = *n;
				x.nb      = *next;
				if (x.trans > 0) {
					/* the zone covers the whole transition, but not more than half of either note */
					const samplecnt_t half = (samplecnt_t) (x.trans * sr / 2) + sr / 100;
					x.a = std::max (std::min (x.a, x.c - half), mid (n));
					x.b = std::min (std::max (x.b, x.c + half), mid (next));
				}
				g.push_back (x);
			}
		} else if (moved (n) && !(next && next->ostart < n->oend)) {
			/* the tail after the note, up to half way to the next one */
			Glide x;
			x.a = n->oend;
			x.b = n->oend + (next ? std::min (tail, (next->ostart - n->oend) / 2) : tail);
			x.s0 = x.s1 = n->shift;
			x.formant   = n->formant;
			x.open      = 1;
			x.trans     = 0;
			x.c         = x.a;
			x.na = x.nb = *n;
			g.push_back (x);
		}
		if (!join_prev && moved (n) && !(prev && prev->oend > n->ostart)) {
			/* the onset before the note (a scoop into it) */
			Glide x;
			x.a = n->ostart - (prev ? std::min (tail, (n->ostart - prev->oend) / 2) : tail);
			x.b = n->ostart;
			x.s0 = x.s1 = n->shift;
			x.formant   = n->formant;
			x.open      = -1;
			x.trans     = 0;
			x.c         = x.b;
			x.na = x.nb = *n;
			g.push_back (x);
		}
	}
	return g;
}

double
glide_weight (Glide const& g, double pos, samplecnt_t sr)
{
	const double xfade = std::max ((samplecnt_t) 32, sr / 100);
	const double half  = std::max (1.0, (g.b - g.a) / 2.0);
	double       l, r;
	if (g.open < 0) {
		l = (pos - g.a) / half;
	} else {
		l = (pos - (g.a - xfade / 2)) / xfade;
	}
	if (g.open > 0) {
		r = (g.b - pos) / half;
	} else {
		r = ((g.b + xfade / 2) - pos) / xfade;
	}
	return std::max (0.0, std::min (1.0, std::min (l, r)));
}

/* the glided shift itself (semitones) at a position between a and b */
static double
glide_curve (Glide const& g, double pos)
{
	if (g.b <= g.a) {
		return g.s1;
	}
	return g.s0 + (g.s1 - g.s0) * smoothstep ((pos - g.a) / (double) (g.b - g.a));
}

/* sung pitch (MIDI note, 0 = unvoiced) at a master-source position, between analysis frames */
static double
pitch_at (Analysis const& an, double src)
{
	if (an.midi.empty ()) {
		return 0;
	}
	const double f  = (src - an.origin) / an.hop;
	const long   f0 = (long) std::floor (f);
	if (f0 < 0 || f0 + 1 >= (long) an.midi.size ()) {
		return 0;
	}
	const float a = an.midi[f0], b = an.midi[f0 + 1];
	if (a <= 0 || b <= 0) {
		return a > 0 ? a : b;
	}
	return a + (b - a) * (f - f0);
}

double
glide_offset (Glide const& g, double pos, Analysis const* an)
{
	const double plain = glide_curve (g, pos);
	if (g.trans <= 0 || !an) {
		return plain;
	}
	/* where this output position was sung: inside a note its own map, between the two notes the gap
	 * is mapped linearly (the same key points as the time warp) */
	Note const& A = g.na;
	Note const& B = g.nb;
	double      src;
	if (pos < A.oend) {
		src = A.to_src (pos);
	} else if (pos >= B.ostart) {
		src = B.to_src (pos);
	} else {
		const double t = B.ostart > A.oend ? (pos - A.oend) / (double) (B.ostart - A.oend) : 0;
		src = A.end + t * (B.start - A.end);
	}
	const double sung = pitch_at (*an, src);

	const double half = std::max (1.0, g.trans * an->sr / 2.0);
	const double t    = (pos - (g.c - half)) / (2 * half); /* 0 .. 1 across the transition */
	const double lin  = g.s0 + (g.s1 - g.s0) * smoothstep (t);
	if (sung <= 0 || t <= 0 || t >= 1) {
		return lin;
	}
	/* the new pitch: an S-curve from one target to the other; at the window edges it fades into the
	 * sung pitch (plus the note's shift), so vibrato does not jump there */
	const double want = (A.pitch + A.shift) + ((B.pitch + B.shift) - (A.pitch + A.shift)) * smoothstep (t);
	const double edge = std::min (1.0, std::min (t, 1 - t) / 0.25);
	return lin + smoothstep (edge) * (want - sung - lin);
}

bool
glide_at (vector<Glide> const& gl, double pos, samplecnt_t sr, double& shift, double& weight, Analysis const* an)
{
	for (auto const& g : gl) {
		const double x = std::max ((samplecnt_t) 32, sr / 100) / 2.0;
		if (pos >= g.a - (g.open < 0 ? 0 : x) && pos < g.b + (g.open > 0 ? 0 : x)) {
			shift  = glide_offset (g, pos, an);
			weight = glide_weight (g, pos, sr);
			return true;
		}
	}
	return false;
}

void
render (vector<vector<float> > const& raw_in, samplepos_t origin, samplecnt_t sr, vector<Note> const& notes,
        Analysis const& an, vector<vector<float> >& out, Progress* progress)
{
	if (raw_in.empty ()) {
		out = raw_in;
		return;
	}
	const vector<Glide> gls = glides (notes, sr);
	/* progress: the time edit, then every glide and every re-pitched note (each one a Rubber Band run) */
	size_t total = 1 + gls.size (), done = 0;
	for (auto const& n : notes) {
		total += n.pitch_edited () ? 1 : 0;
	}
	auto step = [&] () {
		if (progress) {
			progress->set_progress (++done / (float) total);
		}
		return !(progress && progress->cancelled ());
	};

	vector<vector<float> > in;
	time_warp (raw_in, origin, sr, notes, in);
	out = in;
	if (!step ()) {
		return;
	}
	const samplecnt_t len    = in[0].size ();
	const samplecnt_t xfade  = std::max ((samplecnt_t) 32, sr / 100); /* 10 ms */
	const samplecnt_t margin = sr / 5;                                 /* context for the stretcher */

	/* every re-pitched piece is added with its weight; where the weights reach 1 the untouched audio is not used,
	 * so a note and the glide next to it cross over without the original pitch in between */
	vector<vector<float> > acc (out.size (), vector<float> (len, 0.f));
	vector<float>          wsum (len, 0.f);

	vector<float>          joined (len, 0.f); /* weight of glides between two notes: the notes give way there */

	vector<vector<float> > shifted;
	for (auto const& g : gls) {
		if (!step ()) {
			return;
		}
		const samplepos_t ga = g.a - origin, gb = g.b - origin;
		if (gb <= 0 || ga >= len || gb <= ga) {
			continue;
		}
		const samplepos_t s = std::max ((samplepos_t) 0, ga - margin);
		const samplepos_t e = std::min (len, gb + margin);
		/* the slow part (from one note's shift to the next one's) is pitch-shifted, a new transition on top
		 * of it is bent in: quicker than Rubber Band follows a changing ratio */
		shift_span (in, s, e - s, sr, [&] (samplecnt_t p) { return std::pow (2.0, glide_curve (g, origin + s + p) / 12.0); },
		            g.s0 != g.s1, g.formant, shifted);
		if (g.trans > 0) {
			const samplepos_t ba = std::max (ga, s);
			const samplepos_t bb = std::min (gb, e);
			bend_span (shifted, ba - s, bb - s, [&] (samplepos_t i) {
				const double pos = origin + ba + i;
				return 100.0 * (glide_offset (g, pos, &an) - glide_curve (g, pos));
			});
		}
		const samplepos_t ws = std::max (s, ga - xfade);
		const samplepos_t we = std::min (e, gb + xfade);
		for (samplepos_t i = ws; i < we; ++i) {
			const double w = glide_weight (g, origin + i, sr);
			if (w <= 0) {
				continue;
			}
			for (size_t c = 0; c < out.size (); ++c) {
				acc[c][i] += w * shifted[c][i - s];
			}
			wsum[i] += w;
			if (g.open == 0) {
				joined[i] = std::max (joined[i], (float) w);
			}
		}
	}

	for (auto const& n : notes) {
		if (!n.pitch_edited ()) {
			continue;
		}
		if (!step ()) {
			return;
		}
		samplepos_t   df0 = 0;
		vector<float> dev;
		if (n.drift > 0.001f) {
			dev = drift_curve (an, n, df0);
		}
		/* after the time edit the note plays at ostart .. oend */
		const samplepos_t ns = n.ostart - origin;
		const samplepos_t ne = n.oend - origin;
		if (ne <= 0 || ns >= len) {
			continue;
		}
		const samplepos_t s = std::max ((samplepos_t) 0, ns - margin);
		const samplepos_t e = std::min (len, ne + margin);
		shift_span (in, s, e - s, sr, std::pow (2.0, n.shift / 12.0), n.formant, shifted);
		if (!dev.empty ()) {
			/* drift: bend the note against its slow wandering (interpolated between analysis frames) */
			const samplepos_t a = std::max (s, ns) - s, b = std::min (e, ne) - s;
			bend_span (shifted, a, b, [&] (samplepos_t i) {
				const double src = (n.to_src (origin + s + a + i) - an.origin) / an.hop - df0;
				const long   f   = (long) std::floor (src);
				const double fr  = src - f;
				auto at = [&] (long k) { return dev[std::max (0L, std::min ((long) dev.size () - 1, k))]; };
				return -100.0 * n.drift * (at (f) * (1 - fr) + at (f + 1) * fr);
			});
		}

		/* weight: 1 inside the note, linear ramps of `xfade` centred on its edges */
		const double a0 = ns - xfade / 2.0;
		const double b1 = ne + xfade / 2.0;
		const samplepos_t ws = std::max (s, (samplepos_t) std::floor (a0));
		const samplepos_t we = std::min (e, (samplepos_t) std::ceil (b1));
		for (samplepos_t i = ws; i < we; ++i) {
			const double w = std::max (0.0, std::min (1.0, std::min ((i - a0) / xfade, (b1 - i) / xfade))) * (1.0 - joined[i]);
			for (size_t c = 0; c < out.size (); ++c) {
				acc[c][i] += w * shifted[c][i - s];
			}
			wsum[i] += w;
		}
	}
	for (samplecnt_t i = 0; i < len; ++i) {
		const float w = wsum[i];
		if (w <= 0.f) {
			continue;
		}
		for (size_t c = 0; c < out.size (); ++c) {
			out[c][i] = w >= 1.f ? acc[c][i] / w : acc[c][i] + in[c][i] * (1.f - w);
		}
	}

	/* level per note, with the same ramps on its edges */
	for (auto const& n : notes) {
		if (!n.gain_edited ()) {
			continue;
		}
		const double      g  = std::pow (10.0, n.gain / 20.0);
		const double      a0 = n.ostart - origin - xfade / 2.0;
		const double      b1 = n.oend - origin + xfade / 2.0;
		const samplepos_t ws = std::max ((samplepos_t) 0, (samplepos_t) std::floor (a0));
		const samplepos_t we = std::min (len, (samplepos_t) std::ceil (b1));
		for (samplepos_t i = ws; i < we; ++i) {
			const double w = std::max (0.0, std::min (1.0, std::min ((i - a0) / xfade, (b1 - i) / xfade)));
			for (size_t c = 0; c < out.size (); ++c) {
				out[c][i] *= 1.0 + w * (g - 1.0);
			}
		}
	}
}

int
render_range (SourceRange const& range, vector<Note> const& notes, Analysis const& an,
              vector<vector<float> >& out, Progress* progress)
{
	vector<vector<float> > in;
	if (!read_master (range, in)) {
		error << _("Edit Notes: could not read the audio of the region") << endmsg;
		return -1;
	}
	render (in, range.start, range.sr, notes, an, out, progress);
	if (progress && progress->cancelled ()) {
		return -1;
	}
	if (out.size () != in.size () || (!out.empty () && out[0].size () != (size_t) range.len)) {
		error << _("Edit Notes: the edited audio has the wrong length") << endmsg;
		return -1;
	}
	return 0;
}

bool
can_render (std::shared_ptr<AudioRegion> ar)
{
	if (ar->stretch () != 1.f || ar->shift () != 1.f) {
		error << _("Edit Notes: undo the time stretch / pitch shift of this region first") << endmsg;
		return false;
	}
	return true;
}

int
render_region (std::shared_ptr<AudioRegion> ar, vector<Note> const& notes, Analysis const& an,
               vector<vector<float> >& out, Progress* progress)
{
	SourceRange range;
	if (!can_render (ar) || !source_range (ar, range)) {
		return -1;
	}
	return render_range (range, notes, an, out, progress);
}

int
Filter::run (std::shared_ptr<Region> region, Progress* progress)
{
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		return -1;
	}

	samplepos_t start;
	samplecnt_t len;
	region_master_range (ar, start, len);

	vector<vector<float> > out;
	out.swap (_rendered);
	if (out.empty () || out[0].size () != (size_t) len) {
		if (render_region (ar, _notes, _an, out)) {
			return -1;
		}
	}
	if (progress) {
		progress->set_progress (0.8);
	}

	SourceList nsrcs;
	if (make_new_sources (region, nsrcs, "")) {
		return -1;
	}
	int ret = 0;
	for (size_t c = 0; c < nsrcs.size () && c < out.size (); ++c) {
		std::shared_ptr<AudioSource> as = std::dynamic_pointer_cast<AudioSource> (nsrcs[c]);
		const samplecnt_t chunk = 65536;
		for (samplecnt_t pos = 0; as && pos < len; pos += chunk) {
			const samplecnt_t n = std::min (chunk, len - pos);
			if (as->write (&out[c][pos], n) != n) {
				error << _("Edit Notes: could not write the edited audio") << endmsg;
				ret = -1;
				break;
			}
		}
	}

	if (ret == 0) {
		ret = finish (region, nsrcs, region->name ());
	}
	if (ret) {
		for (auto& s : nsrcs) {
			s->mark_for_remove ();
		}
		return ret;
	}

	for (auto& r : results) {
		r->set_ancestral_data (timepos_t (start), timecnt_t (len, timepos_t (start)), 1.f, 1.f);
		r->set_master_sources (region->master_sources ());
		r->set_whole_file (true);
		r->add_extra_xml (*notes_to_xml (_notes));
	}
	if (progress) {
		progress->set_progress (1.0);
	}
	return 0;
}

} // namespace NoteEdit
