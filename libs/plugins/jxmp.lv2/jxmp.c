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

#include <stddef.h>

#include <lv2/core/lv2.h>

extern const LV2_Descriptor jx_mbc_descriptor;
extern const LV2_Descriptor jx_voc_descriptor;
extern const LV2_Descriptor jx_pit_descriptor;
extern const LV2_Descriptor jx_tun_descriptor;
extern const LV2_Descriptor jx_dnr_descriptor;
extern const LV2_Descriptor jx_frz_descriptor;
extern const LV2_Descriptor jx_hrm_descriptor;
extern const LV2_Descriptor jx_spa_descriptor;
extern const LV2_Descriptor jx_loop_descriptor;
extern const LV2_Descriptor jx_smp_descriptor;

static const LV2_Descriptor* const descriptors[] = {
	&jx_mbc_descriptor,
	&jx_voc_descriptor,
	&jx_pit_descriptor,
	&jx_tun_descriptor,
	&jx_dnr_descriptor,
	&jx_frz_descriptor,
	&jx_hrm_descriptor,
	&jx_spa_descriptor,
	&jx_loop_descriptor,
	&jx_smp_descriptor,
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	if (index < sizeof (descriptors) / sizeof (descriptors[0])) {
		return descriptors[index];
	}
	return NULL;
}
