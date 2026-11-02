#include <touch/controller_mapping.h>

#include <cassert>
#include <iostream>
#include <set>
#include <vector>

using namespace touch_mapping;

struct Contact {
    Binding binding;
    uint8_t id;
};

int main() {
    assert(parse_mode("off") == Mode::Off);
    assert(parse_mode("rear") == Mode::RearPreset);
    assert(parse_mode("custom") == Mode::Custom);
    assert(parse_mode("invalid") == Mode::Off);
    assert(serialize(parse_bindings("")) == serialize(rear_preset()));
    assert(serialize(parse_bindings("", true)) == serialize(rear_preset(true)));
    assert(rear_preset(true)[0].button == Button::L);
    assert(rear_preset(true)[1].button == Button::R);

    const Bindings disabled{};
    assert(serialize(parse_bindings(serialize(disabled))) == serialize(disabled));
    const auto clamped = parse_bindings("a,front,-1,9999;b,rear,9999,-1");
    assert(clamped[0].x == 0 && clamped[0].y == 1087);
    assert(clamped[1].x == 1919 && clamped[1].y == 108);
    for (const char *invalid : { "a,front,1", "a,front,1,2,3", "a,back,1,2", "unknown,front,1,2",
             "a,front,1x,2", "a,front,,2", "a,front,2147483648,2", "a,front,1,2x" })
        assert(parse_bindings(invalid)[0].button == Button::None);
    auto bindings = parse_bindings("broken;;a,front,1919,1087;b,rear,0,889");
    assert(bindings[0].button == Button::None && bindings[1].button == Button::None);
    assert(bindings[2].button == Button::A && bindings[3].panel == Panel::Rear);
    assert(serialize(parse_bindings(serialize(bindings))) == serialize(bindings));

    std::array<uint64_t, static_cast<size_t>(Button::Count)> buttons{};
    for (size_t i = 1; i < buttons.size(); ++i)
        buttons[i] = uint64_t{ 1 } << i;
    bindings = {};
    bindings[0] = { Button::A, Panel::Front, 100, 200 };
    bindings[7] = { Button::A, Panel::Rear, 1800, 800 };
    const uint64_t a = buttons[static_cast<size_t>(Button::A)];
    const uint64_t b = buttons[static_cast<size_t>(Button::B)];
    const auto pressed = pressed_bindings(bindings, a | b, buttons);
    assert(pressed.slots == 0x81 && pressed.consumed == a);
    assert(((a | b) & ~pressed.consumed) == b);
    assert(pressed_bindings(bindings, uint64_t{}, buttons).slots == 0);

    Contacts contacts;
    std::vector<Contact> reports;
    const auto sample = [&](uint8_t held, uint8_t sampled = 3) {
        reports.clear();
        contacts.update(bindings, held, sampled, [&](const Binding &binding, uint8_t id) {
            reports.push_back({ binding, id });
            return true;
        });
    };
    sample(pressed.slots);
    assert(reports.size() == 2);
    const uint8_t front_id = reports[0].id, rear_id = reports[1].id;
    assert(front_id >= 64 && rear_id >= 64 && front_id != rear_id);
    assert(reports[0].binding.x == 100 && reports[1].binding.y == 800);
    sample(pressed.slots);
    assert(reports[0].id == front_id && reports[1].id == rear_id);
    sample(1);
    assert(reports.size() == 1 && reports[0].id == front_id);
    sample(pressed.slots);
    assert(reports[0].id == front_id && reports[1].id != rear_id);

    for (int cycle = 0; cycle < 200; ++cycle) {
        sample(1);
        sample(pressed.slots);
        assert(reports[0].id == front_id && reports[1].id != front_id);
        assert(reports[1].id >= 64 && reports[1].id < 128);
    }
    sample(pressed.slots, 1);
    assert(reports.size() == 1 && reports[0].binding.panel == Panel::Front);
    sample(pressed.slots, 0);
    assert(reports.empty());
    sample(pressed.slots);
    assert(reports.size() == 2 && reports[0].id != front_id);

    bindings.fill({ Button::ZL, Panel::Rear, 1919, 889 });
    contacts = {};
    std::set<uint8_t> ids;
    contacts.update(bindings, 255, 3, [&](const Binding &, uint8_t id) {
        assert(ids.insert(id).second);
        return true;
    });
    assert(ids.size() == slot_count);

    // Four physical contacts leave four report entries for mapped contacts.
    size_t report_count = 4;
    contacts.update(bindings, 255, 3, [&](const Binding &, uint8_t) {
        if (report_count == 8)
            return false;
        ++report_count;
        return true;
    });
    assert(report_count == 8);
    sample(0);
    assert(reports.empty());
    assert(pressed_bindings(disabled, ~uint64_t{}, buttons).consumed == 0);
    std::cout << "Touch mapping: parsing, button consumption, panel routing, contact lifetime and capacity passed\n";
}
