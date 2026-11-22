//
//  driver_test.c — exercises the HAL driver's entry points in-process with a stand-in host.
//
//  The driver source is included directly; nothing is installed and coreaudiod is not involved.
//
#include "../Driver/PerAppVolumeDriver.c"
#include <stdio.h>
#include <unistd.h>

static int gFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++gFailures; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- stand-in host ----
static _Atomic int gConfigChanges = 0;
static OSStatus HostPropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress* a) { return 0; }
static OSStatus HostCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef* out) { *out = NULL; return 0; }
static OSStatus HostWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v) { return 0; }
static OSStatus HostDelete(AudioServerPlugInHostRef h, CFStringRef k) { return 0; }
static OSStatus HostRequestChange(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void* info) {
    // The real host stops IO, calls this, and restarts IO.
    OSStatus st = PAV_PerformDeviceConfigurationChange(gDriverRef, dev, action, info);
    atomic_fetch_add(&gConfigChanges, 1);
    return st;
}
static AudioServerPlugInHostInterface gFakeHost = {
    .PropertiesChanged = HostPropertiesChanged, .CopyFromStorage = HostCopy, .WriteToStorage = HostWrite,
    .DeleteFromStorage = HostDelete, .RequestDeviceConfigurationChange = HostRequestChange,
};

/// Waits for the driver's asynchronous configuration-change request to go through.
static void WaitForConfigChange(int before) {
    for (int i = 0; i < 400 && atomic_load(&gConfigChanges) == before; ++i) usleep(5000);
}

// ---- helpers ----
enum { kAppPID = 1000, kOtherPID = 2000 };
static const AudioObjectPropertyAddress kGlobal = { 0, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

static void AddClient(UInt32 clientID, pid_t pid, const char* bundleID) {
    AudioServerPlugInClientInfo ci = { clientID, pid, true, NULL };
    if (bundleID) ci.mBundleID = CFStringCreateWithCString(NULL, bundleID, kCFStringEncodingUTF8);
    PAV_AddDeviceClient(gDriverRef, kObjectID_Device, &ci);
    if (ci.mBundleID) CFRelease(ci.mBundleID);
}
static void RemoveClient(UInt32 clientID) {
    AudioServerPlugInClientInfo ci = { clientID, 0, true, NULL };
    PAV_RemoveDeviceClient(gDriverRef, kObjectID_Device, &ci);
}
static float ClientGain(UInt32 clientID) { PAVClient* c = FindClient(clientID); return c ? atomic_load(&c->gain) : -1; }

static OSStatus SetPlist(pid_t pid, AudioObjectPropertySelector sel, CFPropertyListRef v) {
    AudioObjectPropertyAddress a = kGlobal; a.mSelector = sel;
    return PAV_SetPropertyData(gDriverRef, kObjectID_Device, pid, &a, 0, NULL, sizeof(v), &v);
}
static OSStatus GetPlist(pid_t pid, AudioObjectPropertySelector sel, CFPropertyListRef* out) {
    AudioObjectPropertyAddress a = kGlobal; a.mSelector = sel;
    UInt32 size = 0; *out = NULL;
    return PAV_GetPropertyData(gDriverRef, kObjectID_Device, pid, &a, 0, NULL, sizeof(*out), &size, out);
}
static CFNumberRef Num64(SInt64 v) { return CFNumberCreate(NULL, kCFNumberSInt64Type, &v); }

static CFDictionaryRef Table(int n, const char* keys[], const float gains[]) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < n; ++i) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        CFNumberRef v = CFNumberCreate(NULL, kCFNumberFloat32Type, &gains[i]);
        CFDictionarySetValue(d, k, v); CFRelease(k); CFRelease(v);
    }
    return d;
}

// ---- tests ----

static void TestGainTable(void) {
    printf("gain table: by pid, by bundle ID for clients the app hasn't seen yet\n");
    const char* keys[] = { "b:com.example.player", "b:com.example.player.quiet", "3001" };
    const float gains[] = { 0.25f, 0.1f, 0.5f };
    CFDictionaryRef t = Table(3, keys, gains);
    CHECK(SetPlist(kAppPID, kPAVPropVolumes, t) == 0, "the app couldn't set the table");
    CFRelease(t);

    AddClient(10, 3000, "com.example.player");            // just launched: only its bundle ID is known
    AddClient(11, 3001, "com.something.else");            // listed by pid
    AddClient(12, 3002, "com.example.player.helper");     // a helper of the app
    AddClient(13, 3003, "com.example.playerx");           // a different app that merely starts the same
    AddClient(14, 3004, "com.example.player.quiet.sub");  // the longest matching entry wins
    AddClient(15, 3005, NULL);
    AddClient(16, 3006, "COM.Example.Player");            // bundle IDs compare without case
    CHECK(ClientGain(10) == 0.25f, "new client of a saved app starts at %.2f, expected 0.25", ClientGain(10));
    CHECK(ClientGain(11) == 0.5f, "pid entry: %.2f", ClientGain(11));
    CHECK(ClientGain(12) == 0.25f, "helper: %.2f", ClientGain(12));
    CHECK(ClientGain(13) == 1.0f, "unrelated app got %.2f", ClientGain(13));
    CHECK(ClientGain(14) == 0.1f, "longest match: %.2f", ClientGain(14));
    CHECK(ClientGain(15) == 1.0f, "no bundle ID: %.2f", ClientGain(15));
    CHECK(ClientGain(16) == 0.25f, "case: %.2f", ClientGain(16));

    // The app's next poll lists every pid explicitly; that overrides the bundle-ID guess.
    const char* keys2[] = { "b:com.example.player", "3000", "3002" };
    const float gains2[] = { 0.25f, 0.25f, 1.0f };
    t = Table(3, keys2, gains2);
    SetPlist(kAppPID, kPAVPropVolumes, t); CFRelease(t);
    CHECK(ClientGain(12) == 1.0f, "explicit pid entry didn't override the guess: %.2f", ClientGain(12));
    CHECK(ClientGain(10) == 0.25f, "%.2f", ClientGain(10));
    CHECK(ClientGain(11) == 1.0f, "a pid dropped from the table should be back at 1: %.2f", ClientGain(11));

    // The gain is applied to that client's audio before the mix.
    float buf[256 * 2]; AudioServerPlugInIOCycleInfo ci = { 0 };
    for (int pass = 0; pass < 2; ++pass) {                // first pass ramps, second is steady
        for (int i = 0; i < 512; ++i) buf[i] = 0.8f;
        PAV_DoIOOperation(gDriverRef, kObjectID_Device, kObjectID_Stream, 10, kAudioServerPlugInIOOperationProcessOutput, 256, &ci, buf, NULL);
    }
    CHECK(fabsf(buf[0] - 0.2f) < 1e-6f && fabsf(buf[511] - 0.2f) < 1e-6f, "0.8 at gain 0.25 came out as %.3f", buf[0]);
    for (UInt32 c = 10; c <= 16; ++c) RemoveClient(c);
}

static void TestAccess(void) {
    printf("only the companion app reads the mix or changes the device\n");
    CFPropertyListRef out = NULL;
    CHECK(GetPlist(kOtherPID, kPAVPropAudio, &out) != 0 && out == NULL, "another process could read the mix");
    CHECK(GetPlist(kAppPID, kPAVPropAudio, &out) == 0 && out != NULL, "the app could not read the mix");
    if (out) CFRelease(out);

    const char* keys[] = { "4242" }; const float gains[] = { 0.0f };
    CFDictionaryRef t = Table(1, keys, gains);
    CHECK(SetPlist(kOtherPID, kPAVPropVolumes, t) != 0, "another process could change volumes");
    CFRelease(t);
    pthread_mutex_lock(&gClientMutex); int n = gPidGainCount; pthread_mutex_unlock(&gClientMutex);
    CHECK(n != 1 || gPidGains[0].pid != 4242, "the refused table was applied anyway");

    CFNumberRef id = Num64(12345);
    CHECK(SetPlist(kOtherPID, kPAVPropSurface, id) != 0, "another process could replace the shared ring");
    CHECK(SetPlist(kOtherPID, kPAVPropLatency, id) != 0, "another process could change the latency");
    CFRelease(id);

    // The client list stays readable (the app's own UI needs nothing more than that from others).
    CHECK(GetPlist(kOtherPID, kPAVPropClients, &out) == 0 && out != NULL, "client list");
    if (out) CFRelease(out);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void TestSharedRing(void) {
    printf("shared ring: attaches for the app, reports its write size, never gives the surface ID away\n");
    int bytes = (int)PAV_SHM_BYTES;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &bytes);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, kIOSurfaceAllocSize, n);
    CFDictionarySetValue(d, kIOSurfaceIsGlobal, kCFBooleanTrue);
    IOSurfaceRef surf = IOSurfaceCreate(d);
    CFRelease(d); CFRelease(n);
    if (!surf) { printf("  (skipped: no IOSurface here)\n"); return; }
    IOSurfaceLock(surf, 0, NULL);
    PAVSharedRing* ring = (PAVSharedRing*)IOSurfaceGetBaseAddress(surf);
    memset(ring, 0, PAV_SHM_BYTES);
    ring->magic = PAV_SHM_MAGIC; ring->version = PAV_SHM_VERSION; ring->capacityFrames = PAV_SHM_FRAMES; ring->channels = PAV_SHM_CHANNELS;

    SInt64 sid = IOSurfaceGetID(surf);
    CFNumberRef idn = Num64(sid);
    CHECK(SetPlist(kOtherPID, kPAVPropSurface, idn) != 0 && atomic_load(&ring->attached) == 0, "attached for another process");
    CHECK(SetPlist(kAppPID, kPAVPropSurface, idn) == 0, "attach refused for the app");
    CFRelease(idn);
    CHECK(atomic_load(&ring->attached) == 1, "attached flag not set");
    CHECK(atomic_load(&ring->features) & PAV_SHM_FEATURE_CHUNK, "feature bit not set");

    for (int who = 0; who < 2; ++who) {
        CFPropertyListRef out = NULL; SInt64 v = -1;
        GetPlist(who ? kOtherPID : kAppPID, kPAVPropSurface, &out);
        if (out) { CFNumberGetValue((CFNumberRef)out, kCFNumberSInt64Type, &v); CFRelease(out); }
        CHECK(v == 1, "'apsf' answered %lld (the surface ID is %lld); expected just 1 = attached", v, sid);
    }

    // One IO cycle: the mix lands in the ring with its size.
    PAV_StartIO(gDriverRef, kObjectID_Device, 1);
    float buf[480 * 2]; for (int i = 0; i < 960; ++i) buf[i] = 0.25f;
    AudioServerPlugInIOCycleInfo ci = { 0 }; ci.mOutputTime.mSampleTime = 0;
    PAV_DoIOOperation(gDriverRef, kObjectID_Device, kObjectID_Stream, 1, kAudioServerPlugInIOOperationWriteMix, 480, &ci, buf, NULL);
    CHECK(atomic_load(&ring->writeEnd) == 480, "writeEnd %llu", atomic_load(&ring->writeEnd));
    CHECK(atomic_load(&ring->chunkFrames) == 480, "chunkFrames %u", atomic_load(&ring->chunkFrames));
    CHECK(ring->data[0] == 0.25f && ring->data[959] == 0.25f, "mix not in the ring");
    PAV_StopIO(gDriverRef, kObjectID_Device, 1);

    CFNumberRef zero = Num64(0);
    CHECK(SetPlist(kAppPID, kPAVPropSurface, zero) == 0, "detach");
    CFRelease(zero);
    // The driver keeps the old surface for a while (its IO thread may still be writing); leave ours alone too.
}
#pragma clang diagnostic pop

static Float64 NominalRate(void) {
    AudioObjectPropertyAddress a = kGlobal; a.mSelector = kAudioDevicePropertyNominalSampleRate;
    Float64 r = 0; UInt32 size = 0;
    PAV_GetPropertyData(gDriverRef, kObjectID_Device, kOtherPID, &a, 0, NULL, sizeof(r), &size, &r);
    return r;
}

static void TestSampleRates(void) {
    printf("sample rates: the low rates Bluetooth headsets use with the mic on are offered\n");
    AudioObjectPropertyAddress a = kGlobal; a.mSelector = kAudioDevicePropertyNominalSampleRate;
    const Float64 wanted[] = { 8000, 16000, 24000, 32000, 44100, 48000, 96000, 192000 };
    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
        int before = atomic_load(&gConfigChanges);
        bool same = fabs(NominalRate() - wanted[i]) < 0.5;
        OSStatus st = PAV_SetPropertyData(gDriverRef, kObjectID_Device, kOtherPID, &a, 0, NULL, sizeof(Float64), &wanted[i]);
        if (!same) WaitForConfigChange(before);
        CHECK(st == 0 && fabs(NominalRate() - wanted[i]) < 0.5, "%.0f Hz: status %d, rate now %.0f", wanted[i], (int)st, NominalRate());
    }
    Float64 odd = 12345;
    CHECK(PAV_SetPropertyData(gDriverRef, kObjectID_Device, kOtherPID, &a, 0, NULL, sizeof(Float64), &odd) != 0, "accepted 12345 Hz");

    AudioObjectPropertyAddress av = kGlobal; av.mSelector = kAudioDevicePropertyAvailableNominalSampleRates;
    UInt32 size = 0;
    PAV_GetPropertyDataSize(gDriverRef, kObjectID_Device, kOtherPID, &av, 0, NULL, &size);
    CHECK(size == kPAVNumSampleRates * sizeof(AudioValueRange), "available rates: %u bytes", size);
    CHECK(PAVNearestSampleRate(23990) == 24000 && PAVNearestSampleRate(47999) == 48000 && PAVNearestSampleRate(384000) == 192000,
          "nearest-rate helper");
}

static UInt32 DeviceLatency(AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyLatency, scope, kAudioObjectPropertyElementMain };
    UInt32 l = 99999, size = 0;
    PAV_GetPropertyData(gDriverRef, kObjectID_Device, kOtherPID, &a, 0, NULL, sizeof(l), &size, &l);
    return l;
}

static void TestLatency(void) {
    printf("latency: the device reports what the app measured behind it\n");
    CHECK(DeviceLatency(kAudioObjectPropertyScopeOutput) == 0, "latency before the app set one: %u", DeviceLatency(kAudioObjectPropertyScopeOutput));
    int before = atomic_load(&gConfigChanges);
    CFNumberRef n = Num64(2400);
    CHECK(SetPlist(kAppPID, kPAVPropLatency, n) == 0, "set refused");
    CFRelease(n);
    WaitForConfigChange(before);
    CHECK(atomic_load(&gConfigChanges) == before + 1, "a latency change must go through a configuration change");
    CHECK(DeviceLatency(kAudioObjectPropertyScopeOutput) == 2400, "output latency %u", DeviceLatency(kAudioObjectPropertyScopeOutput));
    CHECK(DeviceLatency(kAudioObjectPropertyScopeInput) == 0, "input latency %u", DeviceLatency(kAudioObjectPropertyScopeInput));
    // Setting the same value again must not restart IO.
    before = atomic_load(&gConfigChanges);
    n = Num64(2400); SetPlist(kAppPID, kPAVPropLatency, n); CFRelease(n);
    usleep(100000);
    CHECK(atomic_load(&gConfigChanges) == before, "an unchanged latency caused a configuration change");
}

int main(void) {
    CFUUIDRef type = kAudioServerPlugInTypeUUID;
    CHECK(PAV_Create(NULL, type) == gDriverRef, "factory");
    CHECK(PAV_Initialize(gDriverRef, &gFakeHost) == 0, "initialize");
    AddClient(1, kAppPID, kPAVAppBundleID);
    AddClient(2, kOtherPID, "com.example.other");

    TestGainTable();
    TestAccess();
    TestSharedRing();
    TestSampleRates();
    TestLatency();
    printf(gFailures ? "driver: %d FAILED\n" : "driver: all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
