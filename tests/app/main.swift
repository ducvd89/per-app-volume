//
//  Tests for the app's pure logic: the output switching rules, call-app matching and the shared constants.
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

print("constants shared with the driver")
do {
    check(kPropClients == 0x6170_636C && kPropVolumes == 0x6170_766C && kPropLatency == 0x6170_6C74, "property selectors")
    check(kVirtualDeviceUID == "PerAppVolume_UID", "device UID")
    check(PAVNearestSampleRate(24000) == 24000 && PAVNearestSampleRate(16000) == 16000, "headset rates are matched exactly")
    check(PAVNearestSampleRate(47999.5) == 48000 && PAVNearestSampleRate(384000) == 192000, "nearest supported rate")
}

print(failures == 0 ? "app: all passed" : "app: \(failures) FAILED")
exit(failures == 0 ? 0 : 1)
