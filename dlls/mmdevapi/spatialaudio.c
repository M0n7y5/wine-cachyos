/*
 * Copyright 2020 Andrew Eikum for CodeWeavers
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

#define COBJMACROS

#include <stdarg.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"

#include "mmdevapi_private.h"

#include "wine/unixlib.h"
#include "unix/unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);
WINE_DECLARE_DEBUG_CHANNEL(spatial);

#define SPATIAL_MAX_DYNAMIC_OBJECTS 112  /* matches Windows Sonic for Headphones */

static BOOL spatial_option_enabled(const WCHAR *value)
{
    return *value == 'y' || *value == 'Y' || *value == 't' || *value == 'T' || *value == '1';
}

static UINT get_spatial_dynamic_budget(void)
{
    /* an over-long env value or an empty registry value leaves buf untouched
     * while still reporting success */
    WCHAR buf[16] = {0};
    DWORD size = sizeof(buf), type = REG_NONE;
    HKEY key;
    BOOL enabled = FALSE;
    const char *source = "registry";

    if(GetEnvironmentVariableW(L"WINE_SPATIAL_SOUND", buf, ARRAY_SIZE(buf))){
        enabled = spatial_option_enabled(buf);
        source = "environment";
    }else if(RegOpenKeyW(HKEY_CURRENT_USER, L"Software\\Wine\\mmdevapi", &key) == ERROR_SUCCESS){
        if(RegQueryValueExW(key, L"SpatialSound", 0, &type, (BYTE*)buf, &size) == ERROR_SUCCESS){
            if(type == REG_SZ || type == REG_EXPAND_SZ){
                enabled = spatial_option_enabled(buf);
            }else if(type == REG_DWORD && size == sizeof(DWORD)){
                /* a DWORD reads as L"\x0001" as a string, which is neither
                 * "1" nor "y", so it would otherwise silently mean "off" */
                DWORD value;
                memcpy(&value, buf, sizeof(value));
                enabled = value != 0;
            }else
                WARN("Ignoring SpatialSound of type %lu.\n", type);
        }
        RegCloseKey(key);
    }

    TRACE_(spatial)("Spatial sound %s (%s), dynamic object budget %u.\n",
            enabled ? "enabled" : "disabled", source,
            enabled ? SPATIAL_MAX_DYNAMIC_OBJECTS : 0);

    return enabled ? SPATIAL_MAX_DYNAMIC_OBJECTS : 0;
}

static BOOL spatial_unix_init(void)
{
    static LONG status = -1;
    if (status == -1) InterlockedExchange(&status, __wine_init_unix_call());
    return !status;
}

/* Index of a static bed channel in static_object_map[], or ~0 for anything
 * that has no channel: None, Dynamic, a multi-bit value, or a bit above
 * BackCenter. The argument is app-supplied, and AudioObjectType is signed, so
 * neither the range nor the shift can be left to chance. */
static UINT32 AudioObjectType_to_index(AudioObjectType type)
{
    UINT32 bits = type, o = 0;

    if(bits < AudioObjectType_FrontLeft || bits > AudioObjectType_BackCenter ||
            (bits & (bits - 1)))
        return ~0;
    while(bits >>= 1)
        ++o;
    return o - 1;
}

static const char *debugstr_fmtex(const WAVEFORMATEX *fmt)
{
    static char buf[2048];

    if (!fmt)
    {
        strcpy(buf, "(null)");
    }
    else if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
        snprintf(buf, sizeof(buf), "tag: 0x%x (%s), ch: %u (mask: 0x%lx), rate: %lu, depth: %u",
                fmt->wFormatTag, debugstr_guid(&fmtex->SubFormat),
                fmt->nChannels, fmtex->dwChannelMask, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    else
    {
        snprintf(buf, sizeof(buf), "tag: 0x%x, ch: %u, rate: %lu, depth: %u",
                fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    return buf;
}

/* WAVE_FORMAT_EXTENSIBLE is a spelling of a format, not a different format:
 * Windows runs one validator over both shapes and accepts either for the same
 * PCM parameters.  Comparing wFormatTag raw rejected the extensible spelling of
 * the very format we advertise, so a title that builds its object format that
 * way got E_INVALIDARG out of activation and no spatial audio at all. */
static WORD object_format_tag(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if(fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        return fmt->wFormatTag;
    if(fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return WAVE_FORMAT_UNKNOWN;
    if(IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        return WAVE_FORMAT_IEEE_FLOAT;
    if(IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
        return WAVE_FORMAT_PCM;
    return WAVE_FORMAT_UNKNOWN;
}

static BOOL object_formats_compatible(const WAVEFORMATEX *fmt1, const WAVEFORMATEX *fmt2)
{
    WORD tag = object_format_tag(fmt1);

    /* Packing and depth are validated separately, by validate_wave_format_ex
     * below, because Windows validates the shape before it looks at whether
     * the parameters are ones the engine supports, and the two failures carry
     * different HRESULTs. */
    return tag != WAVE_FORMAT_UNKNOWN &&
           tag == object_format_tag(fmt2) &&
           fmt1->nChannels == fmt2->nChannels &&
           fmt1->nSamplesPerSec == fmt2->nSamplesPerSec &&
           fmt1->wBitsPerSample == fmt2->wBitsPerSample;
}

/* Windows runs one validator over every WAVEFORMATEX handed to the spatial
 * API, ahead of any test of what the engine actually supports, which is what
 * separates a malformed format (E_INVALIDARG) from a well-formed unsupported
 * one (AUDCLNT_E_UNSUPPORTED_FORMAT).  The set of fields it does and does not
 * look at is not obvious and is load-bearing: the prologue never reads
 * wBitsPerSample, only the extensible arm cross-checks nBlockAlign, and
 * dwChannelMask is never read at all. */
static HRESULT validate_wave_format_ex(const WAVEFORMATEX *fmt)
{
    UINT32 prod;

    if(!fmt)
        return E_POINTER;

    if(!fmt->nChannels || !fmt->nSamplesPerSec || !fmt->nAvgBytesPerSec ||
            !fmt->nBlockAlign || fmt->cbSize > 0x400)
        return E_INVALIDARG;

    /* 32-bit wrapping product, positive for every depth the arms below
     * accept, so the divide needs no sign handling. */
    prod = (UINT32)fmt->nChannels * fmt->wBitsPerSample;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        BOOL pcm;

        if(fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            return E_INVALIDARG;

        /* Only the two known subformats are examined; anything else is
         * accepted on the prologue alone, so none of the tests below run for
         * it. */
        pcm = IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM);
        if(!pcm && !IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return S_OK;

        if(pcm){
            if(fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16 &&
                    fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32)
                return E_INVALIDARG;
        }else if(fmt->wBitsPerSample != 32 && fmt->wBitsPerSample != 64)
            return E_INVALIDARG;

        if(!ext->Samples.wValidBitsPerSample ||
                fmt->wBitsPerSample < ext->Samples.wValidBitsPerSample)
            return E_INVALIDARG;

        if(fmt->nAvgBytesPerSec != ((prod * fmt->nSamplesPerSec) >> 3))
            return E_INVALIDARG;

        if(fmt->nBlockAlign != prod / 8)
            return E_INVALIDARG;

        return S_OK;
    }

    if(fmt->wFormatTag != WAVE_FORMAT_PCM && fmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
        return S_OK;

    if(fmt->cbSize)
        return E_INVALIDARG;

    if(fmt->wBitsPerSample & 7)
        return E_INVALIDARG;

    if(fmt->nChannels > 2)
        return E_INVALIDARG;

    if(fmt->nAvgBytesPerSec != ((prod * fmt->nSamplesPerSec) >> 3))
        return E_INVALIDARG;

    return S_OK;
}

typedef struct SpatialAudioImpl SpatialAudioImpl;
typedef struct SpatialAudioStreamImpl SpatialAudioStreamImpl;
typedef struct SpatialAudioObjectImpl SpatialAudioObjectImpl;

struct SpatialAudioObjectImpl {
    ISpatialAudioObject ISpatialAudioObject_iface;
    LONG ref;

    SpatialAudioStreamImpl *sa_stream;
    AudioObjectType type;
    UINT32 static_idx;

    float *buf;

    BOOL invalidated;
    BOOL updated;
    /* The object's lifetime starts at its first GetBuffer, and only from then
     * on does missing a pass implicitly end the stream (GetBuffer docs). An
     * object activated and not yet written stays valid, which is what lets a
     * caller pre-activate a pool of dynamic objects at setup and only write
     * one when a voice needs it. */
    BOOL started;
    /* SetEndOfStream invalidates the object for the caller at once, but its
     * last buffer still owes one pass through the mixer */
    BOOL eos_pending;
    UINT32 eos_frames;

    float pos[3];
    float volume;
    UINT engine_slot;

    struct list entry;
};

struct SpatialAudioStreamImpl {
    ISpatialAudioObjectRenderStream ISpatialAudioObjectRenderStream_iface;
    LONG ref;
    CRITICAL_SECTION lock;

    SpatialAudioImpl *sa_client;
    SpatialAudioObjectRenderStreamActivationParams params;

    IAudioClient *client;
    IAudioRenderClient *render;

    UINT32 period_frames, update_frames;
    WAVEFORMATEXTENSIBLE stream_fmtex;

    float *buf;

    UINT32 static_object_map[17];
    UINT32 dyn_max, dyn_live;
    UINT32 hud_frames;   /* frames still owed before the next section B publish */
    BOOL virtualize_bed;
    UINT32 dyn_left, dyn_right;

    UINT64 engine;
    float *hrtf_buf;

    /* Bus clip accounting, published in section B.  Cumulative for the life of
     * the stream and deliberately not cleared by SAORS_Reset: they describe
     * what this stream did to the audio, and a title that stops and restarts
     * has not undone it.  clip_peak is a linear magnitude so the sample loop
     * stays free of a log; the conversion to dB happens once per publish.
     * clip_engaged is the edge detector behind clip_engagements. */
    UINT64 clip_samples, clip_total;
    UINT32 clip_passes, bus_passes, clip_engagements, clip_nonfinite;
    float clip_peak;
    BOOL clip_engaged;

    struct list objects;
};

struct SpatialAudioImpl {
    ISpatialAudioClient ISpatialAudioClient_iface;
    IAudioFormatEnumerator IAudioFormatEnumerator_iface;
    IMMDevice *mmdev;
    LONG ref;
    WAVEFORMATEXTENSIBLE object_fmtex;
    UINT dyn_budget;
};

/* Section B of the shared snapshot.  Exactly one stream publishes, because the
 * snapshot holds one stream's worth of bed.  The previous text writer
 * was per-process, so with two spatial streams whichever one called
 * EndUpdatingAudioObjects last in the period silently overwrote the other's
 * channels.  Electing one stream makes which stream you are reading
 * deterministic instead of a race, and a second stream is reported once so the
 * remaining limitation is visible rather than silent.  Section A elects its
 * period group the same way, for the same reason. */
static SpatialAudioStreamImpl *hud_stream;
/* One-way and process-wide on purpose, because both ways of setting it are
 * permanent for the process and neither can turn transient:
 *   - the snapshot file is created only by the driver's hud_init, whose one
 *     call site is pipewire_process_attach, so it exists from before the
 *     first stream or it never exists;
 *   - the unix side maps at most once via hud_map_once, and nothing resets
 *     a failed map, so it is never retried even if the file did appear.
 * Clearing this on stream release would therefore re-learn the same answer at
 * 10 Hz forever, one unixlib transition per tick, and would also restart the
 * per-tick dBFS pass that want_hud gates.  If either half above ever stops
 * holding, this must become a bounded retry rather than a latch. */
static LONG hud_off;            /* the unix side found no snapshot; stop calling */
static LONG hud_multi_warned;

static inline SpatialAudioObjectImpl *impl_from_ISpatialAudioObject(ISpatialAudioObject *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioObjectImpl, ISpatialAudioObject_iface);
}

static inline SpatialAudioStreamImpl *impl_from_ISpatialAudioObjectRenderStream(ISpatialAudioObjectRenderStream *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioStreamImpl, ISpatialAudioObjectRenderStream_iface);
}

static inline SpatialAudioImpl *impl_from_ISpatialAudioClient(ISpatialAudioClient *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, ISpatialAudioClient_iface);
}

static inline SpatialAudioImpl *impl_from_IAudioFormatEnumerator(IAudioFormatEnumerator *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, IAudioFormatEnumerator_iface);
}

static HRESULT WINAPI SAO_QueryInterface(ISpatialAudioObject *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObject)) {
        *ppv = &This->ISpatialAudioObject_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAO_AddRef(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAO_Release(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        EnterCriticalSection(&This->sa_stream->lock);
        list_remove(&This->entry);
        if(This->type == AudioObjectType_Dynamic)
            This->sa_stream->dyn_live--;
        /* bed channels take a slot too when the bed is virtualized */
        if(This->sa_stream->engine && This->engine_slot != ~0){
            struct spatial_object_remove_params params;
            params.handle = This->sa_stream->engine;
            params.slot = This->engine_slot;
            WINE_UNIX_CALL(unix_spatial_object_remove, &params);
        }
        LeaveCriticalSection(&This->sa_stream->lock);

        ISpatialAudioObjectRenderStream_Release(&This->sa_stream->ISpatialAudioObjectRenderStream_iface);
        free(This->buf);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAO_GetBuffer(ISpatialAudioObject *iface,
        BYTE **buffer, UINT32 *bytes)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p, %p)\n", This, buffer, bytes);

    if(!buffer || !bytes)
        return E_POINTER;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->invalidated){
        *buffer = NULL;
        *bytes = 0;
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    This->started = This->updated = TRUE;

    *buffer = (BYTE *)This->buf;
    *bytes = This->sa_stream->update_frames *
        This->sa_stream->sa_client->object_fmtex.Format.nBlockAlign;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetEndOfStream(ISpatialAudioObject *iface, UINT32 frames)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%u)\n", This, frames);

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(frames > This->sa_stream->update_frames){
        LeaveCriticalSection(&This->sa_stream->lock);
        /* Windows rejects this and leaves the object active so the caller can
         * retry with a correct count.  Clamping ends the voice instead: a
         * title that computes its final frame from its own clock and
         * overshoots by one loses the tail here and keeps it on Windows. */
        WARN("End of stream past the buffer: %u of %u frames.\n",
                frames, This->sa_stream->update_frames);
        return E_INVALIDARG;
    }

    /* the caller sees an invalidated object from here on, but the frames it
     * just wrote are part of the stream and are mixed once more */
    if(!This->invalidated){
        This->eos_frames = frames;
        This->eos_pending = TRUE;
    }
    This->invalidated = TRUE;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_IsActive(ISpatialAudioObject *iface, BOOL *active)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, active);

    if(!active)
        return E_POINTER;

    EnterCriticalSection(&This->sa_stream->lock);
    *active = !This->invalidated;
    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_GetAudioObjectType(ISpatialAudioObject *iface,
        AudioObjectType *type)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, type);

    if(!type)
        return E_POINTER;

    *type = This->type;

    return S_OK;
}

static HRESULT WINAPI SAO_SetPosition(ISpatialAudioObject *iface, float x,
        float y, float z)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%f, %f, %f)\n", This, x, y, z);

    if(This->type != AudioObjectType_Dynamic)
        return SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    This->pos[0] = x;
    This->pos[1] = y;
    This->pos[2] = z;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetVolume(ISpatialAudioObject *iface, float vol)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%f)\n", This, vol);

    /* Unlike SetPosition there is no type check: volume applies to static
     * objects too.  The range test is Windows' first check, ahead of even the
     * destroyed test, and NaN falls through it exactly as it does there
     * because both comparisons are false when unordered. */
    if(vol < 0.0f || vol > 1.0f)
        return E_INVALIDARG;

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->invalidated){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    /* The HRESULT above is Windows' answer, which lets NaN through because it
     * is unordered against both bounds.  The gain we then multiply by must
     * still be finite: a non-finite one poisons every sample the object
     * contributes, and on the passthrough path that lands outside the bus
     * clip.  Nothing can read this back, there is no GetVolume. */
    This->volume = isfinite(vol) ? vol : 0.0f;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static ISpatialAudioObjectVtbl ISpatialAudioObject_vtbl = {
    SAO_QueryInterface,
    SAO_AddRef,
    SAO_Release,
    SAO_GetBuffer,
    SAO_SetEndOfStream,
    SAO_IsActive,
    SAO_GetAudioObjectType,
    SAO_SetPosition,
    SAO_SetVolume,
};

static HRESULT WINAPI SAORS_QueryInterface(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStreamBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)) {
        *ppv = &This->ISpatialAudioObjectRenderStream_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAORS_AddRef(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAORS_Release(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        IAudioClient_Stop(This->client);
        if(This->engine){
            struct spatial_release_params params;
            params.handle = This->engine;
            WINE_UNIX_CALL(unix_spatial_release, &params);
        }
        free(This->hrtf_buf);
        if(This->update_frames != ~0 && This->update_frames > 0)
            IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        IAudioRenderClient_Release(This->render);
        IAudioClient_Release(This->client);
        if(This->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_Release(This->params.NotifyObject);
        free((void*)This->params.ObjectFormat);
        CloseHandle(This->params.EventHandle);
        InterlockedCompareExchangePointer((void **)&hud_stream, NULL, This);
        DeleteCriticalSection(&This->lock);
        ISpatialAudioClient_Release(&This->sa_client->ISpatialAudioClient_iface);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAORS_GetAvailableDynamicObjectCount(
        ISpatialAudioObjectRenderStream *iface, UINT32 *count)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    TRACE("(%p)->(%p)\n", This, count);

    if(!count)
        return E_POINTER;

    EnterCriticalSection(&This->lock);
    /* Same source as Begin's out parameter, which Windows requires. */
    *count = This->dyn_max;
    LeaveCriticalSection(&This->lock);
    return S_OK;
}

static HRESULT WINAPI SAORS_GetService(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **service)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), service);

    if(!service)
        return E_POINTER;

    *service = NULL;

    /* Windows recognises seven IIDs here and answers E_NOINTERFACE for
     * everything else, so a blanket E_NOTIMPL was wrong for both halves.  The
     * four below are the ones our own IAudioClient serves, and forwarding is
     * the whole implementation because they are services on this very stream.
     * IAudioClock2 is deliberately not one of them: Windows refuses it here
     * too, and hands it out only through a QueryInterface on the IAudioClock
     * it returns, which our clock object supports.  The three we cannot serve
     * (IMFTrustedOutput, IAudioClientTrustedOutputPriv and IKsControl on the
     * endpoint) fall through to E_NOINTERFACE.  IAudioRenderClient is not
     * forwarded on purpose: the stream owns the render packet for the length
     * of a pass and a second reference would let the caller take it away. */
    if(IsEqualIID(riid, &IID_IAudioClock) ||
            IsEqualIID(riid, &IID_IAudioStreamVolume) ||
            IsEqualIID(riid, &IID_IAudioSessionControl) ||
            IsEqualIID(riid, &IID_ISimpleAudioVolume))
        return IAudioClient_GetService(This->client, riid, service);

    WARN("Unsupported service %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static HRESULT WINAPI SAORS_Start(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Start(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Start failed: %08lx\n", hr);
        /* The spatial API has its own code for this state and Reset below
         * already maps it, so a title testing for the documented code sees it
         * from both methods. */
        if(hr == AUDCLNT_E_NOT_STOPPED)
            return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
        return hr;
    }

    TRACE_(spatial)("stream %p started, rendering through %s.\n", This,
            This->engine ? "the Steam Audio HRTF engine" :
            (This->virtualize_bed ? "stereo panning (HRTF unavailable)" : "multichannel passthrough"));
    return S_OK;
}

static HRESULT WINAPI SAORS_Stop(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    /* S_FALSE for an already-stopped stream, which is not FAILED, so the
     * driver's answer has to be forwarded rather than replaced by S_OK. */
    hr = IAudioClient_Stop(This->client);
    if(FAILED(hr))
        WARN("IAudioClient::Stop failed: %08lx\n", hr);

    return hr;
}

static HRESULT WINAPI SAORS_Reset(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    BOOL pass_open;
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    /* Windows' Reset is one IAudioEndpointControl::Reset and never reads the
     * pass flag, so an open pass survives it.  Ours holds a render packet for
     * the length of a pass and IAudioClient::Reset refuses while one is held,
     * so drop the packet and take a fresh one instead of letting a
     * WASAPI-internal code out of a spatial method.  Nothing is lost: the
     * mixers run in End, so between Begin and End the packet is untouched. */
    pass_open = This->update_frames != ~0 && This->update_frames > 0;
    if(pass_open)
        IAudioRenderClient_ReleaseBuffer(This->render, 0, 0);

    hr = IAudioClient_Reset(This->client);

    if(pass_open){
        HRESULT buf_hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames,
                (BYTE **)&This->buf);
        if(FAILED(buf_hr)){
            WARN("could not reacquire the render buffer: %08lx\n", buf_hr);
            This->update_frames = ~0;
            if(SUCCEEDED(hr))
                hr = buf_hr;
        }
    }

    LeaveCriticalSection(&This->lock);

    if(hr == AUDCLNT_E_NOT_STOPPED)
        return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
    return hr;
}

static HRESULT WINAPI SAORS_BeginUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface,
        UINT32 *dyn_count, UINT32 *frames)
{
    static BOOL fixme_once = FALSE;
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", This, dyn_count, frames);

    if(!dyn_count || !frames)
        return E_POINTER;

    /* Windows zeroes both before any test can fail, so a caller that ignores
     * the HRESULT reads 0 rather than the previous pass's values. */
    *dyn_count = 0;
    *frames = 0;

    EnterCriticalSection(&This->lock);

    if(This->update_frames != ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    This->update_frames = This->period_frames;

    if(This->update_frames > 0){
        hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames, (BYTE **)&This->buf);
        if(FAILED(hr)){
            WARN("GetBuffer failed: %08lx\n", hr);
            This->update_frames = ~0;
            LeaveCriticalSection(&This->lock);
            /* No free packet, i.e. the caller is late.  Windows reports the
             * buffer-size code for the same state.  The two codes the driver
             * produces here describe a ring the spatial caller cannot see and
             * never asked for. */
            if(hr == AUDCLNT_E_BUFFER_TOO_LARGE || hr == AUDCLNT_E_OUT_OF_ORDER)
                return AUDCLNT_E_BUFFER_SIZE_ERROR;
            return hr;
        }

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            /* An object that has never been written is still all zeroes: buf
             * is calloc'd at activation and SAO_GetBuffer is its only writer.
             * Clearing only the voices in use keeps a pre-activated pool off
             * this loop. */
            if(object->started)
                memset(object->buf, 0, This->update_frames *
                        This->sa_client->object_fmtex.Format.nBlockAlign);
            object->updated = FALSE;
        }
    }else if (!fixme_once){
        fixme_once = TRUE;
        FIXME("Zero frame update.\n");
    }

    /* The grant, not the remaining capacity: Windows answers this from the
     * server's per-pass grant, which activation never decrements, so an app
     * holding every object it was granted still reads the full number.
     * Reporting the remainder made Dying Light 2 read 0 on its second pass
     * and rebuild its whole spatial sink once per frame, in silence, forever.
     * Activation stays the gate; see SAORS_ActivateSpatialAudioObject. */
    *dyn_count = This->dyn_max;
    *frames = This->update_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

/* canonical 7.1.4 bed directions plus the bottom quad and back-center, in Steam
 * Audio listener space (+x right, +y up, -z ahead); returns FALSE for the only
 * non-directional channel (LFE), which bypasses the HRTF and sums equally to
 * both ears. */
static BOOL bed_object_position(AudioObjectType type, float pos[3])
{
    switch(type){
    case AudioObjectType_FrontLeft:     pos[0]=-0.5f; pos[1]=0.0f;   pos[2]=-0.866f; return TRUE;
    case AudioObjectType_FrontRight:    pos[0]= 0.5f; pos[1]=0.0f;   pos[2]=-0.866f; return TRUE;
    case AudioObjectType_FrontCenter:   pos[0]= 0.0f; pos[1]=0.0f;   pos[2]=-1.0f;   return TRUE;
    case AudioObjectType_SideLeft:      pos[0]=-1.0f; pos[1]=0.0f;   pos[2]= 0.0f;   return TRUE;
    case AudioObjectType_SideRight:     pos[0]= 1.0f; pos[1]=0.0f;   pos[2]= 0.0f;   return TRUE;
    case AudioObjectType_BackLeft:      pos[0]=-0.5f; pos[1]=0.0f;   pos[2]= 0.866f; return TRUE;
    case AudioObjectType_BackRight:     pos[0]= 0.5f; pos[1]=0.0f;   pos[2]= 0.866f; return TRUE;
    case AudioObjectType_TopFrontLeft:  pos[0]=-0.5f; pos[1]=0.707f; pos[2]=-0.5f;   return TRUE;
    case AudioObjectType_TopFrontRight: pos[0]= 0.5f; pos[1]=0.707f; pos[2]=-0.5f;   return TRUE;
    case AudioObjectType_TopBackLeft:   pos[0]=-0.5f; pos[1]=0.707f; pos[2]= 0.5f;   return TRUE;
    case AudioObjectType_TopBackRight:  pos[0]= 0.5f; pos[1]=0.707f; pos[2]= 0.5f;   return TRUE;
    case AudioObjectType_BottomFrontLeft:  pos[0]=-0.5f; pos[1]=-0.707f; pos[2]=-0.5f;  return TRUE;
    case AudioObjectType_BottomFrontRight: pos[0]= 0.5f; pos[1]=-0.707f; pos[2]=-0.5f;  return TRUE;
    case AudioObjectType_BottomBackLeft:   pos[0]=-0.5f; pos[1]=-0.707f; pos[2]= 0.5f;  return TRUE;
    case AudioObjectType_BottomBackRight:  pos[0]= 0.5f; pos[1]=-0.707f; pos[2]= 0.5f;  return TRUE;
    case AudioObjectType_BackCenter:    pos[0]= 0.0f; pos[1]=0.0f;   pos[2]= 1.0f;   return TRUE;
    default:                            pos[0]= 0.0f; pos[1]=0.0f;   pos[2]= 0.0f;   return FALSE;
    }
}

/* mono LFE summed equally into both stereo output channels, bypassing the HRTF */
static void mix_lfe_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float *in = object->buf, *out = stream->buf;
    float g = 0.5f * object->volume;
    UINT32 nch = stream->stream_fmtex.Format.nChannels, i;
    for(i = 0; i < stream->update_frames; ++i){
        out[stream->dyn_left]  += in[i] * g;
        out[stream->dyn_right] += in[i] * g;
        out += nch;
    }
}

/* WAVEFORMATEXTENSIBLE has no bottom speaker positions, so a bottom bed channel
 * cannot be named in an endpoint mask at all.  Windows resolves that by folding
 * each one into its nearest non-bottom neighbour at -3.0000 dB, and this is the
 * one static path that is therefore not bit-transparent: two objects sum into
 * the fold target's channel.  Nothing downstream of us bounds that sum, which is
 * what the device-pipe limiter does on Windows. */
#define SPATIAL_BOTTOM_FOLD 0.70794576f

static float static_fold_gain(AudioObjectType type)
{
    switch(type){
    case AudioObjectType_BottomFrontLeft:
    case AudioObjectType_BottomFrontRight:
    case AudioObjectType_BottomBackLeft:
    case AudioObjectType_BottomBackRight:
        return SPATIAL_BOTTOM_FOLD;
    default:
        return 1.0f;
    }
}

static void mix_static_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float *in = object->buf, *out, vol;
    UINT32 i;
    if(object->static_idx == ~0 ||
            stream->static_object_map[object->static_idx] == ~0){
        WARN("Got unmapped static object?! Not mixing. Type: 0x%x\n", object->type);
        return;
    }
    out = stream->buf + stream->static_object_map[object->static_idx];
    vol = object->volume * static_fold_gain(object->type);
    for(i = 0; i < stream->update_frames; ++i){
        *out += *in * vol;
        ++in;
        out += stream->stream_fmtex.Format.nChannels;
    }
}

/* lateral equal-power pan; front/back and elevation collapse onto the x axis,
 * the HRTF backend replaces this mixer */
static void mix_dynamic_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float len, p, gl, gr;
    float *in = object->buf, *out = stream->buf;
    UINT32 nch = stream->stream_fmtex.Format.nChannels, i;

    len = sqrtf(object->pos[0] * object->pos[0] +
                object->pos[1] * object->pos[1] +
                object->pos[2] * object->pos[2]);
    p = len > 0.0f ? object->pos[0] / len : 0.0f;
    gl = cosf((p + 1.0f) * 0.78539816f) * object->volume;
    gr = sinf((p + 1.0f) * 0.78539816f) * object->volume;

    for(i = 0; i < stream->update_frames; ++i){
        out[stream->dyn_left] += in[i] * gl;
        out[stream->dyn_right] += in[i] * gr;
        out += nch;
    }
}

static float spatial_channel_dbfs(const float *buf, UINT32 frames)
{
    double s = 0.0;
    UINT32 i;
    if(!frames) return SPATIAL_DB_FLOOR;
    for(i = 0; i < frames; ++i) s += (double)buf[i] * buf[i];
    s = sqrt(s / frames);
    /* The buffer is the application's, so it holds whatever the title wrote,
     * and a NaN fails every comparison: testing only for the quiet case with
     * <= let a NaN through and published it as the channel's level.  Require
     * a finite sum instead, so a NaN or an infinity degrades to the floor the
     * way the driver's peak scan already degrades a NaN, rather than to a
     * value no consumer can plot, compare or average. */
    if(!isfinite(s) || s <= 1e-6) return SPATIAL_DB_FLOOR;
    return (float)(20.0 * log10(s));
}

static BOOL spatial_hud_claim(SpatialAudioStreamImpl *stream)
{
    void *prev;

    if(hud_off) return FALSE;
    prev = InterlockedCompareExchangePointer((void **)&hud_stream, stream, NULL);
    if(!prev || prev == stream) return TRUE;
    if(!InterlockedExchange(&hud_multi_warned, 1))
        WARN("More than one spatial stream is active; the snapshot follows %p only.\n", prev);
    return FALSE;
}

/* 10 Hz, counted down in frames rather than off a clock so the audio thread
 * reads no timer.  Publishing straight from the mix costs one unixlib
 * transition per 100 ms and needs no thread of its own, and the values are
 * consistent by construction because this runs under the stream lock that
 * produced them.  Publishing every period instead would double the
 * transitions on this path for freshness no reader asked for.
 *
 * The counter starts at 0, so a freshly elected stream publishes on its first
 * update: waiting a tenth of a second first would leave a new stream reading
 * "never published", and a stream that lives for less than that, which the
 * churn probe produces by design, would never appear at all. */
static BOOL spatial_hud_due(SpatialAudioStreamImpl *stream)
{
    UINT32 rate = stream->stream_fmtex.Format.nSamplesPerSec;

    if(!rate || !spatial_hud_claim(stream)) return FALSE;
    if(stream->hud_frames >= stream->update_frames){
        stream->hud_frames -= stream->update_frames;
        return FALSE;
    }
    stream->hud_frames = rate / 10;
    return TRUE;
}

/* One dBFS pass computes the levels the HUD publish consumes; the audio thread
 * never computes them twice. */
static void spatial_hud_update(SpatialAudioStreamImpl *stream)
{
    struct spatial_hud_params hud;
    SpatialAudioObjectImpl *object;
    BOOL unix_up;
    int i;

    if(!spatial_hud_due(stream)) return;

    for(i = 0; i < SPATIAL_BED_MAX; ++i) hud.bed_db[i] = SPATIAL_DB_FLOOR;
    hud.bed_mask = 0;
    hud.bed_truncated = 0;
    LIST_FOR_EACH_ENTRY(object, &stream->objects, SpatialAudioObjectImpl, entry){
        if(object->invalidated || object->type == AudioObjectType_Dynamic)
            continue;
        /* The bounds guard and the published bit are the same test, so they
         * cannot drift: a bed wider than the wire array is reported, not
         * silently dropped. */
        if(object->static_idx >= SPATIAL_BED_MAX){
            hud.bed_truncated = 1;
            continue;
        }
        hud.bed_db[object->static_idx] =
                spatial_channel_dbfs(object->buf, stream->update_frames);
        hud.bed_mask |= 1u << object->static_idx;
    }
    hud.hrtf = stream->engine != 0;
    hud.bed_virtualized = stream->virtualize_bed;
    hud.dyn_live = stream->dyn_live;
    hud.dyn_max = stream->dyn_max;
    hud.announce = 0;
    hud.clip_samples = stream->clip_samples;
    hud.clip_total = stream->clip_total;
    hud.clip_passes = stream->clip_passes;
    hud.bus_passes = stream->bus_passes;
    hud.clip_engagements = stream->clip_engagements;
    hud.clip_nonfinite = stream->clip_nonfinite;
    /* One log per publish, off the sample loop.  Exactly 0.0 has to mean
     * "never over", so a peak that never reached full scale must not fall
     * through log10 and land on a small negative. */
    hud.clip_peak_db = stream->clip_peak > 1.0f ?
            (float)(20.0 * log10(stream->clip_peak)) : 0.0f;
    hud.pad = 0;

    /* Activation has already initialised the unixlib for every stream
     * that reaches here, so this is a cached flag read.  It stays to hold
     * "never dispatch without a handle" at the call site: a stream that
     * dispatches on a zero handle faults inside the unix-call frame, and
     * the ignored STATUS_ACCESS_VIOLATION latches hud_off process-wide. */
    unix_up = spatial_unix_init();

    hud.enabled = 0;
    if(unix_up)
        WINE_UNIX_CALL(unix_spatial_hud_publish, &hud);
    if(!hud.enabled){
        if(!InterlockedExchange(&hud_off, 1))
            WARN_(spatial)("Section B is inert for this process: %s.\n", unix_up ?
                    "the driver published no snapshot to write into" :
                    "the mmdevapi unixlib did not initialise");
        InterlockedCompareExchangePointer((void **)&hud_stream, NULL, stream);
    }
}

/* The binaural stage returns more power than it was handed, by a constant that
 * belongs to the engine and not to the content.  Measured two ways that agree:
 * +2.89 dB as a power-weighted mean over the eleven fixed bed directions, and
 * +2.79 dB over 48 directions on a uniform sphere, the latter reachable only
 * through dynamic objects because the bed cannot be steered.  Anything in
 * [-2.89, -2.79] is indistinguishable against the 0.28 dB run-to-run spread of
 * the rig that measured it, so the midpoint ships.
 *
 * The constant is not sensitive to that scope; it is sensitive to where the
 * content sits.  The horizontal band alone measures +3.47 dB and the upper
 * hemisphere +2.32, so a title holding its objects near ear level is
 * under-padded by roughly half a decibel.  That is a chosen constant over a
 * 7.15 dB per-direction spread, not a derived one.
 *
 * user_reports_logs/spatial-publish-and-multistream.md, BUG 4. */
#define SPATIAL_HRTF_PAD 0.72027775f    /* -2.85 dB */

static HRESULT WINAPI SAORS_EndUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr = S_OK;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->update_frames == ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->update_frames > 0){
        struct spatial_mix_object mix_objs[SPATIAL_MAX_SLOTS];
        UINT32 mix_count = 0, i;
        /* Set by every mixer that writes to dyn_left/dyn_right.  Only
         * mix_static_object writes anywhere else, so this flag is exactly
         * "something non-transparent landed on the bus this tick". */
        BOOL bus_dirty = FALSE;

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(object->invalidated && !object->eos_pending)
                continue;
            /* frames past the end of the stream are not part of it */
            if(object->eos_pending && object->eos_frames < This->update_frames)
                memset(object->buf + object->eos_frames, 0,
                        (This->update_frames - object->eos_frames) * sizeof(float));
            if(This->engine && object->engine_slot != ~0 &&
                    mix_count < SPATIAL_MAX_SLOTS){
                mix_objs[mix_count].buffer = (UINT_PTR)object->buf;
                mix_objs[mix_count].slot = object->engine_slot;
                memcpy(mix_objs[mix_count].pos, object->pos, sizeof(object->pos));
                /* Scope is structural rather than conditional: this is the
                 * only line feeding iplBinauralEffectApply, so LFE, an object
                 * that got no engine slot, one that arrived after the slot
                 * table filled, and the whole engine-refused fallback are each
                 * excluded by taking a different branch below.  None of them
                 * incurred the HRIR gain, so none of them may be padded, and
                 * no test here can drift out of step with the engine's own. */
                mix_objs[mix_count].volume = object->volume * SPATIAL_HRTF_PAD;
                mix_count++;
            }else if(object->type == AudioObjectType_Dynamic){
                mix_dynamic_object(This, object);
                bus_dirty = TRUE;
            }else if(This->virtualize_bed){
                if(object->type == AudioObjectType_LowFrequency)
                    mix_lfe_object(This, object);
                else
                    mix_dynamic_object(This, object);
                bus_dirty = TRUE;
            }else{
                mix_static_object(This, object);
            }
        }

        if(mix_count){
            struct spatial_mix_params params;
            memset(This->hrtf_buf, 0, 2 * This->update_frames * sizeof(float));
            params.handle = This->engine;
            params.frames = This->update_frames;
            params.count = mix_count;
            params.objects = (UINT_PTR)mix_objs;
            params.out_l = (UINT_PTR)This->hrtf_buf;
            params.out_r = (UINT_PTR)(This->hrtf_buf + This->update_frames);
            if(!WINE_UNIX_CALL(unix_spatial_mix, &params)){
                UINT32 nch = This->stream_fmtex.Format.nChannels;
                for(i = 0; i < This->update_frames; ++i){
                    This->buf[i * nch + This->dyn_left] += This->hrtf_buf[i];
                    This->buf[i * nch + This->dyn_right] += This->hrtf_buf[This->update_frames + i];
                }
                bus_dirty = TRUE;
            }else{
                /* engine refused the tick, fall back to panning */
                LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
                    if((!object->invalidated || object->eos_pending) &&
                            object->engine_slot != ~0)
                        mix_dynamic_object(This, object);
                }
                bus_dirty = TRUE;
            }
        }

        /* Bound the two channels the engine and the panning mixers share.
         * Several objects sum into each of them and nothing upstream limits
         * the result, so ordinary content reaches the endpoint above full
         * scale: measured on two real titles, up to +9.9 dB after the pad.
         *
         * Scoped to those channels, and only when something mixed into them
         * this tick, because mix_static_object is exactly transparent - one
         * bed channel to one endpoint channel, nothing summed - and that
         * transparency is load-bearing.  It is the calibration reference every
         * device measurement in BUG 4 is read against, and it is what a
         * passthrough title is entitled to.  Widening this to the whole buffer
         * would look like a consistency fix and would destroy both.
         *
         * A hard clip and not a soft knee or a limiter, both measured and both
         * rejected: the knee is no cleaner on any in-band metric and turns an
         * infinite input into a NaN, and a limiter is worse than this on real
         * content below about +5 dB of overshoot.  The residual distortion
         * here is bounded but real; nothing at this stage makes content that
         * far over full scale clean. */
        if(bus_dirty){
            UINT32 nch = This->stream_fmtex.Format.nChannels;
            UINT32 clipped = 0, nonfinite = 0;
            float worst = 0.0f;

            for(i = 0; i < This->update_frames; ++i){
                float *l = &This->buf[i * nch + This->dyn_left];
                float *r = &This->buf[i * nch + This->dyn_right];

                /* A NaN passes both compares, and SPA's f32-to-int conversion
                 * is fminf(fmaxf(v, low), high), whose fmaxf returns low for a
                 * NaN, so an unguarded one leaves here and arrives at the
                 * endpoint as full-scale negative.  Zero, not a rail: a rail
                 * is the loudest possible wrong answer.
                 *
                 * The counters ride branches that already exist, so a sample
                 * inside full scale costs exactly what it did before. */
                if(!isfinite(*l)){ *l = 0.0f; nonfinite++; }
                else if(*l > 1.0f){ if(*l > worst) worst = *l; *l = 1.0f; clipped++; }
                else if(*l < -1.0f){ if(-*l > worst) worst = -*l; *l = -1.0f; clipped++; }
                if(!isfinite(*r)){ *r = 0.0f; nonfinite++; }
                else if(*r > 1.0f){ if(*r > worst) worst = *r; *r = 1.0f; clipped++; }
                else if(*r < -1.0f){ if(-*r > worst) worst = -*r; *r = -1.0f; clipped++; }
            }

            /* Two bus channels, so the denominator is twice the frames: the
             * published ratio is of samples the clip could have truncated and
             * not of the whole stream, which would flatter us by the bed
             * width. */
            This->clip_total += (UINT64)This->update_frames * 2;
            This->clip_samples += clipped;
            This->clip_nonfinite += nonfinite;
            This->bus_passes++;
            if(clipped){
                This->clip_passes++;
                if(!This->clip_engaged) This->clip_engagements++;
            }
            This->clip_engaged = clipped != 0;
            if(worst > This->clip_peak) This->clip_peak = worst;
        }else{
            /* Nothing on the bus ends an engagement rather than suspending it,
             * so a burst either side of a silent gap counts as two. */
            This->clip_engaged = FALSE;
        }
        spatial_hud_update(This);

        /* an object whose lifetime has started and that misses an update
         * cycle is invalidated */
        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(object->started && !object->updated)
                object->invalidated = TRUE;
            object->eos_pending = FALSE;
        }

        hr = IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        if(FAILED(hr))
            WARN("ReleaseBuffer failed: %08lx\n", hr);
    }

    /* Cleared on every path as on Windows, but the commit failure belongs to
     * the caller: after an endpoint loss this is the call that discovers it,
     * and reporting S_OK buys the title one more period of rendering into a
     * dead device. */
    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    return hr;
}

static HRESULT WINAPI SAORS_ActivateSpatialAudioObject(ISpatialAudioObjectRenderStream *iface,
        AudioObjectType type, ISpatialAudioObject **object)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *obj;
    HRESULT hr = S_OK;

    TRACE("(%p)->(0x%x, %p)\n", This, type, object);

    /* Allocated before the lock so the critical section stays short. The
     * admission tests below have to run inside it: dyn_live and the object
     * list are both mutated by SAO_Release, so reading them outside races a
     * concurrent release of a sibling object. */
    if(!(obj = calloc(1, sizeof(*obj))))
        return E_OUTOFMEMORY;
    if(!(obj->buf = calloc(This->period_frames,
            This->sa_client->object_fmtex.Format.nBlockAlign))){
        free(obj);
        return E_OUTOFMEMORY;
    }
    obj->ISpatialAudioObject_iface.lpVtbl = &ISpatialAudioObject_vtbl;
    obj->ref = 1;
    obj->type = type;
    obj->volume = 1.0f;
    obj->engine_slot = ~0;
    if(type == AudioObjectType_None){
        FIXME("AudioObjectType_None not implemented yet!\n");
        obj->static_idx = ~0;
    }else{
        obj->static_idx = AudioObjectType_to_index(type);
    }
    obj->sa_stream = This;

    EnterCriticalSection(&This->lock);

    if(type == AudioObjectType_Dynamic){
        if(This->dyn_live >= This->dyn_max){
            WARN("No dynamic object slots available (%u live, %u max, spatial sound %s).\n",
                    This->dyn_live, This->dyn_max, This->sa_client->dyn_budget ? "on" : "off");
            hr = SPTLAUDCLNT_E_NO_MORE_OBJECTS;
        }
    }else if(type & ~This->params.StaticObjectTypeMask){
        hr = SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
    }else if(type != AudioObjectType_None){
        SpatialAudioObjectImpl *other;

        /* StaticObjectTypeMask is app-supplied and unfiltered, so it can admit
         * a type that maps to no bed channel */
        if(obj->static_idx == ~0)
            hr = SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
        else LIST_FOR_EACH_ENTRY(other, &This->objects, SpatialAudioObjectImpl, entry){
            if(other->static_idx == obj->static_idx){
                hr = SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
                break;
            }
        }
    }

    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        free(obj->buf);
        free(obj);
        return hr;
    }

    if(type == AudioObjectType_Dynamic){
        This->dyn_live++;
        if(This->engine){
            struct spatial_object_add_params params;
            params.handle = This->engine;
            if(!WINE_UNIX_CALL(unix_spatial_object_add, &params))
                obj->engine_slot = params.slot;
            else
                WARN("No HRTF effect slot for dynamic object %p, will use panning.\n", obj);
        }
    }else if(This->virtualize_bed){
        if(bed_object_position(type, obj->pos) && This->engine){
            struct spatial_object_add_params params;
            params.handle = This->engine;
            if(!WINE_UNIX_CALL(unix_spatial_object_add, &params))
                obj->engine_slot = params.slot;
            else
                WARN("No HRTF effect slot for bed channel 0x%x, will use panning.\n", type);
        }
    }
    list_add_tail(&This->objects, &obj->entry);
    SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);

    LeaveCriticalSection(&This->lock);

    *object = &obj->ISpatialAudioObject_iface;

    return S_OK;
}

static ISpatialAudioObjectRenderStreamVtbl ISpatialAudioObjectRenderStream_vtbl = {
    SAORS_QueryInterface,
    SAORS_AddRef,
    SAORS_Release,
    SAORS_GetAvailableDynamicObjectCount,
    SAORS_GetService,
    SAORS_Start,
    SAORS_Stop,
    SAORS_Reset,
    SAORS_BeginUpdatingAudioObjects,
    SAORS_EndUpdatingAudioObjects,
    SAORS_ActivateSpatialAudioObject,
};

static HRESULT WINAPI SAC_QueryInterface(ISpatialAudioClient *iface, REFIID riid, void **ppv)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioClient)) {
        *ppv = &This->ISpatialAudioClient_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAC_AddRef(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAC_Release(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref) {
        IMMDevice_Release(This->mmdev);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAC_GetStaticObjectPosition(ISpatialAudioClient *iface,
        AudioObjectType type, float *x, float *y, float *z)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    float pos[3];

    TRACE("(%p)->(0x%x, %p, %p, %p)\n", This, type, x, y, z);

    if(!x || !y || !z)
        return E_INVALIDARG;

    /* Report the same canonical 7.1.4 directions the HRTF mixer renders each bed
     * channel to, so a probing title reads back exactly where the channel sits;
     * non-directional types (LFE) report the listener origin. */
    bed_object_position(type, pos);
    *x = pos[0];
    *y = pos[1];
    *z = pos[2];

    return S_OK;
}

static HRESULT WINAPI SAC_GetNativeStaticObjectTypeMask(ISpatialAudioClient *iface,
        AudioObjectType *mask)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, mask);

    if(!mask)
        return E_INVALIDARG;

    /* Every static type the bed mixer can place: static_mask_to_channels maps
     * all seventeen and bed_object_position has a direction for each of the
     * sixteen directional ones.  0x3fffe is also a real Windows value, the
     * mask reported for an unrecognised encoder, and no Windows configuration
     * reports a plain 7.1.  The documented pattern is to intersect the desired
     * bed with this mask before activating, so under-advertising costs a title
     * exactly the height channels that make Atmos content sound spatial. */
    *mask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight |
            AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft | AudioObjectType_SideRight |
            AudioObjectType_BackLeft | AudioObjectType_BackRight |
            AudioObjectType_TopFrontLeft | AudioObjectType_TopFrontRight |
            AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight |
            AudioObjectType_BottomFrontLeft | AudioObjectType_BottomFrontRight |
            AudioObjectType_BottomBackLeft | AudioObjectType_BottomBackRight |
            AudioObjectType_BackCenter;

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxDynamicObjectCount(ISpatialAudioClient *iface,
        UINT32 *value)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, value);

    if(!value)
        return E_INVALIDARG;

    *value = This->dyn_budget;

    return S_OK;
}

static HRESULT WINAPI SAC_GetSupportedAudioObjectFormatEnumerator(
        ISpatialAudioClient *iface, IAudioFormatEnumerator **enumerator)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, enumerator);

    if(!enumerator)
        return E_POINTER;

    *enumerator = &This->IAudioFormatEnumerator_iface;
    SAC_AddRef(iface);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format);

static HRESULT WINAPI SAC_GetMaxFrameCount(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    HRESULT hr;

    /* FIXME: should get device period from the device */
    static const REFERENCE_TIME period = 100000;

    TRACE("(%p)->(%p, %p)\n", This, format, count);

    if(!count)
        return E_POINTER;

    *count = 0;

    /* Windows reaches the period only once the format has passed the support
     * test, so a format it would refuse gets that refusal here instead of a
     * frame count for a stream that cannot be opened.  The NULL format is that
     * test's answer to give, which is why only count is checked above. */
    if((hr = SAC_IsAudioObjectFormatSupported(iface, format)) != S_OK)
        return hr;

    *count = MulDiv(period, format->nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format)
{
    SpatialAudioImpl *sac = impl_from_ISpatialAudioClient(iface);
    HRESULT hr;

    TRACE("sac %p, format %s.\n", sac, debugstr_fmtex(format));

    if (!format)
        return E_POINTER;

    /* Shape first, then support, because the two answer with different codes:
     * a caller that distinguishes AUDCLNT_E_UNSUPPORTED_FORMAT (renegotiate)
     * from E_INVALIDARG (its own bug) takes the wrong branch otherwise. */
    if ((hr = validate_wave_format_ex(format)) != S_OK)
        return hr;

    if (!object_formats_compatible(&sac->object_fmtex.Format, format))
    {
        TRACE("Reporting format %s as unsupported.\n", debugstr_fmtex(format));
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_IsSpatialAudioStreamAvailable(ISpatialAudioClient *iface,
        REFIID stream_uuid, const PROPVARIANT *info)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);

    /* No dynamic-object gate on any of the three capability queries: our own
     * activation path does not consult the budget either, so gating them
     * denied availability and then granted the stream for it.  Windows answers
     * this one with no device test at all. */
    if(IsEqualIID(stream_uuid, &IID_ISpatialAudioObjectRenderStream))
        return S_OK;

    WARN("Unsupported spatial stream %s.\n", debugstr_guid(stream_uuid));
    return SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE;
}

static WAVEFORMATEX *clone_fmtex(const WAVEFORMATEX *src)
{
    WAVEFORMATEX *r = malloc(sizeof(WAVEFORMATEX) + src->cbSize);
    memcpy(r, src, sizeof(WAVEFORMATEX) + src->cbSize);
    return r;
}

static void static_mask_to_channels(AudioObjectType static_mask, WORD *count, DWORD *mask, UINT32 *map)
{
    UINT32 out_chan = 0, map_idx = 0;
    *count = 0;
    *mask = 0;
#define CONVERT_MASK(f, t) \
    if(static_mask & f){ \
        *count += 1; \
        *mask |= t; \
        map[map_idx++] = out_chan++; \
        TRACE("mapping 0x%x to %u\n", f, out_chan - 1); \
    }else{ \
        map[map_idx++] = ~0; \
    }
    /* The fold target is always mapped before its bottom channel, because both
     * loops run in AudioObjectType bit order.  A target missing from the mask
     * leaves ~0, which mix_static_object already reports and drops. */
#define FOLD_MASK(f, t) \
    if(static_mask & f){ \
        map[map_idx++] = map[AudioObjectType_to_index(t)]; \
        TRACE("folding 0x%x into 0x%x\n", f, t); \
    }else{ \
        map[map_idx++] = ~0; \
    }
    CONVERT_MASK(AudioObjectType_FrontLeft, SPEAKER_FRONT_LEFT);
    CONVERT_MASK(AudioObjectType_FrontRight, SPEAKER_FRONT_RIGHT);
    CONVERT_MASK(AudioObjectType_FrontCenter, SPEAKER_FRONT_CENTER);
    CONVERT_MASK(AudioObjectType_LowFrequency, SPEAKER_LOW_FREQUENCY);
    CONVERT_MASK(AudioObjectType_SideLeft, SPEAKER_SIDE_LEFT);
    CONVERT_MASK(AudioObjectType_SideRight, SPEAKER_SIDE_RIGHT);
    CONVERT_MASK(AudioObjectType_BackLeft, SPEAKER_BACK_LEFT);
    CONVERT_MASK(AudioObjectType_BackRight, SPEAKER_BACK_RIGHT);
    CONVERT_MASK(AudioObjectType_TopFrontLeft, SPEAKER_TOP_FRONT_LEFT);
    CONVERT_MASK(AudioObjectType_TopFrontRight, SPEAKER_TOP_FRONT_RIGHT);
    CONVERT_MASK(AudioObjectType_TopBackLeft, SPEAKER_TOP_BACK_LEFT);
    CONVERT_MASK(AudioObjectType_TopBackRight, SPEAKER_TOP_BACK_RIGHT);
    /* A bottom channel takes its fold target's endpoint channel rather than one
     * of its own, so nChannels stays equal to popcount(*mask).  Giving these
     * speaker bit 0 while still counting a channel made the two disagree, and
     * the driver then rejected the whole format, so a bed with any bottom
     * channel could not open at all. */
    FOLD_MASK(AudioObjectType_BottomFrontLeft, AudioObjectType_FrontLeft);
    FOLD_MASK(AudioObjectType_BottomFrontRight, AudioObjectType_FrontRight);
    FOLD_MASK(AudioObjectType_BottomBackLeft, AudioObjectType_BackLeft);
    FOLD_MASK(AudioObjectType_BottomBackRight, AudioObjectType_BackRight);
    CONVERT_MASK(AudioObjectType_BackCenter, SPEAKER_BACK_CENTER);
}

static HRESULT activate_stream(SpatialAudioStreamImpl *stream)
{
    WAVEFORMATEXTENSIBLE *object_fmtex = (WAVEFORMATEXTENSIBLE *)stream->params.ObjectFormat;
    HRESULT hr;
    REFERENCE_TIME period;
    UINT32 i;
    WAVEFORMATEX *mix_fmt = NULL;
    WORD bed_ch = 0;
    DWORD bed_mask = 0;
    UINT32 dev_ch = 0;
    AudioObjectType effective_mask;

    if(!(object_fmtex->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (object_fmtex->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(&object_fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))){
        FIXME("Only float formats are supported for now\n");
        return E_INVALIDARG;
    }

    hr = IMMDevice_Activate(stream->sa_client->mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&stream->client);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        return hr;
    }

    hr = IAudioClient_GetDevicePeriod(stream->client, &period, NULL);
    if(FAILED(hr)){
        WARN("GetDevicePeriod failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    effective_mask = stream->params.StaticObjectTypeMask ? stream->params.StaticObjectTypeMask :
            (AudioObjectType_FrontLeft | AudioObjectType_FrontRight);
    static_mask_to_channels(effective_mask, &bed_ch, &bed_mask, stream->static_object_map);

    if(SUCCEEDED(IAudioClient_GetMixFormat(stream->client, &mix_fmt))){
        dev_ch = mix_fmt->nChannels;
        CoTaskMemFree(mix_fmt);
    }

    /* HRTF-virtualize a surround bed only into a stereo endpoint (a headphone
     * target); real multichannel endpoints keep the passthrough downmix. */
    stream->virtualize_bed = stream->sa_client->dyn_budget && dev_ch && dev_ch <= 2 && bed_ch > 2;

    stream->stream_fmtex.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    if(stream->virtualize_bed){
        stream->stream_fmtex.Format.nChannels = 2;
        stream->stream_fmtex.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        stream->dyn_left = 0;
        stream->dyn_right = 1;
        /* Which backend virtualizes it is not known here: the engine is created
         * by the caller after this function returns, so naming HRTF at this
         * point would claim a backend that may never load. */
        TRACE("Bed virtualization requested: %u channels to stereo.\n", bed_ch);
    }else{
        /* The binaural bus has to land on real front-left and front-right
         * channels.  A bed omitting either leaves no correct index for it, and
         * falling back to 0 and 1 puts an ear wherever those happen to be: for
         * FrontLeft|LowFrequency|SideLeft|SideRight channel 1 is the LFE, so
         * the right ear of every dynamic object became bass-managed rumble.
         * Widen the bed by the missing pair instead.  The app never sees this
         * format, only its own object buffers, and the graph downmixes to the
         * device, so the cost is two channels on a stream that is broken
         * today.  Only a dynamic budget can put anything on the bus, so a
         * bed-only stream keeps exactly the channels it asked for.  Widening
         * also gives the Bottom* fold targets real channels, which is why a
         * folded bottom channel stops being dropped in this case.
         *
         * Windows never has to do this: its pipeline width comes from the
         * device mix format, a closed table of layouts that all carry the
         * front pair, and it writes the engine bus to channels 0 and 1 of
         * that. Deriving the format from the caller's bed mask is our
         * divergence, so keeping the pair present is our job. */
        if(stream->sa_client->dyn_budget &&
                (~bed_mask & (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT))){
            effective_mask |= AudioObjectType_FrontLeft | AudioObjectType_FrontRight;
            static_mask_to_channels(effective_mask, &bed_ch, &bed_mask,
                    stream->static_object_map);
            TRACE_(spatial)("bed lacks the front pair, widened to %u channels "
                    "(mask 0x%lx) so the binaural bus has somewhere to go.\n",
                    bed_ch, bed_mask);
        }
        stream->stream_fmtex.Format.nChannels = bed_ch;
        stream->stream_fmtex.dwChannelMask = bed_mask;
        i = stream->static_object_map[AudioObjectType_to_index(AudioObjectType_FrontLeft)];
        stream->dyn_left = i != ~0 ? i : 0;
        i = stream->static_object_map[AudioObjectType_to_index(AudioObjectType_FrontRight)];
        stream->dyn_right = i != ~0 ? i : (stream->stream_fmtex.Format.nChannels > 1 ? 1 : 0);
    }
    stream->stream_fmtex.Format.nSamplesPerSec = stream->params.ObjectFormat->nSamplesPerSec;
    stream->stream_fmtex.Format.wBitsPerSample = stream->params.ObjectFormat->wBitsPerSample;
    stream->stream_fmtex.Format.nBlockAlign = (stream->stream_fmtex.Format.nChannels * stream->stream_fmtex.Format.wBitsPerSample) / 8;
    stream->stream_fmtex.Format.nAvgBytesPerSec = stream->stream_fmtex.Format.nSamplesPerSec * stream->stream_fmtex.Format.nBlockAlign;
    stream->stream_fmtex.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    stream->stream_fmtex.Samples.wValidBitsPerSample = stream->stream_fmtex.Format.wBitsPerSample;
    stream->stream_fmtex.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    hr = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            period, 0, &stream->stream_fmtex.Format, NULL);
    if(FAILED(hr)){
        WARN("Initialize failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_SetEventHandle(stream->client, stream->params.EventHandle);
    if(FAILED(hr)){
        WARN("SetEventHandle failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_GetService(stream->client, &IID_IAudioRenderClient, (void**)&stream->render);
    if(FAILED(hr)){
        WARN("GetService(AudioRenderClient) failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->period_frames = MulDiv(period, stream->stream_fmtex.Format.nSamplesPerSec, 10000000);

    /* Everything here is known now: the widths, the mask, the budget, and that
     * virtualization was asked for.  Which backend serves it is not, because the
     * engine is created in SAC_ActivateSpatialAudioStream after this function
     * returns, so this line reports the request and leaves the backend to the two
     * places that know it, the engine attempt in the caller and SAORS_Start. */
    TRACE_(spatial)("stream %p configured: %u-channel bed -> %u-channel endpoint, %s, static mask 0x%x, dynamic budget %u.\n",
            stream, bed_ch, stream->stream_fmtex.Format.nChannels,
            stream->virtualize_bed ? "bed virtualization requested, backend chosen at activation"
                                   : "multichannel passthrough",
            effective_mask, stream->dyn_max);
    return S_OK;
}

static HRESULT WINAPI SAC_ActivateSpatialAudioStream(ISpatialAudioClient *iface,
        const PROPVARIANT *prop, REFIID riid, void **stream)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    SpatialAudioObjectRenderStreamActivationParams *params;
    HRESULT hr;

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), stream);

    /* Windows validates these two before it looks at the IID, and distinguishes
     * them: E_INVALIDARG for a NULL PROPVARIANT, E_POINTER for a NULL out
     * pointer, which it then zeroes before any other test can fail. */
    if(!prop)
        return E_INVALIDARG;
    if(!stream)
        return E_POINTER;
    *stream = NULL;

    if(IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)){
        SpatialAudioStreamImpl *obj;

        /* Windows accepts exactly two blob sizes, the v1 struct and v1 plus the
         * one UINT32 that SpatialAudioObjectRenderStreamActivationParams2 adds,
         * and rejects everything else with E_INVALIDARG.  Verified as 0x28/0x2c
         * on amd64 and 0x1c/0x20 on i386, so the rule is sizeof and sizeof + 4
         * rather than two literals.  We parse no options out of the v2 tail
         * because we implement none. */
        if(prop->vt != VT_BLOB || !prop->blob.pBlobData ||
                (prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams) &&
                 prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams) + sizeof(UINT32))){
            WARN("Got invalid params\n");
            *stream = NULL;
            return E_INVALIDARG;
        }

        params = (SpatialAudioObjectRenderStreamActivationParams*) prop->blob.pBlobData;

        if(params->StaticObjectTypeMask & AudioObjectType_Dynamic){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(params->EventHandle == INVALID_HANDLE_VALUE ||
                params->EventHandle == 0){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(!(params->ObjectFormat && object_formats_compatible(params->ObjectFormat, &This->object_fmtex.Format))) {
            *stream = NULL;
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        if(params->MinDynamicObjectCount > This->dyn_budget){
            WARN("MinDynamicObjectCount %u exceeds the budget of %u.\n",
                    params->MinDynamicObjectCount, This->dyn_budget);
            *stream = NULL;
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        obj = calloc(1, sizeof(SpatialAudioStreamImpl));

        obj->ISpatialAudioObjectRenderStream_iface.lpVtbl = &ISpatialAudioObjectRenderStream_vtbl;
        obj->ref = 1;
        memcpy(&obj->params, params, sizeof(obj->params));

        obj->update_frames = ~0;
        obj->dyn_max = params->MaxDynamicObjectCount < This->dyn_budget ?
                params->MaxDynamicObjectCount : This->dyn_budget;

        InitializeCriticalSection(&obj->lock);
        list_init(&obj->objects);

        obj->sa_client = This;
        SAC_AddRef(&This->ISpatialAudioClient_iface);

        obj->params.ObjectFormat = clone_fmtex(obj->params.ObjectFormat);

        DuplicateHandle(GetCurrentProcess(), obj->params.EventHandle,
                GetCurrentProcess(), &obj->params.EventHandle, 0, FALSE,
                DUPLICATE_SAME_ACCESS);

        if(obj->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_AddRef(obj->params.NotifyObject);

        if(TRACE_ON(mmdevapi)){
            TRACE("ObjectFormat: {%s}\n", debugstr_fmtex(obj->params.ObjectFormat));
            TRACE("StaticObjectTypeMask: 0x%x\n", obj->params.StaticObjectTypeMask);
            TRACE("MinDynamicObjectCount: 0x%x\n", obj->params.MinDynamicObjectCount);
            TRACE("MaxDynamicObjectCount: 0x%x\n", obj->params.MaxDynamicObjectCount);
            TRACE("Category: 0x%x\n", obj->params.Category);
            TRACE("EventHandle: %p\n", obj->params.EventHandle);
            TRACE("NotifyObject: %p\n", obj->params.NotifyObject);
        }

        hr = activate_stream(obj);
        if(FAILED(hr)){
            if(obj->params.NotifyObject)
                ISpatialAudioObjectRenderStreamNotify_Release(obj->params.NotifyObject);
            DeleteCriticalSection(&obj->lock);
            free((void*)obj->params.ObjectFormat);
            CloseHandle(obj->params.EventHandle);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            *stream = NULL;
            return hr;
        }

        /* Initialise the unixlib for every spatial stream, not only one that
         * wants an engine, because the section B publish on the mix path needs
         * the handle too.  Doing it here keeps the one-time
         * __wine_init_unix_call off the application's audio thread, and it
         * widens nothing: mmdevapi.so loads once per process, and any process
         * that ever publishes would have loaded it on its first publish. It
         * does not pull in libphonon, which spatial_init dlopens behind its
         * own pthread_once (spatial.c:208). */
        if(spatial_unix_init()){
            /* Announce that a spatial stream exists before it renders
             * anything, so a reader can tell "no spatial client in this
             * process" from "one exists and is not publishing"; both read as
             * seq_sp == 0, and the benign one got chased as a fault.  Off the
             * mix path, once per stream, and skipped once hud_off has latched
             * because nothing is listening then.  It sits outside the engine
             * test below because a stream is visible whether or not it ever
             * asks for an engine. */
            if(!hud_off){
                struct spatial_hud_params ann;
                memset(&ann, 0, sizeof(ann));
                ann.announce = 1;
                WINE_UNIX_CALL(unix_spatial_hud_publish, &ann);
            }

            if(obj->dyn_max || obj->virtualize_bed){
                struct spatial_init_params init_params;
                init_params.rate = obj->stream_fmtex.Format.nSamplesPerSec;
                init_params.frames = obj->period_frames;
                init_params.handle = 0;
                if(!WINE_UNIX_CALL(unix_spatial_init, &init_params) &&
                        (obj->hrtf_buf = calloc(2 * obj->period_frames, sizeof(float)))){
                    obj->engine = init_params.handle;
                    TRACE_(spatial)("Using the Steam Audio HRTF engine "
                            "(bed virtualization %s, up to %u dynamic objects).\n",
                            obj->virtualize_bed ? "on" : "off", obj->dyn_max);
                }
                else if(init_params.handle){
                    struct spatial_release_params release_params;
                    release_params.handle = init_params.handle;
                    WINE_UNIX_CALL(unix_spatial_release, &release_params);
                }
                if(!obj->engine)
                    WARN_(spatial)("HRTF engine unavailable, %s will use stereo panning.\n",
                            obj->virtualize_bed ? "the bed and any dynamic objects"
                                                : "dynamic objects");
            }
        }

        *stream = &obj->ISpatialAudioObjectRenderStream_iface;
    }else{
        FIXME("Unsupported audio stream IID: %s\n", debugstr_guid(riid));
        *stream = NULL;
        return E_NOTIMPL;
    }

    return S_OK;
}

static ISpatialAudioClientVtbl ISpatialAudioClient_vtbl = {
    SAC_QueryInterface,
    SAC_AddRef,
    SAC_Release,
    SAC_GetStaticObjectPosition,
    SAC_GetNativeStaticObjectTypeMask,
    SAC_GetMaxDynamicObjectCount,
    SAC_GetSupportedAudioObjectFormatEnumerator,
    SAC_GetMaxFrameCount,
    SAC_IsAudioObjectFormatSupported,
    SAC_IsSpatialAudioStreamAvailable,
    SAC_ActivateSpatialAudioStream,
};

static HRESULT WINAPI SAOFE_QueryInterface(IAudioFormatEnumerator *iface,
        REFIID riid, void **ppvObject)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_QueryInterface(&This->ISpatialAudioClient_iface, riid, ppvObject);
}

static ULONG WINAPI SAOFE_AddRef(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_AddRef(&This->ISpatialAudioClient_iface);
}

static ULONG WINAPI SAOFE_Release(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_Release(&This->ISpatialAudioClient_iface);
}

static HRESULT WINAPI SAOFE_GetCount(IAudioFormatEnumerator *iface, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%p)\n", This, count);

    if(!count)
        return E_POINTER;

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI SAOFE_GetFormat(IAudioFormatEnumerator *iface,
        UINT32 index, WAVEFORMATEX **format)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%u, %p)\n", This, index, format);

    if(!format)
        return E_POINTER;

    if(index > 0)
        return E_INVALIDARG;

    *format = &This->object_fmtex.Format;

    return S_OK;
}

static IAudioFormatEnumeratorVtbl IAudioFormatEnumerator_vtbl = {
    SAOFE_QueryInterface,
    SAOFE_AddRef,
    SAOFE_Release,
    SAOFE_GetCount,
    SAOFE_GetFormat,
};

HRESULT SpatialAudioClient_Create(IMMDevice *mmdev, ISpatialAudioClient **out)
{
    SpatialAudioImpl *obj;

    obj = calloc(1, sizeof(*obj));

    obj->ref = 1;
    obj->ISpatialAudioClient_iface.lpVtbl = &ISpatialAudioClient_vtbl;
    obj->IAudioFormatEnumerator_iface.lpVtbl = &IAudioFormatEnumerator_vtbl;
    obj->dyn_budget = get_spatial_dynamic_budget();

    obj->object_fmtex.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    obj->object_fmtex.Format.nChannels = 1;
    obj->object_fmtex.Format.nSamplesPerSec = 48000;
    obj->object_fmtex.Format.wBitsPerSample = sizeof(float) * 8;
    obj->object_fmtex.Format.nBlockAlign = (obj->object_fmtex.Format.nChannels * obj->object_fmtex.Format.wBitsPerSample) / 8;
    obj->object_fmtex.Format.nAvgBytesPerSec = obj->object_fmtex.Format.nSamplesPerSec * obj->object_fmtex.Format.nBlockAlign;
    obj->object_fmtex.Format.cbSize = 0;

    obj->mmdev = mmdev;
    IMMDevice_AddRef(mmdev);

    *out = &obj->ISpatialAudioClient_iface;

    TRACE_(spatial)("client %p created (spatial sound %s, dynamic object budget %u).\n",
            obj, obj->dyn_budget ? "enabled" : "disabled", obj->dyn_budget);
    return S_OK;
}
