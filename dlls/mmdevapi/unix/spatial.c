/*
 * Spatial audio HRTF rendering through Steam Audio (libphonon)
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(spatial);

/* Minimal Steam Audio 4.x C API declarations, from the documented interface
 * (github.com/ValveSoftware/steam-audio, Apache-2.0).  The library is
 * dlopen'd; phonon.h is not shipped. */

typedef void *IPLContext;
typedef void *IPLHRTF;
typedef void *IPLBinauralEffect;

typedef struct {
    unsigned int version;          /* STEAMAUDIO_VERSION: (major << 16) | (minor << 8) | patch */
    void *logCallback;
    void *allocateCallback;
    void *freeCallback;
    int simdLevel;                 /* max allowed: 0 SSE2 .. 4 AVX512 */
    int flags;
} IPLContextSettings;

typedef struct {
    int samplingRate;
    int frameSize;
} IPLAudioSettings;

typedef struct {
    int type;                      /* 0 = IPL_HRTFTYPE_DEFAULT */
    const char *sofaFileName;
    const unsigned char *sofaData;
    int sofaDataSize;
    float volume;
    int normType;                  /* 0 = IPL_HRTFNORMTYPE_NONE */
} IPLHRTFSettings;

typedef struct {
    int numChannels;
    int numSamples;
    float **data;                  /* deinterleaved */
} IPLAudioBuffer;

typedef struct {
    float x, y, z;
} IPLVector3;

typedef struct {
    IPLHRTF hrtf;
} IPLBinauralEffectSettings;

typedef struct {
    IPLVector3 direction;          /* unit vector, listener space: +x right, +y up, -z ahead */
    int interpolation;             /* 0 = IPL_HRTFINTERPOLATION_NEAREST */
    float spatialBlend;
    IPLHRTF hrtf;
    float *peakDelays;
} IPLBinauralEffectParams;

#define STEAMAUDIO_VERSION ((4u << 16) | (8u << 8) | 1u)

static int (*p_iplContextCreate)(IPLContextSettings *, IPLContext *);
static void (*p_iplContextRelease)(IPLContext *);
static int (*p_iplHRTFCreate)(IPLContext, IPLAudioSettings *, IPLHRTFSettings *, IPLHRTF *);
static void (*p_iplHRTFRelease)(IPLHRTF *);
static int (*p_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings *, IPLBinauralEffectSettings *, IPLBinauralEffect *);
static int (*p_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams *, IPLAudioBuffer *, IPLAudioBuffer *);
static void (*p_iplBinauralEffectRelease)(IPLBinauralEffect *);

static pthread_once_t phonon_once = PTHREAD_ONCE_INIT;
static void *phonon_handle;

static void phonon_load(void)
{
    const char *path = getenv("WINE_SPATIAL_PHONON");

    if (path && path[0])
    {
        if (!(phonon_handle = dlopen(path, RTLD_NOW)))
            /* the override may name the wrong ELF class in a split wow64 setup */
            WARN("Could not load %s: %s\n", path, dlerror());
        else
            TRACE("Loaded %s.\n", path);
    }
    if (!phonon_handle)
    {
        if (!(phonon_handle = dlopen("libphonon.so", RTLD_NOW)))
        {
            WARN("Could not load libphonon.so: %s\n", dlerror());
            return;
        }
        TRACE("Loaded libphonon.so.\n");
    }

#define LOAD_FUNC(f) \
    if (!(p_##f = dlsym(phonon_handle, #f))) goto fail
    LOAD_FUNC(iplContextCreate);
    LOAD_FUNC(iplContextRelease);
    LOAD_FUNC(iplHRTFCreate);
    LOAD_FUNC(iplHRTFRelease);
    LOAD_FUNC(iplBinauralEffectCreate);
    LOAD_FUNC(iplBinauralEffectApply);
    LOAD_FUNC(iplBinauralEffectRelease);
#undef LOAD_FUNC
    return;

fail:
    WARN("Incomplete libphonon.so, disabling HRTF.\n");
    dlclose(phonon_handle);
    phonon_handle = NULL;
}

static pthread_once_t mysofa_once = PTHREAD_ONCE_INIT;
static void *mysofa_handle;
static void *(*p_mysofa_open)(const char *, float, int *, int *);
static void (*p_mysofa_getfilter_float)(void *, float, float, float,
                                        float *, float *, float *, float *);
static void (*p_mysofa_close)(void *);

/* libmysofa is dlopen'd at runtime exactly like libphonon: WINE_SPATIAL_MYSOFA
 * overrides the path (e.g. a proton-bundled copy the container loader would not
 * find by soname), else the bare soname.  mysofa.h is not shipped, so
 * MYSOFA_EASY is treated as an opaque void *. */
static void mysofa_load(void)
{
    const char *path = getenv("WINE_SPATIAL_MYSOFA");

    if (path && path[0] && !(mysofa_handle = dlopen(path, RTLD_NOW)))
        WARN("Could not load %s: %s\n", path, dlerror());
    if (!mysofa_handle &&
        !(mysofa_handle = dlopen("libmysofa.so", RTLD_NOW)) &&
        !(mysofa_handle = dlopen("libmysofa.so.1", RTLD_NOW)))
    {
        WARN("Could not load libmysofa: %s\n", dlerror());
        return;
    }

#define LOAD_FUNC(f) \
    if (!(p_##f = dlsym(mysofa_handle, #f))) goto fail
    LOAD_FUNC(mysofa_open);
    LOAD_FUNC(mysofa_getfilter_float);
    LOAD_FUNC(mysofa_close);
#undef LOAD_FUNC
    return;

fail:
    WARN("Incomplete libmysofa, disabling SOFA backend.\n");
    dlclose(mysofa_handle);
    mysofa_handle = NULL;
}

#define SPATIAL_MAX_SLOTS 128

enum spatial_backend { BACKEND_PHONON = 0, BACKEND_SOFA = 1 };

struct sofa_slot
{
    int    active;
    float *hist;        /* taps-1 input-history samples, persists across ticks */
    float  dir[3];      /* previous SOFA-cartesian unit direction (crossfade gate) */
    int    have_prev;
    float *kl, *kr;     /* previous L/R kernels (taps each), for crossfade */
};

struct sofa_state
{
    void  *easy;        /* MYSOFA_EASY * */
    int    taps;        /* filter length from mysofa_open */
    UINT   frames;
    struct sofa_slot slots[SPATIAL_MAX_SLOTS];
    float *ext;         /* taps-1 + frames mono input scratch */
    float *cur_l, *cur_r;                       /* current-tick kernels (taps each) */
    float *t_oldl, *t_oldr, *t_newl, *t_newr;   /* frames each, crossfade temporaries */
};

/* Calls are serialized by the PE-side stream lock; no locking here. */
struct spatial_engine
{
    IPLContext ctx;
    IPLHRTF hrtf;
    IPLAudioSettings audio;
    IPLBinauralEffect effects[SPATIAL_MAX_SLOTS];
    float *scratch;     /* 2 * frames, deinterleaved L then R */
    int bass_on;        /* WINE_SPATIAL_BASS one-pole low-shelf active */
    float bass_g;       /* LF gain, 0..1 (1 = unchanged) */
    float bass_a;       /* one-pole coefficient = 2*pi*fc/fs */
    float lp_l, lp_r;   /* lowpass state, persists across mix calls */
    UINT rate, frames;       /* mix rate and per-tick frame count */
    int backend;             /* enum spatial_backend */
    struct sofa_state *sofa; /* SOFA backend state, NULL for phonon */
};

static void fir_conv(const float *ext, const float *h, int taps, UINT frames, float *out)
{
    UINT n;
    int k;

    for (n = 0; n < frames; n++)
    {
        float acc = 0.0f;
        for (k = 0; k < taps; k++)
            acc += ext[n + (taps - 1) - k] * h[k];
        out[n] = acc;
    }
}

static void sofa_free(struct spatial_engine *e)
{
    struct sofa_state *s = e->sofa;
    int i;

    if (!s) return;
    if (s->easy) p_mysofa_close(s->easy);
    for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
    {
        free(s->slots[i].hist);
        free(s->slots[i].kl);
        free(s->slots[i].kr);
    }
    free(s->ext);
    free(s->cur_l);
    free(s->cur_r);
    free(s->t_oldl);
    free(s->t_oldr);
    free(s->t_newl);
    free(s->t_newr);
    free(s);
    e->sofa = NULL;
}

static int sofa_init(struct spatial_engine *e, const char *path)
{
    struct sofa_state *s;
    int taps = 0, err = 0, i;

    pthread_once(&mysofa_once, mysofa_load);
    if (!p_mysofa_open) return 0;

    if (!(s = calloc(1, sizeof(*s)))) return 0;
    e->sofa = s;

    if (!(s->easy = p_mysofa_open(path, (float)e->rate, &taps, &err)) || err || taps <= 0)
    {
        WARN("mysofa_open(%s) failed: err %d, taps %d.\n", path, err, taps);
        sofa_free(e);
        return 0;
    }
    s->taps = taps;
    s->frames = e->frames;

    if (!(s->ext    = calloc((size_t)(taps - 1) + e->frames, sizeof(float))) ||
        !(s->cur_l  = calloc(taps, sizeof(float))) ||
        !(s->cur_r  = calloc(taps, sizeof(float))) ||
        !(s->t_oldl = calloc(e->frames, sizeof(float))) ||
        !(s->t_oldr = calloc(e->frames, sizeof(float))) ||
        !(s->t_newl = calloc(e->frames, sizeof(float))) ||
        !(s->t_newr = calloc(e->frames, sizeof(float))))
    {
        sofa_free(e);
        return 0;
    }

    for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
    {
        if (!(s->slots[i].hist = calloc((size_t)(taps - 1) + 1, sizeof(float))) ||
            !(s->slots[i].kl   = calloc(taps, sizeof(float))) ||
            !(s->slots[i].kr   = calloc(taps, sizeof(float))))
        {
            sofa_free(e);
            return 0;
        }
    }

    TRACE("SOFA HRTF backend: %s, %d taps @ %u Hz.\n", path, taps, e->rate);
    return 1;
}

static int phonon_init(struct spatial_engine *e)
{
    IPLContextSettings ctx_settings;
    IPLHRTFSettings hrtf_settings;

    pthread_once(&phonon_once, phonon_load);
    if (!phonon_handle) return 0;

    memset(&ctx_settings, 0, sizeof(ctx_settings));
    ctx_settings.version = STEAMAUDIO_VERSION;
    ctx_settings.simdLevel = 4; /* allow up to AVX512 */
    if (p_iplContextCreate(&ctx_settings, &e->ctx))
    {
        WARN("iplContextCreate failed.\n");
        return 0;
    }

    memset(&hrtf_settings, 0, sizeof(hrtf_settings));
    hrtf_settings.type = 0;      /* IPL_HRTFTYPE_DEFAULT */
    hrtf_settings.volume = 1.0f;
    if (p_iplHRTFCreate(e->ctx, &e->audio, &hrtf_settings, &e->hrtf))
    {
        WARN("iplHRTFCreate failed.\n");
        p_iplContextRelease(&e->ctx);
        return 0;
    }
    return 1;
}

static NTSTATUS spatial_init(void *args)
{
    struct spatial_init_params *params = args;
    const char *sofa_path = getenv("WINE_SPATIAL_SOFA");
    struct spatial_engine *engine;

    if (!(engine = calloc(1, sizeof(*engine)))) return STATUS_NO_MEMORY;
    if (!(engine->scratch = calloc(2 * (size_t)params->frames, sizeof(float))))
    {
        free(engine);
        return STATUS_NO_MEMORY;
    }

    engine->rate = params->rate;
    engine->frames = params->frames;
    engine->audio.samplingRate = params->rate;
    engine->audio.frameSize = params->frames;

    if (sofa_path && sofa_path[0] && sofa_init(engine, sofa_path))
        engine->backend = BACKEND_SOFA;
    else
    {
        if (sofa_path && sofa_path[0])
            WARN("SOFA backend unavailable, falling back to libphonon.\n");
        if (!phonon_init(engine))
        {
            free(engine->scratch);
            free(engine);
            return STATUS_NOT_SUPPORTED;
        }
        engine->backend = BACKEND_PHONON;
    }

    {
        const char *bass = getenv("WINE_SPATIAL_BASS");
        const char *hz = getenv("WINE_SPATIAL_BASS_HZ");
        /* defaults calibrated to the measured correlated-bed bass buildup: a
         * flat ~+5 dB plateau below ~140 Hz that rolls off to 0 by ~1 kHz */
        float fc = hz && hz[0] ? (float)atof(hz) : 500.0f;
        float g = bass && bass[0] ? (float)atof(bass) : 0.48f; /* default-on; WINE_SPATIAL_BASS=1 disables */

        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        engine->bass_g = g;
        engine->bass_a = 2.0f * 3.14159265358979f * fc / (float)params->rate;
        if (engine->bass_a > 1.0f) engine->bass_a = 1.0f;
        engine->bass_on = (g < 1.0f);
        if (engine->bass_on)
            TRACE("bass low-shelf on: LF gain %.2f, corner %.0f Hz.\n", g, fc);
    }

    TRACE("engine %p backend=%s: %u Hz, %u frames.\n", engine,
          engine->backend == BACKEND_SOFA ? "sofa" : "phonon", params->rate, params->frames);
    params->handle = (UINT_PTR)engine;
    return STATUS_SUCCESS;
}

static NTSTATUS spatial_release(void *args)
{
    struct spatial_release_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;
    unsigned int i;

    if (engine->backend == BACKEND_SOFA)
        sofa_free(engine);
    else
    {
        for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
            if (engine->effects[i]) p_iplBinauralEffectRelease(&engine->effects[i]);
        p_iplHRTFRelease(&engine->hrtf);
        p_iplContextRelease(&engine->ctx);
    }
    free(engine->scratch);
    free(engine);
    return STATUS_SUCCESS;
}

static NTSTATUS spatial_object_add(void *args)
{
    struct spatial_object_add_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;
    unsigned int i;

    if (engine->backend == BACKEND_SOFA)
    {
        struct sofa_state *s = engine->sofa;

        for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
            if (!s->slots[i].active) break;
        if (i == SPATIAL_MAX_SLOTS) return STATUS_TOO_MANY_OPENED_FILES;

        s->slots[i].active = 1;
        s->slots[i].have_prev = 0;
        memset(s->slots[i].hist, 0, (size_t)(s->taps - 1) * sizeof(float));
    }
    else
    {
        IPLBinauralEffectSettings settings;

        for (i = 0; i < SPATIAL_MAX_SLOTS; i++)
            if (!engine->effects[i]) break;
        if (i == SPATIAL_MAX_SLOTS) return STATUS_TOO_MANY_OPENED_FILES;

        settings.hrtf = engine->hrtf;
        if (p_iplBinauralEffectCreate(engine->ctx, &engine->audio, &settings, &engine->effects[i]))
        {
            WARN("iplBinauralEffectCreate failed.\n");
            return STATUS_NOT_SUPPORTED;
        }
    }

    params->slot = i;
    return STATUS_SUCCESS;
}

static NTSTATUS spatial_object_remove(void *args)
{
    struct spatial_object_remove_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;

    if (params->slot >= SPATIAL_MAX_SLOTS)
        return STATUS_INVALID_PARAMETER;

    if (engine->backend == BACKEND_SOFA)
    {
        if (!engine->sofa->slots[params->slot].active)
            return STATUS_INVALID_PARAMETER;
        engine->sofa->slots[params->slot].active = 0;
    }
    else
    {
        if (!engine->effects[params->slot])
            return STATUS_INVALID_PARAMETER;
        p_iplBinauralEffectRelease(&engine->effects[params->slot]);
        engine->effects[params->slot] = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS spatial_mix(void *args)
{
    struct spatial_mix_params *params = args;
    struct spatial_engine *engine = (struct spatial_engine *)(UINT_PTR)params->handle;
    const struct spatial_mix_object *objs = (const struct spatial_mix_object *)(UINT_PTR)params->objects;
    float *out_l = (float *)(UINT_PTR)params->out_l;
    float *out_r = (float *)(UINT_PTR)params->out_r;
    unsigned int i, f;

    if (params->frames != (UINT)engine->audio.frameSize) return STATUS_INVALID_PARAMETER;

    if (engine->backend == BACKEND_PHONON)
    {
        for (i = 0; i < params->count; i++)
        {
            IPLBinauralEffectParams effect_params;
            IPLAudioBuffer in, out;
            float *in_data = (float *)(UINT_PTR)objs[i].buffer;
            float *out_data[2];
            float len;

            if (objs[i].slot >= SPATIAL_MAX_SLOTS || !engine->effects[objs[i].slot])
                continue;

            len = sqrtf(objs[i].pos[0] * objs[i].pos[0] +
                        objs[i].pos[1] * objs[i].pos[1] +
                        objs[i].pos[2] * objs[i].pos[2]);
            if (len > 0.0f)
            {
                /* ISpatialAudioObject::SetPosition and Steam Audio use the same
                 * listener-space convention (+x right, +y up, -z ahead) */
                effect_params.direction.x = objs[i].pos[0] / len;
                effect_params.direction.y = objs[i].pos[1] / len;
                effect_params.direction.z = objs[i].pos[2] / len;
            }
            else
            {
                effect_params.direction.x = 0.0f;
                effect_params.direction.y = 0.0f;
                effect_params.direction.z = -1.0f;
            }
            effect_params.interpolation = 0; /* IPL_HRTFINTERPOLATION_NEAREST */
            effect_params.spatialBlend = 1.0f;
            effect_params.hrtf = engine->hrtf;
            effect_params.peakDelays = NULL;

            in.numChannels = 1;
            in.numSamples = params->frames;
            in.data = &in_data;

            out_data[0] = engine->scratch;
            out_data[1] = engine->scratch + params->frames;
            out.numChannels = 2;
            out.numSamples = params->frames;
            out.data = out_data;

            p_iplBinauralEffectApply(engine->effects[objs[i].slot], &effect_params, &in, &out);

            for (f = 0; f < params->frames; f++)
            {
                out_l[f] += out_data[0][f] * objs[i].volume;
                out_r[f] += out_data[1][f] * objs[i].volume;
            }
        }
    }
    else
    {
        struct sofa_state *s = engine->sofa;

        for (i = 0; i < params->count; i++)
        {
            struct sofa_slot *sl;
            const float *in = (const float *)(UINT_PTR)objs[i].buffer;
            float vol = objs[i].volume;
            float sx, sy, sz, len, ux, uy, uz, dot, dL, dR;
            int moved;

            if (objs[i].slot >= SPATIAL_MAX_SLOTS) continue;
            sl = &s->slots[objs[i].slot];
            if (!sl->active) continue;

            /* ISAC (+x right, +y up, -z ahead) -> SOFA cartesian
             * (+x front, +y left, +z up) */
            sx = -objs[i].pos[2];
            sy = -objs[i].pos[0];
            sz =  objs[i].pos[1];
            len = sqrtf(sx * sx + sy * sy + sz * sz);
            if (len > 0.0f) { ux = sx / len; uy = sy / len; uz = sz / len; }
            else            { ux = 1.0f; uy = 0.0f; uz = 0.0f; } /* SOFA front */

            p_mysofa_getfilter_float(s->easy, ux, uy, uz, s->cur_l, s->cur_r, &dL, &dR);

            dot = ux * sl->dir[0] + uy * sl->dir[1] + uz * sl->dir[2];
            moved = !sl->have_prev || dot < 0.9999f;

            memcpy(s->ext, sl->hist, (size_t)(s->taps - 1) * sizeof(float));
            memcpy(s->ext + s->taps - 1, in, s->frames * sizeof(float));

            if (moved && sl->have_prev)
            {
                fir_conv(s->ext, sl->kl, s->taps, s->frames, s->t_oldl);
                fir_conv(s->ext, sl->kr, s->taps, s->frames, s->t_oldr);
                fir_conv(s->ext, s->cur_l, s->taps, s->frames, s->t_newl);
                fir_conv(s->ext, s->cur_r, s->taps, s->frames, s->t_newr);
                for (f = 0; f < params->frames; f++)
                {
                    float w = (f + 1.0f) / s->frames;
                    out_l[f] += (s->t_oldl[f] * (1.0f - w) + s->t_newl[f] * w) * vol;
                    out_r[f] += (s->t_oldr[f] * (1.0f - w) + s->t_newr[f] * w) * vol;
                }
            }
            else
            {
                fir_conv(s->ext, s->cur_l, s->taps, s->frames, s->t_newl);
                fir_conv(s->ext, s->cur_r, s->taps, s->frames, s->t_newr);
                for (f = 0; f < params->frames; f++)
                {
                    out_l[f] += s->t_newl[f] * vol;
                    out_r[f] += s->t_newr[f] * vol;
                }
            }

            memcpy(sl->hist, s->ext + s->frames, (size_t)(s->taps - 1) * sizeof(float));
            memcpy(sl->kl, s->cur_l, (size_t)s->taps * sizeof(float));
            memcpy(sl->kr, s->cur_r, (size_t)s->taps * sizeof(float));
            sl->dir[0] = ux; sl->dir[1] = uy; sl->dir[2] = uz;
            sl->have_prev = 1;
        }
    }

    if (engine->bass_on)
    {
        float a = engine->bass_a, g1 = 1.0f - engine->bass_g;

        for (f = 0; f < params->frames; f++)
        {
            engine->lp_l += a * (out_l[f] - engine->lp_l);
            engine->lp_r += a * (out_r[f] - engine->lp_r);
            out_l[f] -= g1 * engine->lp_l;
            out_r[f] -= g1 * engine->lp_r;
        }
    }

    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    spatial_init,
    spatial_release,
    spatial_object_add,
    spatial_object_remove,
    spatial_mix,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == spatial_funcs_count);

#ifdef _WIN64

/* params structs use zero-extended UINT64 for every pointer, so the layouts
 * are identical for 32-bit callers and the entry points can be shared */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    spatial_init,
    spatial_release,
    spatial_object_add,
    spatial_object_remove,
    spatial_mix,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == spatial_funcs_count);

#endif /* _WIN64 */
