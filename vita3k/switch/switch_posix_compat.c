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

// POSIX functions referenced by third-party libraries (OpenSSL, FFmpeg) that
// devkitA64/newlib does not implement on Horizon. Provided here as sane stubs so
// the .nro links; Horizon has no multi-user model or POSIX signal masking.

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#include <switch.h>

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

// OpenSSL's getrandom seed backend probes getentropy as a weak symbol. Horizon
// exposes a real cryptographically secure RNG service, so bridge it here rather
// than letting TLS/key generation run without automatic reseeding. libnx's
// service guard makes the short initialise/use/exit sequence thread-safe.
int getentropy(void *buffer, size_t length) {
    if (length == 0)
        return 0;
    if (buffer == NULL && length != 0) {
        errno = EFAULT;
        return -1;
    }

    const Result init_rc = csrngInitialize();
    if (R_FAILED(init_rc)) {
        errno = EIO;
        return -1;
    }

    const Result random_rc = csrngGetRandomBytes(buffer, length);
    csrngExit();
    if (R_FAILED(random_rc)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how;
    (void)set;
    if (oldset)
        *oldset = 0;
    return 0;
}

long sysconf(int name) {
    switch (name) {
#ifdef _SC_NPROCESSORS_ONLN
    case _SC_NPROCESSORS_ONLN:
#endif
#ifdef _SC_NPROCESSORS_CONF
    case _SC_NPROCESSORS_CONF:
#endif
        return 3; // Switch: 3 CPU cores available to applications.
#ifdef _SC_PAGESIZE
    case _SC_PAGESIZE:
#endif
        return 0x1000;
    default:
        errno = EINVAL;
        return -1;
    }
}
