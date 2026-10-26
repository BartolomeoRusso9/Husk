// SPDX-License-Identifier: GPL-2.0-or-later
// The side menu, from the avatar on Home: the account, Husk's places, quick actions, and the version.
import SwiftUI
import UIKit

struct SideMenu: View {
    @ObservedObject private var router = Router.shared
    @ObservedObject private var account = Account.shared
    @State private var showHelp = false

    private let width: CGFloat = min(320, UIScreen.main.bounds.width * 0.82)

    var body: some View {
        ZStack(alignment: .leading) {
            if router.showSideMenu {
                Color.black.opacity(0.5)
                    .ignoresSafeArea()
                    .onTapGesture { close() }
                    .transition(.opacity)
                panel
                    .frame(width: width)
                    .frame(maxHeight: .infinity)
                    .background(Console.page.ignoresSafeArea())
                    .overlay(alignment: .trailing) { Rectangle().fill(Console.outline).frame(width: 0.5).ignoresSafeArea() }
                    .transition(.move(edge: .leading))
                    .gesture(DragGesture().onEnded { if $0.translation.width < -60 { close() } })
            }
        }
        .animation(.snappy(duration: 0.28), value: router.showSideMenu)
        .sheet(isPresented: $showHelp) { NavigationStack { FAQTab() } }
    }

    private func close() { router.showSideMenu = false }
    private func go(_ action: @escaping () -> Void) {
        close()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: action)
    }

    private var panel: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button { go { router.showProfile = true } } label: {
                HStack(spacing: 14) {
                    AccountAvatar(size: 56)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(account.user?.displayName ?? "Sign In")
                            .font(.system(size: 20, weight: .bold))
                            .foregroundStyle(Console.text)
                            .lineLimit(1)
                        Text(account.user.map { "@" + $0.username } ?? "Your games on every device")
                            .font(.system(size: 13))
                            .foregroundStyle(Console.textDim)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 20)
            .padding(.top, 20)
            .padding(.bottom, 22)

            VStack(spacing: 4) {
                row("Home", "house.fill", selected: router.tab == .home) { go { router.tab = .home } }
                row("Library", "books.vertical.fill", selected: router.tab == .library) { go { router.tab = .library } }
                row("Store", "bag.fill", selected: router.tab == .store) { go { router.tab = .store } }
                row("Downloads", "arrow.down.circle.fill", selected: false) { go { router.showDownloads = true } }
                row("Settings", "gearshape.fill", selected: router.tab == .settings) { go { router.tab = .settings } }
            }
            .padding(.horizontal, 12)

            Text("Quick Actions")
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Console.textDim)
                .padding(.horizontal, 24)
                .padding(.top, 26)
                .padding(.bottom, 8)
            VStack(spacing: 4) {
                row("Import APK", "square.and.arrow.down.fill", selected: false) { go { router.showImport = true } }
                row("Get Games", "square.grid.2x2.fill", selected: false) { go { router.tab = .store } }
                row("Help & Support", "questionmark.circle.fill", selected: false) { go { showHelp = true } }
            }
            .padding(.horizontal, 12)

            Spacer(minLength: 20)

            Button { go { router.tab = .settings } } label: {
                HStack(spacing: 12) {
                    HuskLogo(size: 30)
                        .frame(width: 40, height: 40)
                        .background(Theme.accent.opacity(0.25), in: Circle())
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Husk \(AppUpdates.current)").font(.system(size: 15, weight: .semibold)).foregroundStyle(Console.text)
                        Text("Build \(CompatibilityStore.build)").font(.system(size: 12)).foregroundStyle(Console.textDim)
                    }
                    Spacer(minLength: 0)
                    Image(systemName: "chevron.right").font(.system(size: 12, weight: .bold)).foregroundStyle(Console.textDim)
                }
                .padding(12)
                .background(Console.card, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).stroke(Console.outline, lineWidth: 0.5))
            }
            .buttonStyle(CardButtonStyle())
            .padding(.horizontal, 16)
            .padding(.bottom, 16)
        }
    }

    private func row(_ title: String, _ icon: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Image(systemName: icon)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(selected ? Theme.accent : Console.text.opacity(0.85))
                    .frame(width: 24)
                Text(title)
                    .font(.system(size: 16, weight: selected ? .semibold : .regular))
                    .foregroundStyle(Console.text)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .frame(height: 46)
            .background(selected ? Console.card : Color.clear, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

// MARK: - profile

/// The account's page: who, how much has been played, what lately, and the achievements. Signed out, the way in.
struct ProfileView: View {
    @ObservedObject private var account = Account.shared
    @Environment(\.dismiss) private var dismiss
    @State private var editing = false

    var body: some View {
        NavigationStack {
            Group {
                if let user = account.user, account.signedIn { profile(user) } else { SignInView() }
            }
            .background(Console.page.ignoresSafeArea())
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
                if account.signedIn {
                    ToolbarItem(placement: .primaryAction) {
                        Menu {
                            Button { editing = true } label: { Label("Edit Profile", systemImage: "pencil") }
                            if let u = account.user {
                                HuskShareLink(item: CompatibilityStore.site.appendingPathComponent("u").appendingPathComponent(u.username)) {
                                    Label("Share Profile", systemImage: "square.and.arrow.up")
                                }
                            }
                            Button(role: .destructive) { Task { await account.signOut() } } label: {
                                Label("Sign Out", systemImage: "rectangle.portrait.and.arrow.right")
                            }
                        } label: { Image(systemName: "ellipsis.circle") }
                    }
                }
            }
            .sheet(isPresented: $editing) { EditProfileView() }
            .task { await account.refresh(); await account.syncLibrary() }
        }
        .environment(\.colorScheme, .dark)
    }

    private func profile(_ u: HuskUser) -> some View {
        ScrollView {
            VStack(spacing: 22) {
                VStack(spacing: 8) {
                    AccountAvatar(size: 92)
                    Text(u.displayName).font(.system(size: 24, weight: .bold)).foregroundStyle(Console.text)
                    Text("@" + u.username).font(.system(size: 14)).foregroundStyle(Console.textDim)
                }
                .padding(.top, 8)

                HStack {
                    stat("\(u.stats.games)", "Games")
                    stat("\(u.stats.apps)", "Apps")
                    stat(Playtime.text(u.stats.playtimeSeconds), "Total Playtime")
                }

                if !u.recent.isEmpty {
                    VStack(alignment: .leading, spacing: 12) {
                        Text("Recently Played").font(.system(size: 18, weight: .bold)).foregroundStyle(Console.text)
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(alignment: .top, spacing: 12) {
                                ForEach(u.recent) { p in recentTile(p) }
                            }
                        }
                    }
                }

                VStack(alignment: .leading, spacing: 12) {
                    Text("Achievements").font(.system(size: 18, weight: .bold)).foregroundStyle(Console.text)
                    ForEach(u.achievements) { a in
                        HStack(spacing: 14) {
                            Image(systemName: a.unlocked ? "trophy.fill" : "lock.fill")
                                .font(.system(size: 18, weight: .semibold))
                                .foregroundStyle(a.unlocked ? Theme.accent : Console.textDim)
                                .frame(width: 44, height: 44)
                                .background(Console.raised, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                            VStack(alignment: .leading, spacing: 2) {
                                Text(a.title).font(.system(size: 15, weight: .semibold)).foregroundStyle(Console.text)
                                Text(a.detail).font(.system(size: 12)).foregroundStyle(Console.textDim)
                            }
                            Spacer(minLength: 0)
                        }
                        .padding(12)
                        .background(Console.card, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                        .opacity(a.unlocked ? 1 : 0.6)
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 30)
        }
    }

    private func stat(_ value: String, _ label: String) -> some View {
        VStack(spacing: 4) {
            Text(value).font(.system(size: 22, weight: .bold)).foregroundStyle(Console.text).lineLimit(1).minimumScaleFactor(0.7)
            Text(label).font(.system(size: 12)).foregroundStyle(Console.textDim)
        }
        .frame(maxWidth: .infinity)
    }

    /// A recently played game: the icon from this iPhone when it is here, its name and when.
    private func recentTile(_ p: HuskUser.Played) -> some View {
        let here = Launcher.items(store: TranslationLayerStore.shared, host: AndroidHost.shared).first { $0.accountID == p.package }
        return VStack(alignment: .leading, spacing: 6) {
            Group {
                if let here { ItemIcon(item: here, size: 96) } else {
                    RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Console.raised).frame(width: 96, height: 96)
                        .overlay(Image(systemName: "gamecontroller.fill").foregroundStyle(Console.textDim))
                }
            }
            Text(p.name).font(.system(size: 13, weight: .semibold)).foregroundStyle(Console.text).lineLimit(1)
            if let s = p.lastPlayed, let d = ISO8601DateFormatter().date(from: s) {
                Text(RelativeDateTimeFormatter().localizedString(for: d, relativeTo: Date()))
                    .font(.system(size: 11)).foregroundStyle(Console.textDim).lineLimit(1)
            }
        }
        .frame(width: 96, alignment: .leading)
    }
}

/// Sign in, or make an account.
struct SignInView: View {
    @ObservedObject private var account = Account.shared
    @State private var creating = false
    @State private var username = ""
    @State private var password = ""
    @State private var displayName = ""
    @State private var error: String?

    var body: some View {
        ScrollView {
            VStack(spacing: 18) {
                HuskLogo(size: 64).padding(.top, 24)
                Text(creating ? "Create a Husk Account" : "Sign In to Husk")
                    .font(.system(size: 26, weight: .bold)).foregroundStyle(Console.text)
                Text("Your profile, playtime and achievements, on this iPhone and on antiweb.lol.")
                    .font(.subheadline).foregroundStyle(Console.textDim).multilineTextAlignment(.center)
                VStack(spacing: 10) {
                    field("Username", text: $username)
                    if creating { field("Display name", text: $displayName) }
                    SecureField("Password", text: $password)
                        .textContentType(creating ? .newPassword : .password)
                        .padding(.horizontal, 14).frame(height: 48)
                        .background(Console.card, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                }
                if let error { Text(error).font(.footnote).foregroundStyle(.orange).multilineTextAlignment(.center) }
                Button { Task { await submit() } } label: {
                    if account.busy { ProgressView().tint(.white) } else { Text(creating ? "Create Account" : "Sign In") }
                }
                .buttonStyle(ConsolePlayButtonStyle())
                .disabled(account.busy || username.count < 3 || password.count < (creating ? 8 : 1))
                Button(creating ? "I already have an account" : "Create an account") {
                    withAnimation { creating.toggle(); error = nil }
                }
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Theme.accent)
            }
            .padding(.horizontal, 24)
        }
    }

    private func field(_ title: String, text: Binding<String>) -> some View {
        TextField(title, text: text)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .padding(.horizontal, 14).frame(height: 48)
            .background(Console.card, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    private func submit() async {
        error = nil
        let name = username.trimmingCharacters(in: .whitespaces).lowercased()
        do {
            if creating {
                try await account.signUp(username: name, password: password,
                                         displayName: displayName.isEmpty ? username : displayName)
            } else {
                try await account.signIn(username: name, password: password)
            }
        } catch {
            self.error = error.localizedDescription
        }
    }
}

/// The name and picture others see.
struct EditProfileView: View {
    @ObservedObject private var account = Account.shared
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var picking = false
    @State private var picked: UIImage?
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack {
                        Spacer()
                        Button { picking = true } label: {
                            Group {
                                if let picked { Image(uiImage: picked).resizable().scaledToFill().frame(width: 92, height: 92).clipShape(Circle()) }
                                else { AccountAvatar(size: 92) }
                            }
                            .overlay(alignment: .bottomTrailing) {
                                Image(systemName: "camera.fill").font(.system(size: 13)).foregroundStyle(.white)
                                    .frame(width: 30, height: 30).background(Theme.accent, in: Circle())
                            }
                        }
                        .buttonStyle(.plain)
                        Spacer()
                    }
                    .listRowBackground(Color.clear)
                }
                Section("Display Name") { TextField("Name", text: $name) }
                if account.avatar != nil {
                    Section { Button("Remove Picture", role: .destructive) { Task { try? await account.update(avatarPNG: .some(nil)) } } }
                }
                if let error { Section { Text(error).foregroundStyle(.orange) } }
            }
            .navigationTitle("Edit Profile")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        Task {
                            do {
                                let png = picked.flatMap(Account.avatarPNG)
                                try await account.update(displayName: name.isEmpty ? nil : name, avatarPNG: png.map { .some($0) } ?? nil)
                                dismiss()
                            } catch { self.error = error.localizedDescription }
                        }
                    }
                    .disabled(account.busy)
                }
            }
            .onAppear { name = account.user?.displayName ?? "" }
            .sheet(isPresented: $picking) { PhotoPicker(image: $picked) }
        }
    }
}

/// The photo library, for one picture.
private struct PhotoPicker: UIViewControllerRepresentable {
    @Binding var image: UIImage?
    @Environment(\.dismiss) private var dismiss

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let p = UIImagePickerController()
        p.sourceType = .photoLibrary
        p.delegate = context.coordinator
        return p
    }
    func updateUIViewController(_ c: UIImagePickerController, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator(self) }

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let parent: PhotoPicker
        init(_ p: PhotoPicker) { parent = p }
        func imagePickerController(_ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]) {
            parent.image = info[.originalImage] as? UIImage
            parent.dismiss()
        }
        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) { parent.dismiss() }
    }
}
