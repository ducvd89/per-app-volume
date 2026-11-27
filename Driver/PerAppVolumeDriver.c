//
//  PerAppVolumeDriver.c
//  Per-App Volume — virtual output device (AudioServerPlugIn / HAL driver)
//
//  How it works (no audio capture / no screen recording => no privacy dot):
//   * Apps play into this virtual output device ("Per-App Volume").
//   * coreaudiod hands each client's audio to us BEFORE it is mixed
//     (kAudioServerPlugInIOOperationProcessOutput). We apply the per-app gain there.
//   * The final mix (WriteMix) goes into a ring buffer.
//   * Apps the user gave an output device of their own are taken out of that mix: their audio goes into
//     a ring per output ("bus") instead, see the 'aprt' property and PAVProtocol.h.
//   * The companion app pulls the mix through a custom device *property* ('apad')
//     and plays it on the real output device. No input stream is ever opened,
//     so macOS never shows the microphone / recording indicator.
//
//  Custom properties on the device object (global scope, element main):
//   'apcl'  get  CFArray<CFDictionary{cid,pid,bid,run,peak}>   connected clients
//   'apvl'  get/set CFDictionary{ "<pid>" | "b:<bundle id>" : gain(0..4) }   gain table
//   'apad'  get  CFData (interleaved stereo Float32)           new mixed frames
//   'apsf'  get/set CFNumber                                    shared ring (see PAVShared.h)
//   'aplt'  get/set CFNumber                                    frames of latency to report
//   'aprt'  get/set CFDictionary{surfaces, routes}              per-app outputs (see PAVProtocol.h)
//   'apms'  get/set CFNumber                                    who applies the master volume (see PAVProtocol.h)
//
//  The properties that carry audio or change behaviour ('apad', 'apsf', 'aplt', setting 'apvl', 'aprt' or 'apms')
//  only answer the companion app: see CallerMayControl().
//

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <os/log.h>
#include <os/lock.h>
#include <IOSurface/IOSurface.h>
#include "PAVShared.h"
#include "PAVProtocol.h"

static os_log_t gLog;
static _Atomic int gTraceCount = 0;
#define kTraceLimit 0   // set >0 to trace HAL property calls to os_log

#pragma mark - Constants

#define kDevice_UID        kPAVDeviceUID
#define kDevice_ModelUID   "PerAppVolume_ModelUID"
#define kDevice_Name       "Per-App Volume"
#define kManufacturer_Name "PerAppVolume"

#define kChannels            2
#define kBytesPerFrame       (kChannels * (UInt32)sizeof(Float32))
#define kZeroTimeStampPeriod 16384
#define kDataRingFrames      65536u   // power of two (~1.3 s @ 48 kHz)
#define kDataRingMask        (kDataRingFrames - 1u)
#define kMaxChunkFrames      16384u
#define kMaxClients          128
#define kMaxPidGains         512
#define kMaxBundleGains      256
#define kBundleIDMax         128

#define kVolumeMinDB (-96.0f)
#define kVolumeMaxDB (0.0f)

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream = 3,
    kObjectID_Volume = 4,
    kObjectID_Mute   = 5,
};

#define kSampleRates    kPAVSampleRates
#define kNumSampleRates ((UInt32)kPAVNumSampleRates)
#define kNumCustomProps 7

#pragma mark - State

static pthread_mutex_t gStateMutex  = PTHREAD_MUTEX_INITIALIZER;   // properties / config
static pthread_mutex_t gClockMutex  = PTHREAD_MUTEX_INITIALIZER;   // zero timestamps
static pthread_mutex_t gClientMutex = PTHREAD_MUTEX_INITIALIZER;   // client table writes
static pthread_mutex_t gReadMutex   = PTHREAD_MUTEX_INITIALIZER;   // audio reader

static AudioServerPlugInHostRef gHost = NULL;
static UInt32  gRefCount = 0;

static Float64 gSampleRate        = 48000.0;
static Float64 gPendingSampleRate = 48000.0;
// Latency of everything behind this device (the app's buffer + the real output device), in frames.
// Set by the app; like the sample rate it only changes through a device configuration change.
static UInt32  gLatencyFrames        = 0;     // under gStateMutex
static UInt32  gPendingLatencyFrames = 0;

static UInt32  gIORunningCount    = 0;
static Float64 gHostTicksPerFrame = 0.0;
static UInt64  gNumberTimeStamps  = 0;
static UInt64  gAnchorHostTime    = 0;   // host time of zero timestamp number gAnchorIndex
static UInt64  gAnchorIndex       = 0;
static Float64 gClockTicksPerFrame = 0;  // rate the virtual clock runs at right now (follows the real device when locked)

static Float32        gVolumeScalar = 1.0f;     // under gStateMutex
static _Atomic float  gMasterGain   = 1.0f;     // linear, read on IO thread
static _Atomic bool   gMuted        = false;
static _Atomic bool   gMasterOnDevice = false;  // the output device applies the master volume (see PAVProtocol.h)
static float          gMasterApplied = 1.0f;    // IO thread only

// Mixed audio ring (written on IO thread, read by property getter)
static float              gDataRing[kDataRingFrames * kChannels];
static _Atomic uint64_t   gWriteEnd = 0;

// Shared-memory ring provided by the app (see PAVShared.h). Swapped pointers are never freed while
// the IO thread might still be writing; old surfaces go to a small graveyard and are released later.
static PAVSharedRing* _Atomic gShm = NULL;
static IOSurfaceRef   gShmSurface = NULL;
static IOSurfaceID    gShmID = 0;
#define kShmGraveyardSize 16
static IOSurfaceRef   gShmGraveyard[kShmGraveyardSize];
static int            gShmGraveyardN = 0;
static pthread_mutex_t gShmMutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t   gStreamGen = 0;
static uint64_t           gReadPos = 0;       // under gReadMutex
static uint32_t           gReadGen = 0xFFFFFFFF;

// Extra outputs. Each bus has a shared ring like the main one, holding the mix of the clients routed to it.
// Clients write from their own IO cycles, which can differ in size and position and may run on different
// threads, so the ring is filled the way the host fills its own mix buffer: frames nobody has written yet
// are zeroed once (up to `clearedEnd`), then every client adds its audio at its sample time.
typedef struct {
    PAVSharedRing* _Atomic ring;   // NULL: no ring; clients routed here stay in the main mix
    IOSurfaceRef   surface;        // under gShmMutex
    IOSurfaceID    surfaceID;      // under gShmMutex
    PAVSharedRing* clearedRing;    // under gBusLock: the ring `clearedEnd` is about
    uint64_t       clearedEnd;     // under gBusLock: frames below this are zeroed or hold audio
} PAVBus;
static PAVBus         gBuses[kPAVMaxBuses + 1];   // [0] is not used: bus 0 is the main mix
static os_unfair_lock gBusLock = OS_UNFAIR_LOCK_INIT;

typedef struct {
    _Atomic bool    used;
    UInt32          clientID;
    pid_t           pid;
    CFStringRef     bundleID;
    char            bundleIDC[kBundleIDMax];   // same, as a C string ("" if none)
    _Atomic float   gain;       // target gain
    float           applied;    // IO thread only (ramping)
    _Atomic int     bus;        // where its audio should go (0 = main mix)
    int             busApplied; // IO thread only: where it went in the last cycle
    _Atomic float   peak;
    _Atomic bool    running;
} PAVClient;

static PAVClient gClients[kMaxClients];

// A per-app setting as the app sends it: by pid, and by bundle ID for clients the app hasn't seen yet (an
// app that was just launched), so those start with their saved setting instead of the default. The app
// corrects the guess with the client's pid on its next poll.
typedef struct { pid_t pid; float value; } PAVPidEntry;
typedef struct { char bundleID[kBundleIDMax]; float value; } PAVBundleEntry;
typedef struct {
    PAVPidEntry    pids[kMaxPidGains];
    int            pidCount;
    PAVBundleEntry bundles[kMaxBundleGains];
    int            bundleCount;
} PAVTable;
static PAVTable gGains;     // under gClientMutex: volume (default 1)
static PAVTable gRoutes;    // under gClientMutex: bus (default 0)

#pragma mark - Prototypes

static HRESULT  PAV_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG    PAV_AddRef(void* inDriver);
static ULONG    PAV_Release(void* inDriver);
static OSStatus PAV_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus PAV_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus PAV_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus PAV_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus PAV_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus PAV_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus PAV_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean  PAV_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus PAV_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus PAV_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus PAV_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus PAV_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static Boolean  PAV_HasProperty_Impl(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus PAV_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus PAV_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus PAV_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus PAV_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus PAV_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus PAV_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus PAV_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    PAV_QueryInterface, PAV_AddRef, PAV_Release,
    PAV_Initialize, PAV_CreateDevice, PAV_DestroyDevice,
    PAV_AddDeviceClient, PAV_RemoveDeviceClient,
    PAV_PerformDeviceConfigurationChange, PAV_AbortDeviceConfigurationChange,
    PAV_HasProperty, PAV_IsPropertySettable, PAV_GetPropertyDataSize, PAV_GetPropertyData, PAV_SetPropertyData,
    PAV_StartIO, PAV_StopIO, PAV_GetZeroTimeStamp,
    PAV_WillDoIOOperation, PAV_BeginIOOperation, PAV_DoIOOperation, PAV_EndIOOperation
};
static AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef        gDriverRef          = &gDriverInterfacePtr;

#pragma mark - Helpers

static void RecomputeHostTicks(void) {
    mach_timebase_info_data_t ti;
    mach_timebase_info(&ti);
    Float64 hostFreq = ((Float64)ti.denom / (Float64)ti.numer) * 1000000000.0;
    gHostTicksPerFrame = hostFreq / gSampleRate;
}

static bool IsSupportedRate(Float64 r) {
    for (UInt32 i = 0; i < kNumSampleRates; ++i) if (fabs(kSampleRates[i] - r) < 0.5) return true;
    return false;
}

static void FillASBD(AudioStreamBasicDescription* d, Float64 rate) {
    memset(d, 0, sizeof(*d));
    d->mSampleRate       = rate;
    d->mFormatID         = kAudioFormatLinearPCM;
    d->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d->mBytesPerPacket   = kBytesPerFrame;
    d->mFramesPerPacket  = 1;
    d->mBytesPerFrame    = kBytesPerFrame;
    d->mChannelsPerFrame = kChannels;
    d->mBitsPerChannel   = 32;
}

static Float32 ScalarToDB(Float32 s) {
    if (s <= 0.0f) return kVolumeMinDB;
    Float32 db = 40.0f * log10f(s);          // gain = scalar^2
    return db < kVolumeMinDB ? kVolumeMinDB : (db > kVolumeMaxDB ? kVolumeMaxDB : db);
}
static Float32 DBToScalar(Float32 db) {
    if (db <= kVolumeMinDB) return 0.0f;
    if (db >= kVolumeMaxDB) return 1.0f;
    return powf(10.0f, db / 40.0f);
}

static void NotifyAsync(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    if (gHost == NULL) return;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
        gHost->PropertiesChanged(gHost, obj, 1, &a);
    });
}

/// A client's setting: its pid if the app listed it, else the longest bundle-ID entry that is the client's
/// bundle ID or a parent of it ("com.x.app" also covers "com.x.app.helper"), else `def`.
static float LookupLocked(const PAVTable* t, pid_t pid, const char* bundleID, float def) {
    for (int i = 0; i < t->pidCount; ++i) if (t->pids[i].pid == pid) return t->pids[i].value;
    float value = def;
    size_t bestLen = 0;
    if (bundleID == NULL || bundleID[0] == 0) return value;
    for (int i = 0; i < t->bundleCount; ++i) {
        const char* e = t->bundles[i].bundleID;
        size_t n = strlen(e);
        if (n <= bestLen || strncasecmp(bundleID, e, n) != 0) continue;
        if (bundleID[n] == 0 || bundleID[n] == '.') { bestLen = n; value = t->bundles[i].value; }
    }
    return value;
}
static float LookupGainLocked(pid_t pid, const char* bundleID) { return LookupLocked(&gGains, pid, bundleID, 1.0f); }
static int   LookupBusLocked(pid_t pid, const char* bundleID)  { return (int)LookupLocked(&gRoutes, pid, bundleID, 0.0f); }

/// Whether the process asking is allowed to read the mix or change how the device behaves.
/// Every caller is registered as a client (with the bundle ID coreaudiod saw) before its first property
/// call, so: a registered client that isn't the companion app is refused. This is not authentication (a
/// process can claim the app's bundle ID); it keeps ordinary processes from tapping system audio through
/// this driver. Unknown pids (the host itself) are let through.
static bool CallerMayControl(pid_t pid) {
    bool known = false, isApp = false;
    pthread_mutex_lock(&gClientMutex);
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (!atomic_load(&c->used) || c->pid != pid) continue;
        known = true;
        if (strcasecmp(c->bundleIDC, kPAVAppBundleID) == 0 || strcasecmp(c->bundleIDC, "com.apple.audio.coreaudiod") == 0) isApp = true;
    }
    pthread_mutex_unlock(&gClientMutex);
    return isApp || !known;
}

static PAVClient* FindClient(UInt32 clientID) {
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (atomic_load_explicit(&c->used, memory_order_acquire) && c->clientID == clientID) return c;
    }
    return NULL;
}

static inline void ApplyRampedGain(float* buf, UInt32 frames, float from, float to) {
    if (from == to) {
        if (to == 1.0f) return;
        UInt32 n = frames * kChannels;
        for (UInt32 i = 0; i < n; ++i) buf[i] *= to;
        return;
    }
    float step = (to - from) / (float)(frames ? frames : 1);
    float g = from;
    for (UInt32 f = 0; f < frames; ++f) {
        g += step;
        buf[f * 2]     *= g;
        buf[f * 2 + 1] *= g;
    }
}

#pragma mark - Factory

void* PAV_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (!gLog) gLog = os_log_create("com.perappvolume.driver", "driver");
    os_log(gLog, "PAV Create");
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

#pragma mark - IUnknown

static HRESULT PAV_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (inDriver != gDriverRef || outInterface == NULL) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex); ++gRefCount; pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        r = 0;
    }
    CFRelease(req);
    return r;
}

static ULONG PAV_AddRef(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex); ULONG r = ++gRefCount; pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG PAV_Release(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex); if (gRefCount > 0) --gRefCount; ULONG r = gRefCount; pthread_mutex_unlock(&gStateMutex);
    return r;
}

#pragma mark - Basic operations

static OSStatus PAV_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    if (!gLog) gLog = os_log_create("com.perappvolume.driver", "driver");
    os_log(gLog, "PAV Initialize");

    // restore persisted sample rate / volume
    CFPropertyListRef v = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("SampleRate"), &v) == 0 && v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            Float64 r; CFNumberGetValue((CFNumberRef)v, kCFNumberFloat64Type, &r);
            if (IsSupportedRate(r)) { gSampleRate = r; gPendingSampleRate = r; }
        }
        CFRelease(v);
    }
    v = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("Volume"), &v) == 0 && v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            Float32 s; CFNumberGetValue((CFNumberRef)v, kCFNumberFloat32Type, &s);
            if (s >= 0.0f && s <= 1.0f) { gVolumeScalar = s; atomic_store(&gMasterGain, s * s); gMasterApplied = s * s; }
        }
        CFRelease(v);
    }
    RecomputeHostTicks();
    return 0;
}

static OSStatus PAV_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* ci, AudioObjectID* out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus PAV_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) {
    (void)d; (void)id;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus PAV_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, const AudioServerPlugInClientInfo* ci) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gClientMutex);
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (!atomic_load(&c->used)) {
            c->clientID = ci->mClientID;
            c->pid      = ci->mProcessID;
            c->bundleID = ci->mBundleID ? CFStringCreateCopy(NULL, ci->mBundleID) : NULL;
            c->bundleIDC[0] = 0;
            if (c->bundleID && !CFStringGetCString(c->bundleID, c->bundleIDC, sizeof(c->bundleIDC), kCFStringEncodingUTF8)) c->bundleIDC[0] = 0;
            float g = LookupGainLocked(ci->mProcessID, c->bundleIDC);
            atomic_store(&c->gain, g);
            c->applied = g;
            int bus = LookupBusLocked(ci->mProcessID, c->bundleIDC);
            atomic_store(&c->bus, bus);
            c->busApplied = bus;
            atomic_store(&c->peak, 0.0f);
            atomic_store(&c->running, false);
            atomic_store_explicit(&c->used, true, memory_order_release);
            break;
        }
    }
    pthread_mutex_unlock(&gClientMutex);
    // No change notification here: it woke every device listener (e.g. mediaremoted) on each client connect, which reconnected and looped. The app polls apcl instead.
    return 0;
}

static OSStatus PAV_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, const AudioServerPlugInClientInfo* ci) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gClientMutex);
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (atomic_load(&c->used) && c->clientID == ci->mClientID) {
            atomic_store_explicit(&c->used, false, memory_order_release);
            if (c->bundleID) { CFRelease(c->bundleID); c->bundleID = NULL; }
            c->bundleIDC[0] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&gClientMutex);
    // No change notification here: it woke every device listener (e.g. mediaremoted) on each client connect, which reconnected and looped. The app polls apcl instead.
    return 0;
}

static OSStatus PAV_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt64 action, void* info) {
    (void)action; (void)info;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (IsSupportedRate(gPendingSampleRate)) {
        gSampleRate = gPendingSampleRate;
        pthread_mutex_lock(&gClockMutex);
        RecomputeHostTicks();
        gClockTicksPerFrame = gHostTicksPerFrame;
        pthread_mutex_unlock(&gClockMutex);
    }
    gLatencyFrames = gPendingLatencyFrames;
    Float64 r = gSampleRate;
    pthread_mutex_unlock(&gStateMutex);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat64Type, &r);
    gHost->WriteToStorage(gHost, CFSTR("SampleRate"), n);
    CFRelease(n);
    return 0;
}

static OSStatus PAV_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt64 action, void* info) {
    (void)action; (void)info;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus RequestRateChange(Float64 rate) {
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    bool changed = fabs(rate - gSampleRate) > 0.5;
    gPendingSampleRate = rate;
    pthread_mutex_unlock(&gStateMutex);
    if (changed) {
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, 0, NULL);
        });
    }
    return 0;
}

/// Presentation latency may only change through a configuration change (the host stops and restarts IO).
/// Who applies the master volume: the IO thread picks the change up and ramps to it.
static OSStatus SetMasterMode(CFPropertyListRef v) {
    SInt64 mode = 0;
    if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &mode) ||
        (mode != kPAVMasterInDriver && mode != kPAVMasterOnDevice))
        return kAudioHardwareIllegalOperationError;
    atomic_store(&gMasterOnDevice, mode == kPAVMasterOnDevice);
    return 0;
}

static OSStatus RequestLatencyChange(CFPropertyListRef v) {
    SInt64 frames = 0;
    if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &frames))
        return kAudioHardwareIllegalOperationError;
    if (frames < 0) frames = 0;
    if (frames > 192000) frames = 192000;
    pthread_mutex_lock(&gStateMutex);
    bool changed = (UInt32)frames != gLatencyFrames;
    gPendingLatencyFrames = (UInt32)frames;
    pthread_mutex_unlock(&gStateMutex);
    if (changed) {
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, 0, NULL);
        });
    }
    return 0;
}

#pragma mark - Property support

static Boolean PAV_HasProperty_Impl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a) {
    (void)pid;
    if (inDriver != gDriverRef || a == NULL) return false;
    switch (obj) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle: case kAudioPlugInPropertyBoxList: case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyClockDeviceList: case kAudioPlugInPropertyTranslateUIDToClockDevice:
            return true;
        }
        return false;
    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyStreams: case kAudioObjectPropertyCustomPropertyInfoList:
        case kPAVPropClients: case kPAVPropVolumes: case kPAVPropAudio: case kPAVPropSurface: case kPAVPropLatency:
        case kPAVPropRoutes: case kPAVPropMaster:
        case kAudioDevicePropertyClockIsStable: case kAudioDevicePropertyClockAlgorithm:
            return true;
        // Answered on every scope (like Apple's NullAudio sample); coreaudiod drops the device if these fail.
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo: case kAudioDevicePropertyPreferredChannelLayout:
            return true;
        }
        return false;
    case kObjectID_Stream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    case kObjectID_Volume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            return true;
        }
        return false;
    case kObjectID_Mute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
            return true;
        }
        return false;
    }
    return false;
}

static OSStatus PAV_IsPropertySettable_Impl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a, Boolean* out) {
    if (inDriver != gDriverRef || a == NULL || out == NULL) return kAudioHardwareIllegalOperationError;
    if (!PAV_HasProperty_Impl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *out = false;
    switch (obj) {
    case kObjectID_Device:
        *out = (a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kPAVPropVolumes ||
                a->mSelector == kPAVPropSurface || a->mSelector == kPAVPropLatency || a->mSelector == kPAVPropRoutes ||
                a->mSelector == kPAVPropMaster);
        break;
    case kObjectID_Stream:
        *out = (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                a->mSelector == kAudioStreamPropertyIsActive);
        break;
    case kObjectID_Volume:
        *out = (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue);
        break;
    case kObjectID_Mute:
        *out = (a->mSelector == kAudioBooleanControlPropertyValue);
        break;
    }
    return 0;
}

static UInt32 DeviceOwnedObjects(AudioObjectPropertyScope scope, AudioObjectID out[3]) {
    if (scope == kAudioObjectPropertyScopeInput) return 0;
    out[0] = kObjectID_Stream; out[1] = kObjectID_Volume; out[2] = kObjectID_Mute;
    return 3;
}

static OSStatus PAV_GetPropertyDataSize_Impl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                        UInt32 qs, const void* q, UInt32* out) {
    (void)qs; (void)q;
    if (inDriver != gDriverRef || a == NULL || out == NULL) return kAudioHardwareIllegalOperationError;
    if (!PAV_HasProperty_Impl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    AudioObjectID tmp[3];
    switch (obj) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: case kAudioPlugInPropertyTranslateUIDToDevice: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyResourceBundle: *out = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList: *out = sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyBoxList: case kAudioPlugInPropertyClockDeviceList: *out = 0; return 0;
        case kAudioPlugInPropertyTranslateUIDToBox: case kAudioPlugInPropertyTranslateUIDToClockDevice: *out = sizeof(AudioObjectID); return 0;
        }
        break;
    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: *out = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = DeviceOwnedObjects(a->mScope, tmp) * sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyTransportType: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable: case kAudioDevicePropertyClockAlgorithm:
            *out = sizeof(UInt32); return 0;
        case kAudioDevicePropertyRelatedDevices: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyControlList: *out = 2 * sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyStreams: *out = (a->mScope == kAudioObjectPropertyScopeInput ? 0 : 1) * sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyNominalSampleRate: *out = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: *out = kNumSampleRates * sizeof(AudioValueRange); return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo: *out = 2 * sizeof(UInt32); return 0;
        case kAudioDevicePropertyPreferredChannelLayout:
            *out = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription)); return 0;
        case kAudioObjectPropertyCustomPropertyInfoList: *out = kNumCustomProps * sizeof(AudioServerPlugInCustomPropertyInfo); return 0;
        case kPAVPropClients: case kPAVPropVolumes: case kPAVPropAudio: case kPAVPropSurface: case kPAVPropLatency:
        case kPAVPropRoutes: case kPAVPropMaster:
            *out = sizeof(CFPropertyListRef); return 0;
        }
        break;
    case kObjectID_Stream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = 0; return 0;
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: *out = sizeof(UInt32); return 0;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *out = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            *out = kNumSampleRates * sizeof(AudioStreamRangedDescription); return 0;
        }
        break;
    case kObjectID_Volume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = 0; return 0;
        case kAudioControlPropertyScope: *out = sizeof(AudioObjectPropertyScope); return 0;
        case kAudioControlPropertyElement: *out = sizeof(AudioObjectPropertyElement); return 0;
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar:
            *out = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyDecibelRange: *out = sizeof(AudioValueRange); return 0;
        }
        break;
    case kObjectID_Mute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *out = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner: *out = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects: *out = 0; return 0;
        case kAudioControlPropertyScope: *out = sizeof(AudioObjectPropertyScope); return 0;
        case kAudioControlPropertyElement: *out = sizeof(AudioObjectPropertyElement); return 0;
        case kAudioBooleanControlPropertyValue: *out = sizeof(UInt32); return 0;
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

#define NEED(sz) do { if (inSize < (UInt32)(sz)) return kAudioHardwareBadPropertySizeError; } while (0)
#define PUT(type, val) do { NEED(sizeof(type)); *(type*)out = (val); *outSize = sizeof(type); return 0; } while (0)

static CFPropertyListRef CopyClientList(void) {
    CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    pthread_mutex_lock(&gClientMutex);
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (!atomic_load(&c->used)) continue;
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        SInt32 cid = (SInt32)c->clientID, p = (SInt32)c->pid;
        float peak = atomic_exchange(&c->peak, 0.0f);
        float gain = atomic_load(&c->gain);
        SInt32 bus = (SInt32)atomic_load(&c->bus);
        CFNumberRef nbus = CFNumberCreate(NULL, kCFNumberSInt32Type, &bus);
        CFDictionarySetValue(d, CFSTR("bus"), nbus);
        CFRelease(nbus);
        CFNumberRef ncid = CFNumberCreate(NULL, kCFNumberSInt32Type, &cid);
        CFNumberRef npid = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
        CFNumberRef npk  = CFNumberCreate(NULL, kCFNumberFloat32Type, &peak);
        CFNumberRef ng   = CFNumberCreate(NULL, kCFNumberFloat32Type, &gain);
        CFDictionarySetValue(d, CFSTR("cid"), ncid);
        CFDictionarySetValue(d, CFSTR("pid"), npid);
        CFDictionarySetValue(d, CFSTR("peak"), npk);
        CFDictionarySetValue(d, CFSTR("gain"), ng);
        CFDictionarySetValue(d, CFSTR("run"), atomic_load(&c->running) ? kCFBooleanTrue : kCFBooleanFalse);
        if (c->bundleID) CFDictionarySetValue(d, CFSTR("bid"), c->bundleID);
        CFRelease(ncid); CFRelease(npid); CFRelease(npk); CFRelease(ng);
        CFArrayAppendValue(arr, d);
        CFRelease(d);
    }
    pthread_mutex_unlock(&gClientMutex);
    return arr;
}

/// A table as the dictionary the app sent: { "<pid>" | "b:<bundle id>" : value }.
static CFMutableDictionaryRef CopyTableLocked(const PAVTable* t) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < t->pidCount; ++i) {
        CFStringRef k = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), (int)t->pids[i].pid);
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &t->pids[i].value);
        CFDictionarySetValue(d, k, n);
        CFRelease(k); CFRelease(n);
    }
    for (int i = 0; i < t->bundleCount; ++i) {
        CFStringRef k = CFStringCreateWithFormat(NULL, NULL, CFSTR(kPAVBundleGainPrefix "%s"), t->bundles[i].bundleID);
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &t->bundles[i].value);
        if (k) { CFDictionarySetValue(d, k, n); CFRelease(k); }
        CFRelease(n);
    }
    return d;
}

static CFPropertyListRef CopyVolumeTable(void) {
    pthread_mutex_lock(&gClientMutex);
    CFMutableDictionaryRef d = CopyTableLocked(&gGains);
    pthread_mutex_unlock(&gClientMutex);
    return d;
}

typedef struct { PAVTable* table; float min, max; bool whole; } TableCtx;
static void TableApplier(const void* key, const void* value, void* ctx) {
    TableCtx* tc = (TableCtx*)ctx;
    PAVTable* t = tc->table;
    if (CFGetTypeID(key) != CFStringGetTypeID() || CFGetTypeID(value) != CFNumberGetTypeID()) return;
    Float32 v = tc->min;
    CFNumberGetValue((CFNumberRef)value, kCFNumberFloat32Type, &v);
    if (!(v >= tc->min)) v = tc->min;
    if (v > tc->max) v = tc->max;
    if (tc->whole) v = floorf(v);
    if (CFStringHasPrefix((CFStringRef)key, CFSTR(kPAVBundleGainPrefix))) {
        if (t->bundleCount >= kMaxBundleGains) return;
        char buf[kBundleIDMax + sizeof(kPAVBundleGainPrefix)];
        if (!CFStringGetCString((CFStringRef)key, buf, sizeof(buf), kCFStringEncodingUTF8)) return;
        const char* bid = buf + sizeof(kPAVBundleGainPrefix) - 1;
        if (bid[0] == 0 || strlen(bid) >= kBundleIDMax) return;
        strlcpy(t->bundles[t->bundleCount].bundleID, bid, kBundleIDMax);
        t->bundles[t->bundleCount].value = v;
        t->bundleCount++;
        return;
    }
    if (t->pidCount >= kMaxPidGains) return;
    SInt32 p = CFStringGetIntValue((CFStringRef)key);
    if (p <= 0) return;
    t->pids[t->pidCount].pid = (pid_t)p;
    t->pids[t->pidCount].value = v;
    t->pidCount++;
}

/// Replaces a table with the dictionary's contents. Call with gClientMutex held.
static void FillTableLocked(PAVTable* t, CFDictionaryRef dict, float min, float max, bool whole) {
    TableCtx ctx = { t, min, max, whole };
    t->pidCount = 0;
    t->bundleCount = 0;
    CFDictionaryApplyFunction(dict, TableApplier, &ctx);
}

static void SetVolumeTable(CFPropertyListRef plist) {
    if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return;
    pthread_mutex_lock(&gClientMutex);
    FillTableLocked(&gGains, (CFDictionaryRef)plist, 0.0f, 4.0f, false);
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (atomic_load(&c->used)) atomic_store(&c->gain, LookupGainLocked(c->pid, c->bundleIDC));
    }
    pthread_mutex_unlock(&gClientMutex);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // IOSurfaceLookup: still the way to share by ID
/// Maps the ring the app created (its IOSurface ID) and marks it attached. `*outSurf` stays locked.
static OSStatus OpenSharedRing(SInt64 id, IOSurfaceRef* outSurf, PAVSharedRing** outRing) {
    IOSurfaceRef surf = IOSurfaceLookup((IOSurfaceID)id);
    if (surf == NULL) { os_log_error(gLog, "PAV shm lookup %lld failed", id); return kAudioHardwareIllegalOperationError; }
    if (IOSurfaceGetAllocSize(surf) < PAV_SHM_BYTES) { CFRelease(surf); return kAudioHardwareBadPropertySizeError; }
    IOSurfaceLock(surf, 0, NULL);   // CPU-only memory: base address stays valid; keep it locked while attached
    PAVSharedRing* ring = (PAVSharedRing*)IOSurfaceGetBaseAddress(surf);
    if (ring->magic != PAV_SHM_MAGIC || ring->version != PAV_SHM_VERSION || ring->capacityFrames != PAV_SHM_FRAMES) {
        IOSurfaceUnlock(surf, 0, NULL); CFRelease(surf);
        return kAudioHardwareIllegalOperationError;
    }
    atomic_store(&ring->writeEnd, atomic_load(&gWriteEnd));
    atomic_store(&ring->chunkFrames, 0);
    atomic_store(&ring->features, PAV_SHM_FEATURE_CHUNK);
    atomic_fetch_add(&ring->generation, 1);
    atomic_store(&ring->attached, 1);
    *outSurf = surf; *outRing = ring;
    return 0;
}
#pragma clang diagnostic pop

/// A surface that was just replaced. The IO thread may still hold its pointer for one cycle, so it is
/// released a few swaps later. Call with gShmMutex held.
static void RetireSurfaceLocked(IOSurfaceRef old) {
    if (old == NULL) return;
    if (gShmGraveyardN == kShmGraveyardSize) {
        IOSurfaceUnlock(gShmGraveyard[0], 0, NULL); CFRelease(gShmGraveyard[0]);
        memmove(gShmGraveyard, gShmGraveyard + 1, (kShmGraveyardSize - 1) * sizeof(IOSurfaceRef));
        gShmGraveyardN = kShmGraveyardSize - 1;
    }
    gShmGraveyard[gShmGraveyardN++] = old;
}

static OSStatus AttachSharedRing(CFPropertyListRef v) {
    SInt64 id = 0;
    if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &id))
        return kAudioHardwareIllegalOperationError;
    IOSurfaceRef surf = NULL;
    PAVSharedRing* ring = NULL;
    if (id != 0) {
        OSStatus st = OpenSharedRing(id, &surf, &ring);
        if (st != 0) return st;
    }
    pthread_mutex_lock(&gShmMutex);
    IOSurfaceRef old = gShmSurface;
    gShmSurface = surf;
    gShmID = surf ? (IOSurfaceID)id : 0;
    atomic_store_explicit(&gShm, ring, memory_order_release);
    RetireSurfaceLocked(old);
    pthread_mutex_unlock(&gShmMutex);
    os_log(gLog, "PAV shm attached id=%lld", id);
    return 0;
}

#pragma mark - Per-app outputs

/// Gives bus `b` the ring with this surface ID (0: none). The same ring again changes nothing.
static OSStatus AttachBusRing(int b, SInt64 id) {
    PAVBus* bus = &gBuses[b];
    pthread_mutex_lock(&gShmMutex);
    bool same = bus->surfaceID == (IOSurfaceID)id;
    pthread_mutex_unlock(&gShmMutex);
    if (same) return 0;
    IOSurfaceRef surf = NULL;
    PAVSharedRing* ring = NULL;
    OSStatus st = id != 0 ? OpenSharedRing(id, &surf, &ring) : 0;
    pthread_mutex_lock(&gShmMutex);
    IOSurfaceRef old = bus->surface;
    bus->surface = surf;
    bus->surfaceID = surf ? (IOSurfaceID)id : 0;
    atomic_store_explicit(&bus->ring, ring, memory_order_release);
    RetireSurfaceLocked(old);
    pthread_mutex_unlock(&gShmMutex);
    os_log(gLog, "PAV bus %d ring id=%lld (%d)", b, id, (int)st);
    return st;
}

static OSStatus SetRoutes(CFPropertyListRef plist) {
    if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
    OSStatus result = 0;
    CFTypeRef surfaces = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR(kPAVRoutesKeySurfaces));
    if (surfaces && CFGetTypeID(surfaces) == CFArrayGetTypeID()) {
        for (int b = 1; b <= kPAVMaxBuses; ++b) {
            SInt64 id = 0;
            if (b - 1 < CFArrayGetCount((CFArrayRef)surfaces)) {
                CFTypeRef n = CFArrayGetValueAtIndex((CFArrayRef)surfaces, b - 1);
                if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)n, kCFNumberSInt64Type, &id);
            }
            OSStatus st = AttachBusRing(b, id);
            if (st != 0 && result == 0) result = st;
        }
    }
    CFTypeRef routes = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR(kPAVRoutesKeyRoutes));
    if (routes && CFGetTypeID(routes) == CFDictionaryGetTypeID()) {
        pthread_mutex_lock(&gClientMutex);
        FillTableLocked(&gRoutes, (CFDictionaryRef)routes, 0.0f, (float)kPAVMaxBuses, true);
        for (int i = 0; i < kMaxClients; ++i) {
            PAVClient* c = &gClients[i];
            if (atomic_load(&c->used)) atomic_store(&c->bus, LookupBusLocked(c->pid, c->bundleIDC));
        }
        pthread_mutex_unlock(&gClientMutex);
    }
    return result;
}

/// The route table and which buses have a ring. Never the surface IDs: they would let any process map the audio.
static CFPropertyListRef CopyRoutes(void) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    pthread_mutex_lock(&gClientMutex);
    CFMutableDictionaryRef routes = CopyTableLocked(&gRoutes);
    pthread_mutex_unlock(&gClientMutex);
    CFDictionarySetValue(d, CFSTR(kPAVRoutesKeyRoutes), routes);
    CFRelease(routes);
    CFMutableArrayRef attached = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    pthread_mutex_lock(&gShmMutex);
    for (int b = 1; b <= kPAVMaxBuses; ++b) CFArrayAppendValue(attached, gBuses[b].surfaceID != 0 ? kCFBooleanTrue : kCFBooleanFalse);
    pthread_mutex_unlock(&gShmMutex);
    CFDictionarySetValue(d, CFSTR(kPAVRoutesKeySurfaces), attached);
    CFRelease(attached);
    return d;
}

static void RingZero(PAVSharedRing* ring, uint64_t from, uint64_t end) {
    if (end <= from) return;
    uint64_t n = end - from;
    if (n >= PAV_SHM_FRAMES) { memset(ring->data, 0, sizeof(ring->data)); return; }
    uint32_t start = (uint32_t)(from & PAV_SHM_MASK);
    uint32_t first = (uint32_t)n;
    if (start + first > PAV_SHM_FRAMES) first = PAV_SHM_FRAMES - start;
    memset(&ring->data[start * 2], 0, first * kBytesPerFrame);
    if (first < n) memset(&ring->data[0], 0, (size_t)(n - first) * kBytesPerFrame);
}

/// Makes every frame below `end` either silence or audio that was already mixed in, so that [st, end) can
/// be added to. Call with gBusLock held.
static void BusClearToLocked(PAVBus* bus, PAVSharedRing* ring, uint64_t st, uint64_t end) {
    uint64_t from = bus->clearedEnd;
    // Another ring, or the sample time jumped (IO restarted, or it ran far ahead): start over at `st`.
    if (ring != bus->clearedRing || from > end + PAV_SHM_FRAMES / 2 || from + PAV_SHM_FRAMES / 2 < st) {
        bus->clearedRing = ring;
        from = st;
    } else if (from >= end) {
        return;
    }
    RingZero(ring, from, end);
    bus->clearedEnd = end;
}

/// Adds one client's audio, times a gain going from `g0` to `g1`, to a bus at its sample time.
/// False (nothing done) if the bus has no ring.
static bool BusAdd(int b, const float* buf, UInt32 frames, uint64_t st, float g0, float g1) {
    PAVBus* bus = &gBuses[b];
    PAVSharedRing* ring = atomic_load_explicit(&bus->ring, memory_order_acquire);
    if (ring == NULL || frames > kMaxChunkFrames) return false;
    float step = (g1 - g0) / (float)frames;
    float g = g0;
    os_unfair_lock_lock(&gBusLock);
    BusClearToLocked(bus, ring, st, st + frames);
    for (UInt32 f = 0; f < frames; ++f) {
        g += step;
        uint32_t i = (uint32_t)((st + f) & PAV_SHM_MASK);
        ring->data[i * 2]     += buf[f * 2] * g;
        ring->data[i * 2 + 1] += buf[f * 2 + 1] * g;
    }
    os_unfair_lock_unlock(&gBusLock);
    return true;
}

/// Sends a client's audio (its gain already applied) where it belongs. What is left in `buf` afterwards
/// is its share of the main mix. A change of output is cross-faded over one cycle.
static void RouteClient(PAVClient* c, float* buf, UInt32 frames, uint64_t st) {
    int to = atomic_load_explicit(&c->bus, memory_order_relaxed);
    if (to < 0 || to > kPAVMaxBuses || (to != 0 && atomic_load_explicit(&gBuses[to].ring, memory_order_acquire) == NULL)) to = 0;
    int from = c->busApplied;
    if (to == from) {
        if (to != 0 && BusAdd(to, buf, frames, st, 1.0f, 1.0f)) memset(buf, 0, frames * kBytesPerFrame);
        return;
    }
    if (from != 0) BusAdd(from, buf, frames, st, 1.0f, 0.0f);
    if (to != 0)   BusAdd(to, buf, frames, st, 0.0f, 1.0f);
    if (from == 0)    ApplyRampedGain(buf, frames, 1.0f, 0.0f);
    else if (to == 0) ApplyRampedGain(buf, frames, 0.0f, 1.0f);
    else              memset(buf, 0, frames * kBytesPerFrame);
    c->busApplied = to;
}

/// The device's timeline reached `st + frames`: every bus ring is readable up to there, like the main one.
static void PublishBuses(UInt32 frames, uint64_t st) {
    uint64_t end = st + frames;
    for (int b = 1; b <= kPAVMaxBuses; ++b) {
        PAVBus* bus = &gBuses[b];
        PAVSharedRing* ring = atomic_load_explicit(&bus->ring, memory_order_acquire);
        if (ring == NULL) continue;
        os_unfair_lock_lock(&gBusLock);
        BusClearToLocked(bus, ring, st, end);
        os_unfair_lock_unlock(&gBusLock);
        atomic_store_explicit(&ring->chunkFrames, frames, memory_order_relaxed);
        uint64_t pe = atomic_load_explicit(&ring->writeEnd, memory_order_relaxed);
        if (end > pe || st + PAV_SHM_FRAMES < pe) atomic_store_explicit(&ring->writeEnd, end, memory_order_release);
    }
}

static CFPropertyListRef CopyAudioChunk(void) {
    pthread_mutex_lock(&gReadMutex);
    uint32_t gen = atomic_load_explicit(&gStreamGen, memory_order_acquire);
    uint64_t end = atomic_load_explicit(&gWriteEnd, memory_order_acquire);
    if (gen != gReadGen || gReadPos > end || end - gReadPos > kDataRingFrames / 2) {
        // (re)sync: start at the newest data
        gReadGen = gen;
        gReadPos = end;
    }
    uint64_t avail = end - gReadPos;
    if (avail > kMaxChunkFrames) { gReadPos = end - kMaxChunkFrames; avail = kMaxChunkFrames; }
    CFMutableDataRef data = CFDataCreateMutable(NULL, (CFIndex)(avail * kBytesPerFrame));
    CFDataSetLength(data, (CFIndex)(avail * kBytesPerFrame));
    if (avail) {
        float* dst = (float*)CFDataGetMutableBytePtr(data);
        uint32_t start = (uint32_t)(gReadPos & kDataRingMask);
        uint32_t first = (uint32_t)avail;
        if (start + first > kDataRingFrames) first = kDataRingFrames - start;
        memcpy(dst, &gDataRing[start * kChannels], first * kBytesPerFrame);
        if (first < avail) memcpy(dst + first * kChannels, &gDataRing[0], (size_t)(avail - first) * kBytesPerFrame);
    }
    gReadPos += avail;
    pthread_mutex_unlock(&gReadMutex);
    return data;
}

static OSStatus PAV_GetPropertyData_Impl(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                    UInt32 qs, const void* q, UInt32 inSize, UInt32* outSize, void* out) {
    if (inDriver != gDriverRef || a == NULL || outSize == NULL || out == NULL) return kAudioHardwareIllegalOperationError;
    if (!PAV_HasProperty_Impl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

    switch (obj) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer_Name));
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (inSize < sizeof(AudioObjectID)) { *outSize = 0; return 0; }
            PUT(AudioObjectID, kObjectID_Device);
        case kAudioPlugInPropertyBoxList: case kAudioPlugInPropertyClockDeviceList: *outSize = 0; return 0;
        case kAudioPlugInPropertyTranslateUIDToBox: case kAudioPlugInPropertyTranslateUIDToClockDevice:
            PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            NEED(sizeof(AudioObjectID));
            if (qs != sizeof(CFStringRef) || q == NULL) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef*)q;
            *(AudioObjectID*)out = (uid && CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo) ? kObjectID_Device : kAudioObjectUnknown;
            *outSize = sizeof(AudioObjectID);
            return 0;
        }
        }
        break;

    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_PlugIn);
        case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR(kDevice_Name));
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer_Name));
        case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(kDevice_UID));
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(kDevice_ModelUID));
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, kObjectID_Device);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: {
            pthread_mutex_lock(&gClockMutex); UInt32 r = gIORunningCount > 0; pthread_mutex_unlock(&gClockMutex);
            PUT(UInt32, r);
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyLatency: {
            pthread_mutex_lock(&gStateMutex); UInt32 l = gLatencyFrames; pthread_mutex_unlock(&gStateMutex);
            PUT(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : l);
        }
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0);
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kZeroTimeStampPeriod);
        case kAudioDevicePropertyClockIsStable: PUT(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: PUT(UInt32, kAudioDeviceClockAlgorithmSimpleIIR);
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID ids[3];
            UInt32 n = DeviceOwnedObjects(a->mScope, ids);
            UInt32 maxN = inSize / sizeof(AudioObjectID);
            if (n > maxN) n = maxN;
            memcpy(out, ids, n * sizeof(AudioObjectID));
            *outSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioObjectPropertyControlList: {
            AudioObjectID ids[2] = { kObjectID_Volume, kObjectID_Mute };
            UInt32 n = inSize / sizeof(AudioObjectID); if (n > 2) n = 2;
            memcpy(out, ids, n * sizeof(AudioObjectID));
            *outSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeInput || inSize < sizeof(AudioObjectID)) { *outSize = 0; return 0; }
            PUT(AudioObjectID, kObjectID_Stream);
        case kAudioDevicePropertyNominalSampleRate: {
            pthread_mutex_lock(&gStateMutex); Float64 r = gSampleRate; pthread_mutex_unlock(&gStateMutex);
            PUT(Float64, r);
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = inSize / sizeof(AudioValueRange); if (n > kNumSampleRates) n = kNumSampleRates;
            AudioValueRange* r = (AudioValueRange*)out;
            for (UInt32 i = 0; i < n; ++i) { r[i].mMinimum = kSampleRates[i]; r[i].mMaximum = kSampleRates[i]; }
            *outSize = n * sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            NEED(2 * sizeof(UInt32));
            ((UInt32*)out)[0] = 1; ((UInt32*)out)[1] = 2;
            *outSize = 2 * sizeof(UInt32);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 sz = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
            NEED(sz);
            AudioChannelLayout* l = (AudioChannelLayout*)out;
            memset(l, 0, sz);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = kChannels;
            l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outSize = sz;
            return 0;
        }
        case kAudioObjectPropertyCustomPropertyInfoList: {
            AudioServerPlugInCustomPropertyInfo info[kNumCustomProps] = {
                { kPAVPropClients, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropVolumes, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropAudio,   kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropSurface, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropLatency, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropRoutes,  kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropMaster,  kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
            };
            UInt32 n = inSize / sizeof(AudioServerPlugInCustomPropertyInfo); if (n > kNumCustomProps) n = kNumCustomProps;
            memcpy(out, info, n * sizeof(AudioServerPlugInCustomPropertyInfo));
            *outSize = n * sizeof(AudioServerPlugInCustomPropertyInfo);
            return 0;
        }
        case kPAVPropClients: NEED(sizeof(CFPropertyListRef)); *(CFPropertyListRef*)out = CopyClientList(); *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropVolumes: NEED(sizeof(CFPropertyListRef)); *(CFPropertyListRef*)out = CopyVolumeTable(); *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropAudio:
            NEED(sizeof(CFPropertyListRef));
            if (!CallerMayControl(pid)) return kAudioHardwareIllegalOperationError;
            *(CFPropertyListRef*)out = CopyAudioChunk();  *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropSurface: {
            // Only whether a ring is attached: the surface ID would let any process map the mix.
            NEED(sizeof(CFPropertyListRef));
            pthread_mutex_lock(&gShmMutex);
            SInt64 on = gShmID != 0;
            pthread_mutex_unlock(&gShmMutex);
            *(CFPropertyListRef*)out = CFNumberCreate(NULL, kCFNumberSInt64Type, &on);
            *outSize = sizeof(CFPropertyListRef);
            return 0;
        }
        case kPAVPropRoutes: NEED(sizeof(CFPropertyListRef)); *(CFPropertyListRef*)out = CopyRoutes(); *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropMaster: {
            NEED(sizeof(CFPropertyListRef));
            SInt64 m = atomic_load(&gMasterOnDevice) ? kPAVMasterOnDevice : kPAVMasterInDriver;
            *(CFPropertyListRef*)out = CFNumberCreate(NULL, kCFNumberSInt64Type, &m);
            *outSize = sizeof(CFPropertyListRef);
            return 0;
        }
        case kPAVPropLatency: {
            NEED(sizeof(CFPropertyListRef));
            pthread_mutex_lock(&gStateMutex); SInt64 l = gLatencyFrames; pthread_mutex_unlock(&gStateMutex);
            *(CFPropertyListRef*)out = CFNumberCreate(NULL, kCFNumberSInt64Type, &l);
            *outSize = sizeof(CFPropertyListRef);
            return 0;
        }
        }
        break;

    case kObjectID_Stream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, 0);   // 0 = output
        case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            NEED(sizeof(AudioStreamBasicDescription));
            pthread_mutex_lock(&gStateMutex); Float64 r = gSampleRate; pthread_mutex_unlock(&gStateMutex);
            FillASBD((AudioStreamBasicDescription*)out, r);
            *outSize = sizeof(AudioStreamBasicDescription);
            return 0;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = inSize / sizeof(AudioStreamRangedDescription); if (n > kNumSampleRates) n = kNumSampleRates;
            AudioStreamRangedDescription* d = (AudioStreamRangedDescription*)out;
            for (UInt32 i = 0; i < n; ++i) {
                FillASBD(&d[i].mFormat, kSampleRates[i]);
                d[i].mSampleRateRange.mMinimum = kSampleRates[i];
                d[i].mSampleRateRange.mMaximum = kSampleRates[i];
            }
            *outSize = n * sizeof(AudioStreamRangedDescription);
            return 0;
        }
        }
        break;

    case kObjectID_Volume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioControlPropertyScope: PUT(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: PUT(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: {
            pthread_mutex_lock(&gStateMutex); Float32 s = gVolumeScalar; pthread_mutex_unlock(&gStateMutex);
            PUT(Float32, s);
        }
        case kAudioLevelControlPropertyDecibelValue: {
            pthread_mutex_lock(&gStateMutex); Float32 s = gVolumeScalar; pthread_mutex_unlock(&gStateMutex);
            PUT(Float32, ScalarToDB(s));
        }
        case kAudioLevelControlPropertyDecibelRange: {
            NEED(sizeof(AudioValueRange));
            ((AudioValueRange*)out)->mMinimum = kVolumeMinDB;
            ((AudioValueRange*)out)->mMaximum = kVolumeMaxDB;
            *outSize = sizeof(AudioValueRange);
            return 0;
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            NEED(sizeof(Float32));
            Float32 s = *(Float32*)out; if (s < 0) s = 0; if (s > 1) s = 1;
            *(Float32*)out = ScalarToDB(s); *outSize = sizeof(Float32); return 0;
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            NEED(sizeof(Float32));
            *(Float32*)out = DBToScalar(*(Float32*)out); *outSize = sizeof(Float32); return 0;
        }
        }
        break;

    case kObjectID_Mute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioControlPropertyScope: PUT(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: PUT(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue: PUT(UInt32, atomic_load(&gMuted) ? 1 : 0);
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

static void SetVolumeScalar(Float32 s) {
    if (s < 0) s = 0; if (s > 1) s = 1;
    pthread_mutex_lock(&gStateMutex);
    bool changed = fabsf(s - gVolumeScalar) > 1e-6f;
    gVolumeScalar = s;
    pthread_mutex_unlock(&gStateMutex);
    atomic_store(&gMasterGain, s * s);
    if (changed) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &s);
        gHost->WriteToStorage(gHost, CFSTR("Volume"), n);
        CFRelease(n);
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            AudioObjectPropertyAddress local[2] = {
                { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            };
            gHost->PropertiesChanged(gHost, kObjectID_Volume, 2, local);
        });
    }
}

static OSStatus PAV_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                    UInt32 qs, const void* q, UInt32 inSize, const void* in) {
    (void)qs; (void)q;
    if (inDriver != gDriverRef || a == NULL || in == NULL) return kAudioHardwareIllegalOperationError;
    if (!PAV_HasProperty_Impl(inDriver, obj, pid, a)) return kAudioHardwareUnknownPropertyError;

    switch (obj) {
    case kObjectID_Device:
        if (a->mSelector == kAudioDevicePropertyNominalSampleRate) {
            if (inSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            return RequestRateChange(*(const Float64*)in);
        }
        if (a->mSelector == kPAVPropVolumes || a->mSelector == kPAVPropSurface || a->mSelector == kPAVPropLatency ||
            a->mSelector == kPAVPropRoutes || a->mSelector == kPAVPropMaster) {
            if (inSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
            if (!CallerMayControl(pid)) return kAudioHardwareIllegalOperationError;
            CFPropertyListRef v = *(const CFPropertyListRef*)in;
            if (a->mSelector == kPAVPropVolumes) { SetVolumeTable(v); return 0; }
            if (a->mSelector == kPAVPropSurface) return AttachSharedRing(v);
            if (a->mSelector == kPAVPropRoutes) return SetRoutes(v);
            if (a->mSelector == kPAVPropMaster) return SetMasterMode(v);
            return RequestLatencyChange(v);
        }
        break;
    case kObjectID_Stream:
        if (a->mSelector == kAudioStreamPropertyIsActive) return 0;
        if (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat) {
            if (inSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            const AudioStreamBasicDescription* d = (const AudioStreamBasicDescription*)in;
            if (d->mFormatID != kAudioFormatLinearPCM || d->mChannelsPerFrame != kChannels || d->mBitsPerChannel != 32 ||
                !(d->mFormatFlags & kAudioFormatFlagIsFloat))
                return kAudioDeviceUnsupportedFormatError;
            return RequestRateChange(d->mSampleRate);
        }
        break;
    case kObjectID_Volume:
        if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        if (a->mSelector == kAudioLevelControlPropertyScalarValue) { SetVolumeScalar(*(const Float32*)in); return 0; }
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) { SetVolumeScalar(DBToScalar(*(const Float32*)in)); return 0; }
        break;
    case kObjectID_Mute:
        if (a->mSelector == kAudioBooleanControlPropertyValue) {
            if (inSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            bool m = *(const UInt32*)in != 0;
            bool old = atomic_exchange(&gMuted, m);
            if (old != m) NotifyAsync(kObjectID_Mute, kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal);
            return 0;
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - IO

static OSStatus PAV_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 clientID) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gClockMutex);
    if (gIORunningCount == 0) {
        gNumberTimeStamps = 0;
        gAnchorIndex = 0;
        gAnchorHostTime = mach_absolute_time();
        gClockTicksPerFrame = gHostTicksPerFrame;
        atomic_store(&gWriteEnd, 0);
        atomic_fetch_add(&gStreamGen, 1);
        PAVSharedRing* ring = atomic_load_explicit(&gShm, memory_order_acquire);
        if (ring) { atomic_store_explicit(&ring->writeEnd, 0, memory_order_release); atomic_fetch_add(&ring->generation, 1); }
        for (int b = 1; b <= kPAVMaxBuses; ++b) {
            PAVSharedRing* br = atomic_load_explicit(&gBuses[b].ring, memory_order_acquire);
            os_unfair_lock_lock(&gBusLock);
            gBuses[b].clearedEnd = 0;
            gBuses[b].clearedRing = NULL;
            os_unfair_lock_unlock(&gBusLock);
            if (br) { atomic_store_explicit(&br->writeEnd, 0, memory_order_release); atomic_fetch_add(&br->generation, 1); }
        }
    }
    gIORunningCount++;
    pthread_mutex_unlock(&gClockMutex);

    pthread_mutex_lock(&gClientMutex);
    PAVClient* c = FindClient(clientID);
    if (c) atomic_store(&c->running, true);
    pthread_mutex_unlock(&gClientMutex);
    // No change notification here: it woke every device listener (e.g. mediaremoted) on each client connect, which reconnected and looped. The app polls apcl instead.
    return 0;
}

static OSStatus PAV_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 clientID) {
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gClockMutex);
    if (gIORunningCount > 0) gIORunningCount--;
    pthread_mutex_unlock(&gClockMutex);

    pthread_mutex_lock(&gClientMutex);
    PAVClient* c = FindClient(clientID);
    if (c) atomic_store(&c->running, false);
    pthread_mutex_unlock(&gClientMutex);
    // No change notification here: it woke every device listener (e.g. mediaremoted) on each client connect, which reconnected and looped. The app polls apcl instead.
    return 0;
}

/// Clock rate to use: the real output device's rate published by the app through the shared ring
/// (so the virtual device runs exactly in step with it and nothing ever has to be resampled), or nominal.
static Float64 CurrentTicksPerFrame(void) {
    PAVSharedRing* ring = atomic_load_explicit(&gShm, memory_order_acquire);
    if (ring) {
        uint64_t bits = atomic_load_explicit(&ring->clockTicksPerFrame, memory_order_relaxed);
        Float64 t; memcpy(&t, &bits, sizeof t);
        // Sanity: within ±0.1% of nominal (real clocks are within ~100 ppm).
        if (bits != 0 && t > gHostTicksPerFrame * 0.999 && t < gHostTicksPerFrame * 1.001) {
            if (!atomic_load_explicit(&ring->clockFollowed, memory_order_relaxed))
                atomic_store_explicit(&ring->clockFollowed, 1, memory_order_relaxed);
            return t;
        }
        if (atomic_load_explicit(&ring->clockFollowed, memory_order_relaxed))
            atomic_store_explicit(&ring->clockFollowed, 0, memory_order_relaxed);
    }
    return gHostTicksPerFrame;
}

static OSStatus PAV_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 clientID,
                                     Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)clientID;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gClockMutex);
    Float64 tpf = CurrentTicksPerFrame();
    if (gClockTicksPerFrame <= 0) gClockTicksPerFrame = tpf;
    if (tpf != gClockTicksPerFrame) {
        // Rate changed: re-anchor at the current timestamp so the timeline stays continuous.
        gAnchorHostTime += (UInt64)((Float64)(gNumberTimeStamps - gAnchorIndex) * gClockTicksPerFrame * kZeroTimeStampPeriod);
        gAnchorIndex = gNumberTimeStamps;
        gClockTicksPerFrame = tpf;
    }
    Float64 ticksPerRing = gClockTicksPerFrame * (Float64)kZeroTimeStampPeriod;
    UInt64 now = mach_absolute_time();
    UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps + 1 - gAnchorIndex) * ticksPerRing);
    if (next <= now) ++gNumberTimeStamps;
    *outSampleTime = (Float64)(gNumberTimeStamps * kZeroTimeStampPeriod);
    *outHostTime   = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps - gAnchorIndex) * ticksPerRing);
    *outSeed       = 1;
    pthread_mutex_unlock(&gClockMutex);
    return 0;
}

static OSStatus PAV_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 clientID, UInt32 op,
                                      Boolean* outWillDo, Boolean* outInPlace) {
    (void)clientID;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    bool will = (op == kAudioServerPlugInIOOperationProcessOutput || op == kAudioServerPlugInIOOperationWriteMix);
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return 0;
}

static OSStatus PAV_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 c, UInt32 op, UInt32 n,
                                     const AudioServerPlugInIOCycleInfo* ci) {
    (void)c; (void)op; (void)n; (void)ci;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus PAV_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, AudioObjectID inStream, UInt32 clientID,
                                  UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo* ci, void* mainBuf, void* secBuf) {
    (void)inStream; (void)secBuf;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    float* buf = (float*)mainBuf;
    if (buf == NULL || frames == 0) return 0;

    if (op == kAudioServerPlugInIOOperationProcessOutput) {
        PAVClient* c = FindClient(clientID);
        if (c) {
            float target = atomic_load_explicit(&c->gain, memory_order_relaxed);
            ApplyRampedGain(buf, frames, c->applied, target);
            c->applied = target;
            float pk = 0.0f;
            UInt32 n = frames * kChannels;
            for (UInt32 i = 0; i < n; ++i) { float v = fabsf(buf[i]); if (v > pk) pk = v; }
            float old = atomic_load_explicit(&c->peak, memory_order_relaxed);
            if (pk > old) atomic_store_explicit(&c->peak, pk, memory_order_relaxed);
            RouteClient(c, buf, frames, (uint64_t)ci->mOutputTime.mSampleTime);
        }
    } else if (op == kAudioServerPlugInIOOperationWriteMix) {
        float target = atomic_load_explicit(&gMuted, memory_order_relaxed) ? 0.0f
                     : atomic_load_explicit(&gMasterOnDevice, memory_order_relaxed) ? 1.0f
                     : atomic_load_explicit(&gMasterGain, memory_order_relaxed);
        ApplyRampedGain(buf, frames, gMasterApplied, target);
        gMasterApplied = target;

        uint64_t st = (uint64_t)ci->mOutputTime.mSampleTime;
        uint64_t prevEnd = atomic_load_explicit(&gWriteEnd, memory_order_relaxed);
        if (st > prevEnd && prevEnd != 0 && st - prevEnd < kDataRingFrames) {
            for (uint64_t t = prevEnd; t < st; ++t) {          // zero a gap (overload)
                uint32_t i = (uint32_t)(t & kDataRingMask);
                gDataRing[i * 2] = 0; gDataRing[i * 2 + 1] = 0;
            }
        }
        uint32_t start = (uint32_t)(st & kDataRingMask);
        uint32_t first = frames;
        if (start + first > kDataRingFrames) first = kDataRingFrames - start;
        memcpy(&gDataRing[start * kChannels], buf, first * kBytesPerFrame);
        if (first < frames) memcpy(&gDataRing[0], buf + first * kChannels, (frames - first) * kBytesPerFrame);
        uint64_t end = st + frames;
        if (end > prevEnd || st + kDataRingFrames < prevEnd) atomic_store_explicit(&gWriteEnd, end, memory_order_release);

        PAVSharedRing* ring = atomic_load_explicit(&gShm, memory_order_acquire);
        if (ring) {
            uint64_t pe = atomic_load_explicit(&ring->writeEnd, memory_order_relaxed);
            if (st > pe && pe != 0 && st - pe < PAV_SHM_FRAMES)
                for (uint64_t t = pe; t < st; ++t) { uint32_t i = (uint32_t)(t & PAV_SHM_MASK); ring->data[i * 2] = 0; ring->data[i * 2 + 1] = 0; }
            uint32_t s0 = (uint32_t)(st & PAV_SHM_MASK);
            uint32_t f0 = frames;
            if (s0 + f0 > PAV_SHM_FRAMES) f0 = PAV_SHM_FRAMES - s0;
            memcpy(&ring->data[s0 * 2], buf, f0 * kBytesPerFrame);
            if (f0 < frames) memcpy(&ring->data[0], buf + f0 * 2, (frames - f0) * kBytesPerFrame);
            atomic_store_explicit(&ring->chunkFrames, frames, memory_order_relaxed);
            if (end > pe || st + PAV_SHM_FRAMES < pe) atomic_store_explicit(&ring->writeEnd, end, memory_order_release);
        }
        PublishBuses(frames, st);
    }
    return 0;
}

static OSStatus PAV_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 c, UInt32 op, UInt32 n,
                                   const AudioServerPlugInIOCycleInfo* ci) {
    (void)c; (void)op; (void)n; (void)ci;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

#pragma mark - Tracing wrappers (diagnostics)

static void FourCC(UInt32 v, char out[5]) {
    out[0] = (char)(v >> 24); out[1] = (char)(v >> 16); out[2] = (char)(v >> 8); out[3] = (char)v; out[4] = 0;
    for (int i = 0; i < 4; ++i) if (out[i] < 32 || out[i] > 126) out[i] = '?';
}
static bool TraceOn(void) { return atomic_fetch_add(&gTraceCount, 1) < kTraceLimit; }

static Boolean PAV_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress* a) {
    Boolean r = PAV_HasProperty_Impl(d, o, p, a);
    if (a && a->mSelector != kPAVPropAudio && TraceOn()) {
        char s[5], sc[5]; FourCC(a->mSelector, s); FourCC(a->mScope, sc);
        os_log(gLog, "PAV has obj=%u %{public}s/%{public}s el=%u pid=%d -> %d", o, s, sc, a->mElement, p, r);
    }
    return r;
}
static OSStatus PAV_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress* a, Boolean* out) {
    OSStatus r = PAV_IsPropertySettable_Impl(d, o, p, a, out);
    if (a && TraceOn()) {
        char s[5], sc[5]; FourCC(a->mSelector, s); FourCC(a->mScope, sc);
        os_log(gLog, "PAV settable obj=%u %{public}s/%{public}s -> err=%d", o, s, sc, (int)r);
    }
    return r;
}
static OSStatus PAV_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress* a,
                                        UInt32 qs, const void* q, UInt32* out) {
    OSStatus r = PAV_GetPropertyDataSize_Impl(d, o, p, a, qs, q, out);
    if (a && a->mSelector != kPAVPropAudio && TraceOn()) {
        char s[5], sc[5]; FourCC(a->mSelector, s); FourCC(a->mScope, sc);
        os_log(gLog, "PAV size obj=%u %{public}s/%{public}s qs=%u -> err=%d size=%u", o, s, sc, qs, (int)r, out ? *out : 0);
    }
    return r;
}
static OSStatus PAV_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress* a,
                                    UInt32 qs, const void* q, UInt32 inSize, UInt32* outSize, void* out) {
    OSStatus r = PAV_GetPropertyData_Impl(d, o, p, a, qs, q, inSize, outSize, out);
    if (a && a->mSelector != kPAVPropAudio && TraceOn()) {
        char s[5], sc[5]; FourCC(a->mSelector, s); FourCC(a->mScope, sc);
        os_log(gLog, "PAV get obj=%u %{public}s/%{public}s in=%u -> err=%d out=%u", o, s, sc, inSize, (int)r, outSize ? *outSize : 0);
    }
    return r;
}
