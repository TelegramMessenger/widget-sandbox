// What one paint of the litehtml document consists of, in CSS px: the
// container records litehtml's draw callbacks into this list, and a platform
// painter executes it (CoreGraphics on iOS/macOS in src/apple, Canvas on
// Android from the JSON form). Everything the painter needs is here; nothing
// refers back to litehtml.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace twlite {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

/// Per-corner elliptical radii (CSS order, x then y). All zero = plain rectangle.
struct Radii {
    float tlx = 0, tly = 0, trx = 0, try_ = 0, brx = 0, bry = 0, blx = 0, bly = 0;
    bool any() const { return tlx > 0 || tly > 0 || trx > 0 || try_ > 0 || brx > 0 || bry > 0 || blx > 0 || bly > 0; }
};

struct Stop {
    float offset = 0;  // 0..1
    Color color;
};

enum class Cmd {
    FillRect,       // rect, radii, color; clipped to `clip` when has_clip
    StrokeRect,     // rect (the stroke's centre line), radii, color, width
    FillEllipse,    // rect (bounding box), color
    StrokeEllipse,  // rect, color, width
    Text,           // text, font, color, x, y (baseline)
    Image,          // image, rect (destination), radii + clip from the background layer
    Linear,         // rect (border box), radii, clip, ax/ay → bx/by, stops
    Radial,         // rect, radii, clip, centre (ax, ay), radii (bx, by), stops
    Conic,          // rect, radii, clip, centre (ax, ay), angle (bx, degrees clockwise from the top), stops
    ClipPush,       // rect, radii: intersect the clip until the matching ClipPop
    ClipPop,
};

struct Command {
    Cmd kind = Cmd::FillRect;
    Rect rect;
    Radii radii;
    Rect clip;
    bool has_clip = false;
    Color color;
    float width = 0;
    int font = 0;
    std::string text;
    float x = 0, y = 0;
    int image = 0;
    float ax = 0, ay = 0, bx = 0, by = 0;
    std::vector<Stop> stops;
};

using DisplayList = std::vector<Command>;

/// Compact JSON for the Kotlin painter (`LitehtmlPainter.kt`): an array of
/// `{"k":<kind>, ...}` objects with the fields above under one-letter keys.
std::string to_json(const DisplayList& list);

} // namespace twlite
