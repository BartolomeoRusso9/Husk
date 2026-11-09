// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// "Install as App": an Android app as a real iOS app, on the Home Screen and in the App Library, that opens Husk on it.
///
/// iOS does not let one app install another, so Husk writes the IPA and hands it to whatever installs IPAs on this phone
/// (SideStore, AltStore, TrollStore) through the share sheet. The IPA is HuskShortcut's executable (built with Husk and copied
/// into its resources as HuskShortcut.stub) with an Info.plist and icons of the Android app's own: its name, its icon, and
/// HuskTarget, the id it opens through husk://open_app (AppClip.handle), which starts it.
///
/// Unlike the App Clip (a Web Clip profile), this is an app: it has a place in the App Library, can be searched for and
/// offloaded, and opens without a trip through Safari. With a free Apple ID, SideStore and AltStore count it against the
/// three apps such an account can have installed; TrollStore has no limit.
@MainActor
enum NativeShortcut {
    struct Failure: LocalizedError {
        let errorDescription: String?
        init(_ message: String) { errorDescription = message }
    }

    static var stub: URL? { Bundle.main.url(forResource: "HuskShortcut", withExtension: "stub") }
    static var available: Bool { stub != nil }

    /// com.husk.shortcut.<id>: the same for every IPA made for one app, so a second install replaces the first.
    static func bundleID(for app: TLApp) -> String {
        let cleaned = app.id.unicodeScalars.map { CharacterSet.alphanumerics.contains($0) && $0.isASCII ? Character($0) : "-" }
        var id = String(cleaned).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
        while id.contains("--") { id = id.replacingOccurrences(of: "--", with: "-") }
        return "com.husk.shortcut.\(id.isEmpty ? "app" : id.lowercased())"
    }

    static func infoPlist(for app: TLApp) -> [String: Any] {
        let phone = ["AppIcon60x60"]
        let pad = ["AppIcon60x60", "AppIcon76x76", "AppIcon83.5x83.5"]
        return [
            "CFBundleDevelopmentRegion": "en",
            "CFBundleExecutable": "HuskShortcut",
            "CFBundleIdentifier": bundleID(for: app),
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": String(app.label.prefix(15)),
            "CFBundleDisplayName": app.label,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": "1.0",
            "CFBundleVersion": "1",
            "CFBundleSupportedPlatforms": ["iPhoneOS"],
            "CFBundleIcons": ["CFBundlePrimaryIcon": ["CFBundleIconFiles": phone]],
            "CFBundleIcons~ipad": ["CFBundlePrimaryIcon": ["CFBundleIconFiles": pad]],
            "MinimumOSVersion": "15.5",
            "LSRequiresIPhoneOS": true,
            "UIDeviceFamily": [1, 2],
            "UIRequiredDeviceCapabilities": ["arm64"],
            "UILaunchScreen": [String: Any](),
            "UISupportedInterfaceOrientations": ["UIInterfaceOrientationPortrait", "UIInterfaceOrientationLandscapeLeft",
                                                 "UIInterfaceOrientationLandscapeRight"],
            "UISupportedInterfaceOrientations~ipad": ["UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortraitUpsideDown",
                                                      "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
            "HuskTarget": app.id,
        ]
    }

    /// The IPA, in the temporary folder: Payload/<label>.app with the stub, its Info.plist and its icons.
    static func makeIPA(for app: TLApp) throws -> URL {
        guard let stub else { throw Failure("This copy of Husk was built without the app maker, so it cannot make apps.") }
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("Shortcuts", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let name = fileSafe(app.label)
        let out = folder.appendingPathComponent("\(name).ipa")
        let root = "Payload/\(name).app/"

        let plist = try PropertyListSerialization.data(fromPropertyList: infoPlist(for: app), format: .binary, options: 0)
        let zip = try ZipWriter(out)
        try zip.add(root + "HuskShortcut", file: stub, executable: true)
        try zip.add(root + "Info.plist", data: plist)
        try zip.add(root + "PkgInfo", data: Data("APPL????".utf8))
        let icons: [(String, CGFloat)] = [
            ("AppIcon60x60@2x.png", 120), ("AppIcon60x60@3x.png", 180),
            ("AppIcon76x76@2x~ipad.png", 152), ("AppIcon83.5x83.5@2x~ipad.png", 167),
        ]
        for (file, side) in icons {
            guard let png = AppClip.iconPNG(app, side: side) else { continue }
            try zip.add(root + file, data: png)
        }
        try zip.finish()
        HuskLog.log("ui", "install as app: wrote \(out.lastPathComponent) for \(app.id) as \(bundleID(for: app))")
        return out
    }

    /// Make the IPA and offer it to the share sheet, where SideStore, AltStore or TrollStore takes it.
    static func install(_ app: TLApp) {
        let ipa: URL
        do { ipa = try makeIPA(for: app) } catch {
            HuskLog.log("ui", "install as app: \(error.localizedDescription)")
            alert("Couldn't Make the App", error.localizedDescription)
            return
        }
        guard let top = topController() else { return }
        let sheet = UIActivityViewController(activityItems: [ipa], applicationActivities: nil)
        if let pop = sheet.popoverPresentationController {
            pop.sourceView = top.view
            pop.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 0, height: 0)
            pop.permittedArrowDirections = []
        }
        top.present(sheet, animated: true)
    }

    private static func alert(_ title: String, _ message: String) {
        let a = UIAlertController(title: title, message: message, preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "OK", style: .default))
        topController()?.present(a, animated: true)
    }

    private static func fileSafe(_ s: String) -> String {
        let bad = CharacterSet(charactersIn: "/\\:?*\"<>|").union(.controlCharacters)
        let cleaned = String(s.unicodeScalars.map { bad.contains($0) ? "-" : Character($0) }).trimmingCharacters(in: .whitespaces)
        return cleaned.isEmpty ? "App" : cleaned
    }

    private static func topController() -> UIViewController? {
        var top = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first?.rootViewController
        while let next = top?.presentedViewController { top = next }
        return top
    }
}
