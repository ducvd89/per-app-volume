import CoreAudio

/// Output-device switching rules, as pure functions so they can be tested.
///
/// The app keeps the output devices in connection order (newest first):
/// - A device that connects (headphones plugged in, a USB DAC, Bluetooth headphones connecting, AirPods put
///   back in your ears) moves to the front and becomes the output.
/// - Choosing a device yourself (Control Center, Sound settings, our menu) also moves it to the front.
/// - Devices that stay connected keep their place. When the current output disappears, the output falls back
///   to the most recently connected (or chosen) device that is still connected; fallbacks don't change the order.
enum DeviceSwitching {
    /// Kinds of devices that take over the output when they appear. Virtual and aggregate devices
    /// (drivers being installed) and AirPlay endpoints never do.
    static func canTakeOverOnConnect(transport: UInt32) -> Bool {
        ![kAudioDeviceTransportTypeVirtual, kAudioDeviceTransportTypeAggregate,
          kAudioDeviceTransportTypeAutoAggregate, kAudioDeviceTransportTypeAirPlay,
          kAudioDeviceTransportTypeUnknown].contains(transport)
    }

    /// The new order after devices connected: they move to the front (keeping their previous relative order),
    /// everything else keeps its place.
    static func order(afterConnecting appeared: [String], history: [String]) -> [String] {
        func rank(_ uid: String) -> Int { history.firstIndex(of: uid) ?? -1 }   // never seen = newest
        let front = appeared.sorted { rank($0) < rank($1) }
        return front + history.filter { !front.contains($0) }
    }

    /// Which newly connected device becomes the output: the newest one (if several connect at once,
    /// the one connected or chosen most recently before).
    static func deviceToSwitchTo(appeared: [String], history: [String]) -> String? {
        order(afterConnecting: appeared, history: history).first { appeared.contains($0) }
    }

    /// Fallback when the current output is gone: most recently connected device still connected.
    static func fallback(history: [String], connected: [String]) -> String? {
        history.first { connected.contains($0) } ?? connected.first
    }
}
