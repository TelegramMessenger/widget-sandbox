#include "twidget/intl.h"

#include "twidget/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace twidget {

static std::string groupDigits(const std::string& intPart) {
    std::string out;
    int n = int(intPart.size());
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) out.push_back(',');
        out.push_back(intPart[size_t(i)]);
    }
    return out;
}

static std::string formatNumber(double value, const Json& opts) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-∞" : "∞";
    std::string style = opts["style"].asString();
    bool percent = style == "percent";
    if (percent) value *= 100;
    int minFrac = opts["minimumFractionDigits"].isNumber() ? opts["minimumFractionDigits"].asInt() : (style == "currency" ? 2 : 0);
    int maxFrac = opts["maximumFractionDigits"].isNumber() ? opts["maximumFractionDigits"].asInt() : std::max(minFrac, style == "currency" ? 2 : percent ? 0 : 3);
    maxFrac = std::clamp(maxFrac, 0, 20);
    minFrac = std::clamp(minFrac, 0, maxFrac);
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", maxFrac, std::fabs(value));
    std::string s(buf);
    size_t dot = s.find('.');
    std::string intPart = dot == std::string::npos ? s : s.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : s.substr(dot + 1);
    while (int(frac.size()) > minFrac && !frac.empty() && frac.back() == '0') frac.pop_back();
    bool grouping = !opts["useGrouping"].isBool() || opts["useGrouping"].asBool();
    std::string out = grouping ? groupDigits(intPart) : intPart;
    if (!frac.empty()) out += "." + frac;
    if (value < 0 && out != "0") out = "-" + out;
    if (percent) out += "%";
    if (style == "currency") {
        std::string code = opts["currency"].asString();
        if (code == "USD") out = "$" + out;
        else if (code == "EUR") out = "€" + out;
        else if (code == "GBP") out = "£" + out;
        else if (!code.empty()) out = code + " " + out;
    }
    return out;
}

static std::string two(int v) { char b[8]; snprintf(b, sizeof b, "%02d", v); return b; }

static std::string formatDate(double epochMs, const Json& opts, const std::string& method) {
    static const char* const months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    if (!std::isfinite(epochMs)) return "Invalid Date";
    time_t t = time_t(std::floor(epochMs / 1000));
    struct tm tmv {};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    auto has = [&](const char* k) { return !opts[k].isNull(); };
    bool wantsDate = has("weekday") || has("year") || has("month") || has("day") || has("dateStyle");
    bool wantsTime = has("hour") || has("minute") || has("second") || has("timeStyle");
    std::string out;
    if (wantsDate || wantsTime) {
        if (has("weekday")) { std::string w = days[tmv.tm_wday]; out += opts["weekday"].asString() == "long" ? w : w.substr(0, 3); out += ", "; }
        if (has("month")) {
            std::string m = opts["month"].asString();
            if (m == "long") out += months[tmv.tm_mon];
            else if (m == "short") out += std::string(months[tmv.tm_mon]).substr(0, 3);
            else out += std::to_string(tmv.tm_mon + 1);
            out += " ";
        }
        if (has("day")) out += std::to_string(tmv.tm_mday) + (has("year") ? ", " : " ");
        if (has("year")) out += std::to_string(tmv.tm_year + 1900) + " ";
        if (has("dateStyle")) out += std::to_string(tmv.tm_mon + 1) + "/" + std::to_string(tmv.tm_mday) + "/" + std::to_string(tmv.tm_year + 1900) + " ";
        if (has("hour") || has("timeStyle")) out += two(tmv.tm_hour) + ":" + two(tmv.tm_min);
        else if (has("minute")) out += two(tmv.tm_min);
        if (has("second") || opts["timeStyle"].asString() == "medium") out += ":" + two(tmv.tm_sec);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }
    std::string date = std::string(months[tmv.tm_mon]).substr(0, 3) + " " + std::to_string(tmv.tm_mday) + ", " + std::to_string(tmv.tm_year + 1900);
    std::string time = two(tmv.tm_hour) + ":" + two(tmv.tm_min) + ":" + two(tmv.tm_sec);
    if (method == "toLocaleDateString") return date;
    if (method == "toLocaleTimeString") return time;
    return date + ", " + time;
}

std::string BasicIntl::format(const Json& req) {
    const std::string& kind = req["kind"].asString();
    const Json& opts = req["opts"];
    if (kind == "number") return formatNumber(req["value"].asNumber(), opts);
    if (kind == "date" || kind == "dateMethod") return formatDate(req["value"].asNumber(), opts, req["method"].asString());
    if (kind == "plural") return req["value"].asNumber() == 1 ? "one" : "other";
    if (kind == "relative") {
        double v = req["value"].asNumber();
        std::string unit = req["unit"].asString();
        if (!unit.empty() && unit.back() == 's') unit.pop_back();
        long long n = (long long)std::llround(std::fabs(v));
        std::string u = unit + (n == 1 ? "" : "s");
        return v < 0 ? std::to_string(n) + " " + u + " ago" : "in " + std::to_string(n) + " " + u;
    }
    if (kind == "list") {
        const auto& items = req["value"].arr();
        std::string out;
        for (size_t i = 0; i < items.size(); i++) {
            if (i > 0) out += items.size() == 2 ? " and " : (i == items.size() - 1 ? ", and " : ", ");
            out += items[i].asString();
        }
        return out;
    }
    if (kind == "compare") {
        int c = req["a"].asString().compare(req["b"].asString());
        return c < 0 ? "-1" : c > 0 ? "1" : "0";
    }
    if (kind == "upper" || kind == "lower") {
        std::string s = req["value"].asString();
        for (auto& ch : s) ch = char(kind == "upper" ? std::toupper((unsigned char)ch) : std::tolower((unsigned char)ch));
        return s;
    }
    return "";
}

} // namespace twidget
