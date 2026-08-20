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

/* Section B of the shared diagnostic snapshot.  SPATIAL_BED_MAX and
 * SPATIAL_DB_FLOOR are asserted against the snapshot's own PWHUD_BED_MAX and
 * PWHUD_DB_FLOOR in unix/spatial.c, which is the one file that knows both, so
 * this header stays independent of the driver's.
 *
 * The layout must be identical for 32- and 64-bit callers, which was free
 * while every field was 4 bytes.  The clip totals are 64-bit because their
 * denominator counts every bus sample and a 32-bit one wraps after 12 hours
 * of stereo at 48 kHz, so they sit at offsets divisible by 8 and the struct
 * carries an explicit tail pad: the System V i386 ABI aligns UINT64 to 4 and
 * x86_64 aligns it to 8, so only offsets that satisfy both agree.  The
 * offsets below are asserted rather than trusted.  Same rule as the
 * snapshot's own header, which has carried mixed widths from the start. */
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
    /* An announce publish stamps "a spatial stream exists in this process"
     * and nothing else: no bed values, no flags, and no seqlock.  It runs
     * once per stream at activation, off the mix path. */
    UINT announce;
    UINT enabled;             /* out: 0 = no snapshot in this process, stop calling */
    float bed_db[SPATIAL_BED_MAX];
    /* Cumulative for the life of the publishing stream, so a lost publish
     * costs freshness and never a count, and the value in the snapshot is
     * always some stream's own total rather than a sum across streams. */
    UINT64 clip_samples;
    UINT64 clip_total;
    UINT clip_passes;
    UINT bus_passes;
    UINT clip_engagements;
    UINT clip_nonfinite;
    float clip_peak_db;       /* dB above full scale; exactly 0.0 = never over */
    UINT pad;                 /* holds the size equal on both arches; unused */
};

C_ASSERT(sizeof(struct spatial_hud_params) == 144);
C_ASSERT(offsetof(struct spatial_hud_params, clip_samples) == 104);
C_ASSERT(offsetof(struct spatial_hud_params, clip_total) == 112);

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
