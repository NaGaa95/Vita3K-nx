// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Vita3K's game compatibility database, consumed directly by the launcher.
// The emulator's vita3k/compat library needs miniz + pugixml + spdlog, none of
// which the launcher links, so the format is reimplemented here on top of the
// vendored tinyxml2 and the zlib already pulled in for covers.

#pragma once

#include <atomic>
#include <string>

// Mirrors CompatibilityState in vita3k/compat/include/compat/state.h.
enum CompatState {
  COMPAT_UNKNOWN = -1,
  COMPAT_NOTHING = 0,
  COMPAT_BOOTABLE,
  COMPAT_INTRO,
  COMPAT_MENU,
  COMPAT_INGAME_LESS,
  COMPAT_INGAME_MORE,
  COMPAT_PLAYABLE,
};

enum {
  COMPATDB_OK = 0,
  COMPATDB_UP_TO_DATE,
  COMPATDB_NO_NET,
  COMPATDB_BAD_DATA,
  COMPATDB_WRITE_FAILED,
  COMPATDB_CANCELLED,
};

// Reads <cacheDir>/app_compat_db.xml into memory. Safe to call when absent.
bool compatdb_load(const std::string &cacheDir);

// Checks GitHub for a newer release and installs it. Returns COMPATDB_UP_TO_DATE
// when the local copy already matches. Blocking; run it off the UI thread.
int compatdb_update(const std::string &cacheDir, const std::atomic_bool *cancel);

bool compatdb_loaded();
int compatdb_count();
const std::string &compatdb_updated_at();

int compatdb_state(const std::string &titleId);
const char *compatdb_state_label(int state);
void compatdb_state_color(int state, unsigned char rgb[3]);
