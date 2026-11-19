//
//  PAVShared.h — layout of the shared-memory ring between the HAL driver and the app.
//
//  The app creates an IOSurface (plain shared memory), hands its ID to the driver through the
//  'apsf' device property, and the driver writes the final mix straight into it from its IO
//  thread. The app's output IOProc reads it directly: no polling thread, no IPC per audio chunk.
//
#pragma once
#include <stdint.h>
#include <stdatomic.h>

#define PAV_SHM_MAGIC    0x50415652u   // 'PAVR'
#define PAV_SHM_VERSION  1u
#define PAV_SHM_FRAMES   65536u        // power of two
#define PAV_SHM_MASK     (PAV_SHM_FRAMES - 1u)
#define PAV_SHM_CHANNELS 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t capacityFrames;
    uint32_t channels;
    _Atomic uint64_t writeEnd;     // sample time just past the newest written frame
    _Atomic uint32_t generation;   // bumped whenever the driver's IO restarts (sample time resets)
    _Atomic uint32_t attached;     // set by the driver once it writes here
    uint8_t  _pad[64 - 32];
    float    data[PAV_SHM_FRAMES * PAV_SHM_CHANNELS];   // interleaved stereo, indexed by sample time & mask
} PAVSharedRing;

#define PAV_SHM_BYTES ((uint32_t)sizeof(PAVSharedRing))
