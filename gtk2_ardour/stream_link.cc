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
#include <memory>
#include <regex>

#include <glibmm/main.h>

#include "pbd/error.h"

#include "ardour/audioengine.h"
#include "ardour/io.h"
#include "ardour/port.h"
#include "ardour/port_set.h"
#include "ardour/rc_configuration.h"
#include "ardour/route.h"
#include "ardour/session.h"

#include "gui_thread.h"
#include "stream_link.h"

using namespace ARDOUR;
using std::string;
using std::vector;

StreamLink&
StreamLink::instance ()
{
	/* never destroyed: a static instance would drop its signal connections after main() returned,
	 * when Config and the AudioEngine are already gone (crash on quit) */
	static StreamLink* sl = new StreamLink;
	return *sl;
}

StreamLink::StreamLink ()
{
	Config->ParameterChanged.connect (*this, invalidator (*this), std::bind (&StreamLink::config_changed, this, _1), gui_context ());
	AudioEngine::instance ()->PortRegisteredOrUnregistered.connect (*this, invalidator (*this), std::bind (&StreamLink::queue_update, this), gui_context ());
	AudioEngine::instance ()->Running.connect (*this, invalidator (*this), std::bind (&StreamLink::queue_update, this), gui_context ());
}

void
StreamLink::set_session (Session* s)
{
	SessionHandlePtr::set_session (s);
	_io_connections.drop_connections ();
	if (s) {
		/* the click follows Master's outputs (see click_follows_master): wake up when either is reconnected */
		if (s->master_out ()) {
			s->master_out ()->output ()->changed.connect (_io_connections, invalidator (*this), std::bind (&StreamLink::queue_update, this), gui_context ());
		}
		if (s->click_io ()) {
			s->click_io ()->changed.connect (_io_connections, invalidator (*this), std::bind (&StreamLink::queue_update, this), gui_context ());
		}
	}
	queue_update ();
}

void
StreamLink::config_changed (std::string const& p)
{
	if (p == "connect-to-stream" || p == "stream-capture-ports") {
		queue_update ();
	}
}

void
StreamLink::queue_update ()
{
	/* ports come and go in bursts (session load, sharing started): act once they settle */
	_timeout.disconnect ();
	_timeout = Glib::signal_timeout ().connect (sigc::mem_fun (*this, &StreamLink::update), 750);
}

bool
StreamLink::update ()
{
	/* off also means: drop links the session restored from an earlier run */
	if (Config->get_connect_to_stream ()) {
		connect ();
	} else {
		disconnect ();
	}
	click_follows_master ();
	return false;
}

typedef std::pair<string, string> LR;

/** The capture's input ports as left/right pairs.
 *
 * A sharing app may leave several capture sinks behind (one per share);
 * only the one it records from has a "-stream" recorder, so when such a
 * recorder exists only it is used, otherwise every matching pair.
 */
static vector<LR>
capture_ports ()
{
	vector<string> ports;
	AudioEngine::instance ()->get_ports (Config->get_stream_capture_ports (), DataType::AUDIO, IsInput, ports);

	/* never feed ourselves, should the pattern be loose */
	string const own = AudioEngine::instance ()->my_name () + ":";

	vector<LR> all, recorders;
	for (auto const& l : ports) {
		if (l.compare (0, own.size (), own) == 0) {
			continue;
		}
		string::size_type const fl = l.rfind ("_FL");
		if (fl == string::npos || fl + 3 != l.size ()) {
			continue;
		}
		string const r = l.substr (0, fl) + "_FR";
		if (std::find (ports.begin (), ports.end (), r) == ports.end ()) {
			continue;
		}
		LR const lr (l, r);
		all.push_back (lr);
		string const client = l.substr (0, l.find (':'));
		if (client.size () > 7 && client.compare (client.size () - 7, 7, "-stream") == 0) {
			recorders.push_back (lr);
		}
	}
	return recorders.empty () ? all : recorders;
}

/** Master and Click output ports */
static void
our_ports (Session* s, vector<std::shared_ptr<Port>>& master, vector<std::shared_ptr<Port>>& click)
{
	if (s->master_out ()) {
		std::shared_ptr<PortSet const> ps = s->master_out ()->output ()->ports ();
		for (uint32_t n = 0; n < ps->num_ports (DataType::AUDIO); ++n) {
			master.push_back (ps->port (DataType::AUDIO, n));
		}
	}
	if (s->click_io ()) {
		std::shared_ptr<PortSet const> ps = s->click_io ()->ports ();
		for (uint32_t n = 0; n < ps->num_ports (DataType::AUDIO); ++n) {
			click.push_back (ps->port (DataType::AUDIO, n));
		}
	}
}

static void
link (std::shared_ptr<Port> p, string const& dst)
{
	if (p && !p->connected_to (dst)) {
		p->connect (dst);
	}
}

void
StreamLink::connect ()
{
	if (!_session || !AudioEngine::instance ()->running ()) {
		return;
	}

	vector<LR> const dst = capture_ports ();
	if (dst.empty ()) {
		return;
	}

	vector<std::shared_ptr<Port>> master, click;
	our_ports (_session, master, click);

	for (auto const& lr : dst) {
		/* Master left/right to the capture's left/right (a mono Master into both) */
		if (master.size () > 0) {
			link (master[0], lr.first);
			link (master[master.size () > 1 ? 1 : 0], lr.second);
		}
		/* the click is mono: into both */
		for (auto const& c : click) {
			link (c, lr.first);
			link (c, lr.second);
		}
	}
}

void
StreamLink::disconnect ()
{
	if (!_session || !AudioEngine::instance ()->running ()) {
		return;
	}

	std::regex re;
	try {
		re = std::regex (Config->get_stream_capture_ports ());
	} catch (...) {
		return;
	}

	vector<std::shared_ptr<Port>> ports;
	our_ports (_session, ports, ports);

	for (auto const& p : ports) {
		if (!p) {
			continue;
		}
		vector<string> c;
		p->get_connections (c);
		for (auto const& o : c) {
			if (std::regex_search (o, re)) {
				p->disconnect (o);
			}
		}
	}
}

/** The click plays where Master plays when it has no output of its own.
 *
 * At session start Ardour connects the click to the first two physical
 * outputs. A JACK client of PipeWire may see none (the interface is not
 * reported as physical), so the click stayed silent until it was
 * connected by hand. Master's outputs are known then (restored with the
 * session or made by the user / the session manager), so the click goes
 * there too. Connections to the stream capture do not count as outputs.
 */
void
StreamLink::click_follows_master ()
{
	if (!_session || !_session->master_out () || !_session->click_io () || !AudioEngine::instance ()->running ()) {
		return;
	}

	std::regex capture;
	bool       have_capture = false;
	try {
		capture      = std::regex (Config->get_stream_capture_ports ());
		have_capture = !Config->get_stream_capture_ports ().empty ();
	} catch (...) {
	}
	string const own = AudioEngine::instance ()->my_name () + ":";

	/* where a port plays, leaving out the stream capture and our own ports */
	auto outputs = [&] (std::shared_ptr<Port> const& p) {
		vector<string> c, out;
		if (p) {
			p->get_connections (c);
		}
		for (auto const& o : c) {
			if (o.compare (0, own.size (), own) == 0 || o.find (':') == string::npos) {
				continue;
			}
			if (have_capture && std::regex_search (o, capture)) {
				continue;
			}
			out.push_back (o);
		}
		return out;
	};

	vector<std::shared_ptr<Port>> master, click;
	our_ports (_session, master, click);

	for (auto const& c : click) {
		if (!outputs (c).empty ()) {
			return; /* connected by the user (or the physical outputs worked) */
		}
	}
	vector<string> dst;
	for (auto const& m : master) {
		for (auto const& o : outputs (m)) {
			if (std::find (dst.begin (), dst.end (), o) == dst.end ()) {
				dst.push_back (o);
			}
		}
	}
	for (auto const& c : click) {
		for (auto const& o : dst) {
			link (c, o);
		}
	}
}
