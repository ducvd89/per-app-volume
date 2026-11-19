import AppKit
import CoreAudio
import Darwin

struct AudioApp: Identifiable, Equatable {
    let key: String            // bundle id (or app path / "proc:<name>")
    var name: String
    var icon: NSImage
    var pids: Set<pid_t>
    var playing: Bool
    var peak: Float
    var isUserApp: Bool        // a regular app with windows (not a daemon / system service)
    var isGame: Bool
    var id: String { key }
    static func == (l: AudioApp, r: AudioApp) -> Bool {
        l.key == r.key && l.name == r.name && l.pids == r.pids && l.playing == r.playing && l.peak == r.peak
            && l.isUserApp == r.isUserApp && l.isGame == r.isGame
    }
}

/// Tracks which apps are using the virtual device and pushes per-app gains to the driver.
/// Core Audio reads/writes run on `audioQueue`; published state is only touched on main.
final class AppStore: ObservableObject {
    @Published private(set) var apps: [AudioApp] = []
    @Published var selection: Set<String> = []
    @Published var showHidden = false

    @Published private(set) var volumes: [String: Double]
    @Published private(set) var muted: Set<String>
    @Published private(set) var pinned: Set<String>
    @Published private(set) var hidden: Set<String>

    let router: Router
    private var timer: Timer?
    private let ownPID = getpid()
    /// Apps that have ever produced sound (persisted). The list shows these while they're running.
    private var everPlayed: Set<String>
    var fastRefresh = false { didSet { scheduleTimer() } }

    // audioQueue-only
    private var refreshing = false
    private var lastPushed: [String: Float] = [:]
    private var lastPushedDevice: AudioObjectID = 0
    private var resolveCache: [pid_t: Resolved] = [:]

    private struct Resolved { let key: String; let name: String; let icon: NSImage; let isUserApp: Bool; let isGame: Bool }

    init(router: Router) {
        self.router = router
        let d = UserDefaults.standard
        volumes = (d.dictionary(forKey: "appVolumes") as? [String: Double]) ?? [:]
        muted = Set(d.stringArray(forKey: "appMuted") ?? [])
        pinned = Set(d.stringArray(forKey: "appPinned") ?? [])
        hidden = Set(d.stringArray(forKey: "appHidden") ?? [])
        // Apps whose volume was ever adjusted have obviously played before.
        let vols = (d.dictionary(forKey: "appVolumes") as? [String: Double]) ?? [:]
        everPlayed = Set(d.stringArray(forKey: "appEverPlayed") ?? []).union(vols.keys).union(d.stringArray(forKey: "appMuted") ?? [])
        scheduleTimer()
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didTerminateApplicationNotification,
                                                          object: nil, queue: .main) { [weak self] n in
            guard let pid = (n.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication)?.processIdentifier else { return }
            audioQueue.async { self?.resolveCache.removeValue(forKey: pid) }
        }
    }

    private func scheduleTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: fastRefresh ? 0.08 : 1.0, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(timer!, forMode: .common)
    }

    // MARK: Accessors

    func volume(_ key: String) -> Double { volumes[key] ?? 1.0 }
    func isMuted(_ key: String) -> Bool { muted.contains(key) }
    func isPinned(_ key: String) -> Bool { pinned.contains(key) }
    func isHidden(_ key: String) -> Bool { hidden.contains(key) }

    /// The list: running apps (with windows) that have ever played audio, plus pinned apps.
    /// Background/system processes are never listed (their saved volume still applies).
    private var listed: [AudioApp] {
        var out = apps.filter { pinned.contains($0.key) || ($0.isUserApp && everPlayed.contains($0.key)) }
        // Apps that played before but have closed their audio connection for now (e.g. a paused browser tab).
        var seen = Set(out.map(\.key))
        for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular {
            guard let key = app.bundleIdentifier ?? app.bundleURL?.path, !seen.contains(key),
                  everPlayed.contains(key) || pinned.contains(key), app.processIdentifier != ownPID else { continue }
            seen.insert(key)
            out.append(AudioApp(key: key, name: app.localizedName ?? key, icon: app.icon ?? NSImage(), pids: [],
                                playing: false, peak: 0, isUserApp: true, isGame: false))
        }
        return out
    }

    var visibleApps: [AudioApp] {
        listed.filter { showHidden || !hidden.contains($0.key) }
            .sorted { a, b in
                let pa = pinned.contains(a.key), pb = pinned.contains(b.key)
                if pa != pb { return pa }
                if a.playing != b.playing { return a.playing }
                return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
            }
    }
    var hiddenCount: Int { listed.filter { hidden.contains($0.key) }.count }

    // MARK: Mutations

    func setVolume(_ v: Double, for keys: [String]) {
        for k in keys { volumes[k] = max(0, min(2, v)) }
        persist(); push()
    }
    func toggleMute(_ key: String) { setMuted(!muted.contains(key), for: [key]) }
    func setMuted(_ m: Bool, for keys: [String]) {
        for k in keys { if m { muted.insert(k) } else { muted.remove(k) } }
        persist(); push()
    }
    func setPinned(_ p: Bool, for keys: [String]) {
        for k in keys { if p { pinned.insert(k) } else { pinned.remove(k) } }
        persist()
    }
    func setHidden(_ h: Bool, for keys: [String]) {
        for k in keys { if h { hidden.insert(k) } else { hidden.remove(k) } }
        selection.subtract(keys)
        persist()
    }

    private func persist() {
        let d = UserDefaults.standard
        d.set(volumes, forKey: "appVolumes")
        d.set(Array(muted), forKey: "appMuted")
        d.set(Array(pinned), forKey: "appPinned")
        d.set(Array(hidden), forKey: "appHidden")
    }

    // MARK: Driver sync

    /// Reads the driver's client list on the audio queue, then updates the UI on main.
    func refresh() {
        audioQueue.async {
            guard !self.refreshing else { return }   // coreaudiod slow: don't pile up requests
            self.refreshing = true
            defer { self.refreshing = false }
            guard let virt = self.router.virtualID else {
                DispatchQueue.main.async { if !self.apps.isEmpty { self.apps = [] } }
                return
            }
            guard let list = CA.plist(virt, kPropClients) as? [[String: Any]] else { return }

            var byKey: [String: AudioApp] = [:]
            for c in list {
                guard let pidN = c["pid"] as? NSNumber else { continue }
                let pid = pid_t(pidN.int32Value)
                if pid == self.ownPID { continue }
                let info = self.resolve(pid: pid, bundleID: c["bid"] as? String)
                let running = (c["run"] as? Bool) ?? false
                let peak = (c["peak"] as? NSNumber)?.floatValue ?? 0
                if var e = byKey[info.key] {
                    e.pids.insert(pid); e.playing = e.playing || running; e.peak = max(e.peak, peak)
                    byKey[info.key] = e
                } else {
                    byKey[info.key] = AudioApp(key: info.key, name: info.name, icon: info.icon, pids: [pid], playing: running,
                                               peak: peak, isUserApp: info.isUserApp, isGame: info.isGame)
                }
            }
            let fresh = Array(byKey.values)
            DispatchQueue.main.async { self.apply(fresh) }
        }
    }

    private func apply(_ fresh: [AudioApp]) {
        // Smooth meters (fall-off) so they don't flicker between polls.
        let old = Dictionary(apps.map { ($0.key, $0.peak) }, uniquingKeysWith: { a, _ in a })
        var newlyPlayed = false
        let newApps = fresh.map { a -> AudioApp in
            var a = a
            a.peak = max(a.peak, (old[a.key] ?? 0) * 0.7)
            if a.peak < 0.001 { a.peak = 0 }
            // "Playing" means actually making sound: many apps (Discord, browsers) keep the device open while silent.
            a.playing = a.playing && a.peak > 0
            if a.playing, a.isUserApp, !everPlayed.contains(a.key) { everPlayed.insert(a.key); newlyPlayed = true }
            return a
        }.sorted { $0.key < $1.key }
        if newlyPlayed { UserDefaults.standard.set(Array(everPlayed), forKey: "appEverPlayed") }
        if newApps != apps { apps = newApps } else { objectWillChange.send() }   // running apps change over time
        selection.formIntersection(Set(visibleApps.map(\.key)))
        push()
    }

    /// Sends the pid → gain table to the driver (only when it changed).
    private func push() {
        var table: [String: Float] = [:]
        for a in apps {
            let g: Float = muted.contains(a.key) ? 0 : Float(volume(a.key))
            for p in a.pids where g != 1 { table[String(p)] = g }
        }
        audioQueue.async {
            guard let virt = self.router.virtualID else { return }
            if table == self.lastPushed && self.lastPushedDevice == virt { return }
            let dict = NSMutableDictionary()
            for (k, v) in table { dict[k] = NSNumber(value: v) }
            if CA.setPlist(virt, kPropVolumes, dict) == noErr {
                self.lastPushed = table
                self.lastPushedDevice = virt
            }
        }
    }

    // MARK: Process → app resolution (audioQueue)

    private typealias RespFn = @convention(c) (pid_t) -> pid_t
    private static let responsibleFn: RespFn? = {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "responsibility_get_pid_responsible_for_pid") else { return nil }
        return unsafeBitCast(sym, to: RespFn.self)
    }()

    private static func isGame(bundleURL: URL?) -> Bool {
        guard let url = bundleURL, let b = Bundle(url: url) else { return false }
        let cat = (b.object(forInfoDictionaryKey: "LSApplicationCategoryType") as? String ?? "").lowercased()
        return cat.contains("games")
    }

    private func resolve(pid: pid_t, bundleID: String?) -> Resolved {
        if let c = resolveCache[pid] { return c }
        var result: Resolved?

        // 1. Responsible process / parents (WebKit & Chrome helpers → the browser, game launchers → game)
        var candidates: [pid_t] = []
        if let f = Self.responsibleFn { let r = f(pid); if r > 0 { candidates.append(r) } }
        candidates.append(pid)
        var p = parentPID(pid)
        for _ in 0..<4 where p > 1 { candidates.append(p); p = parentPID(p) }

        for cand in candidates {
            guard let app = NSRunningApplication(processIdentifier: cand) else { continue }
            let regular = app.activationPolicy == .regular
            if regular || cand == pid, let b = app.bundleIdentifier ?? app.bundleURL?.path {
                result = Resolved(key: b, name: app.localizedName ?? b, icon: app.icon ?? genericIcon(),
                                  isUserApp: regular, isGame: Self.isGame(bundleURL: app.bundleURL))
                if regular { break }
            }
        }
        // 2. Outermost .app bundle in the executable path (e.g. helpers of apps launched oddly)
        if result == nil || result?.isUserApp == false, let path = processPath(pid), let r = path.range(of: ".app/") {
            let appPath = String(path[..<r.lowerBound]) + ".app"
            let url = URL(fileURLWithPath: appPath)
            let bundle = Bundle(url: url)
            let key = bundle?.bundleIdentifier ?? appPath
            // Counts as a user app only if that bundle is currently running as a regular app.
            let runningRegular = NSRunningApplication.runningApplications(withBundleIdentifier: key)
                .contains { $0.activationPolicy == .regular }
            if runningRegular || result == nil {
                let name = (bundle?.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)
                    ?? (bundle?.object(forInfoDictionaryKey: "CFBundleName") as? String)
                    ?? url.deletingPathExtension().lastPathComponent
                result = Resolved(key: key, name: name, icon: NSWorkspace.shared.icon(forFile: appPath),
                                  isUserApp: runningRegular, isGame: Self.isGame(bundleURL: url))
            }
        }
        // 3. Bundle id reported by Core Audio, or the process name (system services: never listed)
        if result == nil {
            let name = processPath(pid).map { URL(fileURLWithPath: $0).lastPathComponent } ?? bundleID ?? "PID \(pid)"
            result = Resolved(key: bundleID ?? "proc:\(name)", name: name, icon: genericIcon(), isUserApp: false, isGame: false)
        }
        resolveCache[pid] = result!
        return result!
    }

    private func genericIcon() -> NSImage {
        NSImage(systemSymbolName: "app.dashed", accessibilityDescription: nil) ?? NSImage()
    }

    private func parentPID(_ pid: pid_t) -> pid_t {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
        guard sysctl(&mib, 4, &info, &size, nil, 0) == 0, size > 0 else { return 0 }
        return info.kp_eproc.e_ppid
    }

    private func processPath(_ pid: pid_t) -> String? {
        var buf = [CChar](repeating: 0, count: 4096)
        let n = proc_pidpath(pid, &buf, UInt32(buf.count))
        return n > 0 ? String(cString: buf) : nil
    }
}
