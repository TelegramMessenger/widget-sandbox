//! widget-sandbox's bundle stamp (`packages/desktop/sandbox/include/twidget/bundle_stamp.h`,
//! `src/bundle_stamp.cpp`; BundleStamp.swift, BundleStamp.kt): the line build-native-bundles.ts writes first
//! in every bundle, from VERSION.json. This glue runs only bundles of its own protocol, profile and sdk;
//! the release itself is not compared. Also the versions themselves (`twidget/version.h`) and the bundles
//! as the glue reads them (`twidget/resources.h`).

#![forbid(unsafe_code)]

use std::collections::BTreeMap;

/// widget-sandbox's `twidget::version` (version.h, generated from VERSION.json): the framework release and
/// the versions this glue and its bundles share.
pub mod version {
    /// The framework release.
    pub const VERSION: &str = "0.6.5";
    /// The wire between this glue and the bundles (docs/protocol.md).
    pub const PROTOCOL: i32 = 1;
    /// The widget profile this glue enforces (docs/profile-v1.md).
    pub const PROFILE: i32 = 1;
    /// The widget SDK (docs/sdk-v1.md).
    pub const SDK: i32 = 1;
}

/// The JavaScript shipped with the core: the three bundles (`SandboxResources`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SandboxResources {
    pub sandbox_bundle: Vec<u8>,
    pub validator_bundle: Vec<u8>,
    pub renderer_bundle: Vec<u8>,
}

/// A bundle's source with each CRLF pair as LF (a lone CR and LF as they are): a checkout may give the
/// bundles CRLF line ends, and their stamp is of the LF source.
pub fn lf_source(mut source: Vec<u8>) -> Vec<u8> {
    if source.windows(2).any(|w| w == b"\r\n") {
        let mut kept = 0;
        for i in 0..source.len() {
            if source[i] != b'\r' || i + 1 == source.len() || source[i + 1] != b'\n' {
                source[kept] = source[i];
                kept += 1;
            }
        }
        source.truncate(kept);
    }
    source
}

fn read_source(path: &str) -> Option<Vec<u8>> {
    std::fs::read(path).ok().map(lf_source)
}

impl SandboxResources {
    /// Reads `dir`/sandbox-bundle.js, validator-bundle.js and renderer-bundle.js (optional) with each CRLF
    /// pair as LF; the C++'s exception text when the sandbox or validator bundle is missing.
    pub fn from_directory(dir: &str) -> Result<SandboxResources, String> {
        let need = |name: &str| {
            read_source(&format!("{dir}/{name}"))
                .ok_or_else(|| format!("WidgetSandbox: missing resource {dir}/{name} \u{2014} run `pnpm build:desktop`"))
        };
        Ok(SandboxResources {
            sandbox_bundle: need("sandbox-bundle.js")?,
            validator_bundle: need("validator-bundle.js")?,
            renderer_bundle: read_source(&format!("{dir}/renderer-bundle.js")).unwrap_or_default(),
        })
    }
}

/// A bundle's stamp, or this glue's own.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BundleStamp {
    pub version: Vec<u8>,
    pub protocol: i32,
    pub profile: i32,
    pub sdk: i32,
}

const FIX: &[u8] = b"the bundles and the glue must come from one release (rebuild the bundles with pnpm build:desktop)";

impl BundleStamp {
    /// This glue's own.
    pub fn glue() -> BundleStamp {
        BundleStamp {
            version: version::VERSION.as_bytes().to_vec(),
            protocol: version::PROTOCOL,
            profile: version::PROFILE,
            sdk: version::SDK,
        }
    }

    /// The stamp on the first line of a bundle's source; None when there is none or it is malformed: the
    /// prefix, then `key=value` pairs separated by one space, unknown keys ignored (a repeated one the last),
    /// `version` non-empty and the three numbers 1 to 6 ASCII digits.
    pub fn parse(source: &[u8]) -> Option<BundleStamp> {
        const PREFIX: &[u8] = b"/* widget-sandbox ";
        const SUFFIX: &[u8] = b" */";
        let line = match source.iter().position(|&c| c == b'\n') {
            Some(n) => &source[..n],
            None => source,
        };
        if line.len() < PREFIX.len() + SUFFIX.len() || !line.starts_with(PREFIX) || !line.ends_with(SUFFIX) {
            return None;
        }
        let body = &line[PREFIX.len()..line.len() - SUFFIX.len()];
        let mut fields: BTreeMap<&[u8], &[u8]> = BTreeMap::new();
        let mut start = 0;
        loop {
            let end = body[start..].iter().position(|&c| c == b' ').map(|n| n + start);
            let pair = &body[start..end.unwrap_or(body.len())];
            match pair.iter().position(|&c| c == b'=') {
                None | Some(0) => return None,
                Some(eq) => fields.insert(&pair[..eq], &pair[eq + 1..]),
            };
            match end {
                None => break,
                Some(end) => start = end + 1,
            }
        }
        let number = |key: &[u8]| -> Option<i32> {
            let digits = fields.get(key)?;
            if digits.is_empty() || digits.len() > 6 {
                return None;
            }
            let mut n = 0;
            for &c in *digits {
                if !c.is_ascii_digit() {
                    return None;
                }
                n = n * 10 + i32::from(c - b'0');
            }
            Some(n)
        };
        let v = fields.get(&b"version"[..]);
        let (p, pr, sdk) = (number(b"protocol"), number(b"profile"), number(b"sdk"));
        match (v, p, pr, sdk) {
            (Some(v), Some(p), Some(pr), Some(sdk)) if !v.is_empty() => {
                Some(BundleStamp { version: v.to_vec(), protocol: p, profile: pr, sdk })
            }
            _ => None,
        }
    }

    /// Why this glue refuses a bundle (`bundle-mismatch`), or None when it runs it. `name` is the bundle's
    /// file name.
    pub fn refusal(source: &[u8], name: &[u8]) -> Option<Vec<u8>> {
        let mine = BundleStamp::glue();
        let Some(stamp) = BundleStamp::parse(source) else {
            return Some([name, b" has no version stamp, this glue is ", &mine.describe(), b": ", FIX].concat());
        };
        if stamp.protocol == mine.protocol && stamp.profile == mine.profile && stamp.sdk == mine.sdk {
            return None;
        }
        Some([name, b" is ", &stamp.describe(), b", this glue is ", &mine.describe(), b": ", FIX].concat())
    }

    /// The first of a set's bundles this glue refuses (sandbox, validator, then the renderer when there is one).
    pub fn refusal_of(resources: &SandboxResources) -> Option<Vec<u8>> {
        if let Some(why) = BundleStamp::refusal(&resources.sandbox_bundle, b"sandbox-bundle.js") {
            return Some(why);
        }
        if let Some(why) = BundleStamp::refusal(&resources.validator_bundle, b"validator-bundle.js") {
            return Some(why);
        }
        if !resources.renderer_bundle.is_empty() {
            return BundleStamp::refusal(&resources.renderer_bundle, b"renderer-bundle.js");
        }
        None
    }

    /// `widget-sandbox 0.6.0 (protocol 1, profile 1, sdk 1)`.
    pub fn describe(&self) -> Vec<u8> {
        [
            &b"widget-sandbox "[..],
            &self.version,
            format!(" (protocol {}, profile {}, sdk {})", self.protocol, self.profile, self.sdk).as_bytes(),
        ]
        .concat()
    }

    /// The line itself, as build-native-bundles.ts writes it.
    pub fn line(&self) -> Vec<u8> {
        [
            &b"/* widget-sandbox version="[..],
            &self.version,
            format!(" protocol={} profile={} sdk={} */", self.protocol, self.profile, self.sdk).as_bytes(),
        ]
        .concat()
    }
}
