//
//  PAVProtocol.h — constants shared by the HAL driver, the engine and the Swift app.
//
//  Keep everything here plain C that Swift can import (no atomics): the app sees it through the
//  bridging header.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

#define kPAVDeviceUID    "PerAppVolume_UID"
#define kPAVAppBundleID  "com.perappvolume.app"

// Custom properties on the device object (global scope, main element). Four-char codes written as
// numbers so Swift imports them.
#define kPAVPropClients  0x6170636Cu   // 'apcl'  get      connected clients
#define kPAVPropVolumes  0x6170766Cu   // 'apvl'  get/set  gain table
#define kPAVPropAudio    0x61706164u   // 'apad'  get      new mixed frames (polling fallback)
#define kPAVPropSurface  0x61707366u   // 'apsf'  get/set  shared ring
#define kPAVPropLatency  0x61706C74u   // 'aplt'  get/set  frames of latency the device reports
#define kPAVPropRoutes   0x61707274u   // 'aprt'  get/set  per-app outputs (added in 1.4)

// Keys of the 'apvl' dictionary are either "<pid>" or this prefix followed by a bundle ID.
#define kPAVBundleGainPrefix "b:"

// Per-app outputs. Apps normally end up in the main mix, which the app plays on the main output device.
// An app can be sent to a "bus" instead (1...kPAVMaxBuses): the driver mixes the apps of a bus into a
// shared ring of their own (same layout as the main one, see PAVShared.h) and the app plays each ring on
// another device. 'aprt' is a dictionary with two optional entries:
//   "surfaces": array of kPAVMaxBuses IOSurface IDs, the ring of bus 1, 2, … (0 = none)
//   "routes":   dictionary { "<pid>" | "b:<bundle id>" : bus }, looked up like the gain table; 0 = main mix
// A client whose bus has no ring stays in the main mix.
#define kPAVMaxBuses          4
#define kPAVRoutesKeySurfaces "surfaces"
#define kPAVRoutesKeyRoutes   "routes"

// Sample rates the virtual device can run at. The low ones are what Bluetooth headsets use while
// their microphone is on.
static const double kPAVSampleRates[] = {
    8000.0, 11025.0, 16000.0, 22050.0, 24000.0, 32000.0,
    44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
};
#define kPAVNumSampleRates (sizeof(kPAVSampleRates) / sizeof(kPAVSampleRates[0]))

/// The supported rate closest to `rate`.
static inline double PAVNearestSampleRate(double rate) {
    double best = kPAVSampleRates[0];
    for (size_t i = 1; i < kPAVNumSampleRates; ++i) {
        double d = kPAVSampleRates[i] - rate, b = best - rate;
        if ((d < 0 ? -d : d) < (b < 0 ? -b : b)) best = kPAVSampleRates[i];
    }
    return best;
}
