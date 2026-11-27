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

    // MARK: Headphones shared with other devices
    // AirPods connect to the Mac while they play from an iPhone. They are in the device list then, but what
    // the Mac plays to them isn't heard. Playing to them is also how the Mac asks for them, and the iPhone
    // gives them up unless it is playing something itself. So such headphones are tried like any other
    // device, and given up (as if they weren't connected) when the Mac doesn't get them.

    /// How long we play to headphones the Mac doesn't have before giving up. When they are free the Mac
    /// gets them in about a second; macOS itself moves to the speakers 2 s after being refused (which takes
    /// most of another second).
    static let ownershipGrace: TimeInterval = 3

    enum Ownership: Equatable {
        case ours                   // heard (or a device that can't be shared at all)
        case asking(TimeInterval)   // look again after this long
        case elsewhere              // another device has them: use something else
    }

    /// What to make of an output we are playing to. `owned`: nil for devices that aren't shared;
    /// `unownedFor`: how long we have been playing to it without having it.
    static func ownership(owned: Bool?, unownedFor: TimeInterval) -> Ownership {
        guard owned == false else { return .ours }
        return unownedFor >= ownershipGrace ? .elsewhere : .asking(ownershipGrace - unownedFor)
    }

    // MARK: The level headphones arrive with
    // Bluetooth headphones say what level they are at only once they play for us: AirPods coming over from
    // an iPhone bring the level they had there. By then we have set the device to full volume and made its
    // old level the master volume, so the level they report would apply on top of ours.

    /// How long after we start playing to a device (or the Mac gets it from another device) a level it
    /// changes to is the level it arrived with. AirPods report it as the stream starts, other headphones
    /// a quarter of a second after they connect.
    static let arrivalWindow: TimeInterval = 3

    /// The level to take over (it becomes the master volume and the device goes back to full volume), if
    /// the device reported one. `before`: its level when the window opened. Changes after the window are
    /// left alone: the volume buttons on headphones keep working below our level.
    static func arrivalLevel(before: Float, now: Float) -> Float? {
        abs(now - before) > 0.005 && now < 0.999 ? now : nil
    }

    // MARK: Headphones that set their own level
    // AirPods change their level themselves: Personalized Volume moves it with the noise around and with
    // what you usually listen at, and they come over from an iPhone at the level they had there. At full
    // volume below a master volume of ours they would be told that you turned them all the way up each time.
    // So they keep the level they ask for and the master volume is that level: it shows what they set, and
    // changing it changes theirs. The driver leaves the mix at full level meanwhile (see PAVProtocol.h).

    enum LevelSync: Equatable {
        case none
        case toDevice(Float)   // the master volume was changed: the headphones go to it
        case toMaster(Float)   // the headphones changed their level: the master volume shows it
    }

    /// What to do so the master volume and the headphones' own level are the same again. `last`: both
    /// after the previous look (nil: the first one, where the headphones' level counts). A change of the
    /// master volume (the volume keys, the slider) wins over one the headphones made at the same moment.
    static func levelSync(master: Float, device: Float, last: (master: Float, device: Float)?) -> LevelSync {
        guard let last else { return abs(master - device) > 0.0005 ? .toMaster(device) : .none }
        if abs(master - last.master) > 0.0005 { return .toDevice(master) }
        if abs(device - last.device) > 0.004 { return .toMaster(device) }   // their level has 127 steps
        return .none
    }

    /// The level such headphones start at. Full volume the first time is what versions before 1.6 left
    /// them at, not a level anyone chose: they start at the master volume instead.
    static func ownLevel(device: Float, master: Float, seenBefore: Bool) -> Float {
        !seenBefore && device > 0.999 ? master : device
    }

    /// The devices automatic choices may use: not the ones another device has. All of them if nothing
    /// else is left (better to keep the choice than to have no output).
    static func usable(connected: [String], elsewhere: Set<String>) -> [String] {
        let rest = connected.filter { !elsewhere.contains($0) }
        return rest.isEmpty ? connected : rest
    }
}
