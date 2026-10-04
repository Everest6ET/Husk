// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import QuartzCore

/// A Unity game's screen: one CAMetalLayer that the game's own GL (ANGLE over Metal) presents into.
///
/// Nothing is copied or composed here. The runtime hands the layer to EGL as the game's window; the
/// game draws and presents on its own thread. This view's jobs are the layer's size, the pause that
/// goes with leaving the screen, and turning touches into the pixel coordinates Android reports.
final class TLUnityUIView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    private let apk: String
    private let dataDir: String
    private var launched = false
    /// Active touches by UITouch identity, each given a small stable id like Android's pointer ids.
    private var pointers: [ObjectIdentifier: Int32] = [:]

    /// "58 fps · 11.2 ms" in the corner, as the other screen has: frames the game finished per second, and the
    /// mean time one frame takes it. Refreshed once a second from the runtime's own counters.
    private let stats = UILabel()
    private var statsTimer: Timer?

    init(apk: String, dataDir: String) {
        self.apk = apk
        self.dataDir = dataDir
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        // Two pixels per point: sharp enough, and a third of the pixels a 3x phone would ask the game
        // for -- a 3D game is limited by fill rate, and ANGLE's translation costs on top.
        contentScaleFactor = 2
        if let metal = layer as? CAMetalLayer {
            metal.pixelFormat = .bgra8Unorm
            metal.framebufferOnly = true
            metal.contentsScale = 2
            metal.isOpaque = true
        }

        stats.font = .monospacedSystemFont(ofSize: 10, weight: .medium)
        stats.textColor = .white
        stats.backgroundColor = UIColor.black.withAlphaComponent(0.55)
        stats.layer.cornerRadius = 4
        stats.layer.masksToBounds = true
        stats.textAlignment = .center
        stats.isUserInteractionEnabled = false
        stats.text = " "
        addSubview(stats)
    }

    deinit { statsTimer?.invalidate() }

    private func updateStats() {
        var p = husk_unity_perf()
        husk_unity_perf_snapshot(&p)
        stats.text = p.fps > 0
            ? String(format: "%.0f fps · %.1f ms · max %.0f", p.fps, p.mean_ms, p.max_ms)
            : "starting"
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        stats.frame = CGRect(x: bounds.width - 148, y: bounds.height - 22, width: 142, height: 16)
        guard bounds.width > 0, bounds.height > 0 else { return }
        let w = Int((bounds.width * contentScaleFactor).rounded())
        let h = Int((bounds.height * contentScaleFactor).rounded())
        (layer as? CAMetalLayer)?.drawableSize = CGSize(width: w, height: h)
        if !launched, window != nil { launch(width: w, height: h) }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        statsTimer?.invalidate()
        statsTimer = nil
        if window != nil {
            husk_unity_set_paused(false)
            statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.updateStats() }
        } else {
            husk_unity_set_paused(true)
        }
        setNeedsLayout()
    }

    private func launch(width: Int, height: Int) {
        launched = true
        let angle = (Bundle.main.privateFrameworksPath ?? "") + "/libANGLE-shared.dylib"
        let ca = Bundle.main.path(forResource: "cacert", ofType: "pem") ?? ""
        try? FileManager.default.createDirectory(atPath: dataDir, withIntermediateDirectories: true)
        let layerPtr = Unmanaged.passUnretained(layer).toOpaque()
        if husk_unity_state() != Int32(HUSK_UNITY_IDLE) {
            // Already started this run: the engine cannot be loaded twice, so just show it again.
            HuskLog.log("tl", "unity: already started; resuming")
            return
        }
        HuskLog.log("tl", "unity: launching \(apk) at \(width)x\(height)")
        if !husk_unity_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca) {
            HuskLog.log("tl", "unity: launch refused")
        }
    }

    // MARK: touch

    private func id(for touch: UITouch) -> Int32 {
        let key = ObjectIdentifier(touch)
        if let existing = pointers[key] { return existing }
        var next: Int32 = 0
        while pointers.values.contains(next) { next += 1 }
        pointers[key] = next
        return next
    }

    private func send(_ touches: Set<UITouch>, phase: Int32) {
        for t in touches {
            let p = t.location(in: self)
            let pid = id(for: t)
            husk_unity_touch(phase, pid, Float(p.x * contentScaleFactor), Float(p.y * contentScaleFactor))
            if phase == 2 || phase == 3 { pointers[ObjectIdentifier(t)] = nil }
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 0) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 1) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 2) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        send(touches, phase: 2)
        pointers.removeAll()
    }
}

struct TLUnityScreen: UIViewRepresentable {
    let apk: String
    let dataDir: String
    /// One view for the life of the process. The engine's GPU surface belongs to this view's layer and an engine
    /// cannot be started twice, so coming back to the game must show the same layer, not a new one.
    private static var shared: TLUnityUIView?

    func makeUIView(context: Context) -> TLUnityUIView {
        if let view = Self.shared { return view }
        let view = TLUnityUIView(apk: apk, dataDir: dataDir)
        Self.shared = view
        return view
    }
    func updateUIView(_ view: TLUnityUIView, context: Context) {}
}

/// Polls the runtime for the status line and its log, ten times a second at most.
@MainActor
final class TLUnityModel: ObservableObject {
    @Published var state: Int32 = 0
    @Published var frames: UInt = 0
    @Published var logText = ""
    private var timer: Timer?
    private var ticks = 0

    func start() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    func stop() { timer?.invalidate(); timer = nil }

    private func poll() {
        ticks += 1
        state = husk_unity_state()
        frames = husk_unity_frames()
        if ticks % 4 == 0, let c = husk_tl_attempt_log() {
            let text = String(cString: c)
            free(c)
            if text != logText { logText = text }
        }
    }

    var statusText: String {
        switch state {
        case Int32(HUSK_UNITY_STARTING): return "Loading the engine…"
        case Int32(HUSK_UNITY_RUNNING):  return "Running"
        case Int32(HUSK_UNITY_FAILED):   return "Could not start — see the log"
        case Int32(HUSK_UNITY_ENDED):    return "The game exited"
        default:                         return "Starting"
        }
    }

    var subStatusText: String {
        switch state {
        case Int32(HUSK_UNITY_RUNNING): return "\(frames) frame(s) drawn · native runtime"
        case Int32(HUSK_UNITY_STARTING): return "Loading libraries and starting Unity"
        default: return "Native runtime"
        }
    }

    var statusColor: Color {
        switch state {
        case Int32(HUSK_UNITY_RUNNING):  return Theme.good
        case Int32(HUSK_UNITY_FAILED):   return .red
        case Int32(HUSK_UNITY_ENDED):    return .orange
        default:                         return Theme.accent
        }
    }
}

/// The Unity game the way the other runner shows a game: a status header, the screen taking whatever the log
/// leaves, and a log underneath that opens and closes. Presented full screen, not as a sheet, so a swipe in the
/// game is the game's.
struct TLUnityAttemptView: View {
    let app: TLApp
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model = TLUnityModel()
    @AppStorage("husk.tl.unity.showLog") private var showLog = false

    private var dataDir: String {
        TranslationLayer.root.appendingPathComponent(app.id, isDirectory: true)
            .appendingPathComponent("unity-data", isDirectory: true).path
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                HStack {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(model.statusText)
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(model.statusColor)
                        Text(model.subStatusText)
                            .font(.system(size: 12))
                            .foregroundStyle(Theme.textDim)
                    }
                    Spacer()
                }
                .padding()
                .background(Theme.surface)

                Divider()

                if let apk = app.apks.first {
                    TLUnityScreen(apk: apk, dataDir: dataDir)
                        .frame(maxWidth: .infinity, maxHeight: showLog ? 380 : .infinity)
                        .background(Color.black)
                    Divider()
                }

                HStack(spacing: 10) {
                    Button {
                        withAnimation(.snappy(duration: 0.25)) { showLog.toggle() }
                    } label: {
                        HStack(spacing: 6) {
                            Image(systemName: showLog ? "chevron.down" : "chevron.right")
                                .font(.system(size: 11, weight: .bold))
                                .frame(width: 12)
                            Text("ATTEMPT LOG")
                                .font(.technical(11, weight: .bold))
                        }
                        .foregroundStyle(Theme.textDim)
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    if showLog {
                        Button { UIPasteboard.general.string = model.logText } label: {
                            Label("Copy", systemImage: "doc.on.doc").font(.system(size: 12))
                        }
                    } else {
                        Text("tap to show")
                            .font(.system(size: 11))
                            .foregroundStyle(Theme.textDim.opacity(0.7))
                    }
                }
                .padding(.horizontal)
                .padding(.vertical, 8)
                .contentShape(Rectangle())
                .onTapGesture {
                    if !showLog { withAnimation(.snappy(duration: 0.25)) { showLog = true } }
                }

                if showLog {
                    ScrollViewReader { proxy in
                        ScrollView {
                            Text(model.logText.isEmpty ? "Starting…" : model.logText)
                                .font(.technical(11))
                                .foregroundStyle(Theme.text)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(12)
                                .textSelection(.enabled)
                                .id("bottom")
                        }
                        .background(Theme.bg)
                        .onChange(of: model.logText) { _ in proxy.scrollTo("bottom", anchor: .bottom) }
                    }
                    .transition(.opacity)
                }
            }
            .background(Theme.bg.ignoresSafeArea())
            .navigationTitle(app.label)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            }
        }
        // Swipes near the edges are the game's: keep the system from taking them for itself.
        .defersSystemGestures(on: .all)
        .onAppear { model.start() }
        .onDisappear { model.stop() }
    }
}
