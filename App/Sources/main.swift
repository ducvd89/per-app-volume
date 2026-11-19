import AppKit
import SwiftUI
import Combine

final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    let router = Router()
    lazy var store = AppStore(router: router)
    let installer = DriverInstaller()
    private var welcomeWindow: NSWindow?
    private var statusItem: NSStatusItem!
    private let popover = NSPopover()
    private var settingsWindow: NSWindow?
    private var cancellables: Set<AnyCancellable> = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        router.start()
        store.refresh()
        installer.onChange = { [weak self] in self?.router.rescan(); self?.store.refresh() }
        installer.willUninstall = { [weak self] in self?.router.releaseOutput() }

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "slider.vertical.3", accessibilityDescription: "Per-App Volume")
            button.action = #selector(togglePopover(_:))
            button.target = self
        }

        popover.behavior = .transient
        popover.animates = false
        popover.delegate = self
        let hosting = NSHostingController(
            rootView: PopoverView(store: store, router: router, installer: installer, openSettings: { [weak self] in self?.showSettings() })
        )
        // Let the popover follow SwiftUI's size as the app list grows/shrinks; otherwise it keeps the old
        // size and ends up floating below the menu bar icon.
        hosting.sizingOptions = [.preferredContentSize]
        popover.contentViewController = hosting

        HotKey.shared.onPress = { [weak self] in self?.togglePopover(nil) }
        applyHotKey()
        NotificationCenter.default.publisher(for: UserDefaults.didChangeNotification)
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in self?.applyHotKey() }
            .store(in: &cancellables)

        if installer.status != .installed { showWelcome() }
    }

    func showWelcome() {
        if welcomeWindow == nil {
            let w = NSWindow(contentViewController: NSHostingController(
                rootView: WelcomeView(installer: installer, close: { [weak self] in self?.welcomeWindow?.close() })))
            w.title = "Welcome"
            w.styleMask = [.titled, .closable, .fullSizeContentView]
            w.titlebarAppearsTransparent = true
            w.isReleasedWhenClosed = false
            welcomeWindow = w
        }
        welcomeWindow?.center()
        welcomeWindow?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // Opening the app again from Finder/Launchpad shows the panel (it has no Dock icon).
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if installer.status != .installed { showWelcome() } else if !popover.isShown { togglePopover(nil) }
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        router.shutdown()
    }

    private var lastHotKey: String?
    private func applyHotKey() {
        let d = UserDefaults.standard
        let enabled = d.object(forKey: "hotKeyEnabled") as? Bool ?? true
        let id = enabled ? (d.string(forKey: "hotKeyPreset") ?? HotKeyPreset.all[0].id) : "off"
        guard id != lastHotKey else { return }
        lastHotKey = id
        HotKey.shared.register(enabled ? HotKeyPreset.find(id) : nil)
    }

    /// When the popover last closed. A click on the icon first makes the transient popover close (mouse-down
    /// outside it), then fires this action — without this check that click would immediately reopen it.
    private var lastPopoverClose = Date.distantPast

    @objc func togglePopover(_ sender: Any?) {
        if sender is NSStatusBarButton, Date().timeIntervalSince(lastPopoverClose) < 0.3 { return }
        if popover.isShown {
            popover.performClose(sender)
        } else if let button = statusItem.button {
            store.refresh()
            installer.refresh()
            NSApp.activate(ignoringOtherApps: true)
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    func popoverWillShow(_ notification: Notification) { store.fastRefresh = true }
    func popoverDidClose(_ notification: Notification) {
        lastPopoverClose = Date()
        store.fastRefresh = false; store.selection.removeAll()
    }

    func showSettings() {
        popover.performClose(nil)
        installer.refresh()
        if settingsWindow == nil {
            let w = NSWindow(contentViewController: NSHostingController(rootView: SettingsView(router: router, installer: installer)))
            w.title = "Per-App Volume Settings"
            w.styleMask = [.titled, .closable]
            w.isReleasedWhenClosed = false
            settingsWindow = w
        }
        settingsWindow?.center()
        settingsWindow?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
}

// Restore the real output if we get killed by a signal.
let signalSources: [DispatchSourceSignal] = [SIGTERM, SIGINT, SIGHUP].map { sig in
    signal(sig, SIG_IGN)
    let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    src.setEventHandler { NSApp.terminate(nil) }
    src.resume()
    return src
}

// Only one instance at a time (two would fight over the default output device).
if let bid = Bundle.main.bundleIdentifier,
   NSRunningApplication.runningApplications(withBundleIdentifier: bid).contains(where: { $0 != NSRunningApplication.current }) {
    exit(0)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
