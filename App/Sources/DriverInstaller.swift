import AppKit
import SwiftUI

/// Installs / updates / removes the bundled HAL driver using the standard macOS admin password prompt.
final class DriverInstaller: ObservableObject {
    enum Status: Equatable { case notInstalled, installed, outdated }

    static let installedPath = "/Library/Audio/Plug-Ins/HAL/PerAppVolume.driver"
    static var bundledPath: String? {
        Bundle.main.resourceURL.map { $0.appendingPathComponent("PerAppVolume.driver").path }
            .flatMap { FileManager.default.fileExists(atPath: $0) ? $0 : nil }
    }

    @Published private(set) var status: Status = .notInstalled
    @Published private(set) var busy = false
    @Published var message: String?
    @Published var isError = false

    /// Called after install/uninstall so the router can re-scan devices.
    var onChange: (() -> Void)?
    /// Called right before uninstall so we can hand output back to the real device.
    var willUninstall: (() -> Void)?

    init() { refresh() }

    func refresh() {
        let fm = FileManager.default
        guard fm.fileExists(atPath: Self.installedPath) else { status = .notInstalled; return }
        guard let bundled = Self.bundledPath else { status = .installed; return }
        let a = fm.contents(atPath: bundled + "/Contents/MacOS/PerAppVolume")
        let b = fm.contents(atPath: Self.installedPath + "/Contents/MacOS/PerAppVolume")
        status = (a != nil && a == b) ? .installed : .outdated
    }

    func install() {
        guard let src = Self.bundledPath else {
            fail("The driver is missing from the app bundle (Contents/Resources/PerAppVolume.driver).")
            return
        }
        let dst = Self.installedPath
        let cmd = """
        /bin/mkdir -p /Library/Audio/Plug-Ins/HAL && \
        /bin/rm -rf \(q(dst)) && \
        /bin/cp -R \(q(src)) \(q(dst)) && \
        /usr/sbin/chown -R root:wheel \(q(dst)) && \
        /bin/chmod -R 755 \(q(dst)) && \
        /usr/bin/xattr -dr com.apple.quarantine \(q(dst)) 2>/dev/null; \
        \(restartCoreAudio)
        """
        run(cmd, success: "Driver installed. Audio now goes through Per-App Volume.")
    }

    func uninstall() {
        willUninstall?()
        let cmd = "/bin/rm -rf \(q(Self.installedPath)); \(restartCoreAudio)"
        run(cmd, success: "Driver removed.")
    }

    // `launchctl kickstart` is refused for coreaudiod (and AirPlayXPCHelper) while SIP is on, so just kill
    // coreaudiod; launchd restarts it right away and it loads the new driver.
    // Note: macOS' AirPlayXPCHelper re-registers its drivers twice as often on every coreaudiod restart and
    // can't be reset without a reboot, so avoid needless reinstalls (a reboot clears it).
    private var restartCoreAudio: String {
        "/usr/bin/killall coreaudiod 2>/dev/null; exit 0"
    }

    /// Single-quote for the shell.
    private func q(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    private func run(_ shell: String, success: String) {
        busy = true; message = nil; isError = false
        // Escape for an AppleScript string literal.
        let lit = shell.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        let source = "do shell script \"\(lit)\" with administrator privileges"
        DispatchQueue.global(qos: .userInitiated).async {
            var err: NSDictionary?
            NSAppleScript(source: source)?.executeAndReturnError(&err)
            DispatchQueue.main.async {
                self.busy = false
                if let err {
                    let code = err[NSAppleScript.errorNumber] as? Int ?? 0
                    if code == -128 { self.message = "Cancelled."; self.isError = false }
                    else { self.fail((err[NSAppleScript.errorMessage] as? String) ?? "Error \(code)") }
                } else {
                    self.message = success
                    self.isError = false
                }
                self.refresh()
                // coreaudiod needs a moment to come back and publish the new device.
                for delay in [0.5, 2.0, 4.0] {
                    DispatchQueue.main.asyncAfter(deadline: .now() + delay) { self.refresh(); self.onChange?() }
                }
            }
        }
    }

    private func fail(_ m: String) { message = m; isError = true }
}

// MARK: - UI

/// Compact block used in the popover and in Settings.
struct DriverPanel: View {
    @ObservedObject var installer: DriverInstaller
    var compact = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: icon).foregroundStyle(color)
                Text(title).font(.system(size: 12, weight: .semibold))
                Spacer()
                if installer.busy { ProgressView().controlSize(.small) }
            }
            if !compact || installer.status != .installed {
                Text(detail).font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 8) {
                switch installer.status {
                case .notInstalled:
                    Button("Install Driver…") { installer.install() }.buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                case .outdated:
                    Button("Update Driver…") { installer.install() }.buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                    Button("Remove Driver…") { installer.uninstall() }
                case .installed:
                    if !compact {
                        Button("Reinstall") { installer.install() }
                        Button("Remove Driver…") { installer.uninstall() }
                    }
                }
            }
            .disabled(installer.busy)
            .controlSize(compact ? .small : .regular)
            if let m = installer.message {
                Text(m).font(.system(size: 11)).foregroundStyle(installer.isError ? Color.red : Color.green)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var icon: String {
        switch installer.status {
        case .notInstalled: return "exclamationmark.triangle.fill"
        case .outdated: return "arrow.triangle.2.circlepath"
        case .installed: return "checkmark.seal.fill"
        }
    }
    private var color: Color {
        switch installer.status {
        case .notInstalled: return .orange
        case .outdated: return .yellow
        case .installed: return .green
        }
    }
    private var title: String {
        switch installer.status {
        case .notInstalled: return "Virtual audio driver not installed"
        case .outdated: return "Driver update available"
        case .installed: return "Driver installed"
        }
    }
    private var detail: String {
        switch installer.status {
        case .notInstalled:
            return "Per-App Volume needs a virtual audio device to control each app's volume. macOS will ask for an administrator password once."
        case .outdated:
            return "This app includes a newer driver than the one installed."
        case .installed:
            return "The \"Per-App Volume\" virtual device is active. Removing the driver sends audio straight to your speakers or headphones again."
        }
    }
}

/// First-run window shown when the driver isn't installed yet.
struct WelcomeView: View {
    @ObservedObject var installer: DriverInstaller
    var close: () -> Void

    var body: some View {
        VStack(spacing: 18) {
            Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 84, height: 84)
            VStack(spacing: 6) {
                Text("Per-App Volume").font(.system(size: 22, weight: .bold))
                Text("Set a separate volume for every app, from 0 to 200%.")
                    .foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 10) {
                Feature(icon: "slider.horizontal.3", text: "Volume, mute, pin and hide for each app, right in the menu bar")
                Feature(icon: "lock.shield", text: "No microphone, no audio capture, no screen recording — no orange or purple dot")
                Feature(icon: "keyboard", text: "Open it with ⌥⌘V; the keyboard volume keys keep working")
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.04)))

            DriverPanel(installer: installer)
                .padding(14)
                .background(RoundedRectangle(cornerRadius: 10).stroke(Color.primary.opacity(0.1)))

            HStack {
                Text("Find the \(Image(systemName: "slider.vertical.3")) icon in the menu bar.")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
                Button(installer.status == .installed ? "Done" : "Later") { close() }
            }
        }
        .padding(24)
        .frame(width: 440)
    }

    private struct Feature: View {
        let icon: String; let text: String
        var body: some View {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: icon).frame(width: 18).foregroundStyle(Color.accentColor)
                Text(text).font(.system(size: 12)).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
