//! The desktop core's remote engine over one byte stream (`twidget/remote_stream.h`, `src/remote_wire.h`): the
//! stream a host supplies, every frame kind's shape, the codec that sizes a frame against its bound before a
//! byte of it is written, the exact frame reader both ends use, and the cut and the naming both ends give a
//! frame. Frames: a `u32` size of the bytes after it (at least 5), a `u32` runtime id (0 is the connection),
//! a `u8` kind, then the kind's fields in order; a `str` is a `u32` length and raw bytes, an `i64` 8 bytes of
//! two's complement, an `f64` its IEEE-754 bits, a `bool` one byte of 0 or 1; all little-endian.

#![forbid(unsafe_code)]

/// One duplex byte stream to the other process, the OS boundary the host supplies: one reader and one
/// writer at a time, each of which may block until `shutdown`.
pub trait Stream: Send + Sync {
    /// Blocks until at least one byte came, then copies up to `out.len()` bytes and returns how many; 0 at
    /// the end of the stream, on an error and after `shutdown`.
    fn read(&self, out: &mut [u8]) -> usize;
    /// Blocks until all of `bytes` is written: true, or false once the stream broke or was shut down.
    fn write(&self, bytes: &[u8]) -> bool;
    /// From any thread, also while a `read` or `write` blocks, idempotent and without waiting.
    fn shutdown(&self);
}

/// The stream's version, in `Opening` and `Ready`.
pub const VERSION: i64 = 1;
/// `SandboxEngine::kRemoteMessageLimit`: a message of the string protocol.
pub const REMOTE_MESSAGE_LIMIT: usize = 1 << 20;
/// `SandboxEngine::kRemoteOpsLimit`: an ops batch.
pub const REMOTE_OPS_LIMIT: usize = 32 << 20;
/// `SandboxEngine::kRemotePlatformLimit`: a hosted renderer's platform message.
pub const REMOTE_PLATFORM_LIMIT: usize = REMOTE_OPS_LIMIT;
/// `SandboxEngine::kRemoteTextLimit` and `kRemoteNameLimit`.
pub const REMOTE_TEXT_LIMIT: usize = 4000;
pub const REMOTE_NAME_LIMIT: usize = 64;
/// The size any frame but an opening may give, in both directions: an ops batch with room for its frame.
pub const MAX_FRAME_BYTES: usize = REMOTE_OPS_LIMIT + (64 << 10);
/// The size `Opening` and `HostingOpening` may give.
pub const MAX_OPENING_BYTES: usize = 16 << 20;
/// Bytes of a stamp line in an opening: a longer one is refused, never cut.
pub const MAX_STAMP_BYTES: usize = 4096;
/// `SandboxRemoteCoding::kStatsFields`.
pub const STATS_FIELDS: usize = 11;

macro_rules! kinds {
    ($($name:ident = $value:literal),* $(,)?) => {
        /// Host to engine from 1, engine to host from 64, a hosting stream's own kinds from 32 and 96.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        #[repr(u8)]
        pub enum Kind {
            $($name = $value,)*
        }

        impl Kind {
            pub fn from_u8(value: u8) -> Option<Kind> {
                match value {
                    $($value => Some(Kind::$name),)*
                    _ => None,
                }
            }
        }
    };
}

kinds! {
    Opening = 1, Open = 2, Start = 3, Event = 4, Activation = 5, AnimationFrame = 6, Viewport = 7, Resize = 8, Theme = 9,
    SafeArea = 10, Data = 11, Visible = 12, PressBegan = 13, PressEnded = 14, ChargeValidator = 15, Stop = 16, Answer = 17,
    Probe = 18, QueryAnswer = 19, IntlAnswer = 20, Close = 21,
    HostingOpening = 32, HostedStart = 33, HostedTheme = 34, Platform = 35, PlatformAnswer = 36,
    Ready = 64, Refused = 65, TaskStarted = 66, TaskCompleted = 67, Ops = 68, Query = 69, Intl = 70, Listen = 71,
    PreventDefault = 72, Anim = 73, RequestsFrame = 74, Log = 75, Diag = 76, Bridge = 77, CopyText = 78, Request = 79,
    Wallet = 80, Storage = 81, ResizeAnchor = 82, WatchesPublicData = 83, Loaded = 84, Stats = 85, Died = 86, ProbeAnswer = 87,
    PlatformMessage = 96, PlatformRequest = 97, ValidatedOps = 98, RendererReady = 99, Rendered = 100, Nodes = 101,
}

/// The side that sends a kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Side {
    Host,
    Engine,
}

/// A kind's row of the table: who sends it, whether on the connection (runtime 0) or on a runtime, and its
/// fields in order, a letter each: `s` str, `i` i64, `f` f64, `b` bool.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Shape {
    pub from: Side,
    pub connection: bool,
    pub fields: &'static str,
}

/// The shape of `kind`, None for no kind.
pub fn shape(kind: u8) -> Option<Shape> {
    use Kind::*;
    use Side::{Engine as E, Host as H};
    let (from, connection, fields) = match Kind::from_u8(kind)? {
        Opening => (H, true, "issss"),
        Open => (H, false, ""),
        Start => (H, false, "ss"),
        Event => (H, false, "s"),
        Activation => (H, false, "si"),
        AnimationFrame => (H, false, "f"),
        Viewport => (H, false, "fff"),
        Resize => (H, false, "s"),
        Theme => (H, false, "ss"),
        SafeArea => (H, false, "s"),
        Data => (H, false, "s"),
        Visible => (H, false, "b"),
        PressBegan => (H, false, ""),
        PressEnded => (H, false, ""),
        ChargeValidator => (H, false, "f"),
        Stop => (H, false, "s"),
        Answer => (H, false, "is"),
        Probe => (H, false, "is"),
        QueryAnswer => (H, false, "sbs"),
        IntlAnswer => (H, false, "s"),
        Close => (H, false, ""),
        HostingOpening => (H, true, "issss"),
        HostedStart => (H, false, "sss"),
        HostedTheme => (H, false, "s"),
        Platform => (H, false, "s"),
        PlatformAnswer => (H, false, "s"),
        Ready => (E, true, "i"),
        Refused => (E, true, "s"),
        TaskStarted => (E, false, ""),
        TaskCompleted => (E, false, ""),
        Ops => (E, false, "s"),
        Query => (E, false, "s"),
        Intl => (E, false, "s"),
        Listen => (E, false, "s"),
        PreventDefault => (E, false, "s"),
        Anim => (E, false, "s"),
        RequestsFrame => (E, false, ""),
        Log => (E, false, "ss"),
        Diag => (E, false, "ss"),
        Bridge => (E, false, "ss"),
        CopyText => (E, false, "s"),
        Request => (E, false, "isb"),
        Wallet => (E, false, "is"),
        Storage => (E, false, "s"),
        ResizeAnchor => (E, false, "s"),
        WatchesPublicData => (E, false, "b"),
        Loaded => (E, false, ""),
        Stats => (E, false, "fffffffffff"),
        Died => (E, false, "s"),
        ProbeAnswer => (E, false, "is"),
        PlatformMessage => (E, false, "s"),
        PlatformRequest => (E, false, "s"),
        ValidatedOps => (E, false, "s"),
        RendererReady => (E, false, ""),
        Rendered => (E, false, ""),
        Nodes => (E, false, "i"),
    };
    Some(Shape { from, connection, fields })
}

/// The size in front of a frame.
pub const SIZE_BYTES: usize = 4;
/// The runtime id and the kind, the least a frame's size counts.
pub const HEAD_BYTES: usize = 5;
/// The strings a frame holds at most.
pub const FRAME_STRINGS: usize = 6;

/// One field of a frame to encode, typed as the wire types it.
#[derive(Clone, Copy, Debug)]
pub enum Field<'a> {
    Str(&'a [u8]),
    I64(i64),
    F64(f64),
    Bool(bool),
}

impl Field<'_> {
    fn letter(&self) -> u8 {
        match self {
            Field::Str(_) => b's',
            Field::I64(_) => b'i',
            Field::F64(_) => b'f',
            Field::Bool(_) => b'b',
        }
    }
}

impl<'a> From<&'a [u8]> for Field<'a> {
    fn from(s: &'a [u8]) -> Field<'a> {
        Field::Str(s)
    }
}

impl<'a> From<&'a str> for Field<'a> {
    fn from(s: &'a str) -> Field<'a> {
        Field::Str(s.as_bytes())
    }
}

impl<'a> From<&'a Vec<u8>> for Field<'a> {
    fn from(s: &'a Vec<u8>) -> Field<'a> {
        Field::Str(s)
    }
}

impl From<i64> for Field<'_> {
    fn from(v: i64) -> Self {
        Field::I64(v)
    }
}

impl From<f64> for Field<'_> {
    fn from(v: f64) -> Self {
        Field::F64(v)
    }
}

impl From<bool> for Field<'_> {
    fn from(v: bool) -> Self {
        Field::Bool(v)
    }
}

/// A whole frame of `kind` on `runtime`, size first, or None when its size would pass `bound` or `fields`
/// are not the kind's.
pub fn encode(runtime: u32, kind: Kind, bound: usize, fields: &[Field]) -> Option<Vec<u8>> {
    let s = shape(kind as u8)?;
    if fields.len() != s.fields.len() {
        return None;
    }
    let bound = bound.min(u32::MAX as usize);
    if bound < HEAD_BYTES {
        return None;
    }
    // Every addition is checked against what is left of the bound, so nothing wraps.
    let mut size = HEAD_BYTES;
    let mut add = |bytes: usize| {
        if bytes > bound - size {
            return false;
        }
        size += bytes;
        true
    };
    for (f, &letter) in fields.iter().zip(s.fields.as_bytes()) {
        if f.letter() != letter {
            return None;
        }
        let fits = match f {
            Field::Str(s) => add(4) && add(s.len()),
            Field::Bool(_) => add(1),
            _ => add(8),
        };
        if !fits {
            return None;
        }
    }
    let mut out = Vec::with_capacity(SIZE_BYTES + size);
    out.extend_from_slice(&(size as u32).to_le_bytes());
    out.extend_from_slice(&runtime.to_le_bytes());
    out.push(kind as u8);
    for f in fields {
        match f {
            Field::Str(s) => {
                out.extend_from_slice(&(s.len() as u32).to_le_bytes());
                out.extend_from_slice(s);
            }
            Field::Bool(b) => out.push(u8::from(*b)),
            Field::I64(v) => out.extend_from_slice(&v.to_le_bytes()),
            Field::F64(v) => out.extend_from_slice(&v.to_bits().to_le_bytes()),
        }
    }
    Some(out)
}

/// One frame's fields, checked against its kind's shape: the strings in order, then the numbers; no kind
/// has more than one `i64` or `bool`. Its strings view the reader's buffer.
#[derive(Clone, Copy, Debug)]
pub struct Frame<'a> {
    pub runtime: u32,
    pub kind: Kind,
    pub str: [&'a [u8]; FRAME_STRINGS],
    pub f64: [f64; STATS_FIELDS],
    pub i64: i64,
    pub flag: bool,
}

/// A frame's body read field by field, each against what is left of it.
struct FieldReader<'a> {
    rest: &'a [u8],
}

impl<'a> FieldReader<'a> {
    fn u8(&mut self) -> Option<u8> {
        let (first, rest) = self.rest.split_first()?;
        self.rest = rest;
        Some(*first)
    }

    fn u32(&mut self) -> Option<u32> {
        let (head, rest) = self.rest.split_first_chunk::<4>()?;
        self.rest = rest;
        Some(u32::from_le_bytes(*head))
    }

    fn u64(&mut self) -> Option<u64> {
        let (head, rest) = self.rest.split_first_chunk::<8>()?;
        self.rest = rest;
        Some(u64::from_le_bytes(*head))
    }

    /// A length, then that many bytes, viewed where they are.
    fn str(&mut self) -> Option<&'a [u8]> {
        let length = self.u32()? as usize;
        if length > self.rest.len() {
            return None;
        }
        let (s, rest) = self.rest.split_at(length);
        self.rest = rest;
        Some(s)
    }
}

/// What one read may bring past the frame being read, and the buffer kept between frames.
const READ_CHUNK: usize = 64 << 10;

/// The frames one side of a stream sends, read exactly: the size first, checked against its bound before
/// the body is read or room made for it, then every field against its kind's shape. After an error or the
/// end the stream is done.
pub struct FrameReader<'s> {
    stream: &'s dyn Stream,
    from: Side,
    buffer: Vec<u8>,
    begin: usize,
    end: usize,
}

impl std::fmt::Debug for FrameReader<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FrameReader").field("from", &self.from).field("begin", &self.begin).field("end", &self.end).finish()
    }
}

impl<'s> FrameReader<'s> {
    pub fn new(stream: &'s dyn Stream, from: Side) -> FrameReader<'s> {
        FrameReader { stream, from, buffer: Vec::new(), begin: 0, end: 0 }
    }

    /// The next frame; `Ok(None)` at the end of the stream on a frame's boundary, `Err` with why it cannot
    /// go on.
    pub fn next(&mut self, bound: usize) -> Result<Option<Frame<'_>>, String> {
        if !self.fill(SIZE_BYTES) {
            if self.end != self.begin {
                return Err("the stream ended inside a frame's size".into());
            }
            return Ok(None);
        }
        let size = u32::from_le_bytes(self.buffer[self.begin..self.begin + 4].try_into().expect("four bytes")) as usize;
        if size < HEAD_BYTES {
            return Err(format!("a frame of {size} bytes, less than its runtime and kind"));
        }
        if size > bound {
            return Err(format!("a frame of {size} bytes, past the bound of {bound}"));
        }
        if !self.fill(SIZE_BYTES + size) {
            return Err("the stream ended inside a frame".into());
        }
        let start = self.begin + SIZE_BYTES;
        self.begin += SIZE_BYTES + size;
        decode(&self.buffer[start..start + size], self.from).map(Some)
    }

    /// Until `bytes` unread bytes are here, false when the stream ends first; room is made for `bytes` only,
    /// never for what a size has not yet been checked for.
    fn fill(&mut self, bytes: usize) -> bool {
        if self.end - self.begin >= bytes {
            return true;
        }
        let rest = self.end - self.begin;
        if self.begin != 0 && rest != 0 {
            self.buffer.copy_within(self.begin..self.end, 0);
        }
        self.begin = 0;
        self.end = rest;
        // The room a large frame took is given back before the next one.
        if self.buffer.len() > READ_CHUNK && rest.max(bytes) <= READ_CHUNK {
            self.buffer.truncate(READ_CHUNK);
            self.buffer.shrink_to_fit();
        }
        if self.buffer.len() < bytes {
            self.buffer.resize(bytes.max(READ_CHUNK), 0);
        }
        while self.end < bytes {
            let room = self.buffer.len() - self.end;
            let got = self.stream.read(&mut self.buffer[self.end..]).min(room);
            if got == 0 {
                return false;
            }
            self.end += got;
        }
        true
    }
}

fn decode(body: &[u8], from: Side) -> Result<Frame<'_>, String> {
    let mut input = FieldReader { rest: body };
    let runtime = input.u32().unwrap_or(0);
    let kind = input.u8().unwrap_or(0);
    let bad = |what: String| format!("kind {kind} {what}");
    let Some(s) = shape(kind) else {
        return Err(format!("an unknown kind {kind}"));
    };
    if s.from != from {
        return Err(bad("from the other side".into()));
    }
    if s.connection && runtime != 0 {
        return Err(bad(format!("on runtime {runtime}, not the connection")));
    }
    if !s.connection && runtime == 0 {
        return Err(bad("on the connection, not a runtime".into()));
    }
    let mut frame = Frame {
        runtime,
        kind: Kind::from_u8(kind).expect("a known kind"),
        str: [&[]; FRAME_STRINGS],
        f64: [0.0; STATS_FIELDS],
        i64: 0,
        flag: false,
    };
    let mut strs = 0;
    let mut numbers = 0;
    for letter in s.fields.bytes() {
        let read = match letter {
            b's' => input.str().map(|v| {
                frame.str[strs] = v;
                strs += 1;
            }),
            b'i' => input.u64().map(|v| frame.i64 = v as i64),
            b'f' => input.u64().map(|v| {
                frame.f64[numbers] = f64::from_bits(v);
                numbers += 1;
            }),
            _ => match input.u8() {
                Some(byte) if byte > 1 => return Err(bad(format!("with a bool of {byte}"))),
                Some(byte) => {
                    frame.flag = byte == 1;
                    Some(())
                }
                None => None,
            },
        };
        if read.is_none() {
            return Err(bad("with a field past the frame's end".into()));
        }
    }
    if !input.rest.is_empty() {
        return Err(bad("with bytes after its fields".into()));
    }
    match frame.kind {
        Kind::Activation if frame.i64 < i64::from(i32::MIN) || frame.i64 > i64::from(i32::MAX) => {
            return Err(bad("with a gesture outside int".into()));
        }
        Kind::Probe | Kind::ProbeAnswer if frame.i64 < 1 => return Err(bad("with a sequence below 1".into())),
        Kind::QueryAnswer if !frame.flag && !frame.str[1].is_empty() => return Err(bad("with binary bytes but no binary".into())),
        _ => {}
    }
    Ok(frame)
}

/// An engine string cut to its limit and one byte more: still past the limit, so the client's own check
/// gives its verdict, and every frame fits `MAX_FRAME_BYTES`.
pub fn cut(s: &[u8], limit: usize) -> &[u8] {
    &s[..s.len().min(limit.saturating_add(1))]
}

/// How both ends name a frame in a failure.
pub fn kind_on(f: &Frame) -> String {
    format!("kind {} on runtime {}", f.kind as u8, f.runtime)
}
