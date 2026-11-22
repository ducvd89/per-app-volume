import AppKit
import CoreAudio
import os

let callLog = Logger(subsystem: "com.perappvolume.app", category: "call")

/// Calling apps that switch the output back to the real device while they use the microphone.
/// Helpers are matched too (an entry matches its own bundle ID and any ID that starts with "entry.").
let kDefaultCallApps: [String] = [
    "com.apple.FaceTime",
    "com.apple.avconferenced",              // FaceTime audio runs in this daemon
    "com.apple.mobilephone",                // Phone (iPhone calls on the Mac)
    "us.zoom.xos",
    "com.microsoft.teams2",
    "com.microsoft.teams",
    "com.tinyspeck.slackmacgap",
    "com.hnc.Discord",
    "net.whatsapp.WhatsApp",
    "ru.keepcoder.Telegram",
    "org.telegram.desktop",
    "com.skype.skype",
    "com.cisco.webexmeetingsapp",
    "com.webex.meetingmanager",
    "com.facebook.archon",                  // Messenger
    "org.whispersystems.signal-desktop",
    "com.viber.osx",
    "com.vng.zalo",
    "app.carrot4127.zaloswift.mac",
]

/// Tells when a calling app uses the microphone, without opening an input stream: it only reads HAL
/// properties (`kAudioDevicePropertyDeviceIsRunningSomewhere` on input devices and, on macOS 14+,
/// `kAudioProcessPropertyIsRunningInput` on process objects), so no permission and no orange dot.
/// Driven by property listeners only; lives on `audioQueue`.
final class MicMonitor {
    struct Config: Equatable {
        var enabled: Bool
        var onlyListed: Bool
        var apps: [String]
    }

    /// Names of the calling apps using the mic, or nil when there is no call. Called on `audioQueue`.
    var onChange: (([String]?) -> Void)?
    /// Whether an input device is busy only because our engine plays to it (devices with both directions).
    var isOurOutput: (AudioObjectID) -> Bool = { _ in false }

    private var config: Config
    private var started = false
    private var systemListeners: [(AudioObjectPropertySelector, AudioObjectPropertyListenerBlock)] = []
    private var deviceListeners: [AudioObjectID: AudioObjectPropertyListenerBlock] = [:]
    private var processListeners: [AudioObjectID: AudioObjectPropertyListenerBlock] = [:]
    private var last: [String]?? = .none
    private let ownPID = getpid()

    static var hasProcessAPI: Bool { if #available(macOS 14, *) { return true } else { return false } }

    init(config: Config) { self.config = config }

    func update(_ c: Config) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        guard c != config else { return }
        config = c
        evaluate()
    }

    func start() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        guard !started else { return }
        started = true
        let sys = AudioObjectID(kAudioObjectSystemObject)
        let devices = CA.listen(sys, kAudioHardwarePropertyDevices, queue: audioQueue) { [weak self] in
            self?.syncDevices(); self?.evaluate()
        }
        systemListeners.append((kAudioHardwarePropertyDevices, devices))
        if #available(macOS 14, *) {
            let procs = CA.listen(sys, kAudioHardwarePropertyProcessObjectList, queue: audioQueue) { [weak self] in
                self?.evaluate()
            }
            systemListeners.append((kAudioHardwarePropertyProcessObjectList, procs))
        }
        syncDevices()
        evaluate()
    }

    func stop() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        let sys = AudioObjectID(kAudioObjectSystemObject)
        for (sel, l) in systemListeners { CA.unlisten(sys, sel, queue: audioQueue, l) }
        systemListeners.removeAll()
        for (d, l) in deviceListeners { CA.unlisten(d, kAudioDevicePropertyDeviceIsRunningSomewhere, queue: audioQueue, l) }
        deviceListeners.removeAll()
        syncProcessListeners(watch: false)
        started = false
    }

    // MARK: Input devices

    private static func inputDevices() -> [AudioObjectID] {
        CA.array(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices, of: AudioObjectID.self)
            .filter { CA.channelCount($0, scope: kAudioObjectPropertyScopeInput) > 0 }
    }

    private func syncDevices() {
        let now = Set(Self.inputDevices())
        for (d, l) in deviceListeners where !now.contains(d) {
            CA.unlisten(d, kAudioDevicePropertyDeviceIsRunningSomewhere, queue: audioQueue, l)
            deviceListeners.removeValue(forKey: d)
        }
        for d in now where deviceListeners[d] == nil {
            deviceListeners[d] = CA.listen(d, kAudioDevicePropertyDeviceIsRunningSomewhere, queue: audioQueue) { [weak self] in
                self?.evaluate()
            }
        }
    }

    /// Some input device is running for someone. A device that also has outputs (USB headset) is running
    /// whenever our engine plays to it. On macOS 14+ that doesn't matter: the per-process check says exactly
    /// who uses the mic, so such a device always counts (skipping it would hide a call that starts while
    /// anything plays, even the ringtone). On macOS 13 there is no per-process check, so it is skipped.
    private func anyInputRunning() -> Bool {
        deviceListeners.keys.contains { d in
            guard (CA.get(d, kAudioDevicePropertyDeviceIsRunningSomewhere, default: UInt32(0)) ?? 0) != 0 else { return false }
            return Self.hasProcessAPI || !isOurOutput(d)
        }
    }

    /// Look again (the engine started or stopped, which changes what `isOurOutput` answers).
    func refresh() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        if started { evaluate() }
    }

    // MARK: Processes (macOS 14+)

    private struct MicUser { let bundleID: String; let pid: pid_t }

    private func processObjects() -> [AudioObjectID] {
        guard #available(macOS 14, *) else { return [] }
        return CA.array(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyProcessObjectList, of: AudioObjectID.self)
    }

    /// Per-process listeners are only kept while some input device runs, so a call app that starts the mic
    /// while another app already holds it is still noticed.
    private func syncProcessListeners(watch: Bool) {
        guard #available(macOS 14, *) else { return }
        let now = watch ? Set(processObjects()) : []
        for (p, l) in processListeners where !now.contains(p) {
            CA.unlisten(p, kAudioProcessPropertyIsRunningInput, queue: audioQueue, l)
            processListeners.removeValue(forKey: p)
        }
        for p in now where processListeners[p] == nil {
            processListeners[p] = CA.listen(p, kAudioProcessPropertyIsRunningInput, queue: audioQueue) { [weak self] in
                self?.evaluate()
            }
        }
    }

    private func micUsers() -> [MicUser] {
        guard #available(macOS 14, *) else { return [] }
        return processObjects().compactMap { p in
            guard (CA.get(p, kAudioProcessPropertyIsRunningInput, default: UInt32(0)) ?? 0) != 0,
                  let pid = CA.get(p, kAudioProcessPropertyPID, default: pid_t(0)), pid != ownPID else { return nil }
            return MicUser(bundleID: CA.string(p, kAudioProcessPropertyBundleID) ?? "", pid: pid)
        }
    }

    private typealias RespFn = @convention(c) (pid_t) -> pid_t
    private static let responsibleFn: RespFn? = {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "responsibility_get_pid_responsible_for_pid") else { return nil }
        return unsafeBitCast(sym, to: RespFn.self)
    }()

    /// The app a process belongs to (helpers → their app), as (bundle ID, name).
    private static func owner(of u: MicUser) -> (String, String) {
        let rpid = responsibleFn.map { $0(u.pid) } ?? u.pid
        if rpid > 0, let app = NSRunningApplication(processIdentifier: rpid), let b = app.bundleIdentifier {
            return (b, app.localizedName ?? b)
        }
        let app = NSRunningApplication(processIdentifier: u.pid)
        let b = u.bundleID.isEmpty ? (app?.bundleIdentifier ?? "") : u.bundleID
        return (b, app?.localizedName ?? b)
    }

    static func matches(_ bundleID: String, _ list: [String]) -> Bool {
        let id = bundleID.lowercased()
        guard !id.isEmpty else { return false }
        return list.contains { e in
            let e = e.trimmingCharacters(in: .whitespaces).lowercased()
            return !e.isEmpty && (id == e || id.hasPrefix(e + "."))
        }
    }

    // MARK: Evaluation

    private func evaluate() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        let running = anyInputRunning()
        var result: [String]?
        if Self.hasProcessAPI {
            syncProcessListeners(watch: running && config.enabled)
            if config.enabled && running {
                var names: [String] = []
                for u in micUsers() {
                    let (bid, name) = Self.owner(of: u)
                    let listed = Self.matches(u.bundleID, config.apps) || Self.matches(bid, config.apps)
                    if (listed || !config.onlyListed) && !names.contains(name) { names.append(name) }
                }
                result = names.isEmpty ? nil : names
            }
        } else if config.enabled && running {
            result = ["Microphone"]
        }
        if last != .some(result) {
            last = .some(result)
            callLog.log("mic users: \(result?.joined(separator: ", ") ?? "none", privacy: .public)")
            onChange?(result)
        }
    }
}
