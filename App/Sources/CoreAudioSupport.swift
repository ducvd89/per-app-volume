import CoreAudio
import Foundation

// Shared with the driver: see App/Engine/PAVProtocol.h.
let kVirtualDeviceUID = kPAVDeviceUID
let kPropClients: AudioObjectPropertySelector = kPAVPropClients
let kPropVolumes: AudioObjectPropertySelector = kPAVPropVolumes
let kPropLatency: AudioObjectPropertySelector = kPAVPropLatency
let kPropRoutes: AudioObjectPropertySelector = kPAVPropRoutes

struct AudioDevice: Identifiable, Hashable {
    let id: AudioObjectID
    let uid: String
    let name: String
    let outputChannels: Int
    let transport: UInt32
    var isVirtualRouter: Bool { uid == kVirtualDeviceUID }
}

enum CA {
    static func addr(_ sel: AudioObjectPropertySelector,
                     _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                     _ element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: element)
    }

    static func get<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector,
                       scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, default def: T) -> T? {
        var a = addr(sel, scope)
        var value = def
        var size = UInt32(MemoryLayout<T>.size)
        let st = withUnsafeMutablePointer(to: &value) { AudioObjectGetPropertyData(obj, &a, 0, nil, &size, $0) }
        return st == noErr ? value : nil
    }

    @discardableResult
    static func set<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector,
                       scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ value: T) -> OSStatus {
        var a = addr(sel, scope)
        var v = value
        return withUnsafePointer(to: &v) { AudioObjectSetPropertyData(obj, &a, 0, nil, UInt32(MemoryLayout<T>.size), $0) }
    }

    static func has(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> Bool {
        var a = addr(sel, scope)
        return AudioObjectHasProperty(obj, &a)
    }

    static func string(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
        var a = addr(sel)
        var ref: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let st = AudioObjectGetPropertyData(obj, &a, 0, nil, &size, &ref)
        guard st == noErr, let r = ref else { return nil }
        return r.takeRetainedValue() as String
    }

    static func array<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector,
                         scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, of: T.Type) -> [T] {
        var a = addr(sel, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(obj, &a, 0, nil, &size) == noErr, size > 0 else { return [] }
        let count = Int(size) / MemoryLayout<T>.stride
        let buf = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<T>.alignment)
        defer { buf.deallocate() }
        guard AudioObjectGetPropertyData(obj, &a, 0, nil, &size, buf) == noErr else { return [] }
        let typed = buf.bindMemory(to: T.self, capacity: count)
        return Array(UnsafeBufferPointer(start: typed, count: Int(size) / MemoryLayout<T>.stride))
    }

    static func plist(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> Any? {
        var a = addr(sel)
        var ref: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        let st = AudioObjectGetPropertyData(obj, &a, 0, nil, &size, &ref)
        guard st == noErr, let r = ref else { return nil }
        return r.takeRetainedValue()
    }

    @discardableResult
    static func setPlist(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, _ value: CFPropertyList) -> OSStatus {
        var a = addr(sel)
        var v: CFPropertyList = value
        return withUnsafePointer(to: &v) { p in
            AudioObjectSetPropertyData(obj, &a, 0, nil, UInt32(MemoryLayout<CFPropertyList>.size), p)
        }
    }

    // MARK: Devices

    static func outputChannelCount(_ id: AudioObjectID) -> Int { channelCount(id, scope: kAudioObjectPropertyScopeOutput) }

    static func channelCount(_ id: AudioObjectID, scope: AudioObjectPropertyScope) -> Int {
        var a = addr(kAudioDevicePropertyStreamConfiguration, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, raw) == noErr else { return 0 }
        let abl = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return abl.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    static func allOutputDevices() -> [AudioDevice] {
        let ids = array(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices, of: AudioObjectID.self)
        return ids.compactMap { id in
            let ch = outputChannelCount(id)
            guard ch > 0, let uid = string(id, kAudioDevicePropertyDeviceUID) else { return nil }
            if (get(id, kAudioDevicePropertyIsHidden, default: UInt32(0)) ?? 0) != 0 { return nil }
            let name = string(id, kAudioObjectPropertyName) ?? uid
            let transport = get(id, kAudioDevicePropertyTransportType, default: UInt32(0)) ?? 0
            return AudioDevice(id: id, uid: uid, name: name, outputChannels: ch, transport: transport)
        }
    }

    /// AirPods in-ear state (private Bluetooth HAL properties): 'iesb' changes whenever a bud goes in or out;
    /// 'iesp' / 'iess' are the primary / secondary bud (1 = in ear, 2 = out of ear, other = in the case / unknown).
    static let kPropInEarStatus: AudioObjectPropertySelector = 0x6965_7362   // 'iesb'
    private static let kPropInEarPrimary: AudioObjectPropertySelector = 0x6965_7370   // 'iesp'
    private static let kPropInEarSecondary: AudioObjectPropertySelector = 0x6965_7373   // 'iess'

    /// How many buds are in the ears, or nil for devices without ear detection.
    static func budsInEar(_ dev: AudioObjectID) -> Int? {
        guard has(dev, kPropInEarStatus) else { return nil }
        return [kPropInEarPrimary, kPropInEarSecondary].filter { (get(dev, $0, default: UInt32(0)) ?? 0) == 1 }.count
    }

    static func device(uid: String) -> AudioObjectID? {
        allOutputDevices().first { $0.uid == uid }?.id
    }

    static var defaultOutput: AudioObjectID {
        CA.get(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultOutputDevice, default: AudioObjectID(0)) ?? 0
    }

    @discardableResult
    static func setDefaultOutput(_ id: AudioObjectID) -> OSStatus {
        set(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultOutputDevice, id)
    }

    static var defaultSystemOutput: AudioObjectID {
        CA.get(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultSystemOutputDevice, default: AudioObjectID(0)) ?? 0
    }

    @discardableResult
    static func setDefaultSystemOutput(_ id: AudioObjectID) -> OSStatus {
        set(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultSystemOutputDevice, id)
    }

    static func sampleRate(_ id: AudioObjectID) -> Double {
        get(id, kAudioDevicePropertyNominalSampleRate, default: Float64(0)) ?? 0
    }

    // MARK: Listeners

    @discardableResult
    static func listen(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector,
                       scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                       queue: DispatchQueue = .main, _ block: @escaping () -> Void) -> AudioObjectPropertyListenerBlock {
        var a = addr(sel, scope)
        let listener: AudioObjectPropertyListenerBlock = { _, _ in block() }
        AudioObjectAddPropertyListenerBlock(obj, &a, queue, listener)
        return listener
    }

    static func unlisten(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector,
                         scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                         queue: DispatchQueue = .main, _ listener: @escaping AudioObjectPropertyListenerBlock) {
        var a = addr(sel, scope)
        AudioObjectRemovePropertyListenerBlock(obj, &a, queue, listener)
    }
}

// MARK: Hardware volume of real devices

extension CA {
    /// Elements that carry a settable output volume: the main element, or else channels 1 and 2.
    static func volumeElements(_ dev: AudioObjectID) -> [AudioObjectPropertyElement] {
        func settable(_ el: AudioObjectPropertyElement) -> Bool {
            var a = addr(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, el)
            guard AudioObjectHasProperty(dev, &a) else { return false }
            var s: DarwinBoolean = false
            return AudioObjectIsPropertySettable(dev, &a, &s) == noErr && s.boolValue
        }
        if settable(kAudioObjectPropertyElementMain) { return [kAudioObjectPropertyElementMain] }
        return [1, 2].filter(settable)
    }

    static func hardwareVolume(_ dev: AudioObjectID) -> Float? {
        let els = volumeElements(dev)
        guard !els.isEmpty else { return nil }
        var sum: Float = 0
        for el in els {
            var a = addr(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, el)
            var v: Float32 = 0; var sz = UInt32(4)
            guard AudioObjectGetPropertyData(dev, &a, 0, nil, &sz, &v) == noErr else { return nil }
            sum += v
        }
        return sum / Float(els.count)
    }

    static func setHardwareVolume(_ dev: AudioObjectID, _ value: Float) {
        for el in volumeElements(dev) {
            var a = addr(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, el)
            var v = Float32(max(0, min(1, value)))
            AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &v)
        }
    }

    static func hardwareMute(_ dev: AudioObjectID) -> Bool? {
        var a = addr(kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput)
        guard AudioObjectHasProperty(dev, &a) else { return nil }
        var v: UInt32 = 0; var sz = UInt32(4)
        return AudioObjectGetPropertyData(dev, &a, 0, nil, &sz, &v) == noErr ? v != 0 : nil
    }

    static func setHardwareMute(_ dev: AudioObjectID, _ on: Bool) {
        var a = addr(kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput)
        guard AudioObjectHasProperty(dev, &a) else { return }
        var v: UInt32 = on ? 1 : 0
        AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &v)
    }
}
