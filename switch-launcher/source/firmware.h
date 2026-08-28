/* firmware.h -- PS Vita firmware presence check + downloader for the Switch
 * launcher. Uses the same libcurl + libnx-TLS backend as griddb.cpp (console CA
 * store); sockets + curl are brought up once by the launcher in main().
 */
#pragma once

#include <string>
#include <vector>

// True if the PS Vita firmware appears installed on the emulated fs
// (sdmc:/switch/vita3k/vita/): the font dir sa0/data/font/pvf has a regular
// file and vs0/sys/external holds at least one regular .suprx module.
bool firmware_is_installed(void);

// The three firmware PUP filenames firmware_download_all writes into install/,
// in download order (pre-install, firmware, system data/font). Exposed so the local-install
// flow (and its UI) can name the exact files the user must supply.
extern const char *const FIRMWARE_PUP_NAMES[3];

// True if all three firmware PUPs are already present in
// sdmc:/switch/vita3k/install/ and each carries a valid SCEUF header (reuses the
// same validation as a finished download). When it returns false and `missing`
// is non-null, `missing` is filled (cleared first) with the names of the files
// that are absent or invalid, so the caller can tell the user what to copy.
bool firmware_local_files_present(std::vector<std::string> *missing);

// Download the three firmware PUPs Vita3K needs into sdmc:/switch/vita3k/install/
// under the distinct names the emulator installer expects:
//   PSP2UPDAT_preinst.PUP  (pre-install firmware)
//   PSVUPDAT.PUP           (firmware)
//   PSP2UPDAT_font.PUP     (firmware system data / font)
// Each file streams to a temp name and is renamed into place only on success, so a
// partial transfer never looks complete. `progress` (may be null) receives the
// 1-based file index, the total, percent, a status label, speed in MB/s and the
// byte counts. Returns 0 on success, else the 1-based index of the file that failed.
int firmware_download_all(void (*progress)(int idx, int total, int pct, const char *label,
                                           double mbps, long long dlnow, long long dltotal));
