#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace touch_mapping {

constexpr size_t slot_count = 8;
constexpr int max_x = 1919;

enum class Button : uint8_t {
    None, A, B, X, Y, L, R, ZL, ZR, StickL, StickR, Up, Down, Left, Right, Plus, Minus, Count
};
enum class Panel : uint8_t { Front, Rear };
enum class Mode { Off, RearPreset, Custom };

inline constexpr std::array<std::string_view, static_cast<size_t>(Button::Count)> button_names = {
    "none", "a", "b", "x", "y", "l", "r", "zl", "zr", "ls", "rs", "up", "down", "left", "right", "plus", "minus"
};

struct Binding {
    Button button = Button::None;
    Panel panel = Panel::Front;
    int x = 960;
    int y = 544;
};
using Bindings = std::array<Binding, slot_count>;

constexpr int min_y(Panel panel) { return panel == Panel::Rear ? 108 : 0; }
constexpr int max_y(Panel panel) { return panel == Panel::Rear ? 889 : 1087; }

inline Mode parse_mode(std::string_view text) {
    if (text == "custom")
        return Mode::Custom;
    return text == "rear" ? Mode::RearPreset : Mode::Off;
}

inline Bindings rear_preset(bool swap_shoulders = false) {
    Bindings bindings{};
    bindings[0] = { swap_shoulders ? Button::L : Button::ZL, Panel::Rear, 480, 300 };
    bindings[1] = { swap_shoulders ? Button::R : Button::ZR, Panel::Rear, 1440, 300 };
    bindings[2] = { Button::StickL, Panel::Rear, 480, 695 };
    bindings[3] = { Button::StickR, Panel::Rear, 1440, 695 };
    return bindings;
}

inline bool parse_coordinate(std::string_view text, int &value) {
    if (text.empty())
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

inline Bindings parse_bindings(std::string_view text, bool swap_shoulders = false) {
    if (text.empty())
        return rear_preset(swap_shoulders);

    Bindings bindings{};
    for (size_t slot = 0; slot < bindings.size() && !text.empty(); ++slot) {
        const size_t end = text.find(';');
        std::string_view entry = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        std::array<std::string_view, 4> fields{};
        bool valid = true;
        for (size_t i = 0; i < fields.size(); ++i) {
            const size_t comma = entry.find(',');
            if ((i + 1 < fields.size() && comma == std::string_view::npos)
                || (i + 1 == fields.size() && comma != std::string_view::npos)) {
                valid = false;
                break;
            }
            fields[i] = entry.substr(0, comma);
            if (comma != std::string_view::npos)
                entry.remove_prefix(comma + 1);
        }
        if (!valid || (fields[1] != "front" && fields[1] != "rear"))
            continue;
        const auto button = std::find(button_names.begin(), button_names.end(), fields[0]);
        int x = 0, y = 0;
        if (button == button_names.end() || !parse_coordinate(fields[2], x) || !parse_coordinate(fields[3], y))
            continue;
        const Panel panel = fields[1] == "rear" ? Panel::Rear : Panel::Front;
        bindings[slot] = { static_cast<Button>(button - button_names.begin()), panel,
            std::clamp(x, 0, max_x), std::clamp(y, min_y(panel), max_y(panel)) };
    }
    return bindings;
}

inline std::string serialize(const Bindings &bindings) {
    std::string text;
    for (const Binding &binding : bindings) {
        if (!text.empty())
            text += ';';
        text += button_names[static_cast<size_t>(binding.button)];
        text += binding.panel == Panel::Rear ? ",rear," : ",front,";
        text += std::to_string(std::clamp(binding.x, 0, max_x)) + ',';
        text += std::to_string(std::clamp(binding.y, min_y(binding.panel), max_y(binding.panel)));
    }
    return text;
}

template <typename Mask>
struct Presses {
    uint8_t slots = 0;
    Mask consumed = 0;
};

template <typename Mask>
inline Presses<Mask> pressed_bindings(const Bindings &bindings, Mask held,
    const std::array<Mask, static_cast<size_t>(Button::Count)> &buttons) {
    Presses<Mask> pressed;
    for (size_t i = 0; i < bindings.size(); ++i) {
        const Mask button = buttons[static_cast<size_t>(bindings[i].button)];
        if (button && (held & button)) {
            pressed.slots |= static_cast<uint8_t>(1u << i);
            pressed.consumed |= button;
        }
    }
    return pressed;
}

class Contacts {
    std::array<int, slot_count> ids = { -1, -1, -1, -1, -1, -1, -1, -1 };
    int next_id = 64;

public:
    template <typename Emit>
    void update(const Bindings &bindings, uint8_t pressed, uint8_t sampled_panels, Emit emit) {
        for (size_t i = 0; i < bindings.size(); ++i) {
            const Binding &binding = bindings[i];
            if (!(pressed & (1u << i)) || binding.button == Button::None ||
                !(sampled_panels & (1u << static_cast<unsigned>(binding.panel)))) {
                ids[i] = -1;
                continue;
            }
            // Physical contacts use 0..63; mapped contacts use 64..127.
            if (ids[i] < 0) {
                while (std::find(ids.begin(), ids.end(), next_id) != ids.end())
                    next_id = 64 + ((next_id + 1) & 63);
                ids[i] = next_id;
                next_id = 64 + ((next_id + 1) & 63);
            }
            if (!emit(binding, static_cast<uint8_t>(ids[i])))
                ids[i] = -1;
        }
    }
};

} // namespace touch_mapping
