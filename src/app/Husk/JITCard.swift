// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The way to turn on JIT, said plainly: StikJIT is built into Husk, and it is the recommended way.
///
/// Android and the Translation Layer's games both need memory they can write and then execute, which on iOS takes an attached
/// debugger. Husk can be that debugger itself -- StikJIT, in the app, with no computer and no other app -- so this is the first
/// thing shown wherever JIT is missing, with the other methods one tap further. Once JIT is on it shrinks to a line saying so.
struct JITCard: View {
    @ObservedObject private var jit = JITCoordinator.shared

    private var isOn: Bool { JITBootstrap.isDebuggerAttached || JITBootstrap.debuggedFlag }

    var body: some View {
        Group {
            if isOn { on } else { off }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 4)
    }

    private var on: some View {
        HStack(spacing: 12) {
            Image(systemName: "checkmark.circle.fill")
                .font(.title2)
                .foregroundStyle(.green)
            VStack(alignment: .leading, spacing: 2) {
                Text("JIT is on").font(.headline)
                Text("Games and Android can run.").font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .padding(14)
        .huskCard()
    }

    private var off: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                ZStack {
                    Circle().fill(Color.accentColor.opacity(0.16)).frame(width: 44, height: 44)
                    Image(systemName: "bolt.fill").font(.title3).foregroundStyle(Color.accentColor)
                }
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 8) {
                        Text("StikJIT").font(.title3.weight(.semibold))
                        Text("RECOMMENDED")
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 7).padding(.vertical, 3)
                            .background(Color.accentColor, in: Capsule())
                    }
                    Text("Turn on JIT").font(.subheadline).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }

            Text("StikJIT is built into Husk. You can turn JIT on right here — no computer and no other app. "
               + "Games and Android both need it.")
                .font(.subheadline)
                .fixedSize(horizontal: false, vertical: true)

            if let why = HuskBuiltInJIT.unavailableReason {
                Text(why).font(.footnote).foregroundStyle(.orange)
            }

            if jit.busy {
                HStack(spacing: 10) {
                    ProgressView()
                    Text(jit.status ?? "Turning on JIT…").font(.subheadline).foregroundStyle(.secondary)
                }
            } else {
                Button {
                    // The built-in one, whatever "Automatic" would have picked: it is the one this card recommends.
                    if HuskBuiltInJIT.isAvailable { jit.method = .builtIn }
                    jit.enable()
                } label: {
                    HStack {
                        Spacer()
                        Label(HuskBuiltInJIT.isAvailable ? "Turn On JIT with StikJIT" : "Turn On JIT", systemImage: "bolt.fill")
                            .font(.headline)
                        Spacer()
                    }
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
            }

            if let error = jit.error {
                Text(error).font(.footnote).foregroundStyle(.orange)
            }

            Button { jit.showSetup = true } label: {
                Text("First time? See the setup, or use StikDebug or TrollStore instead")
                    .font(.footnote)
            }
        }
        .padding(16)
        .huskCard()
    }
}
