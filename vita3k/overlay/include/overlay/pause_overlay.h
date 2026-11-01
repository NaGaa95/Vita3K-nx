#pragma once
#ifdef __SWITCH__
// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <overlay/controls.h>
#include <overlay/overlay.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace overlay {

// One row of the Switch quick menu's generic list screen. The frontend owns all
// semantics; this is only what has to be drawn.
struct list_row {
    std::string primary;
    std::string secondary; ///< optional second line, drawn smaller
    std::string value; ///< optional right-aligned value
    std::string icon_path; ///< optional host path to a PNG
    bool dimmed = false;
};

struct pause_overlay : public overlay {
    static constexpr int k_menu_entries = 8;
    static constexpr int k_list_visible_rows = 5;

    pause_overlay();

    compiled_resource get_compiled() override;

    // Switch frontend quick menu. The regular pause card remains the default
    // for desktop/Android and for lifecycle pauses on Switch.
    void set_switch_menu(bool enabled, int selected = 0,
        bool lsfg_available = false, bool lsfg_enabled = false,
        bool virtual_mouse = false);
    void set_switch_menu_selection(int selected);
    void set_switch_menu_lsfg(bool available, bool enabled);
    void set_switch_menu_virtual_mouse(bool enabled);
    // Only the colour: a game with no cheat file greys the entry out, like frame generation.
    void set_switch_menu_cheats(bool available);

    // Generic list screen, used for the trophy browser and the in-game settings.
    // Passing an empty row vector returns to the quick menu.
    void set_list(const std::string &title, const std::string &subtitle,
        std::vector<list_row> rows, int selected);
    void set_list_selection(int selected);
    // Drawn on the title row, so it stays put while the list scrolls. Cleared by set_list().
    void set_list_status(const std::string &text);
    void close_list();

private:
    struct list_slot {
        rounded_rect highlight;
        image_view icon;
        label primary;
        label secondary;
        label value;
        bool icon_bound = false;
    };

    rounded_rect m_bg_dimmer;
    rounded_rect m_card;
    rounded_rect m_selection;
    label m_title_label;
    label m_subtitle_label;
    std::array<label, k_menu_entries> m_menu_labels;

    std::array<list_slot, k_list_visible_rows> m_list_slots;
    // Decoded icons live here for as long as this overlay does. The overlay
    // renderers key their GPU texture cache on the image_info address and read
    // its pixels on the render thread outside our lock, so an entry must never
    // be freed or reloaded in place while the list is on screen.
    std::unordered_map<std::string, std::unique_ptr<image_info>> m_icon_cache;
    std::vector<list_row> m_list_rows;
    label m_list_empty_label;
    label m_list_status;
    std::string m_list_status_text;
    rounded_rect m_scroll_track;
    rounded_rect m_scroll_thumb;

    mutable std::mutex m_mutex;
    bool m_switch_menu = false;
    bool m_list_mode = false;
    bool m_lsfg_available = false;
    bool m_virtual_mouse = false;
    bool m_lsfg_enabled = false;
    int m_selected = 0;
    int m_list_selected = 0;
    int m_list_top = 0;

    void layout();
    void layout_list();
    void refresh_elements();
    void rebuild_list_slots();
    void clamp_list_scroll();
    int scrolled_top(int selected, int top) const;
    // Decodes the icons for a window of rows. Reads the SD card, so it runs on
    // the frontend thread before the render lock is taken.
    void preload_icons(const std::vector<list_row> &rows, int top);
    const image_info *cached_icon(const std::string &path) const;

    static constexpr uint16_t k_card_w = 280;
    static constexpr uint16_t k_card_h = 72;
    static constexpr uint16_t k_card_radius = 8;
    static constexpr uint16_t k_menu_card_w = 360;
    static constexpr int16_t k_menu_first_item_y = 54;
    static constexpr int16_t k_menu_item_stride = 38;
    static constexpr uint16_t k_menu_card_h =
        k_menu_first_item_y + k_menu_entries * k_menu_item_stride + 48;
    static constexpr uint16_t k_list_card_w = 860;
    static constexpr uint16_t k_list_card_h = 476;
    static constexpr uint16_t k_list_row_h = 72;
    static constexpr size_t k_icon_cache_max = 256;
};

} // namespace overlay

#else
// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <app/session_controller.h>
#include <dialog/state.h>
#include <overlay/controls.h>
#include <overlay/overlay.h>
#include <overlay/user_interface.h>

#include <vector>

namespace overlay {

enum pause_overlay_button_value : int {
    PAUSE_BUTTON_RESUME,
    PAUSE_BUTTON_EXIT,
};

struct pause_overlay_button {
    rounded_rect bg;
    label text;
    pause_overlay_button_value value;
};

struct pause_overlay : public user_interface {
    explicit pause_overlay(app::AppSessionController &session_controller);

    compiled_resource get_compiled() override;
    bool poll_dialog(DialogState &dialog, int date_format, int sys_button);
    void on_button_pressed(pad_button button_press, bool is_auto_repeat) override;
    void on_touch_pressed(float vx, float vy) override;

private:
    app::AppSessionController &m_session_controller;
    rounded_rect m_bg_dimmer;
    rounded_rect m_card;
    label m_title;
    std::vector<pause_overlay_button> m_buttons;
    std::atomic<pause_overlay_button_value> m_selected_button = PAUSE_BUTTON_RESUME;
    std::atomic<int> m_sys_button = 0;
    float m_focus_pulse_time = 0.f;

    void layout();
    bool is_confirm_button(pad_button button) const;
    bool is_cancel_button(pad_button button) const;
    void draw_focus_ring(compiled_resource &res) const;

    static constexpr uint16_t k_card_w = 360;
    static constexpr uint16_t k_card_h = 182;
    static constexpr uint16_t k_card_radius = 8;
    static constexpr uint16_t k_title_font_size = 20;
    static constexpr uint16_t k_button_w = 280;
    static constexpr uint16_t k_button_h = 46;
    static constexpr uint16_t k_button_gap = 12;
    static constexpr uint16_t k_button_radius = 10;
    static constexpr uint16_t k_font_size = 16;
    static constexpr int16_t k_text_voffset = -3;

    static constexpr color4f k_color_bg_dim{ 0.f, 0.f, 0.f, 0.47f };
    static constexpr color4f k_color_card_bg{ 0.145f, 0.157f, 0.176f, 0.95f };
    static constexpr color4f k_color_btn_normal{ 1.f, 1.f, 1.f, 0.078f };
    static constexpr color4f k_color_btn_selected{ 1.f, 1.f, 1.f, 0.14f };
    static constexpr color4f k_color_text{ 1.f, 1.f, 1.f, 1.f };
    static constexpr color4f k_color_focus_ring{ 0.180f, 0.765f, 0.878f, 1.f };
    static constexpr uint8_t k_focus_ring_thickness = 2;
};

} // namespace overlay

#endif
