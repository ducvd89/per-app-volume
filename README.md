# Per-App Volume

A macOS menu bar app that gives every app its own volume slider, from 0 to 200%.

It never uses the microphone, never captures audio (no process taps) and never records the screen, so macOS
shows no orange or purple privacy indicator and asks for no permissions.

## Features

- **Per-app volume** from 0 to 200% (above 100% is a boost), plus mute for each app
- **Remembers volumes** per app (by bundle ID) and re-applies them when the app comes back
- **Groups helper processes** under their app (Chrome Helper → Chrome, WebKit → Safari, game launchers → game)
- **Clean list:** shows the running apps (with windows) that have ever played audio, plus pinned apps;
  system services are never listed
- **Pin, hide and multi-select** (⌘-click) to mute, pin, hide or set the volume of several apps at once
- **Level meter** for each app
- **Master volume** follows the keyboard volume keys (with the macOS HUD). 100% in the app is the real
  maximum of your output device: the device runs at full volume and its previous level becomes the master volume
- **Output switching that behaves like macOS:** plugged-in headphones, DACs and Bluetooth devices take over when
  they are new or were used more recently than the current output; unplugging falls back to the last used device.
  Or use your own priority order
- **Sample rates** from 44.1 kHz to 192 kHz, matched to the real device automatically
- **Global shortcut** to open the panel (⌥⌘V by default). It uses a Carbon hotkey, so no Accessibility permission
- **Open at Login**

## Performance

Measured on an Apple Silicon MacBook Pro while playing audio:

| | |
|---|---|
| coreaudiod CPU | ~1% |
| Per-App Volume CPU | ~0.1% |
| Added latency | ~13 ms over the output buffer (~24 ms total) |
| Dropouts with every core at 100% | 0 |

## How it works

```
Apps (Spotify, Safari, games…) ──► "Per-App Volume" virtual output device (HAL driver inside coreaudiod)
                                      │  ProcessOutput: each client's own gain, applied before the mix
                                      │  WriteMix: the final mix → a shared-memory ring (IOSurface)
                                      ▼
            Per-App Volume.app reads the ring directly from its output IOProc (no input stream)
                                      │
                                      ▼
                         Your real speakers / headphones / DAC
```

- The driver is an AudioServerPlugIn. It applies a per-client gain in `ProcessOutput` and writes the mix into a
  ring buffer that the app shares with it through an IOSurface. There is no IPC per audio chunk and no polling thread.
- The app opens only an output IOProc on the real device. Because it never opens an input stream and never uses
  ScreenCaptureKit or process taps, macOS doesn't treat it as recording.
- The output buffer adapts to the size of the chunks the driver writes, grows after a dropout and slowly shrinks
  back after long clean stretches. Clock drift between the virtual and real devices is corrected by ±1 frame
  per cycle.
- If the shared ring can't be attached, the app falls back to pulling the mix through a custom device property.
- Every Core Audio call in the app runs on a background queue, so the UI stays responsive even when coreaudiod is slow.

## Install

1. Download `Per-App-Volume.dmg` from [Releases](../../releases), open it and drag **Per-App Volume** into
   **Applications**.
2. The app is ad-hoc signed, not notarized, so macOS blocks it the first time. Either right-click the app →
   **Open** → **Open**, or run:
   ```bash
   xattr -dr com.apple.quarantine "/Applications/Per-App Volume.app"
   ```
3. In the welcome window, click **Install Driver…** and enter your administrator password.
4. Done. The icon lives in the menu bar (or press ⌥⌘V).

To reinstall, update or remove the driver, open **Settings → Virtual Audio Driver**. Removing it sends audio back
to your real device.

Requires macOS 13 or later (Apple Silicon or Intel).

## Build from source

Only the Command Line Tools are needed (no Xcode):

```bash
./build.sh
```

This produces `dist/Per-App Volume.app`, `dist/Per-App-Volume.zip` and `dist/Per-App-Volume.dmg`. The driver is
embedded in `Contents/Resources/PerAppVolume.driver`. To install the driver from Terminal instead of the app, run
`sudo ./scripts/install-driver.sh`.

## Project layout

- `Driver/PerAppVolumeDriver.c`: the AudioServerPlugIn (virtual device, per-client gain, shared ring)
- `App/Engine/`: the realtime engine that plays the mix on the real device (`PAVEngine.c`) and the shared ring
  layout (`PAVShared.h`)
- `App/Sources/`: the SwiftUI menu bar app
  - `Router.swift`: chooses the output device, hands the volume over, starts and stops the engine
  - `DeviceSwitching.swift`: macOS' device switching rules
  - `AppStore.swift`: the app list and the per-app gains sent to the driver

## Known issues

- Every coreaudiod restart (installing or removing a driver) makes macOS' AirPlay helper register its drivers twice
  as often. After many restarts coreaudiod can get stuck at 100% CPU. This is a macOS bug; a reboot clears it.
