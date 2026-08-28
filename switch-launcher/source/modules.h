// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Optional Vita modules the launcher can fetch from GitHub. These are not part
// of the firmware: SceShaccCg is the runtime shader compiler some titles look
// for, and kubridge and FdFix are the two taiHEN plugins homebrew ports expect
// to already be on the system.

#pragma once

#include <atomic>
#include <string>

enum {
  MODULE_OK = 0,
  MODULE_UP_TO_DATE,
  MODULE_NO_NET,
  MODULE_NO_RELEASE,
  MODULE_BAD_DATA,
  MODULE_WRITE_FAILED,
  MODULE_CANCELLED,
};

struct ModuleEntry {
  const char *name;
  const char *description;
  const char *repo; // "owner/name" on github.com
  const char *asset; // the file to install, named the same at both ends
  // Set when the module is committed to the repository rather than published as
  // a release: the file is taken from this branch and its version is the commit
  // that last touched it. Left null, the latest release's asset is used.
  const char *branch;
  const char *directory; // absolute install directory
  // taiHEN section to register the plugin under, or nullptr for a plain file.
  // A kernel plugin that is not listed in config.txt is never loaded, so an
  // install that only copied the file would report success and do nothing.
  const char *taiSection;
};

int modules_count();
const ModuleEntry &modules_entry(int index);

// Whether the module's file is present, and the release tag recorded when the
// launcher installed it (empty for a file put there by hand).
bool modules_installed(int index);
const char *modules_installed_tag(int index);
void modules_refresh();

// Resolves the latest release, downloads the asset and installs it. Blocking;
// the caller drives the UI between progress callbacks.
int modules_install(int index, const std::atomic_bool *cancel, std::string &error);

const char *modules_error_text(int result);
