// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <sys/socket.h>

// Horizon has no AF_UNIX transport, but OpenSSL keeps sockaddr_un in its BIO
// address union even when no Unix-domain socket is opened.  Match the common
// POSIX layout so those library sources can be cross-compiled for newlib.
struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

