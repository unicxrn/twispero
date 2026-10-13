#include "twitch/qr.hpp"

#include <cstddef>
#include <cstdint>

#include "qrcodegen/qrcodegen.hpp"

namespace twitch {

QrMatrix makeQr(const std::string& text) noexcept {
  try {
    // Byte mode, forced: qrcodegen::QrCode::encodeText() would pick
    // alphanumeric mode for an all-uppercase device code, which is more
    // compact but behaves differently across inputs. encodeBinary() always
    // uses byte mode, so every text we encode goes through one code path.
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeBinary(bytes, qrcodegen::QrCode::Ecc::MEDIUM);

    QrMatrix out;
    out.size = qr.getSize();
    out.dark.resize(static_cast<std::size_t>(out.size) * static_cast<std::size_t>(out.size));
    for (int y = 0; y < out.size; ++y)
      for (int x = 0; x < out.size; ++x)
        out.dark[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.size) + static_cast<std::size_t>(x)] =
            qr.getModule(x, y) ? 1 : 0;
    return out;
  } catch (...) {
    return QrMatrix{};
  }
}

}  // namespace twitch
