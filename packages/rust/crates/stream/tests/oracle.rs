//! The Rust frame reader and codec against remote_wire.h's (`oracle/stream_oracle`): runs of valid frames
//! of every kind and their random changes, read from either side under several bounds and read sizes,
//! encodes of fields that match their kind's shape and of fields that do not, and the profile page's two
//! kinds by name. Skipped without `TWIDGET_ORACLE_DIR`; `tools/gate.py` sets it.

mod stream;

use stream::Chunks;
use twidget_stream::*;
use twidget_testkit::{Comparison, Lcg, Oracle, framed, mutate};

fn any_field(random: &mut Lcg, letter: u8, keep: &mut Vec<Vec<u8>>) -> usize {
    let bytes: Vec<u8> = match letter {
        b's' => {
            let n = if random.chance(10) { random.below(3000) } else { random.below(24) };
            (0..n).map(|_| random.next() as u8).collect()
        }
        _ => Vec::new(),
    };
    keep.push(bytes);
    keep.len() - 1
}

/// A valid frame of a random kind, its fields drawn at random (and sometimes at the values the reader checks).
fn any_frame(random: &mut Lcg) -> (Side, Vec<u8>) {
    let kinds: Vec<u8> = (0..=255u8).filter(|k| shape(*k).is_some()).collect();
    let kind = kinds[random.below(kinds.len())];
    let s = shape(kind).unwrap();
    let runtime = if s.connection { 0 } else { 1 + random.below(5) as u32 };
    let mut keep = Vec::new();
    let mut fields = Vec::new();
    for letter in s.fields.bytes() {
        let at = any_field(random, letter, &mut keep);
        fields.push((letter, at));
    }
    let i64v = match random.below(4) {
        0 => [i64::MIN, i64::MAX, 0, 1, -1, i64::from(i32::MAX), i64::from(i32::MIN), i64::from(i32::MAX) + 1][random.below(8)],
        _ => i64::from(random.next() as i32),
    };
    let built: Vec<Field> = fields
        .iter()
        .map(|(letter, at)| match letter {
            b's' => Field::Str(&keep[*at]),
            b'i' => Field::I64(i64v),
            b'f' => Field::F64(f64::from_bits((u64::from(random.next()) << 32) | u64::from(random.next()))),
            _ => Field::Bool(random.chance(2)),
        })
        .collect();
    let mut built = built;
    if kind == Kind::QueryAnswer as u8 && random.chance(2) {
        built[1] = Field::Bool(false);
        built[2] = Field::Str(b"");
    }
    let side = s.from;
    (side, encode(runtime, Kind::from_u8(kind).unwrap(), MAX_FRAME_BYTES, &built).unwrap())
}

fn put_u64(out: &mut Vec<u8>, v: u64) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn put_str(out: &mut Vec<u8>, s: &[u8]) {
    out.extend_from_slice(&(s.len() as u32).to_le_bytes());
    out.extend_from_slice(s);
}

/// The oracle's 'r' answer, computed by the Rust reader.
fn read_answer(side: Side, bound: usize, chunk: usize, bytes: &[u8]) -> (u8, Vec<u8>) {
    let stream = Chunks::new(bytes.to_vec(), chunk);
    let mut reader = FrameReader::new(&stream, side);
    let mut out = Vec::new();
    let failure = loop {
        match reader.next(bound) {
            Ok(Some(f)) => {
                out.push(1);
                out.extend_from_slice(&f.runtime.to_le_bytes());
                out.push(f.kind as u8);
                for s in f.str {
                    put_str(&mut out, s);
                }
                for d in f.f64 {
                    put_u64(&mut out, d.to_bits());
                }
                put_u64(&mut out, f.i64 as u64);
                out.push(u8::from(f.flag));
            }
            Ok(None) => break String::new(),
            Err(e) => break e,
        }
    };
    out.push(0);
    put_str(&mut out, failure.as_bytes());
    (1, out)
}

fn read_case(c: &mut Comparison, side: Side, bound: usize, chunk: usize, bytes: &[u8], what: String) {
    let mut input = vec![u8::from(side == Side::Engine)];
    put_u64(&mut input, bound as u64);
    input.extend_from_slice(&(chunk as u32).to_le_bytes());
    input.extend_from_slice(bytes);
    c.case(&framed(b'r', &input), read_answer(side, bound, chunk, bytes), what);
}

#[test]
fn frame_runs_read_alike() {
    let Some(oracle) = Oracle::find("stream_oracle") else { return };
    let mut random = Lcg::new(0x57_0001);
    let mut c = Comparison::new();
    for i in 0..6000 {
        let (side, first) = any_frame(&mut random);
        let mut run = first;
        for _ in 0..random.below(4) {
            let (other, more) = any_frame(&mut random);
            if other == side || random.chance(5) {
                run.extend(more);
            }
        }
        let bound = [MAX_FRAME_BYTES, 64, 20, 5, 4, 3000][random.below(6)];
        let chunk = [1, 2, 3, 7, 64, 65536][random.below(6)];
        read_case(&mut c, side, bound, chunk, &run, format!("run {i}"));
        let mut changed = run.clone();
        let edits = mutate(&mut random, &mut changed);
        read_case(&mut c, side, bound, chunk, &changed, format!("run {i} changed{edits}"));
        let other = if side == Side::Host { Side::Engine } else { Side::Host };
        read_case(&mut c, other, bound, chunk, &run, format!("run {i} from the other side"));
    }
    let tally = c.check(&oracle);
    eprintln!("frame runs: {} read alike", tally[1]);
}

/// The oracle's 'e' record of `fields` for `kind` on `runtime` under `bound`, with the Rust codec's answer.
fn encode_case(c: &mut Comparison, runtime: u32, kind: u8, bound: usize, fields: &[Field], what: String) {
    let mut input = runtime.to_le_bytes().to_vec();
    input.push(kind);
    put_u64(&mut input, bound as u64);
    input.push(fields.len() as u8);
    for f in fields {
        match *f {
            Field::Str(s) => {
                input.push(b's');
                put_str(&mut input, s);
            }
            Field::I64(v) => {
                input.push(b'i');
                put_u64(&mut input, v as u64);
            }
            Field::F64(v) => {
                input.push(b'f');
                put_u64(&mut input, v.to_bits());
            }
            Field::Bool(v) => {
                input.push(b'b');
                input.push(u8::from(v));
            }
        }
    }
    let rust = match Kind::from_u8(kind).and_then(|k| encode(runtime, k, bound, fields)) {
        Some(bytes) => (1, bytes),
        None => (0, Vec::new()),
    };
    c.case(&framed(b'e', &input), rust, what);
}

#[test]
fn encodes_alike() {
    let Some(oracle) = Oracle::find("stream_oracle") else { return };
    let mut random = Lcg::new(0x57_0002);
    let mut c = Comparison::new();
    let letters = [b's', b'i', b'f', b'b'];
    // Every known kind, the last ones too, however long the table grows.
    let known: Vec<u8> = (0..=255u8).filter(|k| shape(*k).is_some()).collect();
    for i in 0..20000 {
        let kind = if random.chance(20) { random.next() as u8 } else { known[random.below(known.len())] };
        let shaped = shape(kind).map(|s| s.fields.as_bytes().to_vec()).unwrap_or_default();
        let mut types: Vec<u8> = shaped.clone();
        if random.chance(5) {
            types = (0..random.below(5)).map(|_| letters[random.below(4)]).collect();
        }
        let runtime = random.next();
        let bound = [MAX_FRAME_BYTES, 4, 5, 9, 13, 30, 100, usize::MAX][random.below(8)];
        let mut keep = Vec::new();
        let mut fields_bits = Vec::new();
        for &t in &types {
            let v = match t {
                b's' => any_field(&mut random, b's', &mut keep) as u64,
                b'b' => u64::from(random.chance(2)),
                _ => (u64::from(random.next()) << 32) | u64::from(random.next()),
            };
            fields_bits.push((t, v));
        }
        let fields: Vec<Field> = fields_bits
            .iter()
            .map(|(t, v)| match t {
                b's' => Field::Str(&keep[*v as usize]),
                b'b' => Field::Bool(*v != 0),
                b'i' => Field::I64(*v as i64),
                _ => Field::F64(f64::from_bits(*v)),
            })
            .collect();
        encode_case(&mut c, runtime, kind, bound, &fields, format!("encode {i} of kind {kind}"));
    }
    let tally = c.check(&oracle);
    eprintln!("encodes: {} refused, {} encoded alike", tally[0], tally[1]);
    assert!(tally[0] > 0 && tally[1] > 0);
}

/// The failure the Rust reader ends `bytes` with, read from `side`.
fn read_failure(side: Side, bytes: &[u8]) -> String {
    let stream = Chunks::new(bytes.to_vec(), 64);
    let mut reader = FrameReader::new(&stream, side);
    loop {
        match reader.next(MAX_FRAME_BYTES) {
            Ok(Some(_)) => continue,
            Ok(None) => return String::new(),
            Err(e) => return e,
        }
    }
}

/// The profile page's two kinds by name: the device's edges (22, one string, the coded object) and a page's
/// viewport (37, one f64, nonfinite and signed zeros among them) encoded with their own fields and with fields
/// of another type or count, under bounds at, one below and past their size, on a runtime and on the
/// connection; then read alone and in a run with older kinds from either side under the same bounds, on the
/// connection, cut short, with a byte after them, and kind 23 next to them, still unknown.
#[test]
fn profile_page_kinds_encode_and_read_alike() {
    let device: &[u8] = br#"{"top":120,"right":0,"bottom":34,"left":-5}"#;
    let heights =
        [844.0, 700.12, 0.5, 0.0, -0.0, 4000.0, 4000.01, -1.0, f64::NAN, f64::INFINITY, f64::NEG_INFINITY, f64::MIN_POSITIVE, f64::MAX];
    let area = encode(1, Kind::DeviceSafeArea, MAX_FRAME_BYTES, &[Field::Str(device)]).unwrap();
    let viewport = encode(1, Kind::HostedViewportHeight, MAX_FRAME_BYTES, &[Field::F64(700.12)]).unwrap();
    assert_eq!(read_failure(Side::Host, &[area.clone(), viewport.clone()].concat()), "");
    assert_eq!(read_failure(Side::Engine, &area), "kind 22 from the other side");
    assert_eq!(read_failure(Side::Engine, &viewport), "kind 37 from the other side");
    assert_eq!(read_failure(Side::Host, b"\x05\0\0\0\x01\0\0\0\x17"), "an unknown kind 23");

    let Some(oracle) = Oracle::find("stream_oracle") else { return };
    let mut c = Comparison::new();
    let strings: [&[u8]; 4] = [device, b"", b"[]", b"\xFF\0{\"top\":1e999}"];
    let mut kept = Vec::new();
    for (i, s) in strings.iter().enumerate() {
        for runtime in [1, 0, u32::MAX] {
            let size = 5 + 4 + s.len();
            for bound in [MAX_FRAME_BYTES, size, size - 1, 5, 4, usize::MAX] {
                encode_case(&mut c, runtime, 22, bound, &[Field::Str(s)], format!("device {i} on {runtime} under {bound}"));
            }
        }
        kept.push(encode(1 + i as u32, Kind::DeviceSafeArea, MAX_FRAME_BYTES, &[Field::Str(s)]).unwrap());
    }
    for (i, h) in heights.iter().enumerate() {
        for runtime in [1, 0, u32::MAX] {
            for bound in [MAX_FRAME_BYTES, 13, 12, 5, 4, usize::MAX] {
                encode_case(&mut c, runtime, 37, bound, &[Field::F64(*h)], format!("viewport {h} on {runtime} under {bound}"));
            }
        }
        kept.push(encode(1 + i as u32, Kind::HostedViewportHeight, MAX_FRAME_BYTES, &[Field::F64(*h)]).unwrap());
    }
    let others: [&[Field]; 6] =
        [&[], &[Field::F64(1.0)], &[Field::I64(1)], &[Field::Bool(true)], &[Field::Str(b"844")], &[Field::Str(b"{}"), Field::F64(1.0)]];
    for kind in [22, 37, 23] {
        for (i, fields) in others.iter().enumerate() {
            encode_case(&mut c, 1, kind, MAX_FRAME_BYTES, fields, format!("kind {kind} with fields {i}"));
        }
    }

    let content = encode(1, Kind::SafeArea, MAX_FRAME_BYTES, &[Field::Str(b"{\"bottom\":24}")]).unwrap();
    let close = encode(1, Kind::Close, MAX_FRAME_BYTES, &[]).unwrap();
    let log = encode(1, Kind::Log, MAX_FRAME_BYTES, &[Field::Str(b"info"), Field::Str(b"x")]).unwrap();
    let mut runs = kept;
    runs.push([content, area.clone(), viewport.clone(), close].concat());
    runs.push([area.clone(), log, viewport.clone()].concat());
    runs.push([area.clone(), b"\x05\0\0\0\x01\0\0\0\x17".to_vec(), viewport.clone()].concat());
    for frame in [&area, &viewport] {
        let mut on_connection = frame.clone();
        on_connection[4..8].copy_from_slice(&0u32.to_le_bytes());
        let mut after = frame.clone();
        after[0] += 1;
        after.push(b'x');
        let mut short = frame.clone();
        short[0] -= 1;
        short.pop();
        runs.extend([on_connection, after, short, frame[..frame.len() - 1].to_vec()]);
    }
    let mut long_string = area.clone();
    long_string[9..13].copy_from_slice(&(device.len() as u32 + 1).to_le_bytes());
    runs.push(long_string);
    for (i, run) in runs.iter().enumerate() {
        for side in [Side::Host, Side::Engine] {
            for bound in [MAX_FRAME_BYTES, run.len() - 4, run.len() - 5, 12, 5] {
                for chunk in [1, 3, 65536] {
                    read_case(&mut c, side, bound, chunk, run, format!("run {i} from {side:?} under {bound} in {chunk}"));
                }
            }
        }
    }
    let tally = c.check(&oracle);
    eprintln!("the profile page's kinds: {} refused, {} encoded or read alike", tally[0], tally[1]);
    assert!(tally[0] > 0 && tally[1] > 0);
}
