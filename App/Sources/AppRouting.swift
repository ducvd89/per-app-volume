import Darwin
import Foundation

/// Which output each app plays on, as pure functions so they can be tested.
///
/// Apps normally play on the main output (the one in the menu's header). An app can be given an output of
/// its own; that choice is remembered by app. The driver mixes such apps into a "bus" per device, and the
/// app plays every bus on its device (see PAVProtocol.h). Whenever the chosen device isn't connected, the
/// app plays on the main output, and moves back by itself when the device returns.
enum AppRouting {
    /// - Parameters:
    ///   - wants: driver table key ("<pid>", or "b:<bundle id>" for apps that aren't running yet) → UID of
    ///     the device the app should play on ("" = the main output).
    ///   - connected: UIDs of the output devices that can be used right now.
    ///   - main: UID of the main output.
    ///   - current: bus → UID of the device it plays on now.
    /// - Returns: the device of every bus that is needed, and the bus of every table entry (0 = main mix).
    static func plan(wants: [String: String], connected: Set<String>, main: String?,
                     current: [Int: String], maxBuses: Int) -> (buses: [Int: String], table: [String: Int]) {
        // A device gets a bus while a running process plays there. Entries by bundle ID alone are apps
        // that may never start; they use the bus if it exists.
        var needed = Set<String>()
        for (key, uid) in wants where !key.hasPrefix(kPAVBundleGainPrefix) && uid != main && connected.contains(uid) {
            needed.insert(uid)
        }
        // A bus that already plays a device keeps it, so its audio isn't interrupted by other changes.
        var buses: [Int: String] = [:]
        for (bus, uid) in current where bus >= 1 && bus <= maxBuses && needed.contains(uid) && !buses.values.contains(uid) {
            buses[bus] = uid
        }
        var free = (1...max(1, maxBuses)).filter { $0 <= maxBuses && buses[$0] == nil }
        for uid in needed.sorted() where !buses.values.contains(uid) {
            guard !free.isEmpty else { break }   // more devices than buses: the rest play on the main output
            buses[free.removeFirst()] = uid
        }
        let busOf = Dictionary(buses.map { ($0.value, $0.key) }, uniquingKeysWith: { a, _ in a })
        return (buses, wants.mapValues { busOf[$0] ?? 0 })
    }

    /// Whether an app key is a bundle ID (the driver can match those against new clients itself), as
    /// opposed to an app path, a process name or a Wine program.
    static func isBundleID(_ key: String) -> Bool {
        !key.isEmpty && !key.hasPrefix("/") && !key.contains(":")
    }
}

/// Windows programs run by Wine (also CrossOver, Whisky, Game Porting Toolkit…). They have no bundle ID and
/// all run inside the same executable ("wine64-preloader"), so they are told apart by the program's file
/// name, which is in the process's arguments.
enum WineApp {
    static let keyPrefix = "wine:"

    /// Whether this executable is Wine's loader.
    static func isLoader(executablePath path: String) -> Bool {
        let name = (path as NSString).lastPathComponent.lowercased()
        return name.hasPrefix("wine") && !name.hasPrefix("wineserver")
    }

    /// The Windows program in a Wine process's arguments: the file name of the first argument that is a
    /// path to an .exe ("C:\Games\Foo\Foo.exe" → "Foo.exe").
    static func program(inArguments args: [String]) -> String? {
        for arg in args.prefix(8) {
            let file = arg.split(whereSeparator: { $0 == "\\" || $0 == "/" }).last.map(String.init) ?? ""
            if file.count > 4, file.lowercased().hasSuffix(".exe") { return file }
        }
        return nil
    }

    /// The key an app's settings are saved under, and the name to show.
    static func identity(program: String) -> (key: String, name: String) {
        (keyPrefix + program.lowercased(), String(program.dropLast(4)))
    }

    /// The Wine program a process runs, if it is one.
    static func identity(pid: pid_t) -> (key: String, name: String)? {
        guard let path = executablePath(pid), isLoader(executablePath: path),
              let args = arguments(pid), let program = program(inArguments: args) else { return nil }
        return identity(program: program)
    }

    static func executablePath(_ pid: pid_t) -> String? {
        var buf = [CChar](repeating: 0, count: 4096)
        let n = proc_pidpath(pid, &buf, UInt32(buf.count))
        return n > 0 ? String(cString: buf) : nil
    }

    /// A process's arguments (KERN_PROCARGS2: argc, the executable's path, then the argument strings).
    /// Wine moves its arguments down by one after starting, so the program comes first; either way works.
    static func arguments(_ pid: pid_t) -> [String]? {
        var mib: [Int32] = [CTL_KERN, KERN_PROCARGS2, pid]
        var size = 0
        guard sysctl(&mib, 3, nil, &size, nil, 0) == 0, size > MemoryLayout<Int32>.size else { return nil }
        var buf = [UInt8](repeating: 0, count: size)
        guard sysctl(&mib, 3, &buf, &size, nil, 0) == 0, size > MemoryLayout<Int32>.size else { return nil }
        return parseArguments(Array(buf[0..<size]))
    }

    static func parseArguments(_ buf: [UInt8]) -> [String]? {
        guard buf.count > 4 else { return nil }
        let argc = Int(buf[0]) | Int(buf[1]) << 8 | Int(buf[2]) << 16 | Int(buf[3]) << 24
        guard argc > 0, argc < 4096 else { return nil }
        var i = 4
        while i < buf.count, buf[i] != 0 { i += 1 }      // the executable's path
        while i < buf.count, buf[i] == 0 { i += 1 }      // padding
        var args: [String] = []
        while i < buf.count, args.count < argc {
            var j = i
            while j < buf.count, buf[j] != 0 { j += 1 }
            args.append(String(decoding: buf[i..<j], as: UTF8.self))
            i = j + 1
        }
        return args
    }
}
