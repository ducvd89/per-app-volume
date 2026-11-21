import CoreAudio
import Foundation
import os

private let volLog = Logger(subsystem: "com.perappvolume.app", category: "volume")

/// Every Core Audio call goes through this serial queue. coreaudiod can stall (driver install,
/// macOS bugs); when it does, only this queue waits — the menu bar UI stays responsive.
let audioQueue = DispatchQueue(label: "com.perappvolume.audio", qos: .userInitiated)

enum OutputMode: String, CaseIterable, Identifiable {
    case followSystem   // follow macOS default output (manual picks / newly connected devices)
    case customOrder    // always use the first available device in the user's priority list
    var id: String { rawValue }
    var title: String {
        switch self {
        case .followSystem: return "Follow macOS output device"
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
        mic.isOurOutput = { [weak self] id in self?.qTarget?.id == id && PAVEngineIsRunning() }
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
        audioQueue.async {
            let sys = AudioObjectID(kAudioObjectSystemObject)
            self.addListener(sys, kAudioHardwarePropertyDevices) { [weak self] in
                self?.lastDeviceListChange = Date()
                self?.reevaluate(reason: .devicesChanged)
            }
            self.addListener(sys, kAudioHardwarePropertyDefaultOutputDevice) { [weak self] in
                self?.reevaluate(reason: .defaultChanged)
            }
            self.reevaluate(reason: .launch)
            self.mic.start()
        }
    }

    /// Restore the real device as macOS default (called on quit). Waits at most `timeout` for coreaudiod.
    func shutdown(timeout: TimeInterval = 2) {
        let done = DispatchSemaphore(value: 0)
        audioQueue.async {
            PAVEngineStop()
            self.mic.stop()
            self.callEndWork?.cancel()
            for (obj, sel, scope, l) in self.listeners { CA.unlisten(obj, sel, scope: scope, queue: audioQueue, l) }
            self.listeners.removeAll()
            if let (dev, l) = self.rateListener { CA.unlisten(dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l) }
            if let v = self.qVirtual, let t = self.qTarget, CA.device(uid: t.uid) != nil {
                if !self.volumeHandedBack { self.handBackVolume(to: t) }
                if CA.defaultOutput == v { CA.setDefaultOutput(t.id) }
                if CA.defaultSystemOutput == v { CA.setDefaultSystemOutput(t.id) }
            }
            done.signal()
        }
        _ = done.wait(timeout: .now() + timeout)
    }

    /// Re-scan devices (e.g. after the driver was installed and coreaudiod restarted).
    func rescan() { audioQueue.async { self.reevaluate(reason: .devicesChanged) } }

    /// Stop forwarding and make the real device the macOS default again (before uninstalling the driver).
    func releaseOutput(timeout: TimeInterval = 3) {
        let done = DispatchSemaphore(value: 0)
        audioQueue.async {
            PAVEngineStop()
            if let t = self.qTarget, CA.device(uid: t.uid) != nil {
                if !self.volumeHandedBack { self.handBackVolume(to: t); self.volumeHandedBack = true }
                CA.setDefaultOutput(t.id)
                CA.setDefaultSystemOutput(t.id)
            }
            done.signal()
        }
        _ = done.wait(timeout: .now() + timeout)
    }

    func choose(_ device: AudioDevice) {
        target = device   // optimistic, so the menu checkmark moves immediately
        audioQueue.async {
            self.promote(device.uid)
            self.applyTarget(device)
            self.publishPriority()
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
        let d = qDevices, t = qTarget, n = qDeviceNames
        UserDefaults.standard.set(n, forKey: "deviceNames")
        onMain {
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
        defer { publishDevices(); publishPriority() }
        qDevices = CA.allOutputDevices()
        let historyBefore = qPriority   // MRU history before newly seen devices get appended
        for d in qRealDevices where qDeviceNames[d.uid] != d.name { qDeviceNames[d.uid] = d.name }
        for d in qRealDevices where !qPriority.contains(d.uid) { qPriority.append(d.uid) }

        guard let virt = qDevices.first(where: { $0.isVirtualRouter }) else {
            // Virtual device gone (driver reinstall / coreaudiod restart): give the real device its level back
            // right away, otherwise it would play at the full volume we set while forwarding.
            if qVirtual != nil, let t = qTarget { handBackVolume(to: t); volumeHandedBack = true }
            qVirtual = nil
            PAVEngineStop(); setPlaying(false)
            publishState(.driverMissing)
            let def = CA.defaultOutput
            qTarget = qRealDevices.first { $0.id == def }
            return
        }
        let virtChanged = qVirtual != virt.id
        qVirtual = virt.id

        // During a call the default output is a real device we chose. Someone else changing it (not macOS
        // reacting to a device being plugged in or out) is the user's choice: respect it until the call ends.
        if reason == .defaultChanged, case .routing(let id) = qCallState, id != 0, CA.defaultOutput != id,
           Date().timeIntervalSince(lastDeviceListChange) >= 1.5 {
            callLog.log("default output changed by the user during the call; leaving it alone")
            qCallState = .overridden
            publishCall()
        }
        if virtChanged { watchVirtualRunning(virt.id); watchVirtualVolume(virt.id); refreshCachedMaster() }

        // A device appeared (headphones, DAC, Bluetooth, display…). Since our virtual device is the macOS default,
        // macOS won't switch by itself, so apply its rules here (see DeviceSwitching).
        let currentUIDs = Set(qRealDevices.map(\.uid))
        defer { knownUIDs = currentUIDs }
        defer { resumeAfterEarIfNeeded() }
        if qMode == .followSystem, reason == .devicesChanged, let known = knownUIDs {
            let appeared = qRealDevices
                .filter { !known.contains($0.uid) && DeviceSwitching.canTakeOverOnConnect(transport: $0.transport) }
                .map(\.uid)
            let current = qTarget.flatMap { currentUIDs.contains($0.uid) ? $0.uid : nil }
            if let uid = DeviceSwitching.deviceToSwitchTo(appeared: appeared, history: historyBefore, current: current),
               let dev = qRealDevices.first(where: { $0.uid == uid }) {
                promote(uid)
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
                promote(picked.uid)
                applyTarget(picked)
                return
            }
        }

        // Pick by priority.
        let available = Dictionary(qRealDevices.map { ($0.uid, $0) }, uniquingKeysWith: { a, _ in a })
        var next: AudioDevice?
        if qMode == .customOrder || qTarget == nil || available[qTarget!.uid] == nil {
            // Current output gone (or custom order): most recently used / highest priority device still connected.
            next = DeviceSwitching.fallback(history: qPriority, connected: qRealDevices.map(\.uid)).flatMap { available[$0] }
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
        guard let virt = qVirtual else { return }
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
        qTarget = device
        watchEars(device)
        if direct {
            if !volumeHandedBack { handBackVolume(to: device) }
            volumeHandedBack = true
        } else if changed || volumeHandedBack {
            takeOverVolume(from: device); volumeHandedBack = false
        }
        publishDevices()

        // Match sample rates so nothing has to be resampled.
        syncRate()
        if let (dev, l) = rateListener, dev != device.id || changed {
            CA.unlisten(dev, kAudioDevicePropertyNominalSampleRate, queue: audioQueue, l)
            rateListener = nil
        }
        if rateListener == nil {
            let l = CA.listen(device.id, kAudioDevicePropertyNominalSampleRate, queue: audioQueue) { [weak self] in
                self?.syncRate()
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

    private func syncRate() {
        guard let virt = qVirtual, let t = qTarget else { return }
        let r = CA.sampleRate(t.id)
        let supported: [Double] = [44100, 48000, 88200, 96000, 176400, 192000]
        guard r > 0 else { return }
        let best = supported.min { abs($0 - r) < abs($1 - r) } ?? 48000
        if abs(CA.sampleRate(virt) - best) > 0.5 { CA.set(virt, kAudioDevicePropertyNominalSampleRate, Float64(best)) }
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
        guard let virt = qVirtual, let t = qTarget else { return }
        if virtualIsRunning() {
            stopWork?.cancel(); stopWork = nil
            if force || !PAVEngineIsRunning() {
                let st = PAVEngineStart(virt, t.id)
                publishState(st == noErr ? .running : .error("Couldn't open \(t.name) (\(st))"))
                setPlaying(st == noErr)
            }
        } else if PAVEngineIsRunning() {
            if force { PAVEngineStop(); setPlaying(false); publishState(.idle); return }
            guard stopWork == nil else { return }
            let w = DispatchWorkItem { [weak self] in
                guard let self else { return }
                self.stopWork = nil
                if !self.virtualIsRunning() { PAVEngineStop(); self.setPlaying(false); self.publishState(.idle) }
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

    private var volumeHandedBack = false
    /// Last known master level, also used when the virtual device has already disappeared.
    private var cachedMaster: (volume: Float, muted: Bool) = (1, false)

    private func refreshCachedMaster() {
        guard let v = qVirtual else { return }
        let vol = CA.get(v, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput, default: Float32(1)) ?? 1
        let mute = (CA.get(v, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput, default: UInt32(0)) ?? 0) != 0
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

    private func handBackVolume(to device: AudioDevice) {
        guard let id = CA.device(uid: device.uid), CA.hardwareVolume(id) != nil else { return }
        refreshCachedMaster()
        volLog.log("handBack \(device.name, privacy: .public) -> \(self.cachedMaster.volume)")
        CA.setHardwareVolume(id, cachedMaster.volume)
        if cachedMaster.muted { CA.setHardwareMute(id, true) }
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
