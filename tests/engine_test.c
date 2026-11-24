//
//  engine_test.c — drives the engine's IOProc against a simulated driver (no audio hardware involved).
//
//  The engine source is included directly so the test can set up its state and call the IOProc. The
//  simulated driver writes chunks into a ring at the virtual device's rate and, like the real one,
//  follows the clock rate the engine publishes.
//
#include "../App/Engine/PAVEngine.c"
#include <stdio.h>

static int gFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++gFailures; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Engine* e;                // the engine under test

// ---- simulated driver ----
static PAVSharedRing* gRing;
static double  gSimDebt;        // source frames the writer still owes (fractional)
static uint32_t gSimChunk;      // write size
static bool    gSimWriting;
static bool    gSimFollowClock;
static double  gSimSpeed;       // how fast the virtual device's clock runs next to the output device's
static double  gSimTone;        // Hz; 0 = write a ramp that identifies each frame
static uint64_t gSimIndex;      // frames written since the test began (never reset, so content stays unique)

static float Ramp(uint64_t i) { return (float)(i % 32768) / 65536.0f; }   // exact in float, never clipped

static void SimWrite(uint32_t n) {
    uint64_t w = atomic_load(&gRing->writeEnd);
    for (uint32_t i = 0; i < n; ++i, ++gSimIndex) {
        uint32_t at = (uint32_t)((w + i) & PAV_SHM_MASK);
        float v = gSimTone > 0 ? (float)(0.5 * sin(2.0 * M_PI * gSimTone * (double)gSimIndex / e->srcRate)) : Ramp(gSimIndex);
        gRing->data[at * 2] = v; gRing->data[at * 2 + 1] = v;
    }
    atomic_store(&gRing->chunkFrames, n);
    atomic_store(&gRing->writeEnd, w + n);
}

/// What the driver does when IO (re)starts: a new stream, sample time back to zero.
static void SimRestartStream(void) {
    atomic_store(&gRing->writeEnd, 0);
    atomic_fetch_add(&gRing->generation, 1);
    gSimDebt = 0;
}

static void Setup(double srcRate, double outRate, uint32_t buf, uint32_t chunk, bool driverChunk) {
    e = EngineAt(0);
    e->steersClock = true;
    if (!gRing) gRing = calloc(1, sizeof(*gRing));
    memset(gRing, 0, sizeof(*gRing));
    atomic_store(&gRing->features, driverChunk ? PAV_SHM_FEATURE_CHUNK : 0);
    atomic_store(&gRing->clockFollowed, 1);
    Configure(e, outRate, srcRate, buf);
    UseSharedRing(e, gRing);
    e->left = (ChanPos){0, 0}; e->right = (ChanPos){0, 1}; e->mono = false;
    gSimDebt = 0; gSimChunk = chunk; gSimWriting = true; gSimFollowClock = true; gSimSpeed = 1.0; gSimTone = 0; gSimIndex = 0;
}

static float gSimOut[4096 * 2 + 8];

/// One output cycle: the simulated driver writes what is due, then the IOProc runs. Returns the output.
static const float* Cycle(void) {
    if (gSimWriting) {
        double speed = gSimSpeed;
        uint64_t bits = atomic_load(&gRing->clockTicksPerFrame);
        if (gSimFollowClock && bits != 0) { double tpf; memcpy(&tpf, &bits, sizeof tpf); speed = e->nominalTicksPerFrame / tpf; }
        gSimDebt += (double)e->bufFrames / e->rate * e->srcRate * speed;
        while (gSimDebt >= gSimChunk) { SimWrite(gSimChunk); gSimDebt -= gSimChunk; }
    }
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 2;
    abl.mBuffers[0].mDataByteSize = e->bufFrames * 2 * (UInt32)sizeof(float);
    abl.mBuffers[0].mData = gSimOut;
    AudioTimeStamp now = { 0 };
    IOProc(0, &now, NULL, NULL, &abl, NULL, e);
    return gSimOut;
}

static void Run(double seconds) { for (uint32_t i = 0, n = (uint32_t)(seconds * e->rate / e->bufFrames); i < n; ++i) Cycle(); }
static double TargetMs(void) { return e->target * 1000.0 / e->srcRate; }
static double SimBufferedMs(void) { return (double)(atomic_load(&gRing->writeEnd) - e->r) * 1000.0 / e->srcRate; }
/// What is really buffered, averaged over the swing between writes (the target is only bookkeeping).
static double AvgBufferedMs(void) { double sum = 0; for (int i = 0; i < 400; ++i) { Cycle(); sum += SimBufferedMs(); } return sum / 400; }

// ---- tests ----

static void TestBitPerfect(void) {
    printf("same rate: samples pass through untouched\n");
    Setup(48000, 48000, 512, 512, true);
    Run(1.0);
    CHECK(e->primed, "not playing after 1 s");
    float last = -1; int breaks = 0; long checked = 0;
    for (int c = 0; c < 2000; ++c) {
        const float* o = Cycle();
        for (uint32_t f = 0; f < e->bufFrames; ++f, ++checked) {
            if (last >= 0) {
                float want = last + 1.0f / 65536.0f; if (want >= 0.5f) want = 0;
                if (o[f * 2] != want || o[f * 2 + 1] != want) ++breaks;
            }
            last = o[f * 2];
        }
    }
    CHECK(breaks == 0, "%d of %ld frames were not the next source frame", breaks, checked);
    CHECK(atomic_load(&e->underruns) == 0 && atomic_load(&e->adjusts) == 0, "underruns %u, resampled cycles %u",
          atomic_load(&e->underruns), atomic_load(&e->adjusts));
    CHECK(fabs(TargetMs() - (512 + 144) * 1000.0 / 48000) < 0.5, "latency target %.1f ms, expected one chunk + 3 ms", TargetMs());
}

static void TestCleanStop(void) {
    printf("end of playback is not an underrun\n");
    Setup(48000, 48000, 512, 512, true);
    Run(2.0);
    uint32_t before = e->target;
    for (int round = 0; round < 6; ++round) {
        gSimWriting = false; Run(1.0);                    // playback stops: the ring runs dry
        float peak = 0; const float* o = Cycle();
        for (uint32_t i = 0; i < e->bufFrames * 2; ++i) if (fabsf(o[i]) > peak) peak = fabsf(o[i]);
        CHECK(peak == 0, "still producing sound %.3f after the writer stopped", peak);
        SimRestartStream(); gSimWriting = true; Run(1.0); // playback starts again
        CHECK(e->primed, "round %d: not playing again", round);
    }
    CHECK(atomic_load(&e->underruns) == 0, "%u underruns counted for 6 clean stops", atomic_load(&e->underruns));
    CHECK(e->target == before, "latency target went from %u to %u frames", before, e->target);
}

static void TestRealUnderrun(void) {
    printf("a stalled writer is an underrun and grows the buffer\n");
    Setup(48000, 48000, 512, 512, true);
    Run(2.0);
    uint32_t before = e->target;
    gSimWriting = false; Run(0.2);                        // the driver's IO thread stalls...
    CHECK(atomic_load(&e->underruns) == 0, "counted before knowing whether the stream goes on");
    gSimWriting = true; Run(1.0);                         // ...and the same stream goes on
    CHECK(atomic_load(&e->underruns) == 1, "underruns = %u, expected 1", atomic_load(&e->underruns));
    CHECK(e->target == before + Ms(e, 5), "latency target %u, expected %u", e->target, before + Ms(e, 5));
    CHECK(e->primed, "not playing again");
}

static void TestChunkDecays(bool driverChunk) {
    printf("latency comes back down after big writes end (%s)\n", driverChunk ? "driver reports its write size" : "write size inferred");
    Setup(48000, 48000, 512, 4096, driverChunk);
    Run(30.0);
    CHECK(TargetMs() > 85 && TargetMs() < 95, "with 4096-frame writes the target is %.1f ms, expected ~88", TargetMs());
    gSimChunk = 512;                                      // the app with the big buffer quit
    double wait = driverChunk ? 120.0 : 480.0;            // an inferred size is remembered for minutes
    Run(wait);
    CHECK(TargetMs() < 26, "%.0f s after going back to 512-frame writes the target is still %.1f ms", wait, TargetMs());
    double buffered = AvgBufferedMs();
    CHECK(buffered > 10 && buffered < 22, "actually buffered: %.1f ms, expected ~14 (one 512-frame write + 3 ms)", buffered);
    CHECK(atomic_load(&e->underruns) == 0, "%u underruns while shrinking", atomic_load(&e->underruns));
    CHECK(e->primed, "stopped playing");
}

static void TestBurst(void) {
    printf("one burst of writes doesn't pin the latency (older driver)\n");
    Setup(48000, 48000, 512, 512, false);
    Run(5.0);
    double normal = TargetMs(), bufferedBefore = AvgBufferedMs();
    for (int i = 0; i < 10; ++i) SimWrite(512);           // ten writes land between two output cycles
    Cycle();
    CHECK(TargetMs() > normal + 50, "the burst didn't raise the target (%.1f ms)", TargetMs());
    Run(480.0);
    // Inferring can still see two writes in one cycle now and then (one extra 512-frame chunk, ~11 ms);
    // what must not happen is the burst's 100 ms staying for good.
    CHECK(TargetMs() < normal + 11, "8 minutes later the target is %.1f ms (was %.1f before the burst)", TargetMs(), normal);
    double bufferedAfter = AvgBufferedMs();
    CHECK(bufferedAfter < bufferedBefore + 12, "really buffered: %.1f ms before the burst, %.1f ms 8 minutes after", bufferedBefore, bufferedAfter);
    CHECK(atomic_load(&e->underruns) == 0, "%u underruns", atomic_load(&e->underruns));
    CHECK(e->primed, "stopped playing");
}

/// Plays a tone through a rate mismatch and measures the frequency that comes out.
static void TestRateConversion(double srcRate, double outRate) {
    printf("rate conversion %.0f -> %.0f Hz keeps pitch and speed\n", srcRate, outRate);
    Setup(srcRate, outRate, 512, 512, true);
    gSimFollowClock = false;                              // as when the driver rejects the published clock
    atomic_store(&gRing->clockFollowed, 0);
    gSimTone = 440.0;
    Run(2.0);
    CHECK(e->primed && e->resample, "not converting");
    uint64_t r0 = e->r; int crossings = 0; float prev = 0; long frames = 0; float peak = 0;
    for (uint32_t c = 0, n = (uint32_t)(4.0 * outRate / e->bufFrames); c < n; ++c) {
        const float* o = Cycle();
        for (uint32_t f = 0; f < e->bufFrames; ++f, ++frames) {
            float v = o[f * 2];
            if (prev <= 0 && v > 0) ++crossings;
            if (fabsf(v) > peak) peak = fabsf(v);
            prev = v;
        }
    }
    double seconds = frames / outRate;
    double hz = crossings / seconds;
    double speed = (double)(e->r - r0) / srcRate / seconds;
    CHECK(fabs(hz - 440.0) < 2.0, "a 440 Hz tone came out at %.1f Hz", hz);
    CHECK(fabs(speed - 1.0) < 0.003, "the source was consumed at %.4fx real time", speed);
    CHECK(peak > 0.4f && peak <= 0.5f, "peak %.3f, expected ~0.5", peak);
    CHECK(atomic_load(&e->underruns) == 0, "%u underruns", atomic_load(&e->underruns));
}

static void TestStaleChannelMap(void) {
    printf("a format change under a stale channel map can't write outside the buffer\n");
    Setup(48000, 48000, 512, 512, true);
    Run(1.0);
    // The device switched to one channel; the map still says "right = channel 2".
    enum { N = 512, GUARD = 64 };
    static float mem[N + GUARD];
    for (int i = 0; i < N + GUARD; ++i) mem[i] = 7.0f;
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 1;
    abl.mBuffers[0].mDataByteSize = N * (UInt32)sizeof(float);
    abl.mBuffers[0].mData = mem;
    AudioTimeStamp now = { 0 };
    SimWrite(512);
    IOProc(0, &now, NULL, NULL, &abl, NULL, e);
    int touched = 0;
    for (int i = N; i < N + GUARD; ++i) if (mem[i] != 7.0f) ++touched;
    CHECK(touched == 0, "%d floats past the end of the buffer were overwritten", touched);
    // A second buffer that is shorter than the first must limit the frame count too.
    static float left[N], right[N / 2 + GUARD];
    for (int i = 0; i < N / 2 + GUARD; ++i) right[i] = 7.0f;
    struct { AudioBufferList l; AudioBuffer second; } two;
    two.l.mNumberBuffers = 2;
    two.l.mBuffers[0] = (AudioBuffer){ 1, N * (UInt32)sizeof(float), left };
    two.l.mBuffers[1] = (AudioBuffer){ 1, (N / 2) * (UInt32)sizeof(float), right };
    e->left = (ChanPos){0, 0}; e->right = (ChanPos){1, 0};
    SimWrite(512);
    IOProc(0, &now, NULL, NULL, &two.l, NULL, e);
    touched = 0;
    for (int i = N / 2; i < N / 2 + GUARD; ++i) if (right[i] != 7.0f) ++touched;
    CHECK(touched == 0, "%d floats past the end of the shorter buffer were overwritten", touched);
}

/// A bus plays on a second device. The virtual device's clock follows the main device, so next to this
/// one it drifts, and nothing steers it: the engine has to keep up by itself.
static void TestBusDrift(double speed) {
    printf("a second output whose clock runs %+.0f ppm off stays in step without the clock lock\n", (1.0 / speed - 1.0) * 1e6);
    Setup(48000, 48000, 512, 512, true);
    e->steersClock = false;
    atomic_store(&gRing->clockFollowed, 0);              // the driver only follows the main ring's clock
    gSimFollowClock = false; gSimSpeed = speed; gSimTone = 440.0;
    Run(5.0);
    CHECK(e->primed, "not playing");
    double before = AvgBufferedMs();
    int crossings = 0; float prev = 0; long frames = 0; float peak = 0, jump = 0;
    for (uint32_t c = 0, n = (uint32_t)(600.0 * e->rate / e->bufFrames); c < n; ++c) {
        const float* o = Cycle();
        for (uint32_t f = 0; f < e->bufFrames; ++f, ++frames) {
            float v = o[f * 2];
            if (prev <= 0 && v > 0) ++crossings;
            if (fabsf(v) > peak) peak = fabsf(v);
            if (frames > 0 && fabsf(v - prev) > jump) jump = fabsf(v - prev);
            prev = v;
        }
    }
    double after = AvgBufferedMs();
    CHECK(atomic_load(&e->underruns) == 0, "%u underruns in 10 minutes", atomic_load(&e->underruns));
    CHECK(e->primed, "stopped playing");
    CHECK(fabs(after - before) < 8, "buffered %.1f ms at the start, %.1f ms 10 minutes later", before, after);
    CHECK(atomic_load(&e->adjusts) > 0, "the drift was never corrected");
    // 440 Hz at 0.5: neighbouring samples differ by at most 0.5 * 2π * 440 / 48000 ≈ 0.029. A dropped or
    // repeated stretch of audio would show as a bigger step.
    CHECK(jump < 0.035f, "a step of %.3f between two samples (a click)", jump);
    CHECK(peak > 0.45f && peak <= 0.5f, "peak %.3f, expected ~0.5", peak);
    double hz = crossings / (frames / e->rate);
    CHECK(fabs(hz - 440.0 * speed) < 0.5, "a 440 Hz tone came out at %.2f Hz", hz);
    CHECK(atomic_load(&gRing->clockTicksPerFrame) == 0, "a bus engine published a clock rate");
}

int main(void) {
    TestBitPerfect();
    TestCleanStop();
    TestRealUnderrun();
    TestChunkDecays(true);
    TestChunkDecays(false);
    TestBurst();
    TestRateConversion(44100, 24000);
    TestRateConversion(16000, 48000);
    TestRateConversion(44100, 48000);
    TestStaleChannelMap();
    TestBusDrift(1.0 + 150e-6);
    TestBusDrift(1.0 - 150e-6);
    printf(gFailures ? "engine: %d FAILED\n" : "engine: all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
