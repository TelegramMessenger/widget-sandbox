//! widget-sandbox's bundle stamp test (packages/desktop/tests/test_main.cpp: bundle_stamp_shipped_and_parsed,
//! on the shipped bundles in packages/desktop/WidgetJS as the suite reads them), ported, and this port's
//! versions against VERSION.json and version.h, which are generated from it. The engine's and the
//! validator's refusals at boot (bundle_stamp_engine_and_validator_refuse_another_release) come with them.

use std::path::{Path, PathBuf};

use twidget_bundle_stamp::*;
use twidget_json::Json;

fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

fn shipped() -> SandboxResources {
    let dir = widget_sandbox().join("packages/desktop/WidgetJS");
    SandboxResources::from_directory(dir.to_str().expect("a UTF-8 path")).expect("the shipped bundles")
}

/// `source` with its first line replaced by `line` (empty: removed).
fn restamped(source: &[u8], line: &[u8]) -> Vec<u8> {
    let rest = match source.iter().position(|&c| c == b'\n') {
        Some(nl) => &source[nl + 1..],
        None => &b""[..],
    };
    if line.is_empty() { rest.to_vec() } else { [line, b"\n", rest].concat() }
}

fn resources_with(res: &SandboxResources, which: &str, line: &[u8]) -> SandboxResources {
    let mut res = res.clone();
    let bundle = match which {
        "sandbox" => &mut res.sandbox_bundle,
        "validator" => &mut res.validator_bundle,
        _ => &mut res.renderer_bundle,
    };
    *bundle = restamped(bundle, line);
    res
}

fn stamp(version: &str, protocol: i32, profile: i32, sdk: i32) -> BundleStamp {
    BundleStamp { version: version.as_bytes().to_vec(), protocol, profile, sdk }
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

#[test]
fn bundle_stamp_shipped_and_parsed() {
    let res = shipped();
    let mine = BundleStamp::glue();
    assert!(BundleStamp::refusal_of(&res).is_none(), "{}", text(&BundleStamp::refusal_of(&res).unwrap_or_default()));
    for source in [&res.sandbox_bundle, &res.validator_bundle, &res.renderer_bundle] {
        let parsed = BundleStamp::parse(source);
        assert!(parsed.as_ref() == Some(&mine), "a shipped bundle is not this release's: {}", text(&source[..80]));
    }
    let parsed = BundleStamp::parse(b"/* widget-sandbox version=1.2.3-rc.1 protocol=7 profile=2 sdk=3 later=x */\n\"use strict\";");
    assert_eq!(parsed, Some(stamp("1.2.3-rc.1", 7, 2, 3)));
    assert_eq!(BundleStamp::parse(&mine.line()), Some(mine.clone()));
    for line in [
        "\"use strict\";(()=>{",
        "/* widget-sandbox */",
        "/* widget-sandbox version=0.6.0 protocol=1 profile=1 */",
        "/* widget-sandbox version=0.6.0 protocol=x profile=1 sdk=1 */",
        "/* widget-sandbox version=0.6.0 protocol=-1 profile=1 sdk=1 */",
        "/* widget-sandbox version= protocol=1 profile=1 sdk=1 */",
        "/* widget-sandbox version=0.6.0  protocol=1 profile=1 sdk=1 */",
        "/* widget-sandbox version=0.6.0 protocol=1 profile=1 sdk=1",
        "\n/* widget-sandbox version=0.6.0 protocol=1 profile=1 sdk=1 */",
        "/* widget-sandbox version=0.6.0 protocol=1234567 profile=1 sdk=1 */",
    ] {
        assert!(BundleStamp::parse(line.as_bytes()).is_none(), "{line}");
    }
    assert!(
        BundleStamp::refusal(&stamp("0.0.1", mine.protocol, mine.profile, mine.sdk).line(), b"x.js").is_none(),
        "another release with the same numbers runs"
    );
    let other = text(
        &BundleStamp::refusal(&stamp("9.9.9", mine.protocol + 1, mine.profile, mine.sdk).line(), b"sandbox-bundle.js").unwrap_or_default(),
    );
    assert!(other.starts_with(&format!("sandbox-bundle.js is widget-sandbox 9.9.9 (protocol {},", mine.protocol + 1)), "{other}");
    assert!(other.contains(&format!("this glue is {}", text(&mine.describe()))), "{other}");
    assert!(BundleStamp::refusal(&stamp("9.9.9", mine.protocol, mine.profile + 1, mine.sdk).line(), b"x.js").is_some());
    assert!(BundleStamp::refusal(&stamp("9.9.9", mine.protocol, mine.profile, mine.sdk + 1).line(), b"x.js").is_some());
    let none = text(&BundleStamp::refusal(b"\"use strict\";", b"validator-bundle.js").unwrap_or_default());
    assert!(none.starts_with("validator-bundle.js has no version stamp"), "{none}");
    let renderer = resources_with(&res, "renderer", &stamp("9.9.9", mine.protocol, mine.profile, 99).line());
    assert!(text(&BundleStamp::refusal_of(&renderer).unwrap_or_default()).starts_with("renderer-bundle.js is "));
}

/// The refusal texts in full, and which bundle of a set is named: the sandbox's first, a missing renderer
/// bundle none.
#[test]
fn refusals_name_the_first_refused_bundle() {
    let res = shipped();
    let mine = BundleStamp::glue();
    assert_eq!(
        text(&BundleStamp::refusal(b"x", b"sandbox-bundle.js").unwrap_or_default()),
        format!(
            "sandbox-bundle.js has no version stamp, this glue is widget-sandbox {} (protocol {}, profile {}, sdk {}): the bundles and the glue \
             must come from one release (rebuild the bundles with pnpm build:desktop)",
            version::VERSION,
            version::PROTOCOL,
            version::PROFILE,
            version::SDK
        )
    );
    let other = stamp("9.9.9", mine.protocol + 1, mine.profile, mine.sdk).line();
    let both = resources_with(&resources_with(&res, "sandbox", &other), "validator", b"");
    assert!(text(&BundleStamp::refusal_of(&both).unwrap_or_default()).starts_with("sandbox-bundle.js is widget-sandbox 9.9.9"));
    let validator = resources_with(&res, "validator", b"");
    assert!(text(&BundleStamp::refusal_of(&validator).unwrap_or_default()).starts_with("validator-bundle.js has no version stamp"));
    let mut without = res.clone();
    without.renderer_bundle.clear();
    assert!(BundleStamp::refusal_of(&without).is_none());
}

/// version.h and this port's `version` are both generated from VERSION.json, by hand here: they must agree.
#[test]
fn versions_are_version_json() {
    let root = widget_sandbox();
    let json = Json::parse(std::fs::read(root.join("VERSION.json")).expect("VERSION.json")).expect("VERSION.json parses");
    assert_eq!(json["version"].as_str(), version::VERSION.as_bytes());
    assert_eq!(json["protocol"].as_int(0), version::PROTOCOL);
    assert_eq!(json["profile"].as_int(0), version::PROFILE);
    assert_eq!(json["sdk"].as_int(0), version::SDK);
    let header = std::fs::read_to_string(root.join("packages/desktop/sandbox/include/twidget/version.h")).expect("version.h");
    for line in [
        format!("inline constexpr const char* VERSION = \"{}\";", version::VERSION),
        format!("inline constexpr int PROTOCOL = {};", version::PROTOCOL),
        format!("inline constexpr int PROFILE = {};", version::PROFILE),
        format!("inline constexpr int SDK = {};", version::SDK),
    ] {
        assert!(header.contains(&line), "version.h has no {line}");
    }
}

/// CRLF pairs become LF, nothing else changes: a lone CR, a CR before the end, LF CR.
#[test]
fn bundles_read_with_crlf_as_lf() {
    assert_eq!(lf_source(b"a\r\nb\r\n".to_vec()), b"a\nb\n");
    assert_eq!(lf_source(b"a\rb\n\r".to_vec()), b"a\rb\n\r");
    assert_eq!(lf_source(b"\r\r\n\n\r".to_vec()), b"\r\n\n\r");
    assert_eq!(lf_source(Vec::new()), b"");
    let missing = SandboxResources::from_directory("no/such/dir").expect_err("no bundles there");
    assert_eq!(missing, "WidgetSandbox: missing resource no/such/dir/sandbox-bundle.js \u{2014} run `pnpm build:desktop`");
}
