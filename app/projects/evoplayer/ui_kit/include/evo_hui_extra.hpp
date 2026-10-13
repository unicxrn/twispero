/*
 * evo_hui_extra.hpp - EVO's on-screen keyboard on ps5-homebrew-ui.
 */
#ifndef EVO_HUI_EXTRA_HPP
#define EVO_HUI_EXTRA_HPP

#include "evo_rmlui_bridge.h"
#include "evo_hui_screens.hpp"

#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <string>
#include <vector>

namespace evo::kit
{

class KeyboardOverlay
{
  public:
    void set(const evo_keyboard_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    bool visible_ = false;
    bool native_only_ = false;
    std::string title_;
    std::string text_;
    std::string mode_label_;
    std::vector<std::string> rows_[4];
    std::vector<std::string> action_labels_;
    int len_ = 0;
    int max_len_ = 0;
    int focus_row_ = 0;
    int focus_col_ = 0;
    bool show_caret_ = false;

    hui::tween::Spring enter_scale_;
    hui::tween::Spring enter_fade_;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_EXTRA_HPP */
