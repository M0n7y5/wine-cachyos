/*
 * Spatial audio HRTF engine unixlib interface
 *
 * Copyright 2026 CachyOS
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_MMDEVAPI_UNIX_UNIXLIB_H
#define __WINE_MMDEVAPI_UNIX_UNIXLIB_H

#include "windef.h"
#include "wine/unixlib.h"

/* All pointer-sized values cross the boundary as zero-extended UINT64 so the
 * params structs have identical layout for 32- and 64-bit PE callers and the
 * wow64 entry points can reuse the regular functions. */

struct spatial_init_params
{
    UINT rate;
    UINT frames;
    UINT64 handle;   /* out */
};

struct spatial_release_params
{
    UINT64 handle;
};

struct spatial_object_add_params
{
    UINT64 handle;
    UINT slot;       /* out */
};

struct spatial_object_remove_params
{
    UINT64 handle;
    UINT slot;
};

/* Engine effect slots. Every mix entry needs one, so this also bounds the
 * object array the PE side sends to spatial_mix. */
#define SPATIAL_MAX_SLOTS 128

struct spatial_mix_object
{
    UINT64 buffer;   /* mono float[frames] */
    UINT slot;
    float pos[3];
    float volume;
};

struct spatial_mix_params
{
    UINT64 handle;
    UINT frames;
    UINT count;
    UINT64 objects;  /* struct spatial_mix_object[count] */
    UINT64 out_l;    /* float[frames] accumulators, zeroed by the caller */
    UINT64 out_r;
};

/* Section B of the shared diagnostic snapshot.  Scalars only, all 4-byte, so
 * the layout is identical for 32- and 64-bit callers without any explicit
 * padding.  SPATIAL_BED_MAX and SPATIAL_DB_FLOOR are asserted against the
 * snapshot's own PWHUD_BED_MAX and PWHUD_DB_FLOOR in unix/spatial.c, which is
 * the one file that knows both, so this header stays independent of the
 * driver's. */
#define SPATIAL_BED_MAX  18
#define SPATIAL_DB_FLOOR (-120.0f)

struct spatial_hud_params
{
    UINT hrtf;
    UINT bed_virtualized;
    UINT bed_mask;            /* bit i set => bed channel i present */
    UINT dyn_live;
    UINT dyn_max;
    UINT bed_truncated;       /* a bed channel index exceeded SPATIAL_BED_MAX */
    UINT enabled;             /* out: 0 = no snapshot in this process, stop calling */
    float bed_db[SPATIAL_BED_MAX];
};

C_ASSERT(sizeof(struct spatial_hud_params) == 100);

enum spatial_unix_func
{
    unix_spatial_init,
    unix_spatial_release,
    unix_spatial_object_add,
    unix_spatial_object_remove,
    unix_spatial_mix,
    unix_spatial_hud_publish,
    spatial_funcs_count,
};

#endif /* __WINE_MMDEVAPI_UNIX_UNIXLIB_H */
