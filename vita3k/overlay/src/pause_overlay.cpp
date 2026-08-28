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

#include <lang/state.h>
#include <overlay/font.h>
#include <overlay/pause_overlay.h>
#include <overlay/types.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace overlay {

namespace {
constexpr color4f k_text = { 0.86f, 0.86f, 0.86f, 1.f };
constexpr color4f k_text_dim = { 0.627f, 0.627f, 0.627f, 1.f };
constexpr color4f k_text_disabled = { 0.42f, 0.42f, 0.42f, 1.f };
constexpr color4f k_value = { 1.f, 0.84f, 0.47f, 1.f };
constexpr color4f k_transparent = { 0.f, 0.f, 0.f, 0.f };
} // namespace

pause_overlay::pause_overlay() {
    visible = true;

    m_bg_dimmer.set_pos(0, 0);
    m_bg_dimmer.set_size(virtual_width, virtual_height);
    m_bg_dimmer.back_color = { 0.f, 0.f, 0.f, 0.5f };
    m_bg_dimmer.border_radius = 0;

    m_card.back_color = { 0.059f, 0.059f, 0.059f, 0.667f };
    m_card.border_radius = k_card_radius;

    m_selection.back_color = { 0.20f, 0.42f, 0.62f, 0.92f };
    m_selection.border_radius = 5;

    m_title_label.fore_color = { 0.863f, 0.863f, 0.863f, 1.f };
    m_title_label.back_color = k_transparent;
    m_title_label.set_font(default_font_name, 16);
    m_title_label.set_text(lang::get(lang::str::emulation_paused));
    m_title_label.align_text(overlay_element::center);

    m_subtitle_label.fore_color = k_text_dim;
    m_subtitle_label.back_color = k_transparent;
    m_subtitle_label.set_font(default_font_name, 12);
    m_subtitle_label.set_text(lang::get(lang::str::press_ps_to_continue));
    m_subtitle_label.align_text(overlay_element::center);

    constexpr std::array<const char *, k_menu_entries> menu_text = {
        "Resume game",
        "Frame generation: unavailable",
        "Virtual mouse: Off",
        "Trophies",
        "Settings",
        "Return to launcher",
        "Exit emulator",
    };
    for (size_t i = 0; i < m_menu_labels.size(); ++i) {
        auto &entry = m_menu_labels[i];
        entry.fore_color = k_text;
        entry.back_color = k_transparent;
        entry.set_font(default_font_name, 14);
        entry.set_text(menu_text[i]);
        entry.align_text(overlay_element::center);
    }

    for (auto &slot : m_list_slots) {
        slot.highlight.back_color = { 0.20f, 0.42f, 0.62f, 0.92f };
        slot.highlight.border_radius = 5;
        slot.icon.back_color = k_transparent;
        slot.primary.back_color = k_transparent;
        slot.primary.fore_color = k_text;
        slot.primary.set_font(default_font_name, 15);
        slot.secondary.back_color = k_transparent;
        slot.secondary.fore_color = k_text_dim;
        slot.secondary.set_font(default_font_name, 11);
        slot.value.back_color = k_transparent;
        slot.value.fore_color = k_value;
        slot.value.set_font(default_font_name, 14);
        slot.value.align_text(overlay_element::right);
    }

    m_list_empty_label.back_color = k_transparent;
    m_list_empty_label.fore_color = k_text_dim;
    m_list_empty_label.set_font(default_font_name, 14);
    m_list_empty_label.align_text(overlay_element::center);

    m_scroll_track.back_color = { 0.16f, 0.17f, 0.21f, 0.85f };
    m_scroll_track.border_radius = 2;
    m_scroll_thumb.back_color = { 0.45f, 0.78f, 1.f, 0.9f };
    m_scroll_thumb.border_radius = 2;

    layout();
}

void pause_overlay::layout() {
    if (m_list_mode) {
        layout_list();
        return;
    }

    const uint16_t card_w = m_switch_menu ? k_menu_card_w : k_card_w;
    const uint16_t card_h = m_switch_menu ? k_menu_card_h : k_card_h;
    const int16_t card_x = static_cast<int16_t>((virtual_width - card_w) / 2);
    const int16_t card_y = static_cast<int16_t>((virtual_height - card_h) / 2);

    m_card.set_pos(card_x, card_y);
    m_card.set_size(card_w, card_h);

    m_title_label.set_pos(card_x, static_cast<int16_t>(card_y + (m_switch_menu ? 12 : 10)));
    m_title_label.set_size(card_w, 26);

    if (!m_switch_menu) {
        m_subtitle_label.set_pos(card_x, static_cast<int16_t>(card_y + 38));
        m_subtitle_label.set_size(card_w, 22);
        return;
    }

    constexpr int16_t first_item_y = k_menu_first_item_y;
    constexpr int16_t item_stride = k_menu_item_stride;
    m_selection.set_pos(static_cast<int16_t>(card_x + 24),
        static_cast<int16_t>(card_y + first_item_y + m_selected * item_stride));
    m_selection.set_size(static_cast<uint16_t>(card_w - 48), 32);
    for (size_t i = 0; i < m_menu_labels.size(); ++i) {
        m_menu_labels[i].set_pos(static_cast<int16_t>(card_x + 24),
            static_cast<int16_t>(card_y + first_item_y + static_cast<int>(i) * item_stride + 3));
        m_menu_labels[i].set_size(static_cast<uint16_t>(card_w - 48), 26);
    }
    m_subtitle_label.set_pos(card_x, static_cast<int16_t>(card_y + card_h - 35));
    m_subtitle_label.set_size(card_w, 22);
}

int pause_overlay::scrolled_top(int selected, int top) const {
    const int count = static_cast<int>(m_list_rows.size());
    if (count <= 0)
        return 0;
    if (selected < top)
        top = selected;
    if (selected >= top + k_list_visible_rows)
        top = selected - k_list_visible_rows + 1;
    return std::clamp(top, 0, std::max(0, count - k_list_visible_rows));
}

void pause_overlay::clamp_list_scroll() {
    const int count = static_cast<int>(m_list_rows.size());
    if (count <= 0) {
        m_list_selected = 0;
        m_list_top = 0;
        return;
    }
    m_list_selected = std::clamp(m_list_selected, 0, count - 1);
    m_list_top = scrolled_top(m_list_selected, m_list_top);
}

const image_info *pause_overlay::cached_icon(const std::string &path) const {
    const auto entry = m_icon_cache.find(path);
    return entry == m_icon_cache.end() ? nullptr : entry->second.get();
}

void pause_overlay::preload_icons(const std::vector<list_row> &rows, int top) {
    const int count = static_cast<int>(rows.size());
    for (int i = 0; i < k_list_visible_rows; ++i) {
        const int index = top + i;
        if (index < 0 || index >= count)
            continue;
        const std::string &path = rows[static_cast<size_t>(index)].icon_path;
        if (path.empty() || m_icon_cache.contains(path))
            continue;
        if (m_icon_cache.size() >= k_icon_cache_max)
            return;

        // A trophy conf directory can be missing an individual TROPnnn.PNG. Cache
        // the failure too so a broken path is not re-read on every scroll step.
        auto image = std::make_unique<image_info>(path);
        if (!image->get_data() || image->w <= 0)
            image.reset();
        m_icon_cache.emplace(path, std::move(image));
    }
}

void pause_overlay::layout_list() {
    constexpr int16_t card_x = static_cast<int16_t>((virtual_width - k_list_card_w) / 2);
    constexpr int16_t card_y = static_cast<int16_t>((virtual_height - k_list_card_h) / 2);
    constexpr int16_t first_row_y = card_y + 52;
    constexpr uint16_t row_w = k_list_card_w - 40;

    m_card.set_pos(card_x, card_y);
    m_card.set_size(k_list_card_w, k_list_card_h);

    m_title_label.set_pos(card_x, static_cast<int16_t>(card_y + 14));
    m_title_label.set_size(k_list_card_w, 26);
    m_subtitle_label.set_pos(card_x, static_cast<int16_t>(card_y + k_list_card_h - 32));
    m_subtitle_label.set_size(k_list_card_w, 22);

    m_list_empty_label.set_pos(card_x, static_cast<int16_t>(card_y + k_list_card_h / 2 - 12));
    m_list_empty_label.set_size(k_list_card_w, 26);

    const int count = static_cast<int>(m_list_rows.size());
    const bool scrollable = count > k_list_visible_rows;
    const uint16_t content_w = static_cast<uint16_t>(scrollable ? row_w - 14 : row_w);

    for (int i = 0; i < k_list_visible_rows; ++i) {
        auto &slot = m_list_slots[static_cast<size_t>(i)];
        const int16_t row_y = static_cast<int16_t>(first_row_y + i * k_list_row_h);

        slot.highlight.set_pos(static_cast<int16_t>(card_x + 20), row_y);
        slot.highlight.set_size(content_w, k_list_row_h - 6);

        const bool has_icon = (m_list_top + i) < count
            && !m_list_rows[static_cast<size_t>(m_list_top + i)].icon_path.empty();
        // Both offsets are relative to the highlight, which starts at card_x + 20.
        const uint16_t text_inset = has_icon ? 72 : 14;
        const int16_t text_x = static_cast<int16_t>(card_x + 20 + text_inset);
        const uint16_t text_w = static_cast<uint16_t>(content_w - text_inset - 12);

        slot.icon.set_pos(static_cast<int16_t>(card_x + 30), static_cast<int16_t>(row_y + 4));
        slot.icon.set_size(58, 58);

        slot.primary.set_pos(text_x, static_cast<int16_t>(row_y + 10));
        slot.primary.set_size(static_cast<uint16_t>(text_w * 2 / 3), 22);
        slot.secondary.set_pos(text_x, static_cast<int16_t>(row_y + 36));
        slot.secondary.set_size(text_w, 20);
        slot.value.set_pos(text_x, static_cast<int16_t>(row_y + 12));
        slot.value.set_size(text_w, 22);
    }

    const int16_t track_x = static_cast<int16_t>(card_x + k_list_card_w - 30);
    m_scroll_track.set_pos(track_x, first_row_y);
    m_scroll_track.set_size(4, k_list_visible_rows * k_list_row_h - 6);
    if (scrollable) {
        const int track_h = k_list_visible_rows * k_list_row_h - 6;
        const int thumb_h = std::max(24, track_h * k_list_visible_rows / count);
        const int span = std::max(1, count - k_list_visible_rows);
        m_scroll_thumb.set_pos(track_x,
            static_cast<int16_t>(first_row_y + (track_h - thumb_h) * m_list_top / span));
        m_scroll_thumb.set_size(4, static_cast<uint16_t>(thumb_h));
    }
}

// Pushes the currently scrolled window of rows into the fixed slot pool. The
// slots (and their image_info objects) are reused rather than recreated so the
// overlay renderers' texture cache, which is keyed on the image_info address and
// only cleared at teardown, stays bounded.
void pause_overlay::rebuild_list_slots() {
    const int count = static_cast<int>(m_list_rows.size());

    for (int i = 0; i < k_list_visible_rows; ++i) {
        auto &slot = m_list_slots[static_cast<size_t>(i)];
        const int index = m_list_top + i;
        if (index >= count) {
            slot.primary.set_text("");
            slot.secondary.set_text("");
            slot.value.set_text("");
            slot.icon.clear_image();
            slot.icon_bound = false;
            continue;
        }

        const list_row &row = m_list_rows[static_cast<size_t>(index)];
        const bool current = index == m_list_selected;
        slot.primary.set_text(row.primary);
        slot.primary.fore_color = row.dimmed ? k_text_disabled : (current ? k_value : k_text);
        slot.secondary.set_text(row.secondary);
        slot.secondary.fore_color = row.dimmed ? k_text_disabled : k_text_dim;
        slot.value.set_text(row.value);
        slot.value.fore_color = row.dimmed ? k_text_disabled : k_value;

        const image_info *icon = row.icon_path.empty() ? nullptr : cached_icon(row.icon_path);
        slot.icon_bound = icon != nullptr;
        if (icon)
            slot.icon.set_raw_image(icon);
        else
            slot.icon.clear_image();
    }
}

void pause_overlay::refresh_elements() {
    m_card.refresh();
    m_selection.refresh();
    m_title_label.refresh();
    m_subtitle_label.refresh();
    for (auto &entry : m_menu_labels)
        entry.refresh();
    if (!m_list_mode)
        return;
    m_list_empty_label.refresh();
    m_scroll_track.refresh();
    m_scroll_thumb.refresh();
    for (auto &slot : m_list_slots) {
        slot.highlight.refresh();
        slot.icon.refresh();
        slot.primary.refresh();
        slot.secondary.refresh();
        slot.value.refresh();
    }
}

void pause_overlay::set_switch_menu(bool enabled, int selected,
    bool lsfg_available, bool lsfg_enabled, bool virtual_mouse) {
    {
        std::lock_guard lock(m_mutex);
        m_switch_menu = enabled;
        m_list_mode = false;
        m_list_rows.clear();
        m_lsfg_available = lsfg_available;
        m_lsfg_enabled = lsfg_enabled;
        m_virtual_mouse = virtual_mouse;
        m_selected = std::clamp(selected, 0, static_cast<int>(m_menu_labels.size()) - 1);
        if (m_switch_menu) {
            m_title_label.set_text("Vita3K quick menu");
            m_subtitle_label.set_text("A: select    B: resume    L + R + Plus: menu");
            m_menu_labels[1].set_text(m_lsfg_available
                    ? (m_lsfg_enabled ? "Frame generation: On" : "Frame generation: Off")
                    : "Frame generation: unavailable");
            m_menu_labels[1].fore_color = m_lsfg_available ? k_text : k_text_disabled;
            m_menu_labels[2].set_text(m_virtual_mouse ? "Virtual mouse: On" : "Virtual mouse: Off");
        } else {
            m_title_label.set_text(lang::get(lang::str::emulation_paused));
            m_subtitle_label.set_text(lang::get(lang::str::press_ps_to_continue));
        }
        layout();
        refresh_elements();
    }
    overlay::refresh();
}

void pause_overlay::set_switch_menu_lsfg(bool available, bool enabled) {
    {
        std::lock_guard lock(m_mutex);
        m_lsfg_available = available;
        m_lsfg_enabled = enabled;
        m_menu_labels[1].set_text(available
                ? (enabled ? "Frame generation: On" : "Frame generation: Off")
                : "Frame generation: unavailable");
        m_menu_labels[1].fore_color = available ? k_text : k_text_disabled;
        m_menu_labels[1].refresh();
    }
    overlay::refresh();
}

void pause_overlay::set_switch_menu_virtual_mouse(bool enabled) {
    {
        std::lock_guard lock(m_mutex);
        m_virtual_mouse = enabled;
        m_menu_labels[2].set_text(enabled ? "Virtual mouse: On" : "Virtual mouse: Off");
        m_menu_labels[2].refresh();
    }
    overlay::refresh();
}

void pause_overlay::set_switch_menu_selection(int selected) {
    {
        std::lock_guard lock(m_mutex);
        if (!m_switch_menu || m_list_mode)
            return;
        const int next = std::clamp(selected, 0, static_cast<int>(m_menu_labels.size()) - 1);
        if (next == m_selected)
            return;
        m_selected = next;
        layout();
        m_selection.refresh();
    }
    overlay::refresh();
}

void pause_overlay::set_list(const std::string &title, const std::string &subtitle,
    std::vector<list_row> rows, int selected) {
    // m_list_* and m_icon_cache are written only from the frontend thread, so
    // the scroll window can be resolved and its icons decoded here. Reading the
    // SD card under m_mutex would stall the render thread in get_compiled().
    const int count = static_cast<int>(rows.size());
    const int wanted = count > 0 ? std::clamp(selected, 0, count - 1) : 0;
    const int top = std::clamp(std::max(0, wanted - k_list_visible_rows + 1),
        0, std::max(0, count - k_list_visible_rows));
    preload_icons(rows, top);
    {
        std::lock_guard lock(m_mutex);
        m_list_mode = true;
        m_switch_menu = true;
        m_list_rows = std::move(rows);
        m_list_selected = wanted;
        m_list_top = top;
        clamp_list_scroll();
        m_title_label.set_text(title);
        m_subtitle_label.set_text(subtitle);
        m_list_empty_label.set_text("Nothing to show here");
        layout();
        rebuild_list_slots();
        refresh_elements();
    }
    overlay::refresh();
}

void pause_overlay::set_list_selection(int selected) {
    if (!m_list_mode)
        return;
    // Same reasoning as set_list: decode the newly exposed icons before locking.
    const int count = static_cast<int>(m_list_rows.size());
    if (count > 0)
        preload_icons(m_list_rows, scrolled_top(std::clamp(selected, 0, count - 1), m_list_top));
    {
        std::lock_guard lock(m_mutex);
        const int previous_top = m_list_top;
        const int previous = m_list_selected;
        m_list_selected = selected;
        clamp_list_scroll();
        if (m_list_selected == previous && m_list_top == previous_top)
            return;
        layout();
        rebuild_list_slots();
        refresh_elements();
    }
    overlay::refresh();
}

void pause_overlay::close_list() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_list_mode)
            return;
        m_list_mode = false;
        m_list_rows.clear();
        for (auto &slot : m_list_slots) {
            slot.icon.clear_image();
            slot.icon_bound = false;
        }
        m_title_label.set_text("Vita3K quick menu");
        m_subtitle_label.set_text("A: select    B: resume    L + R + Plus: menu");
        layout();
        refresh_elements();
    }
    overlay::refresh();
}

compiled_resource pause_overlay::get_compiled() {
    std::lock_guard lock(m_mutex);
    compiled_resource result;

    if (!visible)
        return result;

    result.add(m_bg_dimmer.get_compiled());
    result.add(m_card.get_compiled());
    result.add(m_title_label.get_compiled());

    if (m_list_mode) {
        const int count = static_cast<int>(m_list_rows.size());
        if (count == 0) {
            result.add(m_list_empty_label.get_compiled());
        } else {
            for (int i = 0; i < k_list_visible_rows; ++i) {
                const int index = m_list_top + i;
                if (index >= count)
                    break;
                auto &slot = m_list_slots[static_cast<size_t>(i)];
                if (index == m_list_selected)
                    result.add(slot.highlight.get_compiled());
                if (slot.icon_bound)
                    result.add(slot.icon.get_compiled());
                result.add(slot.primary.get_compiled());
                if (!m_list_rows[static_cast<size_t>(index)].secondary.empty())
                    result.add(slot.secondary.get_compiled());
                if (!m_list_rows[static_cast<size_t>(index)].value.empty())
                    result.add(slot.value.get_compiled());
            }
            if (count > k_list_visible_rows) {
                result.add(m_scroll_track.get_compiled());
                result.add(m_scroll_thumb.get_compiled());
            }
        }
    } else if (m_switch_menu) {
        result.add(m_selection.get_compiled());
        for (auto &entry : m_menu_labels)
            result.add(entry.get_compiled());
    }

    result.add(m_subtitle_label.get_compiled());

    return result;
}

} // namespace overlay
