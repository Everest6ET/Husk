// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The two halves of the library.
enum LibrarySide: String, CaseIterable, Identifiable {
    /// Games that run on the iPhone itself, without Android.
    case translation
    /// Android, emulated, and the apps installed inside it.
    case emulation

    var id: String { rawValue }

    var title: String {
        switch self {
        case .translation: return "Translation Layer"
        case .emulation: return "Emulation"
        }
    }
}

/// The library: every app Husk can open, on two pages a swipe apart.
///
/// Translation Layer holds the games that run straight on the iPhone; Emulation holds Android itself -- start it,
/// watch it boot, open it -- and the apps installed inside it. A tap opens an app's page; playing or opening it, and
/// its settings, are also a long press away. One navigation stack serves both pages, so a page pushed from either comes back to the same place.
struct LibraryHome: View {
    let started: Bool
    let onOpenGuest: () -> Void
    let onStartAndroid: () -> Void

    @ObservedObject private var router = Router.shared
    @ObservedObject private var store = TranslationLayerStore.shared
    @ObservedObject private var host = AndroidHost.shared
    @AppStorage("husk.library.side") private var side: LibrarySide = .translation
    /// A translation-layer game started from the grid.
    @State private var playing: TLApp?
    /// What the search field holds: both sides are filtered by it.
    @State private var query = ""

    var body: some View {
        NavigationStack(path: $router.library) {
            VStack(spacing: 0) {
                header
                TabView(selection: $side) {
                    TranslationPage(playing: $playing, query: query, add: addGames)
                        .tag(LibrarySide.translation)
                    EmulationPage(started: started, onOpenGuest: onOpenGuest, onStartAndroid: onStartAndroid,
                                  query: query, install: installApps)
                        .tag(LibrarySide.emulation)
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
            }
            .background(Color(uiColor: .systemBackground).ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: LibraryRoute.self) { route in
                Group {
                    switch route {
                    case .android(let app):
                        AppDetailView(app: app, onOpenGuest: onOpenGuest)
                    case .game(let id):
                        if let app = store.apps.first(where: { $0.id == id }) { TLAppReportView(app: app) }
                    case .gameSettings(let id):
                        if let app = store.apps.first(where: { $0.id == id }) { TLAppSettingsView(app: app) }
                    }
                }
                .toolbar(.visible, for: .navigationBar)
            }
            // A game is swiped and tapped all over; a sheet would take a swipe down for itself, so it goes full screen.
            .fullScreenCover(item: $playing) { TLAttemptView(app: $0) }
            .onAppear { store.adoptDroppedAPKs() }
            .alert("Could not add the app", isPresented: Binding(
                    get: { store.lastError != nil },
                    set: { if !$0 { store.lastError = nil } })) {
                Button("OK", role: .cancel) { store.lastError = nil }
            } message: {
                Text(store.lastError ?? "")
            }
        }
    }

    // MARK: header

    private var header: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .center, spacing: 10) {
                Text("Library")
                    .font(.largeTitle.weight(.bold))
                Spacer()
                if side == .emulation, host.isReady {
                    HeaderButton(systemImage: "folder") { router.openFiles(at: FilesTab.root) }
                        .accessibilityLabel("Android Files")
                }
                HeaderButton(systemImage: "plus") {
                    if side == .translation { addGames() } else { installApps() }
                }
                .disabled(side == .translation && store.busy != nil)
                .accessibilityLabel(side == .translation ? "Add a Game" : "Install an APK")
            }
            SearchField(text: $query, prompt: side == .translation ? "Search games" : "Search apps")
            SideSwitcher(side: $side)
        }
        .padding(.horizontal, 20)
        .padding(.top, 8)
        .padding(.bottom, 6)
    }

    private func addGames() {
        HuskFilePicker.present { urls in
            HuskLog.log("ui", "translation layer: adding \(urls.count) file(s): "
                      + urls.map(\.lastPathComponent).joined(separator: ", "))
            store.add(urls)
        }
    }

    private func installApps() {
        HuskFilePicker.present { urls in
            HuskLog.log("ui", "importing \(urls.count) file(s): " + urls.map(\.lastPathComponent).joined(separator: ", "))
            host.install(urls)
        }
    }
}

// MARK: - the switcher

/// The two sides as a pair of segments. The selected one sits on a raised capsule that slides between them, and
/// follows a swipe of the pages below as well as a tap here.
private struct SideSwitcher: View {
    @Binding var side: LibrarySide
    @Namespace private var selection

    var body: some View {
        HStack(spacing: 0) {
            ForEach(LibrarySide.allCases) { option in
                Button {
                    withAnimation(.snappy(duration: 0.28)) { side = option }
                } label: {
                    Text(option.title)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(side == option ? Color.primary : Color.secondary)
                        .frame(maxWidth: .infinity)
                        .frame(height: 34)
                        .background {
                            if side == option {
                                Capsule()
                                    .fill(Color(uiColor: .secondarySystemGroupedBackground))
                                    .shadow(color: .black.opacity(0.10), radius: 3, y: 1)
                                    .matchedGeometryEffect(id: "selected", in: selection)
                            }
                        }
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
            }
        }
        .padding(3)
        .background(Color(uiColor: .tertiarySystemFill), in: Capsule())
        .animation(.snappy(duration: 0.28), value: side)
    }
}

/// A round glyph button in the header.
private struct HeaderButton: View {
    let systemImage: String
    let action: () -> Void
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(enabled ? Color.accentColor : Color.secondary)
                .frame(width: 36, height: 36)
                .background(Color(uiColor: .tertiarySystemFill), in: Circle())
        }
        .buttonStyle(.plain)
    }
}

/// A search field as the system draws one, kept in the header so it filters whichever side is showing.
private struct SearchField: View {
    @Binding var text: String
    let prompt: String
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.secondary)
                TextField(prompt, text: $text)
                    .focused($focused)
                    .submitLabel(.search)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                if !text.isEmpty {
                    Button { text = "" } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.tertiary)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Clear")
                }
            }
            .padding(.horizontal, 10)
            .frame(height: 36)
            .background(Color(uiColor: .tertiarySystemFill), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            if focused {
                Button("Cancel") { text = ""; focused = false }
                    .transition(.move(edge: .trailing).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.2), value: focused)
    }
}

/// Nothing on this side matches the search.
private struct NoResults: View {
    let query: String
    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "magnifyingglass").font(.system(size: 28, weight: .light)).foregroundStyle(.secondary)
            Text("No Results").font(.headline)
            Text("Nothing here is called “\(query)”.").font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 48)
    }
}

/// Whether a name matches what was searched for.
private func matches(_ query: String, _ names: String...) -> Bool {
    let q = query.trimmingCharacters(in: .whitespaces)
    return q.isEmpty || names.contains { $0.localizedCaseInsensitiveContains(q) }
}

// MARK: - the grid

/// The grid both pages lay their apps out on: as many columns as fit, with room around each icon.
private let launcherColumns = [GridItem(.adaptive(minimum: 78, maximum: 110), spacing: 14, alignment: .top)]

/// One app in the grid, as a home screen draws it: the icon, and its name under it. A caption only when there is
/// something worth saying.
struct LauncherTile: View {
    let title: String
    let iconPath: String?
    var caption: String? = nil
    var dimmed = false

    private let size: CGFloat = 64

    var body: some View {
        VStack(spacing: 8) {
            AppIcon(path: iconPath, size: size)
                .overlay {
                    RoundedRectangle(cornerRadius: size * 0.225, style: .continuous)
                        .strokeBorder(Color.primary.opacity(0.08), lineWidth: 0.5)
                }
                .shadow(color: .black.opacity(0.12), radius: 6, y: 3)
                .opacity(dimmed ? 0.45 : 1)
            VStack(spacing: 2) {
                Text(title)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.primary)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                if let caption {
                    Text(caption)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .top)
        }
        .frame(maxWidth: .infinity)
        .contentShape(Rectangle())
    }
}

/// Work under way, said in one line.
private struct BusyStrip: View {
    let text: String

    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
            Text(text).font(.subheadline).lineLimit(2)
            Spacer(minLength: 0)
        }
        .padding(14)
        .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

// MARK: - Translation Layer

private struct TranslationPage: View {
    @Binding var playing: TLApp?
    let query: String
    let add: () -> Void

    @ObservedObject private var store = TranslationLayerStore.shared
    @ObservedObject private var router = Router.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @State private var removing: TLApp?

    private var jitOn: Bool { JITBootstrap.isDebuggerAttached || JITBootstrap.debuggedFlag }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                if !jitOn { JITCard(compact: true) }
                if let busy = store.busy { BusyStrip(text: busy) }

                let shown = store.apps.filter { matches(query, $0.label) }
                if store.apps.isEmpty {
                    EmptyState(title: "No Games Yet",
                               message: "Add an APK or a bundle (.xapk, .apkm, .apks). Games here run straight on your "
                                      + "iPhone, without starting Android.",
                               systemImage: "gamecontroller",
                               actionTitle: "Add a Game", action: add)
                } else if shown.isEmpty {
                    NoResults(query: query)
                } else {
                    LazyVGrid(columns: launcherColumns, spacing: 22) {
                        ForEach(shown) { app in tile(app) }
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.top, 14)
            .padding(.bottom, 32)
            // Read again whenever JIT changes, so the card leaves as soon as it is on.
            .id(jit.attachGeneration)
        }
        .scrollIndicators(.hidden)
        .confirmationDialog("Remove \(removing?.label ?? "this game")?", isPresented: Binding(
                get: { removing != nil }, set: { if !$0 { removing = nil } }), titleVisibility: .visible) {
            Button("Remove", role: .destructive) {
                if let app = removing { store.remove(app) }
                removing = nil
            }
        } message: {
            Text("The game and everything it saved here are deleted from Husk.")
        }
    }

    private func tile(_ app: TLApp) -> some View {
        let runs = app.report?.runsOnNativeRuntime == true
        return Button {
            router.library.append(.game(app.id))
        } label: {
            LauncherTile(title: app.label, iconPath: app.iconPath, caption: runs ? nil : "May not run")
        }
        .buttonStyle(CardButtonStyle())
        .contextMenu {
            if runs {
                Button { playing = app } label: { Label("Play", systemImage: "play.fill") }
            }
            Button { router.library.append(.game(app.id)) } label: { Label("Details", systemImage: "info.circle") }
            Button { router.library.append(.gameSettings(app.id)) } label: { Label("Settings", systemImage: "gearshape") }
            Divider()
            Button(role: .destructive) { removing = app } label: { Label("Remove", systemImage: "trash") }
        }
    }
}

// MARK: - Emulation

private struct EmulationPage: View {
    let started: Bool
    let onOpenGuest: () -> Void
    let onStartAndroid: () -> Void
    let query: String
    let install: () -> Void

    @ObservedObject private var host = AndroidHost.shared
    @ObservedObject private var guest = GuestImage.shared
    @ObservedObject private var router = Router.shared
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                AndroidCard(started: started, onOpenGuest: onOpenGuest, onStartAndroid: onStartAndroid)
                if let busy = host.busy { BusyStrip(text: busy) }
                if jit.busy, !jit.showSetup { BusyStrip(text: jit.status ?? "Turning on JIT…") }

                let shown = host.packages.filter { matches(query, $0.label, $0.name) }
                if !host.packages.isEmpty {
                    Text("Apps")
                        .font(.title3.weight(.semibold))
                        .padding(.top, 4)
                    if shown.isEmpty {
                        NoResults(query: query)
                    } else {
                        LazyVGrid(columns: launcherColumns, spacing: 22) {
                            ForEach(shown) { app in tile(app) }
                        }
                    }
                } else if host.isReady {
                    EmptyState(title: "No Apps Yet",
                               message: "Install an APK and it appears here. Split sets work too — pick every piece at once.",
                               systemImage: "square.grid.2x2",
                               actionTitle: "Install an APK", action: install)
                }
            }
            .padding(.horizontal, 20)
            .padding(.top, 14)
            .padding(.bottom, 32)
        }
        .scrollIndicators(.hidden)
    }

    private var canOpen: Bool { host.isReady && host.busy == nil }

    private func tile(_ app: AndroidHost.Package) -> some View {
        Button {
            router.library.append(.android(app))
        } label: {
            LauncherTile(title: app.label, iconPath: app.iconPath, dimmed: !host.isReady)
        }
        .buttonStyle(CardButtonStyle())
        .contextMenu {
            Button { host.launch(app.name) { onOpenGuest() } } label: { Label("Open", systemImage: "play.fill") }
                .disabled(!canOpen)
            Button { router.library.append(.android(app)) } label: { Label("Details", systemImage: "info.circle") }
        }
    }
}

/// Android itself: whether it is here, whether it is running, and the one thing to do next -- download it, start
/// it, or open it.
private struct AndroidCard: View {
    let started: Bool
    let onOpenGuest: () -> Void
    let onStartAndroid: () -> Void

    @ObservedObject private var guest = GuestImage.shared
    @ObservedObject private var runner = QemuRunner.shared
    @ObservedObject private var host = AndroidHost.shared
    @ObservedObject private var jit = JITCoordinator.shared

    private var running: Bool { started }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 14) {
                Image(systemName: "apps.iphone")
                    .font(.system(size: 24, weight: .medium))
                    .foregroundStyle(Color.accentColor)
                    .frame(width: 52, height: 52)
                    .background(Color.accentColor.opacity(0.14),
                                in: RoundedRectangle(cornerRadius: 52 * 0.225, style: .continuous))
                VStack(alignment: .leading, spacing: 3) {
                    Text("Android").font(.headline)
                    HStack(spacing: 6) {
                        if let dot = statusDot { Circle().fill(dot).frame(width: 7, height: 7) }
                        Text(status)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                    }
                }
                Spacer(minLength: 8)
                action
            }
            if let progress {
                ProgressView(value: progress)
                    .tint(Color.accentColor)
            }
            if let note {
                Text(note)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(16)
        .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .animation(.easeInOut(duration: 0.2), value: status)
    }

    private var status: String {
        if running {
            if host.isReady { return "Running" }
            return runner.setupMessage ?? (runner.bootProgress > 0 ? "Starting · \(runner.bootProgress)%" : "Starting…")
        }
        switch guest.state {
        case .missing: return "Not downloaded"
        case .downloading(_, let received, let total):
            return "Downloading · \(bytes(received)) of \(total > 0 ? bytes(total) : "…")"
        case .installing: return "Installing…"
        case .failed: return "Something went wrong"
        case .ready: return JITBootstrap.isDebuggerAttached ? "Ready to start" : "Needs JIT to start"
        }
    }

    private var statusDot: Color? {
        if running { return host.isReady ? .green : .orange }
        if case .failed = guest.state { return .red }
        return nil
    }

    private var progress: Double? {
        if running, !host.isReady, runner.bootProgress > 0 { return Double(runner.bootProgress) / 100 }
        if !running, case .downloading(let p, _, _) = guest.state { return p }
        return nil
    }

    private var note: String? {
        guard !running else { return nil }
        switch guest.state {
        case .missing: return "Husk's Android runtime is about 760 MB. Android itself follows once it is installed."
        case .failed(let message): return message
        default: return nil
        }
    }

    @ViewBuilder
    private var action: some View {
        if running {
            pill("Open", action: onOpenGuest)
        } else {
            switch guest.state {
            case .missing:
                pill("Get") {
                    // Claim the JIT region before the download, not after: it takes a while, and StikDebug will have
                    // let go by the end of it.
                    JITBootstrap.prewarm()
                    guest.download()
                }
            case .downloading:
                pill("Cancel", prominent: false) { guest.cancel() }
            case .installing:
                ProgressView()
            case .failed:
                pill("Retry") { JITBootstrap.prewarm(); guest.download() }
            case .ready:
                pill(JITBootstrap.isDebuggerAttached ? "Start" : "Enable JIT", action: onStartAndroid)
            }
        }
    }

    private func pill(_ title: String, prominent: Bool = true, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.subheadline.weight(.bold))
                .foregroundStyle(prominent ? Color.white : Color.accentColor)
                .padding(.horizontal, 16)
                .frame(height: 32)
                .background(prominent ? Color.accentColor : Color(uiColor: .tertiarySystemFill), in: Capsule())
        }
        .buttonStyle(CardButtonStyle())
    }

    private func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }
}
