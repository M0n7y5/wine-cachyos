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
#define SPATIAL_MAX_MIX_OBJECTS (SPATIAL_MAX_DYNAMIC_OBJECTS + 16)  /* dynamic objects + up to 16 static bed channels */

static BOOL spatial_option_enabled(const WCHAR *value)
{
    return *value == 'y' || *value == 'Y' || *value == 't' || *value == 'T' || *value == '1';
}

static UINT get_spatial_dynamic_budget(void)
{
    WCHAR buf[16];
    DWORD size = sizeof(buf);
    HKEY key;
    BOOL enabled = FALSE;
    const char *source = "registry";

    if(GetEnvironmentVariableW(L"WINE_SPATIAL_SOUND", buf, ARRAY_SIZE(buf))){
        enabled = spatial_option_enabled(buf);
        source = "environment";
    }else if(RegOpenKeyW(HKEY_CURRENT_USER, L"Software\\Wine\\mmdevapi", &key) == ERROR_SUCCESS){
        if(RegQueryValueExW(key, L"SpatialSound", 0, NULL, (BYTE*)buf, &size) == ERROR_SUCCESS)
            enabled = spatial_option_enabled(buf);
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

static BOOL object_formats_compatible(const WAVEFORMATEX *fmt1, const WAVEFORMATEX *fmt2)
{
    /* packing fields (nBlockAlign, nAvgBytesPerSec, cbSize) are not validated by Windows */
    return fmt1->wFormatTag == fmt2->wFormatTag &&
           fmt1->nChannels == fmt2->nChannels &&
           fmt1->nSamplesPerSec == fmt2->nSamplesPerSec &&
           fmt1->wBitsPerSample == fmt2->wBitsPerSample;
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
    BOOL virtualize_bed;
    UINT32 dyn_left, dyn_right;

    UINT64 engine;
    float *hrtf_buf;

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
        if(This->type == AudioObjectType_Dynamic){
            This->sa_stream->dyn_live--;
            if(This->sa_stream->engine && This->engine_slot != ~0){
                struct spatial_object_remove_params params;
                params.handle = This->sa_stream->engine;
                params.slot = This->engine_slot;
                WINE_UNIX_CALL(unix_spatial_object_remove, &params);
            }
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

    This->updated = TRUE;

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

    This->invalidated = TRUE;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_IsActive(ISpatialAudioObject *iface, BOOL *active)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, active);

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

    if(This->type != AudioObjectType_Dynamic){
        FIXME("Volume on static objects not implemented.\n");
        return SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED;
    }

    EnterCriticalSection(&This->sa_stream->lock);
    This->volume = vol;
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

    EnterCriticalSection(&This->lock);
    *count = This->dyn_max - This->dyn_live;
    LeaveCriticalSection(&This->lock);
    return S_OK;
}

static HRESULT WINAPI SAORS_GetService(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **service)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(riid), service);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAORS_Start(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Start(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Start failed: %08lx\n", hr);
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

    hr = IAudioClient_Stop(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Stop failed: %08lx\n", hr);
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI SAORS_Reset(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Reset(This->client);
    if (hr == AUDCLNT_E_NOT_STOPPED)
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
            return hr;
        }

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            memset(object->buf, 0, This->update_frames * This->sa_client->object_fmtex.Format.nBlockAlign);
            object->updated = FALSE;
        }
    }else if (!fixme_once){
        fixme_once = TRUE;
        FIXME("Zero frame update.\n");
    }

    *dyn_count = This->dyn_max - This->dyn_live;
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
    UINT32 nch = stream->stream_fmtex.Format.nChannels, i;
    for(i = 0; i < stream->update_frames; ++i){
        out[stream->dyn_left]  += in[i] * 0.5f;
        out[stream->dyn_right] += in[i] * 0.5f;
        out += nch;
    }
}

static void mix_static_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float *in = object->buf, *out;
    UINT32 i;
    if(object->static_idx == ~0 ||
            stream->static_object_map[object->static_idx] == ~0){
        WARN("Got unmapped static object?! Not mixing. Type: 0x%x\n", object->type);
        return;
    }
    out = stream->buf + stream->static_object_map[object->static_idx];
    for(i = 0; i < stream->update_frames; ++i){
        *out += *in;
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

/* ----------------------------------------------------------------------
 * Per-channel level stats for an external debug overlay.  Opt-in via the
 * WINE_SPATIAL_STATS env var (a unix path); a dedicated writer thread emits a
 * one-line snapshot so the audio thread never does file I/O.
 * ---------------------------------------------------------------------- */

static struct {
    volatile LONG started;     /* 0 unstarted, 2 initializing, 1 running, -1 disabled */
    volatile LONG seq;         /* bumped each update; the writer uses it for liveness */
    WCHAR path[MAX_PATH];
    float db[17];              /* per AudioObjectType_to_index dBFS */
    BOOL present[17];
    volatile LONG hrtf, bed, dyn_live, dyn_max;
} spatial_stats;

static const char *spatial_channel_name(UINT32 idx)
{
    static const char *const names[] = {
        "FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR",
        "TFL", "TFR", "TBL", "TBR", "BFL", "BFR", "BBL", "BBR", "BC" };
    return idx < ARRAY_SIZE(names) ? names[idx] : "?";
}

static float spatial_channel_dbfs(const float *buf, UINT32 frames)
{
    double s = 0.0;
    UINT32 i;
    if(!frames) return -120.0f;
    for(i = 0; i < frames; ++i) s += (double)buf[i] * buf[i];
    s = sqrt(s / frames);
    return s <= 1e-6 ? -120.0f : (float)(20.0 * log10(s));
}

static BOOL spatial_stats_dos_path(WCHAR *out, DWORD cch)
{
    WCHAR env[MAX_PATH];
    DWORD i, n = GetEnvironmentVariableW(L"WINE_SPATIAL_STATS", env, ARRAY_SIZE(env));
    if(!n || n >= ARRAY_SIZE(env)) return FALSE;
    if(env[0] == '/'){                 /* unix path -> Z: drive (Z: maps to /) */
        if(n + 3 >= cch) return FALSE;
        out[0] = 'Z'; out[1] = ':';
        for(i = 0; env[i]; ++i) out[i + 2] = env[i] == '/' ? '\\' : env[i];
        out[i + 2] = 0;
    }else
        lstrcpynW(out, env, cch);
    return TRUE;
}

static DWORD WINAPI spatial_stats_writer(void *arg)
{
    WCHAR tmp[MAX_PATH];
    DWORD lastseq = ~0;
    int idle = 0;

    lstrcpynW(tmp, spatial_stats.path, ARRAY_SIZE(tmp) - 5);
    lstrcatW(tmp, L".tmp");

    for(;;){
        char line[512];
        HANDLE h;
        DWORD wr;
        int len, i;

        Sleep(100);
        if((DWORD)spatial_stats.seq == lastseq){ if(idle < 50) ++idle; }
        else { idle = 0; lastseq = spatial_stats.seq; }

        len = snprintf(line, sizeof(line), "hrtf:%ld bed:%ld dyn:%ld/%ld",
                spatial_stats.hrtf, spatial_stats.bed,
                spatial_stats.dyn_live, spatial_stats.dyn_max);
        if(idle < 5){
            for(i = 0; i < 17 && len < (int)sizeof(line) - 16; ++i)
                if(spatial_stats.present[i])
                    len += snprintf(line + len, sizeof(line) - len, " %s:%.1f",
                            spatial_channel_name(i), spatial_stats.db[i]);
        }else
            len += snprintf(line + len, sizeof(line) - len, " idle");
        if(len < (int)sizeof(line) - 1) line[len++] = '\n';

        h = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if(h != INVALID_HANDLE_VALUE){
            WriteFile(h, line, len, &wr, NULL);
            CloseHandle(h);
            MoveFileExW(tmp, spatial_stats.path, MOVEFILE_REPLACE_EXISTING);
        }
    }
    return 0;
}

static void spatial_stats_start(void)
{
    HANDLE t;
    if(InterlockedCompareExchange(&spatial_stats.started, 2, 0) != 0) return;
    if(spatial_stats_dos_path(spatial_stats.path, ARRAY_SIZE(spatial_stats.path)) &&
            (t = CreateThread(NULL, 0, spatial_stats_writer, NULL, 0, NULL))){
        CloseHandle(t);
        TRACE("Writing spatial channel stats to %s\n", debugstr_w(spatial_stats.path));
        InterlockedExchange(&spatial_stats.started, 1);
    }else
        InterlockedExchange(&spatial_stats.started, -1);
}

static void spatial_stats_update(SpatialAudioStreamImpl *stream)
{
    SpatialAudioObjectImpl *object;
    int i;
    if(spatial_stats.started != 1) return;
    for(i = 0; i < 17; ++i) spatial_stats.present[i] = FALSE;
    LIST_FOR_EACH_ENTRY(object, &stream->objects, SpatialAudioObjectImpl, entry){
        if(object->invalidated || object->type == AudioObjectType_Dynamic ||
                object->static_idx >= 17)
            continue;
        spatial_stats.db[object->static_idx] =
                spatial_channel_dbfs(object->buf, stream->update_frames);
        spatial_stats.present[object->static_idx] = TRUE;
    }
    spatial_stats.hrtf = stream->engine != 0;
    spatial_stats.bed = stream->virtualize_bed;
    spatial_stats.dyn_live = stream->dyn_live;
    spatial_stats.dyn_max = stream->dyn_max;
    InterlockedIncrement(&spatial_stats.seq);
}

static HRESULT WINAPI SAORS_EndUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->update_frames == ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->update_frames > 0){
        struct spatial_mix_object mix_objs[SPATIAL_MAX_MIX_OBJECTS];
        UINT32 mix_count = 0, i;

        if(!spatial_stats.started) spatial_stats_start();

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(object->invalidated)
                continue;
            if(This->engine && object->engine_slot != ~0 &&
                    mix_count < SPATIAL_MAX_MIX_OBJECTS){
                mix_objs[mix_count].buffer = (UINT_PTR)object->buf;
                mix_objs[mix_count].slot = object->engine_slot;
                memcpy(mix_objs[mix_count].pos, object->pos, sizeof(object->pos));
                mix_objs[mix_count].volume = object->volume;
                mix_count++;
            }else if(object->type == AudioObjectType_Dynamic){
                mix_dynamic_object(This, object);
            }else if(This->virtualize_bed){
                if(object->type == AudioObjectType_LowFrequency)
                    mix_lfe_object(This, object);
                else
                    mix_dynamic_object(This, object);
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
            }else{
                /* engine refused the tick, fall back to panning */
                LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
                    if(!object->invalidated && object->engine_slot != ~0)
                        mix_dynamic_object(This, object);
                }
            }
        }
        spatial_stats_update(This);

        /* an object that misses an update cycle is invalidated */
        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            if(!object->updated)
                object->invalidated = TRUE;
        }

        hr = IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        if(FAILED(hr))
            WARN("ReleaseBuffer failed: %08lx\n", hr);
    }

    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI SAORS_ActivateSpatialAudioObject(ISpatialAudioObjectRenderStream *iface,
        AudioObjectType type, ISpatialAudioObject **object)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *obj;

    TRACE("(%p)->(0x%x, %p)\n", This, type, object);

    if(type == AudioObjectType_Dynamic){
        if(This->dyn_live >= This->dyn_max){
            WARN("No dynamic object slots available (%u live, %u max, spatial sound %s).\n",
                    This->dyn_live, This->dyn_max, This->sa_client->dyn_budget ? "on" : "off");
            return SPTLAUDCLNT_E_NO_MORE_OBJECTS;
        }
    }else if(type & ~This->params.StaticObjectTypeMask){
        return SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
    }else if(type != AudioObjectType_None){
        UINT32 idx = AudioObjectType_to_index(type);

        /* StaticObjectTypeMask is app-supplied and unfiltered, so it can admit
         * a type that maps to no bed channel */
        if(idx == ~0)
            return SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
        LIST_FOR_EACH_ENTRY(obj, &This->objects, SpatialAudioObjectImpl, entry){
            if(obj->static_idx == idx)
                return SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
        }
    }

    obj = calloc(1, sizeof(*obj));
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
    SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);

    obj->buf = calloc(This->period_frames, This->sa_client->object_fmtex.Format.nBlockAlign);

    EnterCriticalSection(&This->lock);

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

    if(!This->dyn_budget)
        return E_NOTIMPL;

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

    if(!This->dyn_budget)
        return E_NOTIMPL;

    /* Windows Sonic for Headphones advertises a 7.1 native bed regardless of the
     * physical endpoint; height/Atmos channels arrive as dynamic objects, never as
     * static bed channels, so AudioObjectType_Dynamic and the TOP_* bits are excluded. */
    *mask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight |
            AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft | AudioObjectType_SideRight |
            AudioObjectType_BackLeft | AudioObjectType_BackRight;

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxDynamicObjectCount(ISpatialAudioClient *iface,
        UINT32 *value)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, value);

    *value = This->dyn_budget;

    return S_OK;
}

static HRESULT WINAPI SAC_GetSupportedAudioObjectFormatEnumerator(
        ISpatialAudioClient *iface, IAudioFormatEnumerator **enumerator)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, enumerator);

    *enumerator = &This->IAudioFormatEnumerator_iface;
    SAC_AddRef(iface);

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxFrameCount(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    /* FIXME: should get device period from the device */
    static const REFERENCE_TIME period = 100000;

    TRACE("(%p)->(%p, %p)\n", This, format, count);

    *count = MulDiv(period, format->nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format)
{
    SpatialAudioImpl *sac = impl_from_ISpatialAudioClient(iface);

    TRACE("sac %p, format %s.\n", sac, debugstr_fmtex(format));

    if (!format)
        return E_POINTER;

    if (!object_formats_compatible(&sac->object_fmtex.Format, format))
    {
        FIXME("Reporting format %s as unsupported.\n", debugstr_fmtex(format));
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_IsSpatialAudioStreamAvailable(ISpatialAudioClient *iface,
        REFIID stream_uuid, const PROPVARIANT *info)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);

    if(!This->dyn_budget)
        return E_NOTIMPL;

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
    CONVERT_MASK(AudioObjectType_BottomFrontLeft, 0);
    CONVERT_MASK(AudioObjectType_BottomFrontRight, 0);
    CONVERT_MASK(AudioObjectType_BottomBackLeft, 0);
    CONVERT_MASK(AudioObjectType_BottomBackRight, 0);
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
        TRACE("Virtualizing a %u-channel bed to stereo through HRTF.\n", bed_ch);
    }else{
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

    TRACE_(spatial)("stream %p configured: %u-channel bed -> %u-channel endpoint, %s, static mask 0x%x, dynamic budget %u.\n",
            stream, bed_ch, stream->stream_fmtex.Format.nChannels,
            stream->virtualize_bed ? "HRTF bed virtualization" : "multichannel passthrough",
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

    if(IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)){
        SpatialAudioStreamImpl *obj;

        if(prop &&
                (prop->vt != VT_BLOB ||
                 prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams))){
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

        if((obj->dyn_max || obj->virtualize_bed) && spatial_unix_init()){
            struct spatial_init_params init_params;
            init_params.rate = obj->stream_fmtex.Format.nSamplesPerSec;
            init_params.frames = obj->period_frames;
            init_params.handle = 0;
            if(!WINE_UNIX_CALL(unix_spatial_init, &init_params) &&
                    (obj->hrtf_buf = calloc(2 * obj->period_frames, sizeof(float)))){
                obj->engine = init_params.handle;
                TRACE_(spatial)("Using the Steam Audio HRTF engine (bed virtualization %s, up to %u dynamic objects).\n", obj->virtualize_bed ? "on" : "off", obj->dyn_max);
            }
            else if(init_params.handle){
                struct spatial_release_params release_params;
                release_params.handle = init_params.handle;
                WINE_UNIX_CALL(unix_spatial_release, &release_params);
            }
            if(!obj->engine)
                WARN_(spatial)("HRTF engine unavailable, dynamic objects will use stereo panning.\n");
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

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI SAOFE_GetFormat(IAudioFormatEnumerator *iface,
        UINT32 index, WAVEFORMATEX **format)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%u, %p)\n", This, index, format);

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
    IAudioClient *aclient;
    WAVEFORMATEX *closest;
    HRESULT hr;

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

    hr = IMMDevice_Activate(mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&aclient);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        free(obj);
        return hr;
    }

    hr = IAudioClient_IsFormatSupported(aclient, AUDCLNT_SHAREMODE_SHARED, &obj->object_fmtex.Format, &closest);

    IAudioClient_Release(aclient);

    if(hr == S_FALSE){
        if(sizeof(WAVEFORMATEX) + closest->cbSize > sizeof(obj->object_fmtex)){
            ERR("Returned format too large: %s\n", debugstr_fmtex(closest));
            CoTaskMemFree(closest);
            free(obj);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }else if(!((closest->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (closest->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)closest)->SubFormat,
                         &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) &&
                    closest->wBitsPerSample == 32)){
            ERR("Returned format not 32-bit float: %s\n", debugstr_fmtex(closest));
            CoTaskMemFree(closest);
            free(obj);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        WARN("The audio stack doesn't support 48kHz 32bit float. Using the closest match. Audio may be glitchy. %s\n", debugstr_fmtex(closest));
        memcpy(&obj->object_fmtex,
               closest,
               sizeof(WAVEFORMATEX) + closest->cbSize);
        CoTaskMemFree(closest);
    } else if(hr != S_OK){
        WARN("Checking supported formats failed: %08lx\n", hr);
        free(obj);
        return hr;
    }

    obj->mmdev = mmdev;
    IMMDevice_AddRef(mmdev);

    *out = &obj->ISpatialAudioClient_iface;

    TRACE_(spatial)("client %p created (spatial sound %s, dynamic object budget %u).\n",
            obj, obj->dyn_budget ? "enabled" : "disabled", obj->dyn_budget);
    return S_OK;
}
