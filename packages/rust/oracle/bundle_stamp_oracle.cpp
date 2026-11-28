// widget-sandbox's BundleStamp on stdin, for rust/crates/bundle_stamp's tests to compare with the Rust port.
//
// Records (oracle_io.h): 'p' source: parse -> status 0, or 1 and the version, i32 protocol, profile, sdk; 'r' source, name:
// refusal -> status 0, or 1 and the text; 'R' the sandbox, validator and renderer bundles: refusal of the set, the same;
// 'g' nothing: the glue's version, numbers, describe and line; 'l' version, i32 protocol, profile, sdk: describe and line of
// that stamp; 'D' a directory: SandboxResources::fromDirectory -> status 1 and the three bundles, or 0 and the exception's text.
#include "oracle_io.h"
#include "twidget/bundle_stamp.h"
#include "twidget/resources.h"

#include <stdexcept>

using twidget::BundleStamp;
using twidget::SandboxResources;

namespace {

void refusal(const std::optional<std::string>& why) {
    oracle::answer(why ? 1 : 0, why.value_or(""));
}

void stamp(oracle::Out& out, const BundleStamp& s) {
    out.str(s.version).i32(s.protocol).i32(s.profile).i32(s.sdk).str(s.describe()).str(s.line());
}

} // namespace

int main() {
    return oracle::serve([](int op, oracle::In& in) {
        oracle::Out out;
        switch (op) {
        case 'p': {
            const auto s = BundleStamp::parse(in.str());
            if (s) out.str(s->version).i32(s->protocol).i32(s->profile).i32(s->sdk);
            oracle::answer(s ? 1 : 0, out.s);
            return true;
        }
        case 'r': {
            const std::string source = in.str(), name = in.str();
            refusal(BundleStamp::refusal(source, name));
            return true;
        }
        case 'R': {
            SandboxResources r;
            r.sandboxBundle = in.str();
            r.validatorBundle = in.str();
            r.rendererBundle = in.str();
            refusal(BundleStamp::refusal(r));
            return true;
        }
        case 'g': stamp(out, BundleStamp::glue()); break;
        case 'l': {
            BundleStamp s;
            s.version = in.str();
            s.protocol = in.i32();
            s.profile = in.i32();
            s.sdk = in.i32();
            stamp(out, s);
            break;
        }
        case 'D':
            try {
                const auto r = SandboxResources::fromDirectory(in.str());
                out.str(r->sandboxBundle).str(r->validatorBundle).str(r->rendererBundle);
            } catch (const std::exception& e) {
                oracle::answer(0, e.what());
                return true;
            }
            break;
        default: return false;
        }
        oracle::answer(1, out.s);
        return true;
    });
}
