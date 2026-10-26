// SPDX-License-Identifier: GPL-2.0-or-later
// Husk's mark: an H of two slanted bars joined by a slanted crossbar, and the launch screen that shows it.
import SwiftUI

/// The H, drawn in a square: two parallelogram uprights leaning right, and a crossbar between them rising to the right.
struct HuskHShape: Shape {
    func path(in r: CGRect) -> Path {
        let s = min(r.width, r.height), x0 = r.midX - s / 2, y0 = r.midY - s / 2
        func pt(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x0 + x * s, y: y0 + y * s) }
        var p = Path()
        // left upright
        p.addLines([pt(0.20, 0.30), pt(0.42, 0.12), pt(0.42, 0.88), pt(0.20, 0.98)]); p.closeSubpath()
        // right upright
        p.addLines([pt(0.58, 0.02), pt(0.80, 0.12), pt(0.80, 0.70), pt(0.58, 0.88)]); p.closeSubpath()
        // crossbar, rising to the right
        p.addLines([pt(0.42, 0.52), pt(0.58, 0.40), pt(0.58, 0.56), pt(0.42, 0.68)]); p.closeSubpath()
        return p
    }
}

/// The mark in white, with its faint indigo edge, at a size.
struct HuskLogo: View {
    var size: CGFloat = 64
    var body: some View {
        HuskHShape()
            .fill(Color.white)
            .frame(width: size, height: size)
            .shadow(color: AppTheme.husk.opacity(0.55), radius: size * 0.12)
            .accessibilityLabel("Husk")
    }
}

/// What shows while Husk opens: the mark, the name and the line under it, on the navy page.
struct SplashScreen: View {
    @State private var shown = false
    var body: some View {
        ZStack {
            Console.page.ignoresSafeArea()
            VStack(spacing: 14) {
                HuskLogo(size: 92)
                Text("Husk")
                    .font(.system(size: 46, weight: .bold, design: .rounded))
                    .foregroundStyle(.white)
                Text("Play more. Anywhere.")
                    .font(.system(size: 16, weight: .medium))
                    .foregroundStyle(.white.opacity(0.6))
            }
            .opacity(shown ? 1 : 0)
            .scaleEffect(shown ? 1 : 0.96)
        }
        .environment(\.colorScheme, .dark)
        .onAppear { withAnimation(.easeOut(duration: 0.35)) { shown = true } }
    }
}
