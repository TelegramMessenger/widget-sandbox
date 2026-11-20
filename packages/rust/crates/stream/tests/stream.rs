//! test_remote_stream.cpp's checks of the wire itself, ported: the documented table and the documented
//! frame layout, read back whole from a run of frames and from pieces; and the reader's refusals.

use std::sync::Mutex;

use twidget_stream::*;

/// A stream that hands out its bytes `chunk` at a time.
#[derive(Debug)]
pub struct Chunks {
    bytes: Vec<u8>,
    chunk: usize,
    at: Mutex<usize>,
}

impl Chunks {
    pub fn new(bytes: Vec<u8>, chunk: usize) -> Chunks {
        Chunks { bytes, chunk: chunk.max(1), at: Mutex::new(0) }
    }
}

impl Stream for Chunks {
    fn read(&self, out: &mut [u8]) -> usize {
        let mut at = self.at.lock().unwrap();
        let n = out.len().min(self.chunk).min(self.bytes.len() - *at);
        out[..n].copy_from_slice(&self.bytes[*at..*at + n]);
        *at += n;
        n
    }
    fn write(&self, _: &[u8]) -> bool {
        false
    }
    fn shutdown(&self) {}
}

#[test]
fn the_documented_table() {
    let kinds = [
        (Kind::Opening, 1),
        (Kind::Open, 2),
        (Kind::Start, 3),
        (Kind::Event, 4),
        (Kind::Activation, 5),
        (Kind::AnimationFrame, 6),
        (Kind::Viewport, 7),
        (Kind::Resize, 8),
        (Kind::Theme, 9),
        (Kind::SafeArea, 10),
        (Kind::Data, 11),
        (Kind::Visible, 12),
        (Kind::PressBegan, 13),
        (Kind::PressEnded, 14),
        (Kind::ChargeValidator, 15),
        (Kind::Stop, 16),
        (Kind::Answer, 17),
        (Kind::Probe, 18),
        (Kind::QueryAnswer, 19),
        (Kind::IntlAnswer, 20),
        (Kind::Close, 21),
        (Kind::Ready, 64),
        (Kind::Refused, 65),
        (Kind::TaskStarted, 66),
        (Kind::TaskCompleted, 67),
        (Kind::Ops, 68),
        (Kind::Query, 69),
        (Kind::Intl, 70),
        (Kind::Listen, 71),
        (Kind::PreventDefault, 72),
        (Kind::Anim, 73),
        (Kind::RequestsFrame, 74),
        (Kind::Log, 75),
        (Kind::Diag, 76),
        (Kind::Bridge, 77),
        (Kind::CopyText, 78),
        (Kind::Request, 79),
        (Kind::Wallet, 80),
        (Kind::Storage, 81),
        (Kind::ResizeAnchor, 82),
        (Kind::WatchesPublicData, 83),
        (Kind::Loaded, 84),
        (Kind::Stats, 85),
        (Kind::Died, 86),
        (Kind::ProbeAnswer, 87),
    ];
    for (kind, number) in kinds {
        assert_eq!(kind as u8, number);
        assert_eq!(Kind::from_u8(number), Some(kind));
    }
    assert_eq!(kinds.iter().filter(|(_, n)| *n < 64).count(), 21);
    assert_eq!(VERSION, 1);
    assert_eq!(MAX_FRAME_BYTES, (32 << 20) + (64 << 10));
    assert_eq!(MAX_OPENING_BYTES, 16 << 20);
    assert_eq!(MAX_STAMP_BYTES, 4096);
    let known = (0..=255u8).filter(|k| shape(*k).is_some()).count();
    assert_eq!(known, 56, "45 kinds of an engine stream and 11 of a hosting one");
    for k in 0..=255u8 {
        let Some(s) = shape(k) else { continue };
        assert_eq!(s.from == Side::Engine, k >= 64, "kind {k}'s side");
        assert!(s.fields.bytes().filter(|c| *c == b's').count() <= FRAME_STRINGS);
        assert!(s.fields.bytes().filter(|c| *c == b'i').count() <= 1 && s.fields.bytes().filter(|c| *c == b'b').count() <= 1);
    }
}

fn frame(runtime: u32, kind: Kind, fields: &[Field]) -> Vec<u8> {
    encode(runtime, kind, MAX_FRAME_BYTES, fields).expect("the frame encodes")
}

#[test]
fn frame_bytes_follow_the_documented_layout() {
    assert_eq!(
        frame(7, Kind::Activation, &[Field::Str(b"{}"), Field::I64(-2)]),
        b"\x13\0\0\0\x07\0\0\0\x05\x02\0\0\0{}\xFE\xFF\xFF\xFF\xFF\xFF\xFF\xFF".to_vec()
    );
    assert_eq!(frame(1, Kind::AnimationFrame, &[Field::F64(1.5)]), b"\x0D\0\0\0\x01\0\0\0\x06\0\0\0\0\0\0\xF8\x3F".to_vec());
    assert_eq!(frame(0x01020304, Kind::Visible, &[Field::Bool(true)]), b"\x06\0\0\0\x04\x03\x02\x01\x0C\x01".to_vec());
    assert_eq!(frame(2, Kind::Close, &[]), b"\x05\0\0\0\x02\0\0\0\x15".to_vec());

    let ready = frame(0, Kind::Ready, &[Field::I64(1)]);
    let log = frame(9, Kind::Log, &[Field::Str(b"warn"), Field::Str(b"a\0b")]);
    let viewport = frame(9, Kind::Viewport, &[Field::F64(-0.0), Field::F64(1e300), Field::F64(2.5)]);
    for chunk in [1, 3, 5, 64, 1 << 20] {
        let stream = Chunks::new([ready.clone(), log.clone()].concat(), chunk);
        let mut reader = FrameReader::new(&stream, Side::Engine);
        let f = reader.next(MAX_FRAME_BYTES).unwrap().unwrap();
        assert!(f.runtime == 0 && f.kind == Kind::Ready && f.i64 == 1);
        let f = reader.next(MAX_FRAME_BYTES).unwrap().unwrap();
        assert!(f.runtime == 9 && f.kind == Kind::Log && f.str[0] == b"warn" && f.str[1] == b"a\0b");
        assert!(reader.next(MAX_FRAME_BYTES).unwrap().is_none(), "the end on a frame's boundary");
        let stream = Chunks::new([viewport.clone(), viewport.clone()].concat(), chunk);
        let mut reader = FrameReader::new(&stream, Side::Host);
        for _ in 0..2 {
            let f = reader.next(MAX_FRAME_BYTES).unwrap().unwrap();
            assert!(f.f64[0] == 0.0 && f.f64[0].is_sign_negative() && f.f64[1] == 1e300 && f.f64[2] == 2.5);
        }
        assert!(reader.next(MAX_FRAME_BYTES).unwrap().is_none());
    }
    let stream = Chunks::new([ready.clone(), log[..6].to_vec()].concat(), 7);
    let mut reader = FrameReader::new(&stream, Side::Engine);
    assert!(reader.next(MAX_FRAME_BYTES).unwrap().is_some());
    assert_eq!(reader.next(MAX_FRAME_BYTES).unwrap_err(), "the stream ended inside a frame");
    let stream = Chunks::new(log[..2].to_vec(), 7);
    assert_eq!(FrameReader::new(&stream, Side::Engine).next(MAX_FRAME_BYTES).unwrap_err(), "the stream ended inside a frame's size");
}

fn first_error(bytes: Vec<u8>, from: Side, bound: usize) -> String {
    let stream = Chunks::new(bytes, 4096);
    let mut reader = FrameReader::new(&stream, from);
    loop {
        match reader.next(bound) {
            Ok(Some(_)) => continue,
            Ok(None) => return String::new(),
            Err(e) => return e,
        }
    }
}

#[test]
fn hostile_frames_are_refused_with_why() {
    let log = frame(1, Kind::Log, &[Field::Str(b"a"), Field::Str(b"b")]);
    assert_eq!(first_error(log.clone(), Side::Host, MAX_FRAME_BYTES), "kind 75 from the other side");
    assert_eq!(
        first_error(log.clone(), Side::Engine, log.len() - 5),
        format!("a frame of {} bytes, past the bound of {}", log.len() - 4, log.len() - 5)
    );
    let mut extra = log.clone();
    extra[0] += 1;
    extra.push(b'x');
    assert_eq!(first_error(extra, Side::Engine, MAX_FRAME_BYTES), "kind 75 with bytes after its fields");
    let mut short = log.clone();
    short[0] -= 1;
    short.pop();
    assert_eq!(first_error(short, Side::Engine, MAX_FRAME_BYTES), "kind 75 with a field past the frame's end");
    assert_eq!(
        first_error(b"\x04\0\0\0\0\0\0\0".to_vec(), Side::Engine, MAX_FRAME_BYTES),
        "a frame of 4 bytes, less than its runtime and kind"
    );
    assert_eq!(first_error(b"\x05\0\0\0\x01\0\0\0\xff".to_vec(), Side::Engine, MAX_FRAME_BYTES), "an unknown kind 255");
    assert_eq!(
        first_error(frame(3, Kind::Ready, &[Field::I64(1)])[..].to_vec(), Side::Engine, MAX_FRAME_BYTES),
        "kind 64 on runtime 3, not the connection"
    );
    let open_on_connection = b"\x05\0\0\0\0\0\0\0\x02".to_vec();
    assert_eq!(first_error(open_on_connection, Side::Host, MAX_FRAME_BYTES), "kind 2 on the connection, not a runtime");
    let mut bool2 = frame(1, Kind::Visible, &[Field::Bool(true)]);
    *bool2.last_mut().unwrap() = 2;
    assert_eq!(first_error(bool2, Side::Host, MAX_FRAME_BYTES), "kind 12 with a bool of 2");
    let gesture = frame(1, Kind::Activation, &[Field::Str(b""), Field::I64(i64::from(i32::MAX) + 1)]);
    assert_eq!(first_error(gesture, Side::Host, MAX_FRAME_BYTES), "kind 5 with a gesture outside int");
    let probe = frame(1, Kind::Probe, &[Field::I64(0), Field::Str(b"1")]);
    assert_eq!(first_error(probe, Side::Host, MAX_FRAME_BYTES), "kind 18 with a sequence below 1");
    let answer = frame(1, Kind::QueryAnswer, &[Field::Str(b"{}"), Field::Bool(false), Field::Str(b"x")]);
    assert_eq!(first_error(answer, Side::Host, MAX_FRAME_BYTES), "kind 19 with binary bytes but no binary");
    assert!(encode(1, Kind::Log, MAX_FRAME_BYTES, &[Field::Str(b"a")]).is_none(), "fields of another count");
    assert!(encode(1, Kind::Log, MAX_FRAME_BYTES, &[Field::I64(1), Field::Str(b"a")]).is_none(), "fields of another type");
    assert!(encode(1, Kind::Log, 4, &[Field::Str(b""), Field::Str(b"")]).is_none(), "a bound below the head");
    assert!(
        encode(1, Kind::Log, 13, &[Field::Str(b""), Field::Str(b"")]).is_some()
            && encode(1, Kind::Log, 12, &[Field::Str(b""), Field::Str(b"")]).is_none()
    );
    assert_eq!(cut(b"abcdef", 3), b"abcd");
    assert_eq!(cut(b"ab", 3), b"ab");
}
