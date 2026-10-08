// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Geode, the mod loader for Geometry Dash, for the translation layer to load into the game (husk-tl-geode.c).
///
/// Two files make it work, both downloaded when Geode is turned on in the game's settings:
///   - the Geode release for this version of the game: Geode's own index names it, from the game's versionCode (a release
///     only supports the game versions it was built for -- Geode 4.10.2 is the last for 2.2074, for example);
///   - Geode's Android launcher, for the C++ runtime (libc++_shared.so) that Geode and every mod are built against.
/// Mods themselves are Geode's business: it finds, downloads and loads them from its own button on the game's main menu.
@MainActor
final class GeodeSupport: ObservableObject {
    static let shared = GeodeSupport()
    static let gamePackage = "com.robtopx.geometryjump"

    enum Status: Equatable {
        case idle
        case working(String)
        case ready(String)           // the Geode version
        case failed(String)
    }

    @Published private(set) var status: [String: Status] = [:]

    nonisolated static func folder(_ appID: String) -> URL {
        TranslationLayer.root.appendingPathComponent(appID, isDirectory: true).appendingPathComponent("geode", isDirectory: true)
    }
    nonisolated private static func releaseZip(_ appID: String) -> URL { folder(appID).appendingPathComponent("geode-android64.zip") }
    nonisolated private static func launcherAPK(_ appID: String) -> URL { folder(appID).appendingPathComponent("geode-launcher.apk") }
    nonisolated private static func versionFile(_ appID: String) -> URL { folder(appID).appendingPathComponent("version.txt") }

    /// What a launch hands the game: both files, when Geode is on for this game and has been downloaded. `appDir` is the
    /// game's folder (Documents/TranslationLayer/<id>).
    nonisolated static func files(appDir: String) -> (zip: String, launcher: String)? {
        let id = (appDir as NSString).lastPathComponent
        guard TLAppSettings.load(id).geode else { return nil }
        let zip = releaseZip(id).path, apk = launcherAPK(id).path
        guard FileManager.default.fileExists(atPath: zip), FileManager.default.fileExists(atPath: apk) else { return nil }
        return (zip, apk)
    }

    func current(_ app: TLApp) -> Status {
        if let s = status[app.id] { return s }
        if let v = try? String(contentsOf: Self.versionFile(app.id), encoding: .utf8),
           FileManager.default.fileExists(atPath: Self.releaseZip(app.id).path),
           FileManager.default.fileExists(atPath: Self.launcherAPK(app.id).path) {
            return .ready(v.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        return .idle
    }

    /// Download what is missing or out of date.
    func prepare(_ app: TLApp) async {
        if case .working = status[app.id] { return }
        status[app.id] = .working("Finding the Geode release for this game…")
        do {
            let folder = Self.folder(app.id)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            guard let apk = app.apks.first,
                  let manifest = TranslationLayerStore.entry(apk, "AndroidManifest.xml", limit: 8 << 20),
                  let versionCode = ApkMetadata.versionCode(manifest) else {
                throw GeodeError("Could not read this game's version.")
            }

            // Geode's index: the newest Geode release for this game version.
            let (release, zipURL) = try await Self.latestRelease(versionCode: versionCode)
            let have = (try? String(contentsOf: Self.versionFile(app.id), encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
            if have != release || !FileManager.default.fileExists(atPath: Self.releaseZip(app.id).path) {
                status[app.id] = .working("Downloading Geode \(release)…")
                try await Self.download(zipURL, to: Self.releaseZip(app.id))
                try release.write(to: Self.versionFile(app.id), atomically: true, encoding: .utf8)
            }

            if !FileManager.default.fileExists(atPath: Self.launcherAPK(app.id).path) {
                status[app.id] = .working("Downloading Geode's launcher…")
                try await Self.download(try await Self.launcherURL(), to: Self.launcherAPK(app.id))
            }
            status[app.id] = .ready(release)
            HuskLog.log("geode", "Geode \(release) ready for \(app.label) (versionCode \(versionCode))")
        } catch {
            status[app.id] = .failed(error.localizedDescription)
            HuskLog.log("geode", "could not get Geode: \(error.localizedDescription)")
        }
    }

    /// Remove the downloaded files (Geode turned off). Mods and their settings stay, in the game's own data.
    func remove(_ app: TLApp) {
        try? FileManager.default.removeItem(at: Self.folder(app.id))
        status[app.id] = .idle
    }

    // MARK: network

    struct GeodeError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    private static func json(_ url: URL) async throws -> [String: Any] {
        var req = URLRequest(url: url)
        req.timeoutInterval = 30
        req.setValue("Husk", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await URLSession.shared.data(for: req)
        guard (response as? HTTPURLResponse)?.statusCode == 200,
              let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw GeodeError("\(url.host ?? "The server") did not answer (HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0)).")
        }
        return obj
    }

    private static func latestRelease(versionCode: Int) async throws -> (String, URL) {
        let url = URL(string: "https://api.geode-sdk.org/v1/loader/versions/latest?gd=\(versionCode)&platform=android&prerelease=false")!
        let obj = try await json(url)
        let payload = obj["payload"] as? [String: Any] ?? obj
        guard let version = payload["version"] as? String,
              let downloads = payload["downloads"] as? [String: Any] else {
            throw GeodeError("Geode has no release for this version of Geometry Dash.")
        }
        let entry = downloads["android64"]
        let link = (entry as? String) ?? ((entry as? [String: Any])?["url"] as? String)
        guard let link, let zip = URL(string: link) else { throw GeodeError("Geode's release has no Android build.") }
        return (version, zip)
    }

    private static func launcherURL() async throws -> URL {
        let obj = try await json(URL(string: "https://api.github.com/repos/geode-sdk/android-launcher/releases/latest")!)
        let assets = obj["assets"] as? [[String: Any]] ?? []
        guard let link = assets.compactMap({ $0["browser_download_url"] as? String }).first(where: { $0.hasSuffix(".apk") }),
              let url = URL(string: link) else { throw GeodeError("Geode's launcher release has no APK.") }
        return url
    }

    private static func download(_ url: URL, to dest: URL) async throws {
        var req = URLRequest(url: url)
        req.timeoutInterval = 120
        req.setValue("Husk", forHTTPHeaderField: "User-Agent")
        let (tmp, response) = try await URLSession.shared.download(for: req)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw GeodeError("Download failed (HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0)).")
        }
        try? FileManager.default.removeItem(at: dest)
        try FileManager.default.moveItem(at: tmp, to: dest)
    }
}
