import CoreAudio
import Foundation
import os

private let volLog = Logger(subsystem: "com.perappvolume.app", category: "volume")
private let routeLog = Logger(subsystem: "com.perappvolume.app", category: "routes")

/// Every Core Audio call goes through this serial queue. coreaudiod can stall (driver install,
/// macOS bugs); when it does, only this queue waits — the menu bar UI stays responsive.
let audioQueue = DispatchQueue(label: "com.perappvolume.audio", qos: .userInitiated)

enum OutputMode: String, CaseIterable, Identifiable {
    case followSystem   // follow macOS default output (manual picks / newly connected devices)
    case customOrder    // always use the first available device in the user's priority list
    var id: String { rawValue }
    var title: String {
        switch self {
        case .followSystem: return "Newest connected device"
        case .customOrder:  return "Use custom priority order"
        }
    }
}

/// Keeps the virtual "Per-App Volume" device as the system default output and
/// forwards its mix to the real device the user wants to hear.
///
/// Threading: the `@Published` properties are main-thread snapshots for the UI. The routing logic
/// and its state (`q…` members) live on `audioQueue`. UI actions are forwarded there asynchronously.
final class Router: ObservableObject {
    enum State: Equatable { case driverMissing, idle, running, error(String) }

    // MARK: UI snapshot (main thread)
    @Published private(set) var state: State = .idle
    @Published private(set) var devices: [AudioDevice] = []
    @Published private(set) var target: AudioDevice?
    @Published var mode: OutputMode {
        didSet {
            UserDefaults.standard.set(mode.rawValue, forKey: "outputMode")
            let m = mode
            audioQueue.async { self.qMode = m; self.reevaluate(reason: .modeChanged) }
        }
    }
    /// Most recently chosen first (follow mode), or user-defined order (custom mode).
    @Published private(set) var priority: [String]
    @Published private(set) var deviceNames: [String: String]
    @Published private(set) var masterVolume: Float = 1
    @Published private(set) var masterMuted = false

    // Call mode: while a calling app uses the mic, the macOS default output is the real device itself, so
    // voice processing (echo cancellation) works. Per-app volumes don't apply to the call meanwhile.
    @Published var callModeEnabled: Bool { didSet { saveCallConfig() } }
    @Published var callAppsOnly: Bool { didSet { saveCallConfig() } }
    @Published var callApps: [String] { didSet { saveCallConfig() } }
    /// Calling apps using the mic right now (nil: no call).
    @Published private(set) var callUsers: [String]?
    /// The default output is currently the real device because of a call.
    @Published private(set) var inCall = false
    /// The installed driver can send apps to different outputs (drivers before 1.4 can't).
    @Published private(set) var routingSupported = true
    /// UIDs of connected headphones that another device is playing through (see `DeviceSwitching`).
    @Published private(set) var elsewhere: Set<String> = []

    var realDevices: [AudioDevice] { devices.filter { !$0.isVirtualRouter } }

    // MARK: Logic state (audioQueue only)
    private var qMode: OutputMode
    private var qPriority: [String]
    private var qDeviceNames: [String: String]
    private var qDevices: [AudioDevice] = []
    private var qTarget: AudioDevice?
    private var qVirtual: AudioObjectID?
    private var qRealDevices: [AudioDevice] { qDevices.filter { !$0.isVirtualRouter } }
    private var lastDeviceListChange = Date.distantPast
    private var listeners: [(AudioObjectID, AudioObjectPropertySelector, AudioObjectPropertyScope, AudioObjectPropertyListenerBlock)] = []
    private var rateListener: (AudioObjectID, AudioObjectPropertyListenerBlock)?
    /// Output was handed to the real device on purpose (the driver is being removed): don't take it back.
    private var qReleased = false
    /// What we last told the driver to report as its latency.
    private var lastLatency: (virt: AudioObjectID, frames: Int)?
    private var stopWork: DispatchWorkItem?
    private var napBlocker: NSObjectProtocol?
    /// Real output devices seen so far (nil until the first scan), to detect newly plugged-in ones.
    private var knownUIDs: Set<String>?

    private enum CallState: Equatable {
        case none
        case routing(AudioObjectID)   // we made this real device the default output (0 = not set yet)
        case overridden               // the user changed the default output during the call; leave it alone
    }
    private var qCallState: CallState = .none
    private var callEndWork: DispatchWorkItem?
    private var mic: MicMonitor!
    /// How long the mic must stay off before the virtual device becomes the default again.
    private let callEndDelay: TimeInterval = 4

    // Per-app outputs (see AppRouting and PAVProtocol.h).
    /// Driver table key → UID of the device that app should play on ("" = the main output). From AppStore.
    private var qRouteWants: [String: String] = [:]
    /// Bus → the device it plays on.
    private var qBuses: [Int: AudioDevice] = [:]
    /// Devices a bus couldn't be started on; their apps play on the main output until the device list changes.
    private var qBusFailed: Set<String> = []
    private var busRateListeners: [Int: (dev: AudioObjectID, listener: AudioObjectPropertyListenerBlock)] = [:]
    /// What the driver was last told, so it is only told again when something changes.
    private var sentRoutes: (virt: AudioObjectID, surfaces: [Int64], table: [String: Int])?
    private var qRoutingSupported: (virt: AudioObjectID, yes: Bool)?

    // Headphones shared with other devices (see DeviceSwitching).
    /// Connected headphones another device has: automatic choices leave them alone until they are ours
    /// again, reconnect, or the user picks them.
    private var qElsewhere: Set<String> = []
    /// UID → since when we have been playing to headphones we don't have.
    private var unownedSince: [String: Date] = [:]
    private var ownershipListeners: [AudioObjectID: AudioObjectPropertyListenerBlock] = [:]
    private var knownBluetooth: Set<AudioObjectID> = []
    private var ownershipCheck: DispatchWorkItem?
    /// The real devices automatic choices may use.
    private var qUsableDevices: [AudioDevice] {
        let real = qRealDevices
        let uids = Set(DeviceSwitching.usable(connected: real.map(\.uid), elsewhere: qElsewhere))
        return real.filter { uids.contains($0.uid) }
    }

    init() {
        let d = UserDefaults.standard
        let m = OutputMode(rawValue: d.string(forKey: "outputMode") ?? "") ?? .followSystem
        let p = d.stringArray(forKey: "devicePriority") ?? []
        let n = (d.dictionary(forKey: "deviceNames") as? [String: String]) ?? [:]
        mode = m; priority = p; deviceNames = n
        qMode = m; qPriority = p; qDeviceNames = n
        let ce = d.object(forKey: "callModeEnabled") as? Bool ?? true
        let co = d.object(forKey: "callAppsOnly") as? Bool ?? true
        let ca = d.stringArray(forKey: "callApps") ?? kDefaultCallApps
        callModeEnabled = ce; callAppsOnly = co; callApps = ca
        mic = MicMonitor(config: .init(enabled: ce, onlyListed: co, apps: ca))
        mic.onChange = { [weak self] users in self?.micChanged(users) }
        mic.isOurOutput = { [weak self] id in
            (self?.qTarget?.id == id && PAVEngineIsRunning()) || (1...kPAVMaxBuses).contains { PAVBusOutput($0) == id }
        }
    }

    private func saveCallConfig() {
        let d = UserDefaults.standard
        d.set(callModeEnabled, forKey: "callModeEnabled")
        d.set(callAppsOnly, forKey: "callAppsOnly")
        d.set(callApps, forKey: "callApps")
        let c = MicMonitor.Config(enabled: callModeEnabled, onlyListed: callAppsOnly, apps: callApps)
        audioQueue.async { self.mic.update(c) }
    }

    /// The virtual device's id. Only valid on `audioQueue`.
    var virtualID: AudioObjectID? {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        return qVirtual
    }

    // MARK: Public API (call from main)

    func start() {
        audioQueue.async { [self] in
            addSystemListeners()
            reevaluate(reason: .launch)
            mic.start()
        }
    }

    private func addSystemListeners() {
        let sys = AudioObjectID(kAudioObjectSystemObject)
        addListener(sys, kAudioHardwarePropertyDevices) { [weak self] in
            self?.lastDeviceListChange = Date()
            self?.reevaluate(reason: .devicesChanged)
        }
        addListener(sys, kAudioHardwarePropertyDefaultOutputDevice) { [weak self] in
            self?.reevaluate(reason: .defaultChanged)
        }
        addListener(sys, kAudioHardwarePropertyServiceRestarted) { [weak self] in
            self?.serviceRestarted()
        }
    }

    /// coreaudiod restarted (driver update, crash). Object IDs, listeners and everything the driver held (the
    /// shared ring, the gain table, the latency) are gone: set it all up again.
    private func serviceRestarted() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        volLog.log("coreaudiod restarted: setting up again")
        PAVEngineStop(); setPlaying(false)
        stopBuses(tellDriver: false)   // the driver is a new instance: it has no routes and no rings
        qRoutingSupported = nil
        stopWork?.cancel(); stopWork = nil
        for (obj, sel, scope, l) in listeners { CA.unlisten(obj, sel, scope: scope, queue: audioQueue, l) }
        listeners.removeAll()
        if let (dev, l) = rateListener { CA.unlisten(dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l) }
        rateListener = nil
        if let w = earWatch { CA.unlisten(w.id, CA.kPropInEarStatus, queue: audioQueue, w.listener) }
        earWatch = nil
        dropOwnershipListeners()
        arrivalCheck?.cancel(); arrivalCheck = nil; arrival = nil
        mic.stop()
        watchedVirtual = nil
        lastLatency = nil
        addSystemListeners()
        lastDeviceListChange = Date()
        reevaluate(reason: .devicesChanged)
        mic.start()
    }

    /// Restore the real device as macOS default (called on quit). Waits at most `timeout` for coreaudiod.
    func shutdown(timeout: TimeInterval = 2) {
        let done = DispatchSemaphore(value: 0)
        audioQueue.async {
            PAVEngineStop()
            self.stopBuses(tellDriver: true)
            self.mic.stop()
            self.callEndWork?.cancel()
            for (obj, sel, scope, l) in self.listeners { CA.unlisten(obj, sel, scope: scope, queue: audioQueue, l) }
            self.listeners.removeAll()
            self.dropOwnershipListeners()
            if let (dev, l) = self.rateListener { CA.unlisten(dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l) }
            if let t = self.qTarget, let id = CA.device(uid: t.uid) {
                // Volume first, then the switch: the device must not be the default while still at full volume.
                if !self.volumeHandedBack, self.handBackVolume(to: t) { self.volumeHandedBack = true }
                if let v = self.qVirtual {
                    if CA.defaultOutput == v { CA.setDefaultOutput(id) }
                    if CA.defaultSystemOutput == v { CA.setDefaultSystemOutput(id) }
                }
            }
            done.signal()
        }
        _ = done.wait(timeout: .now() + timeout)
    }

    /// Re-scan devices (e.g. after the driver was installed and coreaudiod restarted).
    func rescan() { audioQueue.async { self.reevaluate(reason: .devicesChanged) } }

    /// Stop forwarding and make the real device the macOS default again (before uninstalling the driver).
    /// Stays that way until the driver is gone or `reclaimOutput()` is called: without that, our own
    /// default-output listener would see the real device become the default and route it straight back.
    func releaseOutput(timeout: TimeInterval = 3) {
        let done = DispatchSemaphore(value: 0)
        audioQueue.async {
            self.qReleased = true
            self.stopWork?.cancel(); self.stopWork = nil
            PAVEngineStop(); self.setPlaying(false)
            self.stopBuses(tellDriver: true)
            self.publishState(.idle)
            if let t = self.qTarget, CA.device(uid: t.uid) != nil {
                if !self.volumeHandedBack, self.handBackVolume(to: t) { self.volumeHandedBack = true }
                CA.setDefaultOutput(t.id)
                CA.setDefaultSystemOutput(t.id)
            }
            done.signal()
        }
        _ = done.wait(timeout: .now() + timeout)
    }

    /// The driver stays after all (uninstall cancelled or failed): route through it again.
    func reclaimOutput() {
        audioQueue.async {
            guard self.qReleased else { return }
            self.qReleased = false
            self.reevaluate(reason: .modeChanged)
        }
    }

    func choose(_ device: AudioDevice) {
        target = device   // optimistic, so the menu checkmark moves immediately
        audioQueue.async {
            self.forgetElsewhere(device.uid)
            self.promote(device.uid)
            self.applyTarget(device)
            self.publishPriority()
        }
    }

    /// The user wants an app on this device: if another device had it, ask for it again.
    func chosenForApp(uid: String) {
        audioQueue.async {
            guard self.qElsewhere.contains(uid) else { return }
            self.forgetElsewhere(uid)
            self.syncRoutes()
            self.publishDevices()
        }
    }

    func movePriority(from: Int, to: Int) {
        audioQueue.async {
            guard self.qPriority.indices.contains(from), to >= 0, to < self.qPriority.count else { return }
            let item = self.qPriority.remove(at: from)
            self.qPriority.insert(item, at: to)
            self.publishPriority()
            self.reevaluate(reason: .modeChanged)
        }
    }

    func removePriority(at index: Int) {
        audioQueue.async {
            guard self.qPriority.indices.contains(index) else { return }
            self.qPriority.remove(at: index)
            self.publishPriority()
        }
    }

    func setMasterVolume(_ v: Float) {
        let v = max(0, min(1, v))
        masterVolume = v
        audioQueue.async {
            guard let virt = self.qVirtual else { return }
            CA.set(virt, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput, Float32(v))
        }
    }

    func setMasterMuted(_ m: Bool) {
        masterMuted = m
        audioQueue.async {
            guard let virt = self.qVirtual else { return }
            CA.set(virt, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput, UInt32(m ? 1 : 0))
        }
    }

    // MARK: Publishing snapshots to the UI

    private func onMain(_ f: @escaping () -> Void) { DispatchQueue.main.async(execute: f) }

    private func publishState(_ s: State) { onMain { if self.state != s { self.state = s } } }

    private func publishPriority() {
        let p = qPriority
        UserDefaults.standard.set(p, forKey: "devicePriority")
        onMain { if self.priority != p { self.priority = p } }
    }

    private func publishDevices() {
        let d = qDevices, t = qTarget, n = qDeviceNames, e = qElsewhere
        UserDefaults.standard.set(n, forKey: "deviceNames")
        onMain {
            if self.elsewhere != e { self.elsewhere = e }
            if self.devices != d { self.devices = d }
            if self.target != t { self.target = t }
            if self.deviceNames != n { self.deviceNames = n }
        }
    }

    // MARK: Logic (audioQueue)

    private enum Reason { case launch, devicesChanged, defaultChanged, modeChanged }

    private func addListener(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector,
                             scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ block: @escaping () -> Void) {
        let l = CA.listen(obj, sel, scope: scope, queue: audioQueue, block)
        listeners.append((obj, sel, scope, l))
    }

    private func dropVirtualListeners() {
        guard let old = watchedVirtual else { return }
        for (obj, sel, scope, l) in listeners where obj == old { CA.unlisten(obj, sel, scope: scope, queue: audioQueue, l) }
        listeners.removeAll { $0.0 == old }
        watchedVirtual = nil
    }

    private func setPlaying(_ on: Bool) {
        if on, napBlocker == nil {
            // Keeps App Nap from throttling us while audio plays.
            napBlocker = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical], reason: "Forwarding audio")
        } else if !on, let b = napBlocker {
            ProcessInfo.processInfo.endActivity(b); napBlocker = nil
        }
    }

    private func promote(_ uid: String) {
        if qMode == .followSystem {
            qPriority.removeAll { $0 == uid }
            qPriority.insert(uid, at: 0)
        } else if !qPriority.contains(uid) {
            qPriority.append(uid)
        }
    }

    private func reevaluate(reason: Reason) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        defer { syncRoutes(); publishDevices(); publishPriority() }
        qDevices = CA.allOutputDevices()
        refreshOwnership()
        if reason == .devicesChanged || reason == .launch { qBusFailed.removeAll() }
        let historyBefore = qPriority   // MRU history before newly seen devices get appended
        for d in qRealDevices where qDeviceNames[d.uid] != d.name { qDeviceNames[d.uid] = d.name }
        for d in qRealDevices where !qPriority.contains(d.uid) { qPriority.append(d.uid) }

        guard let virt = qDevices.first(where: { $0.isVirtualRouter }) else {
            // Virtual device gone (driver removed / coreaudiod restarting): give the real device its level back
            // right away, otherwise it would play at the full volume we set while forwarding.
            // The virtual device's ID is dead (or already belongs to another device), so forget it first: the
            // level to restore is the one we cached, not whatever that ID answers now.
            qVirtual = nil
            dropVirtualListeners()
            qReleased = false
            PAVEngineStop(); setPlaying(false)
            publishState(.driverMissing)
            if !volumeHandedBack, let t = qTarget {
                // If the device isn't back yet (coreaudiod still starting), keep it as the target and try
                // again on the next device change instead of leaving it at full volume.
                guard handBackVolume(to: t) else { return }
                volumeHandedBack = true
            }
            let def = CA.defaultOutput
            qTarget = qRealDevices.first { $0.id == def }
            return
        }
        let virtChanged = watchedVirtual != virt.id
        qVirtual = virt.id

        // During a call the default output is a real device we chose. Someone else changing it (not macOS
        // reacting to a device being plugged in or out) is the user's choice: respect it until the call ends.
        if reason == .defaultChanged, case .routing(let id) = qCallState, id != 0, CA.defaultOutput != id,
           Date().timeIntervalSince(lastDeviceListChange) >= 1.5 {
            callLog.log("default output changed by the user during the call; leaving it alone")
            qCallState = .overridden
            publishCall()
        }
        if virtChanged {
            dropVirtualListeners()   // the ones on the previous ID (before a coreaudiod restart)
            watchedVirtual = virt.id
            lastLatency = nil
            watchVirtualRunning(virt.id); watchVirtualVolume(virt.id); watchVirtualRate(virt.id); refreshCachedMaster()
        }
        if qReleased {
            let def = CA.defaultOutput
            if let d = qRealDevices.first(where: { $0.id == def }) { qTarget = d }
            return
        }

        // A device connected (headphones, DAC, Bluetooth, display…). Since our virtual device is the macOS default,
        // macOS won't switch by itself, so apply the rules here (see DeviceSwitching).
        // Headphones another device has count as not connected; they "connect" when the Mac gets them.
        let usable = qUsableDevices
        let currentUIDs = Set(usable.map(\.uid))
        defer { knownUIDs = currentUIDs }
        defer { resumeAfterEarIfNeeded() }
        if qMode == .followSystem, reason == .devicesChanged, let known = knownUIDs {
            let appeared = usable
                .filter { !known.contains($0.uid) && DeviceSwitching.canTakeOverOnConnect(transport: $0.transport) }
                .map(\.uid)
            if let uid = DeviceSwitching.deviceToSwitchTo(appeared: appeared, history: historyBefore),
               let dev = qRealDevices.first(where: { $0.uid == uid }) {
                let front = DeviceSwitching.order(afterConnecting: appeared, history: historyBefore).filter(appeared.contains)
                qPriority = front + qPriority.filter { !front.contains($0) }
                applyTarget(dev)
                return
            }
        }

        let def = CA.defaultOutput
        let routingCall: Bool = { if case .routing = qCallState { return true } else { return false } }()
        if def != virt.id, !routingCall, let picked = qRealDevices.first(where: { $0.id == def }) {
            // The user (or macOS, e.g. headphones connected) selected a real device.
            // In custom-order mode, ignore macOS' automatic switch on connect and fall through to the priority list.
            let autoSwitch = Date().timeIntervalSince(lastDeviceListChange) < 1.5
            if qMode == .followSystem || (!autoSwitch && reason != .launch) {
                forgetElsewhere(picked.uid)
                promote(picked.uid)
                applyTarget(picked)
                return
            }
        }

        // Pick by priority.
        let available = Dictionary(usable.map { ($0.uid, $0) }, uniquingKeysWith: { a, _ in a })
        var next: AudioDevice?
        if qMode == .customOrder || qTarget == nil || available[qTarget!.uid] == nil {
            // Current output gone (or custom order): most recently used / highest priority device still connected.
            next = DeviceSwitching.fallback(history: qPriority, connected: usable.map(\.uid)).flatMap { available[$0] }
        } else {
            next = available[qTarget!.uid]
        }
        if let n = next { applyTarget(n) } else {
            PAVEngineStop(); setPlaying(false)
            qTarget = nil
            publishState(.error("No output device available"))
        }
    }

    private func applyTarget(_ device: AudioDevice) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        guard let virt = qVirtual, !qReleased else { return }
        let changed = qTarget?.id != device.id
        // Which device should be the macOS default output (nil: leave it as the user set it).
        let wantDefault: AudioObjectID?
        switch qCallState {
        case .none: wantDefault = virt
        case .routing: wantDefault = device.id
        case .overridden: wantDefault = nil
        }
        // Apps play straight to a real device (call): it keeps its own volume instead of running at full volume.
        let direct = (wantDefault ?? CA.defaultOutput) != virt
        if changed, let old = qTarget, !volumeHandedBack { handBackVolume(to: old); volumeHandedBack = true }
        // A bus plays at its device's own volume, the main mix on a device at full volume. So between the
        // old device getting its level back (above) and the new one going to full volume (below): apps that
        // had the new device as their own output join the main mix and that bus stops, and apps whose
        // output is the old device get a bus there.
        if changed { syncRoutes(main: device) }
        qTarget = device
        watchEars(device)
        if direct {
            if !volumeHandedBack { handBackVolume(to: device) }
            volumeHandedBack = true
        } else if changed || volumeHandedBack {
            takeOverVolume(from: device); volumeHandedBack = false
            expectArrivalLevel()
        }
        publishDevices()

        // Match sample rates so nothing has to be resampled, and report the real path's latency.
        syncRate()
        if !direct { syncLatency() }
        if let (dev, l) = rateListener, dev != device.id || changed {
            CA.unlisten(dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l)
            rateListener = nil
        }
        if rateListener == nil {
            let l = CA.listen(device.id, kAudioDevicePropertyNominalSampleRate, queue: audioQueue) { [weak self] in
                self?.syncRate()
                self?.syncLatency()
                self?.restartEngineIfNeeded(force: true)
            }
            rateListener = (device.id, l)
        }

        if let want = wantDefault, CA.defaultOutput != want {
            callLog.log("default output -> \(want == virt ? "Per-App Volume" : device.name, privacy: .public)")
            CA.setDefaultOutput(want)
        }
        if case .routing = qCallState { qCallState = .routing(device.id) }
        if qCallState == .none, CA.defaultSystemOutput != virt { CA.setDefaultSystemOutput(virt) }
        publishCall()
        restartEngineIfNeeded(force: changed || !PAVEngineIsRunning())
        syncRoutes()
    }

    // MARK: Per-app outputs (audioQueue)
    // An app with an output of its own is taken out of the main mix by the driver and mixed into a "bus":
    // a second ring, which a second engine plays on that device. Buses don't go through the master volume
    // (that one stands for the main device's volume, see "Volume hand-off"); their device keeps its own.

    /// Where every app should play: driver table key ("<pid>" or "b:<bundle id>") → device UID ("" = the
    /// main output). Only on `audioQueue`.
    func setRouteWants(_ wants: [String: String]) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        guard wants != qRouteWants || sentRoutes == nil else { return }
        qRouteWants = wants
        syncRoutes()
    }

    /// The bus the driver was last given for a table key. Only on `audioQueue`.
    func sentBus(_ key: String) -> Int? {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        return sentRoutes?.table[key]
    }

    /// The driver doesn't have what it was sent (it restarted): send it again. Only on `audioQueue`.
    func resendRoutes() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        sentRoutes = nil
        syncRoutes()
    }

    private func routingSupported(_ virt: AudioObjectID) -> Bool {
        if let s = qRoutingSupported, s.virt == virt { return s.yes }
        let yes = CA.has(virt, kPropRoutes)
        qRoutingSupported = (virt, yes)
        if !yes { routeLog.log("the installed driver has no per-app outputs") }
        onMain { if self.routingSupported != yes { self.routingSupported = yes } }
        return yes
    }

    /// Works out which app plays on which bus, tells the driver, and starts and stops the buses.
    /// `main`: the main output, when it is about to change.
    private func syncRoutes(main: AudioDevice? = nil) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        guard let virt = qVirtual, !qReleased, routingSupported(virt) else { stopBuses(tellDriver: false); return }
        let devices = Dictionary(qRealDevices.map { ($0.uid, $0) }, uniquingKeysWith: { a, _ in a })
        let plan = AppRouting.plan(wants: qRouteWants, connected: Set(devices.keys).subtracting(qBusFailed).subtracting(qElsewhere),
                                   main: (main ?? qTarget)?.uid, current: qBuses.mapValues(\.uid), maxBuses: Int(kPAVMaxBuses))
        var buses: [Int: AudioDevice] = [:]
        var surfaces = [Int64](repeating: 0, count: Int(kPAVMaxBuses))
        for (bus, uid) in plan.buses {
            guard let dev = devices[uid] else { continue }
            let id = PAVBusSurfaceID(Int32(bus))
            guard id != 0 else { continue }   // no ring: the driver keeps those apps in the main mix
            buses[bus] = dev
            surfaces[bus - 1] = id
        }
        // The driver first, the engines after: apps leaving a bus are in the main mix before that bus stops.
        if sentRoutes == nil || sentRoutes!.virt != virt || sentRoutes!.surfaces != surfaces || sentRoutes!.table != plan.table {
            let st = CA.setPlist(virt, kPropRoutes, Self.routesPlist(surfaces: surfaces, table: plan.table))
            if st == noErr {
                sentRoutes = (virt, surfaces, plan.table)
                let names = buses.sorted { $0.key < $1.key }.map { "\($0.key): \($0.value.name)" }.joined(separator: ", ")
                routeLog.log("outputs -> \(names.isEmpty ? "main only" : names, privacy: .public)")
            } else {
                routeLog.error("couldn't send the routes to the driver (\(st))")
            }
        }
        qBuses = buses
        syncBusEngines()
    }

    private static func routesPlist(surfaces: [Int64], table: [String: Int]) -> NSDictionary {
        [kPAVRoutesKeySurfaces: surfaces.map { NSNumber(value: $0) },
         kPAVRoutesKeyRoutes: table.mapValues { NSNumber(value: $0) }] as NSDictionary
    }

    /// Buses play while the virtual device is in use, like the main output does.
    private func syncBusEngines() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        let run = qVirtual != nil && !qReleased && (virtualIsRunning() || PAVEngineIsRunning())
        var changed = false
        for bus in 1...Int(kPAVMaxBuses) {
            let b = Int32(bus)
            let dev = run ? qBuses[bus] : nil
            if let l = busRateListeners[bus], l.dev != dev?.id {
                CA.unlisten(l.dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l.listener)
                busRateListeners[bus] = nil
            }
            guard let dev, let virt = qVirtual else {
                if PAVBusIsRunning(b) { PAVBusStop(b); changed = true }
                continue
            }
            if busRateListeners[bus] == nil {
                let l = CA.listen(dev.id, kAudioDevicePropertyNominalSampleRate, queue: audioQueue) { [weak self] in
                    PAVBusStop(b)             // it reads the device's rate when it starts
                    self?.syncBusEngines()
                }
                busRateListeners[bus] = (dev.id, l)
            }
            // Also restarted when the virtual device changed its rate (it follows the main output's).
            guard !PAVBusIsRunning(b) || PAVBusOutput(b) != dev.id || abs(PAVBusSourceRate(b) - CA.sampleRate(virt)) > 0.5 else { continue }
            let st = PAVBusStart(b, virt, dev.id)
            changed = true
            if st == noErr {
                routeLog.log("bus \(bus) playing on \(dev.name, privacy: .public)")
            } else {
                // Its apps would be silent: treat the device as not connected, so they play on the main output.
                routeLog.error("couldn't open \(dev.name, privacy: .public) for bus \(bus) (\(st))")
                qBusFailed.insert(dev.uid)
                audioQueue.async { [weak self] in self?.syncRoutes() }
            }
        }
        if changed, !MicMonitor.hasProcessAPI { audioQueue.async { [weak self] in self?.mic.refresh() } }
        checkOwnership()
    }

    /// Stops every bus. `tellDriver`: also put every app back in the main mix (when we stop playing the
    /// buses but the driver stays).
    private func stopBuses(tellDriver: Bool) {
        for bus in 1...Int(kPAVMaxBuses) { PAVBusStop(Int32(bus)) }
        for (_, l) in busRateListeners { CA.unlisten(l.dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l.listener) }
        busRateListeners.removeAll()
        qBuses.removeAll()
        if tellDriver, sentRoutes != nil, let virt = qVirtual {
            CA.setPlist(virt, kPropRoutes, Self.routesPlist(surfaces: [Int64](repeating: 0, count: Int(kPAVMaxBuses)), table: [:]))
        }
        sentRoutes = nil
    }

    // MARK: Headphones shared with other devices (audioQueue)
    // AirPods that play from an iPhone are connected to the Mac too, and nothing we play to them is heard.
    // See DeviceSwitching for the rules; here: watch who has them, and time how long we play unheard.

    /// Follows which shared headphones are connected and who has them. Headphones that are ours again or
    /// that disconnected are no longer "elsewhere" (after reconnecting they get a new try).
    private func refreshOwnership() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        let real = qRealDevices
        let shared = real.filter { CA.has($0.id, CA.kPropOwnsAudio) }
        for (id, l) in ownershipListeners where !shared.contains(where: { $0.id == id }) {
            CA.unlisten(id, CA.kPropOwnsAudio, queue: audioQueue, l)
            ownershipListeners[id] = nil
        }
        for d in shared where ownershipListeners[d.id] == nil {
            ownershipListeners[d.id] = CA.listen(d.id, CA.kPropOwnsAudio, queue: audioQueue) { [weak self] in self?.ownershipChanged() }
        }
        for uid in qElsewhere {
            guard let d = shared.first(where: { $0.uid == uid }) else { qElsewhere.remove(uid); continue }
            if CA.ownsAudio(d.id) != false {
                routeLog.log("\(d.name, privacy: .public) plays for this Mac again")
                qElsewhere.remove(uid)
            }
        }
        unownedSince = unownedSince.filter { uid, _ in real.contains { $0.uid == uid } }

        // Headphones that just connected may not say yet whether they are shared: look once more.
        let bluetooth = Set(real.filter { [kAudioDeviceTransportTypeBluetooth, kAudioDeviceTransportTypeBluetoothLE].contains($0.transport) }.map(\.id))
        if !bluetooth.isSubset(of: knownBluetooth) {
            audioQueue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.ownershipChanged() }
        }
        knownBluetooth = bluetooth
    }

    private func dropOwnershipListeners() {
        for (id, l) in ownershipListeners { CA.unlisten(id, CA.kPropOwnsAudio, queue: audioQueue, l) }
        ownershipListeners.removeAll()
        knownBluetooth.removeAll()
        ownershipCheck?.cancel(); ownershipCheck = nil
        unownedSince.removeAll()
    }

    private func ownershipChanged() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        let before = qElsewhere
        refreshOwnership()
        // Headphones the Mac just got from another device start playing for us now.
        if let t = qTarget, CA.ownsAudio(t.id) == true { expectArrivalLevel() }
        if qElsewhere != before { reevaluate(reason: .devicesChanged) } else { checkOwnership() }
    }

    /// Chosen by hand (or reconnected): try it again.
    private func forgetElsewhere(_ uid: String) {
        qElsewhere.remove(uid)
        unownedSince[uid] = nil
    }

    /// Looks at the outputs we are playing to. Headphones the Mac still doesn't have after
    /// `DeviceSwitching.ownershipGrace` are left to the device that has them: the main output falls back
    /// like when a device disconnects, and apps with those headphones as their output play on the main one.
    private func checkOwnership() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        ownershipCheck?.cancel(); ownershipCheck = nil
        // During a call apps play straight to the device and macOS looks after it.
        guard qVirtual != nil, !qReleased, qCallState == .none else { unownedSince.removeAll(); return }
        var playing: [AudioDevice] = []
        if let t = qTarget, PAVEngineIsRunning() { playing.append(t) }
        for (bus, dev) in qBuses where PAVBusIsRunning(Int32(bus)) { playing.append(dev) }
        unownedSince = unownedSince.filter { uid, _ in playing.contains { $0.uid == uid } }

        let now = Date()
        var lost: [AudioDevice] = []
        var wait: TimeInterval?
        for dev in playing where !qElsewhere.contains(dev.uid) {
            let since = unownedSince[dev.uid] ?? now
            switch DeviceSwitching.ownership(owned: CA.ownsAudio(dev.id), unownedFor: now.timeIntervalSince(since)) {
            case .ours: unownedSince[dev.uid] = nil
            case .asking(let left): unownedSince[dev.uid] = since; wait = min(wait ?? left, left)
            case .elsewhere: lost.append(dev)
            }
        }
        for dev in lost {
            routeLog.log("\(dev.name, privacy: .public) is playing from another device: not using it")
            qElsewhere.insert(dev.uid)
            unownedSince[dev.uid] = nil
        }
        if !lost.isEmpty { audioQueue.async { [weak self] in self?.reevaluate(reason: .devicesChanged) } }
        if let wait {
            let w = DispatchWorkItem { [weak self] in self?.ownershipCheck = nil; self?.checkOwnership() }
            ownershipCheck = w
            audioQueue.asyncAfter(deadline: .now() + wait + 0.05, execute: w)
        }
    }

    // MARK: AirPods ear detection (audioQueue)
    // macOS pauses playback when AirPods leave your ears only if the default output *is* the AirPods
    // (mediaremoted logs "Not playing to device" otherwise). With our virtual device as the default nobody
    // pauses, and once both buds are out macOS hides the AirPods, so audio would jump to the speakers.
    // So watch the AirPods' in-ear state ourselves and pause / resume like macOS does.

    private var earWatch: (id: AudioObjectID, uid: String, inEar: Int, listener: AudioObjectPropertyListenerBlock)?
    /// We paused because buds were taken out; resume when as many are back in, within `earResumeWindow`.
    private var earPaused: (uid: String, inEarBefore: Int, at: Date)?
    private let earResumeWindow: TimeInterval = 60

    /// Follow the in-ear state of the current output (kept while the AirPods are hidden, i.e. out of the ears).
    private func watchEars(_ device: AudioDevice) {
        guard earWatch?.id != device.id, let n = CA.budsInEar(device.id) else { return }
        if let w = earWatch { CA.unlisten(w.id, CA.kPropInEarStatus, queue: audioQueue, w.listener) }
        let id = device.id
        let l = CA.listen(id, CA.kPropInEarStatus, queue: audioQueue) { [weak self] in self?.earsChanged(id) }
        earWatch = (id, device.uid, n, l)
    }

    private func earsChanged(_ id: AudioObjectID) {
        guard let w = earWatch, w.id == id, let n = CA.budsInEar(id) else { return }
        let before = w.inEar
        earWatch?.inEar = n
        guard n != before else { return }
        volLog.log("in-ear buds: \(before) -> \(n)")
        if n < before, earPaused == nil, qTarget?.id == id, qCallState == .none, virtualIsRunning() {
            volLog.log("AirPods taken out of the ears: pausing playback")
            NowPlaying.send(.pause)
            earPaused = (w.uid, before, Date())
        }
        resumeAfterEarIfNeeded()
    }

    private func resumeAfterEarIfNeeded() {
        guard let p = earPaused else { return }
        guard let w = earWatch, w.uid == p.uid, CA.string(w.id, kAudioDevicePropertyDeviceUID) == p.uid else {
            earPaused = nil   // disconnected (back in the case): don't resume later
            return
        }
        guard w.inEar >= p.inEarBefore, qTarget?.uid == p.uid, qRealDevices.contains(where: { $0.uid == p.uid }) else { return }
        earPaused = nil
        if Date().timeIntervalSince(p.at) < earResumeWindow {
            volLog.log("AirPods back in the ears: resuming playback")
            NowPlaying.send(.play)
        }
    }

    // MARK: Calls (audioQueue)

    private func publishCall() {
        let on = qCallState != .none && CA.defaultOutput != qVirtual
        onMain { if self.inCall != on { self.inCall = on } }
    }

    private func micChanged(_ users: [String]?) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        onMain { if self.callUsers != users { self.callUsers = users } }
        if users != nil {
            callEndWork?.cancel(); callEndWork = nil
            guard qCallState == .none, qVirtual != nil else { return }
            callLog.log("call started: \(users!.joined(separator: ", "), privacy: .public)")
            qCallState = .routing(0)
            if let t = qTarget { applyTarget(t) }
        } else if qCallState != .none, callEndWork == nil {
            // Debounce: apps briefly stop and restart the mic (device changes, mute, reconfiguration).
            let w = DispatchWorkItem { [weak self] in
                guard let self else { return }
                self.callEndWork = nil
                self.endCall()
            }
            callEndWork = w
            audioQueue.asyncAfter(deadline: .now() + callEndDelay, execute: w)
        }
    }

    private func endCall() {
        let s = qCallState
        qCallState = .none
        let def = CA.defaultOutput
        switch s {
        case .none: return
        case .routing:
            // Still the device we set (or one macOS picked when a device came or went): take it back.
            callLog.log("call ended; back to Per-App Volume")
            reevaluate(reason: .modeChanged)
        case .overridden:
            // Only take the output back if the user put it on Per-App Volume themselves.
            callLog.log("call ended; output was changed by the user, leaving it")
            if def == qVirtual { reevaluate(reason: .modeChanged) }
        }
        publishCall()
    }

    /// Run the virtual device at the real device's rate (or the closest one the driver offers; the engine
    /// converts the rest, see PAVEngine.c).
    private func syncRate() {
        guard let virt = qVirtual, let t = qTarget, !qReleased else { return }
        let r = CA.sampleRate(t.id)
        guard r > 0 else { return }
        let best = PAVNearestSampleRate(r)
        if abs(CA.sampleRate(virt) - best) > 0.5 { CA.set(virt, kAudioDevicePropertyNominalSampleRate, Float64(best)) }
    }

    /// Tell the driver how much latency sits behind the virtual device, so players keep picture and sound in
    /// sync as they would on the real device: the real device's own latency plus our buffer. The driver has to
    /// restart its IO to change it, so this is only sent when the output device or its rate changes.
    private func syncLatency() {
        guard let virt = qVirtual, let t = qTarget, !qReleased else { return }
        let devRate = CA.sampleRate(t.id)
        guard devRate > 0 else { return }
        let virtRate = PAVNearestSampleRate(devRate)
        let out = kAudioObjectPropertyScopeOutput
        var dev = Double(CA.get(t.id, kAudioDevicePropertyLatency, scope: out, default: UInt32(0)) ?? 0)
        dev += Double(CA.get(t.id, kAudioDevicePropertySafetyOffset, scope: out, default: UInt32(0)) ?? 0)
        dev += Double(CA.get(t.id, kAudioDevicePropertyBufferFrameSize, default: UInt32(512)) ?? 512)
        if let stream = CA.array(t.id, kAudioDevicePropertyStreams, scope: out, of: AudioObjectID.self).first {
            dev += Double(CA.get(stream, kAudioStreamPropertyLatency, default: UInt32(0)) ?? 0)
        }
        // Our buffer: one 512-frame write from the driver plus 3 ms (more while an app uses bigger writes).
        let frames = Int((dev / devRate * virtRate + 512 + 0.003 * virtRate).rounded())
        if let l = lastLatency, l.virt == virt, abs(l.frames - frames) < Int(virtRate * 0.002) { return }
        if CA.setPlist(virt, kPropLatency, NSNumber(value: frames)) == noErr {
            volLog.log("reported latency -> \(frames) frames")
            lastLatency = (virt, frames)
        }
    }

    /// The engine reads the virtual device's rate when it starts. The rate changes a moment after we ask for
    /// it, so restart the engine if it started with the old one.
    private func watchVirtualRate(_ virt: AudioObjectID) {
        addListener(virt, kAudioDevicePropertyNominalSampleRate) { [weak self] in
            guard let self, let v = self.qVirtual else { return }
            if PAVEngineIsRunning(), abs(CA.sampleRate(v) - PAVEngineSourceRate()) > 0.5 { self.restartEngineIfNeeded(force: true) }
            self.syncBusEngines()
        }
    }

    private func watchVirtualVolume(_ virt: AudioObjectID) {
        for sel in [kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute] {
            addListener(virt, sel, scope: kAudioObjectPropertyScopeOutput) { [weak self] in self?.refreshCachedMaster() }
        }
    }

    // Only keep the real device running while something actually plays, so the Mac can sleep.
    private func watchVirtualRunning(_ virt: AudioObjectID) {
        addListener(virt, kAudioDevicePropertyDeviceIsRunningSomewhere) { [weak self] in
            self?.restartEngineIfNeeded(force: false)
        }
    }

    private func virtualIsRunning() -> Bool {
        guard let v = qVirtual else { return false }
        return (CA.get(v, kAudioDevicePropertyDeviceIsRunningSomewhere, default: UInt32(0)) ?? 0) != 0
    }

    private func restartEngineIfNeeded(force: Bool) {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        defer { syncBusEngines() }
        guard let virt = qVirtual, let t = qTarget, !qReleased else { return }
        let wasRunning = PAVEngineIsRunning()
        // macOS 13: a device with inputs and outputs counts as "mic in use" or not depending on whether we
        // play to it (see MicMonitor), so look again when that changes.
        defer {
            if !MicMonitor.hasProcessAPI, PAVEngineIsRunning() != wasRunning { audioQueue.async { [weak self] in self?.mic.refresh() } }
        }
        if virtualIsRunning() {
            stopWork?.cancel(); stopWork = nil
            if force || !PAVEngineIsRunning() {
                expectArrivalLevel()   // AirPods report their level while the stream starts
                let st = PAVEngineStart(virt, t.id)
                checkArrivalLevel()
                publishState(st == noErr ? .running : .error("Couldn't open \(t.name) (\(st))"))
                setPlaying(st == noErr)
            }
        } else if PAVEngineIsRunning() {
            if force { PAVEngineStop(); setPlaying(false); publishState(.idle); return }
            guard stopWork == nil else { return }
            let w = DispatchWorkItem { [weak self] in
                guard let self else { return }
                self.stopWork = nil
                if !self.virtualIsRunning() {
                    PAVEngineStop(); self.setPlaying(false); self.publishState(.idle)
                    self.syncBusEngines()
                    if !MicMonitor.hasProcessAPI { self.mic.refresh() }
                }
            }
            stopWork = w
            audioQueue.asyncAfter(deadline: .now() + 3, execute: w)
        } else {
            publishState(.idle)
        }
    }

    // MARK: Volume hand-off
    // The real device runs at full volume while we forward to it; its previous level becomes the
    // virtual master volume (what the keyboard keys control), so loudness doesn't jump and 100% in
    // Per-App Volume means the device's real maximum. On quit / switch, the level is handed back.

    /// False while a real device runs at full volume because we took its level over. Starts true: nothing
    /// has been taken yet, so there is nothing to give back.
    private var volumeHandedBack = true
    /// The virtual device we have listeners on.
    private var watchedVirtual: AudioObjectID?
    /// Last known master level, also used when the virtual device has already disappeared.
    private var cachedMaster: (volume: Float, muted: Bool) = (1, false)

    private func refreshCachedMaster() {
        // Only a level we could actually read replaces the cached one: if the device is gone, the cache is
        // exactly what must be handed back.
        guard let v = qVirtual,
              let vol = CA.get(v, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput, default: Float32(1))
        else { return }
        let mute = CA.get(v, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput, default: UInt32(0)).map { $0 != 0 }
            ?? cachedMaster.muted
        cachedMaster = (vol, mute)
        onMain {
            if abs(self.masterVolume - vol) > 0.001 { self.masterVolume = vol }
            if self.masterMuted != mute { self.masterMuted = mute }
        }
    }

    private func takeOverVolume(from device: AudioDevice) {
        guard let virt = qVirtual, let hw = CA.hardwareVolume(device.id) else {
            volLog.log("takeOver \(device.name, privacy: .public): no hardware volume")
            return
        }
        volLog.log("takeOver \(device.name, privacy: .public) hw=\(hw)")
        if hw < 0.999 {
            CA.set(virt, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput, Float32(hw))
            CA.setHardwareVolume(device.id, 1)
        }
        if CA.hardwareMute(device.id) == true {
            CA.set(virt, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput, UInt32(1))
            CA.setHardwareMute(device.id, false)
        }
        refreshCachedMaster()
    }

    // The level a device arrives with (see DeviceSwitching): headphones report it after we took their volume
    // over, and macOS sets the device to it. Left there it would apply on top of the master volume, and the
    // volume keys couldn't make up for it.

    /// The output we expect a level from: what it was at when the window opened, and until when.
    private var arrival: (dev: AudioObjectID, level: Float, until: Date)?
    private var arrivalCheck: DispatchWorkItem?

    /// Call when the output's volume was taken over, before it starts playing, and when the Mac gets it
    /// from another device.
    private func expectArrivalLevel() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        checkArrivalLevel()   // a window that is still open: don't lose what it would have seen
        arrivalCheck?.cancel(); arrivalCheck = nil
        guard !volumeHandedBack, let t = qTarget, let hw = CA.hardwareVolume(t.id) else { arrival = nil; return }
        arrival = (t.id, hw, Date().addingTimeInterval(DeviceSwitching.arrivalWindow))
        scheduleArrivalCheck()
    }

    /// The Bluetooth HAL doesn't reliably notify listeners of a level the headphones set, so look.
    private func scheduleArrivalCheck() {
        let w = DispatchWorkItem { [weak self] in self?.arrivalCheck = nil; self?.checkArrivalLevel() }
        arrivalCheck = w
        audioQueue.asyncAfter(deadline: .now() + 0.25, execute: w)
    }

    private func checkArrivalLevel() {
        dispatchPrecondition(condition: .onQueue(audioQueue))
        guard let a = arrival else { return }
        arrivalCheck?.cancel(); arrivalCheck = nil
        // Not while apps play straight to the device (a call) or after it got its level back.
        guard !volumeHandedBack, let t = qTarget, t.id == a.dev, let hw = CA.hardwareVolume(t.id) else { arrival = nil; return }
        if DeviceSwitching.arrivalLevel(before: a.level, now: hw) != nil {
            volLog.log("\(t.name, privacy: .public) arrived at its own level")
            arrival = nil
            takeOverVolume(from: t)
        } else if Date() < a.until {
            scheduleArrivalCheck()
        } else {
            arrival = nil
        }
    }

    /// Gives the device its level back. False if the device can't be reached right now (try again later).
    @discardableResult
    private func handBackVolume(to device: AudioDevice) -> Bool {
        guard let id = CA.device(uid: device.uid) else { return false }
        guard CA.hardwareVolume(id) != nil else { return true }   // no volume control: nothing was taken
        refreshCachedMaster()
        volLog.log("handBack \(device.name, privacy: .public) -> \(self.cachedMaster.volume)")
        CA.setHardwareVolume(id, cachedMaster.volume)
        if cachedMaster.muted { CA.setHardwareMute(id, true) }
        return true
    }
}

/// Play/pause the system's Now Playing app (MediaRemote, the same path as the keyboard media keys).
enum NowPlaying {
    enum Command: UInt32 { case play = 0, pause = 1 }
    private typealias SendFn = @convention(c) (UInt32, CFDictionary?) -> Bool
    private static let sendFn: SendFn? = {
        guard let h = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_LAZY),
              let sym = dlsym(h, "MRMediaRemoteSendCommand") else { return nil }
        return unsafeBitCast(sym, to: SendFn.self)
    }()
    static func send(_ c: Command) { _ = sendFn?(c.rawValue, nil) }
}
