// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Talking to Google Play, as the Play Store app on a phone does: sign-in tokens from android.clients.google.com/auth, device
/// registration through /checkin and /fdfe/uploadDeviceConfig, then the store itself under /fdfe -- details, search, the free
/// "purchase" that every download starts with, and delivery, which answers with the files' addresses.
///
/// Written for Husk from how those requests look on the wire; it shares no code with any other client.
struct PlayError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

/// Everything a signed-in session needs, kept between launches.
struct PlaySession: Codable, Equatable {
    var email: String
    /// The long-lived account token (from signing in once). Never shown, kept in the Keychain.
    var aasToken: String
    /// This device's id with Google (hex), from checkin.
    var gsfID: String = ""
    var consistencyToken: String = ""
    var deviceConfigToken: String = ""
    /// The Play Store's own short-lived token, made from the account token.
    var bearer: String = ""
}

enum PlayAPI {
    static let authURL = URL(string: "https://android.clients.google.com/auth")!
    static let checkinURL = URL(string: "https://android.clients.google.com/checkin")!
    static let fdfe = "https://android.clients.google.com/fdfe/"
    /// The signature of the Google app the requests speak for (Play Services' certificate digest).
    static let callerSig = "38918a453d07199354f8b19af05ec6562ced5788"

    private static let urlSession: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 30
        c.httpCookieStorage = nil
        c.urlCache = nil
        return URLSession(configuration: c)
    }()

    // MARK: sign-in tokens

    /// The `key=value` lines /auth answers with.
    private static func parseKeyValues(_ data: Data) -> [String: String] {
        var out: [String: String] = [:]
        for line in String(decoding: data, as: UTF8.self).split(separator: "\n") {
            guard let eq = line.firstIndex(of: "=") else { continue }
            out[String(line[..<eq])] = String(line[line.index(after: eq)...])
        }
        return out
    }

    private static func form(_ params: [(String, String)]) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return params.map { "\($0.0)=\($0.1.addingPercentEncoding(withAllowedCharacters: allowed) ?? $0.1)" }.joined(separator: "&")
    }

    /// The web sign-in leaves an `oauth_token` cookie; this trades it for the account token that lasts.
    static func accountToken(email: String, oauthToken: String) async throws -> String {
        let params: [(String, String)] = [
            ("lang", PlayDevice.locale.replacingOccurrences(of: "_", with: "-")),
            ("google_play_services_version", "\(PlayDevice.playServicesVersion)"),
            ("sdk_version", "\(PlayDevice.sdk)"),
            ("device_country", PlayDevice.country),
            ("Email", email),
            ("service", "ac2dm"),
            ("get_accountid", "1"),
            ("ACCESS_TOKEN", "1"),
            ("callerPkg", "com.google.android.gms"),
            ("add_account", "1"),
            ("Token", oauthToken),
            ("callerSig", callerSig),
        ]
        var req = URLRequest(url: authURL)
        req.httpMethod = "POST"
        req.setValue("com.google.android.gms", forHTTPHeaderField: "app")
        req.setValue(PlayDevice.authUserAgent, forHTTPHeaderField: "User-Agent")
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.httpBody = Data(form(params).utf8)
        let (data, response) = try await urlSession.data(for: req)
        let kv = parseKeyValues(data)
        guard (response as? HTTPURLResponse)?.statusCode == 200, let token = kv["Token"], !token.isEmpty else {
            throw PlayError("Google did not accept the sign-in (\(kv["Error"] ?? "HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0)")).")
        }
        return token
    }

    /// The Play Store's own token, made from the account token for this device.
    static func playToken(_ s: PlaySession) async throws -> String {
        let params: [(String, String)] = [
            ("androidId", s.gsfID),
            ("sdk_version", "\(PlayDevice.sdk)"),
            ("Email", s.email),
            ("google_play_services_version", "\(PlayDevice.playServicesVersion)"),
            ("device_country", PlayDevice.country),
            ("lang", PlayDevice.locale.replacingOccurrences(of: "_", with: "-")),
            ("callerSig", callerSig),
            ("app", "com.android.vending"),
            ("client_sig", callerSig),
            ("callerPkg", "com.google.android.gms"),
            ("Token", s.aasToken),
            ("oauth2_foreground", "1"),
            ("token_request_options", "CAA4AVAB"),
            ("check_email", "1"),
            ("system_partition", "1"),
            ("service", "oauth2:https://www.googleapis.com/auth/googleplay"),
        ]
        var req = URLRequest(url: authURL)
        req.httpMethod = "POST"
        req.setValue("com.google.android.gms", forHTTPHeaderField: "app")
        req.setValue(PlayDevice.authUserAgent, forHTTPHeaderField: "User-Agent")
        req.setValue(s.gsfID, forHTTPHeaderField: "device")
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.httpBody = Data(form(params).utf8)
        let (data, response) = try await urlSession.data(for: req)
        let kv = parseKeyValues(data)
        guard (response as? HTTPURLResponse)?.statusCode == 200, let token = kv["Auth"], !token.isEmpty else {
            throw PlayError("Google Play would not open a session (\(kv["Error"] ?? "HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0)")).")
        }
        return token
    }

    // MARK: registering the device

    /// Checkin: Google gives this device an id (and a consistency token).
    static func checkin() async throws -> (gsfID: String, consistency: String) {
        var req = URLRequest(url: checkinURL)
        req.httpMethod = "POST"
        req.setValue("com.google.android.gms", forHTTPHeaderField: "app")
        req.setValue(PlayDevice.authUserAgent, forHTTPHeaderField: "User-Agent")
        req.setValue("application/x-protobuffer", forHTTPHeaderField: "Content-Type")
        req.httpBody = PlayDevice.checkinRequest()
        let (data, response) = try await urlSession.data(for: req)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw PlayError("Google did not register the device (HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0)).")
        }
        let reply = ProtoMessage(data)
        guard let id = reply.uint64(7), id != 0 else { throw PlayError("Google registered the device but gave it no id.") }
        return (String(id, radix: 16), reply.string(12) ?? "")
    }

    /// Tell Play what this device is, so it serves the right builds.
    static func uploadDeviceConfig(_ s: PlaySession) async throws -> String {
        var w = ProtoWriter()
        w.message(1, PlayDevice.configuration)
        let reply = try await fdfeCall("uploadDeviceConfig", session: s, body: w.data)
        return reply.message(28)?.string(1) ?? ""
    }

    // MARK: the store

    private static func headers(_ s: PlaySession) -> [String: String] {
        var h = [
            "Authorization": "Bearer \(s.bearer)",
            "User-Agent": PlayDevice.finskyUserAgent,
            "X-DFE-Device-Id": s.gsfID,
            "Accept-Language": PlayDevice.locale.replacingOccurrences(of: "_", with: "-"),
            "X-DFE-Client-Id": "am-android-google",
            "X-DFE-Network-Type": "4",
            "X-DFE-Content-Filters": "",
            "X-Limit-Ad-Tracking-Enabled": "false",
            "X-Ad-Id": "",
            "X-DFE-UserLanguages": PlayDevice.locale,
            "X-DFE-Request-Params": "timeoutMs=4000",
            "X-DFE-MCCMNC": PlayDevice.simOperator,
        ]
        if !s.consistencyToken.isEmpty { h["X-DFE-Device-Checkin-Consistency-Token"] = s.consistencyToken }
        if !s.deviceConfigToken.isEmpty { h["X-DFE-Device-Config-Token"] = s.deviceConfigToken }
        return h
    }

    /// One /fdfe call; the reply's payload (ResponseWrapper.payload, or the prefetched one when that is where the answer is).
    @discardableResult
    static func fdfeCall(_ path: String, session s: PlaySession, query: [(String, String)] = [], body: Data? = nil,
                         post: Bool = false) async throws -> ProtoMessage {
        var comps = URLComponents(string: fdfe + path)!
        if !query.isEmpty { comps.queryItems = (comps.queryItems ?? []) + query.map { URLQueryItem(name: $0.0, value: $0.1) } }
        var req = URLRequest(url: comps.url!)
        req.httpMethod = (body != nil || post) ? "POST" : "GET"
        for (k, v) in headers(s) { req.setValue(v, forHTTPHeaderField: k) }
        if let body {
            req.httpBody = body
            req.setValue("application/x-protobuf", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try await urlSession.data(for: req)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        let wrapper = ProtoMessage(data)
        if status == 401 { throw PlayError("Google Play's session has ended; signing in again.") }
        guard status == 200 else {
            let message = wrapper.message(5)?.string(1) ?? "HTTP \(status)"    // ServerCommands.displayErrorMessage
            throw PlayError("Google Play said no: \(message)")
        }
        if let payload = wrapper.message(1), !payload.fields.isEmpty { return payload }
        return wrapper.message(3)?.message(2)?.message(1) ?? ProtoMessage(Data())
    }

    // MARK: - Store APIs & Models

    public struct PlayApp: Identifiable, Hashable {
        public let id: String // packageName
        public let title: String
        public let developer: String
        public let summary: String
        public let iconURL: String?
        public let screenshotURLs: [String]
        public let versionCode: Int
        public let versionString: String
        public let sizeBytes: Int64
        public let downloads: String
        public let rating: Double
        public let formattedPrice: String
        public let isFree: Bool

        public static func from(doc: ProtoMessage) -> PlayApp? {
            guard let id = doc.string(1), !id.isEmpty else { return nil }
            // Ignore non-app document types if possible, or accept if docid looks like a package name
            let title = doc.string(5) ?? id
            let dev = doc.string(6) ?? ""
            let desc = doc.string(7) ?? ""

            // Images: field 10 (imageType 4 = icon, 1 or 2 = screenshot)
            var icon: String? = nil
            var screenshots: [String] = []
            for img in doc.messages(10) {
                let imgType = img.int(1) ?? 0
                if let url = img.string(5) {
                    if imgType == 4 && icon == nil {
                        icon = url
                    } else if imgType == 1 || imgType == 2 {
                        screenshots.append(url)
                    } else if icon == nil {
                        icon = url
                    }
                }
            }

            // Offer / Price: field 8
            var price = "Free"
            var isFree = true
            if let offer = doc.messages(8).first {
                if let p = offer.string(7), !p.isEmpty {
                    price = p
                }
                let mic = offer.int(1) ?? 0
                isFree = (mic == 0)
            }

            // App details: field 13 message 1
            let appDetails = doc.message(13)?.message(1)
            let vc = Int(appDetails?.int(3) ?? 0)
            let vs = appDetails?.string(4) ?? ""
            let size = appDetails?.int(9) ?? 0
            let downloads = appDetails?.string(13) ?? ""

            // Aggregate rating: field 14 message 1 (stars / 5.0)
            var ratingVal = 0.0
            if let agg = doc.message(14) {
                if let r = agg.string(1), let d = Double(r) {
                    ratingVal = d
                }
            }

            return PlayApp(
                id: id,
                title: title,
                developer: dev,
                summary: desc,
                iconURL: icon,
                screenshotURLs: screenshots,
                versionCode: vc,
                versionString: vs,
                sizeBytes: size,
                downloads: downloads,
                rating: ratingVal,
                formattedPrice: price,
                isFree: isFree
            )
        }

        public static func parseApps(from message: ProtoMessage) -> [PlayApp] {
            var apps: [PlayApp] = []
            var visited = Set<String>()

            func traverse(_ msg: ProtoMessage) {
                // If this is a DocV2 (has docid = 1 and details.appDetails = 13.1 or offer = 8)
                if let docid = msg.string(1), docid.contains("."), !visited.contains(docid) {
                    if let app = PlayApp.from(doc: msg) {
                        visited.insert(docid)
                        apps.append(app)
                    }
                }

                // Recurse into child messages across all fields
                for (_, vals) in msg.fields {
                    for val in vals {
                        if case .bytes(let d) = val {
                            let child = ProtoMessage(d)
                            if !child.fields.isEmpty {
                                traverse(child)
                            }
                        }
                    }
                }
            }

            traverse(message)
            return apps
        }
    }

    /// Search Google Play
    static func search(query: String, session: PlaySession) async throws -> [PlayApp] {
        let q = [
            ("c", "3"), // category 3 = apps
            ("q", query)
        ]
        let payload = try await fdfeCall("search", session: session, query: q)
        return PlayApp.parseApps(from: payload)
    }

    /// Browse top / featured apps
    static func browse(session: PlaySession) async throws -> [PlayApp] {
        // Browse top chart / home
        let q = [
            ("c", "3")
        ]
        let payload = try await fdfeCall("browse", session: session, query: q)
        var apps = PlayApp.parseApps(from: payload)
        if apps.isEmpty {
            // Fallback to a standard list query if browse payload structure differs
            let listPayload = try await fdfeCall("list", session: session, query: [("c", "3"), ("cat", "GAME")])
            apps = PlayApp.parseApps(from: listPayload)
        }
        return apps
    }

    /// Get full details for a single app
    static func details(packageName: String, session: PlaySession) async throws -> PlayApp? {
        let q = [
            ("doc", packageName)
        ]
        let payload = try await fdfeCall("details", session: session, query: q)
        // Payload -> detailsResponse (field 2) -> docV2 (field 4)
        if let doc = payload.message(2)?.message(4) {
            return PlayApp.from(doc: doc)
        }
        let apps = PlayApp.parseApps(from: payload)
        return apps.first(where: { $0.id == packageName }) ?? apps.first
    }

    public struct DeliveryData {
        public struct SplitFile {
            public let name: String
            public let size: Int64
            public let downloadUrl: String
        }
        public let downloadUrl: String?
        public let size: Int64
        public let splits: [SplitFile]
        public let downloadAuthCookie: (name: String, value: String)?
    }

    /// "Purchase" (license) a free app so Google Play allows delivery.
    static func purchase(packageName: String, versionCode: Int, session: PlaySession) async throws {
        let q = [
            ("doc", packageName),
            ("ot", "1"),
            ("vc", "\(versionCode)")
        ]
        // POST to fdfe/purchase
        _ = try await fdfeCall("purchase", session: session, query: q, post: true)
    }

    /// Request delivery URL(s) for an app
    static func delivery(packageName: String, versionCode: Int, session: PlaySession) async throws -> DeliveryData {
        let q = [
            ("doc", packageName),
            ("ot", "1"),
            ("vc", "\(versionCode)")
        ]
        let payload = try await fdfeCall("delivery", session: session, query: q)

        // Delivery response: field 21 message 2 (AndroidAppDeliveryData)
        guard let deliveryDataMsg = payload.message(21)?.message(2) else {
            throw PlayError("Google Play did not return download information.")
        }

        let downloadUrl = deliveryDataMsg.string(3)
        let downloadSize = deliveryDataMsg.int(1) ?? 0

        // Splits: field 15
        var splits: [DeliveryData.SplitFile] = []
        for splitMsg in deliveryDataMsg.messages(15) {
            let sName = splitMsg.string(1) ?? "split"
            let sSize = splitMsg.int(2) ?? 0
            if let sUrl = splitMsg.string(5) {
                splits.append(DeliveryData.SplitFile(name: sName, size: sSize, downloadUrl: sUrl))
            }
        }

        // Cookie: field 5
        var cookie: (name: String, value: String)? = nil
        if let cookieMsg = deliveryDataMsg.messages(5).first,
           let cName = cookieMsg.string(1),
           let cVal = cookieMsg.string(2) {
            cookie = (cName, cVal)
        }

        return DeliveryData(
            downloadUrl: downloadUrl,
            size: downloadSize,
            splits: splits,
            downloadAuthCookie: cookie
        )
    }

    /// Downloads the APK (and splits if present) into a temporary directory and returns the list of file URLs.
    static func downloadApp(
        packageName: String,
        versionCode: Int,
        session: PlaySession,
        progress: (@MainActor (Double) -> Void)? = nil
    ) async throws -> [URL] {
        // First try delivery; if not purchased, purchase first and retry delivery
        var deliveryData: DeliveryData
        do {
            deliveryData = try await delivery(packageName: packageName, versionCode: versionCode, session: session)
        } catch {
            // Attempt purchase
            try await purchase(packageName: packageName, versionCode: versionCode, session: session)
            deliveryData = try await delivery(packageName: packageName, versionCode: versionCode, session: session)
        }

        var downloadsToMake: [(name: String, url: String, size: Int64)] = []
        if let base = deliveryData.downloadUrl {
            downloadsToMake.append(("\(packageName).apk", base, deliveryData.size))
        }
        for split in deliveryData.splits {
            downloadsToMake.append(("\(packageName).\(split.name).apk", split.downloadUrl, split.size))
        }

        guard !downloadsToMake.isEmpty else {
            throw PlayError("No download URLs available for \(packageName).")
        }

        let totalExpectedBytes = downloadsToMake.reduce(Int64(0)) { $0 + max(0, $1.size) }
        let tempDir = FileManager.default.temporaryDirectory.appendingPathComponent("PlayDownload_\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)

        var downloadedFiles: [URL] = []
        var totalBytesWritten: Int64 = 0

        for item in downloadsToMake {
            guard let dlURL = URL(string: item.url) else { continue }
            var req = URLRequest(url: dlURL)
            req.setValue(PlayDevice.finskyUserAgent, forHTTPHeaderField: "User-Agent")
            if let c = deliveryData.downloadAuthCookie {
                req.setValue("\(c.name)=\(c.value)", forHTTPHeaderField: "Cookie")
            }

            let fileDest = tempDir.appendingPathComponent(item.name)
            let (tempLocalURL, resp) = try await URLSession.shared.download(for: req)
            guard (resp as? HTTPURLResponse)?.statusCode == 200 else {
                throw PlayError("Failed to download \(item.name) (HTTP \((resp as? HTTPURLResponse)?.statusCode ?? 0))")
            }
            try FileManager.default.moveItem(at: tempLocalURL, to: fileDest)
            downloadedFiles.append(fileDest)

            totalBytesWritten += item.size
            if totalExpectedBytes > 0 {
                let frac = min(1.0, Double(totalBytesWritten) / Double(totalExpectedBytes))
                await progress?(frac)
            }
        }

        await progress?(1.0)
        return downloadedFiles
    }
}

