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

#include <glibmm/miscutils.h>

#include <ytkmm/filechooserdialog.h>
#include <ytkmm/stock.h>

#include "pbd/controllable.h"

#include "ardour/automation_control.h"
#include "ardour/lv2_plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/session.h"

#include "gtkmm2ext/keyboard.h"

#include "gui_thread.h"
#include "jx_plugin_ui.h"

/* the drawing recipes of the plugins' own displays (C, header only) */
#include "../libs/plugins/jxmp.lv2/jxmp_draw.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using std::string;

/* ---- panel layouts ---- */

enum JxKind {
	K_MAIN,      /* 15 mm knob, the module's main parameter */
	K_KNOB,      /* standard knob */
	K_SMALL,     /* small knob */
	K_STEP,      /* stepped selector knob (black cap) with a readout */
	K_BUTTON,    /* lit rubber button, toggles */
	K_MOMENTARY, /* rubber button, on while held */
	K_CHOICE,    /* row of rubber buttons, one per value */
	K_LOAD,      /* opens a file (the plugin's path property) */
};

enum JxCap { C_MAIN, C_WHITE, C_GREY, C_BLACK };

struct JxItem {
	const char*              sym;
	const char*              legend;
	JxKind                   kind;
	JxCap                    cap;
	uint32_t                 lit; /* button colour, 0: the product colour */
	/* K_CHOICE: the button legends; K_STEP: readout labels for the values
	 * (an enumeration: in the order of its scale points); K_LOAD: file
	 * patterns, then the loaded file is shown in a VFD next to the button */
	std::vector<const char*> choices;
};

struct JxSection {
	string              title;
	std::vector<JxItem> items;
};

typedef std::vector<JxSection> JxRow;

struct JxLayout {
	const char*        uri;
	uint32_t           product;
	std::vector<JxRow> rows;
	int                display_max; /* height of the plugin's display, 0: 220 px */
	/* plugins without an inline display: the panel draws the title band */
	const char* title;
	const char* subtitle;
	const char* brand; /* footer, 0: JXMP AUDIO */
};

#define C_REC  0xE83A30
#define C_PLAY 0x3CD06E
#define C_SOLO 0xFF8A1C

static JxItem
knob (const char* sym, const char* legend, JxCap cap = C_WHITE)
{
	return JxItem { sym, legend, K_KNOB, cap, 0, {} };
}

static JxItem
main_knob (const char* sym, const char* legend)
{
	return JxItem { sym, legend, K_MAIN, C_MAIN, 0, {} };
}

static JxItem
small (const char* sym, const char* legend, JxCap cap = C_WHITE)
{
	return JxItem { sym, legend, K_SMALL, cap, 0, {} };
}

static JxItem
step (const char* sym, const char* legend, JxCap cap = C_BLACK)
{
	return JxItem { sym, legend, K_STEP, cap, 0, {} };
}

static JxItem
button (const char* sym, const char* legend, uint32_t lit = 0)
{
	return JxItem { sym, legend, K_BUTTON, C_BLACK, lit, {} };
}

static JxItem
momentary (const char* sym, const char* legend, uint32_t lit = 0)
{
	return JxItem { sym, legend, K_MOMENTARY, C_BLACK, lit, {} };
}

static JxItem
choice (const char* sym, const char* legend, std::vector<const char*> c)
{
	return JxItem { sym, legend, K_CHOICE, C_BLACK, 0, c };
}

static JxItem
step_labels (const char* sym, const char* legend, std::vector<const char*> labels, JxCap cap = C_BLACK)
{
	return JxItem { sym, legend, K_STEP, cap, 0, labels };
}

static JxItem
load_file (const char* legend, std::vector<const char*> patterns)
{
	return JxItem { "", legend, K_LOAD, C_BLACK, 0, patterns };
}

#define ACE_BRAND "A R D O U R   C O M M U N I T Y   E F F E C T S"

/* the ACE dynamics (mono and stereo share the ports) */
static JxLayout
ace_dynamics (const char* uri, uint32_t product)
{
	JxLayout m { uri, product, {}, 220, 0, 0, ACE_BRAND };
	m.rows.push_back ({ { "DYNAMICS", { main_knob ("thr", "THRESHOLD"), knob ("rat", "RATIO"), knob ("kn", "KNEE") } },
	                    { "TIME", { knob ("att", "ATTACK"), knob ("rel", "RELEASE") } },
	                    { "OUTPUT", { knob ("mak", "MAKEUP") } },
	                    { "SIDECHAIN", { button ("sidech", "EXT") } } });
	return m;
}

static std::vector<JxLayout> const&
layouts ()
{
	static std::vector<JxLayout> l;
	if (!l.empty ()) {
		return l;
	}

	{ /* JX-MBC */
		static const char* const band_name[] = { "LOW", "LO MID", "HI MID", "HIGH" };
		JxLayout m { "urn:jxmp:jx-mbc", 0xE8743B, {}, 200 };
		m.rows.push_back ({ { "CROSSOVER", { knob ("xover1", "LOW · LO MID"), knob ("xover2", "LO MID · HI MID"), knob ("xover3", "HI MID · HIGH") } },
		                    { "OUTPUT", { knob ("knee", "KNEE"), knob ("gain", "OUTPUT") } } });
		static char syms[4][6][12];
		for (int b = 0; b < 4; ++b) {
			const char* f[] = { "thr", "ratio", "att", "rel", "makeup", "active" };
			for (int k = 0; k < 6; ++k) {
				snprintf (syms[b][k], sizeof (syms[b][k]), "%s%d", f[k], b + 1);
			}
		}
		static char solo[4][8];
		for (int b = 0; b < 4; ++b) {
			snprintf (solo[b], sizeof (solo[b]), "solo%d", b + 1);
			m.rows.push_back ({ { string ("BAND ") + std::to_string (b + 1) + " · " + band_name[b],
			                      { main_knob (syms[b][0], "THRESHOLD"), knob (syms[b][1], "RATIO"), knob (syms[b][2], "ATTACK"),
			                        knob (syms[b][3], "RELEASE"), knob (syms[b][4], "MAKEUP"), button (syms[b][5], "ON", C_PLAY),
			                        button (solo[b], "SOLO", C_SOLO) } } });
		}
		l.push_back (m);
	}
	{ /* JX-VOC */
		JxLayout m { "urn:jxmp:jx-voc", 0xA35BD6, {}, 0 };
		m.rows.push_back ({ { "FILTER BANK", { step ("bands", "BANDS"), knob ("shift", "FORMANT"), knob ("attack", "ATTACK"), knob ("release", "RELEASE") } },
		                    { "CARRIER", { choice ("source", "SOURCE", { "SC", "NOISE" }), button ("invert", "SWAP") } } });
		m.rows.push_back ({ { "MIX", { main_knob ("wet", "WET"), knob ("dry", "VOICE DRY"), knob ("cdry", "CARRIER DRY"), knob ("sibilance", "SIBILANCE") } } });
		l.push_back (m);
	}
	{ /* JX-PIT */
		JxLayout m { "urn:jxmp:jx-pit", 0x3FB6E0, {}, 0 };
		m.rows.push_back ({ { "SHIFTER A", { button ("on_a", "ON", C_PLAY), step ("semi_a", "SEMITONES", C_MAIN), knob ("cent_a", "CENTS"), knob ("level_a", "LEVEL"), small ("pan_a", "PAN") } } });
		m.rows.push_back ({ { "SHIFTER B", { button ("on_b", "ON", C_PLAY), step ("semi_b", "SEMITONES", C_MAIN), knob ("cent_b", "CENTS"), knob ("level_b", "LEVEL"), small ("pan_b", "PAN") } } });
		m.rows.push_back ({ { "MIX", { knob ("wet", "WET"), knob ("dry", "DRY"), knob ("window", "WINDOW", C_GREY) } } });
		l.push_back (m);
	}
	{ /* JX-TUN */
		JxLayout m { "urn:jxmp:jx-tun", 0x9CD64A, {}, 200 };
		m.rows.push_back ({ { "TUNER", { choice ("mode", "MODE", { "TUNE", "CORRECT" }), knob ("ref", "A REF"), button ("mute", "MUTE", C_REC) } } });
		m.rows.push_back ({ { "CORRECTION", { step ("key", "KEY"), step ("scale", "SCALE"), knob ("speed", "SPEED"), main_knob ("amount", "AMOUNT") } } });
		l.push_back (m);
	}
	{ /* JX-DNR */
		JxLayout m { "urn:jxmp:jx-dnr", 0x7FA7C9, {}, 0 };
		m.rows.push_back ({ { "PROFILE", { button ("learn", "LEARN", JX_TRACE_CH2) } },
		                    { "REDUCTION", { main_knob ("reduction", "REDUCTION"), knob ("threshold", "THRESHOLD"), knob ("smooth", "SMOOTHING") } },
		                    { "OUTPUT", { knob ("gain", "OUTPUT"), button ("listen", "LISTEN", C_SOLO) } } });
		l.push_back (m);
	}
	{ /* JX-FRZ */
		JxLayout m { "urn:jxmp:jx-frz", 0xA8D8F0, {}, 0 };
		m.rows.push_back ({ { "FREEZE", { button ("hold", "HOLD", JX_TRACE_CH2), momentary ("recapture", "NEW") } },
		                    { "HELD SOUND", { main_knob ("level", "LEVEL"), knob ("drift", "DRIFT", C_GREY), knob ("fade", "FADE") } },
		                    { "DRY", { knob ("dry_hold", "HOLDING"), knob ("dry", "NOT HOLDING") } } });
		l.push_back (m);
	}
	{ /* JX-HRM */
		JxLayout m { "urn:jxmp:jx-hrm", 0xD65BB5, {}, 0 };
		m.rows.push_back ({ { "VOICES", { step ("center", "CENTER", C_MAIN), step ("voices", "VOICES"), step ("channel", "MIDI CH"), knob ("spread", "SPREAD", C_GREY) } },
		                    { "MIX", { knob ("gain", "INPUT"), knob ("wet", "WET"), knob ("dry", "DRY") } } });
		m.rows.push_back ({ { "ENVELOPE", { knob ("attack", "ATTACK"), knob ("release", "RELEASE"), knob ("glide", "GLIDE"), knob ("window", "WINDOW", C_GREY) } } });
		l.push_back (m);
	}
	{ /* JX-SPA */
		JxLayout m { "urn:jxmp:jx-spa", 0x5FF0A0, {}, 0 };
		m.rows.push_back ({ { "ANALYSIS", { choice ("size", "FFT SIZE", { "1K", "2K", "4K", "8K" }), choice ("window", "WINDOW", { "B-H", "BLK", "HANN", "RECT" }),
		                                    choice ("channel", "CHANNEL", { "L+R", "L", "R", "L−R" }) } } });
		m.rows.push_back ({ { "DISPLAY", { main_knob ("integrate", "INTEGRATE"), knob ("floor", "FLOOR"), knob ("slope", "SLOPE"), button ("peak", "PEAK", JX_TRACE_CH2) } } });
		l.push_back (m);
	}
	{ /* JX-LOOP */
		JxLayout m { "urn:jxmp:jx-loop", 0xFF6F91, {}, 120 };
		m.rows.push_back ({ { "LOOPER", { choice ("monitor", "INPUT MONITOR", { "AUTO", "ON", "OFF" }), main_knob ("level", "LOOPS") } } });
		static char syms[8][5][10];
		const char* f[] = { "rec", "play", "rev", "level", "clear" };
		for (int s = 0; s < 8; ++s) {
			for (int k = 0; k < 5; ++k) {
				snprintf (syms[s][k], sizeof (syms[s][k]), "%s%d", f[k], s + 1);
			}
		}
		for (int r = 0; r < 4; ++r) {
			JxRow row;
			for (int s : { r, r + 4 }) {
				row.push_back ({ string ("SLOT ") + std::to_string (s + 1),
				                 { button (syms[s][0], "REC", C_REC), button (syms[s][1], "PLAY", C_PLAY), button (syms[s][2], "REV"),
				                   small (syms[s][3], "LEVEL"), momentary (syms[s][4], "CLEAR") } });
			}
			m.rows.push_back (row);
		}
		l.push_back (m);
	}
	{ /* JX-SMP */
		JxLayout m { "urn:jxmp:jx-smp", 0xE8B23A, {}, 170 };
		m.rows.push_back ({ { "SAMPLE", { JxItem { "", "LOAD", K_LOAD, C_BLACK, 0, {} }, choice ("mode", "PITCH", { "NOTE", "FIXED" }), knob ("start", "START"),
		                                  button ("loop", "LOOP"), knob ("loop_start", "LOOP START") } },
		                    { "KEYS", { step ("note_lo", "LOW"), step ("note_hi", "HIGH"), step ("root", "ROOT", C_MAIN), knob ("tune", "TUNE"), small ("bend", "BEND") } } });
		m.rows.push_back ({ { "ENVELOPE", { knob ("attack", "ATTACK"), knob ("decay", "DECAY"), knob ("sustain", "SUSTAIN"), knob ("release", "RELEASE"),
		                                    button ("obey_off", "NOTE OFF") } },
		                    { "PLAY", { main_knob ("volume", "VOLUME"), knob ("min_volume", "MIN VOL"), small ("pan", "PAN"), step ("voices", "VOICES"),
		                                small ("probability", "CHANCE", C_GREY), step ("channel", "MIDI CH") } } });
		l.push_back (m);
	}

	/* ---- the bundled ACE plugins (Ardour Community Effects) ---- */

	l.push_back (ace_dynamics ("urn:ardour:a-comp", 0xD8443A));
	l.push_back (ace_dynamics ("urn:ardour:a-comp#stereo", 0xD8443A));
	l.push_back (ace_dynamics ("urn:ardour:a-exp", 0x2FB8C4));
	l.push_back (ace_dynamics ("urn:ardour:a-exp#stereo", 0x2FB8C4));

	{ /* ACE EQ */
		JxLayout m { "urn:ardour:a-eq", 0xF2C230, {}, 220, 0, 0, ACE_BRAND };
		static char syms[4][3][8];
		for (int b = 0; b < 4; ++b) {
			snprintf (syms[b][0], sizeof (syms[b][0]), "filtog%d", b + 1);
			snprintf (syms[b][1], sizeof (syms[b][1]), "freq%d", b + 1);
			snprintf (syms[b][2], sizeof (syms[b][2]), "bw%d", b + 1);
		}
		static char gain[4][4];
		for (int b = 0; b < 4; ++b) {
			snprintf (gain[b], sizeof (gain[b]), "g%d", b + 1);
		}
		std::vector<JxSection> bands;
		for (int b = 0; b < 4; ++b) {
			bands.push_back ({ string ("BAND ") + std::to_string (b + 1),
			                   { button (syms[b][0], "ON", C_PLAY), knob (syms[b][1], "FREQ"), knob (gain[b], "GAIN"), small (syms[b][2], "BW", C_GREY) } });
		}
		m.rows.push_back ({ { "LOW SHELF", { button ("filtogl", "ON", C_PLAY), knob ("freql", "FREQ"), knob ("gl", "GAIN") } }, bands[0], bands[1] });
		m.rows.push_back ({ bands[2], bands[3],
		                    { "HIGH SHELF", { button ("filtogh", "ON", C_PLAY), knob ("freqh", "FREQ"), knob ("gh", "GAIN") } },
		                    { "OUTPUT", { main_knob ("master", "MASTER") } } });
		l.push_back (m);
	}
	{ /* ACE Delay */
		JxLayout m { "urn:ardour:a-delay", 0x4AC0A0, {}, 0, "ACE DELAY", "TEMPO DELAY", ACE_BRAND };
		m.rows.push_back ({ { "TIME", { main_knob ("time", "TIME"), button ("sync", "SYNC"),
		                                step_labels ("div", "NOTE", { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T", "1/16 T", "1/32 T" }),
		                                button ("dot", "DOT") } },
		                    { "FEEDBACK", { knob ("feedback", "FEEDBACK"), knob ("lpf", "LPF"), button ("inv", "INVERT") } },
		                    { "OUTPUT", { knob ("drywet", "DRY · WET"), knob ("gain", "GAIN") } } });
		l.push_back (m);
	}
	{ /* ACE Reverb */
		JxLayout m { "urn:ardour:a-reverb", 0x8FC8F0, {}, 0, "ACE REVERB", "ROOM REVERB", ACE_BRAND };
		m.rows.push_back ({ { "REVERB", { main_knob ("roomsz", "ROOM SIZE"), knob ("blend", "BLEND") } } });
		l.push_back (m);
	}
	{ /* ACE Fluid Synth */
		JxLayout m { "urn:ardour:a-fluidsynth", 0x5B6EE1, {}, 0, "ACE FLUID SYNTH", "SF2 PLAYER", ACE_BRAND };
		m.rows.push_back ({ { "SOUNDFONT", { load_file ("LOAD", { "*.sf2", "*.SF2", "*.sf3", "*.SF3" }) } },
		                    { "OUTPUT", { main_knob ("level", "LEVEL") } } });
		m.rows.push_back ({ { "REVERB", { button ("rev_enable", "ON", C_PLAY), knob ("rev_roomsize", "ROOM"), knob ("rev_damp", "DAMPING"),
		                                  knob ("rev_width", "WIDTH"), knob ("rev_level", "WET") } } });
		m.rows.push_back ({ { "CHORUS", { button ("chorus_enable", "ON", C_PLAY), step ("chorus_nr", "VOICES"), knob ("chorus_speed", "SPEED"),
		                                  knob ("chorus_depth", "DEPTH"), knob ("chorus_level", "LEVEL"),
		                                  choice ("chorus_type", "WAVE", { "SINE", "TRI" }) } } });
		l.push_back (m);
	}
	return l;
}

static JxLayout const*
find_layout (std::shared_ptr<Plugin> p)
{
	if (!p) {
		return 0;
	}
	const string uri = p->unique_id ();
	for (auto const& l : layouts ()) {
		if (uri == l.uri) {
			return &l;
		}
	}
	return 0;
}

/* ---- cells ---- */

struct JxPanel::Cell {
	JxItem const*                      item;
	std::shared_ptr<AutomationControl> ac;
	double                             x, y, w, h; /* cell */
	double                             cx, cy, r;  /* knob */
	double                             bx, by, bw, bh; /* button(s) */
};

static double
knob_radius (JxKind k)
{
	switch (k) {
		case K_MAIN: return 22;
		case K_SMALL: return 13;
		case K_STEP: return 15;
		default: return 17;
	}
}

static double
cell_width (JxItem const& it)
{
	switch (it.kind) {
		case K_MAIN: return 76;
		case K_KNOB: return 64;
		case K_SMALL: return 52;
		case K_STEP: return 66;
		case K_CHOICE: return std::max (60.0, it.choices.size () * 42.0 + 4);
		case K_LOAD: return it.choices.empty () ? 56 : 220; /* with the file name */
		default: return 56;
	}
}

#define ROW_H      110 /* legend, knob with its scale, value */
#define SECTION_H  14
#define MARGIN     14
#define SECTION_GAP 18
#define BOTTOM_H   20

JxPanel::JxPanel (std::shared_ptr<PluginInsert> pi, JxLayout const& l)
	: _pi (pi)
	, _layout (l)
	, _width (400)
	, _height (300)
	, _display_h (0)
	, _display (0)
	, _drag (-1)
	, _hover (-1)
	, _drag_y (0)
	, _drag_start (0)
{
	add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK | Gdk::POINTER_MOTION_MASK | Gdk::SCROLL_MASK | Gdk::LEAVE_NOTIFY_MASK);

	std::shared_ptr<LV2Plugin> lv2 = std::dynamic_pointer_cast<LV2Plugin> (pi->plugin ());

	for (auto const& row : _layout.rows) {
		for (auto const& sec : row) {
			for (auto const& it : sec.items) {
				Cell* c = new Cell ();
				c->item = &it;
				if (lv2 && it.kind != K_LOAD) {
					const uint32_t idx = lv2->port_index (it.sym);
					if (idx != (uint32_t)-1) {
						c->ac = pi->automation_control (Evoral::Parameter (PluginAutomation, 0, idx));
					}
				}
				if (c->ac) {
					c->ac->Changed.connect (_connections, invalidator (*this), std::bind (&JxPanel::redraw, this), gui_context ());
				}
				_cells.push_back (c);
			}
		}
	}
	pi->plugin ()->QueueDraw.connect (_connections, invalidator (*this), std::bind (&JxPanel::redraw, this), gui_context ());
	pi->plugin ()->PropertyChanged.connect (_connections, invalidator (*this), std::bind (&JxPanel::redraw, this), gui_context ());
	layout ();
}

JxPanel::~JxPanel ()
{
	_connections.drop_connections ();
	for (auto c : _cells) {
		delete c;
	}
	if (_display) {
		cairo_surface_destroy (_display);
	}
}

void
JxPanel::redraw ()
{
	queue_draw ();
}

uint32_t
JxPanel::display_max () const
{
	return _layout.display_max > 0 ? _layout.display_max : 220;
}

void
JxPanel::layout ()
{
	/* width from the rows */
	double width = 0;
	for (auto const& row : _layout.rows) {
		double w = MARGIN;
		for (auto const& sec : row) {
			for (auto const& it : sec.items) {
				w += cell_width (it);
			}
			w += SECTION_GAP;
		}
		width = std::max (width, w - SECTION_GAP + MARGIN);
	}
	_width = std::max (420, (int)ceil (width));

	/* the plugin draws its own top of the panel (title, stripe, display) */
	Plugin::Display_Image_Surface* dis = _pi->plugin ()->render_inline_display (_width, display_max ());
	_display_h                         = dis ? dis->height : 32;

	/* place the cells */
	_titles.clear ();
	double y  = _display_h + 10;
	size_t ci = 0;
	for (auto const& row : _layout.rows) {
		double x = MARGIN;
		/* centre rows narrower than the panel */
		double rw = 0;
		for (auto const& sec : row) {
			for (auto const& it : sec.items) {
				rw += cell_width (it);
			}
			rw += SECTION_GAP;
		}
		rw -= SECTION_GAP;
		x = std::max ((double)MARGIN, floor ((_width - rw) * 0.5));

		for (auto const& sec : row) {
			double sw = 0;
			for (auto const& it : sec.items) {
				Cell& c = *_cells[ci++];
				c.x     = x + sw;
				c.y     = y + SECTION_H;
				c.w     = cell_width (it);
				c.h     = ROW_H;
				c.r     = knob_radius (it.kind);
				c.cx    = c.x + c.w * 0.5;
				c.cy    = c.y + 34 + c.r;
				switch (it.kind) {
					case K_CHOICE:
						c.bw = 36;
						c.bh = 20;
						c.bx = c.x + (c.w - it.choices.size () * 42.0 + 6) * 0.5;
						c.by = c.cy - c.bh * 0.5;
						break;
					case K_BUTTON:
					case K_MOMENTARY:
					case K_LOAD:
						c.bw = 46;
						c.bh = 24;
						c.bx = (it.kind == K_LOAD && !it.choices.empty ()) ? c.x + 4 : c.cx - c.bw * 0.5;
						c.by = c.cy - c.bh * 0.5;
						break;
					default:
						break;
				}
				sw += c.w;
			}
			_titles.push_back (Title { x, y + 5, sw, sec.title });
			x += sw + SECTION_GAP;
		}
		y += SECTION_H + ROW_H + 4;
	}
	_height = (int)ceil (y + BOTTOM_H);
	queue_resize ();
}

void
JxPanel::on_size_request (Gtk::Requisition* req)
{
	req->width  = _width;
	req->height = _height;
}

bool
JxPanel::render_display (cairo_t* cr)
{
	Plugin::Display_Image_Surface* dis = _pi->plugin ()->render_inline_display (_width, display_max ());
	if (!dis) {
		return false;
	}
	if (!_display || dis->width != cairo_image_surface_get_width (_display) || dis->height != cairo_image_surface_get_height (_display)) {
		if (_display) {
			cairo_surface_destroy (_display);
		}
		_display = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, dis->width, dis->height);
	}
	/* copy: the plugin owns its buffer */
	cairo_surface_flush (_display);
	unsigned char* dst    = cairo_image_surface_get_data (_display);
	const int      stride = cairo_image_surface_get_stride (_display);
	for (int y = 0; y < dis->height; ++y) {
		memcpy (dst + y * stride, dis->data + y * dis->stride, std::min (stride, dis->stride));
	}
	cairo_surface_mark_dirty (_display);
	cairo_set_source_surface (cr, _display, 0, 0);
	cairo_paint (cr);
	return true;
}

/* ---- drawing ---- */

static uint32_t
cap_color (JxCap cap, uint32_t product)
{
	switch (cap) {
		case C_MAIN: return product;
		case C_WHITE: return 0xE8E6E0;
		case C_GREY: return 0x7C8088;
		default: return 0x26272A;
	}
}

static string
short_number (double v)
{
	char        buf[32];
	const char* minus = v < 0 ? "−" : "";
	double      a     = fabs (v);
	if (a >= 1000) {
		snprintf (buf, sizeof (buf), "%s%gK", minus, rint (a / 100) / 10);
	} else if (a >= 10 || a == rint (a)) {
		snprintf (buf, sizeof (buf), "%s%.0f", minus, a);
	} else {
		snprintf (buf, sizeof (buf), "%s%.1f", minus, a);
	}
	return buf;
}

/* knob after 04-controls.md: shadow, ridged body, cap, pointer */
static void
draw_knob (cairo_t* cr, double cx, double cy, double r, uint32_t cap, double pos)
{
	/* drop shadow */
	cairo_pattern_t* p = cairo_pattern_create_radial (cx + 0.08 * r, cy + 0.16 * r, 0.7 * r, cx + 0.08 * r, cy + 0.16 * r, 1.1 * r);
	cairo_pattern_add_color_stop_rgba (p, 0, 0, 0, 0, 0.55);
	cairo_pattern_add_color_stop_rgba (p, 1, 0, 0, 0, 0);
	cairo_arc (cr, cx + 0.08 * r, cy + 0.16 * r, 1.1 * r, 0, 2 * M_PI);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);

	/* ridged body */
	p = cairo_pattern_create_linear (0, cy - r, 0, cy + r);
	cairo_pattern_add_color_stop_rgb (p, 0, 0x34 / 255.0, 0x35 / 255.0, 0x39 / 255.0);
	cairo_pattern_add_color_stop_rgb (p, 1, 0x0C / 255.0, 0x0C / 255.0, 0x0E / 255.0);
	cairo_arc (cr, cx, cy, r, 0, 2 * M_PI);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);
	const int ridges = std::max (12, (int)(2 * r / 2.95 * 2.2));
	cairo_set_line_width (cr, std::max (0.6, 0.05 * r));
	for (int i = 0; i < ridges; ++i) {
		const double a = 2 * M_PI * i / ridges;
		const double s = sin (a), c = -cos (a);
		cairo_set_source_rgba (cr, 1, 1, 1, c < 0 ? 0.10 : 0.04);
		cairo_move_to (cr, cx + s * 0.8 * r, cy + c * 0.8 * r);
		cairo_line_to (cr, cx + s * 0.98 * r, cy + c * 0.98 * r);
		cairo_stroke (cr);
	}

	/* cap */
	const double cr_ = 0.72 * r;
	JxRGB        k   = jx_hex (cap);
	p                = cairo_pattern_create_linear (0, cy - cr_, 0, cy + cr_);
	cairo_pattern_add_color_stop_rgb (p, 0, std::min (1.0, k.r + 0.1), std::min (1.0, k.g + 0.1), std::min (1.0, k.b + 0.1));
	cairo_pattern_add_color_stop_rgb (p, 1, k.r * 0.72, k.g * 0.72, k.b * 0.72);
	cairo_arc (cr, cx, cy, cr_, 0, 2 * M_PI);
	cairo_set_source (cr, p);
	cairo_fill (cr);
	cairo_pattern_destroy (p);
	cairo_set_line_width (cr, 0.6);
	cairo_set_source_rgba (cr, 0, 0, 0, 0.06);
	for (double ring : { 0.52, 0.74, 0.96 }) {
		cairo_arc (cr, cx, cy, cr_ * ring, 0, 2 * M_PI);
		cairo_stroke (cr);
	}
	p = cairo_pattern_create_radial (cx - 0.35 * cr_, cy - 0.45 * cr_, 0, cx - 0.35 * cr_, cy - 0.45 * cr_, cr_);
	cairo_pattern_add_color_stop_rgba (p, 0, 1, 1, 1, 0.32);
	cairo_pattern_add_color_stop_rgba (p, 1, 1, 1, 1, 0);
	cairo_arc (cr, cx, cy, cr_, 0, 2 * M_PI);
	cairo_set_source (cr, p);
	cairo_fill_preserve (cr);
	cairo_pattern_destroy (p);
	cairo_set_source_rgba (cr, 0, 0, 0, 0.55);
	cairo_stroke (cr);

	/* pointer */
	const double a    = -0.83 * M_PI + pos * 1.66 * M_PI;
	const bool   dark = (k.r + k.g + k.b) / 3 > 0.6;
	jx_set (cr, dark ? 0x18181A : 0xF4F4F4, 1);
	cairo_set_line_width (cr, std::max (1.3, 0.13 * r));
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to (cr, cx + sin (a) * 0.15 * r, cy - cos (a) * 0.15 * r);
	cairo_line_to (cr, cx + sin (a) * 0.92 * r, cy - cos (a) * 0.92 * r);
	cairo_stroke (cr);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
}

/* the values of an enumeration with gaps (e.g. the note values 1, 2, 4, 8,
 * 16, 32, 6, 12, 24, 48 of ACE Delay) in the order of its scale points: the
 * labels sort the points ("a - Whole note", "b - Half note", ...). Empty
 * when the parameter is not an enumeration or its values are the integers
 * lower .. upper, which simply step one by one. */
static std::vector<float>
enum_values (std::shared_ptr<AutomationControl> ac)
{
	std::vector<float>         v;
	ParameterDescriptor const& d = ac->desc ();
	if (!d.enumeration || !d.scale_points || d.scale_points->empty ()) {
		return v;
	}
	bool contiguous = d.scale_points->size () == (size_t)rint (d.upper - d.lower) + 1;
	for (auto const& sp : *d.scale_points) {
		v.push_back (sp.second);
		if (sp.second != rint (sp.second)) {
			contiguous = false;
		}
	}
	if (contiguous) {
		v.clear ();
	}
	return v;
}

static int
enum_index (std::vector<float> const& v, double x)
{
	int    best = 0;
	double dist = HUGE_VAL;
	for (size_t i = 0; i < v.size (); ++i) {
		if (fabs (v[i] - x) < dist) {
			dist = fabs (v[i] - x);
			best = i;
		}
	}
	return best;
}

/* position of a stepped selector: the value's step (or index in an enumeration) */
static double
step_position (std::shared_ptr<AutomationControl> ac)
{
	std::vector<float> const v = enum_values (ac);
	if (v.size () > 1) {
		return enum_index (v, ac->get_value ()) / (double)(v.size () - 1);
	}
	return ac->internal_to_interface (ac->get_value ());
}

/* scale printed around a knob, in the parameter's own units */
static void
draw_scale (cairo_t* cr, double cx, double cy, double r, std::shared_ptr<AutomationControl> ac, bool stepped, bool labels)
{
	ParameterDescriptor const& d     = ac->desc ();
	int                        steps = 0;
	if (stepped) {
		const size_t ne = enum_values (ac).size ();
		steps           = ne > 1 ? (int)ne - 1 : (int)rint (d.upper - d.lower);
		if (steps > 24) {
			steps = 0;
		}
	}
	const int n = steps > 0 ? steps + 1 : 11;
	for (int i = 0; i < n; ++i) {
		const double pos   = i / (double)(n - 1);
		const bool   major = steps > 0 || i == 0 || i == n - 1 || i == n / 2;
		const double a     = -0.83 * M_PI + pos * 1.66 * M_PI;
		const double r0 = r + 1.8, r1 = r0 + (major ? 3.5 : 2);
		jx_set (cr, major ? JX_LEGEND : JX_LEGEND_DIM, 1);
		cairo_set_line_width (cr, major ? 1 : 0.7);
		cairo_move_to (cr, cx + sin (a) * r0, cy - cos (a) * r0);
		cairo_line_to (cr, cx + sin (a) * r1, cy - cos (a) * r1);
		cairo_stroke (cr);
	}
	if (!labels || steps > 0) {
		return;
	}
	jx_font (cr, JX_FONT_NUMBER, 7);
	jx_set (cr, JX_LEGEND_DIM, 1);
	const bool bipolar = d.lower < 0 && fabs (d.lower + d.upper) < 1e-6 * (d.upper - d.lower) + 1e-9;
	for (int i = 0; i < 3; ++i) {
		const double pos = i * 0.5;
		const double a   = -0.83 * M_PI + pos * 1.66 * M_PI;
		const double rl  = r + 10;
		string       t;
		if (bipolar) {
			t = i == 0 ? "−" : (i == 1 ? "0" : "+");
		} else {
			t = short_number (ac->interface_to_internal (pos));
		}
		const double tx = cx + sin (a) * rl, ty = cy - cos (a) * rl;
		jx_text (cr, tx, ty + (i == 1 ? -1 : 6), 0, t.c_str ());
	}
}

static string
value_string (std::shared_ptr<AutomationControl> ac, std::vector<const char*> const& labels = std::vector<const char*> ())
{
	ParameterDescriptor const& d = ac->desc ();
	if (!labels.empty ()) {
		std::vector<float> const v   = enum_values (ac);
		const size_t             idx = v.empty () ? (size_t)std::max (0.0, rint (ac->get_value () - d.lower)) : (size_t)enum_index (v, ac->get_value ());
		if (idx < labels.size ()) {
			return labels[idx];
		}
	}
	if (d.scale_points) {
		const double v = ac->get_value ();
		for (auto const& sp : *d.scale_points) {
			if (fabs (sp.second - v) < 0.5) {
				string s = sp.first;
				std::transform (s.begin (), s.end (), s.begin (), ::toupper);
				return s;
			}
		}
	}
	const double v = ac->get_value ();
	if (d.unit == ParameterDescriptor::MIDI_NOTE) {
		const int note = (int)rint (v);
		return string (jx_note_name (note)) + std::to_string (note / 12 - 1);
	}
	/* the unit from the host's string ("10.00 ms" -> " ms"), the number printed short */
	const string us = ac->get_user_string ();
	size_t       k  = 0;
	while (k < us.size () && (isdigit (us[k]) || us[k] == '-' || us[k] == '+' || us[k] == '.' || us[k] == ',')) {
		++k;
	}
	string unit = us.substr (k);
	if (unit == " semi") {
		unit = " st";
	} else if (unit == " cent") {
		unit = " ct";
	}
	char         buf[48];
	const double a = fabs (v);
	if (d.unit == ParameterDescriptor::HZ && a >= 1000) {
		snprintf (buf, sizeof (buf), "%.2f kHz", v / 1000.0);
		unit.clear ();
	} else if (d.unit == ParameterDescriptor::DB) {
		snprintf (buf, sizeof (buf), "%.1f", v);
	} else if (d.integer_step || a >= 100 || a == rint (a)) {
		snprintf (buf, sizeof (buf), "%.0f", v);
	} else if (a >= 10) {
		snprintf (buf, sizeof (buf), "%.1f", v);
	} else {
		snprintf (buf, sizeof (buf), "%.2f", v);
	}
	string s = buf;
	if (s[0] == '-') {
		s = "−" + s.substr (1);
	}
	return s + unit;
}

void
JxPanel::draw_cell (cairo_t* cr, Cell const& c, bool hover)
{
	JxItem const& it = *c.item;
	const double  ly = c.y + 12; /* legend baseline */

	if (it.kind == K_LOAD) {
		jx_tag (cr, c.bx, c.by, c.bw, c.bh, _layout.product, false, it.legend);
		if (!it.choices.empty ()) {
			/* the loaded file in a VFD next to the button */
			string name = loaded_file ();
			name        = name.empty () ? string ("NO FILE") : Glib::path_get_basename (name);
			std::transform (name.begin (), name.end (), name.begin (), ::toupper);
			const JxRGB  lit = jx_vfd_tint (_layout.product);
			const double vx  = c.bx + c.bw + 8, vw = c.x + c.w - 4 - vx;
			jx_vfd_window (cr, vx, c.by - 1, vw, c.bh + 2, lit);
			cairo_save (cr);
			cairo_rectangle (cr, vx + 3, c.by, vw - 6, c.bh);
			cairo_clip (cr);
			jx_vfd_text (cr, vx + 6, c.by + c.bh * 0.5 + 3.5, -1, 9, lit, !loaded_file ().empty (), name.c_str ());
			cairo_restore (cr);
			jx_vfd_glare (cr, vx, c.by - 1, vw, c.bh + 2);
		}
		return;
	}
	if (!c.ac) {
		return;
	}
	const double v = c.ac->get_value ();

	switch (it.kind) {
		case K_BUTTON:
		case K_MOMENTARY: {
			const bool on = v > 0.5;
			/* glow on the light layer */
			if (on) {
				JxRGB            k = jx_hex (it.lit ? it.lit : _layout.product);
				cairo_pattern_t* p = cairo_pattern_create_radial (c.cx, c.cy, 0, c.cx, c.cy, c.bw * 0.9);
				cairo_pattern_add_color_stop_rgba (p, 0, k.r, k.g, k.b, 0.22);
				cairo_pattern_add_color_stop_rgba (p, 1, k.r, k.g, k.b, 0);
				cairo_arc (cr, c.cx, c.cy, c.bw * 0.9, 0, 2 * M_PI);
				cairo_set_source (cr, p);
				cairo_fill (cr);
				cairo_pattern_destroy (p);
			}
			jx_tag (cr, c.bx, c.by, c.bw, c.bh, it.lit ? it.lit : _layout.product, on, it.legend);
			return;
		}
		case K_CHOICE: {
			jx_font (cr, JX_FONT_LABEL, 7.5);
			jx_set (cr, JX_LEGEND, 1);
			jx_text (cr, c.cx, ly, 0, it.legend);
			const int cur = (int)rint (v - c.ac->desc ().lower);
			for (size_t i = 0; i < it.choices.size (); ++i) {
				jx_tag (cr, c.bx + i * 42, c.by, c.bw, c.bh, _layout.product, (int)i == cur, it.choices[i]);
			}
			return;
		}
		default:
			break;
	}

	/* knobs */
	const bool stepped = it.kind == K_STEP;
	jx_font (cr, JX_FONT_LABEL, 7.5);
	jx_set (cr, JX_LEGEND, 1);
	jx_text (cr, c.cx, ly, 0, it.legend);
	draw_scale (cr, c.cx, c.cy, c.r, c.ac, stepped, it.kind != K_SMALL);
	draw_knob (cr, c.cx, c.cy, c.r, cap_color (it.cap, _layout.product), stepped ? step_position (c.ac) : c.ac->internal_to_interface (v));

	/* value: a small VFD readout on selectors, print on knobs */
	const string vs = value_string (c.ac, stepped ? it.choices : std::vector<const char*> ());
	const double vy = c.cy + c.r + 24;
	if (stepped) {
		const JxRGB lit = jx_vfd_tint (_layout.product);
		jx_vfd_window (cr, c.x + 6, vy - 11, c.w - 12, 15, lit);
		jx_vfd_text (cr, c.cx, vy, 0, 8, lit, true, vs.c_str ());
	} else {
		jx_font (cr, JX_FONT_NUMBER, 7.5);
		jx_set (cr, hover ? JX_LEGEND : JX_LEGEND_DIM, 1);
		jx_text (cr, c.cx, vy, 0, vs.c_str ());
	}
}

bool
JxPanel::on_expose_event (GdkEventExpose* ev)
{
	cairo_t* cr = gdk_cairo_create (get_window ()->gobj ());
	cairo_rectangle (cr, ev->area.x, ev->area.y, ev->area.width, ev->area.height);
	cairo_clip (cr);
	paint (cr);
	cairo_destroy (cr);

	return true;
}

void
JxPanel::paint (cairo_t* cr)
{
	jx_set (cr, JX_PANEL, 1);
	cairo_paint (cr);
	if (!render_display (cr) && _layout.title) {
		/* no display of its own: the title band and the double stripe */
		cairo_save (cr);
		cairo_rectangle (cr, 0, 0, _width, _display_h);
		cairo_clip (cr);
		jx_panel (cr, _width, JX_FULL_H, _layout.product, _layout.title, _layout.subtitle);
		cairo_restore (cr);
	}

	for (auto const& t : _titles) {
		jx_section (cr, t.x, t.y, t.w, _layout.product, t.text.c_str ());
	}
	for (size_t i = 0; i < _cells.size (); ++i) {
		draw_cell (cr, *_cells[i], (int)i == _hover || (int)i == _drag);
	}

	/* bottom band with the brand and screws */
	const double by = _height - BOTTOM_H;
	jx_set (cr, JX_PANEL_DARK, 1);
	cairo_rectangle (cr, 0, by, _width, BOTTOM_H);
	cairo_fill (cr);
	jx_font (cr, JX_FONT_LABEL, 8.5);
	jx_set (cr, JX_LEGEND_DIM, 1);
	jx_text (cr, _width * 0.5, by + 13.5, 0, _layout.brand ? _layout.brand : "J X M P   A U D I O");
	for (double sx : { 9.0, _width - 9.0 }) {
		cairo_arc (cr, sx, by + 10, 3.2, 0, 2 * M_PI);
		jx_set (cr, 0x0C0C0E, 1);
		cairo_fill_preserve (cr);
		jx_set (cr, 0x3A3D44, 1);
		cairo_set_line_width (cr, 0.6);
		cairo_stroke (cr);
	}
}

/* ---- interaction ---- */

int
JxPanel::cell_at (double x, double y, int* sub) const
{
	for (size_t i = 0; i < _cells.size (); ++i) {
		Cell const& c = *_cells[i];
		switch (c.item->kind) {
			case K_CHOICE:
				for (size_t k = 0; k < c.item->choices.size (); ++k) {
					const double bx = c.bx + k * 42;
					if (x >= bx && x < bx + c.bw && y >= c.by && y < c.by + c.bh) {
						if (sub) {
							*sub = k;
						}
						return i;
					}
				}
				break;
			case K_BUTTON:
			case K_MOMENTARY:
			case K_LOAD:
				if (x >= c.bx && x < c.bx + c.bw && y >= c.by && y < c.by + c.bh) {
					return i;
				}
				break;
			default:
				if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) {
					return i;
				}
				break;
		}
	}
	return -1;
}

/* a stepped selector: v is the value, or for an enumeration the index of its scale point */
void
JxPanel::set_step (Cell& c, double v)
{
	if (!c.ac) {
		return;
	}
	std::vector<float> const e = enum_values (c.ac);
	if (e.empty ()) {
		set_cell_value (c, v);
		return;
	}
	const int idx = std::max (0, std::min ((int)e.size () - 1, (int)rint (v)));
	set_cell_value (c, e[idx]);
}

string
JxPanel::loaded_file () const
{
	std::shared_ptr<Plugin> p = _pi->plugin ();
	for (auto const& d : p->get_supported_properties ()) {
		if (d.second.datatype == Variant::PATH) {
			Variant const v = p->get_property_value (d.first);
			return v.type () == Variant::PATH ? v.get_path () : string ();
		}
	}
	return string ();
}

void
JxPanel::set_cell_value (Cell& c, double v)
{
	if (!c.ac) {
		return;
	}
	ParameterDescriptor const& d = c.ac->desc ();
	v                            = std::max ((double)d.lower, std::min ((double)d.upper, v));
	c.ac->set_value (v, PBD::Controllable::NoGroup);
}

bool
JxPanel::on_button_press_event (GdkEventButton* ev)
{
	int       sub = 0;
	const int i   = cell_at (ev->x, ev->y, &sub);
	if (i < 0) {
		return false;
	}
	Cell& c = *_cells[i];

	if (c.item->kind == K_LOAD) {
		if (ev->button == 1 && ev->type == GDK_BUTTON_PRESS) {
			load_sample (c.item->choices);
		}
		return true;
	}
	if (!c.ac) {
		return true;
	}
	/* Ctrl + middle click: MIDI learn */
	if (ev->button == 2 && Gtkmm2ext::Keyboard::modifier_state_equals (ev->state, Gtkmm2ext::Keyboard::PrimaryModifier)) {
		PBD::Controllable::StartLearning (c.ac);
		return true;
	}
	if (ev->button != 1) {
		return false;
	}
	switch (c.item->kind) {
		case K_BUTTON:
			if (ev->type == GDK_BUTTON_PRESS) {
				set_cell_value (c, c.ac->get_value () > 0.5 ? 0 : 1);
			}
			return true;
		case K_MOMENTARY:
			if (ev->type == GDK_BUTTON_PRESS) {
				set_cell_value (c, 1);
				_drag = i;
			}
			return true;
		case K_CHOICE:
			set_cell_value (c, c.ac->desc ().lower + sub);
			return true;
		default:
			break;
	}
	if (ev->type == GDK_2BUTTON_PRESS) {
		set_cell_value (c, c.ac->desc ().normal);
		return true;
	}
	_drag   = i;
	_drag_y = ev->y;
	if (c.item->kind == K_STEP) {
		std::vector<float> const ev_ = enum_values (c.ac);
		_drag_start                  = ev_.empty () ? c.ac->get_value () : enum_index (ev_, c.ac->get_value ());
	} else {
		_drag_start = c.ac->get_interface ();
	}
	c.ac->start_touch (timepos_t (c.ac->session ().transport_sample ()));
	return true;
}

bool
JxPanel::on_button_release_event (GdkEventButton* ev)
{
	if (_drag < 0) {
		return false;
	}
	Cell& c = *_cells[_drag];
	if (c.item->kind == K_MOMENTARY) {
		set_cell_value (c, 0);
	} else if (c.ac) {
		c.ac->stop_touch (timepos_t (c.ac->session ().transport_sample ()));
	}
	_drag = -1;
	queue_draw ();
	return true;
}

bool
JxPanel::on_motion_notify_event (GdkEventMotion* ev)
{
	if (_drag >= 0) {
		Cell& c = *_cells[_drag];
		if (!c.ac || c.item->kind == K_MOMENTARY) {
			return true;
		}
		const double dy   = _drag_y - ev->y;
		const bool   fine = ev->state & GDK_SHIFT_MASK;
		if (c.item->kind == K_STEP) {
			set_step (c, rint (_drag_start + dy / (fine ? 24.0 : 10.0)));
		} else {
			const double f = std::max (0.0, std::min (1.0, _drag_start + dy / (fine ? 900.0 : 180.0)));
			c.ac->set_interface (f, false, PBD::Controllable::NoGroup);
		}
		return true;
	}
	const int h = cell_at (ev->x, ev->y, 0);
	if (h != _hover) {
		_hover = h;
		queue_draw ();
	}
	return true;
}

bool
JxPanel::on_leave_notify_event (GdkEventCrossing*)
{
	if (_hover >= 0) {
		_hover = -1;
		queue_draw ();
	}
	return false;
}

bool
JxPanel::on_scroll_event (GdkEventScroll* ev)
{
	const int i = cell_at (ev->x, ev->y, 0);
	if (i < 0) {
		return false;
	}
	Cell& c = *_cells[i];
	if (!c.ac) {
		return false;
	}
	int dir = 0;
	if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_RIGHT) {
		dir = 1;
	} else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_LEFT) {
		dir = -1;
	}
	switch (c.item->kind) {
		case K_STEP: {
			std::vector<float> const v = enum_values (c.ac);
			set_step (c, (v.empty () ? rint (c.ac->get_value ()) : enum_index (v, c.ac->get_value ())) + dir);
		} break;
		case K_CHOICE:
			set_cell_value (c, rint (c.ac->get_value ()) + dir);
			break;
		case K_BUTTON:
		case K_MOMENTARY:
		case K_LOAD:
			return false;
		default: {
			const double f = c.ac->get_interface () + dir * ((ev->state & GDK_SHIFT_MASK) ? 0.004 : 0.02);
			c.ac->set_interface (std::max (0.0, std::min (1.0, f)), false, PBD::Controllable::NoGroup);
		} break;
	}
	return true;
}

void
JxPanel::load_sample (std::vector<const char*> const& patterns)
{
	std::shared_ptr<Plugin>           p     = _pi->plugin ();
	Plugin::PropertyDescriptors const& descs = p->get_supported_properties ();
	for (auto const& d : descs) {
		if (d.second.datatype != Variant::PATH) {
			continue;
		}
		Gtk::FileChooserDialog dialog (patterns.empty () ? _("Load Sample") : _("Load File"), Gtk::FILE_CHOOSER_ACTION_OPEN);
		dialog.add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
		dialog.add_button (Gtk::Stock::OPEN, Gtk::RESPONSE_OK);
		Gtk::FileFilter files;
		if (patterns.empty ()) {
			files.set_name (_("Audio Files"));
			for (const char* ext : { "*.wav", "*.WAV", "*.flac", "*.FLAC", "*.aif", "*.aiff", "*.ogg", "*.caf", "*.w64" }) {
				files.add_pattern (ext);
			}
		} else {
			files.set_name (d.second.label);
			for (const char* ext : patterns) {
				files.add_pattern (ext);
			}
		}
		dialog.add_filter (files);
		string const cur = loaded_file ();
		if (!cur.empty ()) {
			dialog.set_filename (cur);
		}
		Gtk::Window* top = dynamic_cast<Gtk::Window*> (get_toplevel ());
		if (top) {
			dialog.set_transient_for (*top);
		}
		if (dialog.run () == Gtk::RESPONSE_OK) {
			p->set_property (d.first, Variant (Variant::PATH, dialog.get_filename ()));
		}
		return;
	}
}

/* ---- the plugin window content ---- */

bool
JxPluginUI::handles (std::shared_ptr<PlugInsertBase> pib)
{
	return std::dynamic_pointer_cast<PluginInsert> (pib) && find_layout (pib->plugin ()) != 0;
}

JxPluginUI::JxPluginUI (std::shared_ptr<PlugInsertBase> pib)
	: PlugUIBase (pib)
	, _panel (0)
{
	set_name ("PluginEditor");
	set_spacing (0);

	Gtk::HBox* bar = manage (new Gtk::HBox);
	bar->set_spacing (6);
	bar->set_border_width (6);
	add_common_widgets (bar, false);
	pack_start (*bar, false, false);

	_panel = manage (new JxPanel (_pi, *find_layout (pib->plugin ())));
	pack_start (*_panel, false, false);
	show_all ();
}

JxPluginUI::~JxPluginUI ()
{
}

gint
JxPluginUI::get_preferred_height ()
{
	Gtk::Requisition r = size_request ();
	return r.height;
}

gint
JxPluginUI::get_preferred_width ()
{
	Gtk::Requisition r = size_request ();
	return r.width;
}
