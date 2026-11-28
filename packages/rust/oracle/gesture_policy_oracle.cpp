// widget-sandbox's gesture policy on stdin, for rust/crates/gesture's tests to compare with the Rust port.
//
// Records (oracle_io.h): 'C' u32 count, that many touch-action strings, u8 prevented: claimOf -> u8 x, y, hold. 'N' makes a
// new GestureArbiter (no answer); on it 'd' f64 t, x, y, u8 claim bits (x, y, hold from bit 0), u8 pointer (touch, pen,
// mouse), i32 button: down; 'm' f64 t, x, y: move; 't' f64 t: tick; 'u' f64 t, x, y: up; 'c' cancel. Each answers the
// verdict (u8 present, then u8 owner, gesture, cancel, u8 has and u8 action, u8 has and u8 event, u8 edge; down's is always
// present, cancel has none) and holdDeadline (u8 has, f64). 'n' u8 enum (pointer, owner, gesture, action, event), u8 value:
// its name. 'y' f64 x, y: std::hypot as f64, the C library's that the port's f64::hypot calls too.
#include "oracle_io.h"
#include "twidget/gesture_policy.h"

#include <cmath>
#include <memory>

using namespace twidget;

namespace {

std::unique_ptr<GestureArbiter> arbiter;

void verdict(oracle::Out& out, const std::optional<GestureVerdict>& v) {
    out.flag(v.has_value());
    if (!v) return;
    out.u8(uint8_t(v->owner)).u8(uint8_t(v->gesture)).flag(v->cancel);
    out.flag(v->action.has_value()).u8(v->action ? uint8_t(*v->action) : 0);
    out.flag(v->event.has_value()).u8(v->event ? uint8_t(*v->event) : 0);
    out.flag(v->edge);
}

bool run(int op, oracle::In& in) {
    oracle::Out out;
    switch (op) {
    case 'C': {
        std::vector<std::string> touchActions(in.u32());
        for (auto& ta : touchActions) ta = in.str();
        const WidgetGestureClaim c = gesture::claimOf(touchActions, in.flag());
        out.flag(c.x).flag(c.y).flag(c.hold);
        oracle::answer(1, out.s);
        return true;
    }
    case 'N': arbiter = std::make_unique<GestureArbiter>(); return true;
    case 'd': {
        const double t = in.f64(), x = in.f64(), y = in.f64();
        const uint8_t bits = in.u8();
        const PointerKind pointer = PointerKind(in.u8() % 3);
        const int32_t button = in.i32();
        verdict(out, arbiter->down(t, x, y, WidgetGestureClaim{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0}, pointer, button));
        break;
    }
    case 'm': {
        const double t = in.f64(), x = in.f64(), y = in.f64();
        verdict(out, arbiter->move(t, x, y));
        break;
    }
    case 't': verdict(out, arbiter->tick(in.f64())); break;
    case 'u': {
        const double t = in.f64(), x = in.f64(), y = in.f64();
        verdict(out, arbiter->up(t, x, y));
        break;
    }
    case 'c':
        arbiter->cancel();
        verdict(out, std::nullopt);
        break;
    case 'n': {
        const uint8_t which = in.u8(), value = in.u8();
        std::string_view name;
        switch (which % 5) {
        case 0: name = gesture::name(PointerKind(value % 3)); break;
        case 1: name = gesture::name(GestureOwner(value % 3)); break;
        case 2: name = gesture::name(Gesture(value % 7)); break;
        case 3: name = gesture::name(BubbleAction(value % 4)); break;
        default: name = gesture::name(GestureEvent(value % 2)); break;
        }
        oracle::answer(1, name);
        return true;
    }
    case 'y': {
        const double x = in.f64(), y = in.f64();
        oracle::answer(1, out.f64(std::hypot(x, y)).s);
        return true;
    }
    default: return false;
    }
    const std::optional<double> deadline = arbiter->holdDeadline();
    out.flag(deadline.has_value()).f64(deadline.value_or(0));
    oracle::answer(1, out.s);
    return true;
}

} // namespace

int main() {
    return oracle::serve(run);
}
