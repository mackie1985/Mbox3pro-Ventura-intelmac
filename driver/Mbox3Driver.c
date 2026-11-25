/* SPDX-FileCopyrightText: 2026 Mackie
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Audio Server plug-in for the Avid Mbox 3 Pro. It runs inside coreaudiod and
 * only moves audio between CoreAudio and the shared memory rings of mbox3d
 * (see proto.h); the FireWire device is owned by the daemon.
 *
 * One device, one 8-channel input stream and one 8-channel output stream,
 * 44.1 kHz, Float32. Without the daemon the device still ticks and delivers
 * silence.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include "proto.h"

#define kPlugInID   kAudioObjectPlugInObject
#define kDeviceID   2
#define kInStreamID 3
#define kOutStreamID 4

#define kZeroPeriod 2048u
#define kDeviceUID  "Mbox3ProDevice_UID"
#define kModelUID   "Mbox3ProModel_UID"
#define kDeviceName "Mbox 3 Pro"
#define kManufacturer "mbox3pro-driver"

/* ------------------------------------------------------------ shared state */

static AudioServerPlugInHostRef gHost;
static _Atomic UInt32 gRate = M3_SAMPLE_RATE;   /* the nominal rate CoreAudio sees */
static UInt32 gPendingRate;                     /* set between a rate request and PerformChange */
#define kChangeRate 1ull
static _Atomic UInt32 gRefCount = 1;
static _Atomic UInt32 gIOCount;
#define NO_CURSOR INT64_MIN
static int64_t gOutNext = NO_CURSOR;   /* next output frame to write (IO thread only) */
static int64_t gInNext = NO_CURSOR;    /* next input frame to read (IO thread only) */

/* CoreAudio's cycle start times wobble by a few frames; placing each cycle at
 * its own time would repeat or drop a sample at every buffer boundary. Keep a
 * contiguous cursor instead and only follow CoreAudio on a real jump. */
static int64_t Place(int64_t *cursor, double sampleTime, _Atomic UInt32 *jumps, _Atomic int32_t *jumpMax,
                     _Atomic UInt32 *wobble, _Atomic int32_t *wobbleMax, m3_shm_header *h)
{
    int64_t t = (int64_t)floor(sampleTime + 0.5);
    if (*cursor == NO_CURSOR) return t;
    int64_t d = t - *cursor;
    if (d == 0) return t;
    if (wobble && h) {
        atomic_fetch_add_explicit(&h->io_wobble_abs_sum, (uint64_t)llabs((long long)d), memory_order_relaxed);
        if (llabs((long long)d) > 128) atomic_fetch_add_explicit(&h->io_wobble_gt128, 1, memory_order_relaxed);
    }
    if (llabs((long long)d) <= M3_JUMP_TOLERANCE_FOR(atomic_load(&gRate))) {
        if (wobble) {
            atomic_fetch_add_explicit(wobble, 1, memory_order_relaxed);
            if (llabs((long long)d) > llabs((long long)atomic_load_explicit(wobbleMax, memory_order_relaxed)))
                atomic_store_explicit(wobbleMax, (int32_t)d, memory_order_relaxed);
        }
        return *cursor;
    }
    atomic_fetch_add_explicit(jumps, 1, memory_order_relaxed);
    if (jumpMax) {
        int64_t j = d > INT32_MAX ? INT32_MAX : d < INT32_MIN ? INT32_MIN : d;
        if (llabs((long long)j) > llabs((long long)atomic_load_explicit(jumpMax, memory_order_relaxed)))
            atomic_store_explicit(jumpMax, (int32_t)j, memory_order_relaxed);
    }
    return t;
}
/* where the output cursor goes when it is (re)placed: just far enough ahead of the daemon's read position for this cycle's
 * frames to arrive in time (one cycle + the safety offset CoreAudio keeps + the daemon's take-in delay + a slack) */
static int64_t OutLead(m3_shm_header *h, UInt32 frames)
{
    return (int64_t)frames + (int64_t)M3_OUT_SAFETY_FOR(atomic_load(&gRate)) + (int64_t)atomic_load_explicit(&h->out_lead_extra, memory_order_relaxed);
}
static m3_shm_header *_Atomic gShm;
static mach_timebase_info_data_t gTimebase;
static pthread_once_t gOnce = PTHREAD_ONCE_INIT;

/* local clock used when the daemon is not there */
static _Atomic uint64_t gFallbackBase;
static _Atomic UInt64 gFallbackSeed = 1;

static void InitOnce(void) { mach_timebase_info(&gTimebase); }

static uint64_t NsNow(void)
{
    return mach_absolute_time() * gTimebase.numer / gTimebase.denom;
}
static uint64_t NsToTicks(uint64_t ns) { return ns * gTimebase.denom / gTimebase.numer; }

/* ------------------------------------------------------- daemon connection */

static xpc_connection_t gConn;
static uint32_t gLastBeat;
static uint64_t gLastBeatNs;

static void Connect(void)
{
    if (gConn) { xpc_connection_cancel(gConn); gConn = NULL; }
    xpc_connection_t c = xpc_connection_create_mach_service(M3_SERVICE,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), 0);
    if (!c) return;
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) { (void)ev; });
    xpc_connection_resume(c);
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "cmd", "hello");
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, msg);
    if (reply && xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
        xpc_object_t shm = xpc_dictionary_get_value(reply, "shm");
        void *addr = NULL;
        size_t len = shm ? xpc_shmem_map(shm, &addr) : 0;
        if (addr && len >= M3_SHM_SIZE) {
            m3_shm_header *h = addr;
            if (h->magic == M3_MAGIC && h->version == M3_VERSION &&
                h->channels == M3_CHANNELS && h->ring_frames == M3_RING_FRAMES) {
                gShm = h;                       /* an earlier mapping, if any, is leaked on purpose:
                                                   IO threads may still be using it */
                gLastBeat = atomic_load(&h->heartbeat);
                gLastBeatNs = NsNow();
                gConn = c;
                return;
            }
        }
    }
    xpc_connection_cancel(c);
}

static void *ConnectThread(void *arg)
{
    (void)arg;
    for (;;) {
        m3_shm_header *h = gShm;
        int ok = 0;
        if (h) {
            uint32_t beat = atomic_load(&h->heartbeat);
            if (beat != gLastBeat) { gLastBeat = beat; gLastBeatNs = NsNow(); }
            ok = NsNow() - gLastBeatNs < 3000000000ull;
        }
        if (!ok) Connect();
        sleep(1);
    }
    return NULL;
}

/* The shm is usable for audio only while the daemon is streaming. */
static m3_shm_header *LiveShm(void)
{
    m3_shm_header *h = gShm;
    if (!h) return NULL;
    /* the daemon follows the rate CoreAudio asked for; until it streams at it there is no audio */
    if (atomic_load(&h->req_rate) != atomic_load(&gRate)) atomic_store(&h->req_rate, atomic_load(&gRate));
    if (atomic_load_explicit(&h->state, memory_order_acquire) != M3_STATE_STREAMING) return NULL;
    if (atomic_load(&h->cur_rate) != atomic_load(&gRate)) return NULL;
    if (NsNow() - gLastBeatNs > 3000000000ull) return NULL;
    return h;
}

/* ---------------------------------------------------------------- formats */

static AudioStreamBasicDescription Format(void)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = atomic_load(&gRate);
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = M3_CHANNELS * 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = M3_CHANNELS * 4;
    f.mChannelsPerFrame = M3_CHANNELS;
    f.mBitsPerChannel = 32;
    return f;
}

/* ------------------------------------------------------------- properties */

#define RET_BYTES(ptr, n) do { \
    if (sizeOnly) { *outSize = (n); return 0; } \
    if (inSize < (n)) return kAudioHardwareBadPropertySizeError; \
    memcpy(out, (ptr), (n)); *outSize = (n); return 0; } while (0)
#define RET_VAL(type, v) do { type tmp_ = (v); RET_BYTES(&tmp_, sizeof(type)); } while (0)
#define RET_LIST(arr, count) do { \
    UInt32 want_ = (count) * sizeof((arr)[0]); \
    if (sizeOnly) { *outSize = want_; return 0; } \
    UInt32 n_ = inSize < want_ ? inSize - inSize % sizeof((arr)[0]) : want_; \
    memcpy(out, (arr), n_); *outSize = n_; return 0; } while (0)

static void PutCFString(CFStringRef s, void *out, UInt32 *outSize, bool sizeOnly, UInt32 inSize,
                        OSStatus *st)
{
    if (sizeOnly) { *outSize = sizeof(CFStringRef); *st = 0; return; }
    if (inSize < sizeof(CFStringRef)) { *st = kAudioHardwareBadPropertySizeError; return; }
    *(CFStringRef *)out = s;
    *outSize = sizeof(CFStringRef);
    *st = 0;
}
#define RET_STR(cstr) do { OSStatus st_; \
    PutCFString(CFStringCreateWithCString(NULL, (cstr), kCFStringEncodingUTF8), out, outSize, sizeOnly, inSize, &st_); \
    return st_; } while (0)

static bool Known(AudioObjectID obj) { return obj >= kPlugInID && obj <= kOutStreamID; }

static OSStatus Prop(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                     UInt32 qSize, const void *q, UInt32 inSize, UInt32 *outSize, void *out,
                     bool sizeOnly)
{
    (void)qSize;
    if (!Known(obj)) return kAudioHardwareBadObjectError;

    /* ---- plug-in ---- */
    if (obj == kPlugInID) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET_VAL(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET_VAL(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: RET_VAL(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: RET_STR(kManufacturer);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: { AudioObjectID l[1] = { kDeviceID }; RET_LIST(l, 1); }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID d = kAudioObjectUnknown;
            if (q && qSize == sizeof(CFStringRef) && CFStringCompare(*(CFStringRef *)q,
                    CFSTR(kDeviceUID), 0) == kCFCompareEqualTo) d = kDeviceID;
            RET_VAL(AudioObjectID, d);
        }
        case kAudioPlugInPropertyResourceBundle: RET_STR("");
        }
        return kAudioHardwareUnknownPropertyError;
    }

    /* ---- device ---- */
    if (obj == kDeviceID) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET_VAL(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET_VAL(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: RET_VAL(AudioObjectID, kPlugInID);
        case kAudioObjectPropertyName: RET_STR(kDeviceName);
        case kAudioObjectPropertyManufacturer: RET_STR(kManufacturer);
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID l[2] = { kInStreamID, kOutStreamID };
            if (a->mScope == kAudioObjectPropertyScopeInput) RET_LIST(l, 1);
            if (a->mScope == kAudioObjectPropertyScopeOutput) RET_LIST((l + 1), 1);
            RET_LIST(l, 2);
        }
        case kAudioDevicePropertyDeviceUID: RET_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: RET_STR(kModelUID);
        case kAudioDevicePropertyTransportType: RET_VAL(UInt32, kAudioDeviceTransportTypeFireWire);
        case kAudioDevicePropertyRelatedDevices: { AudioObjectID l[1] = { kDeviceID }; RET_LIST(l, 1); }
        case kAudioDevicePropertyClockDomain: RET_VAL(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: RET_VAL(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: RET_VAL(UInt32, atomic_load(&gIOCount) > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: RET_VAL(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RET_VAL(UInt32, 1);
        case kAudioDevicePropertyLatency:
            RET_VAL(UInt32, a->mScope == kAudioObjectPropertyScopeOutput ? 0 : 0);
        case kAudioDevicePropertyStreams: {
            AudioObjectID l[2] = { kInStreamID, kOutStreamID };
            if (a->mScope == kAudioObjectPropertyScopeInput) RET_LIST(l, 1);
            if (a->mScope == kAudioObjectPropertyScopeOutput) RET_LIST((l + 1), 1);
            if (a->mScope == kAudioObjectPropertyScopeGlobal) RET_LIST(l, 2);
            RET_LIST(l, 0);
        }
        case kAudioObjectPropertyControlList: { AudioObjectID l[1] = { 0 }; RET_LIST(l, 0); }
        case kAudioDevicePropertySafetyOffset:
            RET_VAL(UInt32, a->mScope == kAudioObjectPropertyScopeOutput ? M3_OUT_SAFETY_FOR(atomic_load(&gRate)) : M3_IN_SAFETY_FOR(atomic_load(&gRate)));
        case kAudioDevicePropertyNominalSampleRate: RET_VAL(Float64, (Float64)atomic_load(&gRate));
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r[4] = { { 44100, 44100 }, { 48000, 48000 }, { 88200, 88200 }, { 96000, 96000 } };
            RET_LIST(r, 4);
        }
        case kAudioDevicePropertyIsHidden: RET_VAL(UInt32, 0);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 ch[2] = { 1, 2 }; RET_LIST(ch, 2);
        }
        case kAudioDevicePropertyZeroTimeStampPeriod: RET_VAL(UInt32, kZeroPeriod);
        }
        return kAudioHardwareUnknownPropertyError;
    }

    /* ---- streams ---- */
    bool input = obj == kInStreamID;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: RET_VAL(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass: RET_VAL(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner: RET_VAL(AudioObjectID, kDeviceID);
    case kAudioObjectPropertyOwnedObjects: { AudioObjectID l[1] = { 0 }; RET_LIST(l, 0); }
    case kAudioStreamPropertyIsActive: RET_VAL(UInt32, 1);
    case kAudioStreamPropertyDirection: RET_VAL(UInt32, input ? 1 : 0);
    case kAudioStreamPropertyTerminalType:
        RET_VAL(UInt32, input ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel: RET_VAL(UInt32, 1);
    case kAudioStreamPropertyLatency: RET_VAL(UInt32, input ? M3_IN_LATENCY_FOR(atomic_load(&gRate)) : M3_OUT_LATENCY_FOR(atomic_load(&gRate)));
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: { AudioStreamBasicDescription f = Format(); RET_BYTES(&f, sizeof(f)); }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        /* Audio MIDI Setup builds its format menu from this list: one entry per rate */
        AudioStreamRangedDescription r[4];
        static const Float64 rates[4] = { 44100, 48000, 88200, 96000 };
        for (int k = 0; k < 4; k++) {
            r[k].mFormat = Format();
            r[k].mFormat.mSampleRate = rates[k];
            r[k].mSampleRateRange.mMinimum = rates[k];
            r[k].mSampleRateRange.mMaximum = rates[k];
        }
        RET_LIST(r, 4);
    }
    }
    return kAudioHardwareUnknownPropertyError;
}

static bool Has(AudioObjectID obj, const AudioObjectPropertyAddress *a)
{
    UInt32 sz = 0;
    return Prop(obj, a, 0, NULL, 0, &sz, NULL, true) == 0;
}

/* ----------------------------------------------------------- driver vtable */

static HRESULT QueryInterface(void *drv, REFIID uuid, LPVOID *outIface)
{
    (void)drv;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&gRefCount, 1);
        *outIface = drv;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}
static ULONG AddRef(void *drv) { (void)drv; return atomic_fetch_add(&gRefCount, 1) + 1; }
static ULONG Release(void *drv) { (void)drv; return atomic_fetch_sub(&gRefCount, 1) - 1; }

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host)
{
    (void)drv;
    pthread_once(&gOnce, InitOnce);
    gHost = host;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, ConnectThread, NULL);
    return 0;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{ (void)d; (void)desc; (void)ci; (void)out; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{ (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *ci)
{ (void)d; (void)o; (void)ci; return 0; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *ci)
{ (void)d; (void)o; (void)ci; return 0; }
static OSStatus PerformChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{
    (void)d; (void)i;
    if (o == kDeviceID && a == kChangeRate && M3_RATE_OK(gPendingRate)) {
        atomic_store(&gRate, gPendingRate);
        m3_shm_header *h = gShm;
        if (h) atomic_store(&h->req_rate, gPendingRate);   /* the daemon restarts the stream at this rate */
        gOutNext = NO_CURSOR; gInNext = NO_CURSOR;
    }
    return 0;
}
static OSStatus AbortChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{ (void)d; (void)o; (void)a; (void)i; gPendingRate = 0; return 0; }

/* CoreAudio wants another nominal rate: the change is made while the device is stopped */
static OSStatus RequestRate(double r)
{
    UInt32 want = fabs(r - 44100) < 0.5 ? 44100 : fabs(r - 48000) < 0.5 ? 48000 : fabs(r - 88200) < 0.5 ? 88200 : fabs(r - 96000) < 0.5 ? 96000 : 0;
    if (!want) return kAudioDeviceUnsupportedFormatError;
    if (want == atomic_load(&gRate)) return 0;
    gPendingRate = want;
    return gHost ? gHost->RequestDeviceConfigurationChange(gHost, kDeviceID, kChangeRate, NULL) : kAudioHardwareUnspecifiedError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a)
{ (void)d; (void)pid; return Known(o) && Has(o, a); }

static OSStatus IsSettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a, Boolean *settable)
{
    (void)d; (void)pid;
    if (!Known(o)) return kAudioHardwareBadObjectError;
    if (!Has(o, a)) return kAudioHardwareUnknownPropertyError;
    *settable = 0;
    if (o == kDeviceID && a->mSelector == kAudioDevicePropertyNominalSampleRate) *settable = 1;
    if (o >= kInStreamID && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                             a->mSelector == kAudioStreamPropertyPhysicalFormat)) *settable = 1;
    return 0;
}

static OSStatus GetSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                        const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size)
{
    (void)d; (void)pid;
    return Prop(o, a, qs, q, 0, size, NULL, true);
}

static OSStatus GetData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                        const AudioObjectPropertyAddress *a, UInt32 qs, const void *q,
                        UInt32 inSize, UInt32 *outSize, void *out)
{
    (void)d; (void)pid;
    return Prop(o, a, qs, q, inSize, outSize, out, false);
}

static OSStatus SetData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                        const AudioObjectPropertyAddress *a, UInt32 qs, const void *q,
                        UInt32 inSize, const void *in)
{
    (void)d; (void)pid; (void)qs; (void)q;
    if (!Known(o)) return kAudioHardwareBadObjectError;
    if (o == kDeviceID && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return RequestRate(*(const Float64 *)in);
    }
    if (o >= kInStreamID && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                             a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = in;
        AudioStreamBasicDescription want = Format();
        if (f->mFormatID != want.mFormatID ||
            f->mChannelsPerFrame != want.mChannelsPerFrame || f->mBitsPerChannel != 32 ||
            !(f->mFormatFlags & kAudioFormatFlagIsFloat))
            return kAudioDeviceUnsupportedFormatError;
        return RequestRate(f->mSampleRate);
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d; (void)client;
    if (dev != kDeviceID) return kAudioHardwareBadObjectError;
    if (atomic_fetch_add(&gIOCount, 1) == 0) {
        gOutNext = NO_CURSOR;
        gInNext = NO_CURSOR;
        atomic_store(&gFallbackBase, NsNow());
        atomic_fetch_add(&gFallbackSeed, 1);
    }
    return 0;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d; (void)client;
    if (dev != kDeviceID) return kAudioHardwareBadObjectError;
    UInt32 c = atomic_load(&gIOCount);
    if (c > 0) atomic_fetch_sub(&gIOCount, 1);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                 Float64 *outSample, UInt64 *outHost, UInt64 *outSeed)
{
    (void)d; (void)client;
    if (dev != kDeviceID) return kAudioHardwareBadObjectError;
    double nsf = 1e9 / (double)atomic_load(&gRate);
    uint64_t base, seed;
    int64_t afr = 0; uint64_t ans = 0; int fit = 0;
    m3_shm_header *h = LiveShm();
    if (h) {
        base = atomic_load(&h->base_ns);
        seed = atomic_load(&h->seed);
        if (atomic_load(&h->tl_valid)) {
            for (int tries = 0; tries < 4; tries++) {
                uint32_t a = atomic_load(&h->tl_seq);
                if (a & 1) continue;
                uint64_t f = atomic_load(&h->tl_anchor_frame), n = atomic_load(&h->tl_anchor_ns);
                uint64_t bits = atomic_load(&h->tl_nsf_bits);
                if (atomic_load(&h->tl_seq) != a) continue;
                double v; memcpy(&v, &bits, 8);
                afr = (int64_t)f; ans = n; nsf = v; fit = 1;
                break;
            }
        }
    } else {
        base = atomic_load(&gFallbackBase);
        if (!base) { base = NsNow(); atomic_store(&gFallbackBase, base); }
        seed = (1ull << 32) | atomic_load(&gFallbackSeed);
    }
    uint64_t now = NsNow();
    double k, hostNs;
    if (fit) {
        double frames = (double)afr + ((double)now - (double)ans) / nsf;
        k = floor(frames / kZeroPeriod); if (k < 0) k = 0;
        hostNs = (double)ans + (k * kZeroPeriod - (double)afr) * nsf;
    } else {
        double frames = now > base ? (double)(now - base) / nsf : 0.0;
        k = floor(frames / kZeroPeriod);
        hostNs = (double)base + k * kZeroPeriod * nsf;
    }
    *outSample = k * kZeroPeriod;
    *outHost = NsToTicks((uint64_t)hostNs);
    *outSeed = seed;
    return 0;
}

static OSStatus WillDoIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                         Boolean *will, Boolean *inPlace)
{
    (void)d; (void)dev; (void)client;
    *will = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = 1;
    return 0;
}

static OSStatus BeginIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                        UInt32 frames, const AudioServerPlugInIOCycleInfo *ci)
{ (void)d; (void)dev; (void)client; (void)op; (void)frames; (void)ci; return 0; }
static OSStatus EndIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                      UInt32 frames, const AudioServerPlugInIOCycleInfo *ci)
{ (void)d; (void)dev; (void)client; (void)op; (void)frames; (void)ci; return 0; }

/* Resample one block of F frames to n frames (n = F - 2 .. F + 2) with a cubic (Catmull-Rom) interpolator. The ends of the
 * block stay where they are, so consecutive blocks join without a repeated or dropped sample; the change is spread over the
 * block, which is how a rate difference of a tenth of a percent is absorbed without a click. */
static void StretchBlock(const float *in, UInt32 F, float *out, UInt32 n)
{
    const double step = (double)(F - 1) / (double)(n - 1);
    for (UInt32 j = 0; j < n; j++) {
        double p = (double)j * step;
        UInt32 i = (UInt32)p;
        double u = p - (double)i;
        if (i >= F - 1) { i = F - 2; u = 1.0; }
        UInt32 i0 = i ? i - 1 : 0, i2 = i + 1, i3 = i + 2 < F ? i + 2 : F - 1;
        for (unsigned c = 0; c < M3_CHANNELS; c++) {
            double y0 = in[(size_t)i0 * M3_CHANNELS + c], y1 = in[(size_t)i * M3_CHANNELS + c];
            double y2 = in[(size_t)i2 * M3_CHANNELS + c], y3 = in[(size_t)i3 * M3_CHANNELS + c];
            out[(size_t)j * M3_CHANNELS + c] = (float)(y1 + 0.5 * u * (y2 - y0 + u * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + u * (3.0 * (y1 - y2) + y3 - y0))));
        }
    }
}

static OSStatus DoIO(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream,
                     UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci,
                     void *mainBuf, void *secondaryBuf)
{
    (void)d; (void)dev; (void)client; (void)secondaryBuf;
    float *buf = mainBuf;
    m3_shm_header *h = LiveShm();
    const uint64_t mask = M3_RING_FRAMES - 1;

    /* the daemon started a new timeline (restart, or the device was re-opened):
     * the old cursors belong to the old one */
    if (h) {
        static uint32_t seedSeen = 0;
        uint32_t sd = atomic_load(&h->seed);
        if (sd != seedSeen) { seedSeen = sd; gOutNext = NO_CURSOR; gInNext = NO_CURSOR; }
    }

    if (op == kAudioServerPlugInIOOperationReadInput && stream == kInStreamID) {
        if (!h) { memset(buf, 0, (size_t)frames * M3_CHANNELS * sizeof(float)); return 0; }
        const int32_t *cap = m3_cap_ring(h);
        const int64_t inBefore = gInNext;
        int64_t t0 = Place(&gInNext, ci->mInputTime.mSampleTime, &h->in_jumps, NULL, NULL, NULL, NULL);
        uint64_t w = atomic_load_explicit(&h->cap_w, memory_order_acquire);
        const float scale = 1.0f / 8388608.0f;
        /* The read cursor is contiguous, and the device delivers a few frames per second more than CoreAudio's cycles
         * take, so the read margin creeps up (measured: about 4 frames per second) until a jump. Hold the margin where
         * a target (measured: a cursor left where it was first placed starts at a margin of about 120 frames and then creeps
         * up): a slow average of it, and now and then one source frame more or less, resampled into the block. */
        static double iEma, iBase, iAcc;
        static int iSettle;
        static float inBuf[(8192 + 8) * M3_CHANNELS];
        int inE = 0;
        {
            double m = (double)w - (double)(t0 + (int64_t)frames);
            if (inBefore == NO_CURSOR || t0 != inBefore) {
                iSettle = 0; iEma = m; iBase = m; iAcc = 0.0;
            } else if (iSettle < 128) {
                iSettle++;
                iEma += (m - iEma) / 16.0;
                if (iSettle == 128) iBase = (double)(M3_IN_SAFETY_FOR(atomic_load(&gRate)) + 64u * M3_RATE_MULT(atomic_load(&gRate)));
            } else if (frames >= 16 && frames <= 8192) {
                iEma += (m - iEma) / 64.0;
                iAcc += (iEma - iBase) * ((double)frames / 128.0) / 256.0;
                int e = (int)iAcc;
                if (e > 2) e = 2;
                if (e < -2) e = -2;
                iAcc -= (double)e;
                if (iAcc > 2.0) iAcc = 2.0;
                if (iAcc < -2.0) iAcc = -2.0;
                if (e) inE = e;
            }
        }
        gInNext = t0 + (int64_t)frames + inE;
        {
            int64_t margin = (int64_t)w - (t0 + (int64_t)frames);
            if (margin < 0) atomic_fetch_add_explicit(&h->late_read_frames, (UInt32)(-margin > (int64_t)frames ? frames : -margin), memory_order_relaxed);
            if (margin > INT32_MAX) margin = INT32_MAX;
            if (margin < INT32_MIN) margin = INT32_MIN;
            if ((int32_t)margin < atomic_load_explicit(&h->read_margin_min, memory_order_relaxed))
                atomic_store_explicit(&h->read_margin_min, (int32_t)margin, memory_order_relaxed);
        }
        const UInt32 nIn = (UInt32)((int)frames + inE);
        float *dst = inE ? inBuf : buf;
        for (UInt32 i = 0; i < nIn; i++) {
            int64_t t = t0 + i;
            float *f = dst + (size_t)i * M3_CHANNELS;
            if (t >= 0 && (uint64_t)t < w && w - (uint64_t)t <= M3_RING_FRAMES) {
                const int32_t *s = cap + ((uint64_t)t & mask) * M3_CHANNELS;
                for (unsigned c = 0; c < M3_CHANNELS; c++) f[c] = (float)s[c] * scale;
            } else {
                for (unsigned c = 0; c < M3_CHANNELS; c++) f[c] = 0.0f;
            }
        }
        if (inE) StretchBlock(inBuf, nIn, buf, frames);
        return 0;
    }
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStreamID) {
        if (!h) return 0;
        {
            static uint64_t lastWake;
            uint64_t nowNs = NsNow();
            if (lastWake && nowNs > lastWake) {
                uint32_t us = (uint32_t)((nowNs - lastWake) / 1000);
                if (us > atomic_load_explicit(&h->wake_gap_max_us, memory_order_relaxed))
                    atomic_store_explicit(&h->wake_gap_max_us, us, memory_order_relaxed);
                if (us > 5000) atomic_fetch_add_explicit(&h->wake_over5, 1, memory_order_relaxed);
                if (us > 10000) atomic_fetch_add_explicit(&h->wake_over10, 1, memory_order_relaxed);
                if (us > 20000) atomic_fetch_add_explicit(&h->wake_over20, 1, memory_order_relaxed);
            }
            lastWake = nowNs;
        }
        int32_t *play = m3_play_ring(h);
        const int64_t cursorBefore = gOutNext;
        const int64_t tRaw = (int64_t)floor(ci->mOutputTime.mSampleTime + 0.5);
        int64_t t0 = Place(&gOutNext, ci->mOutputTime.mSampleTime, &h->io_jumps, &h->io_jump_max,
                           &h->io_wobble, &h->io_wobble_max, h);
        uint64_t taken = atomic_load_explicit(&h->play_pos, memory_order_acquire);
        /* The cursor is contiguous, so a cycle that CoreAudio skipped (a late wake-up) moves CoreAudio's time ahead of
         * the cursor for good: the lead over the daemon shrinks by one buffer per skip, about every 6 s at a buffer of
         * 128. Single cycles are noisy (hundreds of frames), so watch a slow average of (CoreAudio time - cursor)
         * and move the cursor by the part that has built up since the cursor was last placed. */
        static double dEma, dBase, stretchAcc;
        static int dSettle;
        static float stretchBuf[(8192 + 4) * M3_CHANNELS];
        double drift = 0.0;
        int rebased = (cursorBefore == NO_CURSOR) || (t0 != cursorBefore);
        if (rebased) {
            /* Not at CoreAudio's own time, which here lies about 1000 frames further ahead than the daemon needs
             * (measured: write margin 1350 frames, a round trip 30 ms longer than reported): a small lead over the daemon */
            t0 = (int64_t)taken + OutLead(h, frames);
        }
        if (!rebased) {
            /* Hold the lead over the daemon where it settled (measured: CoreAudio's time and the device drift apart by
             * a few frames a second, so following CoreAudio's time let the lead, and the round trip, creep up). */
            const double x = (double)(t0 - (int64_t)taken);
            if (dSettle < 128) {
                dSettle++;
                dEma += (x - dEma) / 16.0;
                if (dSettle == 128) dBase = dEma;
            } else {
                dEma += (x - dEma) / 64.0;
                drift = dBase - dEma;
            }
            if (drift >= 512.0 || drift <= -512.0) {
                /* far off: move the cursor at once */
                int64_t adj = (int64_t)llround(drift);
                t0 += adj;
                dEma += (double)adj;
                drift = 0.0;
                atomic_fetch_add_explicit(&h->io_drift_fixes, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&h->io_drift_frames, adj, memory_order_relaxed);
            }
        }
        {
            /* Writing contiguously ignores cycles that CoreAudio dropped (overload): every later block then lands a
             * little earlier than it should, and the write position slides below the daemon's read position until
             * everything is late. Follow the daemon's read position: if blocks keep arriving late, or the lead is
             * absurd, put the cursor a nominal lead ahead of it. */
            static int lateRun;
            int64_t lead = t0 - (int64_t)taken;
            lateRun = lead < 0 ? lateRun + 1 : 0;
            /* more than one block behind at once (play restarted after a pause: the daemon went on while the cursor stood
             * still): do not wait for three late blocks, which would be a click at every start */
            if (lateRun >= 3 || lead > M3_MAX_LEAD || lead < -(int64_t)frames) {
                int64_t j = (int64_t)taken + OutLead(h, frames) - t0;
                atomic_fetch_add_explicit(&h->io_jumps, 1, memory_order_relaxed);
                if (llabs((long long)j) > llabs((long long)atomic_load_explicit(&h->io_jump_max, memory_order_relaxed)))
                    atomic_store_explicit(&h->io_jump_max, (int32_t)(j > INT32_MAX ? INT32_MAX : j < INT32_MIN ? INT32_MIN : j), memory_order_relaxed);
                t0 = (int64_t)taken + OutLead(h, frames);
                lateRun = 0;
                rebased = 1;
            }
        }
        (void)tRaw;
        if (rebased) { dSettle = 0; dEma = (double)(t0 - (int64_t)taken); dBase = dEma; stretchAcc = 0.0; }
        /* CoreAudio's cycles come a little slower than the device consumes (about a tenth of a percent here), so the
         * cursor falls behind CoreAudio's time. Absorb that by writing one frame more now and then: a slow control loop
         * on the drift decides how many extra frames this cycle gets, and the block is resampled to that length. */
        const float *src = buf;
        UInt32 nOut = frames;
        if (!rebased && frames >= 16 && frames <= 8192) {
            stretchAcc += drift * ((double)frames / 128.0) / 256.0;
            int e = (int)stretchAcc;
            if (e > 2) e = 2;
            if (e < -2) e = -2;
            stretchAcc -= (double)e;
            if (stretchAcc > 2.0) stretchAcc = 2.0;
            if (stretchAcc < -2.0) stretchAcc = -2.0;
            if (e) {
                nOut = frames + (UInt32)e;
                StretchBlock(buf, frames, stretchBuf, nOut);
                src = stretchBuf;
                atomic_fetch_add_explicit(&h->io_drift_fixes, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&h->io_drift_frames, e, memory_order_relaxed);
            }
        }
        gOutNext = t0 + (int64_t)nOut;
        {
            int64_t margin = t0 - (int64_t)taken;
            atomic_fetch_add_explicit(&h->io_cycles, 1, memory_order_relaxed);
            if (margin < 0) atomic_fetch_add_explicit(&h->late_write_frames, (UInt32)(-margin > (int64_t)frames ? frames : -margin), memory_order_relaxed);
            if (margin > INT32_MAX) margin = INT32_MAX;
            if (margin < INT32_MIN) margin = INT32_MIN;
            if ((int32_t)margin < atomic_load_explicit(&h->write_margin_min, memory_order_relaxed))
                atomic_store_explicit(&h->write_margin_min, (int32_t)margin, memory_order_relaxed);
        }
        atomic_store_explicit(&h->io_frames_last, frames, memory_order_relaxed);
        if (frames > atomic_load_explicit(&h->io_frames_max, memory_order_relaxed)) atomic_store_explicit(&h->io_frames_max, frames, memory_order_relaxed);
        uint32_t peak = 0, ahead = 0;
        for (UInt32 i = 0; i < nOut; i++) {
            int64_t t = t0 + i;
            const float *f = src + (size_t)i * M3_CHANNELS;
            for (unsigned c = 0; c < M3_CHANNELS; c++) {
                float a = f[c] < 0 ? -f[c] : f[c];
                uint32_t q = a >= 1.0f ? 8388607u : (uint32_t)(a * 8388607.0f);
                if (q > peak) peak = q;
            }
            if (t >= 0 && (uint64_t)t >= taken + M3_RING_FRAMES) ahead++;
            if (t < 0 || (uint64_t)t < taken || (uint64_t)t >= taken + M3_RING_FRAMES) continue;
            int32_t *s = play + ((uint64_t)t & mask) * M3_CHANNELS;
            for (unsigned c = 0; c < M3_CHANNELS; c++) {
                float v = f[c] * 8388607.0f;
                if (v > 8388607.0f) v = 8388607.0f;
                if (v < -8388608.0f) v = -8388608.0f;
                s[c] = (int32_t)v;
            }
        }
        if (peak > atomic_load_explicit(&h->plug_out_peak, memory_order_relaxed)) atomic_store_explicit(&h->plug_out_peak, peak, memory_order_relaxed);
        if (ahead) atomic_fetch_add_explicit(&h->ahead_write_frames, ahead, memory_order_relaxed);
        return 0;
    }
    return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    QueryInterface, AddRef, Release,
    Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformChange, AbortChange,
    HasProperty, IsSettable, GetSize, GetData, SetData,
    StartIO, StopIO, GetZeroTimeStamp,
    WillDoIO, BeginIO, DoIO, EndIO
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

void *Mbox3_Create(CFAllocatorRef alloc, CFUUIDRef typeUUID)
{
    (void)alloc;
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}
