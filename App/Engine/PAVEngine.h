#ifndef PAVEngine_h
#define PAVEngine_h

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

/// Plays the virtual device's mix on `outputDevice` (shared-memory ring, or 'apad' polling as fallback). Output-only: no input stream, no capture.
OSStatus PAVEngineStart(AudioObjectID virtualDevice, AudioObjectID outputDevice);
void     PAVEngineStop(void);
bool     PAVEngineIsRunning(void);
/// True when reading the driver's mix through shared memory (false = polling fallback).
bool     PAVEngineIsShared(void);
uint32_t PAVEngineUnderruns(void);
/// Current buffered latency in milliseconds (approx).
double   PAVEngineBufferedMs(void);

#endif
