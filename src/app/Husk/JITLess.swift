// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Husk 3.2's experimental JIT-less mode. An Android game's libraries run from Mach-O dylibs made from them ahead of time, with the
/// translation layer's code rewrites already applied, and signed with the app (they ship in Frameworks as aot-<hash>-<library>.dylib).
/// iOS maps signed code as executable on its own, so the game needs no JIT: no debugger, no StikJIT. Experimental: only the games
/// whose libraries were made into dylibs for this build can run this way (see `libraryCount`), and an engine that generates code
/// at run time (Geode's mods, a Mono or LuaJIT JIT) still needs JIT.
enum JITLess {
    static let key = "husk.jitless"

    static var enabled: Bool { UserDefaults.standard.bool(forKey: key) }

    /// The folder the dylibs are in: the app's Frameworks, where the installer signs them with the app.
    static var directory: String? { Bundle.main.privateFrameworksPath }

    /// How many libraries this build carries as dylibs.
    static var libraryCount: Int {
        guard let dir = directory, let names = try? FileManager.default.contentsOfDirectory(atPath: dir) else { return 0 }
        return names.filter { $0.hasPrefix("aot-") && $0.hasSuffix(".dylib") }.count
    }

    /// Before a game starts: no executable memory is made in this run (even when JIT is on, so the test is real), and libraries
    /// come from the dylibs. A library with no dylib cannot load, and the log says which one.
    static func prepareLaunch() {
        guard enabled, let dir = directory else { return }
        setenv("TL_NO_XMEM", "1", 1)
        tl_ld_set_aot_dir(dir)
        HuskLog.log("tl", "JIT-less mode (experimental): libraries from dylibs made ahead of time (\(libraryCount) in this build); no executable memory is made")
    }
}
