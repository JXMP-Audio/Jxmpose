/*
 * Copyright (C) 2026 xGlck
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

#include <cstdint>
#include <string>

#include <cairo.h>

#include "gtkmm2ext/colors.h"

/* "Hardware panel" decoration for mixer strips (JXMP look):
 * a double stripe in the route colour and subtle, deterministic wear.
 * Everything is drawn under the strip's widgets; nothing here affects layout.
 */
namespace MixerStripDecor
{

/* FNV-1a of a stable identity (e.g. the route ID), so a strip always looks the same */
uint32_t seed_for (std::string const& identity);

/* double stripe: a thick band and a thin line below it, filling the given rectangle */
void draw_stripe (cairo_t*, double x, double y, double w, double h, Gtkmm2ext::Color);

/* wear: worn edges, scratches, chipped paint, grime. Drawn with alpha only, so it works on any panel colour */
void draw_patina (cairo_t*, double w, double h, uint32_t seed, double scale);

} // namespace MixerStripDecor
