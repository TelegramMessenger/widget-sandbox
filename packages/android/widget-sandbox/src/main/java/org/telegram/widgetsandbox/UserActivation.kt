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
    /** Pointer presses only: widgets get no keyboard (docs/decisions.md, "Profile v1"), so no key is a gesture. */
    private val TYPES = setOf("click", "dblclick", "mousedown", "mouseup", "pointerdown", "pointerup")

    /** A pointer press or release, or a click with a click count (`detail` > 0; the click Enter or Space activates has 0). */
    fun isUserActivation(type: String, detail: Double = 0.0): Boolean {
        if (type !in TYPES) return false
        return if (type == "click" || type == "dblclick") detail > 0 else true
    }
}

/**
 * Host side: one physical pointer press (pointerdown … click / dblclick) is one gesture; keys never are.
 * A click without a click count counts only as the end of an open press. WidgetHost feeds it only
 * events the renderer marks `trusted`.
 */
class GestureCounter {
    private var number = 0
    private var pressOpen = false
    private var lastType = ""

    /** The gesture number for this renderer event, 0 when it is not a user activation. */
    fun gestureFor(type: String, detail: Double = 0.0): Int {
        val continues = when (type) {
            "mousedown" -> lastType == "pointerdown"
            "pointerup", "mouseup", "click" -> pressOpen
            "dblclick" -> lastType == "click"
            "pointerdown" -> false
            else -> return 0
        }
        if (!continues && !UserActivation.isUserActivation(type, detail)) {
            // A keyboard click ends any chain: a dblclick after it is not the pointer's.
            pressOpen = false
            lastType = ""
            return 0
        }
        if (!continues) number += 1
        pressOpen = type != "click" && type != "dblclick"
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
