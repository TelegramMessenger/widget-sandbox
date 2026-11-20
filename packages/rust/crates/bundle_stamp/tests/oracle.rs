//! The Rust bundle stamp against widget-sandbox's bundle_stamp.cpp and resources.cpp
//! (`oracle/bundle_stamp_oracle`): generated and mutated first lines through `parse` and `refusal`, sets of
//! bundles through the set's refusal, stamps of any numbers through `describe` and `line`, and directories of
//! bundles with CRLF, CR and LF through `from_directory`. Skipped without `TWIDGET_ORACLE_DIR`.

use std::path::{Path, PathBuf};

use twidget_bundle_stamp::*;
use twidget_testkit::{Comparison, Fields, Lcg, Oracle, mutate};

fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

fn pick<T: Copy>(random: &mut Lcg, items: &[T]) -> T {
    items[random.below(items.len())]
}

fn parsed(source: &[u8]) -> (u8, Vec<u8>) {
    match BundleStamp::parse(source) {
        None => (0, Vec::new()),
        Some(s) => (1, Fields::new().str(&s.version).i32(s.protocol).i32(s.profile).i32(s.sdk).take()),
    }
}

fn refused(why: Option<Vec<u8>>) -> (u8, Vec<u8>) {
    match why {
        None => (0, Vec::new()),
        Some(why) => (1, why),
    }
}

fn stamped(s: &BundleStamp) -> (u8, Vec<u8>) {
    (1, Fields::new().str(&s.version).i32(s.protocol).i32(s.profile).i32(s.sdk).str(&s.describe()).str(&s.line()).take())
}

/// A first line of pairs from the stamp's own pieces and their near misses.
fn line(random: &mut Lcg) -> Vec<u8> {
    let prefix: &[u8] = if random.chance(6) {
        pick(random, &[&b"/* widget-sandbox"[..], b"/*  widget-sandbox ", b"", b"/* Widget-sandbox ", b"/* widget-sandbox  "])
    } else {
        b"/* widget-sandbox "
    };
    let suffix: &[u8] = if random.chance(6) { pick(random, &[&b"*/"[..], b" */ ", b"", b" */\r", b" **/", b"  */"]) } else { b" */" };
    let keys = [&b"version"[..], b"protocol", b"profile", b"sdk", b"later", b"", b"Version", b"sdk ", b"v=x"];
    let numbers =
        [&b"1"[..], b"1", b"0", b"2", b"000001", b"999999", b"1234567", b"", b"x", b"-1", b"+1", b"12a", b"7", b"01", b"1 ", b"\xFF"];
    let versions = [&b"0.6.5"[..], b"1.2.3-rc.1", b"", b"=", b"a=b", b"\xC3\xA9", b"9.9.9"];
    let value = |random: &mut Lcg, key: &[u8]| {
        if key == b"version" { pick(random, &versions).to_vec() } else { pick(random, &numbers).to_vec() }
    };
    let mut pairs: Vec<(Vec<u8>, Vec<u8>)> = Vec::new();
    if !random.chance(4) {
        for key in [&b"version"[..], b"protocol", b"profile", b"sdk"] {
            let v = if random.chance(3) {
                value(random, key)
            } else if key == b"version" {
                b"0.6.5".to_vec()
            } else {
                b"1".to_vec()
            };
            pairs.push((key.to_vec(), v));
        }
    }
    for _ in 0..random.below(4) {
        let at = random.below(pairs.len() + 1);
        let key = pick(random, &keys);
        pairs.insert(at, (key.to_vec(), value(random, key)));
    }
    if !pairs.is_empty() && random.chance(5) {
        pairs.remove(random.below(pairs.len()));
    }
    let mut body = Vec::new();
    for (i, (k, v)) in pairs.iter().enumerate() {
        if i > 0 {
            body.extend_from_slice(if random.chance(12) { pick(random, &[&b"  "[..], b"\t", b""]) } else { b" " });
        }
        body.extend_from_slice(k);
        if !random.chance(30) {
            body.push(b'=');
        }
        body.extend_from_slice(v);
    }
    let mut out = [prefix, &body, suffix].concat();
    out.extend_from_slice(pick(
        random,
        &[&b"\n\"use strict\";"[..], b"\r\n(()=>{", b"", b"\n", b"\nx\n/* widget-sandbox version=1 protocol=1 profile=1 sdk=1 */"],
    ));
    out
}

#[test]
fn stamps_alike() {
    let Some(oracle) = Oracle::find("bundle_stamp_oracle") else { return };
    let shipped =
        SandboxResources::from_directory(widget_sandbox().join("packages/desktop/WidgetJS").to_str().expect("UTF-8")).expect("bundles");
    let first = |s: &[u8]| s[..s.iter().position(|&c| c == b'\n').unwrap_or(s.len())].to_vec();
    let real =
        [first(&shipped.sandbox_bundle), first(&shipped.validator_bundle), first(&shipped.renderer_bundle), BundleStamp::glue().line()];
    let mut random = Lcg::new(0x5_7a3b);
    let mut c = Comparison::new();
    let mut f = Fields::new();
    let mut taken = 0;
    c.case(&f.framed(b'g'), stamped(&BundleStamp::glue()), "the glue's stamp");
    for i in 0..30000 {
        let mut source = if random.chance(3) {
            let mut s = real[random.below(real.len())].clone();
            s.extend_from_slice(pick(&mut random, &[&b"\n\"use strict\";"[..], b"", b"\r\n"]));
            s
        } else {
            line(&mut random)
        };
        let edits = if random.chance(3) { mutate(&mut random, &mut source) } else { String::new() };
        taken += usize::from(BundleStamp::parse(&source).is_some());
        let what = format!("source {i} {:?}{edits}", String::from_utf8_lossy(&source));
        c.case(&f.str(&source).framed(b'p'), parsed(&source), format!("parse of {what}"));
        f.take();
        let name = pick(&mut random, &[&b"sandbox-bundle.js"[..], b"x.js", b"", b"\xff\x00"]);
        c.case(&f.str(&source).str(name).framed(b'r'), refused(BundleStamp::refusal(&source, name)), format!("refusal of {what}"));
        f.take();
        let s = BundleStamp {
            version: pick(&mut random, &[&b"0.6.5"[..], b"", b"9.9.9", b"\xff\"x"]).to_vec(),
            protocol: pick(&mut random, &[1, 0, -1, i32::MAX, i32::MIN, 99, 999999]),
            profile: pick(&mut random, &[1, 2, -7, 123456]),
            sdk: random.next() as i32,
        };
        c.case(&f.str(&s.version).i32(s.protocol).i32(s.profile).i32(s.sdk).framed(b'l'), stamped(&s), format!("stamp {s:?}"));
        f.take();
        if i % 3 == 0 {
            let mut set = SandboxResources::default();
            for bundle in [&mut set.sandbox_bundle, &mut set.validator_bundle, &mut set.renderer_bundle] {
                *bundle = match random.below(4) {
                    0 => Vec::new(),
                    1 => line(&mut random),
                    _ => real[random.below(real.len())].clone(),
                };
            }
            let record = f.str(&set.sandbox_bundle).str(&set.validator_bundle).str(&set.renderer_bundle).framed(b'R');
            f.take();
            c.case(&record, refused(BundleStamp::refusal_of(&set)), format!("refusal of set {i}"));
        }
    }
    let record = f.str(&shipped.sandbox_bundle).str(&shipped.validator_bundle).str(&shipped.renderer_bundle).framed(b'R');
    c.case(&record, refused(BundleStamp::refusal_of(&shipped)), "the shipped set");
    let tally = c.check(&oracle);
    eprintln!("bundle stamps: {} answers alike, {taken} of 30000 sources stamped", tally[0] + tally[1]);
    assert!(taken > 5000, "{taken} stamps parsed");
}

/// Directories of bundles with every line end, and with bundles missing, read by both.
#[test]
fn directories_alike() {
    let Some(oracle) = Oracle::find("bundle_stamp_oracle") else { return };
    let root = Path::new(env!("CARGO_TARGET_TMPDIR")).join("bundle_stamp_directories");
    let _ = std::fs::remove_dir_all(&root);
    let mut random = Lcg::new(0xd1_2ec7);
    let mut c = Comparison::new();
    let mut f = Fields::new();
    for i in 0..300 {
        let dir = root.join(format!("d{i}"));
        std::fs::create_dir_all(&dir).expect("a test directory");
        for name in ["sandbox-bundle.js", "validator-bundle.js", "renderer-bundle.js"] {
            if random.chance(9) {
                continue;
            }
            let bytes: Vec<u8> =
                (0..random.below(40)).map(|_| pick(&mut random, &[b'\r', b'\n', b'\r', b'a', b'\n', b' ', 0xff])).collect();
            std::fs::write(dir.join(name), bytes).expect("a bundle");
        }
        let path = dir.to_str().expect("a UTF-8 path").to_string();
        let rust = match SandboxResources::from_directory(&path) {
            Ok(r) => (1, Fields::new().str(&r.sandbox_bundle).str(&r.validator_bundle).str(&r.renderer_bundle).take()),
            Err(e) => (0, e.into_bytes()),
        };
        c.case(&f.str(path.as_bytes()).framed(b'D'), rust, format!("directory {i}"));
        f.take();
    }
    let tally = c.check(&oracle);
    let _ = std::fs::remove_dir_all(&root);
    eprintln!("bundle directories: {} missing a bundle, {} read, alike", tally[0], tally[1]);
    assert!(tally[0] > 10 && tally[1] > 100);
}
