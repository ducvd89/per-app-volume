# Per-App Volume

A macOS menu bar app that gives every app its own volume slider, from 0 to 200%.

It asks for no permissions. It never opens the microphone, never captures audio (no process taps) and never
records the screen, so macOS shows no orange or purple privacy indicator.

Requires macOS 13 or later, on Apple Silicon or Intel.

## Install

1. Download `Per-App-Volume.dmg` from [Releases](../../releases), open it and drag **Per-App Volume** into
   **Applications**.
2. The app is ad-hoc signed and not notarized, so macOS blocks it the first time. Either right-click the app →
   **Open** → **Open**, or run:
   ```bash
   xattr -dr com.apple.quarantine "/Applications/Per-App Volume.app"
   ```
3. In the welcome window, click **Install Driver…** and enter your administrator password. This installs the
   virtual audio device that apps play into.
4. Click the sliders icon in the menu bar, or press ⌥⌘V.

To update, reinstall or remove the driver, open **Settings → Virtual Audio Driver**. Removing it sends audio
straight back to your real device, after which you can delete the app.

## Using it

The panel lists the apps that are playing audio. Each one has a slider, a mute button and a level meter.

| To | Do this |
|---|---|
| Change an app's volume | Drag its slider. It snaps at 100%; anything above is a boost |
| Change several apps at once | ⌘-click or ⇧-click the rows, then use the bar that appears at the top |
| Pin an app to the top, hide it or reset it to 100% | Right-click its row |
| Change the master volume | Use the keyboard volume keys as usual, or the slider at the top of the panel |
| Pick the output device | Click the device name at the top of the panel |

Quitting the app sends audio back to your real device.

## Features

**Volumes**

- 0 to 200% and mute for each app.
- Volumes are remembered by bundle ID and applied from the first sound an app makes when it comes back.
- Windows programs run by Wine (CrossOver, Whisky, Game Porting Toolkit…) have no bundle ID. When no Mac app
  stands for them, they are remembered by the name of their `.exe`.
- Helper processes are grouped under their app: Chrome Helper under Chrome, WebKit under Safari, game launchers
  under the game.
- The master volume follows the keyboard volume keys and shows the macOS HUD. 100% is the real maximum of your
  output device: the device itself runs at full volume and its previous level becomes the master volume.

**The app list**

- Shows running apps with windows that have played audio, plus the apps you pinned. System services are never
  listed.
- Hidden apps keep their volume and can be shown again from the bottom of the panel.

**Output devices**

- The device that connected last (headphones, a DAC, Bluetooth) or that you pick in Control Center becomes the
  output. When it disconnects, the output falls back to the most recently connected device that is still there.
  Devices that stay connected keep their order.
- Or set your own priority order in Settings, and the first connected device in it is always used.
- An output device for each app: the speaker button on an app's row sends that app to a device of its own,
  while everything else stays on the main output. Up to four devices besides the main one can play at once.
- The choice is remembered for each app. While its device isn't connected, the app plays on the main output,
  and it moves back by itself when the device returns.
- An app on a device of its own plays at that device's own volume. The master volume and the volume keys
  belong to the main output.
- Sample rates from 8 kHz to 192 kHz are matched to the real device automatically, including the low rates
  Bluetooth headsets switch to while their microphone is on. Any other rate is converted.
- AirPods ear detection: taking AirPods out of your ears pauses playback and putting them back in resumes it.
  macOS only does this itself when the AirPods are the default output, not a virtual device.
- AirPods in use by another device: AirPods connect to the Mac while they play from your iPhone, and what the
  Mac plays to them then isn't heard. Per-App Volume asks for them like macOS does; if the iPhone keeps them
  (it is playing something), the output goes back to the previous device after 3 seconds, and apps with the
  AirPods as their own output play on the main output. They are used again once the Mac has them, when they
  reconnect, or when you pick them.

**Everything else**

- A global shortcut opens the panel: ⌥⌘V by default, with four alternatives in Settings. It is a Carbon hotkey,
  so it needs no Accessibility permission.
- Open at Login.

## Calls

Echo cancellation and voice processing need the real output device, so calls bypass Per-App Volume.

While a calling app uses the microphone, the macOS default output becomes your real speakers or headphones. A
few seconds after the microphone stops, the output goes back to Per-App Volume. Per-app volumes and per-app
outputs don't apply to anything during the call.

- Plugging or unplugging headphones during a call still switches the output. If you pick another output
  yourself, it is left alone.
- The app only reads whether the microphone is in use. It opens no input stream, needs no permission and causes
  no orange dot.
- The default calling apps are FaceTime, iPhone calls, Zoom, Teams, Slack, Discord, WhatsApp, Telegram, Skype,
  Webex, Messenger, Signal, Viber and Zalo. You can edit the list in Settings. On macOS 13 the list isn't
  available and any app using the microphone counts as a call.
- You can turn the whole feature off in Settings.

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

- **The driver** is an AudioServerPlugIn. coreaudiod hands it each client's audio before mixing, so it applies
  the per-app gain in `ProcessOutput`, then writes the mix into a ring buffer it shares with the app through an
  IOSurface. There is no IPC per audio chunk and no polling thread.
- **An output for each app.** The driver takes the apps you gave a device of their own out of the mix and
  mixes them into a second ring for each device. The app plays every ring on its device with another output
  IOProc. The virtual device's clock can follow only one real device, the main one, so on the others the app
  makes up for clock drift by stretching or shortening a buffer by one frame now and then.
- **The app** opens only an output IOProc on the real device. Because it never opens an input stream and never
  uses ScreenCaptureKit or process taps, macOS doesn't treat it as recording.
- **Bit-perfect at 100%.** The virtual device's clock is locked to the real device's clock. The app publishes
  the real device's measured rate (plus a tiny correction from the buffer fill level) in the shared ring, and
  the driver generates its timestamps from it. Samples never need resampling to make up for clock drift.
- **Adaptive buffer.** The output buffer follows the size of the chunks the driver writes, which the driver
  reports in the shared ring. It grows after a dropout and shrinks back: within seconds when the chunks get
  smaller, slowly after a dropout.
- **Picture stays in sync.** The virtual device reports the latency of everything behind it (the app's buffer
  plus the real device), so players line up picture and sound as they would on the real device.
- **Only the app can read the mix.** The driver hands the mix, the shared ring and its settings only to the
  process coreaudiod reports under the app's bundle ID. That keeps other processes from reading system audio
  through the driver, but it is not a signature check, since the app is not signed with a Developer ID.
- **Fallback.** If the shared ring can't be attached, the app pulls the mix through a custom device property
  instead.
- Every Core Audio call in the app runs on a background queue, so the UI stays responsive even when coreaudiod
  is slow.

## Performance

Measured on an Apple Silicon MacBook Pro while playing audio:

| | |
|---|---|
| coreaudiod CPU | ~1% |
| Per-App Volume CPU | ~0.1% |
| Added latency | ~13 ms over the output buffer (~24 ms total) with apps that use 512-frame buffers. An app that asks for a bigger buffer adds its size while it plays |
| Dropouts with every core at 100% | 0 |
| Resampled audio (clock drift) | None: the virtual clock is locked to the device |

## Build from source

Only the Command Line Tools are needed, not Xcode:

```bash
./build.sh
```

This produces a universal build in `dist/`: `Per-App Volume.app`, `Per-App-Volume.zip` and
`Per-App-Volume.dmg`. The driver is embedded in the app at `Contents/Resources/PerAppVolume.driver`.

Copy the built app to `/Applications` before running it. If another copy with the same bundle ID is already
installed there, macOS treats the menu bar icon of a copy started from `dist/` as anonymous: it forgets the
icon's position on every launch, and menu bar managers (Thaw, Ice…) can't move it.

To install or remove the driver from Terminal instead of from the app:

```bash
sudo ./scripts/install-driver.sh
sudo ./scripts/uninstall-driver.sh
```

Both restart coreaudiod, which interrupts audio for a moment.

## Tests

```bash
./scripts/test.sh
```

Runs the engine against a simulated driver, the driver's entry points against a stand-in host, and the app's
switching and per-app output rules. Nothing is installed and no audio device is touched.

## Project layout

| Path | What it is |
|---|---|
| `Driver/PerAppVolumeDriver.c` | The AudioServerPlugIn: virtual device, per-client gain, shared ring |
| `App/Engine/PAVEngine.c` | The realtime engine that plays the mix on the real device |
| `App/Engine/PAVShared.h` | The layout of the shared ring |
| `App/Engine/PAVProtocol.h` | The constants the driver and the app share |
| `App/Sources/` | The SwiftUI menu bar app |
| `App/Sources/Router.swift` | Chooses the output device, hands the volume over, starts and stops the engine |
| `App/Sources/DeviceSwitching.swift` | The output switching rules |
| `App/Sources/AppRouting.swift` | Which app plays on which output, and how Wine programs are told apart |
| `App/Sources/CallMode.swift` | Notices calling apps using the microphone (HAL properties only) |
| `App/Sources/AppStore.swift` | The app list and the per-app gains and outputs sent to the driver |
| `App/Sources/DriverInstaller.swift` | Installs, updates and removes the driver from the app |
| `tests/` | The tests, run by `scripts/test.sh` |

## Known issues

- **Force-quit leaves the device at full volume.** While Per-App Volume forwards audio, the real device runs at
  full volume and the app applies your level. If the app is force-quit or crashes, it can't put the device's own
  level back, and nothing plays until it runs again. Open it again before playing straight to that device.
- **Picture and sound on a second output.** The virtual device reports the latency of the main output. An app
  playing video on another device with a very different latency (Bluetooth next to wired) can be slightly out
  of sync.
- **coreaudiod at 100% CPU after many driver installs.** Every coreaudiod restart (installing or removing a
  driver) makes the macOS AirPlay helper register its drivers twice as often. After many restarts coreaudiod can
  get stuck at 100% CPU. This is a macOS bug, and a reboot clears it.

## License

Copyright (C) 2026 the Per-App Volume contributors.

Per-App Volume is free software: you can redistribute it and modify it under the terms of the
[GNU General Public License, version 3](LICENSE). It comes with no warranty.
