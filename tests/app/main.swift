//
//  Tests for the app's pure logic: the output switching rules, per-app outputs, Wine programs, call-app
//  matching and the shared constants.
//  Built together with the app's sources (minus its main.swift) by scripts/test.sh.
//
import CoreAudio
import Foundation

var failures = 0
func check(_ ok: @autoclosure () -> Bool, _ what: String, line: Int = #line) {
    if !ok() { failures += 1; print("  FAIL line \(line): \(what)") }
}

print("output switching rules")
do {
    // History is newest first.
    let history = ["speakers", "dac", "airpods"]
    check(DeviceSwitching.deviceToSwitchTo(appeared: ["headphones"], history: history) == "headphones",
          "a device never seen before becomes the output")
    check(DeviceSwitching.order(afterConnecting: ["airpods"], history: history) == ["airpods", "speakers", "dac"],
          "a reconnecting device moves to the front, the rest keep their order")
    check(DeviceSwitching.deviceToSwitchTo(appeared: ["airpods", "dac"], history: history) == "dac",
          "several at once: the one used most recently before")
    check(DeviceSwitching.deviceToSwitchTo(appeared: [], history: history) == nil, "nothing appeared: no switch")
    check(DeviceSwitching.fallback(history: history, connected: ["airpods", "dac"]) == "dac",
          "output gone: the most recent device that is still connected")
    check(DeviceSwitching.fallback(history: history, connected: ["new"]) == "new", "nothing known is connected: any device")
    check(DeviceSwitching.fallback(history: history, connected: []) == nil, "no devices at all")
    // Headphones shared with other devices (AirPods playing from an iPhone).
    let grace = DeviceSwitching.ownershipGrace
    check(DeviceSwitching.ownership(owned: nil, unownedFor: 60) == .ours, "a device that can't be shared is always ours")
    check(DeviceSwitching.ownership(owned: true, unownedFor: 0) == .ours, "headphones the Mac has")
    check(DeviceSwitching.ownership(owned: false, unownedFor: 0) == .asking(grace), "just started playing: wait for the answer")
    check(DeviceSwitching.ownership(owned: false, unownedFor: 1) == .asking(grace - 1), "still waiting")
    check(DeviceSwitching.ownership(owned: false, unownedFor: grace) == .elsewhere, "not ours after the grace time: give up")
    check(DeviceSwitching.usable(connected: ["airpods", "speakers"], elsewhere: ["airpods"]) == ["speakers"],
          "headphones another device has aren't used")
    check(DeviceSwitching.fallback(history: history, connected: DeviceSwitching.usable(connected: ["airpods", "speakers"], elsewhere: ["airpods"])) == "speakers",
          "fallback skips headphones another device has")
    check(DeviceSwitching.usable(connected: ["airpods", "speakers"], elsewhere: []) == ["airpods", "speakers"], "nothing elsewhere")
    check(DeviceSwitching.usable(connected: ["airpods"], elsewhere: ["airpods"]) == ["airpods"], "the only device stays usable")
    check(DeviceSwitching.usable(connected: ["speakers"], elsewhere: ["gone"]) == ["speakers"], "unknown UIDs don't matter")
    // The level headphones arrive with (AirPods coming over from an iPhone at 69%).
    check(DeviceSwitching.arrivalLevel(before: 1, now: 0.69) == 0.69, "a level reported after we set full volume is taken over")
    check(DeviceSwitching.arrivalLevel(before: 1, now: 1) == nil, "still at full volume: nothing reported")
    check(DeviceSwitching.arrivalLevel(before: 0.8, now: 0.8) == nil, "a level it already had before playing stays")
    check(DeviceSwitching.arrivalLevel(before: 0.8, now: 0.5) == 0.5, "a new level replaces one left from before")
    check(DeviceSwitching.arrivalLevel(before: 0.8, now: 1) == nil, "back at full volume: nothing to take over")
    check(DeviceSwitching.canTakeOverOnConnect(transport: kAudioDeviceTransportTypeBluetooth), "Bluetooth takes over")
    check(DeviceSwitching.canTakeOverOnConnect(transport: kAudioDeviceTransportTypeUSB), "USB takes over")
    for t in [kAudioDeviceTransportTypeVirtual, kAudioDeviceTransportTypeAggregate, kAudioDeviceTransportTypeAirPlay] {
        check(!DeviceSwitching.canTakeOverOnConnect(transport: t), "virtual, aggregate and AirPlay devices never take over")
    }
}

print("call-app matching")
do {
    let list = ["us.zoom.xos", " com.example.Meet "]
    check(MicMonitor.matches("us.zoom.xos", list), "exact bundle ID")
    check(MicMonitor.matches("US.ZOOM.XOS", list), "case doesn't matter")
    check(MicMonitor.matches("us.zoom.xos.helper", list), "helpers of a listed app")
    check(MicMonitor.matches("com.example.meet", list), "entries are trimmed")
    check(!MicMonitor.matches("us.zoom.xosx", list), "an ID that merely starts the same")
    check(!MicMonitor.matches("", list), "no bundle ID")
}

print("per-app outputs")
do {
    let connected: Set<String> = ["speakers", "dac", "airpods"]
    // Keys are pids, or "b:<bundle id>" for apps that aren't running.
    var p = AppRouting.plan(wants: ["10": "dac", "11": "", "12": "speakers", "b:com.example.player": "dac"],
                            connected: connected, main: "speakers", current: [:], maxBuses: 4)
    check(p.buses == [1: "dac"], "one bus, for the only other device a running app plays on")
    check(p.table == ["10": 1, "11": 0, "12": 0, "b:com.example.player": 1],
          "an app whose output is the main one stays in the main mix; every entry is in the table")

    p = AppRouting.plan(wants: ["10": "gone", "b:com.example.player": "dac"], connected: connected, main: "speakers",
                        current: [:], maxBuses: 4)
    check(p.buses.isEmpty && p.table == ["10": 0, "b:com.example.player": 0],
          "a device that isn't connected, or one only apps that aren't running want: main output")

    p = AppRouting.plan(wants: ["10": "dac"], connected: connected, main: "dac", current: [1: "dac"], maxBuses: 4)
    check(p.buses.isEmpty && p.table == ["10": 0], "the device became the main output: no bus any more")

    // Buses in use keep their number when others come and go.
    p = AppRouting.plan(wants: ["10": "dac", "11": "airpods"], connected: connected, main: "speakers",
                        current: [2: "dac"], maxBuses: 4)
    check(p.buses == [2: "dac", 1: "airpods"] && p.table == ["10": 2, "11": 1], "a playing bus is not renumbered")
    p = AppRouting.plan(wants: ["11": "airpods"], connected: connected, main: "speakers",
                        current: [2: "dac", 1: "airpods"], maxBuses: 4)
    check(p.buses == [1: "airpods"], "a bus nobody plays on is given up")

    p = AppRouting.plan(wants: ["10": "dac", "11": "airpods"], connected: connected, main: "speakers", current: [:], maxBuses: 1)
    check(p.buses.count == 1 && Set(p.table.values) == [0, 1], "more devices than buses: the rest play on the main output")
    p = AppRouting.plan(wants: ["10": "dac"], connected: connected, main: nil, current: [:], maxBuses: 0)
    check(p.buses.isEmpty && p.table == ["10": 0], "no buses at all")

    check(AppRouting.isBundleID("com.example.app") && !AppRouting.isBundleID("/Applications/Foo.app")
          && !AppRouting.isBundleID("wine:foo.exe") && !AppRouting.isBundleID("proc:afplay") && !AppRouting.isBundleID(""),
          "only bundle IDs are sent to the driver as such")
}

print("Windows programs run by Wine")
do {
    check(WineApp.isLoader(executablePath: "/opt/wine/bin/wine64-preloader") && WineApp.isLoader(executablePath: "/x/Wine.app/Contents/MacOS/wine")
          && WineApp.isLoader(executablePath: "/x/wine-preloader"), "Wine's loaders")
    check(!WineApp.isLoader(executablePath: "/opt/wine/bin/wineserver") && !WineApp.isLoader(executablePath: "/usr/bin/afplay"),
          "the Wine server and other programs are not")
    check(WineApp.program(inArguments: ["C:\\Program Files\\Some Game\\Game.exe", "-windowed"]) == "Game.exe", "Windows path")
    check(WineApp.program(inArguments: ["/opt/wine/bin/wine64-preloader", "/opt/wine/bin/wine64", "/games/My Game/Play.EXE"]) == "Play.EXE",
          "arguments that weren't moved down, a Unix path, upper-case extension")
    check(WineApp.program(inArguments: ["player.exe"]) == "player.exe", "a bare file name")
    check(WineApp.program(inArguments: ["/bin/sh", "-c", "echo"]) == nil && WineApp.program(inArguments: [".exe"]) == nil, "no program")
    let id = WineApp.identity(program: "Game.exe")
    check(id.key == "wine:game.exe" && id.name == "Game", "saved by program name, without case; shown without the extension")

    // KERN_PROCARGS2: argc, the executable's path, padding, then the arguments.
    var raw: [UInt8] = [3, 0, 0, 0]
    raw += Array("/opt/wine/bin/wine64-preloader".utf8) + [0, 0, 0, 0]
    for a in ["C:\\Games\\Foo\\Foo.exe", "-fullscreen", ""] { raw += Array(a.utf8) + [0] }
    raw += Array("HOME=/somewhere".utf8) + [0]
    check(WineApp.parseArguments(raw) ?? [] == ["C:\\Games\\Foo\\Foo.exe", "-fullscreen", ""], "arguments are read, the environment is not")
    check(WineApp.parseArguments([1, 0]) == nil && WineApp.parseArguments([0, 0, 0, 0, 65, 0]) == nil, "nonsense is refused")
    check(WineApp.identity(pid: getpid()) == nil, "this test is not a Wine program")
    check(WineApp.arguments(getpid())?.first?.hasSuffix("app_test") == true, "a real process's arguments can be read")
}

print("constants shared with the driver")
do {
    check(kPropClients == 0x6170_636C && kPropVolumes == 0x6170_766C && kPropLatency == 0x6170_6C74, "property selectors")
    check(kVirtualDeviceUID == "PerAppVolume_UID", "device UID")
    check(kPropRoutes == 0x6170_7274 && kPAVMaxBuses >= 1 && kPAVRoutesKeyRoutes == "routes" && kPAVRoutesKeySurfaces == "surfaces",
          "per-app outputs")
    check(PAVNearestSampleRate(24000) == 24000 && PAVNearestSampleRate(16000) == 16000, "headset rates are matched exactly")
    check(PAVNearestSampleRate(47999.5) == 48000 && PAVNearestSampleRate(384000) == 192000, "nearest supported rate")
}

print(failures == 0 ? "app: all passed" : "app: \(failures) FAILED")
exit(failures == 0 ? 0 : 1)
