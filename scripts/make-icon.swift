// Renders the app icon into an .iconset folder: swiftc make-icon.swift -o mkicon && ./mkicon Out.iconset
import AppKit

let out = CommandLine.arguments[1]
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)

func render(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)
    let inset = s * 0.1
    let rect = NSRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let path = NSBezierPath(roundedRect: rect, xRadius: rect.width * 0.225, yRadius: rect.width * 0.225)
    NSGradient(colors: [NSColor(calibratedRed: 0.36, green: 0.30, blue: 0.98, alpha: 1),
                        NSColor(calibratedRed: 0.10, green: 0.62, blue: 0.98, alpha: 1)])!.draw(in: path, angle: -60)

    // three sliders with knobs at different heights
    let trackW = rect.width * 0.075
    let knob = rect.width * 0.17
    let levels: [CGFloat] = [0.62, 0.32, 0.78]
    for (i, lvl) in levels.enumerated() {
        let cx = rect.minX + rect.width * (0.27 + 0.23 * CGFloat(i))
        let top = rect.minY + rect.height * 0.80, bottom = rect.minY + rect.height * 0.20
        NSColor.white.withAlphaComponent(0.35).setFill()
        NSBezierPath(roundedRect: NSRect(x: cx - trackW / 2, y: bottom, width: trackW, height: top - bottom),
                     xRadius: trackW / 2, yRadius: trackW / 2).fill()
        let ky = bottom + (top - bottom) * lvl
        NSColor.white.setFill()
        NSBezierPath(roundedRect: NSRect(x: cx - trackW / 2, y: bottom, width: trackW, height: ky - bottom),
                     xRadius: trackW / 2, yRadius: trackW / 2).fill()
        let shadow = NSShadow(); shadow.shadowBlurRadius = s * 0.015; shadow.shadowOffset = NSSize(width: 0, height: -s * 0.006)
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.25); shadow.set()
        NSBezierPath(ovalIn: NSRect(x: cx - knob / 2, y: ky - knob / 2, width: knob, height: knob)).fill()
        NSShadow().set()
    }
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for base in [16, 32, 128, 256, 512] {
    try! render(base).write(to: URL(fileURLWithPath: "\(out)/icon_\(base)x\(base).png"))
    try! render(base * 2).write(to: URL(fileURLWithPath: "\(out)/icon_\(base)x\(base)@2x.png"))
}
