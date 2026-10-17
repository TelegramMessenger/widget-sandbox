#include "twidget/intl.h"

#include "twidget/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

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

using Parts = std::vector<std::pair<std::string, std::string>>;

static std::string joined(const Parts& parts) {
    std::string out;
    for (const auto& p : parts) out += p.second;
    return out;
}

/// JS `NumberFormat.formatToParts` over this formatter's en output.
static Parts numberParts(const std::string& s) {
    Parts out;
    auto push = [&](const char* type, const std::string& text) {
        if (!out.empty() && out.back().first == type && (out.back().first == "literal" || out.back().first == "currency" || out.back().first == "integer" || out.back().first == "fraction")) out.back().second += text;
        else out.emplace_back(type, text);
    };
    if (s == "NaN") return {{"nan", s}};
    bool fraction = false;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (std::isdigit(c)) { push(fraction ? "fraction" : "integer", std::string(1, char(c))); i++; continue; }
        if (c == ',' ) { push("group", ","); i++; continue; }
        if (c == '.') { push("decimal", "."); fraction = true; i++; continue; }
        if (c == '-') { push("minusSign", "-"); i++; continue; }
        if (c == '%') { push("percentSign", "%"); i++; continue; }
        if (c == ' ') { push("literal", " "); i++; continue; }
        if (s.compare(i, 3, "\xE2\x88\x9E") == 0) { push("infinity", s.substr(i, 3)); i += 3; continue; }
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        push("currency", s.substr(i, n));
        i += n;
    }
    return out;
}

static Parts dateParts(double epochMs, const Json& opts, const std::string& method) {
    static const char* const months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    if (!std::isfinite(epochMs)) return {{"literal", "Invalid Date"}};
    time_t t = time_t(std::floor(epochMs / 1000));
    struct tm tmv {};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    Parts out;
    auto field = [&](const char* type, const std::string& text) { out.emplace_back(type, text); };
    auto lit = [&](const std::string& text) {
        if (!out.empty() && out.back().first == "literal") out.back().second += text;
        else out.emplace_back("literal", text);
    };
    auto has = [&](const char* k) { return !opts[k].isNull(); };
    bool wantsDate = has("weekday") || has("year") || has("month") || has("day") || has("dateStyle");
    bool wantsTime = has("hour") || has("minute") || has("second") || has("timeStyle");
    if (wantsDate || wantsTime) {
        if (has("weekday")) { std::string w = days[tmv.tm_wday]; field("weekday", opts["weekday"].asString() == "long" ? w : w.substr(0, 3)); lit(", "); }
        if (has("month")) {
            std::string m = opts["month"].asString();
            if (m == "long") field("month", months[tmv.tm_mon]);
            else if (m == "short") field("month", std::string(months[tmv.tm_mon]).substr(0, 3));
            else field("month", std::to_string(tmv.tm_mon + 1));
            lit(" ");
        }
        if (has("day")) { field("day", std::to_string(tmv.tm_mday)); lit(has("year") ? ", " : " "); }
        if (has("year")) { field("year", std::to_string(tmv.tm_year + 1900)); lit(" "); }
        if (has("dateStyle")) {
            field("month", std::to_string(tmv.tm_mon + 1)); lit("/"); field("day", std::to_string(tmv.tm_mday)); lit("/"); field("year", std::to_string(tmv.tm_year + 1900)); lit(" ");
        }
        if (has("hour") || has("timeStyle")) { field("hour", two(tmv.tm_hour)); lit(":"); field("minute", two(tmv.tm_min)); }
        else if (has("minute")) field("minute", two(tmv.tm_min));
        if (has("second") || opts["timeStyle"].asString() == "medium") { lit(":"); field("second", two(tmv.tm_sec)); }
        while (!out.empty() && out.back().first == "literal") {
            std::string& v = out.back().second;
            while (!v.empty() && v.back() == ' ') v.pop_back();
            if (!v.empty()) break;
            out.pop_back();
        }
        return out;
    }
    auto date = [&]() { field("month", std::string(months[tmv.tm_mon]).substr(0, 3)); lit(" "); field("day", std::to_string(tmv.tm_mday)); lit(", "); field("year", std::to_string(tmv.tm_year + 1900)); };
    auto time = [&]() { field("hour", two(tmv.tm_hour)); lit(":"); field("minute", two(tmv.tm_min)); lit(":"); field("second", two(tmv.tm_sec)); };
    if (method == "toLocaleDateString") date();
    else if (method == "toLocaleTimeString") time();
    else { date(); lit(", "); time(); }
    return out;
}

static std::string formatDate(double epochMs, const Json& opts, const std::string& method) { return joined(dateParts(epochMs, opts, method)); }

static std::string partsJson(const Parts& parts) {
    std::string out = "[";
    for (const auto& p : parts) {
        if (out.size() > 1) out += ",";
        out += "{\"type\":" + Json(p.first).dump() + ",\"value\":" + Json(p.second).dump() + "}";
    }
    return out + "]";
}

std::string BasicIntl::format(const Json& req) {
    const std::string& kind = req["kind"].asString();
    const Json& opts = req["opts"];
    if (kind == "number") return formatNumber(req["value"].asNumber(), opts);
    if (kind == "numberParts") return partsJson(numberParts(formatNumber(req["value"].asNumber(), opts)));
    if (kind == "date" || kind == "dateMethod") return formatDate(req["value"].asNumber(), opts, req["method"].asString());
    if (kind == "dateParts") return partsJson(dateParts(req["value"].asNumber(), opts, req["method"].asString()));
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
