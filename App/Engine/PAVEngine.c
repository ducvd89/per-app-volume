//
//  PAVEngine.c — plays the mix of the Per-App Volume driver on the real output device.
//
//  Fast path: the app creates a shared-memory ring (IOSurface, see PAVShared.h) and hands its ID
//  to the driver; the driver writes the mix into it from its IO thread and our output IOProc reads
//  it directly. No polling, no IPC per audio chunk, minimal latency.
//
//  Fallback (if the driver can't attach the ring): a poll thread pulls chunks through the 'apad'
//  property, as before.
//
//  Only an OUTPUT IOProc is used, so macOS never shows the recording indicator and no TCC
//  permission is needed.
//

#include "PAVEngine.h"
#include "PAVShared.h"
#include <IOSurface/IOSurface.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>
#include <os/log.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#define kPropAudio   'apad'
#define kPropSurface 'apsf'

// ---- source ring (either the shared ring or the local fallback ring) ----
#define LOCAL_FRAMES 32768u
static float              gLocal[LOCAL_FRAMES * 2];
static _Atomic uint64_t   gLocalW = 0;

static const float*       gSrc  = NULL;
static uint32_t           gMask = 0;
static _Atomic uint64_t*  gSrcW = NULL;
static PAVSharedRing*     gShm  = NULL;     // non-NULL in direct mode
static uint32_t           gGen  = 0;        // IO thread: last seen generation

// ---- shared ring (created once per process) ----
static IOSurfaceRef       gSurface = NULL;
static PAVSharedRing*     gShmMem  = NULL;

// ---- engine state ----
static _Atomic bool       gRunning = false;
static pthread_t          gPollThread;
static bool               gPollStarted = false;
static AudioObjectID      gVirt = kAudioObjectUnknown;
static AudioObjectID      gOut  = kAudioObjectUnknown;
static AudioDeviceIOProcID gProcID = NULL;
static dispatch_source_t  gStatsTimer = NULL;
static os_log_t           gLog;

// IO-thread-owned
static uint64_t gR = 0;
static bool     gPrimed = false;
static uint32_t gTarget = 2048, gTargetMin = 512, gTargetMax = 8192;
static uint32_t gMinSlack = UINT32_MAX;     // smallest spare frames seen since the last adapt step
static uint64_t gCyclesClean = 0;           // IO cycles since the last underrun / adapt step
static uint32_t gCyclesPerStep = 2000;
static double   gRate = 48000.0;
static uint64_t gLastW = 0;                // last writer position seen
static uint32_t gChunk = 0;                // largest chunk the driver has written at once
static uint32_t gBufFrames = 512;

static _Atomic uint32_t gUnderruns = 0;
static _Atomic uint64_t gReadPos = 0;       // mirror of gR for stats
static _Atomic uint32_t gTargetPub = 0;

typedef struct { int buffer; int offset; } ChanPos;
static ChanPos gLeft = {0, 0}, gRight = {0, 1};
static bool    gMono = false;

#pragma mark - Shared ring

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // kIOSurfaceIsGlobal: needed to share by ID
static bool EnsureSurface(void) {
    if (gSurface) return true;
    int bytes = (int)PAV_SHM_BYTES;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &bytes);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, kIOSurfaceAllocSize, n);
    CFDictionarySetValue(d, kIOSurfaceIsGlobal, kCFBooleanTrue);
    gSurface = IOSurfaceCreate(d);
    CFRelease(d); CFRelease(n);
    if (!gSurface) return false;
    IOSurfaceLock(gSurface, 0, NULL);   // CPU-only memory; stays locked (mapped) for the process lifetime
    gShmMem = (PAVSharedRing*)IOSurfaceGetBaseAddress(gSurface);
    memset(gShmMem, 0, PAV_SHM_BYTES);
    gShmMem->magic = PAV_SHM_MAGIC;
    gShmMem->version = PAV_SHM_VERSION;
    gShmMem->capacityFrames = PAV_SHM_FRAMES;
    gShmMem->channels = PAV_SHM_CHANNELS;
    return true;
}
#pragma clang diagnostic pop

/// Hand the ring to the driver; true if it attached.
static bool AttachToDriver(AudioObjectID virt) {
    if (!EnsureSurface()) return false;
    atomic_store(&gShmMem->attached, 0);
    SInt64 id = IOSurfaceGetID(gSurface);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &id);
    AudioObjectPropertyAddress a = { kPropSurface, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    OSStatus st = AudioObjectSetPropertyData(virt, &a, 0, NULL, sizeof(CFPropertyListRef), &n);
    CFRelease(n);
    if (st != noErr) { os_log_error(gLog, "shared ring: driver refused (%d), falling back to polling", (int)st); return false; }
    return atomic_load(&gShmMem->attached) != 0;
}

#pragma mark - Fallback poll thread

static void MakeRealtime(double periodMs) {
    mach_timebase_info_data_t t; mach_timebase_info(&t);
    double perMs = 1e6 * t.denom / t.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)(periodMs * perMs), .computation = (uint32_t)(0.3 * perMs),
        .constraint = (uint32_t)(periodMs * 0.75 * perMs), .preemptible = 1 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static void* PollMain(void* arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    MakeRealtime(3.0);
    AudioObjectPropertyAddress a = { kPropAudio, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    while (atomic_load(&gRunning)) {
        CFPropertyListRef plist = NULL;
        UInt32 size = sizeof(plist);
        if (AudioObjectGetPropertyData(gVirt, &a, 0, NULL, &size, &plist) == noErr && plist) {
            if (CFGetTypeID(plist) == CFDataGetTypeID()) {
                uint32_t frames = (uint32_t)(CFDataGetLength((CFDataRef)plist) / (2 * sizeof(float)));
                const float* src = (const float*)CFDataGetBytePtr((CFDataRef)plist);
                uint64_t w = atomic_load_explicit(&gLocalW, memory_order_relaxed);
                if (frames > LOCAL_FRAMES / 2) frames = LOCAL_FRAMES / 2;
                for (uint32_t i = 0; i < frames; ++i) {
                    uint32_t idx = (uint32_t)((w + i) & (LOCAL_FRAMES - 1));
                    gLocal[idx * 2] = src[i * 2]; gLocal[idx * 2 + 1] = src[i * 2 + 1];
                }
                atomic_store_explicit(&gLocalW, w + frames, memory_order_release);
            }
            CFRelease(plist);
        }
        usleep(3000);
    }
    return NULL;
}

#pragma mark - IO

static inline float Clip(float x) { return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x); }

static inline void PutFrame(AudioBufferList* out, uint32_t f, float l, float r) {
    l = Clip(l); r = Clip(r);
    if (gMono) {
        AudioBuffer* b = &out->mBuffers[gLeft.buffer];
        ((float*)b->mData)[f * b->mNumberChannels + gLeft.offset] = 0.5f * (l + r);
        return;
    }
    AudioBuffer* bl = &out->mBuffers[gLeft.buffer];
    AudioBuffer* br = &out->mBuffers[gRight.buffer];
    ((float*)bl->mData)[f * bl->mNumberChannels + gLeft.offset]  = l;
    ((float*)br->mData)[f * br->mNumberChannels + gRight.offset] = r;
}

static inline void Grow(void) {
    uint32_t step = (uint32_t)(gRate * 0.005);
    gTarget = gTarget + step > gTargetMax ? gTargetMax : gTarget + step;
    gCyclesClean = 0; gMinSlack = UINT32_MAX;
}

static OSStatus IOProc(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* inData, const AudioTimeStamp* inTime,
                       AudioBufferList* outData, const AudioTimeStamp* outTime, void* ctx) {
    (void)dev; (void)now; (void)inData; (void)inTime; (void)outTime; (void)ctx;
    if (outData == NULL || outData->mNumberBuffers == 0) return noErr;
    for (UInt32 i = 0; i < outData->mNumberBuffers; ++i)
        if (outData->mBuffers[i].mData) memset(outData->mBuffers[i].mData, 0, outData->mBuffers[i].mDataByteSize);
    if (gLeft.buffer >= (int)outData->mNumberBuffers || gRight.buffer >= (int)outData->mNumberBuffers) return noErr;
    AudioBuffer* b0 = &outData->mBuffers[gLeft.buffer];
    if (b0->mNumberChannels == 0) return noErr;
    uint32_t frames = b0->mDataByteSize / (b0->mNumberChannels * sizeof(float));
    if (frames == 0 || gSrcW == NULL) return noErr;

    if (gShm) {
        uint32_t g = atomic_load_explicit(&gShm->generation, memory_order_acquire);
        if (g != gGen) { gGen = g; gPrimed = false; }
    }
    uint64_t w = atomic_load_explicit(gSrcW, memory_order_acquire);
    uint64_t r = gR;

    // Learn how big the driver's writes are (= the virtual device's IO buffer, set by the client app,
    // e.g. 512 for games, ~2048 for browsers). The buffer must hold at least one such chunk.
    if (w != gLastW) {
        if (gLastW != 0 && w > gLastW && w - gLastW < 16384) {
            uint32_t c = (uint32_t)(w - gLastW);
            if (c > gChunk) {
                gChunk = c;
                uint32_t floor = c + (uint32_t)(gRate * 0.003);
                if (floor > gTargetMax) floor = gTargetMax;
                if (gTargetMin < floor) gTargetMin = floor;
                if (gTarget < floor) gTarget = floor;
            }
        }
        gLastW = w;
    }

    if (!gPrimed) {
        if (gChunk == 0) return noErr;                       // wait until we've seen one write
        if (w < (uint64_t)gTarget + frames) return noErr;
        r = w - gTarget - frames;
        gPrimed = true;
    }
    if (r > w) { gPrimed = false; gR = w; return noErr; }   // writer restarted
    uint64_t avail = w - r;

    // Big backlog (stall / device change): jump forward.
    if (avail > (uint64_t)gTarget * 2 + frames + gChunk) { r = w - gTarget - frames; avail = gTarget + frames; }

    // Clock drift between the virtual device and the real one: consume ±1 frame.
    uint32_t tol = gTarget / 6 + 32;
    uint32_t consume = frames;
    if (avail > (uint64_t)gTarget + frames + tol) consume = frames + 1;
    else if (avail + tol < (uint64_t)gTarget + frames && frames > 1) consume = frames - 1;

    if (avail < (uint64_t)consume + 1) {
        uint32_t n = (uint32_t)avail; if (n > frames) n = frames;
        for (uint32_t f = 0; f < n; ++f) {
            uint32_t i = (uint32_t)((r + f) & gMask);
            PutFrame(outData, f, gSrc[i * 2], gSrc[i * 2 + 1]);
        }
        gR = r + n;
        gPrimed = false;
        atomic_fetch_add(&gUnderruns, 1);
        Grow();
        atomic_store_explicit(&gTargetPub, gTarget, memory_order_relaxed);
        atomic_store_explicit(&gReadPos, gR, memory_order_relaxed);
        return noErr;
    }

    if (consume == frames) {
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t i = (uint32_t)((r + f) & gMask);
            PutFrame(outData, f, gSrc[i * 2], gSrc[i * 2 + 1]);
        }
    } else {
        double step = (double)consume / (double)frames;
        for (uint32_t f = 0; f < frames; ++f) {
            double pos = f * step;
            uint32_t i0 = (uint32_t)pos;
            float t = (float)(pos - i0);
            uint32_t a = (uint32_t)((r + i0) & gMask);
            uint32_t b = (uint32_t)((r + i0 + 1) & gMask);
            PutFrame(outData, f, gSrc[a * 2] + (gSrc[b * 2] - gSrc[a * 2]) * t,
                                 gSrc[a * 2 + 1] + (gSrc[b * 2 + 1] - gSrc[a * 2 + 1]) * t);
        }
    }
    gR = r + consume;

    // Adapt latency: after a long clean stretch, shave 1 ms if we always had spare frames.
    uint32_t slack = (uint32_t)(avail - consume);
    if (slack < gMinSlack) gMinSlack = slack;
    if (++gCyclesClean >= gCyclesPerStep) {
        uint32_t ms1 = (uint32_t)(gRate * 0.001);
        if (gMinSlack > ms1 * 3 && gTarget > gTargetMin + ms1) gTarget -= ms1;
        gCyclesClean = 0; gMinSlack = UINT32_MAX;
    }
    atomic_store_explicit(&gTargetPub, gTarget, memory_order_relaxed);
    atomic_store_explicit(&gReadPos, gR, memory_order_relaxed);
    return noErr;
}

#pragma mark - Setup

static void ComputeChannelMap(AudioObjectID dev) {
    gLeft = (ChanPos){0, 0}; gRight = (ChanPos){0, 1}; gMono = false;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) return;
    AudioBufferList* abl = (AudioBufferList*)malloc(size);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, abl) != noErr) { free(abl); return; }
    UInt32 stereo[2] = {1, 2};
    AudioObjectPropertyAddress sa = { kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 ssz = sizeof(stereo);
    if (AudioObjectGetPropertyData(dev, &sa, 0, NULL, &ssz, stereo) != noErr) { stereo[0] = 1; stereo[1] = 2; }
    UInt32 total = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) total += abl->mBuffers[i].mNumberChannels;
    if (total == 0) { free(abl); return; }
    if (total == 1) gMono = true;
    for (int which = 0; which < 2; ++which) {
        UInt32 ch = stereo[which];
        if (ch < 1 || ch > total) ch = (UInt32)which + 1;
        if (ch > total) ch = total;
        UInt32 base = 0;
        for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) {
            UInt32 n = abl->mBuffers[i].mNumberChannels;
            if (ch - 1 < base + n) {
                ChanPos p = { (int)i, (int)(ch - 1 - base) };
                if (which == 0) gLeft = p; else gRight = p;
                break;
            }
            base += n;
        }
    }
    free(abl);
}

static void StartStats(void) {
    gStatsTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(gStatsTimer, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
    __block uint32_t last = 0;
    dispatch_source_set_event_handler(gStatsTimer, ^{
        uint32_t u = atomic_load(&gUnderruns);
        os_log(gLog, "engine %{public}s: underruns +%u, latency target %.1f ms, buffered %.1f ms",
               gShm ? "shared-memory" : "polling", u - last,
               (atomic_load(&gTargetPub) + gBufFrames) * 1000.0 / gRate, PAVEngineBufferedMs());
        last = u;
    });
    dispatch_resume(gStatsTimer);
}

OSStatus PAVEngineStart(AudioObjectID virtualDevice, AudioObjectID outputDevice) {
    PAVEngineStop();
    if (!gLog) gLog = os_log_create("com.perappvolume.app", "engine");
    gVirt = virtualDevice;
    gOut  = outputDevice;

    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(gRate);
    if (AudioObjectGetPropertyData(outputDevice, &ra, 0, NULL, &sz, &gRate) != noErr || gRate <= 0) gRate = 48000.0;

    UInt32 bufFrames = 512;
    AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    sz = sizeof(bufFrames);
    AudioObjectGetPropertyData(outputDevice, &ba, 0, NULL, &sz, &bufFrames);
    if (bufFrames < 64) bufFrames = 64;
    if (bufFrames > 4096) bufFrames = 4096;
    gBufFrames = bufFrames;
    gCyclesPerStep = (uint32_t)(20.0 * gRate / bufFrames);   // ~20 s between shrink steps

    ComputeChannelMap(outputDevice);
    gPrimed = false; gR = 0; gMinSlack = UINT32_MAX; gCyclesClean = 0; gLastW = 0; gChunk = 0;
    atomic_store(&gUnderruns, 0);

    if (AttachToDriver(virtualDevice)) {
        // Direct: data arrives in virtual-device IO chunks (usually 512 frames); keep one chunk + a few ms.
        gShm = gShmMem; gSrc = gShmMem->data; gMask = PAV_SHM_MASK; gSrcW = &gShmMem->writeEnd;
        gGen = atomic_load(&gShmMem->generation);
        gTargetMin = (uint32_t)(gRate * 0.003);          // raised to chunk + 3 ms once the chunk size is known
        gTarget    = (uint32_t)(gRate * 0.006);
        gTargetMax = 4096 + (uint32_t)(gRate * 0.100);
    } else {
        gShm = NULL; gSrc = gLocal; gMask = LOCAL_FRAMES - 1; gSrcW = &gLocalW;
        atomic_store(&gLocalW, 0);
        gTargetMin = 512 + (uint32_t)(gRate * 0.008);
        gTarget    = 512 + (uint32_t)(gRate * 0.025);
        gTargetMax = 512 + (uint32_t)(gRate * 0.150);
        AudioObjectPropertyAddress a = { kPropAudio, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFPropertyListRef p = NULL; UInt32 s = sizeof(p);
        if (AudioObjectGetPropertyData(gVirt, &a, 0, NULL, &s, &p) == noErr && p) CFRelease(p);   // drain stale data
    }
    atomic_store(&gTargetPub, gTarget);

    OSStatus st = AudioDeviceCreateIOProcID(outputDevice, IOProc, NULL, &gProcID);
    if (st != noErr) { gProcID = NULL; return st; }

    atomic_store(&gRunning, true);
    if (!gShm) {
        if (pthread_create(&gPollThread, NULL, PollMain, NULL) != 0) {
            atomic_store(&gRunning, false);
            AudioDeviceDestroyIOProcID(outputDevice, gProcID); gProcID = NULL;
            return kAudioHardwareUnspecifiedError;
        }
        gPollStarted = true;
    }
    st = AudioDeviceStart(outputDevice, gProcID);
    if (st != noErr) { PAVEngineStop(); return st; }
    os_log(gLog, "engine started (%{public}s), output buffer %u frames", gShm ? "shared-memory" : "polling", bufFrames);
    StartStats();
    return noErr;
}

void PAVEngineStop(void) {
    if (gStatsTimer) { dispatch_source_cancel(gStatsTimer); gStatsTimer = NULL; }
    if (gProcID) {
        AudioDeviceStop(gOut, gProcID);
        AudioDeviceDestroyIOProcID(gOut, gProcID);
        gProcID = NULL;
    }
    atomic_store(&gRunning, false);
    if (gPollStarted) { pthread_join(gPollThread, NULL); gPollStarted = false; }
}

bool PAVEngineIsRunning(void) { return gProcID != NULL && atomic_load(&gRunning); }
bool PAVEngineIsShared(void) { return gShm != NULL; }
uint32_t PAVEngineUnderruns(void) { return atomic_load(&gUnderruns); }
double PAVEngineBufferedMs(void) {
    if (!gSrcW) return 0;
    uint64_t w = atomic_load(gSrcW), r = atomic_load(&gReadPos);
    return (w > r && gRate > 0) ? (double)(w - r) * 1000.0 / gRate : 0;
}
