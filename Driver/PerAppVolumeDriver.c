//
//  PerAppVolumeDriver.c
//  Per-App Volume — virtual output device (AudioServerPlugIn / HAL driver)
//
//  How it works (no audio capture / no screen recording => no privacy dot):
//   * Apps play into this virtual output device ("Per-App Volume").
//   * coreaudiod hands each client's audio to us BEFORE it is mixed
//     (kAudioServerPlugInIOOperationProcessOutput). We apply the per-app gain there.
//   * The final mix (WriteMix) goes into a ring buffer.
//   * The companion app pulls the mix through a custom device *property* ('apad')
//     and plays it on the real output device. No input stream is ever opened,
//     so macOS never shows the microphone / recording indicator.
//
//  Custom properties on the device object (global scope, element main):
//   'apcl'  get  CFArray<CFDictionary{cid,pid,bid,run,peak}>   connected clients
//   'apvl'  get/set CFDictionary{ "<pid>" : gain(0..4) }       per-process gain
//   'apad'  get  CFData (interleaved stereo Float32)           new mixed frames
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
#include <os/log.h>
#include <IOSurface/IOSurface.h>
#include "PAVShared.h"

static os_log_t gLog;
static _Atomic int gTraceCount = 0;
#define kTraceLimit 0   // set >0 to trace HAL property calls to os_log

#pragma mark - Constants

#define kDevice_UID        "PerAppVolume_UID"
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

#define kVolumeMinDB (-96.0f)
#define kVolumeMaxDB (0.0f)

#define kPAVPropClients 'apcl'
#define kPAVPropVolumes 'apvl'
#define kPAVPropAudio   'apad'
#define kPAVPropSurface 'apsf'   // set: CFNumber IOSurfaceID of the app's shared ring; get: attached ID (0 = none)

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream = 3,
    kObjectID_Volume = 4,
    kObjectID_Mute   = 5,
};

static const Float64 kSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
#define kNumSampleRates (sizeof(kSampleRates) / sizeof(kSampleRates[0]))

#pragma mark - State

static pthread_mutex_t gStateMutex  = PTHREAD_MUTEX_INITIALIZER;   // properties / config
static pthread_mutex_t gClockMutex  = PTHREAD_MUTEX_INITIALIZER;   // zero timestamps
static pthread_mutex_t gClientMutex = PTHREAD_MUTEX_INITIALIZER;   // client table writes
static pthread_mutex_t gReadMutex   = PTHREAD_MUTEX_INITIALIZER;   // audio reader

static AudioServerPlugInHostRef gHost = NULL;
static UInt32  gRefCount = 0;

static Float64 gSampleRate        = 48000.0;
static Float64 gPendingSampleRate = 48000.0;

static UInt32  gIORunningCount    = 0;
static Float64 gHostTicksPerFrame = 0.0;
static UInt64  gNumberTimeStamps  = 0;
static UInt64  gAnchorHostTime    = 0;

static Float32        gVolumeScalar = 1.0f;     // under gStateMutex
static _Atomic float  gMasterGain   = 1.0f;     // linear, read on IO thread
static _Atomic bool   gMuted        = false;
static float          gMasterApplied = 1.0f;    // IO thread only

// Mixed audio ring (written on IO thread, read by property getter)
static float              gDataRing[kDataRingFrames * kChannels];
static _Atomic uint64_t   gWriteEnd = 0;

// Shared-memory ring provided by the app (see PAVShared.h). Swapped pointers are never freed while
// the IO thread might still be writing; old surfaces go to a small graveyard and are released later.
static PAVSharedRing* _Atomic gShm = NULL;
static IOSurfaceRef   gShmSurface = NULL;
static IOSurfaceID    gShmID = 0;
static IOSurfaceRef   gShmGraveyard[8];
static int            gShmGraveyardN = 0;
static pthread_mutex_t gShmMutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t   gStreamGen = 0;
static uint64_t           gReadPos = 0;       // under gReadMutex
static uint32_t           gReadGen = 0xFFFFFFFF;

typedef struct {
    _Atomic bool    used;
    UInt32          clientID;
    pid_t           pid;
    CFStringRef     bundleID;
    _Atomic float   gain;       // target gain
    float           applied;    // IO thread only (ramping)
    _Atomic float   peak;
    _Atomic bool    running;
} PAVClient;

static PAVClient gClients[kMaxClients];

typedef struct { pid_t pid; float gain; } PAVPidGain;
static PAVPidGain gPidGains[kMaxPidGains];    // under gClientMutex
static int        gPidGainCount = 0;

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

static float LookupPidGainLocked(pid_t pid) {
    for (int i = 0; i < gPidGainCount; ++i) if (gPidGains[i].pid == pid) return gPidGains[i].gain;
    return 1.0f;
}

static PAVClient* FindClient(UInt32 clientID) {
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (atomic_load_explicit(&c->used, memory_order_acquire) && c->clientID == clientID) return c;
    }
    return NULL;
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
            float g = LookupPidGainLocked(ci->mProcessID);
            atomic_store(&c->gain, g);
            c->applied = g;
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
        pthread_mutex_unlock(&gClockMutex);
    }
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
        case kPAVPropClients: case kPAVPropVolumes: case kPAVPropAudio: case kPAVPropSurface:
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
        *out = (a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kPAVPropVolumes || a->mSelector == kPAVPropSurface);
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
        case kAudioObjectPropertyCustomPropertyInfoList: *out = 4 * sizeof(AudioServerPlugInCustomPropertyInfo); return 0;
        case kPAVPropClients: case kPAVPropVolumes: case kPAVPropAudio: case kPAVPropSurface: *out = sizeof(CFPropertyListRef); return 0;
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

static CFPropertyListRef CopyVolumeTable(void) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    pthread_mutex_lock(&gClientMutex);
    for (int i = 0; i < gPidGainCount; ++i) {
        CFStringRef k = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), (int)gPidGains[i].pid);
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &gPidGains[i].gain);
        CFDictionarySetValue(d, k, n);
        CFRelease(k); CFRelease(n);
    }
    pthread_mutex_unlock(&gClientMutex);
    return d;
}

typedef struct { int count; } VolCtx;
static void VolApplier(const void* key, const void* value, void* ctx) {
    VolCtx* vc = (VolCtx*)ctx;
    if (vc->count >= kMaxPidGains) return;
    if (CFGetTypeID(key) != CFStringGetTypeID() || CFGetTypeID(value) != CFNumberGetTypeID()) return;
    SInt32 p = CFStringGetIntValue((CFStringRef)key);
    if (p <= 0) return;
    Float32 g = 1.0f;
    CFNumberGetValue((CFNumberRef)value, kCFNumberFloat32Type, &g);
    if (!(g >= 0.0f)) g = 0.0f;
    if (g > 4.0f) g = 4.0f;
    gPidGains[vc->count].pid = (pid_t)p;
    gPidGains[vc->count].gain = g;
    vc->count++;
}

static void SetVolumeTable(CFPropertyListRef plist) {
    if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return;
    pthread_mutex_lock(&gClientMutex);
    VolCtx ctx = { 0 };
    CFDictionaryApplyFunction((CFDictionaryRef)plist, VolApplier, &ctx);
    gPidGainCount = ctx.count;
    for (int i = 0; i < kMaxClients; ++i) {
        PAVClient* c = &gClients[i];
        if (atomic_load(&c->used)) atomic_store(&c->gain, LookupPidGainLocked(c->pid));
    }
    pthread_mutex_unlock(&gClientMutex);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // IOSurfaceLookup: still the way to share by ID
static OSStatus AttachSharedRing(CFPropertyListRef v) {
    SInt64 id = 0;
    if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &id))
        return kAudioHardwareIllegalOperationError;
    IOSurfaceRef surf = NULL;
    PAVSharedRing* ring = NULL;
    if (id != 0) {
        surf = IOSurfaceLookup((IOSurfaceID)id);
        if (surf == NULL) { os_log_error(gLog, "PAV shm lookup %lld failed", id); return kAudioHardwareIllegalOperationError; }
        if (IOSurfaceGetAllocSize(surf) < PAV_SHM_BYTES) { CFRelease(surf); return kAudioHardwareBadPropertySizeError; }
        IOSurfaceLock(surf, 0, NULL);   // CPU-only memory: base address stays valid; keep it locked while attached
        ring = (PAVSharedRing*)IOSurfaceGetBaseAddress(surf);
        if (ring->magic != PAV_SHM_MAGIC || ring->version != PAV_SHM_VERSION || ring->capacityFrames != PAV_SHM_FRAMES) {
            IOSurfaceUnlock(surf, 0, NULL); CFRelease(surf);
            return kAudioHardwareIllegalOperationError;
        }
        atomic_store(&ring->writeEnd, atomic_load(&gWriteEnd));
        atomic_fetch_add(&ring->generation, 1);
        atomic_store(&ring->attached, 1);
    }
    pthread_mutex_lock(&gShmMutex);
    IOSurfaceRef old = gShmSurface;
    gShmSurface = surf;
    gShmID = surf ? (IOSurfaceID)id : 0;
    atomic_store_explicit(&gShm, ring, memory_order_release);
    if (old) {
        // The IO thread may still hold the old pointer for one cycle; release it a few swaps later.
        if (gShmGraveyardN == 8) { IOSurfaceUnlock(gShmGraveyard[0], 0, NULL); CFRelease(gShmGraveyard[0]);
            memmove(gShmGraveyard, gShmGraveyard + 1, 7 * sizeof(IOSurfaceRef)); gShmGraveyardN = 7; }
        gShmGraveyard[gShmGraveyardN++] = old;
    }
    pthread_mutex_unlock(&gShmMutex);
    os_log(gLog, "PAV shm attached id=%lld", id);
    return 0;
}
#pragma clang diagnostic pop

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
        case kAudioDevicePropertyLatency: PUT(UInt32, 0);
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
            AudioServerPlugInCustomPropertyInfo info[4] = {
                { kPAVPropClients, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropVolumes, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropAudio,   kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kPAVPropSurface, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
            };
            UInt32 n = inSize / sizeof(AudioServerPlugInCustomPropertyInfo); if (n > 4) n = 4;
            memcpy(out, info, n * sizeof(AudioServerPlugInCustomPropertyInfo));
            *outSize = n * sizeof(AudioServerPlugInCustomPropertyInfo);
            return 0;
        }
        case kPAVPropClients: NEED(sizeof(CFPropertyListRef)); *(CFPropertyListRef*)out = CopyClientList(); *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropVolumes: NEED(sizeof(CFPropertyListRef)); *(CFPropertyListRef*)out = CopyVolumeTable(); *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropAudio:   NEED(sizeof(CFPropertyListRef)); *(CFPropertyListRef*)out = CopyAudioChunk();  *outSize = sizeof(CFPropertyListRef); return 0;
        case kPAVPropSurface: {
            NEED(sizeof(CFPropertyListRef));
            pthread_mutex_lock(&gShmMutex);
            SInt64 id = gShmID;
            pthread_mutex_unlock(&gShmMutex);
            *(CFPropertyListRef*)out = CFNumberCreate(NULL, kCFNumberSInt64Type, &id);
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
        if (a->mSelector == kPAVPropVolumes) {
            if (inSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
            SetVolumeTable(*(const CFPropertyListRef*)in);
            return 0;
        }
        if (a->mSelector == kPAVPropSurface) {
            if (inSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
            return AttachSharedRing(*(const CFPropertyListRef*)in);
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
        gAnchorHostTime = mach_absolute_time();
        atomic_store(&gWriteEnd, 0);
        atomic_fetch_add(&gStreamGen, 1);
        PAVSharedRing* ring = atomic_load_explicit(&gShm, memory_order_acquire);
        if (ring) { atomic_store_explicit(&ring->writeEnd, 0, memory_order_release); atomic_fetch_add(&ring->generation, 1); }
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

static OSStatus PAV_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 clientID,
                                     Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)clientID;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gClockMutex);
    UInt64 now = mach_absolute_time();
    Float64 ticksPerRing = gHostTicksPerFrame * (Float64)kZeroTimeStampPeriod;
    UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps + 1) * ticksPerRing);
    if (next <= now) ++gNumberTimeStamps;
    *outSampleTime = (Float64)(gNumberTimeStamps * kZeroTimeStampPeriod);
    *outHostTime   = gAnchorHostTime + (UInt64)((Float64)gNumberTimeStamps * ticksPerRing);
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
        }
    } else if (op == kAudioServerPlugInIOOperationWriteMix) {
        float target = atomic_load_explicit(&gMuted, memory_order_relaxed) ? 0.0f : atomic_load_explicit(&gMasterGain, memory_order_relaxed);
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
            if (end > pe || st + PAV_SHM_FRAMES < pe) atomic_store_explicit(&ring->writeEnd, end, memory_order_release);
        }
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
