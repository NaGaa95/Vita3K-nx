/* firmware.cpp -- PS Vita firmware presence check + downloader. See firmware.h.
 * Mirrors griddb.cpp's libcurl usage (libnx TLS backend, console CA store).
 * curl + sockets are owned by the launcher for its full lifetime, exactly as
 * the updater, SMB importer, and griddb_fetch_cover rely on -- so no re-init. */
#include "firmware.h"

#include <switch.h>          // armGetSystemTick / armTicksToNs for download-speed timing
#include <curl/curl.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef VITA3K_NX_VERSION
#error "VITA3K_NX_VERSION must come from switch-launcher/version.mk"
#endif

// All under sdmc:/switch/vita3k/ (matches main.cpp's DATA_DIR layout).
static const char *FW_INSTALL_DIR = "sdmc:/switch/vita3k/install";
static const char *FW_VITA_DIR    = "sdmc:/switch/vita3k/vita";

// The three PUP filenames written to install/ (single source of truth, shared by
// the download table below and the local-file presence check). Order: pre-install,
// firmware, system data/font -- distinct names so the emulator installs all three.
const char *const FIRMWARE_PUP_NAMES[3] = {
  "PSP2UPDAT_preinst.PUP", "PSVUPDAT.PUP", "PSP2UPDAT_font.PUP"
};

// --- presence check ---------------------------------------------------------
// true if `dir` exists and holds at least one regular file; when `ext` is set
// (e.g. ".suprx"), only files with that extension count.
static bool dirHasFiles(const std::string &dir, const char *ext) {
  DIR *d = opendir(dir.c_str());
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    if (ext) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot || strcasecmp(dot, ext) != 0) continue;
    }
    struct stat info{};
    const std::string path = dir + "/" + e->d_name;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    found = true;
    break;
  }
  closedir(d);
  return found;
}

bool firmware_is_installed(void) {
  // Fonts extract to sa0/data/font/pvf; the system modules to vs0/sys/external.
  const std::string fontDir = std::string(FW_VITA_DIR) + "/sa0/data/font/pvf";
  const std::string extDir  = std::string(FW_VITA_DIR) + "/vs0/sys/external";
  return dirHasFiles(fontDir, nullptr) && dirHasFiles(extDir, ".suprx");
}

// --- download with per-file progress ---------------------------------------
static constexpr std::uint64_t MIN_PUP_BYTES = 1ULL * 1024 * 1024;
static constexpr std::uint64_t MAX_PUP_BYTES = 512ULL * 1024 * 1024;

struct Prog {
  void (*cb)(int, int, int, const char *, double, long long, long long);
  int idx, total, lastPct;
  const char *label;
  std::uint64_t expectedBytes;
  u64 lastTick;         // armGetSystemTick() at the last speed sample (0 = uninitialised)
  long long lastBytes;  // dlnow at the last speed sample
  double mbps;          // most recently computed download speed (MB/s)
};

struct FileSink {
  FILE *file;
  std::uint64_t maximum;
  std::uint64_t written;
  bool failed;
  Sha256Context hash;
};

// curl progress hook: fire on every whole-percent change AND ~twice a second so
// the on-screen speed/size readout stays smooth without spamming the UI. Speed is
// bytes-since-last-sample / elapsed, timed with libnx's monotonic system tick
// (armGetSystemTick/armTicksToNs) -- no wall clock. Returning 0 keeps the transfer.
static int xfer_cb(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
  Prog *pr = (Prog *)p;
  if (dlnow < 0 || static_cast<std::uint64_t>(dlnow) > pr->expectedBytes ||
      (dltotal > 0 && static_cast<std::uint64_t>(dltotal) != pr->expectedBytes))
    return 1;
  int pct = (dltotal > 0) ? (int)(dlnow * 100 / dltotal) : 0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  const u64 now = armGetSystemTick();
  if (pr->lastTick == 0) { pr->lastTick = now; pr->lastBytes = (long long)dlnow; }
  const double dsec = (double)armTicksToNs(now - pr->lastTick) / 1.0e9;
  const bool sample = dsec >= 0.5;      // recompute speed ~2x/sec
  if (sample) {
    pr->mbps = (dsec > 0.0) ? ((double)((long long)dlnow - pr->lastBytes) / dsec) / (1024.0 * 1024.0) : 0.0;
    if (pr->mbps < 0.0) pr->mbps = 0.0; // guard against any counter reset
    pr->lastTick = now;
    pr->lastBytes = (long long)dlnow;
  }
  if (pct != pr->lastPct || sample) {
    pr->lastPct = pct;
    if (pr->cb) pr->cb(pr->idx, pr->total, pct, pr->label, pr->mbps, (long long)dlnow, (long long)dltotal);
  }
  return 0;
}

static size_t write_file_cb(void *ptr, size_t sz, size_t n, void *userp) {
  FileSink *sink = static_cast<FileSink *>(userp);
  if (sz != 0 && n > std::numeric_limits<size_t>::max() / sz) {
    sink->failed = true;
    return 0;
  }
  const size_t bytes = sz * n;
  if (sink->written > sink->maximum || bytes > sink->maximum - sink->written) {
    sink->failed = true;
    return 0;
  }
  const size_t written = fwrite(ptr, 1, bytes, sink->file);
  if (written != 0) {
    sha256ContextUpdate(&sink->hash, ptr, written);
    sink->written += written;
  }
  if (written != bytes) sink->failed = true;
  return written;
}

static int hexDigit(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

static bool parseSha256(const char *text, std::array<u8, SHA256_HASH_SIZE> &hash) {
  if (!text || strlen(text) != SHA256_HASH_SIZE * 2) return false;
  for (size_t index = 0; index < hash.size(); ++index) {
    const int high = hexDigit(text[index * 2]);
    const int low = hexDigit(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    hash[index] = static_cast<u8>((high << 4) | low);
  }
  return true;
}

// A finished file must be a real SCE updater and stay within broad, sane PUP
// bounds. Network downloads additionally require their exact trusted size and
// SHA-256 pin; local imports remain compatible with other official versions.
static bool looksLikePup(const std::string &path) {
  struct stat info{};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) < MIN_PUP_BYTES ||
      static_cast<std::uint64_t>(info.st_size) > MAX_PUP_BYTES)
    return false;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char magic[5] = {0};
  size_t rd = fread(magic, 1, sizeof(magic), f);
  const bool failed = ferror(f) != 0;
  fclose(f);
  bool sceuf = (rd == sizeof(magic) && memcmp(magic, "SCEUF", 5) == 0);
  return !failed && sceuf;
}

static bool regularOrMissing(const std::string &path, bool &exists) {
  struct stat info{};
  if (stat(path.c_str(), &info) == 0) {
    exists = true;
    return S_ISREG(info.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static bool activateDownloadedPup(const std::string &temporary, const std::string &destination,
                                  std::uint64_t expectedBytes) {
  const std::string backup = destination + ".old";
  bool currentExists = false, backupExists = false;
  if (!regularOrMissing(destination, currentExists) || !regularOrMissing(backup, backupExists))
    return false;
  if (!currentExists && backupExists) {
    if (rename(backup.c_str(), destination.c_str()) != 0 ||
        R_FAILED(fsdevCommitDevice("sdmc")))
      return false;
    currentExists = true;
    backupExists = false;
  }
  if (currentExists && backupExists) {
    if (remove(backup.c_str()) != 0 || R_FAILED(fsdevCommitDevice("sdmc")))
      return false;
    backupExists = false;
  }
  const bool hadCurrent = currentExists;
  const auto restoreBackup = [&](bool removeCurrent) {
    const bool removed = !removeCurrent || remove(destination.c_str()) == 0 || errno == ENOENT;
    const bool restored = !hadCurrent || (removed && rename(backup.c_str(), destination.c_str()) == 0);
    const bool committed = R_SUCCEEDED(fsdevCommitDevice("sdmc"));
    return removed && restored && committed;
  };
  if (hadCurrent) {
    if (rename(destination.c_str(), backup.c_str()) != 0 ||
        R_FAILED(fsdevCommitDevice("sdmc"))) {
      (void)restoreBackup(false);
      return false;
    }
  }
  if (rename(temporary.c_str(), destination.c_str()) != 0) {
    (void)restoreBackup(false);
    return false;
  }
  if (R_FAILED(fsdevCommitDevice("sdmc"))) {
    (void)restoreBackup(true);
    return false;
  }
  struct stat activated{};
  if (stat(destination.c_str(), &activated) != 0 || !S_ISREG(activated.st_mode) ||
      activated.st_size < 0 || static_cast<std::uint64_t>(activated.st_size) != expectedBytes ||
      !looksLikePup(destination)) {
    (void)restoreBackup(true);
    return false;
  }
  if (hadCurrent && (remove(backup.c_str()) != 0 ||
      R_FAILED(fsdevCommitDevice("sdmc"))))
    return false;
  return true;
}

// GET `url` -> `destPath`. Streams to <destPath>.part and renames on success so a
// partial/aborted transfer never looks like a complete PUP.
static bool http_download_file(const std::string &url, const std::string &destPath,
                               std::uint64_t expectedBytes, const char *expectedSha256,
                               Prog *pr) {
  if (expectedBytes < MIN_PUP_BYTES || expectedBytes > MAX_PUP_BYTES) return false;
  std::array<u8, SHA256_HASH_SIZE> expectedHash{};
  if (!parseSha256(expectedSha256, expectedHash)) return false;
  const std::string tmp = destPath + ".part";
  remove(tmp.c_str());
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  FileSink sink{f, expectedBytes, 0, false, {}};
  sha256ContextCreate(&sink.hash);
  CURL *c = curl_easy_init();
  if (!c) { fclose(f); remove(tmp.c_str()); return false; }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
  // Sony serves these immutable blobs directly over HTTP; its CDN certificate
  // does not match these legacy hostnames. Redirects are disabled and every byte
  // is protected by an exact trusted size + SHA-256 content pin below.
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP);
#endif
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(expectedBytes));
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, pr);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // <1 KB/s ...
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 45L);      // ... for 45 s -> give up (dead link)
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Vita3K-nx/" VITA3K_NX_VERSION);
  // No CURLOPT_TIMEOUT: the firmware PUPs are large (hundreds of MB total).
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  std::array<u8, SHA256_HASH_SIZE> actualHash{};
  sha256ContextGetHash(&sink.hash, actualHash.data());
  bool ok = (rc == CURLE_OK) && (code >= 200 && code < 300) && !sink.failed &&
            sink.written == expectedBytes && actualHash == expectedHash;
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (ok && R_FAILED(fsdevCommitDevice("sdmc"))) ok = false;
  if (ok && !looksLikePup(tmp)) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!activateDownloadedPup(tmp, destPath, expectedBytes)) { remove(tmp.c_str()); return false; }
  return true;
}

int firmware_download_all(void (*progress)(int idx, int total, int pct, const char *label,
                                           double mbps, long long dlnow, long long dltotal)) {
  mkdir(FW_INSTALL_DIR, 0777);
  struct FwItem {
    const char *url;
    const char *name;
    const char *label;
    std::uint64_t size;
    const char *sha256;
  };
  // Distinct destination names so the emulator installs all three (it installs
  // every .pup found in install/, keyed by filename). Names come from the shared
  // FIRMWARE_PUP_NAMES so the local-install check looks for the exact same files.
  static const FwItem items[] = {
    { "http://dus01.psp2.update.playstation.net/update/psp2/image/2022_0209/pre_efd1ef6c1cc2fe92e72e9e783e421237/PSP2UPDAT.PUP?dest=us",
      FIRMWARE_PUP_NAMES[0], "pre-install firmware", 128798720,
      "339d1439eb329cfbd1a936f0a1563458e0555ef52975486b2844e24b6049e33e" },
    { "http://dus01.psv.update.playstation.net/update/psv/image/2022_0209/rel_f2c7b12fe85496ec88a0391b514d6e3b/PSVUPDAT.PUP",
      FIRMWARE_PUP_NAMES[1], "firmware", 133834240,
      "6ef6dc8da6db026f28647713e473486d770087a605c52a8d751bfca7478386cf" },
    { "http://dus01.psp2.update.playstation.net/update/psp2/image/2022_0209/sd_59dcf059d3328fb67be7e51f8aa33418/PSP2UPDAT.PUP?dest=us",
      FIRMWARE_PUP_NAMES[2], "firmware system data / font", 56778752,
      "c3c03fc7363dd573d90e5157629bf11551f434b283cc898d9ffc71dd716b791c" },
  };
  const int total = (int)(sizeof(items) / sizeof(items[0]));
  for (int i = 0; i < total; i++) {
    Prog pr = { progress, i + 1, total, -1, items[i].label, items[i].size, 0, 0, 0.0 };
    if (progress) progress(i + 1, total, 0, items[i].label, 0.0, 0, 0);   // show the file starting
    const std::string dest = std::string(FW_INSTALL_DIR) + "/" + items[i].name;
    if (!http_download_file(items[i].url, dest, items[i].size, items[i].sha256, &pr))
      return i + 1;   // non-zero: the 1-based index of the file that failed
  }
  return 0;
}

// Local-install support: verify the three PUPs are already sitting in install/
// (a user with no network can copy them there manually). Reuses looksLikePup so a
// truncated/wrong file is rejected exactly like a bad download.
bool firmware_local_files_present(std::vector<std::string> *missing) {
  if (missing) missing->clear();
  bool all = true;
  for (int i = 0; i < 3; i++) {
    const std::string path = std::string(FW_INSTALL_DIR) + "/" + FIRMWARE_PUP_NAMES[i];
    struct stat st;
    const bool ok = (stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode) && looksLikePup(path);
    if (!ok) { all = false; if (missing) missing->push_back(FIRMWARE_PUP_NAMES[i]); }
  }
  return all;
}
