/*
 * Shared-memory diagnostic snapshot published by winepipewire.drv
 *
 * Copyright 2026 M0n7y5
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

#ifndef __WINE_WINEPIPEWIRE_HUD_H
#define __WINE_WINEPIPEWIRE_HUD_H

#include <stdint.h>
#include <stddef.h>

/* Also consumed outside the Wine tree, where winnt.h is absent. */
#ifndef C_ASSERT
#define C_ASSERT(e) _Static_assert(e, #e)
#endif

#define PWHUD_MAGIC     0x54535750u  /* 'PWST' */
#define PWHUD_VERSION   1u
#define PWHUD_BYTES     4096u
#define PWHUD_BED_MAX   16u
#define PWHUD_OUT_MAX   8u

/* $HOME is bind-mounted into the Steam pressure-vessel container at the same
 * path; $XDG_RUNTIME_DIR is reconstructed there with only selected sockets
 * bound in, so it is not a shared drop point.  The pid disambiguates the
 * launcher process from the game process, which both load mmdevapi. */
#define PWHUD_DIR_SUFFIX  "/.cache/winepipewire"
#define PWHUD_FILE_PREFIX "hud."

#define PWHUD_F_CAPTURE    0x1u  /* the published stream is eCapture */
#define PWHUD_F_GRID_VALID 0x2u  /* period timer grid locked to the graph clock */

#define PWHUD_DISPATCH_UNKNOWN 0u
#define PWHUD_DISPATCH_DATA    1u
#define PWHUD_DISPATCH_LOOP    2u

/* Silence, and the value of every unused out_peak_db/sp_bed_db slot: a meter
 * plots this, where -INFINITY would collapse its range. */
#define PWHUD_DB_FLOOR (-120.0f)

/* Byte identical under -m32 and -m64: fixed width only, and every 64-bit
 * field sits at an offset divisible by 8 because the System V i386 ABI aligns
 * uint64_t to 4 while x86_64 aligns it to 8.  Fields are appended, never
 * reordered or repurposed; a reader accepts version <= its own and uses size
 * to learn which trailing fields exist.
 *
 * Each section has its own seqlock and exactly one writer.  Section A is
 * written by the driver's elected period-timer thread, section B by the PE
 * spatial publisher, so the two need no coordination. */
struct pwhud_snapshot
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t writer_pid;
    uint64_t clock_ns;           /* CLOCK_MONOTONIC at the last section A publish */
    uint32_t flags;
    uint32_t seq_drv;            /* seqlock A */

    uint32_t pw_quantum;         /* frames */
    uint32_t pw_rate;            /* Hz */
    uint32_t pw_xruns;
    uint32_t pw_stream_count;
    float    pw_dsp_load;        /* 0..1 */
    uint32_t drv_dispatch;
    uint32_t drv_underruns;
    uint32_t drv_overruns;
    uint32_t drv_bad_buffers;
    uint32_t drv_period_usec;
    uint64_t drv_held_bytes;
    uint64_t drv_ring_bytes;
    uint64_t drv_period_bytes;
    int64_t  drv_phase_adjust_us;
    float    out_peak_db[PWHUD_OUT_MAX];
    uint32_t out_channels;
    uint32_t _pad_a;

    uint32_t seq_sp;             /* seqlock B */
    uint32_t sp_hrtf;
    uint32_t sp_bed_virtualized;
    uint32_t sp_bed_mask;
    uint32_t sp_dyn_live;
    uint32_t sp_dyn_max;
    float    sp_bed_db[PWHUD_BED_MAX];
};

C_ASSERT(sizeof(struct pwhud_snapshot) == 232);
C_ASSERT(sizeof(struct pwhud_snapshot) <= PWHUD_BYTES);
C_ASSERT(sizeof(struct pwhud_snapshot) % 8 == 0);
C_ASSERT(sizeof(float) == 4);

C_ASSERT(offsetof(struct pwhud_snapshot, magic)               ==   0);
C_ASSERT(offsetof(struct pwhud_snapshot, version)             ==   4);
C_ASSERT(offsetof(struct pwhud_snapshot, size)                ==   8);
C_ASSERT(offsetof(struct pwhud_snapshot, writer_pid)          ==  12);
C_ASSERT(offsetof(struct pwhud_snapshot, clock_ns)            ==  16);
C_ASSERT(offsetof(struct pwhud_snapshot, flags)               ==  24);
C_ASSERT(offsetof(struct pwhud_snapshot, seq_drv)             ==  28);
C_ASSERT(offsetof(struct pwhud_snapshot, pw_quantum)          ==  32);
C_ASSERT(offsetof(struct pwhud_snapshot, pw_rate)             ==  36);
C_ASSERT(offsetof(struct pwhud_snapshot, pw_xruns)            ==  40);
C_ASSERT(offsetof(struct pwhud_snapshot, pw_stream_count)     ==  44);
C_ASSERT(offsetof(struct pwhud_snapshot, pw_dsp_load)         ==  48);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_dispatch)        ==  52);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_underruns)       ==  56);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_overruns)        ==  60);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_bad_buffers)     ==  64);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_period_usec)     ==  68);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_held_bytes)      ==  72);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_ring_bytes)      ==  80);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_period_bytes)    ==  88);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_phase_adjust_us) ==  96);
C_ASSERT(offsetof(struct pwhud_snapshot, out_peak_db)         == 104);
C_ASSERT(offsetof(struct pwhud_snapshot, out_channels)        == 136);
C_ASSERT(offsetof(struct pwhud_snapshot, _pad_a)              == 140);
C_ASSERT(offsetof(struct pwhud_snapshot, seq_sp)              == 144);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_hrtf)             == 148);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bed_virtualized)  == 152);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bed_mask)         == 156);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_dyn_live)         == 160);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_dyn_max)          == 164);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bed_db)           == 168);

#endif /* __WINE_WINEPIPEWIRE_HUD_H */
