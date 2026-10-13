/*
 * evo_about_text.hpp - what the About page says (Settings > System > About).
 *
 * GPLv3 section 5(d) asks an interactive program to show "Appropriate Legal
 * Notices" (section 0): a copyright notice, that there is no warranty, that
 * the program may be shared under the license, and how to see the license.
 * This file holds those lines and the credits. The kit page
 * (ui_kit/src/evo_hui_misc.cpp, AboutScreen) and the RmlUi page
 * (assets/rml/about.rml, filled in by ui_rml/src/evo_rmlui_app.cpp) both read
 * it, so a change here shows on both.
 *
 * The strings are UTF-8 and stay inside what the kit's fonts cover
 * (Latin-1, the copyright sign, the middle dot).
 */
#ifndef EVO_ABOUT_TEXT_HPP
#define EVO_ABOUT_TEXT_HPP

/* EVO_APP_NAME and EVO_TITLE_ID: the titleName and titleId in
 * sce_sys/param.json, written into this generated header by
 * tools/gen_identity.py. Rename the app there, not here. */
#include "evo_identity.h"

namespace evo::about
{

inline constexpr const char *kAppName = EVO_APP_NAME;
inline constexpr const char *kTitleId = EVO_TITLE_ID;
inline constexpr const char *kBasedOn = "Based on EVO Player by sainsaji (GPL-3.0-or-later).";
inline constexpr const char *kNotAffiliated = "Not affiliated with Twitch or Sony.";

/* The license notice, one paragraph per entry. */
inline constexpr const char *kLicense[] = {
    "Copyright \xC2\xA9 2026 unicxrn.",
    "This app is free software: you can share and change it under the GNU General Public License, "
    "version 3 or any later version. It comes with no warranty, as far as the law allows.",
    "The full license text ships with the app as assets/LICENSE.txt and is also at "
    "gnu.org/licenses/gpl-3.0.html. The other licenses are in assets/licenses, assets/fonts and "
    "assets/hui/fonts.",
};
inline constexpr int kLicenseCount = static_cast<int>(sizeof kLicense / sizeof kLicense[0]);

/* Where the source is. The line under it is EVO Player's (kEvoProject), so
 * this one reads the same way. */
inline constexpr const char *kSource = "Twispero: github.com/unicxrn/twispero";

/* What the app is built from, and under which license: the projects it is
 * based on, and every library with code in the linked binary (checked with nm
 * against output/app/.build/llvm-pie.elf; libsamplerate, libass, HarfBuzz,
 * FriBidi, xz and zstd are on the link line but nothing pulls them in).
 * qrcodegen (MIT) is in the binary too but has no row: adding it to the MIT
 * row runs off the screen. assets/licenses/qrcodegen.txt carries its notice,
 * and the license paragraph above points there. */
struct Credit
{
    const char *what;
    const char *who;     /* "" when the project has no single author to name */
    const char *license;
};
inline constexpr Credit kCredits[] = {
    {"EVO Player", "sainsaji", "GPL-3.0-or-later"},
    {"ProsperoPlayer", "KINGDKAK", "GPL-3.0-or-later"},
    {"ps5-homebrew-ui", "BlackBearReloaded", "GPL-3.0-or-later"},
    {"PS5 Payload SDK", "John T\xC3\xB6rnblom", "GPL-3.0-or-later"},
    {"FFmpeg", "", "LGPL/GPL, as built"},
    {"GNU libiconv", "", "LGPL-2.1-or-later"},
    {"OpenSSL", "", "Apache-2.0"},
    {"FreeType", "", "FreeType License"},
    {"RmlUi, nlohmann/json, libxml2", "", "MIT"},
    {"dav1d", "", "BSD-2-Clause"},
    {"SDL2, zlib", "", "zlib License"},
    {"libpng", "", "libpng License"},
    {"bzip2", "", "bzip2 License"},
    {"stb_image", "", "public domain"},
    {"Inter, Montserrat, Lato", "", "SIL Open Font License 1.1"},
    {"DejaVu fonts", "", "Bitstream Vera License"},
};
inline constexpr int kCreditCount = static_cast<int>(sizeof kCredits / sizeof kCredits[0]);
/* assets/rml/about.rml has one row per credit, about-credit-0 to -15; add
 * rows there before adding credits here. */
static_assert(kCreditCount <= 16, "about.rml has 16 credit rows");

/* EVO's own project page, kept from its About screen. */
inline constexpr const char *kEvoProject = "EVO Player: github.com/sainsaji/EVO-PLAYER-PS5";

} // namespace evo::about

#endif /* EVO_ABOUT_TEXT_HPP */
