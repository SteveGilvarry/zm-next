// overlay/draw.hpp — pure pixel-drawing helpers for the overlay PROCESS plugin.
//
// No ABI / no FFmpeg / no plugin headers — just RGB24 pixel writes so the
// helpers can be unit-tested in isolation. All writes are clamped to the image
// bounds (w x h, 3 bytes per pixel, tightly packed). Coordinates that fall
// partly or fully off-screen are silently clipped — never out-of-bounds.
//
// A compact embedded 5x7 ASCII bitmap font (digits, uppercase letters and a few
// punctuation chars) is used for labels. Lowercase letters map to uppercase.
// Unknown characters are skipped.

#pragma once

#include <cstdint>
#include <string>

namespace zm {
namespace overlay {

// ---------------------------------------------------------------------------
// 5x7 bitmap font. Each glyph is 7 rows of 5 bits (LSB = leftmost column,
// stored in the low 5 bits of each byte, MSB->bit4 unused -> we use bit0..bit4
// where bit0 is the LEFT-most pixel). A set bit means "draw this pixel".
// ---------------------------------------------------------------------------
constexpr int kFontW = 5;
constexpr int kFontH = 7;

struct Glyph {
    char ch;
    uint8_t rows[7];  // each row: low 5 bits, bit0 = leftmost column
};

// Helper to read columns: row byte, bit (1<<col) where col in [0,4].
inline const Glyph* font_lookup(char c) {
    // Compact table. Lowercase mapped to uppercase by caller.
    static const Glyph kFont[] = {
        {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
        {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
        {'2', {0x0E,0x11,0x10,0x08,0x04,0x02,0x1F}},
        {'3', {0x1F,0x08,0x04,0x08,0x10,0x11,0x0E}},
        {'4', {0x08,0x0C,0x0A,0x09,0x1F,0x08,0x08}},
        {'5', {0x1F,0x01,0x0F,0x10,0x10,0x11,0x0E}},
        {'6', {0x0C,0x02,0x01,0x0F,0x11,0x11,0x0E}},
        {'7', {0x1F,0x10,0x08,0x04,0x02,0x02,0x02}},
        {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
        {'9', {0x0E,0x11,0x11,0x1E,0x10,0x08,0x06}},
        {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'B', {0x0F,0x11,0x11,0x0F,0x11,0x11,0x0F}},
        {'C', {0x0E,0x11,0x01,0x01,0x01,0x11,0x0E}},
        {'D', {0x07,0x09,0x11,0x11,0x11,0x09,0x07}},
        {'E', {0x1F,0x01,0x01,0x0F,0x01,0x01,0x1F}},
        {'F', {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01}},
        {'G', {0x0E,0x11,0x01,0x1D,0x11,0x11,0x1E}},
        {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
        {'J', {0x1C,0x08,0x08,0x08,0x08,0x09,0x06}},
        {'K', {0x11,0x09,0x05,0x03,0x05,0x09,0x11}},
        {'L', {0x01,0x01,0x01,0x01,0x01,0x01,0x1F}},
        {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
        {'N', {0x11,0x13,0x15,0x19,0x11,0x11,0x11}},
        {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'P', {0x0F,0x11,0x11,0x0F,0x01,0x01,0x01}},
        {'Q', {0x0E,0x11,0x11,0x11,0x15,0x09,0x16}},
        {'R', {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11}},
        {'S', {0x1E,0x01,0x01,0x0E,0x10,0x10,0x0F}},
        {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
        {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
        {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
        {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
        {'Z', {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F}},
        {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
        {',', {0x00,0x00,0x00,0x00,0x0C,0x04,0x02}},
        {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
        {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
        {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
        {'#', {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
        {'%', {0x03,0x13,0x08,0x04,0x02,0x19,0x18}},
        {'/', {0x10,0x10,0x08,0x04,0x02,0x01,0x01}},
        {'(', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
        {')', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    };
    for (const auto& g : kFont) {
        if (g.ch == c) return &g;
    }
    return nullptr;
}

// Write a single pixel, clamped to bounds. (x,y) in pixels.
inline void put_px(uint8_t* rgb, int w, int h, int x, int y,
                   uint8_t r, uint8_t g, uint8_t b) {
    if (!rgb || x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* p = rgb + (static_cast<size_t>(y) * w + x) * 3;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

// Draw a rectangle OUTLINE at (x,y) with size bw x bh, given border thickness.
// The outline is drawn inward so a thickness>1 stays adjacent to the edge.
// All writes are clamped via put_px.
inline void draw_rect(uint8_t* rgb, int w, int h, int x, int y, int bw, int bh,
                      uint8_t r, uint8_t g, uint8_t b, int thickness) {
    if (!rgb || w <= 0 || h <= 0 || bw <= 0 || bh <= 0) return;
    if (thickness < 1) thickness = 1;
    // Don't let thickness exceed half the box (otherwise it becomes a fill).
    int tmax = (bw < bh ? bw : bh);
    if (thickness > tmax) thickness = tmax;

    const int x0 = x;
    const int y0 = y;
    const int x1 = x + bw - 1;  // inclusive right
    const int y1 = y + bh - 1;  // inclusive bottom

    for (int t = 0; t < thickness; ++t) {
        // Top & bottom edges (rows y0+t and y1-t), spanning full width.
        for (int xx = x0; xx <= x1; ++xx) {
            put_px(rgb, w, h, xx, y0 + t, r, g, b);
            put_px(rgb, w, h, xx, y1 - t, r, g, b);
        }
        // Left & right edges (cols x0+t and x1-t), spanning full height.
        for (int yy = y0; yy <= y1; ++yy) {
            put_px(rgb, w, h, x0 + t, yy, r, g, b);
            put_px(rgb, w, h, x1 - t, yy, r, g, b);
        }
    }
}

// Draw text starting at top-left (x,y) using the 5x7 font scaled by `scale`.
// Lowercase letters map to uppercase. Unknown glyphs are skipped (their advance
// is still consumed so spacing stays regular). All writes clamped via put_px.
inline void draw_text(uint8_t* rgb, int w, int h, int x, int y,
                      const std::string& text,
                      uint8_t r, uint8_t g, uint8_t b, int scale) {
    if (!rgb || w <= 0 || h <= 0 || text.empty()) return;
    if (scale < 1) scale = 1;

    const int advance = (kFontW + 1) * scale;  // 1px gap between glyphs
    int penX = x;
    for (char c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        const Glyph* glyph = font_lookup(c);
        if (glyph) {
            for (int row = 0; row < kFontH; ++row) {
                uint8_t bits = glyph->rows[row];
                for (int col = 0; col < kFontW; ++col) {
                    if (bits & (1u << col)) {
                        // Scale each font pixel into a scale x scale block.
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                put_px(rgb, w, h,
                                       penX + col * scale + sx,
                                       y + row * scale + sy,
                                       r, g, b);
                            }
                        }
                    }
                }
            }
        }
        penX += advance;
    }
}

}  // namespace overlay
}  // namespace zm
