#include "gfx/raster.h"

#include <algorithm>
#include <cmath>

#include "gfx/font_data.h"

namespace droid {

namespace {

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Length of the overlap between [a0, a1) and [b0, b1).
float overlap(float a0, float a1, float b0, float b1) {
    return std::max(0.f, std::min(a1, b1) - std::max(a0, b0));
}

const font_data::Glyph* findGlyph(uint32_t cp) {
    const auto* begin = std::begin(font_data::kGlyphs);
    const auto* end = std::end(font_data::kGlyphs);
    const auto* it = std::lower_bound(begin, end, cp, [](const font_data::Glyph& g, uint32_t c) { return g.cp < c; });
    if (it != end && it->cp == cp) return it;
    if (cp != '?') return findGlyph('?');
    return nullptr;
}

// Bilinear sample of a glyph bitmap; outside the bitmap is transparent.
float sampleGlyph(const font_data::Glyph& g, float u, float v) {
    int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    float fx = u - x0, fy = v - y0;
    auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= g.w || y >= g.h) return 0.f;
        return font_data::kPixels[g.offset + size_t(y) * g.w + x] / 255.f;
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
           (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

}  // namespace

Raster::Raster(int width, int height) : w_(width), h_(height), px_(size_t(width) * height, 0xff000000) {
    st_.clip = {0, 0, width, height};
}

void Raster::save() { stack_.push_back(st_); }

void Raster::restore() {
    if (stack_.empty()) return;
    st_ = stack_.back();
    stack_.pop_back();
}

void Raster::translate(float dx, float dy) {
    st_.tx += dx * st_.sx;
    st_.ty += dy * st_.sy;
}

void Raster::scale(float sx, float sy) {
    st_.sx *= sx;
    st_.sy *= sy;
}

void Raster::clipRect(float l, float t, float r, float b) {
    RectI c{int(std::floor(mapX(l))), int(std::floor(mapY(t))), int(std::ceil(mapX(r))), int(std::ceil(mapY(b)))};
    st_.clip.left = std::max(st_.clip.left, c.left);
    st_.clip.top = std::max(st_.clip.top, c.top);
    st_.clip.right = std::min(st_.clip.right, c.right);
    st_.clip.bottom = std::min(st_.clip.bottom, c.bottom);
}

// Source-over blending of a non-premultiplied ARGB colour.
void Raster::blend(int x, int y, uint32_t color, float coverage) {
    float a = (color >> 24) / 255.f * coverage;
    if (a <= 0) return;
    uint32_t& d = px_[size_t(y) * w_ + x];
    if (a >= 1) { d = color | 0xff000000; return; }
    auto mix = [a](uint32_t s, uint32_t dst) { return uint32_t(float(s) * a + float(dst) * (1 - a) + 0.5f); };
    uint32_t r = mix((color >> 16) & 0xff, (d >> 16) & 0xff);
    uint32_t g = mix((color >> 8) & 0xff, (d >> 8) & 0xff);
    uint32_t b = mix(color & 0xff, d & 0xff);
    d = 0xff000000 | (r << 16) | (g << 8) | b;
}

void Raster::fill(uint32_t color) {
    bool opaque = (color >> 24) == 0xff;
    for (int y = st_.clip.top; y < st_.clip.bottom; ++y) {
        if (opaque) {
            std::fill(px_.begin() + y * w_ + st_.clip.left, px_.begin() + y * w_ + st_.clip.right, color);
            continue;
        }
        for (int x = st_.clip.left; x < st_.clip.right; ++x) blend(x, y, color, 1.f);
    }
}

// Visits every pixel of the (device space) box l,t,r,b inside the clip and
// blends with coverage computed from a signed distance: < 0 inside, > 0 outside.
//
// Speed-up: the box is walked in 8x8 tiles. A signed distance tells how far
// the nearest edge is, so if the tile centre is further from the edge than
// the tile's half-diagonal, the whole tile is either fully inside (fill it
// without further maths) or fully outside (skip it).
template <typename F>
void Raster::coverageFill(float l, float t, float r, float b, F sd, uint32_t color) {
    int x0 = std::max(st_.clip.left, int(std::floor(l)) - 1), x1 = std::min(st_.clip.right, int(std::ceil(r)) + 1);
    int y0 = std::max(st_.clip.top, int(std::floor(t)) - 1), y1 = std::min(st_.clip.bottom, int(std::ceil(b)) + 1);
    constexpr int T = 8;
    const float reach = T * 0.7072f + 1.f;  // half-diagonal of a tile plus the anti-aliasing band
    for (int ty = y0; ty < y1; ty += T)
        for (int tx = x0; tx < x1; tx += T) {
            int ex = std::min(tx + T, x1), ey = std::min(ty + T, y1);
            float d = sd(tx + T / 2.f, ty + T / 2.f);
            if (d > reach) continue;
            bool inside = d < -reach;
            for (int y = ty; y < ey; ++y)
                for (int x = tx; x < ex; ++x) {
                    float c = inside ? 1.f : clamp01(0.5f - sd(x + 0.5f, y + 0.5f));
                    if (c > 0) blend(x, y, color, c);
                }
        }
}

void Raster::fillRect(float l, float t, float r, float b, uint32_t color) {
    float L = mapX(std::min(l, r)), R = mapX(std::max(l, r)), T = mapY(std::min(t, b)), B = mapY(std::max(t, b));
    int x0 = std::max(st_.clip.left, int(std::floor(L))), x1 = std::min(st_.clip.right, int(std::ceil(R)));
    int y0 = std::max(st_.clip.top, int(std::floor(T))), y1 = std::min(st_.clip.bottom, int(std::ceil(B)));
    for (int y = y0; y < y1; ++y) {
        float cy = overlap(float(y), float(y + 1), T, B);
        for (int x = x0; x < x1; ++x) blend(x, y, color, cy * overlap(float(x), float(x + 1), L, R));
    }
}

void Raster::strokeRect(float l, float t, float r, float b, float width, uint32_t color) {
    float h = width / 2;
    fillRect(l - h, t - h, r + h, t + h, color);
    fillRect(l - h, b - h, r + h, b + h, color);
    fillRect(l - h, t + h, l + h, b - h, color);
    fillRect(r - h, t + h, r + h, b - h, color);
}

namespace {
// Signed distance from point (px,py) to a rounded box centred at (cx,cy).
float roundBoxSd(float px, float py, float cx, float cy, float hw, float hh, float radius) {
    float qx = std::fabs(px - cx) - hw + radius, qy = std::fabs(py - cy) - hh + radius;
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
}
}  // namespace

void Raster::fillRoundRect(float l, float t, float r, float b, float radius, uint32_t color) {
    float L = mapX(l), R = mapX(r), T = mapY(t), B = mapY(b), rad = radius * st_.sx;
    float cx = (L + R) / 2, cy = (T + B) / 2, hw = std::fabs(R - L) / 2, hh = std::fabs(B - T) / 2;
    rad = std::min({rad, hw, hh});
    coverageFill(std::min(L, R), std::min(T, B), std::max(L, R), std::max(T, B),
                 [=](float x, float y) { return roundBoxSd(x, y, cx, cy, hw, hh, rad); }, color);
}

void Raster::strokeRoundRect(float l, float t, float r, float b, float radius, float width, uint32_t color) {
    float L = mapX(l), R = mapX(r), T = mapY(t), B = mapY(b), rad = radius * st_.sx, half = width * st_.sx / 2;
    float cx = (L + R) / 2, cy = (T + B) / 2, hw = std::fabs(R - L) / 2, hh = std::fabs(B - T) / 2;
    rad = std::min({rad, hw, hh});
    coverageFill(std::min(L, R) - half, std::min(T, B) - half, std::max(L, R) + half, std::max(T, B) + half,
                 [=](float x, float y) { return std::fabs(roundBoxSd(x, y, cx, cy, hw, hh, rad)) - half; }, color);
}

namespace {
// Approximate signed distance to an ellipse (exact for circles).
float ellipseSd(float px, float py, float cx, float cy, float rx, float ry) {
    float dx = (px - cx) / rx, dy = (py - cy) / ry;
    float k = std::sqrt(dx * dx + dy * dy);
    if (k == 0) return -std::min(rx, ry);
    // Scale the normalised distance by the local radius in the direction of the point.
    float localR = std::sqrt((dx * rx) * (dx * rx) + (dy * ry) * (dy * ry)) / k;
    return (k - 1) * localR;
}
}  // namespace

void Raster::fillOval(float l, float t, float r, float b, uint32_t color) {
    float L = mapX(l), R = mapX(r), T = mapY(t), B = mapY(b);
    float cx = (L + R) / 2, cy = (T + B) / 2, rx = std::fabs(R - L) / 2, ry = std::fabs(B - T) / 2;
    if (rx <= 0 || ry <= 0) return;
    coverageFill(cx - rx, cy - ry, cx + rx, cy + ry, [=](float x, float y) { return ellipseSd(x, y, cx, cy, rx, ry); }, color);
}

void Raster::strokeOval(float l, float t, float r, float b, float width, uint32_t color) {
    float L = mapX(l), R = mapX(r), T = mapY(t), B = mapY(b), half = width * st_.sx / 2;
    float cx = (L + R) / 2, cy = (T + B) / 2, rx = std::fabs(R - L) / 2, ry = std::fabs(B - T) / 2;
    if (rx <= 0 || ry <= 0) return;
    coverageFill(cx - rx - half, cy - ry - half, cx + rx + half, cy + ry + half,
                 [=](float x, float y) { return std::fabs(ellipseSd(x, y, cx, cy, rx, ry)) - half; }, color);
}

void Raster::line(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    float ax = mapX(x0), ay = mapY(y0), bx = mapX(x1), by = mapY(y1);
    float half = std::max(width * st_.sx, 1.f) / 2;  // width 0 means "hairline" in Android
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    // Distance to the segment, with flat ("butt") ends like Paint.Cap.BUTT.
    auto sd = [=](float x, float y) {
        if (len2 == 0) return std::hypot(x - ax, y - ay) - half;
        float t = ((x - ax) * dx + (y - ay) * dy) / len2;
        float len = std::sqrt(len2);
        float along = std::max(-t * len, (t - 1) * len);  // > 0 beyond the ends
        float across = std::fabs((x - ax) * dy - (y - ay) * dx) / len - half;
        return std::max(along, across);
    };
    coverageFill(std::min(ax, bx) - half, std::min(ay, by) - half, std::max(ax, bx) + half, std::max(ay, by) + half, sd, color);
}

// ---------------------------------------------------------------- text

float Raster::ascent(float size) { return font_data::kAscent * size / font_data::kSize; }
float Raster::descent(float size) { return (font_data::kLineHeight - font_data::kAscent) * size / font_data::kSize; }
float Raster::lineHeight(float size) { return font_data::kLineHeight * size / font_data::kSize; }

float Raster::textWidth(const std::u16string& s, float size) {
    float w = 0;
    for (char16_t c : s)
        if (const auto* g = findGlyph(c)) w += g->advance;
    return w * size / font_data::kSize;
}

void Raster::text(const std::u16string& s, float x, float y, float size, uint32_t color) {
    float scale = size * st_.sx / font_data::kSize;
    if (scale <= 0) return;
    float penX = mapX(x);
    float top = mapY(y) - font_data::kAscent * scale;
    for (char16_t c : s) {
        const auto* g = findGlyph(c);
        if (!g) continue;
        if (g->w > 0 && g->h > 0) {
            float gx = penX + g->x * scale, gy = top + g->y * scale;
            float gw = g->w * scale, gh = g->h * scale;
            int x0 = std::max(st_.clip.left, int(std::floor(gx))), x1 = std::min(st_.clip.right, int(std::ceil(gx + gw)));
            int y0 = std::max(st_.clip.top, int(std::floor(gy))), y1 = std::min(st_.clip.bottom, int(std::ceil(gy + gh)));
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px) {
                    float u = (px + 0.5f - gx) / scale - 0.5f, v = (py + 0.5f - gy) / scale - 0.5f;
                    float cov = sampleGlyph(*g, u, v);
                    if (cov > 0.01f) blend(px, py, color, cov);
                }
        }
        penX += g->advance * scale;
    }
}

}  // namespace droid
