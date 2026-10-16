// User activation for privileged bridge calls (docs/protocol.md, "User
// activation"). Everything but `ready` needs a real user gesture: the
// main-thread host numbers gestures from trusted renderer events (the DOM
// event's isTrusted), hands the number to the worker with the event, and
// accepts a privileged bridge message only when it carries a gesture the host
// issued, has not consumed yet and issued recently. Nothing the worker says
// creates a gesture. Twin of the Swift/C++/Kotlin glue (WidgetHost.gesture(for:),
// GestureCounter in widget_host.h and in UserActivation.kt), which also counts
// only events the renderer marks `trusted`.

export const UNPRIVILEGED_BRIDGE_METHODS = new Set(['ready']);
const TYPES = new Set(['click', 'dblclick', 'mousedown', 'mouseup', 'pointerdown', 'pointerup', 'keydown']);
/** The keys that activate a focused control; Tab, arrows, letters and Escape are not a gesture. */
const ACTIVATION_KEYS = new Set(['Enter', ' ']);
/** A privileged call must reach the host this soon after its gesture was issued. */
export const GESTURE_TTL_MS = 5000;

type GestureEvent = { type?: unknown; key?: unknown; repeat?: unknown; modifiers?: unknown; detail?: unknown };

/** Clicks and presses of Enter or Space count; not a key repeat, not a Command shortcut (modifier bit 4). */
export function isUserActivation(ev: GestureEvent): boolean {
  const type = String(ev.type ?? '');
  if (!TYPES.has(type)) return false;
  if (type === 'keydown') return ACTIVATION_KEYS.has(String(ev.key)) && ev.repeat !== true && ((Number(ev.modifiers) | 0) & 4) === 0;
  return true;
}

/**
 * One physical press (pointerdown … click / dblclick) or one key press is one
 * gesture. The click an Enter or Space press activates (`detail` 0) belongs to
 * that press, and so does every click of the key held down.
 */
export class GestureCounter {
  private n = 0;
  private pressOpen = false;
  private lastType = '';

  /** The gesture number for this event, 0 when it is not a user activation. */
  gestureFor(ev: GestureEvent): number {
    const type = String(ev.type ?? '');
    if (type === 'keydown' && ev.repeat === true && ACTIVATION_KEYS.has(String(ev.key))) {
      this.pressOpen = false;
      this.lastType = type;
      return 0;
    }
    if (!isUserActivation(ev)) return 0;
    let continues = false;
    if (type === 'mousedown') continues = this.lastType === 'pointerdown';
    else if (type === 'pointerup' || type === 'mouseup') continues = this.pressOpen;
    else if (type === 'click') continues = this.pressOpen || (this.lastType === 'keydown' && !(Number(ev.detail) > 0));
    else if (type === 'dblclick') continues = this.lastType === 'click';
    if (!continues) this.n++;
    this.pressOpen = type !== 'click' && type !== 'dblclick' && type !== 'keydown';
    this.lastType = type;
    return this.n;
  }
}

/** Host side: gestures handed to the worker, and the last one a privileged call consumed. */
export class HostActivationGate {
  private issued = new Map<number, number>();
  private consumed = 0;

  issue(gesture: number, now: number) {
    this.issued.set(gesture, now);
    if (this.issued.size > 64) for (const k of [...this.issued.keys()].slice(0, this.issued.size - 64)) this.issued.delete(k);
  }

  /** True once per issued gesture, while the widget is visible and the gesture is recent. */
  accept(gesture: unknown, now: number, visible: boolean): boolean {
    const n = typeof gesture === 'number' && Number.isInteger(gesture) ? gesture : 0;
    const at = this.issued.get(n);
    if (!visible || n <= this.consumed || at === undefined || now - at > GESTURE_TTL_MS) return false;
    this.consumed = n;
    return true;
  }
}
