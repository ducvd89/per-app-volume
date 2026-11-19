import Carbon
import Foundation

/// Global shortcut via Carbon RegisterEventHotKey — needs no Accessibility / Input Monitoring permission.
struct HotKeyPreset: Identifiable, Hashable {
    let id: String
    let title: String
    let keyCode: UInt32
    let modifiers: UInt32

    static let all: [HotKeyPreset] = [
        HotKeyPreset(id: "optcmdV",   title: "⌥⌘V",   keyCode: UInt32(kVK_ANSI_V), modifiers: UInt32(optionKey | cmdKey)),
        HotKeyPreset(id: "ctrlOptV",  title: "⌃⌥V",   keyCode: UInt32(kVK_ANSI_V), modifiers: UInt32(controlKey | optionKey)),
        HotKeyPreset(id: "ctrlOptCmdV", title: "⌃⌥⌘V", keyCode: UInt32(kVK_ANSI_V), modifiers: UInt32(controlKey | optionKey | cmdKey)),
        HotKeyPreset(id: "optShiftA", title: "⌥⇧A",   keyCode: UInt32(kVK_ANSI_A), modifiers: UInt32(optionKey | shiftKey)),
        HotKeyPreset(id: "ctrlOptM",  title: "⌃⌥M",   keyCode: UInt32(kVK_ANSI_M), modifiers: UInt32(controlKey | optionKey)),
    ]
    static func find(_ id: String?) -> HotKeyPreset { all.first { $0.id == id } ?? all[0] }
}

final class HotKey {
    static let shared = HotKey()
    var onPress: (() -> Void)?
    private var ref: EventHotKeyRef?
    private var handlerInstalled = false

    func register(_ preset: HotKeyPreset?) {
        unregister()
        guard let preset else { return }
        if !handlerInstalled {
            var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
            InstallEventHandler(GetApplicationEventTarget(), { _, _, _ in
                DispatchQueue.main.async { HotKey.shared.onPress?() }
                return noErr
            }, 1, &spec, nil, nil)
            handlerInstalled = true
        }
        let id = EventHotKeyID(signature: OSType(0x5041_5656), id: 1) // 'PAVV'
        RegisterEventHotKey(preset.keyCode, preset.modifiers, id, GetApplicationEventTarget(), 0, &ref)
    }

    func unregister() {
        if let r = ref { UnregisterEventHotKey(r); ref = nil }
    }
}
