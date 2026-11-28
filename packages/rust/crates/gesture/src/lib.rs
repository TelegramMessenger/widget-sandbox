//! widget-sandbox's gesture policy (`packages/desktop/sandbox/include/twidget/gesture_policy.h`,
//! `src/gesture_policy.cpp`; gesture-policy.ts, GesturePolicy.swift, GesturePolicy.kt; docs/sdk-v1.md §5.5):
//! who gets a press over a widget, the widget or the bubble. Taps and double taps over the widget are the
//! widget's, a vertical drag is always the chat's; for the rest the widget claims per element with CSS
//! `touch-action`: `auto` leaves the press to the bubble, `pan-y` gives the widget horizontal drags, `none`
//! horizontal drags and holds. The first move past the slop locks the axis for the whole press; the leading
//! `EDGE_STRIP_PX` of the widget never take a claim. Times are parameters (ms on one clock), positions CSS px
//! from the widget's leading edge and top; checked against `packages/core/tests/fixtures/gesture-policy.json`.

#![forbid(unsafe_code)]

/// What the widget takes of a press beyond its taps and double taps, from `touch-action` (and a learned
/// `preventDefault` on pointerdown).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WidgetGestureClaim {
    /// Horizontal drags (no swipe-to-reply or back).
    pub x: bool,
    /// Always false since v0.1.3: vertical drags are the chat's, and the arbiter ignores it.
    pub y: bool,
    /// Holds, and a right click with a mouse: no message menu; the widget gets `contextmenu`.
    pub hold: bool,
}

/// `Mouse` has no holds, no swipes and no edge strip; `Touch` and `Pen` have all of them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PointerKind {
    #[default]
    Touch,
    Pen,
    Mouse,
}

/// Who has the press: still undecided, the widget's, or the bubble's.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GestureOwner {
    #[default]
    Pending,
    Widget,
    Bubble,
}

/// What a press is so far.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Gesture {
    #[default]
    Press,
    Tap,
    DoubleTap,
    Hold,
    DragX,
    DragY,
    MenuClick,
}

/// What the bubble does with a press it takes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BubbleAction {
    Menu,
    Swipe,
    Scroll,
    Back,
}

/// The event the widget's renderer delivers after the press's own events when the widget wins a double tap
/// or a hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GestureEvent {
    DblClick,
    ContextMenu,
}

/// The arbiter's verdict on a press, after each of its steps.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GestureVerdict {
    pub owner: GestureOwner,
    pub gesture: Gesture,
    /// Once, in the verdict where the bubble takes a press the widget has had since its pointerdown: end it
    /// for the widget now (`pointercancel`, nothing more of this pointer).
    pub cancel: bool,
    /// Owner `Bubble`: what the bubble does with the press.
    pub action: Option<BubbleAction>,
    /// Once, when the widget wins a double tap or a hold: deliver this event after the press's own events.
    pub event: Option<GestureEvent>,
    /// The press started in the leading edge strip, where the widget's claim does not apply.
    pub edge: bool,
}

/// Movement (CSS px, from the pointerdown) below which a press is still a tap or a hold.
pub const TOUCH_SLOP_PX: f64 = 10.0;
/// A touch held this long within the slop is a hold.
pub const HOLD_MS: f64 = 500.0;
/// A second tap whose pointerdown comes at most this long after the first tap's pointerup is a double tap.
pub const DOUBLE_TAP_MS: f64 = 300.0;
/// ... and lands at most this far (CSS px) from the first tap's pointerdown.
pub const DOUBLE_TAP_SLOP_PX: f64 = 40.0;
/// Presses that start this close (CSS px) to the widget's leading edge take no claim.
pub const EDGE_STRIP_PX: f64 = 20.0;

pub const NO_CLAIM: WidgetGestureClaim = WidgetGestureClaim { x: false, y: false, hold: false };

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// A regex word character (`\w` without the `u` flag): every other byte, a UTF-8 one included, is a boundary.
fn is_word(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

fn trim_lower(s: &[u8]) -> Vec<u8> {
    let mut b = 0;
    let mut e = s.len();
    while b < e && is_space(s[b]) {
        b += 1;
    }
    while e > b && is_space(s[e - 1]) {
        e -= 1;
    }
    s[b..e].to_ascii_lowercase()
}

/// `/\bpan-(a|b|c)\b/.test(ta)` for the three `words`.
fn has_pan(ta: &[u8], words: &[&[u8]; 3]) -> bool {
    let mut from = 0;
    while let Some(i) = ta.get(from..).and_then(|rest| rest.windows(4).position(|w| w == b"pan-")).map(|n| n + from) {
        from = i + 1;
        if i > 0 && is_word(ta[i - 1]) {
            continue;
        }
        let rest = &ta[i + 4..];
        if words.iter().any(|w| rest.starts_with(w) && (rest.len() == w.len() || !is_word(rest[w.len()]))) {
            return true;
        }
    }
    false
}

/// The claim of a press from the computed `touch-action` of the target and of each ancestor up to the root,
/// target first, and whether the widget prevented pointerdown on the target before. `none` anywhere claims
/// horizontal drags and holds; a value without a horizontal pan claims `x`; `auto`, `manipulation` and a value
/// with a horizontal pan claim nothing. A prevented pointerdown claims `x`, never holds. `y` is always false.
/// Values are trimmed of ASCII whitespace and lowercased A–Z only.
pub fn claim_of<S: AsRef<[u8]>>(touch_actions: &[S], prevented: bool) -> WidgetGestureClaim {
    const HORIZONTAL: [&[u8]; 3] = [b"x", b"left", b"right"];
    let mut x = prevented;
    for raw in touch_actions {
        let ta = trim_lower(raw.as_ref());
        if ta.is_empty() || ta == b"auto" || ta == b"manipulation" {
            continue;
        }
        if ta == b"none" {
            return WidgetGestureClaim { x: true, y: false, hold: true };
        }
        if !has_pan(&ta, &HORIZONTAL) {
            x = true;
        }
    }
    WidgetGestureClaim { x, y: false, hold: false }
}

impl PointerKind {
    /// "touch" | "pen" | "mouse".
    pub fn name(self) -> &'static str {
        match self {
            PointerKind::Touch => "touch",
            PointerKind::Pen => "pen",
            PointerKind::Mouse => "mouse",
        }
    }
}

impl GestureOwner {
    /// "pending" | "widget" | "bubble".
    pub fn name(self) -> &'static str {
        match self {
            GestureOwner::Pending => "pending",
            GestureOwner::Widget => "widget",
            GestureOwner::Bubble => "bubble",
        }
    }
}

impl Gesture {
    /// "press" | "tap" | "double-tap" | "hold" | "drag-x" | "drag-y" | "menu-click".
    pub fn name(self) -> &'static str {
        match self {
            Gesture::Press => "press",
            Gesture::Tap => "tap",
            Gesture::DoubleTap => "double-tap",
            Gesture::Hold => "hold",
            Gesture::DragX => "drag-x",
            Gesture::DragY => "drag-y",
            Gesture::MenuClick => "menu-click",
        }
    }
}

impl BubbleAction {
    /// "menu" | "swipe" | "scroll" | "back".
    pub fn name(self) -> &'static str {
        match self {
            BubbleAction::Menu => "menu",
            BubbleAction::Swipe => "swipe",
            BubbleAction::Scroll => "scroll",
            BubbleAction::Back => "back",
        }
    }
}

impl GestureEvent {
    /// "dblclick" | "contextmenu".
    pub fn name(self) -> &'static str {
        match self {
            GestureEvent::DblClick => "dblclick",
            GestureEvent::ContextMenu => "contextmenu",
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct Press {
    t0: f64,
    x0: f64,
    y0: f64,
    claim: WidgetGestureClaim,
    pointer: PointerKind,
    edge: bool,
    second: bool,
    /// The first move past the slop has happened: the press's axis is decided.
    locked: bool,
    owner: GestureOwner,
    gesture: Gesture,
    action: Option<BubbleAction>,
}

#[derive(Clone, Copy, Debug)]
struct Tap {
    t: f64,
    x: f64,
    y: f64,
}

/// One pointer's presses over one widget, in order. Calls with a `t` past the hold deadline decide the hold
/// first, so a late `tick` changes nothing.
#[derive(Clone, Debug, Default)]
pub struct GestureArbiter {
    press: Option<Press>,
    last_tap: Option<Tap>,
}

fn verdict(p: &Press) -> GestureVerdict {
    GestureVerdict { owner: p.owner, gesture: p.gesture, action: p.action, edge: p.edge, ..GestureVerdict::default() }
}

fn decide(
    p: &mut Press,
    owner: GestureOwner,
    gesture: Gesture,
    action: Option<BubbleAction>,
    event: Option<GestureEvent>,
) -> GestureVerdict {
    p.owner = owner;
    p.gesture = gesture;
    p.action = action;
    let mut v = verdict(p);
    v.cancel = owner == GestureOwner::Bubble;
    v.event = event;
    v
}

fn hold_first(p: &mut Press, t: f64) -> Option<GestureVerdict> {
    if p.owner != GestureOwner::Pending || p.pointer == PointerKind::Mouse || t - p.t0 < HOLD_MS {
        return None;
    }
    Some(if p.claim.hold {
        decide(p, GestureOwner::Widget, Gesture::Hold, None, Some(GestureEvent::ContextMenu))
    } else {
        decide(p, GestureOwner::Bubble, Gesture::Hold, Some(BubbleAction::Menu), None)
    })
}

fn moved(p: &mut Press, t: f64, x: f64, y: f64) -> GestureVerdict {
    if let Some(held) = hold_first(p, t) {
        return held;
    }
    let dx = x - p.x0;
    let dy = y - p.y0;
    if p.locked || dx.hypot(dy) <= TOUCH_SLOP_PX {
        return verdict(p);
    }
    p.locked = true;
    let horizontal = dx.abs() > dy.abs();
    if p.owner == GestureOwner::Widget && p.gesture == Gesture::Hold {
        return if horizontal { verdict(p) } else { decide(p, GestureOwner::Bubble, Gesture::DragY, Some(BubbleAction::Scroll), None) };
    }
    if p.owner != GestureOwner::Pending {
        return verdict(p);
    }
    let gesture = if horizontal { Gesture::DragX } else { Gesture::DragY };
    if p.pointer == PointerKind::Mouse || (horizontal && p.claim.x) {
        return decide(p, GestureOwner::Widget, gesture, None, None);
    }
    let action = if !horizontal {
        BubbleAction::Scroll
    } else if p.edge {
        BubbleAction::Back
    } else {
        BubbleAction::Swipe
    };
    decide(p, GestureOwner::Bubble, gesture, Some(action), None)
}

impl GestureArbiter {
    pub fn new() -> GestureArbiter {
        GestureArbiter::default()
    }

    /// A primary pointerdown. `button` 2 with a mouse is the secondary click. A press still open is replaced.
    pub fn down(&mut self, t: f64, x: f64, y: f64, claim: WidgetGestureClaim, pointer: PointerKind, button: i32) -> GestureVerdict {
        let edge = pointer != PointerKind::Mouse && x < EDGE_STRIP_PX;
        let last = self.last_tap.take();
        let second = last.is_some_and(|last| t - last.t <= DOUBLE_TAP_MS && (x - last.x).hypot(y - last.y) <= DOUBLE_TAP_SLOP_PX);
        let p = self.press.insert(Press {
            t0: t,
            x0: x,
            y0: y,
            claim: if edge { NO_CLAIM } else { claim },
            pointer,
            edge,
            second,
            locked: false,
            owner: GestureOwner::Pending,
            gesture: Gesture::Press,
            action: None,
        });
        if pointer == PointerKind::Mouse && button == 2 {
            return if p.claim.hold {
                decide(p, GestureOwner::Widget, Gesture::MenuClick, None, Some(GestureEvent::ContextMenu))
            } else {
                decide(p, GestureOwner::Bubble, Gesture::MenuClick, Some(BubbleAction::Menu), None)
            };
        }
        verdict(p)
    }

    /// A move of the press's pointer (the C++ `move`). None when no press is open.
    pub fn move_to(&mut self, t: f64, x: f64, y: f64) -> Option<GestureVerdict> {
        let p = self.press.as_mut()?;
        Some(moved(p, t, x, y))
    }

    /// The hold timer: call it at `hold_deadline()`. None when no press is open.
    pub fn tick(&mut self, t: f64) -> Option<GestureVerdict> {
        let p = self.press.as_mut()?;
        if let Some(held) = hold_first(p, t) {
            return Some(held);
        }
        Some(verdict(p))
    }

    /// The pointerup. A press still undecided is a tap, or the second tap of a double tap. None when no press
    /// is open.
    pub fn up(&mut self, t: f64, x: f64, y: f64) -> Option<GestureVerdict> {
        let mut p = self.press.take()?;
        let v = moved(&mut p, t, x, y);
        if p.owner != GestureOwner::Pending {
            return Some(if v.cancel || v.event.is_some() { v } else { verdict(&p) });
        }
        if p.second {
            return Some(decide(&mut p, GestureOwner::Widget, Gesture::DoubleTap, None, Some(GestureEvent::DblClick)));
        }
        self.last_tap = Some(Tap { t, x: p.x0, y: p.y0 });
        Some(decide(&mut p, GestureOwner::Widget, Gesture::Tap, None, None))
    }

    /// The platform cancelled the press: it is over, and no double tap continues it.
    pub fn cancel(&mut self) {
        self.press = None;
        self.last_tap = None;
    }

    /// When `tick` may turn the open press into a hold, or None.
    pub fn hold_deadline(&self) -> Option<f64> {
        match &self.press {
            Some(p) if p.owner == GestureOwner::Pending && p.pointer != PointerKind::Mouse => Some(p.t0 + HOLD_MS),
            _ => None,
        }
    }
}
