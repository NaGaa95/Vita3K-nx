// Vita3K emulator project
// Copyright (C) 2026 Vita3K team

#include <util/switch_vibration.h>

#ifdef __SWITCH__

#include <util/log.h>

#include <switch.h>
#ifdef BIT
#undef BIT
#endif
#ifdef BITL
#undef BITL
#endif

#include <algorithm>
#include <array>
#include <mutex>

namespace {
struct VibrationState {
    std::array<HidVibrationDeviceHandle, 2> handles{};
    HidNpadIdType id = HidNpadIdType_No1;
    HidNpadStyleTag style = static_cast<HidNpadStyleTag>(0);
    int count = 0;
};

std::mutex vibration_mutex;
VibrationState vibration_state;

HidNpadStyleTag select_style(u32 styles) {
    constexpr std::array candidates = {
        HidNpadStyleTag_NpadFullKey,
        HidNpadStyleTag_NpadJoyDual,
        HidNpadStyleTag_NpadHandheld,
        HidNpadStyleTag_NpadJoyLeft,
        HidNpadStyleTag_NpadJoyRight,
    };
    for (const auto style : candidates)
        if ((styles & style) != 0)
            return style;
    return static_cast<HidNpadStyleTag>(0);
}

void stop_locked() {
    if (vibration_state.count == 0)
        return;
    std::array<HidVibrationValue, 2> values{};
    for (auto &value : values) {
        value.freq_low = 160.0f;
        value.freq_high = 320.0f;
    }
    hidSendVibrationValues(vibration_state.handles.data(), values.data(), vibration_state.count);
}

bool refresh_devices_locked() {
    HidNpadIdType id = HidNpadIdType_No1;
    HidNpadStyleTag style = select_style(hidGetNpadStyleSet(id));
    if (style == static_cast<HidNpadStyleTag>(0)) {
        id = HidNpadIdType_Handheld;
        style = select_style(hidGetNpadStyleSet(id));
    }

    if (style == static_cast<HidNpadStyleTag>(0)) {
        stop_locked();
        vibration_state.count = 0;
        vibration_state.style = style;
        return false;
    }

    if (vibration_state.count != 0 && vibration_state.id == id && vibration_state.style == style)
        return true;

    stop_locked();
    vibration_state = {};
    vibration_state.id = id;
    vibration_state.style = style;

    const bool supports_two_handles = style == HidNpadStyleTag_NpadFullKey
        || style == HidNpadStyleTag_NpadHandheld || style == HidNpadStyleTag_NpadJoyDual;
    int count = supports_two_handles ? 2 : 1;
    Result rc = hidInitializeVibrationDevices(vibration_state.handles.data(), count, id, style);
    if (R_FAILED(rc) && count == 2) {
        count = 1;
        rc = hidInitializeVibrationDevices(vibration_state.handles.data(), count, id, style);
    }
    if (R_FAILED(rc)) {
        LOG_WARN("Switch vibration device initialization failed (id={}, style=0x{:x}, rc={})",
            static_cast<int>(id), static_cast<u32>(style), log_hex(rc));
        vibration_state.count = 0;
        return false;
    }

    vibration_state.count = count;
    LOG_INFO("Switch vibration ready (id={}, style=0x{:x}, devices={})",
        static_cast<int>(id), static_cast<u32>(style), count);
    return true;
}
} // namespace

bool switch_set_vibration(uint8_t small_motor, uint8_t large_motor) {
    std::lock_guard lock(vibration_mutex);
    if (!refresh_devices_locked())
        return false;

    // Vita's large motor maps naturally to HD Rumble's low band; its small
    // motor maps to the high band. Keep amplitudes inside libnx's [0, 1] range.
    HidVibrationValue value{};
    value.amp_low = std::clamp(static_cast<float>(large_motor) / 255.0f, 0.0f, 1.0f);
    value.freq_low = 160.0f;
    value.amp_high = std::clamp(static_cast<float>(small_motor) / 255.0f, 0.0f, 1.0f);
    value.freq_high = 320.0f;

    std::array<HidVibrationValue, 2> values{ value, value };
    const Result rc = hidSendVibrationValues(
        vibration_state.handles.data(), values.data(), vibration_state.count);
    if (R_FAILED(rc)) {
        vibration_state.count = 0;
        return false;
    }
    return true;
}

void switch_stop_vibration() {
    std::lock_guard lock(vibration_mutex);
    stop_locked();
}

#else

bool switch_set_vibration(uint8_t, uint8_t) {
    return false;
}

void switch_stop_vibration() {
}

#endif
