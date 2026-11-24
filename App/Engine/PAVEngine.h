#ifndef PAVEngine_h
#define PAVEngine_h

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include "PAVProtocol.h"

/// Plays the virtual device's mix on `outputDevice` (shared-memory ring, or 'apad' polling as fallback). Output-only: no input stream, no capture.
OSStatus PAVEngineStart(AudioObjectID virtualDevice, AudioObjectID outputDevice);
void     PAVEngineStop(void);
bool     PAVEngineIsRunning(void);
/// True when reading the driver's mix through shared memory (false = polling fallback).
bool     PAVEngineIsShared(void);
/// The virtual device's sample rate the running engine was started for. If the virtual device's rate
/// changes afterwards, restart the engine.
double   PAVEngineSourceRate(void);
uint32_t PAVEngineUnderruns(void);
/// Current buffered latency in milliseconds (approx).
double   PAVEngineBufferedMs(void);

// Buses (1...kPAVMaxBuses): apps with an output device of their own. The driver mixes the apps of a bus
// into the bus's ring, and the bus is played on its device like the main mix is on the main one.

/// The IOSurface ID of the bus's ring (created on first use), to hand to the driver in 'aprt'. 0 if there is none.
int64_t  PAVBusSurfaceID(int bus);
/// Plays the bus on `outputDevice`. The driver must have the bus's ring already.
OSStatus PAVBusStart(int bus, AudioObjectID virtualDevice, AudioObjectID outputDevice);
void     PAVBusStop(int bus);
bool     PAVBusIsRunning(int bus);
/// The device a running bus plays on (kAudioObjectUnknown if it isn't running).
AudioObjectID PAVBusOutput(int bus);
/// The virtual device's sample rate the bus was started for.
double   PAVBusSourceRate(int bus);

#endif
