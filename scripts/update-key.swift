//
//  The key that signs updates (Ed25519). The app only installs an update whose signature matches the public
//  key it was built with, so a download that didn't come from the person who built the app is refused.
//
//    swift scripts/update-key.swift generate      make the key (once) and print the public key
//    swift scripts/update-key.swift public        print the public key
//    swift scripts/update-key.swift sign <file>   print the signature of a file
//
//  The private key is kept in the login Keychain of the Mac that makes the releases and never printed.
//  Without it no update can be signed for the apps already out there: back the Keychain item up
//  ("Per-App Volume update key" in Keychain Access).
//
import CryptoKit
import Foundation
import Security

let service = "Per-App Volume update key"
let account = "ed25519"

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

func loadKey() -> Curve25519.Signing.PrivateKey? {
    let query: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: service, kSecAttrAccount: account,
                                  kSecReturnData: true, kSecMatchLimit: kSecMatchLimitOne]
    var out: CFTypeRef?
    guard SecItemCopyMatching(query as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
    return try? Curve25519.Signing.PrivateKey(rawRepresentation: data)
}

func requireKey() -> Curve25519.Signing.PrivateKey {
    guard let key = loadKey() else { fail("No update key in the Keychain. Make one with: swift scripts/update-key.swift generate") }
    return key
}

let args = Array(CommandLine.arguments.dropFirst())
switch args.first {
case "generate":
    if let key = loadKey() {
        // Never replace it: the apps already out there only accept this one.
        FileHandle.standardError.write(Data("The key already exists; this is its public key.\n".utf8))
        print(key.publicKey.rawRepresentation.base64EncodedString())
        break
    }
    let key = Curve25519.Signing.PrivateKey()
    let item: [CFString: Any] = [kSecClass: kSecClassGenericPassword, kSecAttrService: service, kSecAttrAccount: account,
                                 kSecAttrLabel: service, kSecValueData: key.rawRepresentation]
    let status = SecItemAdd(item as CFDictionary, nil)
    guard status == errSecSuccess else { fail("Couldn't save the key in the Keychain (\(status)).") }
    print(key.publicKey.rawRepresentation.base64EncodedString())
case "public":
    print(requireKey().publicKey.rawRepresentation.base64EncodedString())
case "sign":
    guard args.count == 2, let data = FileManager.default.contents(atPath: args[1]) else { fail("usage: update-key.swift sign <file>") }
    guard let signature = try? requireKey().signature(for: data) else { fail("Couldn't sign \(args[1]).") }
    print(signature.base64EncodedString())
default:
    fail("usage: update-key.swift generate | public | sign <file>")
}
