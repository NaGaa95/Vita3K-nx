// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

namespace Vita3KLauncher::Storage {

struct SmbShare {
    // Stable identifier used to form the devoptab name. It may contain ASCII
    // letters, digits and underscores and is limited to 16 characters.
    std::string id;
    std::string name;
    std::string server;
    std::string share;
    std::string path;
    std::string user;
    std::string password;
    std::string domain;
    bool auto_mount = true;
};

struct Location {
    // A devoptab root/browse path, always with a trailing slash.
    std::string path;
    std::string label;
    // Stable physical/volume identity. `path` and `mount_alias` may change
    // whenever Horizon renumbers umsN: devices.
    std::string id;
    std::string mount_alias;
    std::string physical_id;
    std::string serial_number;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::uint8_t lun = 0;
    std::uint32_t partition = 0;
    std::uint8_t filesystem_type = 0;
    std::uint64_t capacity = 0;
};

struct UsbSnapshot {
    std::uint64_t generation = 0;
    std::vector<Location> locations;
};

using UsbStatusCallback = void (*)(void *user_data);

enum class SmbConnectionState { Disconnected, Connecting, Connected, Reconnecting, Failed };

// SD is always exposed by libnx as sdmc:/. USB locations are populated and
// mounted by libusbhsfs after InitializeUsb().
Location SdLocation();
std::vector<Location> ListLocalLocations();

bool InitializeUsb(std::string *error = nullptr);
std::uint64_t UsbStatusGeneration();
void SetUsbStatusCallback(UsbStatusCallback callback, void *user_data = nullptr);
UsbSnapshot GetUsbSnapshot();
std::vector<Location> ListUsbLocations();
std::string ResolveUsbPath(const std::string &id);
bool SafelyEjectUsb(const std::string &id, std::string *error = nullptr);

// Mounts an SMB share as v3ksmb_<id>:/ through a thread-safe devoptab. The
// launcher must keep libnx sockets initialized for the lifetime of its SMB
// mounts. Existing file and directory descriptors remain valid if the share is
// unmounted; the connection is retired once the last operation releases it.
bool MountSmb(const SmbShare &share, std::string *error = nullptr,
              const std::atomic_bool *cancel = nullptr);
bool UnmountSmb(const std::string &id, std::string *error = nullptr);
bool IsSmbMounted(const std::string &id);
SmbConnectionState GetSmbConnectionState(const std::string &id);
bool ReconnectSmb(const std::string &id, std::string *error = nullptr,
                  const std::atomic_bool *cancel = nullptr);
std::string SmbRootPath(const std::string &id);
std::string SmbBrowsePath(const SmbShare &share);

// Unregisters active SMB devices and stops libusbhsfs. Call after import jobs
// have closed their files; mounts with outstanding descriptors are retained
// until those descriptors close rather than being destroyed underneath them.
void Shutdown();

} // namespace Vita3KLauncher::Storage
