// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef __SWITCH__

#include <dialog/state.h>
#include <emuenv/state.h>
#include <ime/keyboard.h>
#include <ime/state.h>
#include <ime/types.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ime {
namespace {

std::atomic<bool> s_show_requested{ false };

struct KeyboardSnapshot {
    bool dialog = false;
    bool cancelable = true;
    bool multiline = false;
    uint32_t max_length = SCE_IME_MAX_TEXT_LENGTH;
    uint32_t type = SCE_IME_TYPE_DEFAULT;
    std::string title;
    std::string ok_label;
    std::u16string text;
};

bool take_snapshot(EmuEnvState &emuenv, KeyboardSnapshot &snapshot) {
    std::lock_guard<std::recursive_mutex> dialog_lock(emuenv.common_dialog.mutex);
    std::lock_guard<std::mutex> ime_lock(emuenv.ime.mutex);

    snapshot.dialog = emuenv.common_dialog.type == IME_DIALOG
        && emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING;
    if (!snapshot.dialog && !emuenv.ime.state)
        return false;

    snapshot.text = emuenv.ime.str;
    snapshot.ok_label = emuenv.ime.enter_label.empty() ? "Enter" : emuenv.ime.enter_label;
    if (snapshot.dialog) {
        snapshot.title = emuenv.common_dialog.ime.title;
        snapshot.max_length = emuenv.common_dialog.ime.max_length;
        snapshot.multiline = emuenv.common_dialog.ime.multiline;
        snapshot.cancelable = emuenv.common_dialog.ime.cancelable;
    } else {
        snapshot.title = emuenv.current_app_title.empty() ? "Vita3K" : emuenv.current_app_title;
        snapshot.max_length = emuenv.ime.param.maxTextLength;
        snapshot.multiline = (emuenv.ime.param.option & SCE_IME_OPTION_MULTILINE) != 0;
        snapshot.type = emuenv.ime.param.type;
    }

    snapshot.max_length = std::clamp<uint32_t>(snapshot.max_length ? snapshot.max_length : 1,
        1, SCE_IME_MAX_TEXT_LENGTH);
    if (snapshot.text.size() > snapshot.max_length)
        snapshot.text.resize(snapshot.max_length);
    return true;
}

void replace_ime_text(Ime &ime, std::u16string text, uint32_t max_length) {
    if (text.size() > max_length)
        text.resize(max_length);
    ime.str = std::move(text);
    ime.caretIndex = static_cast<uint32_t>(ime.str.size());
    ime.edit_text.editIndex = 0;
    ime.edit_text.editLengthChange = static_cast<int32_t>(ime.str.size());
    ime.edit_text.caretIndex = ime.caretIndex;
    ime.edit_text.preeditIndex = ime.caretIndex;
    ime.edit_text.preeditLength = 0;
}

void finish_dialog(EmuEnvState &emuenv, const std::u16string &text) {
    std::lock_guard<std::recursive_mutex> dialog_lock(emuenv.common_dialog.mutex);
    std::lock_guard<std::mutex> ime_lock(emuenv.ime.mutex);

    replace_ime_text(emuenv.ime, text, emuenv.common_dialog.ime.max_length);
    const size_t copy_len = std::min(emuenv.ime.str.size(),
        static_cast<size_t>(emuenv.common_dialog.ime.max_length));
    if (emuenv.common_dialog.ime.result) {
        std::memcpy(emuenv.common_dialog.ime.result, emuenv.ime.str.data(), copy_len * sizeof(uint16_t));
        emuenv.common_dialog.ime.result[copy_len] = 0;
    }
    const std::string utf8 = string_utils::utf16_to_utf8(emuenv.ime.str);
    std::snprintf(emuenv.common_dialog.ime.text, sizeof(emuenv.common_dialog.ime.text), "%s", utf8.c_str());
    emuenv.common_dialog.ime.status = SCE_IME_DIALOG_BUTTON_ENTER;
    emuenv.common_dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    emuenv.common_dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
}

void cancel_dialog(EmuEnvState &emuenv) {
    std::lock_guard<std::recursive_mutex> dialog_lock(emuenv.common_dialog.mutex);
    emuenv.common_dialog.ime.status = SCE_IME_DIALOG_BUTTON_CLOSE;
    emuenv.common_dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    emuenv.common_dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
}

} // namespace

void set_keyboard_active(bool active) {
    s_show_requested.store(active, std::memory_order_release);
}

void notify_ime_state_changed() {
    // swkbd returns the complete edited string, so there is no inline native UI
    // snapshot to update between sceImeUpdate calls on Horizon.
}

bool has_pending_request() {
    return s_show_requested.load(std::memory_order_acquire);
}

bool process_pending_request(EmuEnvState &emuenv) {
    if (!s_show_requested.exchange(false, std::memory_order_acq_rel))
        return false;

    KeyboardSnapshot snapshot;
    if (!take_snapshot(emuenv, snapshot))
        return false;

    SwkbdConfig keyboard{};
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc)) {
        LOG_ERROR("swkbdCreate failed: 0x{:08x}", rc);
        return true;
    }

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, snapshot.title.c_str());
    swkbdConfigSetOkButtonText(&keyboard, snapshot.ok_label.c_str());
    swkbdConfigSetInitialText(&keyboard, string_utils::utf16_to_utf8(snapshot.text).c_str());
    swkbdConfigSetStringLenMax(&keyboard, snapshot.max_length);
    swkbdConfigSetReturnButtonFlag(&keyboard, snapshot.multiline ? 1 : 0);
    if (snapshot.type == SCE_IME_TYPE_NUMBER || snapshot.type == SCE_IME_TYPE_EXTENDED_NUMBER)
        swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    else if (snapshot.type == SCE_IME_TYPE_BASIC_LATIN || snapshot.type == SCE_IME_TYPE_URL || snapshot.type == SCE_IME_TYPE_MAIL)
        swkbdConfigSetType(&keyboard, SwkbdType_Latin);

    // UTF-8 can use four bytes per UTF-16 code unit. Leave one extra byte for
    // the terminator required by swkbdShow.
    std::vector<char> output(static_cast<size_t>(snapshot.max_length) * 4 + 1, 0);
    rc = swkbdShow(&keyboard, output.data(), output.size());
    swkbdClose(&keyboard);

    if (R_SUCCEEDED(rc)) {
        std::u16string text = string_utils::utf8_to_utf16(output.data());
        if (snapshot.dialog) {
            finish_dialog(emuenv, text);
        } else {
            std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
            if (emuenv.ime.state) {
                replace_ime_text(emuenv.ime, std::move(text), snapshot.max_length);
                emuenv.ime.event_id = SCE_IME_EVENT_UPDATE_TEXT;
                emuenv.ime.queued_event_id = SCE_IME_EVENT_PRESS_ENTER;
            }
        }
    } else if (snapshot.dialog) {
        if (snapshot.cancelable)
            cancel_dialog(emuenv);
        else
            s_show_requested.store(true, std::memory_order_release);
    } else {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        if (emuenv.ime.state) {
            emuenv.ime.event_id = SCE_IME_EVENT_PRESS_CLOSE;
            emuenv.ime.queued_event_id = SCE_IME_EVENT_OPEN;
        }
    }

    LOG_INFO("Switch software keyboard closed ({})", R_SUCCEEDED(rc) ? "accepted" : "cancelled");
    return true;
}

} // namespace ime

#endif // __SWITCH__
