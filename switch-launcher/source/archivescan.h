// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Enough of a zip reader to tell what an archive holds. Only the central
// directory is read, so this stays fast on a multi-gigabyte file.

#pragma once

#include <string>

struct ArchiveContents {
  bool readable = false; ///< the file parsed as a zip at all
  bool app_content = false; ///< sce_sys/param.sfo or theme.xml: installs directly
  bool package = false; ///< a .pkg, which needs a license
  bool license = false; ///< work.bin or .rif shipped with it
};

ArchiveContents archive_inspect(const std::string &path);
