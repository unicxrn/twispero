/*
 * evo_hui_misc.hpp - EVO's miscellaneous screens on ps5-homebrew-ui.
 */
#ifndef EVO_HUI_MISC_HPP
#define EVO_HUI_MISC_HPP

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

class DialogOverlay
{
  public:
    void set(const evo_rmlui_dialog_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    struct Action
    {
        std::string icon_path;
        std::string label;
        bool is_primary = false;
    };

    std::string eyebrow_;
    std::string title_;
    std::string detail_;
    double progress_pct_ = -1.0;
    int focused_action_ = -1;
    std::vector<Action> actions_;

    hui::tween::Spring enter_scale_;
    hui::tween::Spring enter_fade_;
};

class ToastOverlay
{
  public:
    void set(const evo_rmlui_toast_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string title_;
    std::string message_;
    int kind_ = 0;
    bool visible_ = false;
    int alpha_ = 0;
    int slide_ = 0;
    float bottom_target_ = 990.0f; /* the card's lower edge (evo_rmlui_toast_params_t.bottom) */
    float bottom_ = 990.0f;
    bool bottom_snapped_ = false;  /* a toast that just came up starts where it belongs */
};

/* About: the app, its version and title id, the license notice and the
 * credits (include/evo_about_text.hpp). Nothing on it takes the cursor;
 * Circle goes back to Settings. */
class AboutScreen
{
  public:
    void set(const evo_rmlui_about_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    std::string app_name_;
    std::string version_;
    std::string title_id_;
    bool rail_focused_ = false;
    float age_ = 0.0f;
    /* The paragraphs, wrapped again only when the fonts change. */
    struct Lines
    {
        const void *fonts = nullptr;
        std::vector<std::vector<std::string>> license;
        std::vector<std::string> source;
    };
    mutable Lines lines_;
};

class ClosedScreen
{
  public:
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;
};

} // namespace evo::kit

#endif /* EVO_HUI_MISC_HPP */
