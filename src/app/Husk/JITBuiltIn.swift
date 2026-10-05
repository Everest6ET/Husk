// SPDX-License-Identifier: GPL-2.0-or-later
import ExtensionFoundation
import Foundation
import XPC

@available(iOS 26.0, *)
extension AppExtensionPoint {
    @Definition
    static var huskJITHelper: AppExtensionPoint {
        Name("HuskJITHelper")
    }
}

@available(iOS 26.0, *)
private final class HuskJITExtensionSession {
    let process: AppExtensionProcess
    let session: XPCSession

    init(process: AppExtensionProcess, session: XPCSession) {
        self.process = process
        self.session = session
    }

    deinit {
        session.cancel(reason: "Husk JIT request finished")
        process.invalidate()
    }
}

/// Starts the separate helper process (HuskJITHelper) that debugs Husk. The
/// session is held until the helper replies, which for `enable` is when Husk
/// detaches -- dropping it earlier would kill the debugger mid-attach.
@MainActor
enum HuskBuiltInJIT {
    @available(iOS 26.0, *)
    private static var activeSession: HuskJITExtensionSession?

    static var unavailableReason: String? {
#if targetEnvironment(simulator)
        return "Built-in JIT is available only on a physical device."
#else
        guard #available(iOS 26.0, *) else {
            return "Built-in JIT needs iOS 26 or later. Use StikDebug instead."
        }
        if getenv("LC_HOME_PATH") != nil {
            return "Built-in JIT is unavailable inside LiveContainer. Use StikDebug instead."
        }
        return nil
#endif
    }

    static var isAvailable: Bool { unavailableReason == nil }

    static func send(_ request: HuskJITRequest,
                     started: @escaping () -> Void = {},
                     completion: @escaping (Result<HuskJITRequest.Response, Error>) -> Void) {
        guard #available(iOS 26.0, *), unavailableReason == nil else {
            completion(.failure(NSError(
                domain: "HuskBuiltInJIT", code: 1,
                userInfo: [NSLocalizedDescriptionKey: unavailableReason ?? "Built-in JIT is unavailable."])))
            return
        }

        Task { @MainActor in
            do {
                let monitor = try await AppExtensionPoint.Monitor(appExtensionPoint: .huskJITHelper)
                guard let identity = monitor.identities.first else {
                    throw NSError(
                        domain: "HuskBuiltInJIT", code: 2,
                        userInfo: [NSLocalizedDescriptionKey:
                            "Husk's JIT helper was not found in this installation. Reinstall Husk with a "
                          + "sideloader that keeps app extensions, or use StikDebug."])
                }
                let process = try await AppExtensionProcess(configuration: .init(
                    appExtensionIdentity: identity,
                    onInterruption: { Task { @MainActor in activeSession = nil } }))
                let session = try process.makeXPCSession()
                try session.activate()
                activeSession = HuskJITExtensionSession(process: process, session: session)
                started()
                try session.send(request) { (result: Result<HuskJITRequest.Response, any Error>) in
                    Task { @MainActor in
                        activeSession = nil
                        completion(result.mapError { $0 as Error })
                    }
                }
            } catch {
                activeSession = nil
                completion(.failure(error))
            }
        }
    }
}
