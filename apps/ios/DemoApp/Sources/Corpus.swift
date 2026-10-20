import Foundation

/// Corpus files copied into the app bundle by `pnpm build:ios`
/// (Corpus/compat, Corpus/attacks, Corpus/generated/{with,without}-profile,
/// Corpus/htmlbubbles-ported/<chat> — Telegram's demos rewritten as classic widgets,
/// Corpus/common/<batch> — the renderer-comparison corpus on the common CSS profile,
/// Corpus/showcase/<batch> — the interactive, animated demos on the full web profile).
enum Corpus {
    static let generatedGroups = ["generated/with-profile", "generated/without-profile"]
    /// One group per demo chat, as copied by the build script (corpus/htmlbubbles-ported/README.md).
    static let htmlbubblesGroups = subGroups(of: "htmlbubbles-ported")
    /// The renderer-comparison corpus: one group per batch (corpus/common/README.md).
    static let commonGroups = subGroups(of: "common")
    /// The showcase: interactive, animated demos on the full web profile, one group per batch (corpus/showcase/README.md).
    static let showcaseGroups = subGroups(of: "showcase")

    private static func subGroups(of parent: String) -> [String] {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Corpus/\(parent)"),
              let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return [] }
        return names.filter { !$0.hasPrefix(".") && $0 != "README.md" }.sorted().map { "\(parent)/\($0)" }
    }

    static func files(in group: String) -> [String] {
        guard let urls = Bundle.main.urls(forResourcesWithExtension: "html", subdirectory: "Corpus/\(group)") else { return [] }
        return urls.map(\.lastPathComponent).sorted()
    }

    static func html(group: String, file: String) -> String {
        let name = (file as NSString).deletingPathExtension
        guard let url = Bundle.main.url(forResource: name, withExtension: "html", subdirectory: "Corpus/\(group)") else { return "<p>missing \(file)</p>" }
        return (try? String(contentsOf: url, encoding: .utf8)) ?? "<p>unreadable</p>"
    }
}
