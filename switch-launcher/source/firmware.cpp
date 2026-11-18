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
#ifdef __SWITCH__
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

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

#ifdef __SWITCH__
// When the console's DNS blocks Sony's update hosts (90DNS, Pi-hole...), resolve them through public DNS for curl
// (CURLOPT_RESOLVE); the pinned PUP size and SHA-256 still guard the data.
static std::string firmware_url_host(const std::string &url) {
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  const size_t end = url.find_first_of(":/?", start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::string ipv4_text(std::uint32_t address) {
  char text[16];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, (address >> 16) & 255, (address >> 8) & 255,
           address & 255);
  return text;
}

// Skips a (possibly compressed) DNS name; returns the offset after it, or 0 when malformed.
static size_t dns_skip_name(const std::uint8_t *packet, size_t size, size_t at) {
  while (at < size) {
    const std::uint8_t length = packet[at];
    if (length == 0) return at + 1;
    if ((length & 0xC0) == 0xC0) return at + 2 <= size ? at + 2 : 0;
    at += 1u + length;
  }
  return 0;
}

// One A query to `server` (dotted IPv4) on UDP port 53, 3 s timeout. The first usable IPv4 answer, or 0.
static std::uint32_t dns_query_a(const char *server, const std::string &host) {
  std::uint8_t query[512];
  size_t n = 0;
  const std::uint16_t id = static_cast<std::uint16_t>(armGetSystemTick());
  const std::uint8_t header[12] = {static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id), 0x01, 0x00,
                                   0, 1, 0, 0, 0, 0, 0, 0}; // recursion desired, one question
  memcpy(query, header, sizeof(header));
  n = sizeof(header);
  for (size_t start = 0; start < host.size();) {
    size_t dot = host.find('.', start);
    if (dot == std::string::npos) dot = host.size();
    const size_t length = dot - start;
    if (!length || length > 63 || n + length + 6 > sizeof(query)) return 0;
    query[n++] = static_cast<std::uint8_t>(length);
    memcpy(query + n, host.data() + start, length);
    n += length;
    start = dot + 1;
  }
  const std::uint8_t tail[5] = {0, 0, 1, 0, 1}; // root, type A, class IN
  memcpy(query + n, tail, sizeof(tail));
  n += sizeof(tail);

  const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return 0;
  struct timeval timeout{};
  timeout.tv_sec = 3;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  std::uint32_t found = 0;
  if (inet_pton(AF_INET, server, &to.sin_addr) == 1 &&
      sendto(s, query, n, 0, reinterpret_cast<struct sockaddr *>(&to), sizeof(to)) == static_cast<ssize_t>(n)) {
    std::uint8_t reply[1024];
    const ssize_t got = recv(s, reply, sizeof(reply), 0);
    if (got >= 12 && reply[0] == query[0] && reply[1] == query[1] && (reply[2] & 0x80) && (reply[3] & 0x0F) == 0) {
      const size_t size = static_cast<size_t>(got);
      const unsigned questions = (reply[4] << 8) | reply[5];
      const unsigned answers = (reply[6] << 8) | reply[7];
      size_t at = 12;
      for (unsigned i = 0; i < questions && at; i++) {
        at = dns_skip_name(reply, size, at);
        at = at && at + 4 <= size ? at + 4 : 0;
      }
      for (unsigned i = 0; i < answers && at && !found; i++) {
        at = dns_skip_name(reply, size, at);
        if (!at || at + 10 > size) break;
        const unsigned type = (reply[at] << 8) | reply[at + 1];
        const unsigned klass = (reply[at + 2] << 8) | reply[at + 3];
        const unsigned length = (reply[at + 8] << 8) | reply[at + 9];
        at += 10;
        if (at + length > size) break;
        if (type == 1 && klass == 1 && length == 4)
          found = (std::uint32_t(reply[at]) << 24) | (std::uint32_t(reply[at + 1]) << 16) |
                  (std::uint32_t(reply[at + 2]) << 8) | reply[at + 3];
        at += length;
      }
    }
  }
  close(s);
  // (an answer in 127/8 or 0.0.0.0 is a block too)
  return found && (found >> 24) != 127 ? found : 0;
}

// "host:80:address" for CURLOPT_RESOLVE when the console's DNS blocks the host (no answer, or only loopback or
// null addresses), else empty.
static std::string firmware_dns_bypass(const std::string &url) {
  const std::string host = firmware_url_host(url);
  if (host.empty()) return {};
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  const int error = getaddrinfo(host.c_str(), nullptr, &hints, &result);
  bool blocked = true;
  for (const struct addrinfo *entry = error ? nullptr : result; entry; entry = entry->ai_next) {
    if (!entry->ai_addr) continue;
    const std::uint32_t address =
        ntohl(reinterpret_cast<const struct sockaddr_in *>(entry->ai_addr)->sin_addr.s_addr);
    if (address != 0 && (address >> 24) != 127) blocked = false;
  }
  if (result) freeaddrinfo(result);
  if (!blocked) return {};
  static const char *const kResolvers[] = {"1.1.1.1", "8.8.8.8"};
  for (const char *resolver : kResolvers) {
    const std::uint32_t answer = dns_query_a(resolver, host);
    if (answer) return host + ":80:" + ipv4_text(answer);
  }
  return {};
}
#endif

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
  // Combine curl's small chunks into larger SD writes.
  setvbuf(f, nullptr, _IOFBF, 256 * 1024);
  FileSink sink{f, expectedBytes, 0, false, {}};
  sha256ContextCreate(&sink.hash);
  CURL *c = curl_easy_init();
  if (!c) { fclose(f); remove(tmp.c_str()); return false; }
#ifdef __SWITCH__
  struct curl_slist *resolve = nullptr;
  const std::string bypass = firmware_dns_bypass(url);
  if (!bypass.empty()) {
    resolve = curl_slist_append(nullptr, bypass.c_str());
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
  }
#endif
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
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
#ifdef __SWITCH__
  if (resolve) curl_slist_free_all(resolve);
#endif
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
