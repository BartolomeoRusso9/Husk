// SPDX-License-Identifier: GPL-2.0-or-later
// The app behind "Install as App" (NativeShortcut.swift): a real iOS app, with an Android app's name and icon, whose only job is
// to open Husk on that app. Husk does not ship this as an .app: its executable is copied into Husk's resources, and Husk writes
// an IPA around it for each app, with the app's own Info.plist (HuskTarget = the app's id) and icons. SideStore, AltStore or
// TrollStore installs that IPA, so the app shows on the Home Screen and in the App Library like any other.
import UIKit

final class ShortcutDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    private var opened = false

    private var target: String? { Bundle.main.object(forInfoDictionaryKey: "HuskTarget") as? String }
    private var name: String {
        (Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)
            ?? (Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String) ?? "App"
    }

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        let w = UIWindow(frame: UIScreen.main.bounds)
        w.rootViewController = ShortcutScreen(name: name)
        w.makeKeyAndVisible()
        window = w
        return true
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        // Every time the icon is tapped: the first launch and each return to the foreground.
        openHusk()
    }

    func applicationDidEnterBackground(_ application: UIApplication) { opened = false }

    private func openHusk() {
        guard !opened, let id = target, !id.isEmpty else { return }
        var c = URLComponents()
        c.scheme = "husk"
        c.host = "open_app"
        c.queryItems = [URLQueryItem(name: "unique_identifier", value: id)]
        guard let url = c.url else { return }
        opened = true
        UIApplication.shared.open(url, options: [:]) { [weak self] ok in
            guard !ok else { return }
            self?.opened = false
            (self?.window?.rootViewController as? ShortcutScreen)?.showHuskMissing()
        }
    }
}

/// What shows for the moment before Husk comes up, or when Husk is not installed.
final class ShortcutScreen: UIViewController {
    private let name: String
    private let label = UILabel()
    private let spinner = UIActivityIndicatorView(style: .large)

    init(name: String) { self.name = name; super.init(nibName: nil, bundle: nil) }
    required init?(coder: NSCoder) { fatalError() }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground
        label.text = "Opening \(name) in Husk…"
        label.font = .preferredFont(forTextStyle: .headline)
        label.textColor = .secondaryLabel
        label.textAlignment = .center
        label.numberOfLines = 0
        let stack = UIStackView(arrangedSubviews: [spinner, label])
        stack.axis = .vertical
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: view.layoutMarginsGuide.leadingAnchor),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: view.layoutMarginsGuide.trailingAnchor),
        ])
        spinner.startAnimating()
    }

    func showHuskMissing() {
        spinner.stopAnimating()
        label.text = "\(name) runs in Husk, and Husk is not installed. Install Husk, then open \(name) again."
    }
}

UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(ShortcutDelegate.self))
