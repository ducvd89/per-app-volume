import CoreAudio

/// macOS' output-device switching rules, as pure functions so they can be tested.
///
/// macOS keeps a most-recently-used history of output devices (newest first):
/// - Choosing a device (Control Center, Sound settings, our menu) moves it to the front.
/// - When a device appears, it becomes the output if it was never seen before, or if it was used more
///   recently than the current output (e.g. headphones you were using are plugged back in).
///   A device you deliberately switched away from does not grab the output back when it reconnects.
/// - When the current output disappears, macOS falls back to the most recently used device that is
///   still connected. Automatic fallbacks do not change the history.
enum DeviceSwitching {
    /// Kinds of devices that take over the output when they appear. Virtual and aggregate devices
    /// (drivers being installed) and AirPlay endpoints never do.
    static func canTakeOverOnConnect(transport: UInt32) -> Bool {
        ![kAudioDeviceTransportTypeVirtual, kAudioDeviceTransportTypeAggregate,
          kAudioDeviceTransportTypeAutoAggregate, kAudioDeviceTransportTypeAirPlay,
          kAudioDeviceTransportTypeUnknown].contains(transport)
    }

    /// Which newly appeared device (if any) should become the output.
    /// - Parameters:
    ///   - appeared: UIDs of devices that just appeared (and are allowed to take over).
    ///   - history: MRU history before this event, newest first.
    ///   - current: UID of the current output, if it is still connected.
    static func deviceToSwitchTo(appeared: [String], history: [String], current: String?) -> String? {
        func rank(_ uid: String) -> Int { history.firstIndex(of: uid) ?? -1 }   // never seen = newest
        guard let best = appeared.min(by: { rank($0) < rank($1) }) else { return nil }
        guard let current else { return best }
        let currentRank = history.firstIndex(of: current) ?? Int.max
        return rank(best) < currentRank ? best : nil
    }

    /// Fallback when the current output is gone: most recently used device still connected.
    static func fallback(history: [String], connected: [String]) -> String? {
        history.first { connected.contains($0) } ?? connected.first
    }
}
