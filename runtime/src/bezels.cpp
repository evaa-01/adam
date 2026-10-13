// Game artwork as decoration, two "targets" with the same picker and adjustments (user's ideas, 2026-10-12):
// - Bezels: in the black bars beside the 4:3 picture while the game runs (settings tab "Bezels", data\bezels.json).
// - Launcher background: the whole window behind the launcher (the first screen; its "Background" entry opens the
//   settings tab "Background", data\launcher_background.json). It replaced the fixed D02END20 picture (the default).
// All of the game's named full-screen images that have a palette (93, listed in bezel_images.inc by
// scripts\make_bezel_list.py; the same ones scripts\export_named_images.py exports in colour) are decoded from the
// player's stored ROM (launcher_art.cpp; nothing from the ROM is in the exe) as soon as the UI exists and a ROM is stored,
// and queued as UI images. Each target's tab shows them as clickable previews (plus Off) under pinned controls: a
// brightness slider, a black & white toggle and one control for position and size (d-pad / left stick move, right
// stick scales).
//
// How bezels are drawn: RecompFrontend's UI is drawn over the game, so the bezel can't be "behind" it. Instead one UI
// context that takes no input (captures_input / captures_mouse off, so it doesn't hold the game like a menu: main.cpp
// and the menu-pause patch only count contexts that capture input) holds two panels exactly over the bars. The picture
// repeats endlessly in both directions (tiles; bezel_layout.h has the math): each panel holds one image per piece of a
// tile that falls into it, each showing only its part of the picture (RmlUi img "rect" = source rectangle).
// The bars exist while the renderer's aspect ratio is Original (main.cpp check_aspect_ratio: everywhere except mission
// gameplay with widescreen on) and the window is wider than 4:3; otherwise the panels are hidden.
// The context is shown once (when the game starts) and stays shown: a context shown later is pulled in front of the menus.
// The launcher background is one panel covering the window, in the launcher's own background container (updated from
// the launcher's per-frame callback, main.cpp on_launcher_update, on the UI thread).
//
// Settings storage: per target a config tab (create_config_tab: RecompFrontend loads/saves <id>.json) that is hidden;
// the visible tab is drawn here and writes that config.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "SDL.h"
#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "elements/ui_slider.h"
#include "elements/ui_toggle.h"

#include "bezel_layout.h"
#include "launcher_art.h"

extern SDL_Window* window; // main.cpp

namespace {
    // Image number (asset tables, see launcher_art.cpp) and the image's own name, in ROM order.
    const std::vector<std::pair<uint32_t, std::string>> bezel_images = {
#include "bezel_images.inc"
    };

    // One decoration with its own settings. Option ids of the bezels are the ones bezels.json already uses.
    struct Target {
        std::string config_id;      // <config_id>.json, hidden tab
        std::string picker_id;      // the visible tab
        std::string tab_name;
        std::string image_option, brightness_option, tile_option, position_option, position_y_option, desaturate_option;
        const char* default_image;
        const char* off_text;       // what "Off" means, for the tab's hint when no ROM is loaded yet
        // Picker buttons while the tab is open, with the value each one selects (0 = Off). Rebuilt each time it opens.
        std::vector<std::pair<uint32_t, recompui::Button*>> picker_buttons;
        // Slider / position changes are saved once they have been still for a moment (they fire on every step).
        std::atomic<bool> save_pending = false;
        std::atomic<uint64_t> save_due = 0;
    };

    Target bezels_target{ "bezels", "bezel_picker", "Bezels", "bezel", "bezel_brightness", "bezel_tile", "bezel_position",
        "bezel_position_y", "bezel_desaturate", "D03END00", "black bars" };
    Target launcher_target{ "launcher_background", "background_picker", "Background", "image", "brightness", "tile",
        "position_x", "position_y", "desaturate", "D02END20", "the launcher's default look" };

    // An image whose "rect" attribute (the part of the source picture it shows, in source pixels) can be set: recompui's
    // Element::set_attribute is protected.
    class PartImage : public recompui::Image {
    public:
        using recompui::Image::Image;
        void set_source_rect(double x, double y, double w, double h) {
            char text[128];
            snprintf(text, sizeof(text), "%.3f %.3f %.3f %.3f", x, y, w, h);
            set_attribute("rect", text);
        }
    };

    struct Panel {
        recompui::Element* box = nullptr;
        std::vector<PartImage*> images; // one per segment (bezel_layout.h), created as needed; unused ones hidden
    };

    // Last applied layout (only touch the UI when something changed).
    struct Applied {
        int w = -1, h = -1, choice = -1;
        double brightness = -1, tile = -1, pos_x = -1, pos_y = -1;
        bool visible = false, grey = false;
        bool operator==(const Applied&) const = default;
    };

    bool decode_tried = false;                 // main thread only
    std::atomic<bool> pictures_ready = false;  // set once `pictures` is complete (read by the UI thread too)
    std::vector<adam::launcher_art::Picture> pictures;

    bool overlay_created = false;
    recompui::ContextId overlay = recompui::ContextId::null();
    Panel left_panel, right_panel;
    Applied bezels_applied;

    Panel launcher_panel;                      // UI thread only
    Applied launcher_applied;

    recomp::config::Config& config(Target& t) {
        return recompui::config::get_config(t.config_id);
    }

    uint32_t chosen(Target& t) {
        return std::get<uint32_t>(config(t).get_option_value(t.image_option));
    }

    double number(Target& t, const std::string& option) {
        return std::get<double>(config(t).get_option_value(option));
    }

    void set_number(Target& t, const std::string& option, double value) {
        config(t).set_option_value(option, value);
        t.save_due = SDL_GetTicks64() + 500;
        t.save_pending = true;
    }

    bool desaturated(Target& t) {
        return std::get<bool>(config(t).get_option_value(t.desaturate_option));
    }

    bool usable(uint32_t choice) {
        return pictures_ready && choice >= 1 && choice <= pictures.size() && !pictures[choice - 1].rgba.empty();
    }

    std::string image_src(size_t i) {
        return "adam_bezel_" + std::to_string(i);
    }

    // Black and white copy of picture i, made and queued the first time it's needed ("adam_bezel_<i>_bw"). Called from the
    // main thread (bezels) and the UI thread (launcher).
    std::mutex grey_mutex;
    std::vector<bool> grey_queued;
    std::string grey_src(size_t i) {
        std::string src = image_src(i) + "_bw";
        std::lock_guard lock{ grey_mutex };
        if (grey_queued.size() < pictures.size()) {
            grey_queued.resize(pictures.size(), false);
        }
        if (!grey_queued[i]) {
            grey_queued[i] = true;
            std::vector<char> grey = pictures[i].rgba;
            for (size_t p = 0; p + 3 < grey.size(); p += 4) {
                double luma = 0.299 * uint8_t(grey[p]) + 0.587 * uint8_t(grey[p + 1]) + 0.114 * uint8_t(grey[p + 2]);
                grey[p] = grey[p + 1] = grey[p + 2] = char(uint8_t(std::lround(luma)));
            }
            recompui::queue_image_from_bytes_rgba32(src, grey, pictures[i].width, pictures[i].height);
        }
        return src;
    }

    bool ui_ready() {
        // The game counts as started before RT64 and the UI exist (crash: queue_image before RecompFrontend's ui_state).
        // The settings menu is the last thing the UI setup creates (ui_state.cpp create_menus), so it means "ready".
        try {
            recompui::config::get_config_modal();
            return true;
        } catch (...) {
            return false;
        }
    }

    void decode_images() {
        pictures = adam::launcher_art::load_images(bezel_images);
        size_t count = 0;
        for (size_t i = 0; i < pictures.size(); i++) {
            if (!pictures[i].rgba.empty()) {
                recompui::queue_image_from_bytes_rgba32(image_src(i), pictures[i].rgba, pictures[i].width, pictures[i].height);
                count++;
            }
        }
        pictures_ready = true;
        printf("[bezels] decoded %zu of %zu images from the stored ROM\n", count, pictures.size());
    }

    // A panel is a box holding the images of the tile pieces that fall into it. (First version: the full-size image inside
    // an overflow-hidden box. RmlUi didn't clip it, and the picture covered the whole window.)
    void make_panel(recompui::ContextId ctx, Panel& panel, recompui::Element* parent) {
        panel.box = ctx.create_element<recompui::Container>(parent, recompui::FlexDirection::Row, recompui::JustifyContent::FlexStart);
        panel.box->set_position(recompui::Position::Absolute);
        panel.box->set_top(0);
        panel.box->set_height(100.0f, recompui::Unit::Percent);
        panel.box->set_display(recompui::Display::None);
    }

    void create_overlay() {
        overlay = recompui::create_context();
        overlay.set_captures_input(false);
        overlay.set_captures_mouse(false);
        overlay.open();
        recompui::Element* root = overlay.get_root_element();
        root->set_position(recompui::Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_width(100.0f, recompui::Unit::Percent);
        root->set_height(100.0f, recompui::Unit::Percent);
        make_panel(overlay, left_panel, root);
        make_panel(overlay, right_panel, root);
        overlay.close();
        recompui::show_context(overlay, "");
    }

    // Places one panel over window columns [x, x + bar) of a w x h window (ctx open). Layout: bezel_layout.h.
    void place(recompui::ContextId ctx, Panel& panel, double x, double bar, double w, double h, size_t picture_index, const Applied& a) {
        const auto& picture = pictures[picture_index];
        std::string src = a.grey ? grey_src(picture_index) : image_src(picture_index);
        adam::bezels::layout::Panel layout =
            adam::bezels::layout::place(x, bar, w, h, picture.width, picture.height, a.pos_x, a.pos_y, a.tile);
        panel.box->set_display(recompui::Display::Block);
        // What's under it is black (bars) or dark (launcher): lower opacity = darker. Measured: RmlUi's result came out as
        // opacity squared (50% gave a quarter of the brightness), so the square root makes the slider linear.
        panel.box->set_opacity(float(std::sqrt(a.brightness / 100.0)));
        panel.box->set_left(float(x / w * 100.0), recompui::Unit::Percent);
        panel.box->set_width(float(bar / w * 100.0), recompui::Unit::Percent);
        while (panel.images.size() < layout.segments.size()) {
            PartImage* image = ctx.create_element<PartImage>(panel.box, src);
            image->set_position(recompui::Position::Absolute);
            panel.images.push_back(image);
        }
        for (size_t i = 0; i < panel.images.size(); i++) {
            PartImage* image = panel.images[i];
            if (i >= layout.segments.size()) {
                image->set_display(recompui::Display::None);
                continue;
            }
            const auto& s = layout.segments[i];
            image->set_display(recompui::Display::Block);
            image->set_src(src);
            image->set_left(float(s.left * 100.0), recompui::Unit::Percent);
            image->set_top(float(s.top * 100.0), recompui::Unit::Percent);
            image->set_width(float(s.width * 100.0), recompui::Unit::Percent);
            image->set_height(float(s.height * 100.0), recompui::Unit::Percent);
            image->set_source_rect(s.src_x, s.src_y, s.src_w, s.src_h);
        }
    }

    Applied current(Target& t, int w, int h) {
        Applied a;
        a.w = w;
        a.h = h;
        a.choice = int(chosen(t));
        a.brightness = number(t, t.brightness_option);
        a.tile = number(t, t.tile_option);
        a.pos_x = number(t, t.position_option);
        a.pos_y = number(t, t.position_y_option);
        a.grey = desaturated(t);
        return a;
    }

    void save_if_due(Target& t) {
        if (t.save_pending && SDL_GetTicks64() >= t.save_due) {
            t.save_pending = false;
            config(t).save_config();
        }
    }

    // The selected preview gets a thick primary-colour border, the others a thin dim one.
    void mark_selection(Target& t) {
        uint32_t choice = chosen(t);
        for (auto& [value, button] : t.picker_buttons) {
            bool selected = value == choice;
            button->set_border_width(selected ? 4.0f : 1.0f);
            button->set_border_color(selected ? recompui::theme::color::Primary : recompui::theme::color::TextDim,
                selected ? 255 : 80);
        }
    }

    void select(Target& t, uint32_t choice) {
        config(t).set_option_value(t.image_option, choice);
        config(t).save_config();
        mark_selection(t);
    }

    // A labelled slider bound to one of the number options (label above the slider, in its own column).
    void add_slider(Target& t, recompui::ContextId ctx, recompui::Element* parent, const std::string& title, const std::string& option, double min) {
        auto* column = ctx.create_element<recompui::Container>(parent, recompui::FlexDirection::Column, recompui::JustifyContent::FlexStart);
        column->set_margin_right(48.0f);
        column->set_flex_shrink(0.0f);
        auto* label = ctx.create_element<recompui::Label>(column, title, recompui::LabelStyle::Normal);
        label->set_flex_shrink(0.0f);
        auto* slider = ctx.create_element<recompui::Slider>(column, recompui::SliderType::Percent);
        slider->set_min_value(min);
        slider->set_max_value(100.0);
        slider->set_step_value(1.0);
        slider->set_value(number(t, option));
        slider->set_width(250.0f); // half the first width (user): quicker to drag across
        slider->set_flex_shrink(0.0f);
        Target* target = &t;
        slider->add_value_changed_callback([target, option](double value) { set_number(*target, option, value); });
    }

    // Position and size control (one button, user 2026-10-12: it replaced the tile size slider): press it (A / Enter /
    // click) to start; then the d-pad, left stick or arrow keys move the picture in x and y (one step = 1% of a tile; it
    // wraps around endlessly) and the right stick up / down (or + / - / Page Up / Page Down) makes the tiles bigger /
    // smaller; press it again (or leave it) to finish. While active it takes the direction keys before the menu's focus
    // navigation sees them (handle_navigation_event + StopPropagation). The right stick isn't turned into menu keys, so it
    // is read from SDL directly on every UI update while active.
    class MovePad : public recompui::Element {
        Target& t;
        bool moving = false;
        uint64_t last_tick = 0;
        void update_text() {
            char text[200];
            snprintf(text, sizeof(text),
                moving ? "Adjusting: D-pad / left stick = move, right stick up/down = size (X %.0f  Y %.0f  size %.0f%%) - press to finish"
                       : "Position & size (X %.0f  Y %.0f  size %.0f%%) - press to adjust with D-pad / sticks",
                number(t, t.position_option), number(t, t.position_y_option), number(t, t.tile_option));
            set_text(text);
            set_border_color(moving ? recompui::theme::color::Primary : recompui::theme::color::TextDim, moving ? 255 : 120);
        }
        static double wrap(double v) {
            v = std::fmod(v, 100.0);
            return v < 0 ? v + 100.0 : v;
        }
        void change_size(double delta) {
            double size = std::clamp(number(t, t.tile_option) + delta, 10.0, 100.0);
            if (size != number(t, t.tile_option)) {
                set_number(t, t.tile_option, size);
                update_text();
            }
        }
        // Strongest right stick Y over all pads (-1 = up .. 1 = down), 0 inside a small dead zone.
        static double right_stick_y() {
            double strongest = 0;
            for (int i = 0; i < SDL_NumJoysticks(); i++) {
                SDL_GameController* pad = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
                if (pad == nullptr) {
                    continue;
                }
                double y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0;
                if (std::fabs(y) > std::fabs(strongest)) {
                    strongest = y;
                }
            }
            return std::fabs(strongest) < 0.25 ? 0.0 : strongest;
        }
    protected:
        void process_event(const recompui::Event& e) override {
            switch (e.type) {
            case recompui::EventType::Click:
                moving = !moving;
                last_tick = SDL_GetTicks64();
                update_text();
                if (moving) {
                    queue_update();
                }
                break;
            case recompui::EventType::Focus:
                if (!std::get<recompui::EventFocus>(e.variant).active && moving) {
                    moving = false;
                    update_text();
                }
                set_background_color(recompui::theme::color::PrimaryA20,
                    std::get<recompui::EventFocus>(e.variant).active ? 255 : 0);
                break;
            case recompui::EventType::Update:
                if (moving) {
                    // Right stick: up = bigger, full tilt = 60% of the range per second.
                    uint64_t now = SDL_GetTicks64();
                    double seconds = std::min(0.1, (now - last_tick) / 1000.0);
                    last_tick = now;
                    double y = right_stick_y();
                    if (y != 0.0) {
                        change_size(-y * 60.0 * seconds);
                    }
                    queue_update();
                }
                break;
            default:
                break;
            }
        }
        bool handle_navigation_event(Rml::Event& event) override {
            if (!moving || event.GetId() != Rml::EventId::Keydown) {
                return false;
            }
            int key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
            double dx = 0, dy = 0;
            switch (key) {
                case Rml::Input::KI_LEFT: dx = -1; break;
                case Rml::Input::KI_RIGHT: dx = 1; break;
                case Rml::Input::KI_UP: dy = -1; break;
                case Rml::Input::KI_DOWN: dy = 1; break;
                case Rml::Input::KI_OEM_PLUS: case Rml::Input::KI_ADD: case Rml::Input::KI_PRIOR:
                    change_size(5.0);
                    event.StopPropagation();
                    return true;
                case Rml::Input::KI_OEM_MINUS: case Rml::Input::KI_SUBTRACT: case Rml::Input::KI_NEXT:
                    change_size(-5.0);
                    event.StopPropagation();
                    return true;
                default: return false;
            }
            // Picture moves the way the input points: right = picture right (pos_x up), down = picture down (pos_y up).
            set_number(t, t.position_option, wrap(number(t, t.position_option) + dx));
            set_number(t, t.position_y_option, wrap(number(t, t.position_y_option) + dy));
            update_text();
            event.StopPropagation();
            return true;
        }
        std::string_view get_type_name() override { return "BezelMovePad"; }
    public:
        MovePad(recompui::ResourceId rid, recompui::Element* parent, Target* target)
            : Element(rid, parent, recompui::Events(recompui::EventType::Click, recompui::EventType::Focus, recompui::EventType::Navigate,
                recompui::EventType::Update), "button", true), t(*target) {
            enable_focus();
            set_cursor(recompui::Cursor::Pointer);
            set_margin_top(20.0f);
            set_padding(12.0f);
            set_border_width(2.0f);
            set_border_radius(8.0f);
            set_font_size(20.0f);
            set_flex_shrink(0.0f);
            update_text();
        }
    };

    // A preview button that scrolls itself into view when it gets the focus, so the grid follows d-pad / stick navigation
    // (the scroll area doesn't do that by itself: the selection moved below the visible part, user report).
    class PreviewButton : public recompui::Button {
    protected:
        void process_event(const recompui::Event& e) override {
            recompui::Button::process_event(e);
            if (e.type == recompui::EventType::Focus && std::get<recompui::EventFocus>(e.variant).active) {
                scroll_into_view(true);
            }
        }
    public:
        using recompui::Button::Button;
    };

    // Contents of a picker tab: the controls pinned at the top (brightness, black & white, position & size; user: easy
    // access), below them the grid of previews (rows of 5, "Off" first) in its own scroll area.
    void create_picker(Target& t, recompui::ContextId ctx, recompui::Element* parent) {
        t.picker_buttons.clear();
        Target* target = &t;
        auto* page = ctx.create_element<recompui::Container>(parent, recompui::FlexDirection::Column, recompui::JustifyContent::FlexStart);
        page->set_width(100.0f, recompui::Unit::Percent);
        page->set_height(100.0f, recompui::Unit::Percent);
        page->set_padding(24.0f);

        auto* controls = ctx.create_element<recompui::Container>(page, recompui::FlexDirection::Column, recompui::JustifyContent::FlexStart);
        controls->set_flex_shrink(0.0f);
        controls->set_margin_bottom(16.0f);
        auto* sliders = ctx.create_element<recompui::Container>(controls, recompui::FlexDirection::Row, recompui::JustifyContent::FlexStart);
        sliders->set_flex_shrink(0.0f);
        add_slider(t, ctx, sliders, "Brightness (0 = off)", t.brightness_option, 0.0);
        {
            auto* column = ctx.create_element<recompui::Container>(sliders, recompui::FlexDirection::Column, recompui::JustifyContent::FlexStart);
            column->set_flex_shrink(0.0f);
            ctx.create_element<recompui::Label>(column, "Black & white", recompui::LabelStyle::Normal);
            auto* toggle = ctx.create_element<recompui::Toggle>(column, recompui::ToggleSize::Medium); // 64x36: half the default 144x72 (user)
            toggle->set_checked(desaturated(t));
            toggle->add_checked_callback([target](bool checked) {
                config(*target).set_option_value(target->desaturate_option, checked);
                target->save_due = SDL_GetTicks64();
                target->save_pending = true;
            });
        }
        ctx.create_element<MovePad>(controls, target);

        // Everything below the controls scrolls (flex-grow 1 + min-height 0 so it takes the rest of the page and no more).
        auto* scroll = ctx.create_element<recompui::Container>(page, recompui::FlexDirection::Column, recompui::JustifyContent::FlexStart);
        scroll->set_flex_grow(1.0f);
        scroll->set_min_height(0.0f);
        scroll->set_overflow_y(recompui::Overflow::Auto);
        if (!pictures_ready || pictures.empty()) {
            ctx.create_element<recompui::Label>(scroll,
                "The images come from your ROM: load it in the launcher (Load ROM) to see them here.",
                recompui::LabelStyle::Normal);
        }

        // Explicit rows of 5 for controller navigation: the grid is a GridCol container of GridRow containers, so left/right
        // moves within a row and up/down keeps the column (with one wrapping container, "left" didn't reach the neighbour).
        constexpr size_t per_row = 5;
        constexpr float button_w = 200.0f, button_h = 104.0f, inner_w = 192.0f, inner_h = 96.0f;
        auto* grid = ctx.create_element<recompui::Container>(scroll, recompui::FlexDirection::Column, recompui::JustifyContent::FlexStart);
        grid->set_as_navigation_container(recompui::NavigationType::GridCol);
        // A flex column squeezes its children to fit: without this the 19 rows were squeezed and the sliders (then below
        // the grid) were drawn over them (OCR read thumbnail text and slider labels at the same height).
        grid->set_flex_shrink(0.0f);
        recompui::Element* row = nullptr;
        size_t in_row = per_row;

        auto add_button = [&](uint32_t value, const std::string& text) {
            if (in_row == per_row) {
                row = ctx.create_element<recompui::Container>(grid, recompui::FlexDirection::Row, recompui::JustifyContent::FlexStart);
                row->set_as_navigation_container(recompui::NavigationType::GridRow);
                row->set_flex_shrink(0.0f);
                in_row = 0;
            }
            in_row++;
            auto* button = ctx.create_element<PreviewButton>(row, text, recompui::ButtonStyle::Basic, recompui::ButtonSize::Medium);
            button->set_width(button_w);
            // Button fixes min/max height to its ButtonSize: override all three for the previews.
            button->set_height(button_h);
            button->set_min_height(button_h);
            button->set_max_height(button_h);
            button->set_padding(4.0f);
            button->set_margin(6.0f);
            button->set_border_radius(8.0f);
            button->add_pressed_callback([target, value]() { select(*target, value); });
            t.picker_buttons.push_back({ value, button });
            return button;
        };
        add_button(0, "Off");
        for (size_t i = 0; pictures_ready && i < pictures.size(); i++) {
            if (pictures[i].rgba.empty()) {
                continue; // not in this ROM: no preview (its value just isn't offered)
            }
            auto* button = add_button(uint32_t(i + 1), "");
            auto* image = ctx.create_element<recompui::Image>(button, image_src(i));
            // Fit inside the button, keeping the picture's shape (sizes range from 88x154 to 320x148).
            double aspect = double(pictures[i].width) / pictures[i].height;
            if (aspect >= inner_w / inner_h) {
                image->set_width(inner_w);
                image->set_height(float(inner_w / aspect));
            } else {
                image->set_height(inner_h);
                image->set_width(float(inner_h * aspect));
            }
        }
        mark_selection(t);
        // (set_as_primary_focus on the chosen button was tried so Down from the tab row lands on the current choice: no
        // effect, Down lands on the button under the tab.)
    }

    void create_tabs(Target& t) {
        recomp::config::Config& storage = recompui::config::create_config_tab(t.tab_name, t.config_id, false);
        auto* options = new std::vector<recomp::config::ConfigOptionEnumOption>(); // kept for the config's lifetime
        options->push_back({ 0u, "Off", "Off" });
        uint32_t default_value = 0;
        for (size_t i = 0; i < bezel_images.size(); i++) {
            const std::string& name = bezel_images[i].second;
            options->push_back({ uint32_t(i + 1), name, name });
            if (name == t.default_image) {
                default_value = uint32_t(i + 1);
            }
        }
        storage.add_enum_option(t.image_option, "Picture", "", *options, default_value);
        storage.add_number_option(t.brightness_option, "Brightness", "", 0, 100, 1, 0, true, 100);
        storage.add_number_option(t.tile_option, "Tile size", "", 10, 100, 1, 0, true, 100);
        storage.add_number_option(t.position_option, "Position X", "", 0, 100, 1, 0, true, 50);
        storage.add_number_option(t.position_y_option, "Position Y", "", 0, 100, 1, 0, true, 50);
        storage.add_bool_option(t.desaturate_option, "Black & white", "", false);
        recompui::config::set_tab_visible(t.config_id, false);
        Target* target = &t;
        recompui::config::create_tab(t.tab_name, t.picker_id,
            [target](recompui::ContextId ctx, recompui::Element* parent) { create_picker(*target, ctx, parent); });
    }
}

namespace adam::bezels {
    // Settings tabs (config.cpp init_config, before finalize): "Bezels", then "Background" (the launcher's).
    void create_config_tab() {
        create_tabs(bezels_target);
        create_tabs(launcher_target);
    }

    // The launcher's "Background" entry (main.cpp on_launcher_init): the settings menu on the Background tab.
    void open_launcher_background_settings() {
        recompui::config::set_tab(launcher_target.picker_id);
        recompui::hide_all_contexts();
        recompui::config::open();
    }

    // The launcher background, every frame while the launcher shows (UI thread, launcher context open). `container` is the
    // launcher's background container (behind its menu). Off or no ROM yet: nothing, the launcher's default look.
    void update_launcher(recompui::Element* container) {
        if (window == nullptr) {
            return;
        }
        save_if_due(launcher_target);
        recompui::ContextId ctx = recompui::get_current_context();
        if (launcher_panel.box == nullptr) {
            make_panel(ctx, launcher_panel, container);
        }
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        Applied now = current(launcher_target, w, h);
        now.visible = usable(uint32_t(now.choice)) && w > 0 && h > 0 && now.brightness > 0.0;
        if (now == launcher_applied) {
            return;
        }
        launcher_applied = now;
        if (now.visible) {
            place(ctx, launcher_panel, 0.0, w, w, h, size_t(now.choice - 1), now);
        } else {
            launcher_panel.box->set_display(recompui::Display::None);
        }
    }

    // Every tick on the main thread (main.cpp update_gfx).
    void update() {
        if (window == nullptr || !ui_ready()) {
            return;
        }
        save_if_due(bezels_target);
        save_if_due(launcher_target);
        if (!decode_tried) {
            std::error_code ec;
            if (!std::filesystem::exists(recomp::get_config_path() / "adam.z64", ec)) {
                return; // no stored ROM yet (first run): try again after Load ROM
            }
            decode_tried = true;
            decode_images();
        }
        if (!ultramodern::is_game_started()) {
            return;
        }
        if (!overlay_created) {
            overlay_created = true;
            create_overlay();
        }
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        Applied now = current(bezels_target, w, h);
        bool original = ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Original;
        double bar = (w - h * 4.0 / 3.0) / 2.0;
        now.visible = original && usable(uint32_t(now.choice)) && bar >= 2.0 && h > 0 && now.brightness > 0.0;
        if (now == bezels_applied) {
            return;
        }
        bezels_applied = now;

        overlay.open();
        if (now.visible) {
            place(overlay, left_panel, 0.0, bar, w, h, size_t(now.choice - 1), now);
            place(overlay, right_panel, w - bar, bar, w, h, size_t(now.choice - 1), now);
        } else {
            left_panel.box->set_display(recompui::Display::None);
            right_panel.box->set_display(recompui::Display::None);
        }
        overlay.close();
    }
}
