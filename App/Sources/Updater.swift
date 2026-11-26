import AppKit
import CryptoKit
import SwiftUI
import os

private let updateLog = Logger(subsystem: "com.perappvolume.app", category: "update")

/// The rules of updating, as pure functions so they can be tested.
///
/// The app looks for new releases in the GitHub repo it was built from and installs only what was signed
/// with the key of whoever built it. Both come from the build (see build.sh): the source names neither.
/// The app is ad-hoc signed, so macOS can't tell who published a download; the signature does.
enum UpdateLogic {
    /// Names of the release's files: the app, zipped, and the signature of that zip (scripts/update-key.swift).
    static let zipName = "Per-App-Volume.zip"
    static let signatureName = zipName + ".sig"
    /// A release is a few megabytes. Anything much bigger isn't one.
    static let maxZipBytes = 100 << 20
    static let checkInterval: TimeInterval = 24 * 3600

    struct Release: Equatable {
        let version: String
        let page: URL
        /// The zipped app and the signature of that zip. Nil if the release lacks either (before 1.5,
        /// or a release made without the key): the app then only points to the page.
        let zip: URL?
        let signature: URL?
        var installable: Bool { zip != nil && signature != nil }
    }

    /// "owner/name" of a GitHub repo, or nil if it isn't one.
    static func repo(_ s: String?) -> String? {
        guard let s, s.range(of: #"^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$"#, options: .regularExpression) != nil else { return nil }
        return s
    }

    /// "1.10.0" is newer than "1.9.3"; a leading "v" doesn't matter. Anything that isn't numbers and dots
    /// is never newer.
    static func isNewer(_ a: String, than b: String) -> Bool {
        guard let x = numbers(a), let y = numbers(b) else { return false }
        for i in 0..<max(x.count, y.count) {
            let l = i < x.count ? x[i] : 0, r = i < y.count ? y[i] : 0
            if l != r { return l > r }
        }
        return false
    }

    static func numbers(_ version: String) -> [Int]? {
        let v = version.hasPrefix("v") ? String(version.dropFirst()) : version
        let parts = v.split(separator: ".", omittingEmptySubsequences: false).map { Int($0) }
        guard !parts.isEmpty, parts.count <= 4, parts.allSatisfy({ $0 != nil && $0! >= 0 }) else { return nil }
        return parts.map { $0! }
    }

    /// The latest release as GitHub's API describes it ("releases/latest"). Its files count only if both
    /// are there and are this repo's own downloads.
    static func release(fromJSON data: Data, repo: String) -> Release? {
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tag = json["tag_name"] as? String, numbers(tag) != nil,
              let assets = json["assets"] as? [[String: Any]] else { return nil }
        let base = "https://github.com/\(repo)/releases/"
        func asset(_ name: String) -> URL? {
            guard let a = assets.first(where: { $0["name"] as? String == name }), let s = a["browser_download_url"] as? String,
                  s.hasPrefix(base + "download/"), let url = URL(string: s) else { return nil }
            return url
        }
        guard let page = URL(string: base + "tag/" + tag) else { return nil }
        let zip = asset(zipName), sig = asset(signatureName)
        let both = zip != nil && sig != nil
        return Release(version: tag.hasPrefix("v") ? String(tag.dropFirst()) : tag, page: page, zip: both ? zip : nil, signature: both ? sig : nil)
    }

    /// Whether `signature` (base64, Ed25519) of `data` was made with the private half of `publicKey` (base64).
    static func verify(_ data: Data, signature: String, publicKey: String) -> Bool {
        guard let k = Data(base64Encoded: publicKey), let key = try? Curve25519.Signing.PublicKey(rawRepresentation: k),
              let sig = Data(base64Encoded: signature.trimmingCharacters(in: .whitespacesAndNewlines)) else { return false }
        return key.isValidSignature(sig, for: data)
    }

    static func checkDue(last: Date?, now: Date) -> Bool {
        guard let last else { return true }
        return now.timeIntervalSince(last) >= checkInterval || last > now
    }

    /// Whether the app can replace itself where it is. Not from a disk image, not while macOS runs it from
    /// a random read-only place (a quarantined app that was never moved), not without write access.
    static func canReplace(bundlePath: String, writable: (String) -> Bool) -> Bool {
        guard bundlePath.hasSuffix(".app"), !bundlePath.contains("/AppTranslocation/"), !bundlePath.hasPrefix("/Volumes/") else { return false }
        return writable(bundlePath) && writable((bundlePath as NSString).deletingLastPathComponent)
    }
}

/// Looks for a new release once a day and installs it when asked: download, check the signature, replace
/// the app, open it again. Nothing is sent but the request for the latest release.
final class Updater: ObservableObject {
    enum State: Equatable {
        case idle, checking, upToDate
        case available(UpdateLogic.Release)
        case installing(UpdateLogic.Release)
        case failed(String)
    }

    @Published private(set) var state: State = .idle
    /// Why the last install didn't happen. The update stays available, to try again.
    @Published private(set) var installError: String?
    @Published private(set) var lastCheck: Date?
    @Published var automatic: Bool {
        didSet { UserDefaults.standard.set(automatic, forKey: "updateCheckEnabled"); if automatic { checkIfDue() } }
    }

    /// Where the releases are and the key they are signed with. Nil in a build made without them: no updater.
    let source: (repo: String, publicKey: String)?
    let currentVersion = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    /// Whether the app can install this release itself. Otherwise it offers the release's page.
    func canInstall(_ release: UpdateLogic.Release) -> Bool {
        release.installable && UpdateLogic.canReplace(bundlePath: Bundle.main.bundlePath) { FileManager.default.isWritableFile(atPath: $0) }
    }
    /// Something to show in the panel.
    var hasNews: Bool {
        switch state { case .available, .installing: return true; default: return false }
    }

    private var timer: Timer?
    private let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral   // no cookies, no cache on disk
        c.timeoutIntervalForRequest = 20
        c.timeoutIntervalForResource = 300
        c.httpAdditionalHeaders = ["User-Agent": "Per-App-Volume", "Accept": "application/vnd.github+json"]
        return URLSession(configuration: c)
    }()

    init() {
        let info = Bundle.main.infoDictionary
        if let repo = UpdateLogic.repo(info?["PAVUpdateRepo"] as? String), let key = info?["PAVUpdateKey"] as? String, !key.isEmpty {
            source = (repo, key)
        } else {
            source = nil
        }
        let d = UserDefaults.standard
        automatic = d.object(forKey: "updateCheckEnabled") as? Bool ?? true
        lastCheck = d.object(forKey: "updateLastCheck") as? Date
    }

    func start() {
        guard source != nil else { return }
        // Not right at launch (login: the network may not be up yet); then look every hour whether a day has passed.
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in self?.checkIfDue() }
        timer = Timer.scheduledTimer(withTimeInterval: 3600, repeats: true) { [weak self] _ in self?.checkIfDue() }
    }

    private func checkIfDue() {
        guard automatic, UpdateLogic.checkDue(last: lastCheck, now: Date()) else { return }
        check(byUser: false)
    }

    /// `byUser`: say so when it fails or there is nothing new. The daily check stays quiet.
    func check(byUser: Bool = true) {
        guard let source else { return }
        switch state { case .checking, .installing: return; default: break }
        let before = state
        state = .checking
        let url = URL(string: "https://api.github.com/repos/\(source.repo)/releases/latest")!
        session.dataTask(with: url) { [weak self] data, response, error in
            let ok = (response as? HTTPURLResponse)?.statusCode == 200
            let release = ok ? data.flatMap { UpdateLogic.release(fromJSON: $0, repo: source.repo) } : nil
            DispatchQueue.main.async {
                guard let self else { return }
                if ok {
                    self.lastCheck = Date()
                    UserDefaults.standard.set(self.lastCheck, forKey: "updateLastCheck")
                }
                if let release, UpdateLogic.isNewer(release.version, than: self.currentVersion) {
                    updateLog.log("version \(release.version, privacy: .public) is available")
                    self.state = .available(release)
                } else if ok {
                    self.state = .upToDate
                } else if byUser {
                    self.state = .failed("Couldn't reach GitHub" + (error.map { ": \($0.localizedDescription)" } ?? "."))
                } else {
                    self.state = before   // try again in an hour
                }
            }
        }.resume()
    }

    func openReleasePage() {
        if case .available(let r) = state { NSWorkspace.shared.open(r.page) }
        else if let source, let url = URL(string: "https://github.com/\(source.repo)/releases/latest") { NSWorkspace.shared.open(url) }
    }

    /// Downloads the release, replaces this app with it and opens it again.
    func install() {
        guard let source, case .available(let release) = state else { return }
        guard canInstall(release), let zipURL = release.zip, let sigURL = release.signature else { openReleasePage(); return }
        state = .installing(release)
        installError = nil
        let version = currentVersion
        Task.detached(priority: .userInitiated) { [session] in
            do {
                let zip = try await Self.download(zipURL, session: session, limit: UpdateLogic.maxZipBytes)
                let sig = try await Self.download(sigURL, session: session, limit: 1024)
                guard UpdateLogic.verify(zip, signature: String(decoding: sig, as: UTF8.self), publicKey: source.publicKey) else {
                    throw UpdateError("The download isn't signed with this app's update key. Nothing was installed.")
                }
                let old = Bundle.main.bundleURL
                let new = try Self.unpack(zip, release: release, currentVersion: version, bundleID: Bundle.main.bundleIdentifier, replacing: old)
                try Self.replace(old, with: new)
                updateLog.log("installed version \(release.version, privacy: .public); reopening")
                await MainActor.run { Self.reopen(old) }
            } catch {
                updateLog.error("update failed: \(error.localizedDescription, privacy: .public)")
                await MainActor.run { self.state = .available(release); self.installError = error.localizedDescription }
            }
        }
    }

    private static func download(_ url: URL, session: URLSession, limit: Int) async throws -> Data {
        let (data, response) = try await session.data(from: url)
        guard (response as? HTTPURLResponse)?.statusCode == 200, !data.isEmpty, data.count <= limit else {
            throw UpdateError("Couldn't download \(url.lastPathComponent).")
        }
        return data
    }

    /// Unpacks a zip whose signature was already checked, next to the app it will replace (same volume), and
    /// makes sure it holds this app, newer and intact. Returns the new app.
    static func unpack(_ zip: Data, release: UpdateLogic.Release, currentVersion: String, bundleID: String?, replacing old: URL) throws -> URL {
        let fm = FileManager.default
        let dir = try fm.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: old, create: true)
        let file = dir.appendingPathComponent(UpdateLogic.zipName)
        try zip.write(to: file)
        var done = false
        defer { if !done { try? fm.removeItem(at: dir) } }
        guard run("/usr/bin/ditto", ["-x", "-k", file.path, dir.path]) else { throw UpdateError("Couldn't unpack the update.") }
        try? fm.removeItem(at: file)
        let apps = try fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil).filter { $0.pathExtension == "app" }
        guard apps.count == 1, let info = NSDictionary(contentsOf: apps[0].appendingPathComponent("Contents/Info.plist")),
              let id = info["CFBundleIdentifier"] as? String, id == bundleID,
              let version = info["CFBundleShortVersionString"] as? String,
              version == release.version, UpdateLogic.isNewer(version, than: currentVersion) else {
            throw UpdateError("The download isn't version \(release.version) of this app.")
        }
        guard run("/usr/bin/codesign", ["--verify", "--deep", "--strict", apps[0].path]) else {
            throw UpdateError("The downloaded app is damaged.")
        }
        done = true
        return apps[0]
    }

    /// Puts the unpacked app where the running one is, and removes the folder it was unpacked in.
    static func replace(_ old: URL, with new: URL) throws {
        defer { try? FileManager.default.removeItem(at: new.deletingLastPathComponent()) }
        _ = try FileManager.default.replaceItemAt(old, withItemAt: new)
    }

    private static func run(_ tool: String, _ arguments: [String]) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = arguments
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    /// Quits (which hands the output back to the real device) and opens the app again once this process is
    /// gone: only one copy may run at a time.
    private static func reopen(_ app: URL) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "while /bin/kill -0 \"$0\" 2>/dev/null; do /bin/sleep 0.1; done; /bin/sleep 0.5; /usr/bin/open \"$1\"",
                       String(ProcessInfo.processInfo.processIdentifier), app.path]
        try? p.run()
        NSApp.terminate(nil)
    }
}

struct UpdateError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

// MARK: - UI

/// In the panel: only while there is something to say.
struct UpdateBanner: View {
    @ObservedObject var updater: Updater

    var body: some View {
        switch updater.state {
        case .available(let r):
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Image(systemName: "arrow.down.circle.fill").foregroundStyle(Color.accentColor)
                    Text("Version \(r.version) is available").font(.system(size: 11, weight: .semibold))
                    Spacer()
                    Button("What's New") { updater.openReleasePage() }.buttonStyle(.link).font(.system(size: 11))
                    Button(updater.canInstall(r) ? "Update" : "Download") { updater.install() }.controlSize(.small)
                        .help(updater.canInstall(r) ? "Downloads version \(r.version), replaces the app and opens it again"
                              : "Opens the download page: the app can't install this version itself")
                }
                if let e = updater.installError {
                    Text(e).font(.system(size: 10)).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
                }
            }
        case .installing(let r):
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Updating to version \(r.version)…").font(.system(size: 11))
                Spacer()
            }
        default:
            EmptyView()
        }
    }
}

struct UpdateSection: View {
    @ObservedObject var updater: Updater

    var body: some View {
        Section("Updates") {
            if updater.source == nil {
                Text("This copy was built without an update address and signing key, so it doesn't look for updates.")
                    .font(.caption).foregroundStyle(.secondary)
            } else {
                Toggle("Check for updates automatically", isOn: $updater.automatic)
                Text("Once a day the app asks GitHub for the latest release. Nothing else is sent, and nothing is installed until you click Update. Updates are checked against the signing key this copy was built with.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack(spacing: 8) {
                    status
                    Spacer()
                    if case .available(let r) = updater.state {
                        Button("What's New") { updater.openReleasePage() }
                        Button(updater.canInstall(r) ? "Update and Reopen" : "Download…") { updater.install() }
                            .buttonStyle(.borderedProminent)
                            .help(updater.canInstall(r) ? "Downloads version \(r.version), replaces the app and opens it again"
                                  : "Opens the download page")
                    } else {
                        Button("Check Now") { updater.check() }.disabled(updater.state == .checking || isInstalling)
                    }
                }
                if let e = updater.installError {
                    Text(e).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
                }
                if case .available(let r) = updater.state, !updater.canInstall(r) {
                    Text(r.installable ? "The app can't replace itself where it is. Move it to Applications, or download the new version."
                         : "This release has no signed update, so the app can't install it itself. Download it from the release page.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private var isInstalling: Bool { if case .installing = updater.state { return true } else { return false } }

    @ViewBuilder private var status: some View {
        switch updater.state {
        case .idle:
            Text("Version \(updater.currentVersion)" + (updater.lastCheck.map { ", last checked \(Self.ago($0))" } ?? ""))
                .font(.caption).foregroundStyle(.secondary)
        case .checking:
            HStack(spacing: 6) { ProgressView().controlSize(.small); Text("Checking…").font(.caption) }
        case .upToDate:
            Label("Version \(updater.currentVersion) is the latest", systemImage: "checkmark.circle").font(.caption)
        case .available(let r):
            Label("Version \(r.version) is available (you have \(updater.currentVersion))", systemImage: "arrow.down.circle").font(.caption)
        case .installing(let r):
            HStack(spacing: 6) { ProgressView().controlSize(.small); Text("Updating to version \(r.version)…").font(.caption) }
        case .failed(let m):
            Text(m).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
        }
    }

    private static func ago(_ date: Date) -> String {
        let f = RelativeDateTimeFormatter()
        f.unitsStyle = .full
        return f.localizedString(for: date, relativeTo: Date())
    }
}
