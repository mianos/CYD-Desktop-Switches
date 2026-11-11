#pragma once
#include <cstdint>

// Antialiased bitmap faces, generated at build-authoring time by
// tools/gen_font.py from a TTF (main/font_gen.h holds the pixels). Each pixel
// carries four bits of coverage so glyph edges fade into whatever is behind
// them instead of being stamped as hard pixels — at 143 ppi that is the
// difference between "1980s terminal" and a modern panel.
namespace font {

struct Glyph {
  uint32_t offset;  // into Face::bits
  uint8_t w, h;     // ink size in pixels (0 = blank, e.g. space)
  int8_t left;      // ink left edge, relative to the pen on the baseline
  int8_t top;       // ink top edge, relative to the baseline (negative = above)
  uint8_t advance;  // pen advance in pixels
};

struct Face {
  const char* name;
  const uint8_t* bits;  // 4bpp coverage, two pixels per byte, row-major
  const Glyph* glyphs;
  uint8_t first, last;  // inclusive range of character codes covered
  int16_t lineHeight;   // ascent + descent
  int16_t ascent;       // top of the line box to the baseline
};

// Null when the face has no glyph for `c` (anything outside first..last).
inline const Glyph* glyph(const Face& f, char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u < f.first || u > f.last) return nullptr;
  return &f.glyphs[u - f.first];
}

}  // namespace font
