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
/* The size a version 1 writer has always published at minimum: every field
 * through sp_bed_db, which is what any reader may assume without consulting
 * size.  A reader validates against this and not against its own sizeof,
 * because the append rule is what lets a NEW reader work with an OLD writer
 * and testing sizeof would reject exactly that case. */
#define PWHUD_SIZE_V1_BASE 240u
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
 * 18 rather than 17 for alignment: 17 floats is 68 bytes and ends the array on
 * 236, which fails the sizeof % 8 assertion below and would need a trailing
 * pad field to fix.  18 floats is 72 bytes and ends it on 240 with no pad, and
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
 * out_peak_db is the per-channel MAXIMUM over every started render stream in
 * the elected period group, not the elected stream's own level, and
 * out_channels is the widest metered count among them.  It used to be the
 * elected stream alone, which published silence for entire sessions: a title
 * whose elected stream is permanently quiet read -120.0 dBFS while all of its
 * audio played on a sibling in the same group.  Per stream levels are exact in
 * drv_str[], so nothing is lost by aggregating here.  A consequence for
 * readers: PWHUD_F_OUT_NO_METER and PWHUD_F_OUT_TRUNCATED are now the OR over
 * that set, so either bit means at least one stream is affected and the
 * maximum may understate the group, not that the number shown is unusable.
 *
 * 8 is chosen, not assumed.  It covers stereo through 7.1, which is every
 * endpoint that occurs here, and the scan is O(frames x channels) so raising
 * it would double the worst-case tick cost for a configuration nobody has.
 * A wider endpoint (7.1.4 over HDMI is the realistic case) is metered on its
 * first 8 channels and says so with PWHUD_F_OUT_TRUNCATED, which is the part
 * that matters: the limit is visible rather than silent.  Raising this is an
 * ABI change; adding a flag bit is not. */
#define PWHUD_OUT_MAX   8u

/* Started render streams in the elected period group, not a process-wide
 * list.  Seqlock A has one writer, the elected group's timer thread, so
 * streams in any other group are out of reach; pw_stream_count versus
 * drv_group_streams is how a reader sees the unmetered remainder.
 *
 * 8, not a guess to tidy later.  Titles that open more than one render
 * stream still open a handful (Borderlands 3 opens two), and the scan is
 * ~470 ns per stream, so the cap is a few us of a 10 ms period and only
 * when WINEPIPEWIRE_HUD=1.  More than 8 in the elected group sets
 * PWHUD_F_STR_TRUNCATED and drv_str_count is the metered count.  Raising
 * this is an ABI change; the flag is not. */
#define PWHUD_STR_MAX   8u

/* First size that includes drv_str[].  A reader that wants those rows
 * tests size against this, not against sizeof. */
#define PWHUD_SIZE_V1_STR 584u

/* First size that includes the section B clip counters.  Same rule: gate on
 * this, not on sizeof, so a new reader still works against an old writer. */
#define PWHUD_SIZE_V1_CLIP 624u

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
#define PWHUD_F_NO_DSP_LOAD    0x20u /* pw_dsp_load is not implemented, so 0.0 means "not
                                      * measured" and must be rendered as unavailable.
                                      * Scoped to that one field so it can be cleared alone */
#define PWHUD_F_STR_TRUNCATED  0x40u /* elected group has more started render streams than
                                      * PWHUD_STR_MAX; drv_str_count is the metered count */

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
                     PWHUD_F_BED_TRUNCATED | PWHUD_F_OUT_NO_METER | PWHUD_F_NO_DSP_LOAD | \
                     PWHUD_F_STR_TRUNCATED)
#define PWHUD_F_MASK_A (PWHUD_F_CAPTURE | PWHUD_F_GRID_VALID | PWHUD_F_OUT_TRUNCATED | \
                        PWHUD_F_OUT_NO_METER | PWHUD_F_NO_DSP_LOAD | PWHUD_F_STR_TRUNCATED)
#define PWHUD_F_MASK_B (PWHUD_F_BED_TRUNCATED)

C_ASSERT((PWHUD_F_MASK_A & PWHUD_F_MASK_B) == 0);
C_ASSERT((PWHUD_F_MASK_A | PWHUD_F_MASK_B) == PWHUD_F_ALL);

#define PWHUD_DISPATCH_UNKNOWN 0u
#define PWHUD_DISPATCH_DATA    1u
#define PWHUD_DISPATCH_LOOP    2u

/* Silence, and the value of every unused out_peak_db/sp_bed_db/drv_str
 * peak_db slot: a meter plots this, where -INFINITY would collapse its range. */
#define PWHUD_DB_FLOOR (-120.0f)

/* One started render stream in the elected group.  id is the same monotonic
 * counter as drv_stream_id, not a pointer.  channels is the metered width,
 * at most PWHUD_OUT_MAX, and 0 means this tick had nothing to scan. */
struct pwhud_str
{
    uint32_t id;
    uint32_t channels;
    float    peak_db[PWHUD_OUT_MAX];
};

/* Byte identical under -m32 and -m64: fixed width only, and every 64-bit
 * field sits at an offset divisible by 8 because the System V i386 ABI aligns
 * uint64_t to 4 while x86_64 aligns it to 8.  Fields are appended, never
 * reordered or repurposed; a reader accepts version <= its own and uses size
 * to learn which trailing fields exist.
 *
 * Explicit padding is the one exception, and it is an exception because of
 * what the rule is for: a reader must never misread a field it believes it
 * understands.  Padding has never carried meaning, so no reader has ever read
 * it and claiming it cannot produce a misread.  Two conditions make that
 * true and both are checked rather than assumed: the whole page is zeroed at
 * creation, so a writer too old to know the new field leaves it 0 and a newer
 * reader sees the zero value rather than garbage, and the claimed slot must
 * never have been written by anything.  Repurposing a field that once meant
 * something stays forbidden, because there the old meaning is exactly what a
 * reader would misread it as.
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
    /* The GRAPH DRIVER node's xruns, not this stream's: it is the device the
     * graph is clocked by, shared with every other client on it, so a nonzero
     * value means the sink glitched and not that we starved.  Our own ring
     * starving is drv_underruns, a different failure with a different fix.
     *
     * A count of observed episodes.  The underlying signal is spa_io_clock.xrun,
     * an accumulated duration in samples at the clock rate; one episode here is
     * one increase of that accumulator, however many quanta it spans. */
    uint32_t pw_xruns;
    uint32_t pw_stream_count;
    float    pw_dsp_load;        /* 0..1, meaningless unless PWHUD_F_NO_DSP_LOAD is clear */
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
    /* Successful render-ring resync repairs.  A repair snaps the process
     * callback's read cursor forward onto the writer's, so whatever it had not
     * yet played is dropped: an audible discontinuity bounded by the ring,
     * which is 60 ms at 48 kHz stereo float32 with the usual six periods.  It
     * is counted rather than flagged because it is rare, roughly one event
     * against 9750 publishes in a measured 195 second session, so a bit that
     * cleared per tick would be missed and its absence would read as "none".
     * The failure path is not counted here; it already reports through
     * ring_op_failed and returns an error to the application.  Claimed from
     * padding, so no existing offset moved. */
    uint32_t drv_ring_resyncs;

    uint32_t seq_sp;             /* seqlock B */
    uint32_t sp_hrtf;
    uint32_t sp_bed_virtualized;
    uint32_t sp_bed_mask;
    uint32_t sp_dyn_live;
    uint32_t sp_dyn_max;
    float    sp_bed_db[PWHUD_BED_MAX];

    /* Appended 2026-08-15, after section A published one stream's peaks, ring
     * and xruns beside a process-wide stream count with nothing naming the
     * stream, and two independent investigations drew the same wrong
     * conclusion from it.
     *
     * drv_stream_id is a small monotonic counter assigned when the driver
     * creates a stream, deliberately NOT a pointer: the reader is another
     * process, so an address is both meaningless and an information leak, and
     * a recycled allocation would silently alias two streams.  0 means no
     * stream is elected.  drv_group_streams counts started render streams in
     * the elected period group, so a reader can see the ratio: id 3 out of 4
     * says the peaks describe one stream of four.  Both are section A and are
     * written inside seqlock A. */
    uint32_t drv_stream_id;
    uint32_t drv_group_streams;

    /* Section B, same problem at the other end.  seq_sp == 0 meant three
     * different things and rendered as one string, "never published", which
     * reads as a fault when the usual cause is a title that has simply never
     * activated spatial audio.
     *
     * sp_clients counts ISpatialAudioObjectRenderStream activations in this
     * process.  It is NOT covered by seqlock B and must not be: the seqlock
     * has exactly one writer, the stream that won the publish election, while
     * every activating stream stamps this one.  It is incremented atomically
     * instead, the same treatment the flags word gets, so an activation on one
     * thread cannot corrupt a mix publish on another.  sp_publishes counts mix
     * publishes and IS inside the seqlock with the bed values it describes.
     *
     * So a reader has three states: clients 0 means no spatial client has ever
     * existed, clients nonzero with publishes 0 means one exists and has never
     * mixed, and publishes nonzero means the bed values above are live. */
    uint32_t sp_clients;
    uint32_t sp_publishes;

    /* Per-stream meters, section A, and the exact levels behind the group
     * maximum in out_peak_db.  Only the elected group's started render
     * streams; other groups are outside this writer. */
    uint32_t drv_str_count;
    uint32_t drv_str_pad;        /* keeps drv_str 8-aligned; unused */
    struct pwhud_str drv_str[PWHUD_STR_MAX];

    /* Truncation accounting for the bus clip, section B, appended 2026-08-20.
     * Cumulative over the elected stream's life and never reset, so a reader
     * samples them at any two instants and subtracts.
     *
     * They exist because the clip is the one stage in our mixer that changes
     * samples irreversibly, and it did so with no instrument: a user report of
     * harshness could not be separated from a title that simply mixes hot.
     * Windows reports clipping nowhere, so this is a place where we can be
     * better than the reference rather than merely match it.
     *
     * sp_clip_total is the denominator sp_clip_samples needs and it counts
     * samples the clip examined, which is bus channels only, so the ratio is
     * "of the samples that could have been truncated" and not "of the stream".
     * sp_bus_passes is the same service for sp_clip_passes: passes in which
     * the clip ran at all, which is the ones with something on the bus.
     *
     * sp_clip_engagements counts clean-to-clipping transitions, not passes,
     * because that is the quantity the limiter question turns on: duty and
     * modulation rate move in opposite directions with release time, and the
     * audible artefact tracks the rate.  A pass with nothing on the bus ends
     * an engagement, since there is no signal left to hold.
     *
     * sp_clip_nonfinite counts samples sent to zero by the non-finite guard.
     * Separate from sp_clip_samples on purpose: mixing them would make the
     * truncation ratio ambiguous, and a NaN reaching the bus is a different
     * fault with a different cause.  Without it that guard is invisible in
     * the field.
     *
     * sp_clip_peak_db is dB above full scale, worst sample magnitude before
     * truncation, and exactly 0.0 means nothing ever exceeded full scale.
     * Published as dB rather than a ratio because the log runs once per
     * publish on the PE side and never in the sample loop. */
    uint64_t sp_clip_samples;
    uint64_t sp_clip_total;
    uint32_t sp_clip_passes;
    uint32_t sp_bus_passes;
    uint32_t sp_clip_engagements;
    uint32_t sp_clip_nonfinite;
    float    sp_clip_peak_db;
    uint32_t sp_clip_pad;        /* keeps the struct 8-aligned; unused */
};

C_ASSERT(sizeof(struct pwhud_str) == 40);
C_ASSERT(sizeof(struct pwhud_snapshot) == 624);
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
C_ASSERT(offsetof(struct pwhud_snapshot, drv_ring_resyncs)    == 140);
C_ASSERT(offsetof(struct pwhud_snapshot, seq_sp)              == 144);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_hrtf)             == 148);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bed_virtualized)  == 152);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bed_mask)         == 156);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_dyn_live)         == 160);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_dyn_max)          == 164);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bed_db)           == 168);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_stream_id)       == 240);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_group_streams)   == 244);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clients)          == 248);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_publishes)        == 252);
C_ASSERT(offsetof(struct pwhud_str, id)                       ==   0);
C_ASSERT(offsetof(struct pwhud_str, channels)                 ==   4);
C_ASSERT(offsetof(struct pwhud_str, peak_db)                  ==   8);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_str_count)       == 256);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_str_pad)         == 260);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_str)             == 264);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_str[0].id)       == 264);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_str[0].channels) == 268);
C_ASSERT(offsetof(struct pwhud_snapshot, drv_str[0].peak_db)  == 272);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clip_samples)     == 584);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clip_total)       == 592);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clip_passes)      == 600);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_bus_passes)       == 604);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clip_engagements) == 608);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clip_nonfinite)   == 612);
C_ASSERT(offsetof(struct pwhud_snapshot, sp_clip_peak_db)     == 616);
/* The baseline is exactly the end of sp_bed_db.  Pinned, because a reader
 * validating against it would otherwise be trusting a number that could drift
 * away from the last field an old writer actually wrote. */
C_ASSERT(PWHUD_SIZE_V1_BASE == offsetof(struct pwhud_snapshot, sp_bed_db) +
                               sizeof(((struct pwhud_snapshot *)0)->sp_bed_db));
C_ASSERT(PWHUD_SIZE_V1_BASE <= sizeof(struct pwhud_snapshot));
C_ASSERT(PWHUD_SIZE_V1_STR == offsetof(struct pwhud_snapshot, drv_str) +
                             sizeof(((struct pwhud_snapshot *)0)->drv_str));
C_ASSERT(PWHUD_SIZE_V1_STR <= sizeof(struct pwhud_snapshot));
C_ASSERT(PWHUD_SIZE_V1_CLIP == offsetof(struct pwhud_snapshot, sp_clip_pad) +
                               sizeof(((struct pwhud_snapshot *)0)->sp_clip_pad));
C_ASSERT(PWHUD_SIZE_V1_CLIP <= sizeof(struct pwhud_snapshot));

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
