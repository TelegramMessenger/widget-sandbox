// widget-sandbox's sdk_policy on stdin, for rust/crates/sdk_policy's tests to compare with the Rust port.
//
// Records (oracle_io.h). JSON inputs are texts parsed with Surrogates::Keep, as the engine parses bridge calls and
// commits; a text that does not parse answers status 3. An optional string is a u8 flag and a string; a map a u32
// count and that many key, value strings; a wallet call a u8 method, data, the text of a transaction and of a payload
// (each the checked one, or the default when it is refused). Answers are status 1 and the fields listed unless said.
//   's' a string: utf8Length, utf16Length as u64, wellFormed, then the optional results of checkRequestData,
//       checkCopyText, checkWidgetDataValue, checkChatType, checkLocale, checkResizeAnchor, checkLink as openLink and as
//       openTelegramLink, then isPrivilegedMethod, allowedInPreview, viewerFieldRule.
//   'L' method, url: checkLink -> status 0 or 1 and the URL.
//   'j' a JSON text: checkLinkValue (openLink, openTelegramLink), checkHaptic (flag, haptic, optional value),
//       checkResizeAnchorValue, checkRequestOptions (flag, value), checkWalletAddress, checkWalletDestination,
//       checkWalletTransaction's toJson, checkSignDataPayload's toJson, checkRequestDataValue, checkCopyTextValue,
//       themeParamsJson of checkThemeParams, checkLocaleValue, checkStorageArea (flag, storageBytes, fits, the commit
//       of it as local), checkStorageCommit's toJson; each optional one a flag and the text.
//   'b' method text, args text: checkBridgeCall -> status 0, or 1, a u8 mask of the fields set (action, resizeAnchor,
//       request, wallet, copyText, watchPublicData) and toJson.
//   'v' u8 has, i64 id, optional first, last and user names, u8 premium: checkViewer's toJson, then viewerJson of the
//       input itself.
//   'w' optional chatData, publicData, u8 copy: checkWidgetData's toJson.
//   'a' optional result: checkRequestAnswer -> status 0, or 1 and toJson.
//   'e' u8 failure, u8 verdict, a field name: requestFailureCode, walletFailureCode, requestVerdictName, requestRefusal
//       (optional), viewerFieldRule.
//   'G' a new RequestGate (no answer). 'g' u8 step (0 decide, 1 decideWallet, 2 finish, 3 show), f64 now, u8 bits
//       (gesture, visible, canQuery, backgroundQuery, withWallet, wallet from bit 0), u8 answered, f64 answeredAt -> the
//       verdict's name or show's flag (nothing for finish), then busy.
//   'H' a new HapticGate (no answer). 'h' u8 step (0 decide, 1 press, 2 release), f64 now, u8 visible -> the decision's
//       name for decide, nothing else.
//   'T' a map: themeParamsJson of checkThemeParams of the map.
//   'S' local map, session map: storageBytes and storageAreaFits of each, then storagePreloadJson's flag and text.
//   'A' u8 kind, url, haptic kind, haptic value: WidgetAction::toJson.
//   'W' two wallet calls: the first's toJson and methodName, and whether the two are equal.
//   'B' u8 mask, an action as for 'A', an edge, i64 id, data, u8 wallet (the request), i64 id and a wallet call, a text
//       to copy, u8 watch: BridgeCall::toJson of the fields the mask sets.
#include "oracle_io.h"
#include "twidget/sdk_policy.h"

#include <memory>

using namespace twidget;
namespace S = twidget::sdk;

namespace {

std::optional<Json> json(oracle::In& in) {
    return Json::parse(in.str(), Json::Surrogates::Keep);
}

void optJson(oracle::Out& out, bool has, const std::string& text) {
    out.flag(has).str(has ? text : std::string());
}

StorageArea area(oracle::In& in) {
    StorageArea a;
    for (uint32_t i = 0, n = in.u32(); i < n; i++) {
        std::string k = in.str();
        a[k] = in.str();
    }
    return a;
}

WalletCall walletCall(oracle::In& in) {
    WalletCall w;
    w.method = in.u8() ? WalletCall::Method::SignData : WalletCall::Method::SendTransaction;
    w.data = in.str();
    const auto tx = json(in);
    const auto payload = json(in);
    if (tx) w.transaction = S::checkWalletTransaction(*tx).value_or(WalletTransaction{});
    if (payload) w.payload = S::checkSignDataPayload(*payload).value_or(SignDataPayload{});
    return w;
}

WidgetAction action(oracle::In& in) {
    WidgetAction a;
    a.kind = WidgetAction::Kind(in.u8() % 4);
    a.url = in.str();
    a.hapticKind = in.str();
    a.hapticValue = in.str();
    return a;
}

std::unique_ptr<S::RequestGate> gate;
std::unique_ptr<S::HapticGate> haptics;

bool run(int op, oracle::In& in) {
    oracle::Out out;
    switch (op) {
    case 's': {
        const std::string s = in.str();
        out.u64(S::utf8Length(s)).u64(S::utf16Length(s)).flag(S::wellFormed(s));
        out.optStr(S::checkRequestData(s)).optStr(S::checkCopyText(s)).optStr(S::checkWidgetDataValue(s)).optStr(S::checkChatType(s));
        out.optStr(S::checkLocale(s)).optStr(S::checkResizeAnchor(s)).optStr(S::checkLink("openLink", s)).optStr(S::checkLink("openTelegramLink", s));
        out.flag(S::isPrivilegedMethod(s)).flag(S::allowedInPreview(s)).str(S::viewerFieldRule(s));
        break;
    }
    case 'L': {
        const std::string method = in.str(), url = in.str();
        const auto link = S::checkLink(method, url);
        oracle::answer(link ? 1 : 0, link.value_or(""));
        return true;
    }
    case 'j': {
        const auto v = json(in);
        if (!v) {
            oracle::answer(3, {});
            return true;
        }
        out.optStr(S::checkLinkValue("openLink", *v)).optStr(S::checkLinkValue("openTelegramLink", *v));
        const auto h = S::checkHaptic(*v);
        out.flag(h.has_value()).str(h ? h->haptic : std::string()).optStr(h ? h->value : std::nullopt);
        out.optStr(S::checkResizeAnchorValue(*v));
        const auto options = S::checkRequestOptions(*v);
        out.flag(options.has_value()).flag(options.value_or(false));
        out.optStr(S::checkWalletAddress(*v)).optStr(S::checkWalletDestination(*v));
        const auto tx = S::checkWalletTransaction(*v);
        optJson(out, tx.has_value(), tx ? tx->toJson().dump() : "");
        const auto payload = S::checkSignDataPayload(*v);
        optJson(out, payload.has_value(), payload ? payload->toJson().dump() : "");
        out.optStr(S::checkRequestDataValue(*v)).optStr(S::checkCopyTextValue(*v));
        out.str(S::themeParamsJson(S::checkThemeParams(*v)).dump());
        out.optStr(S::checkLocaleValue(*v));
        const auto a = S::checkStorageArea(*v);
        out.flag(a.has_value());
        if (a) out.u64(S::storageBytes(*a)).flag(S::storageAreaFits(*a)).str(S::StorageCommit{a, std::nullopt}.toJson().dump());
        const auto commit = S::checkStorageCommit(*v);
        optJson(out, commit.has_value(), commit ? commit->toJson().dump() : "");
        break;
    }
    case 'b': {
        const auto method = json(in);
        const auto args = json(in);
        if (!method || !args) {
            oracle::answer(3, {});
            return true;
        }
        const auto call = S::checkBridgeCall(*method, *args);
        if (!call) {
            oracle::answer(0, {});
            return true;
        }
        const uint8_t mask = uint8_t(int(call->action.has_value()) | int(call->resizeAnchor.has_value()) << 1 | int(call->request.has_value()) << 2 |
                                     int(call->wallet.has_value()) << 3 | int(call->copyText.has_value()) << 4 | int(call->watchPublicData.has_value()) << 5);
        out.u8(mask).str(call->toJson().dump());
        break;
    }
    case 'v': {
        const bool has = in.flag();
        WidgetViewer v;
        v.id = in.i64();
        v.firstName = in.optStr();
        v.lastName = in.optStr();
        v.username = in.optStr();
        v.isPremium = in.flag();
        out.str(S::checkViewer(has ? std::optional<WidgetViewer>(v) : std::nullopt).toJson().dump()).str(S::viewerJson(v).dump());
        break;
    }
    case 'w': {
        WidgetData d;
        d.chatData = in.optStr();
        d.publicData = in.optStr();
        out.str(S::checkWidgetData(d, in.flag()).toJson().dump());
        break;
    }
    case 'a': {
        WidgetRequestAnswer answer;
        answer.result = in.optStr();
        const auto checked = S::checkRequestAnswer(answer);
        oracle::answer(checked ? 1 : 0, checked ? checked->toJson().dump() : "");
        return true;
    }
    case 'e': {
        const auto failure = WidgetRequestFailure(in.u8() % 7);
        const auto verdict = S::RequestVerdict(in.u8() % 9);
        const std::string field = in.str();
        const auto refusal = S::requestRefusal(verdict);
        out.str(S::requestFailureCode(failure)).str(S::walletFailureCode(failure)).str(S::requestVerdictName(verdict));
        out.optStr(refusal ? std::optional<std::string>(*refusal) : std::nullopt).str(S::viewerFieldRule(field));
        break;
    }
    case 'G': gate = std::make_unique<S::RequestGate>(); return true;
    case 'g': {
        const uint8_t step = in.u8();
        const double now = in.f64();
        const uint8_t bits = in.u8();
        const bool answered = in.flag();
        const double answeredAt = in.f64();
        auto bit = [&](int i) { return (bits >> i & 1) != 0; };
        if (step == 0) out.str(S::requestVerdictName(gate->decide(now, {bit(0), bit(1), bit(2), bit(3), bit(4), bit(5)})));
        else if (step == 1) out.str(S::requestVerdictName(gate->decideWallet(now, {bit(0), bit(1), bit(2), bit(5)})));
        else if (step == 2) gate->finish(answered ? std::optional<double>(answeredAt) : std::nullopt);
        else out.flag(gate->show(now));
        out.flag(gate->busy());
        break;
    }
    case 'H': haptics = std::make_unique<S::HapticGate>(); return true;
    case 'h': {
        const uint8_t step = in.u8();
        const double now = in.f64();
        const bool visible = in.flag();
        if (step == 0) out.str(S::HapticGate::name(haptics->decide(now, visible)));
        else if (step == 1) haptics->press(now);
        else haptics->release(now);
        break;
    }
    case 'T': {
        const StorageArea params = area(in);
        out.str(S::themeParamsJson(S::checkThemeParams(params)).dump());
        break;
    }
    case 'S': {
        WidgetStorage storage;
        storage.local = area(in);
        storage.session = area(in);
        out.u64(S::storageBytes(storage.local)).u64(S::storageBytes(storage.session));
        out.flag(S::storageAreaFits(storage.local)).flag(S::storageAreaFits(storage.session));
        bool accepted = false;
        const std::string text = S::storagePreloadJson(storage, &accepted).dump();
        out.flag(accepted).str(text);
        break;
    }
    case 'A': out.str(action(in).toJson().dump()); break;
    case 'W': {
        const WalletCall a = walletCall(in);
        const WalletCall b = walletCall(in);
        out.str(a.toJson().dump()).str(a.methodName()).flag(a == b);
        break;
    }
    case 'B': {
        const uint8_t mask = in.u8();
        S::BridgeCall call;
        const WidgetAction a = action(in);
        const std::string edge = in.str();
        S::BridgeCall::Request request;
        request.id = in.i64();
        request.data = in.str();
        request.wallet = in.flag();
        S::BridgeCall::Wallet wallet;
        wallet.id = in.i64();
        wallet.call = walletCall(in);
        const std::string copy = in.str();
        const bool watch = in.flag();
        if (mask & 1) call.action = a;
        if (mask & 2) call.resizeAnchor = edge;
        if (mask & 4) call.request = request;
        if (mask & 8) call.wallet = wallet;
        if (mask & 16) call.copyText = copy;
        if (mask & 32) call.watchPublicData = watch;
        out.str(call.toJson().dump());
        break;
    }
    default: return false;
    }
    oracle::answer(1, out.s);
    return true;
}

} // namespace

int main() {
    return oracle::serve(run);
}
