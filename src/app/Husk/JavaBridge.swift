// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import UIKit
import Photos
import UniformTypeIdentifiers
import UserNotifications

/// The bridge between a Java app running in Husk and the iPhone's own apps:
/// - its notifications are iOS notifications (NotificationManager.notify, through husk_java_set_bridge); tapping one opens Husk on
///   the app that posted it;
/// - what it shares (ACTION_SEND: text, pictures, files) opens the iPhone's share sheet;
/// - what an iPhone app shares with Husk ("Open in Husk" from the share sheet or Files) goes to the app running in Husk, as an
///   ACTION_SEND to the activity that takes it;
/// - its links open in the iPhone app that handles them: web pages in Safari, tel: in Phone, mailto: in Mail, geo: in Maps
///   (`iosURL`, used by the open-URL handler).
@MainActor
final class JavaBridge: NSObject, UNUserNotificationCenterDelegate {
    static let shared = JavaBridge()
    private var asked = false

    /// Once, at launch: Husk shows notifications in front too (an Android app's are shown while it is open), and a tap opens the app.
    static func install() {
        UNUserNotificationCenter.current().delegate = shared
        husk_java_set_bridge({ text, paths, n in
            let t = text.map { String(cString: $0) }
            var files: [URL] = []
            for i in 0..<Int(n) { if let p = paths?[i] { files.append(URL(fileURLWithPath: String(cString: p))) } }
            DispatchQueue.main.async { JavaBridge.share(text: t, files: files) }
        }, { key, title, text in
            guard let key else { return }
            let k = String(cString: key), ti = title.map { String(cString: $0) } ?? "", tx = text.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async { JavaBridge.shared.post(key: k, title: ti, text: tx) }
        }, { key in
            let k = key.map { String(cString: $0) }
            DispatchQueue.main.async { JavaBridge.shared.cancel(key: k) }
        })
        husk_java_set_media_saver { path, mime in
            guard let path else { return }
            let url = URL(fileURLWithPath: String(cString: path)), m = mime.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async { JavaBridge.saveToPhotos(url, mime: m) }
        }
    }

    // MARK: - the gallery

    /// A photo or video the Android app saved to its gallery (MediaStore), added to the iPhone's Photos. Anything else
    /// (a PDF, a song) goes to the share sheet, where Save to Files is.
    static func saveToPhotos(_ url: URL, mime: String) {
        let video = mime.hasPrefix("video/"), image = mime.hasPrefix("image/")
        guard video || image else { share(text: nil, files: [url]); return }
        PHPhotoLibrary.requestAuthorization(for: .addOnly) { status in
            guard status == .authorized || status == .limited else {
                HuskLog.log("tl", "gallery: Photos access refused; offering the file to share instead")
                DispatchQueue.main.async { share(text: nil, files: [url]) }
                return
            }
            PHPhotoLibrary.shared().performChanges({
                let req = PHAssetCreationRequest.forAsset()
                req.addResource(with: video ? .video : .photo, fileURL: url, options: nil)
            }) { ok, err in
                if ok {
                    HuskLog.log("tl", "gallery: \(url.lastPathComponent) added to Photos")
                    try? FileManager.default.removeItem(at: url)
                } else {
                    HuskLog.log("tl", "gallery: Photos refused \(url.lastPathComponent): \(err?.localizedDescription ?? "unknown")")
                    DispatchQueue.main.async { share(text: nil, files: [url]) }
                }
            }
        }
    }

    // MARK: - notifications

    private func identifier(_ app: TLApp, _ key: String) -> String { "husk.java.\(app.id).\(key)" }

    func post(key: String, title: String, text: String) {
        guard let app = CrashReport.running else { return }
        let center = UNUserNotificationCenter.current()
        let id = identifier(app, key)
        let send = {
            let c = UNMutableNotificationContent()
            c.title = title.isEmpty ? app.label : title
            if !title.isEmpty { c.subtitle = app.label }
            c.body = text
            c.sound = .default
            c.threadIdentifier = app.id
            c.userInfo = ["husk_app": app.id]
            center.add(UNNotificationRequest(identifier: id, content: c, trigger: nil)) { err in
                if let err { HuskLog.log("tl", "notification not shown: \(err.localizedDescription)") }
            }
        }
        if asked { send(); return }
        asked = true
        center.requestAuthorization(options: [.alert, .sound, .badge]) { ok, _ in
            HuskLog.log("tl", "notifications \(ok ? "allowed" : "not allowed")")
            if ok { DispatchQueue.main.async(execute: send) }
        }
    }

    func cancel(key: String?) {
        guard let app = CrashReport.running else { return }
        let center = UNUserNotificationCenter.current()
        if let key {
            let id = identifier(app, key)
            center.removeDeliveredNotifications(withIdentifiers: [id])
            center.removePendingNotificationRequests(withIdentifiers: [id])
            return
        }
        let prefix = "husk.java.\(app.id)."
        center.getDeliveredNotifications { list in
            center.removeDeliveredNotifications(withIdentifiers: list.map(\.request.identifier).filter { $0.hasPrefix(prefix) })
        }
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                            withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .list, .sound])
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                            withCompletionHandler completionHandler: @escaping () -> Void) {
        let id = response.notification.request.content.userInfo["husk_app"] as? String
        DispatchQueue.main.async {
            // the app is already on screen: nothing to open
            if let id, CrashReport.running?.id != id { AppClip.handle(AppClip.link(for: id)) }
            completionHandler()
        }
    }

    // MARK: - sharing out

    static func share(text: String?, files: [URL]) {
        var items: [Any] = files
        if let text, !text.isEmpty { items.insert(text, at: 0) }
        guard !items.isEmpty, let top = topController() else { return }
        let sheet = UIActivityViewController(activityItems: items, applicationActivities: nil)
        if let pop = sheet.popoverPresentationController {
            pop.sourceView = top.view
            pop.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 1, height: 1)
            pop.permittedArrowDirections = []
        }
        top.present(sheet, animated: true)
    }

    // MARK: - sharing in

    /// A file another iPhone app opened Husk with that is not an Android app: a copy goes to the app running in Husk. True when it
    /// was handed over.
    @discardableResult
    static func receive(_ url: URL) -> Bool {
        guard url.isFileURL else { return false }
        guard CrashReport.running != nil else {
            alert("Open an App First", "Open the Android app you want to send this to in Husk, then share it with Husk again.")
            return false
        }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("Shared/\(UUID().uuidString)", isDirectory: true)
        let copy = dir.appendingPathComponent(url.lastPathComponent)
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: url, to: copy)
        } catch {
            HuskLog.log("tl", "shared file not copied: \(error.localizedDescription)")
            return false
        }
        let type = UTType(filenameExtension: url.pathExtension)
        let mime = type?.preferredMIMEType ?? "application/octet-stream"
        // a text file is shared as its text, the way an app's share sheet sends text
        if type?.conforms(to: .plainText) == true, let text = try? String(contentsOf: copy, encoding: .utf8) {
            return husk_java_incoming("text/plain", text, 0)
        }
        HuskLog.log("tl", "shared with the running app: \(url.lastPathComponent) (\(mime))")
        return husk_java_incoming(mime, copy.path, 1)
    }

    /// Text or a link (husk://share?text=…, which a Shortcut can open): to the running app.
    static func receive(text: String) {
        guard CrashReport.running != nil else {
            alert("Open an App First", "Open the Android app you want to send this to in Husk, then share it again.")
            return
        }
        _ = husk_java_incoming("text/plain", text, 0)
    }

    /// husk://share?text=… True when the URL was one.
    static func handle(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "husk", url.host?.lowercased() == "share",
              let text = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "text" })?.value
        else { return false }
        receive(text: text)
        return true
    }

    // MARK: - links

    /// An Android app's link as the iPhone opens it: geo: becomes Apple Maps, an intent:// link its web fallback, market: the Play
    /// Store's page; tel:, mailto:, sms: and http(s) open as they are.
    nonisolated static func iosURL(_ s: String) -> URL? {
        let lower = s.lowercased()
        if lower.hasPrefix("geo:") {
            // geo:lat,lng?q=place  or  geo:0,0?q=address
            let parts = String(s.dropFirst(4)).split(separator: "?", maxSplits: 1).map(String.init)
            var c = URLComponents(string: "https://maps.apple.com/")!
            var q: [URLQueryItem] = []
            if let coords = parts.first, coords != "0,0", !coords.isEmpty { q.append(URLQueryItem(name: "ll", value: coords)) }
            if parts.count > 1, let query = URLComponents(string: "x:?" + parts[1])?.queryItems?.first(where: { $0.name == "q" })?.value {
                q.append(URLQueryItem(name: "q", value: query))
            }
            c.queryItems = q.isEmpty ? nil : q
            return c.url
        }
        if lower.hasPrefix("intent:") {
            // intent://host/path#Intent;scheme=https;...;S.browser_fallback_url=...;end
            if let r = s.range(of: "S.browser_fallback_url=") {
                let v = s[r.upperBound...].split(separator: ";").first.map(String.init) ?? ""
                if let u = v.removingPercentEncoding, let url = URL(string: u) { return url }
            }
            if let hash = s.firstIndex(of: "#"), let r = s.range(of: "scheme="), lower.hasPrefix("intent://") {
                let scheme = s[r.upperBound...].split(separator: ";").first.map(String.init) ?? "https"
                let body = s[s.index(s.startIndex, offsetBy: 9)..<hash]
                return URL(string: "\(scheme)://\(body)")
            }
            return nil
        }
        if lower.hasPrefix("market://") { return URL(string: "https://play.google.com/store/apps/" + s.dropFirst(9)) }
        return URL(string: s) ?? s.addingPercentEncoding(withAllowedCharacters: .urlFragmentAllowed).flatMap(URL.init(string:))
    }

    // MARK: -

    private static func alert(_ title: String, _ message: String) {
        let a = UIAlertController(title: title, message: message, preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "OK", style: .default))
        topController()?.present(a, animated: true)
    }

    private static func topController() -> UIViewController? {
        var top = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first?.rootViewController
        while let next = top?.presentedViewController { top = next }
        return top
    }
}
