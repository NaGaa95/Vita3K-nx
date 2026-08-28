// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// OpenSSL's library build does not use syslog on Horizon.  Some generated
// feature probes still include the header, so provide the standard constants
// and harmless inline fallbacks instead of carrying host headers into the
// cross-build.
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7

#define LOG_PID 0x01
#define LOG_CONS 0x02
#define LOG_NDELAY 0x08
#define LOG_DAEMON (3 << 3)

static inline void openlog(const char *ident, int option, int facility) {
    (void)ident;
    (void)option;
    (void)facility;
}

static inline void syslog(int priority, const char *format, ...) {
    (void)priority;
    (void)format;
}

static inline void closelog(void) {
}

