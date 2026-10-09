// SPDX-License-Identifier: GPL-2.0-or-later
import BackgroundTasks
import Foundation
import UIKit
#if canImport(ActivityKit)
import ActivityKit
#endif

/// Downloads from a link: one file, or a whole folder served from a computer (tools/serve-folder.py).
///
/// They run in a background URLSession, so they carry on while Husk is in the background or closed, and each file resumes where it
/// stopped. A finished APK is added to the library; anything else goes into Shared Storage, at the path it had in the served folder,
/// which is where games that keep their data on /sdcard look for it.
@MainActor
final class Downloads: ObservableObject {
    static let shared = Downloads()

    struct File: Codable, Hashable {
        var path: String            // where it goes, relative to Shared Storage (a folder) or its file name (a single file)
        var url: String
        var size: Int64             // 0 when not known
        var received: Int64 = 0
        var done = false
        var error: String?
    }

    struct Job: Codable, Identifiable, Hashable {
        enum Kind: String, Codable { case file, folder, apk }
        var id = UUID()
        var title: String
        var source: String
        var kind: Kind
        var files: [File]
        var added = Date()
        var paused = false
        var finished: Date?

        var total: Int64 { files.reduce(0) { $0 + max($1.size, $1.received) } }
        var received: Int64 { files.reduce(0) { $0 + ($1.done ? max($1.size, $1.received) : $1.received) } }
        var filesDone: Int { files.filter(\.done).count }
        var failed: Bool { files.contains { $0.error != nil } }
        var complete: Bool { files.allSatisfy(\.done) }
    }

    @Published private(set) var jobs: [Job] = []
    /// Bytes per second across everything running, smoothed.
    @Published private(set) var speed: Double = 0
    @Published var problem: String?
    /// Live Activities are turned off for Husk in Settings, so a download cannot show on the Lock Screen or in the Dynamic Island.
    @Published var liveActivitiesOff = false

    nonisolated static let sessionID = "com.husk.downloads"
    private let session: URLSession
    private let delegate = LinkDownloadDelegate()
    /// From the app delegate, when iOS woke Husk for this session's events: call it once they are handled.
    var backgroundCompletion: (() -> Void)?

    private init() {
        let config = URLSessionConfiguration.background(withIdentifier: Self.sessionID)
        config.isDiscretionary = false
        config.sessionSendsLaunchEvents = true
        config.allowsCellularAccess = true
        // Several files at once: one stream is all iOS gives a background download over Wi-Fi, and it was about 10 MB/s. Four keep a
        // computer's hard drive reading mostly in long runs (the server reads big pieces), and fill the link.
        config.httpMaximumConnectionsPerHost = 4
        config.timeoutIntervalForRequest = 120
        config.timeoutIntervalForResource = 7 * 24 * 3600
        session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
        jobs = Self.loadJobs()
        reconcile()
    }

    // MARK: adding

    /// Work out what a link is -- a folder's manifest, an APK, or any other file -- and start on it.
    func add(link raw: String) async {
        problem = nil
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard var url = URL(string: text.contains("://") ? text : "http://" + text), url.scheme?.hasPrefix("http") == true else {
            problem = "That is not a web address."
            return
        }
        HuskLog.log("downloads", "adding \(url.absoluteString)")
        // A served folder answers its root with a manifest; so may any link that returns JSON listing files.
        if let manifest = await Self.fetchManifest(url) {
            if url.path.isEmpty || !url.absoluteString.hasSuffix("/") { url = url.appendingPathComponent("") }
            let files = manifest.files.map { f -> File in
                let path = f.path.split(separator: "/").map { $0.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? String($0) }.joined(separator: "/")
                return File(path: f.path, url: URL(string: "f/" + path, relativeTo: url)!.absoluteString, size: f.size)
            }
            guard !files.isEmpty else { problem = "That folder is empty."; return }
            let need = files.reduce(0) { $0 + $1.size }
            if let free = Self.freeSpace(), free < need {
                problem = "This needs \(Self.bytes(need)) and the device has \(Self.bytes(free)) free."
                return
            }
            start(Job(title: manifest.name.isEmpty ? (url.host ?? "Folder") : manifest.name, source: url.absoluteString, kind: .folder, files: files))
            return
        }
        let (name, size) = await Self.probe(url)
        let kind: Job.Kind = name.lowercased().hasSuffix(".apk") ? .apk : .file
        if size > 0, let free = Self.freeSpace(), free < size {
            problem = "This needs \(Self.bytes(size)) and the device has \(Self.bytes(free)) free."
            return
        }
        start(Job(title: name, source: url.absoluteString, kind: kind, files: [File(path: name, url: url.absoluteString, size: size)]))
    }

    private func start(_ job: Job) {
        // The same link again, while it is still downloading: carry on with that one rather than fetch everything twice.
        if let existing = jobs.first(where: { $0.source == job.source && !$0.complete }) {
            HuskLog.log("downloads", "\(job.title) is already downloading; resuming it")
            resume(existing.id)
            return
        }
        jobs.insert(job, at: 0)
        save()
        for f in job.files where !f.done { begin(job.id, f, kind: job.kind) }
        keepRunning()
        HuskLog.log("downloads", "started \(job.title): \(job.files.count) file(s), \(Self.bytes(job.total))")
        LiveDownload.update(jobs: jobs, speed: speed)
    }

    private func begin(_ job: UUID, _ f: File, kind: Job.Kind) {
        guard let url = URL(string: f.url) else { return }
        let task: URLSessionDownloadTask
        if let resume = Self.resumeData(job, f.path) {
            task = session.downloadTask(withResumeData: resume)
            Self.dropResumeData(job, f.path)
        } else {
            task = session.downloadTask(with: url)
        }
        task.taskDescription = TaskTag(job: job, path: f.path, kind: kind).encoded
        task.priority = URLSessionTask.highPriority
        if f.size > 0 { task.countOfBytesClientExpectsToReceive = f.size }
        task.resume()
    }

    // MARK: controls

    func pause(_ id: UUID) {
        guard let i = jobs.firstIndex(where: { $0.id == id }) else { return }
        jobs[i].paused = true
        save()
        session.getAllTasks { tasks in
            for case let t as URLSessionDownloadTask in tasks where TaskTag(t.taskDescription)?.job == id {
                t.cancel { data in if let data, let tag = TaskTag(t.taskDescription) { Downloads.storeResumeData(data, tag.job, tag.path) } }
            }
        }
        LiveDownload.update(jobs: jobs, speed: speed)
    }

    func resume(_ id: UUID) {
        guard let i = jobs.firstIndex(where: { $0.id == id }) else { return }
        jobs[i].paused = false
        defer { keepRunning() }
        for k in jobs[i].files.indices { jobs[i].files[k].error = nil }
        save()
        let job = jobs[i]
        session.getAllTasks { tasks in
            let running = Set(tasks.compactMap { TaskTag($0.taskDescription) }.filter { $0.job == id }.map(\.path))
            Task { @MainActor in for f in job.files where !f.done && !running.contains(f.path) { self.begin(job.id, f, kind: job.kind) } }
        }
    }

    func remove(_ id: UUID) {
        session.getAllTasks { tasks in for t in tasks where TaskTag(t.taskDescription)?.job == id { t.cancel() } }
        if let job = jobs.first(where: { $0.id == id }) { for f in job.files { Self.dropResumeData(id, f.path) } }
        jobs.removeAll { $0.id == id }
        save()
        LiveDownload.update(jobs: jobs, speed: speed)
    }

    func clearFinished() {
        jobs.removeAll { $0.complete }
        save()
    }

    /// iOS 26: ask to keep running after Husk leaves the screen (a continued processing task). The system shows its progress in the
    /// Dynamic Island and on the Lock Screen, and Husk keeps receiving progress, so the numbers there stay live. Only from something
    /// the person did -- adding or resuming a download -- which is what the system allows.
    private func keepRunning() {
        guard jobs.contains(where: { !$0.complete && !$0.paused }) else { return }
        if #available(iOS 26.0, *) { ContinuedDownload.begin(title: jobs.first { !$0.complete && !$0.paused }?.title ?? "Downloads") }
    }

    // MARK: from the session (main actor)

    private var retries: [String: Int] = [:]
    private var lastSample = (time: Date(), bytes: Int64(0))
    private var lastLive = Date.distantPast
    private var lastSave = Date.distantPast

    fileprivate func progressed(_ tag: TaskTag, received: Int64, expected: Int64) {
        guard let j = jobs.firstIndex(where: { $0.id == tag.job }), let k = jobs[j].files.firstIndex(where: { $0.path == tag.path }) else { return }
        jobs[j].files[k].received = received
        if expected > 0 { jobs[j].files[k].size = expected }
        let now = Date(), all = jobs.reduce(0) { $0 + $1.received }
        let dt = now.timeIntervalSince(lastSample.time)
        if dt >= 1 {
            let rate = Double(all - lastSample.bytes) / dt
            if rate >= 0 { speed = speed == 0 ? rate : speed * 0.7 + rate * 0.3 }
            lastSample = (now, all)
        }
        if now.timeIntervalSince(lastLive) > 3 { lastLive = now; LiveDownload.update(jobs: jobs, speed: speed) }
        if now.timeIntervalSince(lastSave) > 10 { lastSave = now; save() }
    }

    fileprivate func finished(_ tag: TaskTag, placedAt: URL?, error: String?) {
        guard let j = jobs.firstIndex(where: { $0.id == tag.job }), let k = jobs[j].files.firstIndex(where: { $0.path == tag.path }) else { return }
        if let error {
            jobs[j].files[k].error = error
            HuskLog.log("downloads", "\(tag.path): \(error)")
        } else {
            jobs[j].files[k].done = true
            jobs[j].files[k].error = nil
            if jobs[j].files[k].size > 0 { jobs[j].files[k].received = jobs[j].files[k].size }
            HuskLog.log("downloads", "\(tag.path) done (\(jobs[j].filesDone)/\(jobs[j].files.count))")
            if tag.kind == .apk, let placedAt { TranslationLayerStore.shared.add([placedAt], move: true) }
        }
        if jobs[j].complete, jobs[j].finished == nil {
            jobs[j].finished = Date()
            HuskLog.log("downloads", "\(jobs[j].title) finished")
        }
        save()
        LiveDownload.update(jobs: jobs, speed: speed)
    }

    fileprivate func failedToStart(_ tag: TaskTag, error: String, resumeData: Data?) {
        if let resumeData { Self.storeResumeData(resumeData, tag.job, tag.path) }
        // A dropped connection or a server that went away for a moment: try again from where it stopped, a few times, then leave it to Retry.
        let key = "\(tag.job)|\(tag.path)"
        if let j = jobs.firstIndex(where: { $0.id == tag.job }), !jobs[j].paused,
           let k = jobs[j].files.firstIndex(where: { $0.path == tag.path }), retries[key, default: 0] < 3 {
            retries[key, default: 0] += 1
            // Waiting first, longer each time: a computer whose server is restarting, or a phone moving between networks, needs a moment.
            let n = retries[key]!, wait: UInt64 = n == 1 ? 5 : n == 2 ? 30 : 120
            HuskLog.log("downloads", "\(tag.path): \(error); trying again in \(wait) s (\(n))")
            let file = jobs[j].files[k]
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: wait * 1_000_000_000)
                guard let job = self.jobs.first(where: { $0.id == tag.job }), !job.paused,
                      let f = job.files.first(where: { $0.path == file.path }), !f.done else { return }
                self.begin(tag.job, f, kind: tag.kind)
            }
            return
        }
        retries[key] = nil
        finished(tag, placedAt: nil, error: error)
    }

    func sessionEventsDone() {
        save()
        LiveDownload.update(jobs: jobs, speed: speed)
        backgroundCompletion?()
        backgroundCompletion = nil
    }

    /// After a relaunch: files whose task is gone (the app was closed while it ran, or the device restarted) start again, from their
    /// resume data when there is some.
    private func reconcile() {
        session.getAllTasks { tasks in
            let running = Set(tasks.compactMap { TaskTag($0.taskDescription) }.map { "\($0.job)|\($0.path)" })
            Task { @MainActor in
                for job in self.jobs where !job.paused {
                    for f in job.files where !f.done && f.error == nil && !running.contains("\(job.id)|\(f.path)") {
                        self.begin(job.id, f, kind: job.kind)
                    }
                }
                LiveDownload.update(jobs: self.jobs, speed: self.speed)
            }
        }
    }

    // MARK: storage

    nonisolated static var stateDir: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Downloads", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
    private static var jobsFile: URL { stateDir.appendingPathComponent("jobs.json") }
    private static func loadJobs() -> [Job] { (try? JSONDecoder().decode([Job].self, from: Data(contentsOf: jobsFile))) ?? [] }
    private func save() { try? JSONEncoder().encode(jobs).write(to: Self.jobsFile, options: .atomic) }

    nonisolated private static func resumeFile(_ job: UUID, _ path: String) -> URL {
        stateDir.appendingPathComponent("resume-\(job.uuidString)-\(path.replacingOccurrences(of: "/", with: "_")).data")
    }
    nonisolated static func storeResumeData(_ d: Data, _ job: UUID, _ path: String) { try? d.write(to: resumeFile(job, path)) }
    nonisolated private static func resumeData(_ job: UUID, _ path: String) -> Data? { try? Data(contentsOf: resumeFile(job, path)) }
    nonisolated private static func dropResumeData(_ job: UUID, _ path: String) { try? FileManager.default.removeItem(at: resumeFile(job, path)) }

    /// Where a finished file goes. A folder's files keep their paths under Shared Storage; a single file goes to its top; an APK waits
    /// in Husk's own space until the library takes it.
    nonisolated static func destination(_ tag: TaskTag) -> URL? {
        let parts = tag.path.split(separator: "/").map(String.init)
        guard !parts.isEmpty, !parts.contains(".."), !parts.contains(".") else { return nil }
        if tag.kind == .apk { return stateDir.appendingPathComponent("incoming", isDirectory: true).appendingPathComponent(parts.last!) }
        return parts.reduce(TranslationLayer.sharedStorage) { $0.appendingPathComponent($1) }
    }

    // MARK: probing a link

    private struct Manifest: Decodable {
        struct Entry: Decodable { let path: String; let size: Int64 }
        let name: String
        let files: [Entry]
        enum CodingKeys: String, CodingKey { case name, files }
        init(from d: Decoder) throws {
            let c = try d.container(keyedBy: CodingKeys.self)
            name = (try? c.decode(String.self, forKey: .name)) ?? ""
            files = try c.decode([Entry].self, forKey: .files)
        }
    }

    private static func fetchManifest(_ url: URL) async -> Manifest? {
        var req = URLRequest(url: url)
        req.httpMethod = "HEAD"
        req.timeoutInterval = 15
        guard let (_, head) = try? await URLSession.shared.data(for: req), let http = head as? HTTPURLResponse,
              (http.value(forHTTPHeaderField: "Content-Type") ?? "").contains("json"), http.expectedContentLength < 64 << 20 else { return nil }
        req.httpMethod = "GET"
        guard let (data, _) = try? await URLSession.shared.data(for: req) else { return nil }
        return try? JSONDecoder().decode(Manifest.self, from: data)
    }

    private static func probe(_ url: URL) async -> (String, Int64) {
        var req = URLRequest(url: url)
        req.httpMethod = "HEAD"
        req.timeoutInterval = 15
        let fallback = url.lastPathComponent.isEmpty ? (url.host ?? "download") : url.lastPathComponent
        guard let (_, r) = try? await URLSession.shared.data(for: req) else { return (fallback, 0) }
        let name = r.suggestedFilename.flatMap { $0.isEmpty || $0 == "Unknown" ? nil : $0 } ?? fallback
        return (name, max(0, r.expectedContentLength))
    }

    static func freeSpace() -> Int64? {
        let values = try? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return values?.volumeAvailableCapacityForImportantUsage
    }

    static func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }
}

/// Which job and file a session task is for, kept in its taskDescription so it survives a relaunch.
struct TaskTag: Codable {
    let job: UUID
    let path: String
    let kind: Downloads.Job.Kind

    init(job: UUID, path: String, kind: Downloads.Job.Kind) { self.job = job; self.path = path; self.kind = kind }
    init?(_ s: String?) {
        guard let s, let d = s.data(using: .utf8), let t = try? JSONDecoder().decode(TaskTag.self, from: d) else { return nil }
        self = t
    }
    var encoded: String { (try? JSONEncoder().encode(self)).flatMap { String(data: $0, encoding: .utf8) } ?? "" }
}

/// The session's delegate, on its own queue. A finished file has to be moved before this callback returns (iOS deletes it after),
/// so that happens here; everything else goes to the main actor.
final class LinkDownloadDelegate: NSObject, URLSessionDownloadDelegate {
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let tag = TaskTag(downloadTask.taskDescription) else { return }
        Task { @MainActor in Downloads.shared.progressed(tag, received: totalBytesWritten, expected: totalBytesExpectedToWrite) }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let tag = TaskTag(downloadTask.taskDescription) else { return }
        if let http = downloadTask.response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            Task { @MainActor in Downloads.shared.finished(tag, placedAt: nil, error: "the server answered \(http.statusCode)") }
            return
        }
        guard let dest = Downloads.destination(tag) else {
            Task { @MainActor in Downloads.shared.finished(tag, placedAt: nil, error: "not a usable path") }
            return
        }
        let fm = FileManager.default
        do {
            try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            if fm.fileExists(atPath: dest.path) { try fm.removeItem(at: dest) }
            try fm.moveItem(at: location, to: dest)
            Task { @MainActor in Downloads.shared.finished(tag, placedAt: dest, error: nil) }
        } catch {
            let message = error.localizedDescription
            Task { @MainActor in Downloads.shared.finished(tag, placedAt: nil, error: message) }
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error, let tag = TaskTag(task.taskDescription) else { return }
        let ns = error as NSError
        if ns.code == NSURLErrorCancelled, ns.userInfo[NSURLSessionDownloadTaskResumeData] == nil { return }   // removed or paused
        let resume = ns.userInfo[NSURLSessionDownloadTaskResumeData] as? Data
        let message = error.localizedDescription
        Task { @MainActor in
            if ns.code == NSURLErrorCancelled { if let resume { Downloads.storeResumeData(resume, tag.job, tag.path) }; return }
            Downloads.shared.failedToStart(tag, error: message, resumeData: resume)
        }
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        Task { @MainActor in Downloads.shared.sessionEventsDone() }
    }
}

/// The Live Activity: one for everything downloading, ended when nothing is.
@MainActor
enum LiveDownload {
    static func update(jobs: [Downloads.Job], speed: Double) {
        if #available(iOS 26.0, *), ContinuedDownload.update(jobs: jobs, speed: speed) {
            // The system's own progress is showing; Husk's Live Activity would only repeat it.
            #if canImport(ActivityKit)
            if #available(iOS 16.1, *) { Live.end() }
            #endif
            return
        }
        #if canImport(ActivityKit)
        if #available(iOS 16.1, *) { Live.update(jobs: jobs, speed: speed) }
        #endif
    }
}

#if canImport(ActivityKit)
@available(iOS 16.1, *)
@MainActor
private enum Live {
    static var activity: Activity<HuskDownloadAttributes>?
    static var started = Date()
    static var logged = false

    static func end() {
        if let a = activity ?? Activity<HuskDownloadAttributes>.activities.first {
            Task { await a.end(dismissalPolicy: .immediate) }
            activity = nil
        }
    }

    static func update(jobs: [Downloads.Job], speed: Double) {
        let active = jobs.filter { !$0.complete && !$0.paused }
        if activity == nil { activity = Activity<HuskDownloadAttributes>.activities.first }
        guard !active.isEmpty else {
            if let a = activity {
                let last = jobs.first
                let state = HuskDownloadAttributes.ContentState(title: last?.title ?? "Downloads", received: last?.received ?? 0, total: last?.total ?? 0,
                                                                finishBy: nil, started: started, finished: !(last?.failed ?? false), failed: last?.failed ?? false)
                Task { await a.end(using: state, dismissalPolicy: .after(Date().addingTimeInterval(60 * 10))) }
                activity = nil
            }
            return
        }
        let received = active.reduce(0) { $0 + $1.received }, total = active.reduce(0) { $0 + $1.total }
        let finishBy = speed > 1 && total > received ? Date().addingTimeInterval(Double(total - received) / speed) : nil
        let title = active.count == 1 ? active[0].title : "\(active.count) downloads"
        var state = HuskDownloadAttributes.ContentState(title: title, received: received, total: total, finishBy: finishBy, started: started,
                                                        finished: false, failed: active.contains { $0.failed })
        state.speed = speed
        state.filesDone = active.reduce(0) { $0 + $1.filesDone }
        state.filesTotal = active.reduce(0) { $0 + $1.files.count }
        if let a = activity, a.activityState == .active {
            Task { await a.update(using: state) }
        } else {
            activity = nil
            started = Date()
            state.started = started
            let auth = ActivityAuthorizationInfo().areActivitiesEnabled
            let widget = Bundle.main.builtInPlugInsURL.map { FileManager.default.fileExists(atPath: $0.appendingPathComponent("HuskDownloadsWidget.appex").path) } ?? false
            if !logged { logged = true; HuskLog.log("downloads", "Live Activity: allowed \(auth), widget extension installed \(widget)") }
            Downloads.shared.liveActivitiesOff = !auth
            guard auth else { return }
            do {
                let a = try Activity.request(attributes: HuskDownloadAttributes(), contentState: state, pushType: nil)
                activity = a
                HuskLog.log("downloads", "Live Activity started (\(a.id))")
            } catch {
                HuskLog.log("downloads", "no Live Activity: \(error.localizedDescription)")
            }
        }
    }
}
#endif

/// The download as a continued processing task (iOS 26): the system keeps Husk running in the background for it and shows its
/// progress in the Dynamic Island and on the Lock Screen.
@available(iOS 26.0, *)
@MainActor
enum ContinuedDownload {
    private static var task: BGContinuedProcessingTask?
    private static var pending: String?
    private static var lastSubtitle = Date.distantPast

    static var running: Bool { task != nil || pending != nil }

    static func begin(title: String) {
        guard !running else { return }
        // The identifier has to start with the app's bundle ID and match a pattern in Info.plist. A sideloader renames the bundle
        // (com.husk.app.<team>) but not Info.plist, so two forms are tried: one from the bundle ID as it is now, which Info.plist's
        // com.husk.app.* covers, and Info.plist's own downloads pattern, as Husk was built.
        let suffix = UUID().uuidString.prefix(8)
        var candidates: [String] = []
        if let bundle = Bundle.main.bundleIdentifier { candidates.append("\(bundle).downloads.\(suffix)") }
        let permitted = Bundle.main.object(forInfoDictionaryKey: "BGTaskSchedulerPermittedIdentifiers") as? [String] ?? []
        if let pattern = permitted.first(where: { $0.hasSuffix(".downloads.*") }) { candidates.append(String(pattern.dropLast()) + suffix) }
        for id in Array(NSOrderedSet(array: candidates)) as? [String] ?? candidates {
            let registered = BGTaskScheduler.shared.register(forTaskWithIdentifier: id, using: .main) { t in
                MainActor.assumeIsolated { started(t) }
            }
            guard registered else { HuskLog.log("downloads", "background task: \(id) not permitted"); continue }
            let request = BGContinuedProcessingTaskRequest(identifier: id, title: "Downloading \(title)", subtitle: "Starting…")
            request.strategy = .fail
            do {
                try BGTaskScheduler.shared.submit(request)
                pending = id
                HuskLog.log("downloads", "background task submitted (\(id))")
                return
            } catch {
                HuskLog.log("downloads", "background task \(id) refused: \(error.localizedDescription)")
            }
        }
        HuskLog.log("downloads", "no background task; using Husk's Live Activity instead")
    }

    private static func started(_ t: BGTask) {
        guard let t = t as? BGContinuedProcessingTask else { t.setTaskCompleted(success: false); return }
        task = t
        pending = nil
        HuskLog.log("downloads", "background task running")
        t.expirationHandler = {
            MainActor.assumeIsolated {
                HuskLog.log("downloads", "background task ended by the system; the downloads carry on in iOS's download service")
                task = nil
            }
        }
        _ = update(jobs: Downloads.shared.jobs, speed: Downloads.shared.speed)
    }

    /// Progress into the task. True while the system's progress is the one showing.
    @discardableResult
    static func update(jobs: [Downloads.Job], speed: Double) -> Bool {
        guard let t = task else { return pending != nil }
        let active = jobs.filter { !$0.complete && !$0.paused }
        if active.isEmpty {
            let failed = jobs.contains { $0.failed }
            t.progress.completedUnitCount = t.progress.totalUnitCount
            t.updateTitle(failed ? "Download stopped" : "Download finished", subtitle: failed ? "Open Husk to retry" : (jobs.first?.title ?? ""))
            t.setTaskCompleted(success: !failed)
            task = nil
            HuskLog.log("downloads", "background task completed")
            return false
        }
        let received = active.reduce(0) { $0 + $1.received }, total = active.reduce(0) { $0 + $1.total }
        // In megabytes: Progress counts in Int64 and the system shows a fraction, so the unit only has to be fine enough.
        t.progress.totalUnitCount = max(1, total / 1_000_000)
        t.progress.completedUnitCount = min(t.progress.totalUnitCount, received / 1_000_000)
        if Date().timeIntervalSince(lastSubtitle) > 2 {
            lastSubtitle = Date()
            var sub = "\(Downloads.bytes(received)) of \(Downloads.bytes(total))"
            if speed > 1 { sub += " · \(Downloads.bytes(Int64(speed)))/s" }
            let files = active.reduce(0) { $0 + $1.files.count }
            if files > 1 { sub += " · \(active.reduce(0) { $0 + $1.filesDone })/\(files) files" }
            if speed > 1, total > received {
                let left = Double(total - received) / speed
                let f = DateComponentsFormatter(); f.allowedUnits = left > 3600 ? [.hour, .minute] : [.minute, .second]; f.unitsStyle = .abbreviated
                if let tl = f.string(from: left) { sub += " · \(tl) left" }
            }
            t.updateTitle(active.count == 1 ? "Downloading \(active[0].title)" : "Downloading \(active.count) items", subtitle: sub)
        }
        return true
    }
}
