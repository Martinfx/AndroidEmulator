// A small anti-aliased software rasterizer: the pixels behind android.graphics.Canvas.
//
// Everything is drawn into a 32-bit ARGB framebuffer (the same 0xAARRGGBB
// layout Android's Color ints use). Shapes are rendered by computing, for
// every pixel near the shape, how much of the pixel the shape covers
// (0..1) and blending the colour with that coverage ("source over").
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace droid {

struct RectI {
    int left, top, right, bottom;
};

class Raster {
public:
    Raster(int width, int height);

    int width() const { return w_; }
    int height() const { return h_; }
    const uint32_t* pixels() const { return px_.data(); }
    uint32_t pixel(int x, int y) const { return px_[size_t(y) * w_ + x]; }

    // Transform and clip state, like Canvas.save()/restore().
    void save();
    void restore();
    int saveCount() const { return int(stack_.size()); }
    void translate(float dx, float dy);
    void scale(float sx, float sy);
    void clipRect(float l, float t, float r, float b);  // intersects, in local coordinates
    RectI clipBounds() const { return st_.clip; }

    void fill(uint32_t color);  // fills the clip area
    void fillRect(float l, float t, float r, float b, uint32_t color);
    void strokeRect(float l, float t, float r, float b, float width, uint32_t color);
    void fillRoundRect(float l, float t, float r, float b, float radius, uint32_t color);
    void strokeRoundRect(float l, float t, float r, float b, float radius, float width, uint32_t color);
    void fillOval(float l, float t, float r, float b, uint32_t color);
    void strokeOval(float l, float t, float r, float b, float width, uint32_t color);
    void line(float x0, float y0, float x1, float y1, float width, uint32_t color);

    // Text with the baseline at y; size is the font size in pixels.
    void text(const std::u16string& s, float x, float y, float size, uint32_t color);
    static float textWidth(const std::u16string& s, float size);
    static float ascent(float size);   // positive, above the baseline
    static float descent(float size);  // positive, below the baseline
    static float lineHeight(float size);

private:
    struct State {
        float tx = 0, ty = 0, sx = 1, sy = 1;
        RectI clip;
    };

    void blend(int x, int y, uint32_t color, float coverage);
    template <typename F>
    void coverageFill(float l, float t, float r, float b, F signedDistance, uint32_t color);
    float mapX(float x) const { return st_.tx + x * st_.sx; }
    float mapY(float y) const { return st_.ty + y * st_.sy; }

    int w_, h_;
    std::vector<uint32_t> px_;
    State st_;
    std::vector<State> stack_;
};

}  // namespace droid
