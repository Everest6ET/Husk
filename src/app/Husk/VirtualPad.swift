// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// A controller on the glass, for a game whose menus only answer one.
///
/// Minecraft Dungeons' menus are navigated with a gamepad (its touch controls exist only in a mission), so without a
/// controller paired there is no getting past the language screen. This is the minimum that gets you through: a D-pad,
/// A/B/X/Y and Start, reported to the runtime like any other controller. It takes the last slot so a real controller,
/// which GameController hands the first free one, is never displaced by it.
@MainActor
final class VirtualPad: ObservableObject {
    static let slot: Int32 = 3

    /// The bits of the button mask (husk-tl-gamepad.h, TL_PAD_*).
    enum Button: UInt32 {
        case a = 0, b, x, y, start = 8, up = 11, down, left, right
        var mask: UInt32 { 1 << rawValue }
    }

    @Published private(set) var held: UInt32 = 0
    private var connected = false

    func connect() {
        guard !connected else { return }
        connected = true
        husk_gamepad_connect(Self.slot, "Xbox Wireless Controller")
    }

    func disconnect() {
        guard connected else { return }
        connected = false
        held = 0
        husk_gamepad_disconnect(Self.slot)
    }

    func set(_ button: Button, down: Bool) {
        guard connected else { return }
        let was = held
        if down { held |= button.mask } else { held &= ~button.mask }
        if held != was { husk_gamepad_update(Self.slot, held, 0, 0, 0, 0, 0, 0) }
    }
}

struct VirtualPadView: View {
    @ObservedObject var pad: VirtualPad
    private let size: CGFloat = 46

    var body: some View {
        GeometryReader { geo in
            ZStack {
                // D-pad, bottom left.
                cluster(origin: CGPoint(x: 28 + size * 1.5, y: geo.size.height - 28 - size * 1.5), spread: size * 1.05, items: [
                    (.up, "chevron.up", 0, -1), (.down, "chevron.down", 0, 1),
                    (.left, "chevron.left", -1, 0), (.right, "chevron.right", 1, 0)
                ])
                // Face buttons, bottom right, in the Xbox arrangement.
                cluster(origin: CGPoint(x: geo.size.width - 28 - size * 1.5, y: geo.size.height - 28 - size * 1.5), spread: size * 1.05, items: [
                    (.y, "Y", 0, -1), (.a, "A", 0, 1), (.x, "X", -1, 0), (.b, "B", 1, 0)
                ])
                // Start, low in the middle.
                key(.start, label: "Start", width: 64, height: 30)
                    .position(x: geo.size.width / 2, y: geo.size.height - 28)
            }
        }
        .onAppear { pad.connect() }
        .onDisappear { pad.disconnect() }
    }

    private func cluster(origin: CGPoint, spread: CGFloat, items: [(VirtualPad.Button, String, CGFloat, CGFloat)]) -> some View {
        ZStack {
            ForEach(items.indices, id: \.self) { i in
                let item = items[i]
                key(item.0, label: item.1, width: size, height: size, circle: true)
                    .position(x: origin.x + item.2 * spread, y: origin.y + item.3 * spread)
            }
        }
    }

    /// One flat key: lit while a finger is on it, and it lets go when the finger leaves, as a controller button does.
    private func key(_ button: VirtualPad.Button, label: String, width: CGFloat, height: CGFloat, circle: Bool = false) -> some View {
        let down = pad.held & button.mask != 0
        return Group {
            if label.hasPrefix("chevron") {
                Image(systemName: label).font(.system(size: 17, weight: .bold))
            } else {
                Text(label).font(.system(size: label.count > 1 ? 12 : 18, weight: .bold, design: .rounded))
            }
        }
        .foregroundStyle(.white.opacity(down ? 1 : 0.85))
        .frame(width: width, height: height)
        .background((circle ? AnyShape(Circle()) : AnyShape(Capsule())).fill(Color.white.opacity(down ? 0.42 : 0.2)))
        .overlay((circle ? AnyShape(Circle()) : AnyShape(Capsule())).stroke(Color.white.opacity(0.35), lineWidth: 1))
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { _ in pad.set(button, down: true) }
            .onEnded { _ in pad.set(button, down: false) })
    }
}
