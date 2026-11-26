import SwiftUI
import AppKit
import ServiceManagement

// MARK: - Popover

struct PopoverView: View {
    @ObservedObject var store: AppStore
    @ObservedObject var router: Router
    @ObservedObject var installer: DriverInstaller
    @ObservedObject var updater: Updater
    var openSettings: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if router.state == .driverMissing || installer.status != .installed {
                DriverPanel(installer: installer, compact: true).padding(12)
                Divider()
            }
            if router.state != .driverMissing {
                content
            }
            if updater.hasNews {
                Divider()
                UpdateBanner(updater: updater).padding(.horizontal, 12).padding(.vertical, 8)
            }
            Divider()
            footer
        }
        .frame(width: 340)
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "hifispeaker.fill").foregroundStyle(.secondary)
                Menu {
                    ForEach(router.realDevices) { d in
                        Button {
                            router.choose(d)
                        } label: {
                            let name = d.name + (router.elsewhere.contains(d.uid) ? " (in use by another device)" : "")
                            if d.id == router.target?.id { Label(name, systemImage: "checkmark") } else { Text(name) }
                        }
                    }
                } label: {
                    Text(router.target?.name ?? "Choose Output Device").font(.system(size: 12, weight: .semibold)).lineLimit(1)
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                Spacer()
                StatusDot(state: router.state)
            }
            MasterRow(router: router)
            if router.inCall, let t = router.target {
                Label("In a call\(router.callUsers.map { " (\($0.joined(separator: ", ")))" } ?? ""): playing straight to \(t.name). Per-app volumes and outputs are paused.",
                      systemImage: "phone.fill")
                    .font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(2)
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
    }

    @ViewBuilder private var content: some View {
        if !store.selection.isEmpty {
            BulkBar(store: store).padding(.horizontal, 10).padding(.top, 8)
        }
        let list = store.visibleApps
        if list.isEmpty {
            VStack(spacing: 6) {
                Image(systemName: "speaker.zzz").font(.system(size: 22)).foregroundStyle(.tertiary)
                Text("No apps are playing audio").font(.system(size: 12)).foregroundStyle(.secondary)
                Text("Running apps show up here once they play audio").font(.system(size: 10)).foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity).padding(.vertical, 28)
        } else {
            ScrollView {
                LazyVStack(spacing: 2) {
                    ForEach(list) { app in AppRow(store: store, router: router, app: app) }
                }
                .padding(.horizontal, 6).padding(.vertical, 6)
            }
            .frame(maxHeight: min(CGFloat(list.count) * 56 + 12, 430))
        }
    }

    private var footer: some View {
        HStack(spacing: 12) {
            if store.hiddenCount > 0 {
                Toggle(isOn: $store.showHidden) {
                    Text("Show \(store.hiddenCount) hidden").font(.system(size: 11))
                }
                .toggleStyle(.checkbox)
            } else {
                Text("⌘-click to select multiple").font(.system(size: 11)).foregroundStyle(.tertiary)
            }
            Spacer()
            Button(action: openSettings) { Image(systemName: "gearshape") }
                .buttonStyle(.borderless).help("Settings")
            Button { NSApp.terminate(nil) } label: { Image(systemName: "power") }
                .buttonStyle(.borderless).help("Quit (returns output to your real device)")
        }
        .padding(.horizontal, 12).padding(.vertical, 8)
    }
}

struct StatusDot: View {
    let state: Router.State
    var body: some View {
        let (color, text): (Color, String) = {
            switch state {
            case .running: return (.green, "Playing")
            case .idle: return (.secondary, "Idle")
            case .driverMissing: return (.orange, "Driver not installed")
            case .error(let m): return (.red, m)
            }
        }()
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(text).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
        }
        .help(text)
    }
}

struct MasterRow: View {
    @ObservedObject var router: Router
    @StateObject private var st = MasterState()

    var body: some View {
        HStack(spacing: 8) {
            Button {
                st.muted.toggle(); router.setMasterMuted(st.muted)
            } label: {
                Image(systemName: st.muted ? "speaker.slash.fill" : "speaker.wave.3.fill")
                    .frame(width: 18)
            }
            .buttonStyle(.borderless)
            Slider(value: Binding(get: { st.value }, set: { st.value = $0; router.setMasterVolume(Float($0)) }), in: 0...1)
                .controlSize(.small)
            Text("\(Int((st.value * 100).rounded()))%").font(.system(size: 10).monospacedDigit())
                .foregroundStyle(.secondary).frame(width: 32, alignment: .trailing)
        }
        .onAppear { st.value = Double(router.masterVolume); st.muted = router.masterMuted }
        // Follows changes made elsewhere (keyboard volume keys, Control Center) via the router's snapshot.
        .onReceive(router.$masterVolume) { v in if abs(Double(v) - st.value) > 0.005 { st.value = Double(v) } }
        .onReceive(router.$masterMuted) { m in if st.muted != m { st.muted = m } }
    }
}

// MARK: - Rows

struct AppRow: View {
    @ObservedObject var store: AppStore
    @ObservedObject var router: Router
    let app: AudioApp
    @StateObject private var hover = Box(false)

    private var selected: Bool { store.selection.contains(app.key) }
    private var muted: Bool { store.isMuted(app.key) }
    /// The apps this row's controls act on: the whole selection when the row is part of it.
    private var targets: [String] { selected && store.selection.count > 1 ? Array(store.selection) : [app.key] }

    var body: some View {
        let vol = store.volume(app.key)
        HStack(spacing: 9) {
            Image(nsImage: app.icon).resizable().interpolation(.high).frame(width: 24, height: 24)
                .opacity(store.isHidden(app.key) ? 0.4 : 1)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(app.name).font(.system(size: 12, weight: .medium)).lineLimit(1)
                    if store.isPinned(app.key) { Image(systemName: "pin.fill").font(.system(size: 8)).foregroundStyle(.orange) }
                    if app.playing { Image(systemName: "waveform").font(.system(size: 8)).foregroundStyle(.green) }
                    if let uid = store.output(app.key) { OutputTag(router: router, uid: uid) }
                    Spacer(minLength: 4)
                    Text("\(Int((vol * 100).rounded()))%")
                        .font(.system(size: 10).monospacedDigit())
                        .foregroundStyle(vol > 1.001 ? Color.orange : Color.secondary)
                }
                Slider(value: Binding(get: { vol }, set: { newV in
                    var v = newV
                    if abs(v - 1) < 0.04 { v = 1 }        // detent at 100 %
                    store.setVolume(v, for: targets)
                }), in: 0...2)
                .controlSize(.mini)
                .disabled(muted)
                MeterBar(level: muted ? 0 : app.peak)
            }
            Menu {
                OutputMenuItems(store: store, router: router, keys: targets, current: store.output(app.key))
            } label: {
                Image(systemName: store.output(app.key) == nil ? "hifispeaker" : "hifispeaker.fill").frame(width: 18)
            }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .disabled(!router.routingSupported)
            .help(router.routingSupported ? "Output device for this app" : "Update the driver to choose an output device for each app")
            Button { store.toggleMute(app.key) } label: {
                Image(systemName: muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    .foregroundStyle(muted ? Color.red : Color.primary)
                    .frame(width: 18)
            }
            .buttonStyle(.borderless)
            .help(muted ? "Unmute" : "Mute")
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(
            RoundedRectangle(cornerRadius: 7)
                .fill(selected ? Color.accentColor.opacity(0.18) : (hover.v ? Color.primary.opacity(0.05) : .clear))
        )
        .contentShape(Rectangle())
        .onHover { hover.v = $0 }
        .onTapGesture {
            if NSEvent.modifierFlags.contains(.command) || NSEvent.modifierFlags.contains(.shift) {
                if selected { store.selection.remove(app.key) } else { store.selection.insert(app.key) }
            } else {
                store.selection.removeAll()
            }
        }
        .contextMenu {
            Button(store.isPinned(app.key) ? "Unpin" : "Pin to Top") { store.setPinned(!store.isPinned(app.key), for: [app.key]) }
            Button(muted ? "Unmute" : "Mute") { store.toggleMute(app.key) }
            Button("Reset to 100%") { store.setVolume(1, for: [app.key]) }
            Menu("Output") {
                OutputMenuItems(store: store, router: router, keys: targets, current: store.output(app.key))
            }
            .disabled(!router.routingSupported)
            Divider()
            Button(store.isHidden(app.key) ? "Unhide" : "Hide This App") { store.setHidden(!store.isHidden(app.key), for: [app.key]) }
        }
    }
}

/// The choices of the per-app output menu: the main output, or one device for this app.
struct OutputMenuItems: View {
    @ObservedObject var store: AppStore
    @ObservedObject var router: Router
    let keys: [String]
    /// The saved choice of the app the menu was opened on.
    let current: String?

    var body: some View {
        Button { store.setOutput(nil, for: keys) } label: {
            item("Main Output" + (router.target.map { " (\($0.name))" } ?? ""), checked: current == nil)
        }
        Divider()
        ForEach(router.realDevices) { d in
            Button { store.setOutput(d.uid, for: keys) } label: {
                item(d.name + (router.elsewhere.contains(d.uid) ? " (in use by another device)" : ""), checked: current == d.uid)
            }
        }
        // The saved device, while it isn't connected: the app plays on the main output until it is back.
        if let c = current, !router.realDevices.contains(where: { $0.uid == c }) {
            Button {} label: { item("\(router.deviceNames[c] ?? "Unknown device") (not connected)", checked: true) }
                .disabled(true)
        }
    }

    @ViewBuilder private func item(_ title: String, checked: Bool) -> some View {
        if checked { Label(title, systemImage: "checkmark") } else { Text(title) }
    }
}

/// Next to an app's name: the output device chosen for it.
struct OutputTag: View {
    @ObservedObject var router: Router
    let uid: String

    var body: some View {
        let elsewhere = router.elsewhere.contains(uid)
        let connected = router.realDevices.contains { $0.uid == uid } && !elsewhere
        let name = router.deviceNames[uid] ?? "Unknown device"
        let main = router.target?.name ?? "the main output"
        Text("→ \(name)")
            .font(.system(size: 10)).lineLimit(1).truncationMode(.tail)
            .foregroundStyle(connected ? .secondary : .tertiary)
            .strikethrough(!connected)
            .help(connected ? "Plays on \(name)"
                  : elsewhere ? "\(name) is in use by another device: playing on \(main) until this Mac has it"
                  : "\(name) isn't connected: playing on \(main) until it is")
    }
}

struct MeterBar: View {
    let level: Float
    var body: some View {
        GeometryReader { g in
            let w = g.size.width * CGFloat(min(1, max(0, level)))
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.07))
                Capsule().fill(level > 0.98 ? Color.red : Color.green.opacity(0.75)).frame(width: w)
                    .animation(.linear(duration: 0.08), value: level)
            }
        }
        .frame(height: 2)
    }
}

struct BulkBar: View {
    @ObservedObject var store: AppStore
    @StateObject private var bulk = Box(1.0)

    var body: some View {
        let keys = Array(store.selection)
        let allMuted = keys.allSatisfy { store.isMuted($0) }
        let allPinned = keys.allSatisfy { store.isPinned($0) }
        VStack(spacing: 6) {
            HStack(spacing: 10) {
                Text("\(keys.count) selected").font(.system(size: 11, weight: .semibold))
                Spacer()
                Button(allMuted ? "Unmute" : "Mute") { store.setMuted(!allMuted, for: keys) }
                Button(allPinned ? "Unpin" : "Pin") { store.setPinned(!allPinned, for: keys) }
                Button("Hide") { store.setHidden(true, for: keys) }
                Button { store.selection.removeAll() } label: { Image(systemName: "xmark.circle.fill") }.buttonStyle(.borderless)
            }
            .controlSize(.small)
            HStack(spacing: 8) {
                Slider(value: Binding(get: { bulk.v }, set: { v in
                    bulk.v = abs(v - 1) < 0.04 ? 1 : v
                    store.setVolume(bulk.v, for: keys)
                }), in: 0...2).controlSize(.mini)
                Text("\(Int((bulk.v * 100).rounded()))%").font(.system(size: 10).monospacedDigit()).frame(width: 34, alignment: .trailing)
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.accentColor.opacity(0.08)))
        .onAppear { bulk.v = keys.first.map { store.volume($0) } ?? 1 }
    }
}

// MARK: - Settings

struct SettingsView: View {
    @ObservedObject var router: Router
    @ObservedObject var installer: DriverInstaller
    @ObservedObject var updater: Updater
    @AppStorage("hotKeyEnabled") private var hotKeyEnabled = true
    @AppStorage("hotKeyPreset") private var hotKeyPreset = HotKeyPreset.all[0].id
    @StateObject private var login = LoginState()

    var body: some View {
        Form {
            Section("General") {
                Toggle("Open at Login", isOn: Binding(get: { login.enabled }, set: { on in
                    do {
                        if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
                        login.error = nil
                    } catch { login.error = error.localizedDescription }
                    login.enabled = SMAppService.mainApp.status == .enabled
                }))
                if let e = login.error { Text(e).font(.caption).foregroundStyle(.red) }
                Toggle("Global shortcut to open the panel", isOn: $hotKeyEnabled)
                Picker("Shortcut", selection: $hotKeyPreset) {
                    ForEach(HotKeyPreset.all) { Text($0.title).tag($0.id) }
                }
                .disabled(!hotKeyEnabled)
            }
            Section("Output Device") {
                Picker("Switching", selection: $router.mode) {
                    ForEach(OutputMode.allCases) { Text($0.title).tag($0) }
                }
                Text(router.mode == .followSystem
                     ? "Uses the device that connected last (headphones, DAC, Bluetooth) or the one you pick in Control Center or System Settings. When it disconnects, goes back to the most recently connected device that's still connected."
                     : "Always uses the first connected device in the list below.")
                    .font(.caption).foregroundStyle(.secondary)
                ForEach(Array(router.priority.enumerated()), id: \.element) { idx, uid in
                    let connected = router.realDevices.contains { $0.uid == uid }
                    HStack {
                        Text("\(idx + 1).").monospacedDigit().foregroundStyle(.secondary)
                        Text(router.deviceNames[uid] ?? uid).foregroundStyle(connected ? .primary : .secondary)
                        if !connected { Text("(not connected)").font(.caption).foregroundStyle(.tertiary) }
                        else if router.elsewhere.contains(uid) { Text("(in use by another device)").font(.caption).foregroundStyle(.tertiary) }
                        Spacer()
                        Button { router.movePriority(from: idx, to: idx - 1) } label: { Image(systemName: "chevron.up") }
                            .buttonStyle(.borderless).disabled(idx == 0)
                        Button { router.movePriority(from: idx, to: idx + 1) } label: { Image(systemName: "chevron.down") }
                            .buttonStyle(.borderless).disabled(idx == router.priority.count - 1)
                        Button { router.removePriority(at: idx) } label: { Image(systemName: "minus.circle") }
                            .buttonStyle(.borderless).disabled(connected)
                    }
                }
            }
            CallSection(router: router)
            Section("Virtual Audio Driver") {
                DriverPanel(installer: installer)
            }
            UpdateSection(updater: updater)
            Section("Privacy") {
                Label("No microphone, no audio capture, no screen recording — no orange or purple dot in the menu bar.",
                      systemImage: "lock.shield")
                    .font(.caption)
            }
        }
        .formStyle(.grouped)
        .frame(width: 480, height: 640)
    }
}

struct CallSection: View {
    @ObservedObject var router: Router
    @StateObject private var newID = Box("")

    var body: some View {
        Section("Calls") {
            Toggle("Use the real output device during calls", isOn: $router.callModeEnabled)
            Text("While an app uses the microphone for a call, macOS outputs straight to \(router.target?.name ?? "your speakers or headphones") so echo cancellation works. Per-app volumes don't apply until the call ends. Per-App Volume only reads whether the mic is in use; it never opens it.")
                .font(.caption).foregroundStyle(.secondary)
            if router.callModeEnabled {
                if MicMonitor.hasProcessAPI {
                    Toggle("Only for these apps", isOn: $router.callAppsOnly)
                    if router.callAppsOnly { appList }
                } else {
                    Text("macOS 13: any app using the microphone counts as a call.").font(.caption).foregroundStyle(.secondary)
                }
                if let users = router.callUsers {
                    Label("In a call now: \(users.joined(separator: ", "))", systemImage: "phone.fill").font(.caption)
                }
            }
        }
    }

    @ViewBuilder private var appList: some View {
        ForEach(Array(router.callApps.enumerated()), id: \.offset) { idx, bid in
            HStack {
                Text(Self.name(of: bid)).lineLimit(1)
                Text(bid).font(.caption).foregroundStyle(.tertiary).lineLimit(1)
                Spacer()
                Button { router.callApps.remove(at: idx) } label: { Image(systemName: "minus.circle") }
                    .buttonStyle(.borderless)
            }
        }
        HStack {
            TextField("Bundle ID", text: $newID.v).onSubmit(add)
            Button("Add", action: add).disabled(newID.v.trimmingCharacters(in: .whitespaces).isEmpty)
            Menu("Running App") {
                ForEach(runningApps, id: \.0) { bid, name in
                    Button(name) { append(bid) }
                }
            }
            .fixedSize()
        }
        Button("Restore Default Apps") { router.callApps = kDefaultCallApps }
            .disabled(router.callApps == kDefaultCallApps)
    }

    private var runningApps: [(String, String)] {
        NSWorkspace.shared.runningApplications
            .filter { $0.activationPolicy == .regular }
            .compactMap { a in a.bundleIdentifier.map { ($0, a.localizedName ?? $0) } }
            .filter { !router.callApps.contains($0.0) }
            .sorted { $0.1.localizedCaseInsensitiveCompare($1.1) == .orderedAscending }
    }

    private func add() { append(newID.v); newID.v = "" }

    private func append(_ id: String) {
        let id = id.trimmingCharacters(in: .whitespaces)
        guard !id.isEmpty, !router.callApps.contains(id) else { return }
        router.callApps.append(id)
    }

    private static func name(of bid: String) -> String {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bid) else { return bid }
        return FileManager.default.displayName(atPath: url.path).replacingOccurrences(of: ".app", with: "")
    }
}

// Small observable holders (the CLT SDK lacks the SwiftUI @State macro plugin).
final class Box<T>: ObservableObject { @Published var v: T; init(_ v: T) { self.v = v } }
final class MasterState: ObservableObject { @Published var value: Double = 1; @Published var muted = false }
final class LoginState: ObservableObject {
    @Published var enabled = SMAppService.mainApp.status == .enabled
    @Published var error: String?
}
