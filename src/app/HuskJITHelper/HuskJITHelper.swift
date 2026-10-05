// SPDX-License-Identifier: GPL-2.0-or-later
import ExtensionFoundation
import Foundation
import StikJIT
import XPC

/// The debugger half of Built-in StikJIT (docs/06-built-in-jit.md).
///
/// A process cannot synchronously debug itself, so Husk starts this extension,
/// sends it its PID, the pairing file and husk-jit.js, and StikJIT attaches over
/// LocalDevVPN and services the script's trap requests until Husk detaches.
private struct HuskJITMessageHandler: XPCPeerHandler {
    private static let queue = DispatchQueue(label: "com.husk.app.jit-helper")

    func handleIncomingRequest(_ message: XPCReceivedMessage) -> (any Encodable)? {
        guard let request = try? message.decode(as: HuskJITRequest.self) else {
            return HuskJITRequest.Response(success: false,
                                           message: "The helper could not read Husk's request.",
                                           txmPresent: nil)
        }
        // Enabling JIT holds this queue for as long as the debugger is attached.
        return message.handoffReply(to: Self.queue) { message.reply(handle(request)) }
    }

    private func handle(_ request: HuskJITRequest) -> HuskJITRequest.Response {
        let manager = FileManager.default
        let root = manager.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
        let paths = DDIPaths.default(in: root)

        do {
            try manager.createDirectory(at: root, withIntermediateDirectories: true)
            if request.operation == .resetDDI {
                try StikJIT.resetCachedDDI(at: paths)
                return .init(success: true, message: "Developer Disk Image cache reset.",
                             txmPresent: StikJIT.isTXMPresent)
            }

            guard let pairingData = request.pairingData, !pairingData.isEmpty else {
                throw NSError(domain: "HuskJITHelper", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "The pairing file was not provided."])
            }
            let pairingURL = root.appendingPathComponent("pairingFile-\(UUID().uuidString).plist")
            try pairingData.write(to: pairingURL, options: .atomic)
            defer { try? manager.removeItem(at: pairingURL) }

            switch request.operation {
            case .prepare:
                let readiness = StikJIT.prepareDevice(
                    pairingFile: pairingURL, paths: paths,
                    progress: { NSLog("[HuskJIT] prepare: %@", String(describing: $0)) })
                switch readiness {
                case .ready(let security):
                    return .init(success: true,
                                 message: "LocalDevVPN is reachable and the Developer Disk Image is ready.",
                                 txmPresent: security.isTXMPresent)
                case .unreachable(let reason), .preparationFailed(let reason):
                    return .init(success: false, message: reason, txmPresent: StikJIT.isTXMPresent)
                @unknown default:
                    return .init(success: false, message: "StikJIT returned an unknown preparation state.",
                                 txmPresent: StikJIT.isTXMPresent)
                }
            case .enable:
                guard let targetPID = request.targetPID,
                      let scriptBase64 = request.scriptBase64, !scriptBase64.isEmpty else {
                    throw NSError(domain: "HuskJITHelper", code: 2,
                                  userInfo: [NSLocalizedDescriptionKey:
                                    "The target process or Husk's JIT script was not provided."])
                }
                try StikJIT.enableJIT(
                    targetPID: targetPID,
                    pairingFile: pairingURL,
                    ddiPaths: paths,
                    script: .customBase64(scriptBase64),
                    forceScript: true,
                    preparationProgress: { NSLog("[HuskJIT] prepare: %@", String(describing: $0)) },
                    progress: { NSLog("[HuskJIT] %@", $0) })
                return .init(success: true, message: "Husk detached cleanly from its built-in JIT helper.",
                             txmPresent: StikJIT.isTXMPresent)
            case .resetDDI:
                preconditionFailure("Handled above")
            }
        } catch {
            return .init(success: false, message: error.localizedDescription,
                         txmPresent: StikJIT.isTXMPresent)
        }
    }
}

@main
struct HuskJITHelperExtension: AppExtension {
    var configuration: some AppExtensionConfiguration {
        ConnectionHandler(onSessionRequest: { request in
            request.accept { _ in HuskJITMessageHandler() }
        })
    }
}
