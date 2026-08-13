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
/* 18, not 16, and not a round number somebody should tidy.
 *
 * AudioObjectType has 17 distinct static positions, FrontLeft 0x2 through
 * BackCenter 0x20000 (include/spatialaudioclient.idl:27-43), which is why
 * spatialaudio.c declares static_object_map[17].  16 was therefore below the
 * real ceiling.  It was also provably reached: GTA V Enhanced ships a
 * 16-channel static bed, 7.1 without LFE plus 4 Top plus 4 Bottom plus
 * BackCenter (spatial-audio-test-games.md:11), so a title in the test set sat
 * exactly on the old boundary with no margin.
 *
 * 18 rather than 17 for alignment: 17 floats is 68 bytes and takes sizeof to
 * 236, which fails the sizeof % 8 assertion below and would need a trailing
 * pad field to fix.  18 floats is 72 bytes and lands on 240 with no pad, and
 * the spare slot sits above the real ceiling rather than inside it.
 *
 * Changed while section B was still unpublished, when it cost one constant
 * and two assertions.  Once step 2 fills these fields in, the same change is
 * an ABI migration. */
#define PWHUD_BED_MAX   18u

/* out_peak_db counts the driver's own output channels to the endpoint, from
 * the negotiated PipeWire format, not spatial bed channels: those are
 * sp_bed_db[PWHUD_BED_MAX], written by the other publisher.
 *
 * 8 is chosen, not assumed.  It covers stereo through 7.1, which is every
 * endpoint that occurs here, and the scan is O(frames x channels) so raising
 * it would double the worst-case tick cost for a configuration nobody has.
 * A wider endpoint (7.1.4 over HDMI is the realistic case) is metered on its
 * first 8 channels and says so with PWHUD_F_OUT_TRUNCATED, which is the part
 * that matters: the limit is visible rather than silent.  Raising this is an
 * ABI change; adding a flag bit is not. */
#define PWHUD_OUT_MAX   8u

/* $HOME is bind-mounted into the Steam pressure-vessel container at the same
 * path; $XDG_RUNTIME_DIR is reconstructed there with only selected sockets
 * bound in, so it is not a shared drop point.  The pid disambiguates the
 * launcher process from the game process, which both load mmdevapi. */
#define PWHUD_DIR_SUFFIX  "/.cache/winepipewire"
#define PWHUD_FILE_PREFIX "hud."

#define PWHUD_F_CAPTURE        0x1u  /* the published stream is eCapture */
#define PWHUD_F_GRID_VALID     0x2u  /* period timer grid locked to the graph clock */
#define PWHUD_F_OUT_TRUNCATED  0x4u  /* endpoint has more than PWHUD_OUT_MAX channels;
                                      * out_channels is the metered count, not the real one */
#define PWHUD_F_BED_TRUNCATED  0x8u  /* section B: bed wider than PWHUD_BED_MAX */
#define PWHUD_F_OUT_NO_METER   0x10u /* the negotiated format carries no peak meter, so
                                      * out_channels 0 means "no meter", not "silent" */

/* flags is one word with two publishers, so it is partitioned by owner and the
 * partition is checked at compile time rather than remembered.  Adding a bit
 * means adding it to PWHUD_F_ALL and to exactly one mask; miss the mask and the
 * build fails here instead of the bit mysteriously never appearing.
 *
 * Section A used to assign this word outright, which silently erased every
 * section B bit within one tick: A republishes at the period rate and B at
 * 10 Hz, so a B bit survived at most one tick in ten.  Both sides now go
 * through pwhud_flags_publish below and touch only their own mask. */
#define PWHUD_F_ALL (PWHUD_F_CAPTURE | PWHUD_F_GRID_VALID | PWHUD_F_OUT_TRUNCATED | \
                     PWHUD_F_BED_TRUNCATED | PWHUD_F_OUT_NO_METER)
#define PWHUD_F_MASK_A (PWHUD_F_CAPTURE | PWHUD_F_GRID_VALID | PWHUD_F_OUT_TRUNCATED | \
                        PWHUD_F_OUT_NO_METER)
#define PWHUD_F_MASK_B (PWHUD_F_BED_TRUNCATED)

C_ASSERT((PWHUD_F_MASK_A & PWHUD_F_MASK_B) == 0);
C_ASSERT((PWHUD_F_MASK_A | PWHUD_F_MASK_B) == PWHUD_F_ALL);

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

C_ASSERT(sizeof(struct pwhud_snapshot) == 240);
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

/* Replace this publisher's bits and leave the other side's untouched.  A
 * compare-exchange loop, not fetch-and followed by fetch-or: that pair leaves a
 * window in which this side's own bits all read clear, which is a state that
 * never existed and which a reader can sample.  A 32-bit aligned atomic cannot
 * tear, so a reader observes one coherent word without needing either seqlock,
 * and the two publishers need no ordering relative to each other. */
static inline void pwhud_flags_publish(struct pwhud_snapshot *snap, uint32_t mask,
                                       uint32_t bits)
{
    uint32_t cur = __atomic_load_n(&snap->flags, __ATOMIC_RELAXED);

    while (!__atomic_compare_exchange_n(&snap->flags, &cur, (cur & ~mask) | (bits & mask),
                                        0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        ;
}

#endif /* __WINE_WINEPIPEWIRE_HUD_H */
