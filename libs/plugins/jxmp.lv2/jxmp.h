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

/* common include of the JX plugins (one LV2 bundle, one binary, several plugins) */

#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "jxmp_dsp.h"
#include "jxmp_draw.h"

#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

#define JX_URI "urn:jxmp:"

static inline LV2_Inline_Display*
jx_queue_draw_feature (const LV2_Feature* const* features)
{
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_INLINEDISPLAY__queue_draw)) {
			return (LV2_Inline_Display*)features[i]->data;
		}
	}
	return NULL;
}

static inline void*
jx_feature (const LV2_Feature* const* features, const char* uri)
{
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, uri)) {
			return features[i]->data;
		}
	}
	return NULL;
}
