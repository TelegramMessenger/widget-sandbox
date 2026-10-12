// `Intl` is delegated to the host (kinds number, numberParts, numberOptions,
// date, dateMethod, dateParts, dateOptions, plural, relative, list, compare,
// upper, lower; see src/sandbox/globals.ts). The core ships a locale-agnostic
// fallback good enough for the harness and tests; a product host (tdesktop)
// plugs QLocale / ICU in through this interface.
#pragma once

#include <string>

namespace twidget {

class Json;

class IntlProvider {
public:
    virtual ~IntlProvider() = default;
    /// Request object → formatted string ("" when unsupported). Pure, no I/O.
    virtual std::string format(const Json& request) = 0;
};

/// English-style formatting without locale data: grouping, fraction digits,
/// percent/currency codes, ISO-like dates in UTC, `one`/`other` plurals,
/// byte-wise collation. Returns "" for the `*Parts` / `*Options` kinds.
class BasicIntl : public IntlProvider {
public:
    std::string format(const Json& request) override;
};

} // namespace twidget
