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

struct EmuEnvState;

// On-device installer for the Switch build. Scans "sdmc:/switch/vita3k/install/"
// and installs any firmware (.pup), licenses (work.bin / .rif), packages (.pkg)
// and homebrew archives (.vpk) it finds, then returns. Renders progress on the
// libnx text console (the caller must not hold the Vulkan window). This exists
// because the Switch build has no Qt/Compose GUI to drive install_pup/install_pkg.
void run_switch_installer(EmuEnvState &emuenv);
