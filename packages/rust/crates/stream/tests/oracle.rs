//! The Rust frame reader and codec against remote_wire.h's (`oracle/stream_oracle`): runs of valid frames
//! of every kind and their random changes, read from either side under several bounds and read sizes, and
//! encodes of fields that match their kind's shape and of fields that do not. Skipped without
//! `TWIDGET_ORACLE_DIR`; `tools/gate.py` sets it.

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

#[test]
fn encodes_alike() {
    let Some(oracle) = Oracle::find("stream_oracle") else { return };
    let mut random = Lcg::new(0x57_0002);
    let mut c = Comparison::new();
    let letters = [b's', b'i', b'f', b'b'];
    for i in 0..20000 {
        let kind = if random.chance(20) {
            random.next() as u8
        } else {
            (0..=255u8).filter(|k| shape(*k).is_some()).nth(random.below(56)).unwrap()
        };
        let shaped = shape(kind).map(|s| s.fields.as_bytes().to_vec()).unwrap_or_default();
        let mut types: Vec<u8> = shaped.clone();
        if random.chance(5) {
            types = (0..random.below(5)).map(|_| letters[random.below(4)]).collect();
        }
        let runtime = random.next();
        let bound = [MAX_FRAME_BYTES, 4, 5, 9, 13, 30, 100, usize::MAX][random.below(8)];
        let mut input = runtime.to_le_bytes().to_vec();
        input.push(kind);
        put_u64(&mut input, bound as u64);
        input.push(types.len() as u8);
        let mut keep = Vec::new();
        let mut fields_bits = Vec::new();
        for &t in &types {
            input.push(t);
            match t {
                b's' => {
                    let at = any_field(&mut random, b's', &mut keep);
                    put_str(&mut input, &keep[at]);
                    fields_bits.push((t, at as u64));
                }
                b'b' => {
                    let v = random.chance(2);
                    input.push(u8::from(v));
                    fields_bits.push((t, u64::from(v)));
                }
                _ => {
                    let v = (u64::from(random.next()) << 32) | u64::from(random.next());
                    put_u64(&mut input, v);
                    fields_bits.push((t, v));
                }
            }
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
        let rust = match Kind::from_u8(kind).and_then(|k| encode(runtime, k, bound, &fields)) {
            Some(bytes) => (1, bytes),
            None => (0, Vec::new()),
        };
        c.case(&framed(b'e', &input), rust, format!("encode {i} of kind {kind}"));
    }
    let tally = c.check(&oracle);
    eprintln!("encodes: {} refused, {} encoded alike", tally[0], tally[1]);
    assert!(tally[0] > 0 && tally[1] > 0);
}
