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
#include <functional>
#include <map>

#include <rubberband/RubberBandStretcher.h>
#include <vamp-hostsdk/PluginLoader.h>

#include "pbd/error.h"
#include "pbd/progress.h"
#include "pbd/xml++.h"

#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/session.h"

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

bool
read_master (std::shared_ptr<AudioRegion> r, samplepos_t start, samplecnt_t len, vector<vector<float> >& out)
{
	const uint32_t nch = r->n_channels ();
	out.assign (nch, vector<float> (len, 0.f));
	/* master_read_at() maps a timeline position to the master sources the same way the region maps it to
	 * its own: source = timeline - position + start */
	const samplepos_t timeline = start - r->start_sample () + r->position_sample ();
	const samplecnt_t chunk    = 65536;
	for (uint32_t c = 0; c < nch; ++c) {
		for (samplecnt_t pos = 0; pos < len; pos += chunk) {
			const samplecnt_t n = std::min (chunk, len - pos);
			if (r->master_read_at (&out[c][pos], timeline + pos, n, c) != n) {
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

int
analyse (std::shared_ptr<AudioRegion> r, Analysis& a, Progress* progress)
{
	using namespace Vamp;
	using namespace Vamp::HostExt;

	const float sr = r->session ().sample_rate ();
	a = Analysis ();
	a.sr = sr;
	region_master_range (r, a.origin, a.length);

	vector<vector<float> > ch;
	if (!read_master (r, a.origin, a.length, ch) || ch.empty ()) {
		error << _("Edit Notes: could not read the audio of the region") << endmsg;
		return -1;
	}
	vector<float> mono (a.length, 0.f);
	for (auto const& c : ch) {
		for (samplecnt_t i = 0; i < a.length; ++i) {
			mono[i] += c[i] / ch.size ();
		}
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
			progress->set_progress (0.9 * pos / (double) a.length);
		}
	}
	Vamp::Plugin::FeatureSet rest = p->getRemainingFeatures ();
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
			if (!c->get_property ("ostart", n.ostart) || !c->get_property ("oend", n.oend)) {
				n.ostart = n.start;
				n.oend   = n.end;
			}
			notes.push_back (n);
		}
	}
	return !notes.empty ();
}

/** One span of all channels shifted by a constant ratio; same length and timing as the input */
static void
shift_span (vector<vector<float> > const& in, samplepos_t from, samplecnt_t len, samplecnt_t sr,
            double ratio, double formant_semitones, vector<vector<float> >& out)
{
	using namespace RubberBand;
	const size_t nch = in.size ();
	out.assign (nch, vector<float> ());

	RubberBandStretcher rb (sr, nch,
	                        RubberBandStretcher::OptionProcessRealTime | RubberBandStretcher::OptionEngineFiner |
	                        RubberBandStretcher::OptionFormantPreserved | RubberBandStretcher::OptionChannelsTogether);
	rb.setPitchScale (ratio);
	if (formant_semitones != 0) {
		/* preserved formants would be scaled by 1 / ratio; move them on top of that */
		rb.setFormantScale (std::pow (2.0, formant_semitones / 12.0) / ratio);
	}

	const int        block = 1024;
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

void
render (vector<vector<float> > const& raw_in, samplepos_t origin, samplecnt_t sr, vector<Note> const& notes,
        Analysis const& an, vector<vector<float> >& out)
{
	if (raw_in.empty ()) {
		out = raw_in;
		return;
	}
	vector<vector<float> > in;
	time_warp (raw_in, origin, sr, notes, in);
	out = in;
	const samplecnt_t len    = in[0].size ();
	const samplecnt_t xfade  = std::max ((samplecnt_t) 32, sr / 100); /* 10 ms */
	const samplecnt_t margin = sr / 5;                                 /* context for the stretcher */

	vector<vector<float> > shifted;
	for (auto const& n : notes) {
		if (!n.pitch_edited ()) {
			continue;
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
			const double w = std::max (0.0, std::min (1.0, std::min ((i - a0) / xfade, (b1 - i) / xfade)));
			for (size_t c = 0; c < out.size (); ++c) {
				out[c][i] = out[c][i] * (1.0 - w) + shifted[c][i - s] * w;
			}
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
Filter::run (std::shared_ptr<Region> region, Progress* progress)
{
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		return -1;
	}
	if (ar->stretch () != 1.f || ar->shift () != 1.f) {
		error << _("Edit Notes: undo the time stretch / pitch shift of this region first") << endmsg;
		return -1;
	}

	samplepos_t start;
	samplecnt_t len;
	region_master_range (ar, start, len);

	vector<vector<float> > in, out;
	if (!read_master (ar, start, len, in)) {
		error << _("Edit Notes: could not read the audio of the region") << endmsg;
		return -1;
	}
	if (progress) {
		progress->set_progress (0.1);
	}
	render (in, start, session.sample_rate (), _notes, _an, out);
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
