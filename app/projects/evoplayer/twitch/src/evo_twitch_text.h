// evo_twitch_text.h - text from Twitch made safe for the app's baked fonts.
//
// Names and stream titles carry emoji and scripts the kit's fonts were not
// baked with (ps5-homebrew-ui tools/font-baker, the "european" set: ASCII,
// Latin-1, Latin Extended-A, Greek, basic Cyrillic and a little punctuation),
// and a missing glyph draws as '?'. The Twitch screens filter with the font
// itself (ui_kit drawable()); the toasts and the player title are built here,
// away from the kit, so this keeps a fixed list of what the fonts cover,
// checked against the four .huifont files in assets/hui/fonts:
//
//   - Inter (toasts) has all of it but U+00AD and U+0149;
//   - Montserrat (the player's title) has the same Latin and Cyrillic but
//     only four Greek letters, so the title drops Greek.
//
// The rules match drawable(): a run of dropped characters becomes one space,
// none next to a space or before punctuation; control characters become
// spaces; spaces collapse; the ends are trimmed. Header-only, no exceptions
// thrown beyond std::string's own allocation.
#ifndef EVO_TWITCH_TEXT_H
#define EVO_TWITCH_TEXT_H

#include <cstdint>
#include <cstring>
#include <string>

namespace evo_twitch_text {

// Next code point of s from i (moved past it), or 0xFFFD for a bad byte.
inline std::uint32_t nextCodepoint(const std::string& s, std::size_t* i) {
  const unsigned char c = static_cast<unsigned char>(s[*i]);
  int n = 0;
  std::uint32_t cp = 0;
  if (c < 0x80) {
    *i += 1;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    n = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    n = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    n = 3;
  } else {
    *i += 1;
    return 0xFFFD;
  }
  // The continuation bytes are s[*i + 1] .. s[*i + n]: cut short at the end.
  if (*i + n >= s.size()) {
    *i = s.size();
    return 0xFFFD;
  }
  for (int k = 1; k <= n; ++k) {
    const unsigned char b = static_cast<unsigned char>(s[*i + k]);
    if ((b & 0xC0) != 0x80) {
      *i += k;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  *i += n + 1;
  return cp;
}

// True when the baked fonts draw cp (see the top of the file).
inline bool covered(std::uint32_t cp, bool greek) {
  if (cp >= 0x20 && cp < 0x7F) return true;
  if (cp >= 0xA1 && cp <= 0x17F) return cp != 0xAD && cp != 0x149;
  if (cp == 0x401 || cp == 0x451 || (cp >= 0x410 && cp <= 0x44F)) return true;
  if (greek && cp >= 0x386 && cp <= 0x3CE)
    return cp != 0x387 && cp != 0x38B && cp != 0x38D && cp != 0x3A2;
  switch (cp) {
    case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201C: case 0x201D:
    case 0x201E: case 0x2022: case 0x2026: case 0x20AC: case 0x2122:
      return true;
    default:
      return false;
  }
}

// text with what the fonts lack left out; greek: keep Greek letters (the
// toasts' font has them, the player title's does not).
inline std::string drawable(const std::string& text, bool greek) {
  std::string out;
  out.reserve(text.size());
  bool gap = false;  // characters were left out since the last one kept
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t start = i;
    std::uint32_t cp = nextCodepoint(text, &i);
    if (cp < 0x20 || cp == 0x7F || cp == 0xA0) cp = ' ';
    if (cp == ' ') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      gap = false;
      continue;
    }
    if (!covered(cp, greek)) {
      gap = true;
      continue;
    }
    const bool punct = cp < 0x80 && std::strchr(".,!?:;)]}'\"", static_cast<int>(cp)) != nullptr;
    if (gap && punct && !out.empty() && out.back() == ' ')
      out.pop_back();  // "Sonntag ☕:" reads "Sonntag:"
    else if (gap && !punct && !out.empty() && out.back() != ' ')
      out.push_back(' ');
    gap = false;
    out.append(text, start, i - start);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// A channel's name for the screen: its display name, or the login when
// nothing of the name is left (a name all in a script the fonts lack).
inline std::string channelName(const std::string& displayName, const std::string& login, bool greek) {
  std::string name = drawable(displayName, greek);
  return name.empty() ? login : name;
}

}  // namespace evo_twitch_text

#endif  // EVO_TWITCH_TEXT_H
