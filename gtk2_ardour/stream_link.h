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

#ifndef __gtk2_ardour_stream_link_h__
#define __gtk2_ardour_stream_link_h__

#include <sigc++/connection.h>
#include <sigc++/trackable.h>

#include "pbd/signals.h"

#include "ardour/session_handle.h"

/** Sends Master (and the Click) also to the stream capture of a screen
 * sharing app (Fluxer) when JXMPOSE runs as a JACK client of PipeWire.
 *
 * JXMPOSE plays straight into the audio interface and so bypasses the
 * PipeWire mixer the sharing app records from. With the option
 * "connect-to-stream" on, Master and Click are connected to every input
 * port that matches "stream-capture-ports" as soon as the session runs
 * or the capture shows up (sharing started later). Turning the option
 * off removes these connections again.
 *
 * It also connects the Click to Master's outputs when the click has no
 * output of its own (no physical outputs reported under PipeWire).
 */
class StreamLink : public ARDOUR::SessionHandlePtr, public PBD::ScopedConnectionList, public sigc::trackable
{
public:
	static StreamLink& instance ();

	void set_session (ARDOUR::Session*);

private:
	StreamLink ();

	void config_changed (std::string const&);
	void queue_update ();
	bool update ();
	void connect ();
	void disconnect ();
	void click_follows_master ();

	sigc::connection          _timeout;
	PBD::ScopedConnectionList _io_connections;
};

#endif
