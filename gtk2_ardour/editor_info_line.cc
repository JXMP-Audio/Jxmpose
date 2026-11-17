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

/* Info line (studio look): a dark strip below the editor toolbar that shows
 * name, start, end and length of the selected region(s).
 */

#include <ytkmm/eventbox.h>
#include <ytkmm/label.h>

#include "pbd/compose.h"

#include "ardour/region.h"
#include "ardour/session.h"

#include "gtkmm2ext/cairo_theme.h"
#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/utils.h"

#include "audio_clock.h"
#include "editor.h"
#include "gui_thread.h"
#include "region_view.h"
#include "selection.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace Gtk;
using namespace Temporal;

static Gtk::Label*
info_line_title (std::string const& txt)
{
	Gtk::Label* l = manage (new Gtk::Label);
	l->set_markup (string_compose ("<span foreground=\"#8A8D90\">%1</span>", Gtkmm2ext::markup_escape_text (txt)));
	return l;
}

Gtk::Widget*
Editor::setup_info_line ()
{
	if (!Gtkmm2ext::CairoTheme::studio_widgets ()) {
		return 0;
	}

	info_start_clock  = manage (new AudioClock (X_("infoline-start"), true, X_("nudge"), false, false, false, false));
	info_end_clock    = manage (new AudioClock (X_("infoline-end"), true, X_("nudge"), false, false, false, false));
	info_length_clock = manage (new AudioClock (X_("infoline-length"), true, X_("nudge"), false, false, true, false));

	info_start_clock->set_mode (AudioClock::BBT);
	info_end_clock->set_mode (AudioClock::BBT);
	info_length_clock->set_mode (AudioClock::BBT);

	info_line_name.set_alignment (0, 0.5);
	info_line_name.set_size_request (160, -1);
	info_line_name.set_ellipsize (Pango::ELLIPSIZE_END);
	info_line_empty.set_markup (string_compose ("<span foreground=\"#8A8D90\">%1</span>", _("No object selected")));

	info_line_fields.set_spacing (6);
	info_line_fields.pack_start (*info_line_title (_("Name")), false, false);
	info_line_fields.pack_start (info_line_name, false, false, 4);
	info_line_fields.pack_start (*info_line_title (_("Start")), false, false);
	info_line_fields.pack_start (*info_start_clock, false, false, 4);
	info_line_fields.pack_start (*info_line_title (_("End")), false, false);
	info_line_fields.pack_start (*info_end_clock, false, false, 4);
	info_line_fields.pack_start (*info_line_title (_("Length")), false, false);
	info_line_fields.pack_start (*info_length_clock, false, false, 4);
	info_line_fields.show_all ();
	info_line_fields.set_no_show_all ();

	HBox* hb = manage (new HBox);
	hb->set_border_width (2);
	hb->pack_start (info_line_empty, false, false, 6);
	hb->pack_start (info_line_fields, false, false, 6);

	EventBox* eb = manage (new EventBox);
	eb->modify_bg (STATE_NORMAL, Gtkmm2ext::gdk_color_from_rgba (UIConfiguration::instance ().color ("neutral:backgroundest")));
	eb->add (*hb);
	eb->show_all ();

	selection->RegionsChanged.connect (sigc::mem_fun (*this, &Editor::update_info_line));
	update_info_line ();
	return eb;
}

void
Editor::update_info_line ()
{
	if (!info_start_clock) {
		return;
	}

	RegionSelection const& rs (selection->regions);

	if (rs.empty () || !_session) {
		info_line_fields.hide ();
		info_line_empty.show ();
		return;
	}

	timepos_t start = timepos_t::max (Temporal::AudioTime);
	timepos_t end;
	for (auto const& rv : rs) {
		std::shared_ptr<Region> r = rv->region ();
		start = std::min (start, r->position ());
		end   = std::max (end, r->end_position ());
	}

	if (rs.size () == 1) {
		info_line_name.set_text (rs.front ()->region ()->name ());
	} else {
		info_line_name.set_text (string_compose (_("%1 regions"), rs.size ()));
	}

	info_start_clock->set (start, true);
	info_end_clock->set (end, true);
	info_length_clock->set_duration (start.distance (end), true);

	info_line_empty.hide ();
	info_line_fields.show ();
}
