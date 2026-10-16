package org.telegram.widgetsandbox

/**
 * Privileged bridge calls (everything but `ready`) need a real user gesture:
 * the host numbers gestures from renderer events, the engine lets one call
 * through per gesture, made in that gesture's own event task on a visible
 * widget. Pure policy, shared by WidgetHost and SandboxEngine (the Swift and
 * C++ copies: WidgetHost.gesture(for:) / SandboxEngine.activationEvent).
 */
object UserActivation {
    val UNPRIVILEGED_BRIDGE_METHODS = setOf("ready")
    private val TYPES = setOf("click", "dblclick", "mousedown", "mouseup", "pointerdown", "pointerup", "keydown")
    /** The keys that activate a focused control; Tab, arrows, letters and Escape are not a gesture. */
    val ACTIVATION_KEYS = setOf("Enter", " ")

    /** Clicks and presses of Enter or Space count; not a key repeat, not a Command shortcut (modifier bit 4). */
    fun isUserActivation(type: String, key: String?, repeat: Boolean, modifiers: Int): Boolean {
        if (type !in TYPES) return false
        if (type == "keydown") return key in ACTIVATION_KEYS && !repeat && modifiers and 4 == 0
        return true
    }
}

/**
 * Host side: one physical press (pointerdown … click / dblclick) or one key press is one gesture.
 * The click an Enter or Space press activates (`detail` 0) belongs to that press, and so does
 * every click of the key held down. WidgetHost feeds it only events the renderer marks `trusted`.
 */
class GestureCounter {
    private var number = 0
    private var pressOpen = false
    private var lastType = ""

    /** The gesture number for this renderer event, 0 when it is not a user activation. */
    fun gestureFor(type: String, key: String?, repeat: Boolean, modifiers: Int, detail: Double = 0.0): Int {
        if (type == "keydown" && repeat && key in UserActivation.ACTIVATION_KEYS) {
            pressOpen = false
            lastType = type
            return 0
        }
        if (!UserActivation.isUserActivation(type, key, repeat, modifiers)) return 0
        val continues = when (type) {
            "mousedown" -> lastType == "pointerdown"
            "pointerup", "mouseup" -> pressOpen
            "click" -> pressOpen || (lastType == "keydown" && !(detail > 0))
            "dblclick" -> lastType == "click"
            else -> false
        }
        if (!continues) number += 1
        pressOpen = type != "click" && type != "dblclick" && type != "keydown"
        lastType = type
        return number
    }
}

/** Engine side: the gesture whose event task runs, and the last one a privileged call consumed. */
class ActivationGate {
    private var active = 0
    private var consumed = 0

    fun begin(gesture: Int, visible: Boolean) {
        active = if (visible && gesture > consumed) gesture else 0
    }

    fun end() {
        active = 0
    }

    /** True once per gesture, only inside its task and while the widget is visible. */
    fun consume(visible: Boolean): Boolean {
        if (!visible || active <= consumed) return false
        consumed = active
        active = 0
        return true
    }
}
