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
//  Apps with an output device of their own are not in that mix: the driver mixes them into one ring per
//  output ("bus", see PAVProtocol.h), and a further engine plays each of those rings on its device.
//
//  Only OUTPUT IOProcs are used, so macOS never shows the recording indicator and no TCC
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

// ---- local ring of the polling fallback (main engine only) ----
#define LOCAL_FRAMES 32768u
static float              gLocal[LOCAL_FRAMES * 2];
static _Atomic uint64_t   gLocalW = 0;

static os_log_t           gLog;

typedef struct { int buffer; int offset; } ChanPos;

/// One ring played on one output device. gEngines[0] plays the main mix; the others play the buses
/// (apps with an output device of their own, see PAVProtocol.h), each on its own device.
typedef struct Engine {
    bool     inited;
    int      index;                         // 0 = main, 1...kPAVMaxBuses = that bus
    // The virtual device's clock can follow only one real device: the main one. A bus engine never steers
    // it and keeps in step with its device by consuming a frame more or less now and then.
    bool     steersClock;

    // ---- source ring (either a shared ring or the local fallback ring) ----
    const float*       src;
    uint32_t           mask;
    _Atomic uint64_t*  srcW;
    PAVSharedRing*     shm;                 // non-NULL in direct mode
    uint32_t           gen;                 // IO thread: last seen generation

    // ---- shared ring (created once per process and engine) ----
    IOSurfaceRef       surface;
    PAVSharedRing*     shmMem;

    // ---- engine state ----
    _Atomic bool       running;
    pthread_t          pollThread;
    bool               pollStarted;
    AudioObjectID      virt;
    AudioObjectID      out;
    AudioDeviceIOProcID procID;
    dispatch_source_t  statsTimer;

    // Rates. The ring holds audio at the virtual device's rate; normally that equals the real device's rate
    // (the app keeps them matched). If it can't (a rate the driver doesn't offer, or a bus device running
    // at another rate than the main one), the IOProc resamples.
    double   rate;                          // real output device
    double   srcRate;                       // virtual device = the ring
    double   ratio;                         // source frames per output frame
    bool     resample;

    // IO-thread-owned. Buffer sizes below are in source (ring) frames.
    uint64_t r;
    double   frac;                          // fractional read position (resampling only)
    bool     primed;
    uint64_t playedEnd;                     // never re-prime before this position: those frames were already played
    uint32_t target, targetMin, targetMax;
    uint32_t targetBase;                    // the mode's own minimum; targetMin = max(this, chunk + 3 ms)
    uint32_t minSlack;                      // smallest spare frames seen since the last adapt step
    uint64_t cyclesClean;                   // IO cycles since the last underrun / adapt step
    uint32_t cyclesPerStep;                 // ~20 s: one cautious shrink step
    uint32_t cyclesFast;                    // ~2 s: shrink steps while rebalancing
    bool     rebalance;                     // the chunk size went down: bring the fill down to what is needed now
    uint64_t lastW;                         // last writer position seen
    bool     driverChunk;                   // the driver publishes its write size in the ring
    uint32_t chunk;                         // largest write of the last two windows
    uint32_t chunkCur, chunkPrev, chunkCycles, chunkWindow;   // window: 10 s, or 2 min when inferred
    // The ring ran dry. Whether that was an underrun or just the end of playback is only known once the
    // writer moves again: same stream = it was late (count it, grow the buffer); new stream = a clean stop.
    bool     drained;
    uint64_t drainW;

    // Clock lock (shared-memory mode): instead of resampling, the virtual device's clock is steered to run exactly
    // in step with the real device. Feed-forward: the real device's measured rate (HAL rate scalar). Feedback: a
    // gentle correction from the average buffer fill, so the fill level stays where priming put it.
    double   nominalTicksPerFrame;          // of the virtual device
    double   fillAvg;                       // smoothed frames available before consuming
    double   fillSetpoint;                  // fill level to hold (learned right after priming)
    uint32_t lockCycles;                    // cycles since priming
    double   pendingRaise;                  // target increases before the setpoint was learned
    _Atomic int32_t clockPPMx10;            // last correction applied, for stats
    uint32_t bufFrames;

    _Atomic uint32_t underruns;
    _Atomic uint32_t adjusts;               // cycles that had to be resampled by ±1 frame (clock drift)
    _Atomic uint64_t readPos;               // mirror of `r` for stats
    _Atomic uint32_t targetPub;

    ChanPos  left, right;
    bool     mono;
} Engine;

static Engine gEngines[1 + kPAVMaxBuses];

static Engine* EngineAt(int index) {
    if (index < 0 || index > kPAVMaxBuses) return NULL;
    Engine* e = &gEngines[index];
    if (!e->inited) {
        e->inited = true;
        e->index = index;
        e->steersClock = index == 0;
        e->rate = 48000.0; e->srcRate = 48000.0; e->ratio = 1.0;
        e->target = 2048; e->targetMin = 512; e->targetMax = 8192; e->targetBase = 512;
        e->minSlack = UINT32_MAX;
        e->cyclesPerStep = 2000; e->cyclesFast = 200; e->chunkWindow = 1000;
        e->bufFrames = 512;
        e->left = (ChanPos){0, 0}; e->right = (ChanPos){0, 1};
        e->virt = kAudioObjectUnknown; e->out = kAudioObjectUnknown;
    }
    return e;
}

#pragma mark - Shared ring

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // kIOSurfaceIsGlobal: needed to share by ID
static bool EnsureSurface(Engine* e) {
    if (e->surface) return true;
    int bytes = (int)PAV_SHM_BYTES;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &bytes);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, kIOSurfaceAllocSize, n);
    CFDictionarySetValue(d, kIOSurfaceIsGlobal, kCFBooleanTrue);
    e->surface = IOSurfaceCreate(d);
    CFRelease(d); CFRelease(n);
    if (!e->surface) return false;
    IOSurfaceLock(e->surface, 0, NULL);   // CPU-only memory; stays locked (mapped) for the process lifetime
    e->shmMem = (PAVSharedRing*)IOSurfaceGetBaseAddress(e->surface);
    memset(e->shmMem, 0, PAV_SHM_BYTES);
    e->shmMem->magic = PAV_SHM_MAGIC;
    e->shmMem->version = PAV_SHM_VERSION;
    e->shmMem->capacityFrames = PAV_SHM_FRAMES;
    e->shmMem->channels = PAV_SHM_CHANNELS;
    return true;
}
#pragma clang diagnostic pop

/// Hand the ring to the driver; true if it attached.
static bool AttachToDriver(Engine* e, AudioObjectID virt) {
    if (!EnsureSurface(e)) return false;
    atomic_store(&e->shmMem->attached, 0);
    atomic_store(&e->shmMem->features, 0);        // an older driver leaves this zero
    atomic_store(&e->shmMem->chunkFrames, 0);
    SInt64 id = IOSurfaceGetID(e->surface);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &id);
    AudioObjectPropertyAddress a = { kPropSurface, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    OSStatus st = AudioObjectSetPropertyData(virt, &a, 0, NULL, sizeof(CFPropertyListRef), &n);
    CFRelease(n);
    if (st != noErr) { os_log_error(gLog, "shared ring: driver refused (%d), falling back to polling", (int)st); return false; }
    return atomic_load(&e->shmMem->attached) != 0;
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
    Engine* e = (Engine*)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    MakeRealtime(3.0);
    AudioObjectPropertyAddress a = { kPropAudio, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    while (atomic_load(&e->running)) {
        CFPropertyListRef plist = NULL;
        UInt32 size = sizeof(plist);
        if (AudioObjectGetPropertyData(e->virt, &a, 0, NULL, &size, &plist) == noErr && plist) {
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

static inline void PutFrame(const Engine* e, AudioBufferList* out, uint32_t f, float l, float r) {
    l = Clip(l); r = Clip(r);
    if (e->mono) {
        AudioBuffer* b = &out->mBuffers[e->left.buffer];
        ((float*)b->mData)[f * b->mNumberChannels + e->left.offset] = 0.5f * (l + r);
        return;
    }
    AudioBuffer* bl = &out->mBuffers[e->left.buffer];
    AudioBuffer* br = &out->mBuffers[e->right.buffer];
    ((float*)bl->mData)[f * bl->mNumberChannels + e->left.offset]  = l;
    ((float*)br->mData)[f * br->mNumberChannels + e->right.offset] = r;
}

static inline uint32_t Ms(const Engine* e, double ms) { return (uint32_t)(e->srcRate * ms * 0.001); }

/// The clock lock holds the average fill at a setpoint. Changing the target means moving the setpoint with
/// it (or the lock would steer the difference away again); before the setpoint is learned, remember the move.
static inline void MoveSetpoint(Engine* e, double frames) {
    if (e->lockCycles > 300) e->fillSetpoint += frames; else e->pendingRaise += frames;
}

/// How far the fill swings between one write arriving and the next: nothing when a write is no bigger than
/// what a cycle takes, else the difference. The steady quantity is the *average* fill, half a swing below
/// the peak, so the average that keeps a given low point depends on it.
static inline uint32_t Swing(uint32_t chunk, uint32_t need) { return chunk > need ? chunk - need : 0; }

/// `swingGrowth`: how much bigger the swing gets along with this raise (see Swing).
static inline void RaiseTarget(Engine* e, uint32_t to, uint32_t swingGrowth) {
    if (to <= e->target) return;
    MoveSetpoint(e, (double)(to - e->target) - swingGrowth * 0.5);
    e->target = to;
}

static inline void Grow(Engine* e) {
    uint32_t to = e->target + Ms(e, 5);
    RaiseTarget(e, to > e->targetMax ? e->targetMax : to, 0);
    e->rebalance = false;
    e->cyclesClean = 0; e->minSlack = UINT32_MAX;
}

/// The driver's write size changed. The buffer must hold at least one write (the virtual device's IO
/// buffer, set by the client apps: 512 for games, up to 4096 for music players) plus a few ms.
static inline void SetChunk(Engine* e, uint32_t chunk, uint32_t need) {
    uint32_t old = e->chunk;
    e->chunk = chunk;
    uint32_t floor = chunk + Ms(e, 3);
    if (floor > e->targetMax) floor = e->targetMax;
    if (floor < e->targetBase) floor = e->targetBase;
    if (floor < e->targetMin) {
        // Smaller writes: less has to be buffered. How much less is measured, not calculated (the size may
        // have been an over-estimate, see the IOProc): the shrink steps take the fill down until the spare
        // frames are what the new size needs.
        e->targetMin = floor;
        e->rebalance = true;
        e->cyclesClean = 0; e->minSlack = UINT32_MAX;
    } else if (floor > e->targetMin) {
        e->targetMin = floor;
        e->rebalance = false;
        uint32_t before = Swing(old, need), after = Swing(chunk, need);
        RaiseTarget(e, floor, after > before ? after - before : 0);
    }
}

static inline void Lerp(const Engine* e, AudioBufferList* out, uint32_t f, uint64_t r, double pos) {
    uint32_t i0 = (uint32_t)pos;
    float t = (float)(pos - i0);
    uint32_t a = (uint32_t)((r + i0) & e->mask);
    uint32_t b = (uint32_t)((r + i0 + 1) & e->mask);
    PutFrame(e, out, f, e->src[a * 2] + (e->src[b * 2] - e->src[a * 2]) * t,
                     e->src[a * 2 + 1] + (e->src[b * 2 + 1] - e->src[a * 2 + 1]) * t);
}

static OSStatus IOProc(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* inData, const AudioTimeStamp* inTime,
                       AudioBufferList* outData, const AudioTimeStamp* outTime, void* ctx) {
    (void)dev; (void)inData; (void)inTime; (void)outTime;
    Engine* e = (Engine*)ctx;
    if (outData == NULL || outData->mNumberBuffers == 0) return noErr;
    for (UInt32 i = 0; i < outData->mNumberBuffers; ++i)
        if (outData->mBuffers[i].mData) memset(outData->mBuffers[i].mData, 0, outData->mBuffers[i].mDataByteSize);

    // The channel map was computed when the engine started. If the device's format changed since (the app
    // restarts the engine a moment later), it may no longer fit these buffers: play nothing rather than
    // write outside them.
    if (e->left.buffer >= (int)outData->mNumberBuffers || e->right.buffer >= (int)outData->mNumberBuffers) return noErr;
    AudioBuffer* bl = &outData->mBuffers[e->left.buffer];
    AudioBuffer* br = &outData->mBuffers[e->mono ? e->left.buffer : e->right.buffer];
    if (bl->mData == NULL || br->mData == NULL || bl->mNumberChannels == 0 || br->mNumberChannels == 0) return noErr;
    if ((UInt32)e->left.offset >= bl->mNumberChannels || (!e->mono && (UInt32)e->right.offset >= br->mNumberChannels)) return noErr;
    uint32_t frames = bl->mDataByteSize / (bl->mNumberChannels * (UInt32)sizeof(float));
    uint32_t framesR = br->mDataByteSize / (br->mNumberChannels * (UInt32)sizeof(float));
    if (framesR < frames) frames = framesR;
    if (frames == 0 || e->srcW == NULL) return noErr;

    if (e->shm) {
        uint32_t g = atomic_load_explicit(&e->shm->generation, memory_order_acquire);
        if (g != e->gen) { e->gen = g; e->primed = false; e->playedEnd = 0; e->drained = false; }
    }
    uint64_t w = atomic_load_explicit(e->srcW, memory_order_acquire);
    uint64_t r = e->r;

    // How big the driver's writes are: told by the driver, or (older driver, polling) inferred from how far
    // the writer moved since the last cycle. Inferring over-estimates when several writes land between two
    // cycles. Either way only the last two windows count, so one burst can't pin the latency for good.
    uint32_t seen = 0;
    if (w != e->lastW) {
        if (e->driverChunk) seen = atomic_load_explicit(&e->shm->chunkFrames, memory_order_relaxed);
        else if (e->lastW != 0 && w > e->lastW && w - e->lastW < 16384) seen = (uint32_t)(w - e->lastW);
        if (seen > 16384) seen = 16384;
        e->lastW = w;
    }
    if (seen > e->chunkCur) e->chunkCur = seen;
    if (++e->chunkCycles >= e->chunkWindow) { e->chunkPrev = e->chunkCur; e->chunkCur = 0; e->chunkCycles = 0; }
    uint32_t chunk = e->chunkCur > e->chunkPrev ? e->chunkCur : e->chunkPrev;
    // Source frames one cycle takes (an upper bound when resampling).
    uint32_t need = e->resample ? (uint32_t)((double)frames * e->ratio) + 2 : frames;
    if (chunk != 0 && chunk != e->chunk) SetChunk(e, chunk, need);

    if (!e->primed) {
        if (e->drained && w != e->drainW) {                      // the same stream went on: that was an underrun
            e->drained = false;
            atomic_fetch_add_explicit(&e->underruns, 1, memory_order_relaxed);
            Grow(e);
            atomic_store_explicit(&e->targetPub, e->target, memory_order_relaxed);
        }
        if (e->chunk == 0) return noErr;                       // wait until we've seen one write
        // After the ring ran dry, wait for a full target of *new* audio. Re-priming on the old data would replay
        // the last sound over and over once the writer stops (an app killed mid-playback) until the engine stops.
        if (w < e->playedEnd + e->target + need) return noErr;
        r = w - e->target - need;
        e->frac = 0;
        e->primed = true;
        e->lockCycles = 0; e->pendingRaise = 0;
    }
    if (r > w) { e->primed = false; e->r = w; e->playedEnd = 0; e->drained = false; return noErr; }   // writer restarted
    uint64_t avail = w - r;

    // Big backlog (stall / device change): jump forward.
    if (avail > (uint64_t)e->target * 2 + need + e->chunk) { r = w - e->target - need; avail = e->target + need; e->frac = 0; }

    if (e->shm && e->steersClock) {
        // Feed-forward: the real device's actual speed, as host ticks per (virtual device) frame.
        double rs = (now && (now->mFlags & kAudioTimeStampRateScalarValid) && now->mRateScalar > 0.99 && now->mRateScalar < 1.01)
                    ? now->mRateScalar : 1.0;
        double tpf = e->nominalTicksPerFrame * rs;
        // Feedback: hold the average fill level. Too much buffered → slow the virtual clock down, and vice versa.
        if (e->lockCycles == 0) e->fillAvg = (double)avail;
        e->fillAvg += 0.01 * ((double)avail - e->fillAvg);
        if (++e->lockCycles == 300) e->fillSetpoint = e->fillAvg + e->pendingRaise;   // ~3 s after priming
        double ppm = 0;
        if (e->lockCycles > 300) {
            // 1 ppm per frame of error: drift-level errors are trimmed in ~20 s; big level changes (e.g. the target
            // grew) are absorbed at up to 800 ppm (~38 frames/s) — the apps simply run a hair faster or slower,
            // every sample stays intact.
            ppm = 1.0 * (e->fillAvg - e->fillSetpoint);
            if (ppm > 800) ppm = 800; else if (ppm < -800) ppm = -800;
        }
        tpf *= 1.0 + ppm * 1e-6;
        uint64_t bits; memcpy(&bits, &tpf, sizeof bits);
        atomic_store_explicit(&e->shm->clockTicksPerFrame, bits, memory_order_relaxed);
        atomic_store_explicit(&e->clockPPMx10, (int32_t)lround(ppm * 10), memory_order_relaxed);
    }

    // Clock drift between the virtual device and the real one: consume ±1 frame. With the clock lock active this
    // is only an emergency net (wide dead band); without it (polling / older driver / a bus, whose device the
    // virtual clock doesn't follow) it's the regular mechanism.
    bool locked = e->shm && e->steersClock && atomic_load_explicit(&e->shm->clockFollowed, memory_order_relaxed);
    uint32_t tol = locked ? e->target + 512 : e->target / 6 + 32;   // locked: emergency only
    int adj = 0;
    if (avail > (uint64_t)e->target + need + tol) adj = 1;
    else if (avail + tol < (uint64_t)e->target + need && frames > 1) adj = -1;

    // Same rate and nothing to correct: copy the samples untouched. Otherwise interpolate, `step` source
    // frames per output frame.
    bool straight = !e->resample && adj == 0;
    double step = 1.0, end = 0;
    uint32_t consume = frames, required = frames + 1;
    if (e->resample) {
        step = e->ratio * (double)((int)frames + adj) / (double)frames;
        end = e->frac + (double)frames * step;
        consume = (uint32_t)end;
        required = consume + 2;
    } else if (adj != 0) {
        consume = (uint32_t)((int)frames + adj);
        step = (double)consume / (double)frames;
        required = consume + 1;
    }

    if (avail < required) {
        // Ran dry: play what's left, then wait for new audio (see e->drained).
        if (straight) {
            uint32_t n = (uint32_t)avail; if (n > frames) n = frames;
            for (uint32_t f = 0; f < n; ++f) {
                uint32_t i = (uint32_t)((r + f) & e->mask);
                PutFrame(e, outData, f, e->src[i * 2], e->src[i * 2 + 1]);
            }
            e->r = r + n;
        } else {
            for (uint32_t f = 0; f < frames; ++f) {
                double pos = e->frac + f * step;
                if ((uint64_t)pos + 1 >= avail) break;
                Lerp(e, outData, f, r, pos);
            }
            e->r = r + avail;
        }
        e->frac = 0;
        e->primed = false;
        e->playedEnd = e->r;
        e->drained = true; e->drainW = w;
        e->cyclesClean = 0; e->minSlack = UINT32_MAX;
        atomic_store_explicit(&e->targetPub, e->target, memory_order_relaxed);
        atomic_store_explicit(&e->readPos, e->r, memory_order_relaxed);
        return noErr;
    }

    if (straight) {
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t i = (uint32_t)((r + f) & e->mask);
            PutFrame(e, outData, f, e->src[i * 2], e->src[i * 2 + 1]);
        }
    } else {
        for (uint32_t f = 0; f < frames; ++f) Lerp(e, outData, f, r, e->frac + f * step);
        if (e->resample) e->frac = end - consume;
    }
    e->r = r + consume;
    if (adj != 0) atomic_fetch_add_explicit(&e->adjusts, 1, memory_order_relaxed);

    // Adapt latency, one step of 1 ms after each clean stretch in which there always were spare frames.
    uint32_t slack = (uint32_t)(avail - consume);
    if (slack < e->minSlack) e->minSlack = slack;
    uint32_t ms1 = Ms(e, 1); if (ms1 == 0) ms1 = 1;
    if (++e->cyclesClean >= (e->rebalance ? e->cyclesFast : e->cyclesPerStep)) {
        if (e->rebalance) {
            // After the chunk size went down (every ~2 s): lower the fill until the spare frames left after a
            // cycle are what the target's floor stands for, one write (or one cycle, if that is less) + 3 ms.
            // With the clock lock the fill follows the setpoint slowly, so count the part still to come.
            uint32_t safe = (e->chunk < need ? e->chunk : need) + Ms(e, 3);
            double lag = (locked && e->lockCycles > 300) ? e->fillAvg - e->fillSetpoint : 0;
            if (lag < 0) lag = 0;
            if ((double)e->minSlack - lag > (double)(safe + ms1)) {
                MoveSetpoint(e, -(double)ms1);
                e->target = e->target > e->targetMin + ms1 ? e->target - ms1 : e->targetMin;
            } else if (e->minSlack <= safe + 2 * ms1) {
                e->rebalance = false;                      // there: the fill is what the floor stands for
                e->target = e->targetMin;
            }
        } else if (e->minSlack > ms1 * 3 && e->target > e->targetMin + ms1) {
            // Latency an underrun added (every ~20 s).
            e->target -= ms1; MoveSetpoint(e, -(double)ms1);
        }
        e->cyclesClean = 0; e->minSlack = UINT32_MAX;
    }
    atomic_store_explicit(&e->targetPub, e->target, memory_order_relaxed);
    atomic_store_explicit(&e->readPos, e->r, memory_order_relaxed);
    return noErr;
}

#pragma mark - Setup

static void ComputeChannelMap(Engine* e, AudioObjectID dev) {
    e->left = (ChanPos){0, 0}; e->right = (ChanPos){0, 1}; e->mono = false;
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
    if (total == 1) e->mono = true;
    for (int which = 0; which < 2; ++which) {
        UInt32 ch = stereo[which];
        if (ch < 1 || ch > total) ch = (UInt32)which + 1;
        if (ch > total) ch = total;
        UInt32 base = 0;
        for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) {
            UInt32 n = abl->mBuffers[i].mNumberChannels;
            if (ch - 1 < base + n) {
                ChanPos p = { (int)i, (int)(ch - 1 - base) };
                if (which == 0) e->left = p; else e->right = p;
                break;
            }
            base += n;
        }
    }
    free(abl);
}

static double BufferedMs(const Engine* e) {
    if (!e->srcW) return 0;
    uint64_t w = atomic_load(e->srcW), r = atomic_load(&e->readPos);
    return (w > r && e->srcRate > 0) ? (double)(w - r) * 1000.0 / e->srcRate : 0;
}

static void StartStats(Engine* e) {
    e->statsTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(e->statsTimer, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
    __block uint32_t last = 0, lastAdj = 0;
    dispatch_source_set_event_handler(e->statsTimer, ^{
        uint32_t u = atomic_load(&e->underruns), adj = atomic_load(&e->adjusts);
        bool lk = e->shm && e->steersClock && atomic_load(&e->shm->clockFollowed);
        os_log(gLog, "engine %d %{public}s%{public}s%{public}s: underruns +%u, resampled cycles +%u, clock trim %.1f ppm, latency target %.1f ms, buffered %.1f ms",
               e->index, e->shm ? "shared-memory" : "polling", lk ? " +clock lock" : "", e->resample ? " +rate conversion" : "", u - last, adj - lastAdj,
               atomic_load(&e->clockPPMx10) / 10.0,
               atomic_load(&e->targetPub) * 1000.0 / e->srcRate + e->bufFrames * 1000.0 / e->rate, BufferedMs(e));
        last = u; lastAdj = adj;
    });
    dispatch_resume(e->statsTimer);
}

static double DeviceRate(AudioObjectID dev, double fallback) {
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    double r = 0; UInt32 sz = sizeof(r);
    return (AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &r) == noErr && r > 0) ? r : fallback;
}

static uint32_t Cycles(Engine* e, double seconds) {
    double c = seconds * e->rate / e->bufFrames;
    return c < 1 ? 1 : (uint32_t)c;
}

/// Rates, the timing that depends on them, and a clean IO state. (Also the entry point for the tests.)
static void Configure(Engine* e, double outRate, double srcRate, UInt32 bufFrames) {
    e->rate = outRate;
    e->srcRate = srcRate;
    e->resample = fabs(e->srcRate - e->rate) > 0.5;
    e->ratio = e->resample ? e->srcRate / e->rate : 1.0;
    if (bufFrames < 64) bufFrames = 64;
    if (bufFrames > 4096) bufFrames = 4096;
    e->bufFrames = bufFrames;
    { mach_timebase_info_data_t tb; mach_timebase_info(&tb); e->nominalTicksPerFrame = 1e9 * tb.denom / tb.numer / e->srcRate; }
    e->cyclesPerStep = Cycles(e, 20.0);   // between cautious shrink steps
    e->cyclesFast    = Cycles(e, 2.0);

    e->lockCycles = 0; e->fillSetpoint = 0; e->fillAvg = 0; e->pendingRaise = 0;
    e->primed = false; e->r = 0; e->frac = 0; e->playedEnd = 0; e->minSlack = UINT32_MAX; e->cyclesClean = 0; e->lastW = 0;
    e->chunk = 0; e->chunkCur = 0; e->chunkPrev = 0; e->chunkCycles = 0; e->rebalance = false; e->drained = false; e->drainW = 0;
    atomic_store(&e->underruns, 0); atomic_store(&e->adjusts, 0); atomic_store(&e->clockPPMx10, 0); atomic_store(&e->readPos, 0);
}

/// Direct: data arrives in virtual-device IO chunks (usually 512 frames); keep one chunk + a few ms.
static void UseSharedRing(Engine* e, PAVSharedRing* ring) {
    e->shm = ring; e->src = ring->data; e->mask = PAV_SHM_MASK; e->srcW = &ring->writeEnd;
    e->gen = atomic_load(&ring->generation);
    e->driverChunk = (atomic_load(&ring->features) & PAV_SHM_FEATURE_CHUNK) != 0;
    // A reported size is exact, so a short memory is enough. An inferred one reads double whenever two writes
    // land in one cycle, which happens now and then: remember it for minutes, or the target would keep
    // moving up and down between the two readings.
    e->chunkWindow = Cycles(e, e->driverChunk ? 10.0 : 120.0);
    e->targetBase = Ms(e, 3);                             // raised to chunk + 3 ms once the chunk size is known
    e->target     = Ms(e, 6);
    e->targetMax  = 4096 + Ms(e, 100);
    e->targetMin  = e->targetBase;
    atomic_store(&e->targetPub, e->target);
}

/// Fallback: a poll thread fills a local ring every few ms, so keep more.
static void UseLocalRing(Engine* e) {
    e->shm = NULL; e->src = gLocal; e->mask = LOCAL_FRAMES - 1; e->srcW = &gLocalW;
    e->driverChunk = false;
    e->chunkWindow = Cycles(e, 120.0);
    atomic_store(&gLocalW, 0);
    e->targetBase = 512 + Ms(e, 8);
    e->target     = 512 + Ms(e, 25);
    e->targetMax  = 512 + Ms(e, 150);
    e->targetMin  = e->targetBase;
    atomic_store(&e->targetPub, e->target);
}

static void Stop(Engine* e) {
    if (e->statsTimer) { dispatch_source_cancel(e->statsTimer); dispatch_release(e->statsTimer); e->statsTimer = NULL; }
    // The driver falls back to its nominal clock.
    if (e->steersClock && e->shmMem) atomic_store(&e->shmMem->clockTicksPerFrame, 0);
    if (e->procID) {
        AudioDeviceStop(e->out, e->procID);
        AudioDeviceDestroyIOProcID(e->out, e->procID);
        e->procID = NULL;
    }
    atomic_store(&e->running, false);
    if (e->pollStarted) { pthread_join(e->pollThread, NULL); e->pollStarted = false; }
}

/// Plays the engine's ring on `outputDevice`. The main engine hands its ring to the driver itself (and polls
/// if that fails); a bus engine's ring was given to the driver by the app together with the routes.
static OSStatus Start(Engine* e, AudioObjectID virtualDevice, AudioObjectID outputDevice) {
    Stop(e);
    if (!gLog) gLog = os_log_create("com.perappvolume.app", "engine");
    e->virt = virtualDevice;
    e->out  = outputDevice;

    UInt32 bufFrames = 512;
    AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(bufFrames);
    AudioObjectGetPropertyData(outputDevice, &ba, 0, NULL, &sz, &bufFrames);
    double outRate = DeviceRate(outputDevice, 48000.0);
    Configure(e, outRate, DeviceRate(virtualDevice, outRate), bufFrames);
    bufFrames = e->bufFrames;
    ComputeChannelMap(e, outputDevice);

    if (e->index != 0) {
        if (!e->shmMem || !atomic_load(&e->shmMem->attached)) return kAudioHardwareNotRunningError;
        UseSharedRing(e, e->shmMem);
    } else if (AttachToDriver(e, virtualDevice)) {
        UseSharedRing(e, e->shmMem);
    } else {
        UseLocalRing(e);
        AudioObjectPropertyAddress a = { kPropAudio, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFPropertyListRef p = NULL; UInt32 s = sizeof(p);
        if (AudioObjectGetPropertyData(e->virt, &a, 0, NULL, &s, &p) == noErr && p) CFRelease(p);   // drain stale data
    }

    OSStatus st = AudioDeviceCreateIOProcID(outputDevice, IOProc, e, &e->procID);
    if (st != noErr) { e->procID = NULL; return st; }

    atomic_store(&e->running, true);
    if (!e->shm) {
        if (pthread_create(&e->pollThread, NULL, PollMain, e) != 0) {
            atomic_store(&e->running, false);
            AudioDeviceDestroyIOProcID(outputDevice, e->procID); e->procID = NULL;
            return kAudioHardwareUnspecifiedError;
        }
        e->pollStarted = true;
    }
    st = AudioDeviceStart(outputDevice, e->procID);
    if (st != noErr) { Stop(e); return st; }
    if (e->resample)
        os_log(gLog, "engine %d started (%{public}s), output buffer %u frames, converting %.0f Hz to %.0f Hz",
               e->index, e->shm ? "shared-memory" : "polling", bufFrames, e->srcRate, e->rate);
    else
        os_log(gLog, "engine %d started (%{public}s), output buffer %u frames", e->index, e->shm ? "shared-memory" : "polling", bufFrames);
    StartStats(e);
    return noErr;
}

static bool IsRunning(const Engine* e) { return e->procID != NULL && atomic_load(&e->running); }

#pragma mark - Main output

OSStatus PAVEngineStart(AudioObjectID virtualDevice, AudioObjectID outputDevice) { return Start(EngineAt(0), virtualDevice, outputDevice); }
void PAVEngineStop(void) { Stop(EngineAt(0)); }
bool PAVEngineIsRunning(void) { return IsRunning(EngineAt(0)); }
bool PAVEngineIsShared(void) { return EngineAt(0)->shm != NULL; }
double PAVEngineSourceRate(void) { return EngineAt(0)->srcRate; }
uint32_t PAVEngineUnderruns(void) { return atomic_load(&EngineAt(0)->underruns); }
double PAVEngineBufferedMs(void) { return BufferedMs(EngineAt(0)); }

#pragma mark - Buses

static Engine* BusAt(int bus) { return bus >= 1 ? EngineAt(bus) : NULL; }

int64_t PAVBusSurfaceID(int bus) {
    Engine* e = BusAt(bus);
    return (e && EnsureSurface(e)) ? (int64_t)IOSurfaceGetID(e->surface) : 0;
}

OSStatus PAVBusStart(int bus, AudioObjectID virtualDevice, AudioObjectID outputDevice) {
    Engine* e = BusAt(bus);
    return e ? Start(e, virtualDevice, outputDevice) : kAudioHardwareBadObjectError;
}

void PAVBusStop(int bus) { Engine* e = BusAt(bus); if (e) Stop(e); }
bool PAVBusIsRunning(int bus) { Engine* e = BusAt(bus); return e && IsRunning(e); }
AudioObjectID PAVBusOutput(int bus) { Engine* e = BusAt(bus); return (e && IsRunning(e)) ? e->out : kAudioObjectUnknown; }
double PAVBusSourceRate(int bus) { Engine* e = BusAt(bus); return e ? e->srcRate : 0; }
