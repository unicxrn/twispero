#pragma once
#include <string>
#include <vector>

namespace twitch {

// A square grid of QR Code modules. dark[y*size+x] is non-zero for a dark
// (black) module at column x, row y; size == 0 means encoding failed (text
// too long for any QR version), and dark is then empty.
struct QrMatrix {
  int size = 0;
  std::vector<unsigned char> dark;
};

// Encodes text as a QR Code (medium error correction, byte mode - see
// qr.cpp). Never throws.
QrMatrix makeQr(const std::string& text) noexcept;

}  // namespace twitch
