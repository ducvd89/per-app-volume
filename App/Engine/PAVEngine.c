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
#include "PAVProtocol.h"
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

#define kPropAudio   kPAVPropAudio
#define kPropSurface kPAVPropSurface

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

// Rates. The ring holds audio at the virtual device's rate; normally that equals the real device's rate
// (the app keeps them matched). If it can't (a rate the driver doesn't offer), the IOProc resamples.
static double   gRate = 48000.0;            // real output device
static double   gSrcRate = 48000.0;         // virtual device = the ring
static double   gRatio = 1.0;               // source frames per output frame
static bool     gResample = false;

// IO-thread-owned. Buffer sizes below are in source (ring) frames.
static uint64_t gR = 0;
static double   gFrac = 0;                  // fractional read position (resampling only)
static bool     gPrimed = false;
static uint64_t gPlayedEnd = 0;             // never re-prime before this position: those frames were already played
static uint32_t gTarget = 2048, gTargetMin = 512, gTargetMax = 8192;
static uint32_t gTargetBase = 512;          // the mode's own minimum; gTargetMin = max(this, chunk + 3 ms)
static uint32_t gMinSlack = UINT32_MAX;     // smallest spare frames seen since the last adapt step
static uint64_t gCyclesClean = 0;           // IO cycles since the last underrun / adapt step
static uint32_t gCyclesPerStep = 2000;      // ~20 s: one cautious shrink step
static uint32_t gCyclesFast = 200;          // ~2 s: shrink steps while rebalancing
static bool     gRebalance = false;         // the chunk size went down: bring the fill down to what is needed now
static uint64_t gLastW = 0;                 // last writer position seen
static bool     gDriverChunk = false;       // the driver publishes its write size in the ring
static uint32_t gChunk = 0;                 // largest write of the last two windows
static uint32_t gChunkCur = 0, gChunkPrev = 0, gChunkCycles = 0, gChunkWindow = 1000;   // window: 10 s, or 2 min when inferred
// The ring ran dry. Whether that was an underrun or just the end of playback is only known once the
// writer moves again: same stream = it was late (count it, grow the buffer); new stream = a clean stop.
static bool     gDrained = false;
static uint64_t gDrainW = 0;

// Clock lock (shared-memory mode): instead of resampling, the virtual device's clock is steered to run exactly
// in step with the real device. Feed-forward: the real device's measured rate (HAL rate scalar). Feedback: a
// gentle correction from the average buffer fill, so the fill level stays where priming put it.
static double   gNominalTicksPerFrame = 0;  // of the virtual device
static double   gFillAvg = 0;               // smoothed frames available before consuming
static double   gFillSetpoint = 0;          // fill level to hold (learned right after priming)
static uint32_t gLockCycles = 0;            // cycles since priming
static double   gPendingRaise = 0;          // target increases before the setpoint was learned
static _Atomic int32_t gClockPPMx10 = 0;    // last correction applied, for stats
static uint32_t gBufFrames = 512;

static _Atomic uint32_t gUnderruns = 0;
static _Atomic uint32_t gAdjusts = 0;      // cycles that had to be resampled by ±1 frame (clock drift)
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
    atomic_store(&gShmMem->features, 0);        // an older driver leaves this zero
    atomic_store(&gShmMem->chunkFrames, 0);
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

static inline uint32_t Ms(double ms) { return (uint32_t)(gSrcRate * ms * 0.001); }

/// The clock lock holds the average fill at a setpoint. Changing the target means moving the setpoint with
/// it (or the lock would steer the difference away again); before the setpoint is learned, remember the move.
static inline void MoveSetpoint(double frames) {
    if (gLockCycles > 300) gFillSetpoint += frames; else gPendingRaise += frames;
}

/// How far the fill swings between one write arriving and the next: nothing when a write is no bigger than
/// what a cycle takes, else the difference. The steady quantity is the *average* fill, half a swing below
/// the peak, so the average that keeps a given low point depends on it.
static inline uint32_t Swing(uint32_t chunk, uint32_t need) { return chunk > need ? chunk - need : 0; }

/// `swingGrowth`: how much bigger the swing gets along with this raise (see Swing).
static inline void RaiseTarget(uint32_t to, uint32_t swingGrowth) {
    if (to <= gTarget) return;
    MoveSetpoint((double)(to - gTarget) - swingGrowth * 0.5);
    gTarget = to;
}

static inline void Grow(void) {
    uint32_t to = gTarget + Ms(5);
    RaiseTarget(to > gTargetMax ? gTargetMax : to, 0);
    gRebalance = false;
    gCyclesClean = 0; gMinSlack = UINT32_MAX;
}

/// The driver's write size changed. The buffer must hold at least one write (the virtual device's IO
/// buffer, set by the client apps: 512 for games, up to 4096 for music players) plus a few ms.
static inline void SetChunk(uint32_t chunk, uint32_t need) {
    uint32_t old = gChunk;
    gChunk = chunk;
    uint32_t floor = chunk + Ms(3);
    if (floor > gTargetMax) floor = gTargetMax;
    if (floor < gTargetBase) floor = gTargetBase;
    if (floor < gTargetMin) {
        // Smaller writes: less has to be buffered. How much less is measured, not calculated (the size may
        // have been an over-estimate, see the IOProc): the shrink steps take the fill down until the spare
        // frames are what the new size needs.
        gTargetMin = floor;
        gRebalance = true;
        gCyclesClean = 0; gMinSlack = UINT32_MAX;
    } else if (floor > gTargetMin) {
        gTargetMin = floor;
        gRebalance = false;
        uint32_t before = Swing(old, need), after = Swing(chunk, need);
        RaiseTarget(floor, after > before ? after - before : 0);
    }
}

static inline void Lerp(AudioBufferList* out, uint32_t f, uint64_t r, double pos) {
    uint32_t i0 = (uint32_t)pos;
    float t = (float)(pos - i0);
    uint32_t a = (uint32_t)((r + i0) & gMask);
    uint32_t b = (uint32_t)((r + i0 + 1) & gMask);
    PutFrame(out, f, gSrc[a * 2] + (gSrc[b * 2] - gSrc[a * 2]) * t,
                     gSrc[a * 2 + 1] + (gSrc[b * 2 + 1] - gSrc[a * 2 + 1]) * t);
}

static OSStatus IOProc(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* inData, const AudioTimeStamp* inTime,
                       AudioBufferList* outData, const AudioTimeStamp* outTime, void* ctx) {
    (void)dev; (void)inData; (void)inTime; (void)outTime; (void)ctx;
    if (outData == NULL || outData->mNumberBuffers == 0) return noErr;
    for (UInt32 i = 0; i < outData->mNumberBuffers; ++i)
        if (outData->mBuffers[i].mData) memset(outData->mBuffers[i].mData, 0, outData->mBuffers[i].mDataByteSize);

    // The channel map was computed when the engine started. If the device's format changed since (the app
    // restarts the engine a moment later), it may no longer fit these buffers: play nothing rather than
    // write outside them.
    if (gLeft.buffer >= (int)outData->mNumberBuffers || gRight.buffer >= (int)outData->mNumberBuffers) return noErr;
    AudioBuffer* bl = &outData->mBuffers[gLeft.buffer];
    AudioBuffer* br = &outData->mBuffers[gMono ? gLeft.buffer : gRight.buffer];
    if (bl->mData == NULL || br->mData == NULL || bl->mNumberChannels == 0 || br->mNumberChannels == 0) return noErr;
    if ((UInt32)gLeft.offset >= bl->mNumberChannels || (!gMono && (UInt32)gRight.offset >= br->mNumberChannels)) return noErr;
    uint32_t frames = bl->mDataByteSize / (bl->mNumberChannels * (UInt32)sizeof(float));
    uint32_t framesR = br->mDataByteSize / (br->mNumberChannels * (UInt32)sizeof(float));
    if (framesR < frames) frames = framesR;
    if (frames == 0 || gSrcW == NULL) return noErr;

    if (gShm) {
        uint32_t g = atomic_load_explicit(&gShm->generation, memory_order_acquire);
        if (g != gGen) { gGen = g; gPrimed = false; gPlayedEnd = 0; gDrained = false; }
    }
    uint64_t w = atomic_load_explicit(gSrcW, memory_order_acquire);
    uint64_t r = gR;

    // How big the driver's writes are: told by the driver, or (older driver, polling) inferred from how far
    // the writer moved since the last cycle. Inferring over-estimates when several writes land between two
    // cycles. Either way only the last two windows count, so one burst can't pin the latency for good.
    uint32_t seen = 0;
    if (w != gLastW) {
        if (gDriverChunk) seen = atomic_load_explicit(&gShm->chunkFrames, memory_order_relaxed);
        else if (gLastW != 0 && w > gLastW && w - gLastW < 16384) seen = (uint32_t)(w - gLastW);
        if (seen > 16384) seen = 16384;
        gLastW = w;
    }
    if (seen > gChunkCur) gChunkCur = seen;
    if (++gChunkCycles >= gChunkWindow) { gChunkPrev = gChunkCur; gChunkCur = 0; gChunkCycles = 0; }
    uint32_t chunk = gChunkCur > gChunkPrev ? gChunkCur : gChunkPrev;
    // Source frames one cycle takes (an upper bound when resampling).
    uint32_t need = gResample ? (uint32_t)((double)frames * gRatio) + 2 : frames;
    if (chunk != 0 && chunk != gChunk) SetChunk(chunk, need);

    if (!gPrimed) {
        if (gDrained && w != gDrainW) {                      // the same stream went on: that was an underrun
            gDrained = false;
            atomic_fetch_add_explicit(&gUnderruns, 1, memory_order_relaxed);
            Grow();
            atomic_store_explicit(&gTargetPub, gTarget, memory_order_relaxed);
        }
        if (gChunk == 0) return noErr;                       // wait until we've seen one write
        // After the ring ran dry, wait for a full target of *new* audio. Re-priming on the old data would replay
        // the last sound over and over once the writer stops (an app killed mid-playback) until the engine stops.
        if (w < gPlayedEnd + gTarget + need) return noErr;
        r = w - gTarget - need;
        gFrac = 0;
        gPrimed = true;
        gLockCycles = 0; gPendingRaise = 0;
    }
    if (r > w) { gPrimed = false; gR = w; gPlayedEnd = 0; gDrained = false; return noErr; }   // writer restarted
    uint64_t avail = w - r;

    // Big backlog (stall / device change): jump forward.
    if (avail > (uint64_t)gTarget * 2 + need + gChunk) { r = w - gTarget - need; avail = gTarget + need; gFrac = 0; }

    if (gShm) {
        // Feed-forward: the real device's actual speed, as host ticks per (virtual device) frame.
        double rs = (now && (now->mFlags & kAudioTimeStampRateScalarValid) && now->mRateScalar > 0.99 && now->mRateScalar < 1.01)
                    ? now->mRateScalar : 1.0;
        double tpf = gNominalTicksPerFrame * rs;
        // Feedback: hold the average fill level. Too much buffered → slow the virtual clock down, and vice versa.
        if (gLockCycles == 0) gFillAvg = (double)avail;
        gFillAvg += 0.01 * ((double)avail - gFillAvg);
        if (++gLockCycles == 300) gFillSetpoint = gFillAvg + gPendingRaise;   // ~3 s after priming
        double ppm = 0;
        if (gLockCycles > 300) {
            // 1 ppm per frame of error: drift-level errors are trimmed in ~20 s; big level changes (e.g. the target
            // grew) are absorbed at up to 800 ppm (~38 frames/s) — the apps simply run a hair faster or slower,
            // every sample stays intact.
            ppm = 1.0 * (gFillAvg - gFillSetpoint);
            if (ppm > 800) ppm = 800; else if (ppm < -800) ppm = -800;
        }
        tpf *= 1.0 + ppm * 1e-6;
        uint64_t bits; memcpy(&bits, &tpf, sizeof bits);
        atomic_store_explicit(&gShm->clockTicksPerFrame, bits, memory_order_relaxed);
        atomic_store_explicit(&gClockPPMx10, (int32_t)lround(ppm * 10), memory_order_relaxed);
    }

    // Clock drift between the virtual device and the real one: consume ±1 frame. With the clock lock active this
    // is only an emergency net (wide dead band); without it (polling / older driver) it's the regular mechanism.
    bool locked = gShm && atomic_load_explicit(&gShm->clockFollowed, memory_order_relaxed);
    uint32_t tol = locked ? gTarget + 512 : gTarget / 6 + 32;   // locked: emergency only
    int adj = 0;
    if (avail > (uint64_t)gTarget + need + tol) adj = 1;
    else if (avail + tol < (uint64_t)gTarget + need && frames > 1) adj = -1;

    // Same rate and nothing to correct: copy the samples untouched. Otherwise interpolate, `step` source
    // frames per output frame.
    bool straight = !gResample && adj == 0;
    double step = 1.0, end = 0;
    uint32_t consume = frames, required = frames + 1;
    if (gResample) {
        step = gRatio * (double)((int)frames + adj) / (double)frames;
        end = gFrac + (double)frames * step;
        consume = (uint32_t)end;
        required = consume + 2;
    } else if (adj != 0) {
        consume = (uint32_t)((int)frames + adj);
        step = (double)consume / (double)frames;
        required = consume + 1;
    }

    if (avail < required) {
        // Ran dry: play what's left, then wait for new audio (see gDrained).
        if (straight) {
            uint32_t n = (uint32_t)avail; if (n > frames) n = frames;
            for (uint32_t f = 0; f < n; ++f) {
                uint32_t i = (uint32_t)((r + f) & gMask);
                PutFrame(outData, f, gSrc[i * 2], gSrc[i * 2 + 1]);
            }
            gR = r + n;
        } else {
            for (uint32_t f = 0; f < frames; ++f) {
                double pos = gFrac + f * step;
                if ((uint64_t)pos + 1 >= avail) break;
                Lerp(outData, f, r, pos);
            }
            gR = r + avail;
        }
        gFrac = 0;
        gPrimed = false;
        gPlayedEnd = gR;
        gDrained = true; gDrainW = w;
        gCyclesClean = 0; gMinSlack = UINT32_MAX;
        atomic_store_explicit(&gTargetPub, gTarget, memory_order_relaxed);
        atomic_store_explicit(&gReadPos, gR, memory_order_relaxed);
        return noErr;
    }

    if (straight) {
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t i = (uint32_t)((r + f) & gMask);
            PutFrame(outData, f, gSrc[i * 2], gSrc[i * 2 + 1]);
        }
    } else {
        for (uint32_t f = 0; f < frames; ++f) Lerp(outData, f, r, gFrac + f * step);
        if (gResample) gFrac = end - consume;
    }
    gR = r + consume;
    if (adj != 0) atomic_fetch_add_explicit(&gAdjusts, 1, memory_order_relaxed);

    // Adapt latency, one step of 1 ms after each clean stretch in which there always were spare frames.
    uint32_t slack = (uint32_t)(avail - consume);
    if (slack < gMinSlack) gMinSlack = slack;
    uint32_t ms1 = Ms(1); if (ms1 == 0) ms1 = 1;
    if (++gCyclesClean >= (gRebalance ? gCyclesFast : gCyclesPerStep)) {
        if (gRebalance) {
            // After the chunk size went down (every ~2 s): lower the fill until the spare frames left after a
            // cycle are what the target's floor stands for, one write (or one cycle, if that is less) + 3 ms.
            // With the clock lock the fill follows the setpoint slowly, so count the part still to come.
            uint32_t safe = (gChunk < need ? gChunk : need) + Ms(3);
            double lag = (locked && gLockCycles > 300) ? gFillAvg - gFillSetpoint : 0;
            if (lag < 0) lag = 0;
            if ((double)gMinSlack - lag > (double)(safe + ms1)) {
                MoveSetpoint(-(double)ms1);
                gTarget = gTarget > gTargetMin + ms1 ? gTarget - ms1 : gTargetMin;
            } else if (gMinSlack <= safe + 2 * ms1) {
                gRebalance = false;                      // there: the fill is what the floor stands for
                gTarget = gTargetMin;
            }
        } else if (gMinSlack > ms1 * 3 && gTarget > gTargetMin + ms1) {
            // Latency an underrun added (every ~20 s).
            gTarget -= ms1; MoveSetpoint(-(double)ms1);
        }
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
    __block uint32_t last = 0, lastAdj = 0;
    dispatch_source_set_event_handler(gStatsTimer, ^{
        uint32_t u = atomic_load(&gUnderruns), adj = atomic_load(&gAdjusts);
        bool lk = gShm && atomic_load(&gShm->clockFollowed);
        os_log(gLog, "engine %{public}s%{public}s%{public}s: underruns +%u, resampled cycles +%u, clock trim %.1f ppm, latency target %.1f ms, buffered %.1f ms",
               gShm ? "shared-memory" : "polling", lk ? " +clock lock" : "", gResample ? " +rate conversion" : "", u - last, adj - lastAdj,
               atomic_load(&gClockPPMx10) / 10.0,
               atomic_load(&gTargetPub) * 1000.0 / gSrcRate + gBufFrames * 1000.0 / gRate, PAVEngineBufferedMs());
        last = u; lastAdj = adj;
    });
    dispatch_resume(gStatsTimer);
}

static double DeviceRate(AudioObjectID dev, double fallback) {
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    double r = 0; UInt32 sz = sizeof(r);
    return (AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &r) == noErr && r > 0) ? r : fallback;
}

static uint32_t Cycles(double seconds) {
    double c = seconds * gRate / gBufFrames;
    return c < 1 ? 1 : (uint32_t)c;
}

/// Rates, the timing that depends on them, and a clean IO state. (Also the entry point for the tests.)
static void Configure(double outRate, double srcRate, UInt32 bufFrames) {
    gRate = outRate;
    gSrcRate = srcRate;
    gResample = fabs(gSrcRate - gRate) > 0.5;
    gRatio = gResample ? gSrcRate / gRate : 1.0;
    if (bufFrames < 64) bufFrames = 64;
    if (bufFrames > 4096) bufFrames = 4096;
    gBufFrames = bufFrames;
    { mach_timebase_info_data_t tb; mach_timebase_info(&tb); gNominalTicksPerFrame = 1e9 * tb.denom / tb.numer / gSrcRate; }
    gCyclesPerStep = Cycles(20.0);   // between cautious shrink steps
    gCyclesFast    = Cycles(2.0);

    gLockCycles = 0; gFillSetpoint = 0; gFillAvg = 0; gPendingRaise = 0;
    gPrimed = false; gR = 0; gFrac = 0; gPlayedEnd = 0; gMinSlack = UINT32_MAX; gCyclesClean = 0; gLastW = 0;
    gChunk = 0; gChunkCur = 0; gChunkPrev = 0; gChunkCycles = 0; gRebalance = false; gDrained = false; gDrainW = 0;
    atomic_store(&gUnderruns, 0); atomic_store(&gAdjusts, 0); atomic_store(&gClockPPMx10, 0); atomic_store(&gReadPos, 0);
}

/// Direct: data arrives in virtual-device IO chunks (usually 512 frames); keep one chunk + a few ms.
static void UseSharedRing(PAVSharedRing* ring) {
    gShm = ring; gSrc = ring->data; gMask = PAV_SHM_MASK; gSrcW = &ring->writeEnd;
    gGen = atomic_load(&ring->generation);
    gDriverChunk = (atomic_load(&ring->features) & PAV_SHM_FEATURE_CHUNK) != 0;
    // A reported size is exact, so a short memory is enough. An inferred one reads double whenever two writes
    // land in one cycle, which happens now and then: remember it for minutes, or the target would keep
    // moving up and down between the two readings.
    gChunkWindow = Cycles(gDriverChunk ? 10.0 : 120.0);
    gTargetBase = Ms(3);                             // raised to chunk + 3 ms once the chunk size is known
    gTarget     = Ms(6);
    gTargetMax  = 4096 + Ms(100);
    gTargetMin  = gTargetBase;
    atomic_store(&gTargetPub, gTarget);
}

/// Fallback: a poll thread fills a local ring every few ms, so keep more.
static void UseLocalRing(void) {
    gShm = NULL; gSrc = gLocal; gMask = LOCAL_FRAMES - 1; gSrcW = &gLocalW;
    gDriverChunk = false;
    gChunkWindow = Cycles(120.0);
    atomic_store(&gLocalW, 0);
    gTargetBase = 512 + Ms(8);
    gTarget     = 512 + Ms(25);
    gTargetMax  = 512 + Ms(150);
    gTargetMin  = gTargetBase;
    atomic_store(&gTargetPub, gTarget);
}

OSStatus PAVEngineStart(AudioObjectID virtualDevice, AudioObjectID outputDevice) {
    PAVEngineStop();
    if (!gLog) gLog = os_log_create("com.perappvolume.app", "engine");
    gVirt = virtualDevice;
    gOut  = outputDevice;

    UInt32 bufFrames = 512;
    AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(bufFrames);
    AudioObjectGetPropertyData(outputDevice, &ba, 0, NULL, &sz, &bufFrames);
    double outRate = DeviceRate(outputDevice, 48000.0);
    Configure(outRate, DeviceRate(virtualDevice, outRate), bufFrames);
    bufFrames = gBufFrames;
    ComputeChannelMap(outputDevice);

    if (AttachToDriver(virtualDevice)) {
        UseSharedRing(gShmMem);
    } else {
        UseLocalRing();
        AudioObjectPropertyAddress a = { kPropAudio, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFPropertyListRef p = NULL; UInt32 s = sizeof(p);
        if (AudioObjectGetPropertyData(gVirt, &a, 0, NULL, &s, &p) == noErr && p) CFRelease(p);   // drain stale data
    }

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
    if (gResample)
        os_log(gLog, "engine started (%{public}s), output buffer %u frames, converting %.0f Hz to %.0f Hz",
               gShm ? "shared-memory" : "polling", bufFrames, gSrcRate, gRate);
    else
        os_log(gLog, "engine started (%{public}s), output buffer %u frames", gShm ? "shared-memory" : "polling", bufFrames);
    StartStats();
    return noErr;
}

void PAVEngineStop(void) {
    if (gStatsTimer) { dispatch_source_cancel(gStatsTimer); dispatch_release(gStatsTimer); gStatsTimer = NULL; }
    if (gShmMem) atomic_store(&gShmMem->clockTicksPerFrame, 0);   // driver falls back to its nominal clock
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
double PAVEngineSourceRate(void) { return gSrcRate; }
uint32_t PAVEngineUnderruns(void) { return atomic_load(&gUnderruns); }
double PAVEngineBufferedMs(void) {
    if (!gSrcW) return 0;
    uint64_t w = atomic_load(gSrcW), r = atomic_load(&gReadPos);
    return (w > r && gSrcRate > 0) ? (double)(w - r) * 1000.0 / gSrcRate : 0;
}
