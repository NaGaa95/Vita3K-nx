// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "compatdb.h"

#include "tinyxml2.h"

#include <switch.h>

#include <curl/curl.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifndef VITA3K_NX_VERSION
#error "VITA3K_NX_VERSION must come from switch-launcher/version.mk"
#endif

// Same endpoints the desktop frontend uses (vita3k/gui-qt/src/game_compatibility.cpp).
static const char *kVersionUrl = "https://api.github.com/repos/Vita3K/compatibility/releases/latest";
static const char *kDbUrl = "https://github.com/Vita3K/compatibility/releases/download/compat_db/app_compat_db.xml.zip";
static const char *kDbFile = "app_compat_db.xml";

static constexpr size_t kMaxDownload = 32u * 1024 * 1024;
static constexpr size_t kMaxXml = 64u * 1024 * 1024;

// GitHub issue label ids, from vita3k/compat/src/compat.cpp.
static int label_to_state(unsigned long raw) {
  switch (raw) {
  case 1260231569ul: return COMPAT_NOTHING;
  case 1344750319ul: return COMPAT_BOOTABLE;
  case 1260231381ul: return COMPAT_INTRO;
  case 1344751053ul: return COMPAT_MENU;
  case 1344752299ul: return COMPAT_INGAME_LESS;
  case 1260231985ul: return COMPAT_INGAME_MORE;
  case 920344019ul: return COMPAT_PLAYABLE;
  default: return COMPAT_UNKNOWN;
  }
}

static std::unordered_map<std::string, int> g_db;
static std::string g_updatedAt;

bool compatdb_loaded() { return !g_db.empty(); }
int compatdb_count() { return (int)g_db.size(); }
const std::string &compatdb_updated_at() { return g_updatedAt; }

int compatdb_state(const std::string &titleId) {
  const auto it = g_db.find(titleId);
  return it == g_db.end() ? COMPAT_UNKNOWN : it->second;
}

const char *compatdb_state_label(int state) {
  switch (state) {
  case COMPAT_NOTHING: return "Nothing";
  case COMPAT_BOOTABLE: return "Bootable";
  case COMPAT_INTRO: return "Intro";
  case COMPAT_MENU: return "Menu";
  case COMPAT_INGAME_LESS: return "In-game (less)";
  case COMPAT_INGAME_MORE: return "In-game (more)";
  case COMPAT_PLAYABLE: return "Playable";
  default: return "Untested";
  }
}

// Matches AppsListTable::compat_color in the desktop frontend.
void compatdb_state_color(int state, unsigned char rgb[3]) {
  static const unsigned char table[8][3] = {
    { 0x8a, 0x8a, 0x8a }, // unknown
    { 0xff, 0x00, 0x00 }, // nothing
    { 0x62, 0x1f, 0xa5 }, // bootable
    { 0xc7, 0x15, 0x85 }, // intro
    { 0x1d, 0x76, 0xdb }, // menu
    { 0xe0, 0x8a, 0x1e }, // in-game (less)
    { 0xff, 0xd7, 0x00 }, // in-game (more)
    { 0x0e, 0x8a, 0x16 }, // playable
  };
  const int row = (state < COMPAT_NOTHING || state > COMPAT_PLAYABLE) ? 0 : state + 1;
  memcpy(rgb, table[row], 3);
}

// ---------------------------------------------------------------------------
// XML
// ---------------------------------------------------------------------------

static bool parse_xml(const char *data, size_t size) {
  tinyxml2::XMLDocument doc;
  if (doc.Parse(data, size) != tinyxml2::XML_SUCCESS)
    return false;

  const tinyxml2::XMLElement *root = doc.FirstChildElement("compatibility");
  if (!root)
    return false;

  std::unordered_map<std::string, int> parsed;
  const char *updated = root->Attribute("iso_db_updated_at");
  for (const tinyxml2::XMLElement *app = root->FirstChildElement(); app;
       app = app->NextSiblingElement()) {
    const char *titleId = app->Attribute("title_id");
    if (!titleId || !*titleId)
      continue;
    // The upstream DB also carries issues filed against non-app title ids.
    if (!strstr(titleId, "PCS") && strcmp(titleId, "NPXS10007") != 0)
      continue;
    if (parsed.count(titleId))
      continue;

    int state = COMPAT_UNKNOWN;
    if (const tinyxml2::XMLElement *labels = app->FirstChildElement("labels")) {
      for (const tinyxml2::XMLElement *label = labels->FirstChildElement(); label;
           label = label->NextSiblingElement()) {
        const char *text = label->GetText();
        if (!text)
          continue;
        const int mapped = label_to_state(strtoul(text, nullptr, 10));
        if (mapped != COMPAT_UNKNOWN)
          state = mapped;
      }
    }
    parsed[titleId] = state;
  }

  if (parsed.empty())
    return false;

  g_db = std::move(parsed);
  g_updatedAt = updated ? updated : "";
  return true;
}

bool compatdb_load(const std::string &cacheDir) {
  const std::string path = cacheDir + "/" + kDbFile;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return false;

  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  if (size <= 0 || (size_t)size > kMaxXml) {
    fclose(f);
    return false;
  }
  fseek(f, 0, SEEK_SET);

  std::vector<char> buffer((size_t)size);
  const size_t read = fread(buffer.data(), 1, buffer.size(), f);
  fclose(f);
  if (read != buffer.size())
    return false;

  return parse_xml(buffer.data(), buffer.size());
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

static size_t write_cb(void *ptr, size_t sz, size_t n, void *user) {
  std::string *out = (std::string *)user;
  if (n != 0 && sz > std::numeric_limits<size_t>::max() / n)
    return 0;
  const size_t add = sz * n;
  if (add > kMaxDownload - out->size())
    return 0;
  out->append((const char *)ptr, add);
  return add;
}

static int progress_cb(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto *cancel = (const std::atomic_bool *)user;
  return cancel && cancel->load(std::memory_order_acquire) ? 1 : 0;
}

static bool http_get(const char *url, bool githubApi, std::string &out, long *code,
                     const std::atomic_bool *cancel) {
  if (code)
    *code = 0;
  CURL *c = curl_easy_init();
  if (!c)
    return false;

  curl_slist *headers = nullptr;
  if (githubApi) {
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2026-03-10");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  }
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)kMaxDownload);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Vita3K-nx/" VITA3K_NX_VERSION);
  if (cancel) {
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)cancel);
  }

  const CURLcode rc = curl_easy_perform(c);
  long httpCode = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
  if (code)
    *code = httpCode;
  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return rc == CURLE_OK;
}

// The release body carries "Last updated: YYYY-MM-DD HH:MM:SSZ", the same value
// the shipped XML exposes as iso_db_updated_at.
static std::string parse_version(const std::string &body) {
  static const char marker[] = "Last updated: ";
  const size_t at = body.find(marker);
  if (at == std::string::npos)
    return {};
  const size_t start = at + sizeof(marker) - 1;
  if (start + 20 > body.size())
    return {};

  const std::string stamp = body.substr(start, 20);
  for (size_t i = 0; i < stamp.size(); i++) {
    const char c = stamp[i];
    if (i == 4 || i == 7) {
      if (c != '-')
        return {};
    } else if (i == 10) {
      if (c != ' ')
        return {};
    } else if (i == 13 || i == 16) {
      if (c != ':')
        return {};
    } else if (i == 19) {
      if (c != 'Z')
        return {};
    } else if (c < '0' || c > '9') {
      return {};
    }
  }
  return stamp;
}

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------

static uint16_t rd16(const unsigned char *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Extracts one named member. The compat archive holds a single XML file, so a
// full zip reader (and a dependency on miniz) would not earn its keep.
static bool zip_extract(const std::string &zip, const char *name, std::string &out) {
  if (zip.size() < 22)
    return false;
  const unsigned char *base = (const unsigned char *)zip.data();

  size_t eocd = 0;
  bool found = false;
  const size_t scan = zip.size() < 66000 ? zip.size() : 66000;
  for (size_t back = 22; back <= scan; back++) {
    const size_t at = zip.size() - back;
    if (rd32(base + at) == 0x06054b50u) {
      eocd = at;
      found = true;
      break;
    }
  }
  if (!found)
    return false;

  const uint16_t entries = rd16(base + eocd + 10);
  uint32_t dir = rd32(base + eocd + 16);
  const size_t nameLength = strlen(name);

  for (uint16_t i = 0; i < entries; i++) {
    if ((size_t)dir + 46 > zip.size() || rd32(base + dir) != 0x02014b50u)
      return false;
    const uint16_t method = rd16(base + dir + 10);
    const uint32_t compressed = rd32(base + dir + 20);
    const uint32_t uncompressed = rd32(base + dir + 24);
    const uint16_t entryName = rd16(base + dir + 28);
    const uint16_t extraLen = rd16(base + dir + 30);
    const uint16_t commentLen = rd16(base + dir + 32);
    const uint32_t localAt = rd32(base + dir + 42);
    if ((size_t)dir + 46 + entryName > zip.size())
      return false;

    const bool match = entryName == nameLength
        && memcmp(base + dir + 46, name, entryName) == 0;
    dir += 46u + entryName + extraLen + commentLen;
    if (!match)
      continue;

    // Zip64 saturates these 32-bit fields. The DB is a couple of megabytes, so
    // treat that as corruption instead of parsing the extra field.
    if (compressed == 0xFFFFFFFFu || uncompressed == 0xFFFFFFFFu
        || localAt == 0xFFFFFFFFu || uncompressed > kMaxXml)
      return false;

    if ((size_t)localAt + 30 > zip.size() || rd32(base + localAt) != 0x04034b50u)
      return false;
    const uint16_t localName = rd16(base + localAt + 26);
    const uint16_t localExtra = rd16(base + localAt + 28);
    const size_t dataAt = (size_t)localAt + 30 + localName + localExtra;
    if (dataAt + compressed > zip.size())
      return false;

    if (method == 0) {
      if (compressed != uncompressed)
        return false;
      out.assign((const char *)base + dataAt, compressed);
      return true;
    }
    if (method != 8)
      return false;

    out.assign((size_t)uncompressed, 0);
    z_stream stream{};
    // A negative window size selects a raw deflate stream (no zlib wrapper).
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
      return false;
    stream.next_in = (Bytef *)(base + dataAt);
    stream.avail_in = compressed;
    stream.next_out = (Bytef *)out.data();
    stream.avail_out = uncompressed;
    const int rc = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);
    if (rc != Z_STREAM_END || produced != uncompressed) {
      out.clear();
      return false;
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------

// FAT rename() will not replace an existing file, so the current copy is moved
// aside first and only removed once the replacement is in place. Losing power
// mid-way leaves either the old file or the .old beside it, never nothing.
static bool write_file(const std::string &path, const std::string &data) {
  const std::string temporary = path + ".tmp";
  const std::string previous = path + ".old";

  FILE *f = fopen(temporary.c_str(), "wb");
  if (!f)
    return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
  fclose(f);
  if (!ok) {
    unlink(temporary.c_str());
    return false;
  }

  struct stat info {};
  const bool had_current = stat(path.c_str(), &info) == 0;
  unlink(previous.c_str());
  if (had_current && rename(path.c_str(), previous.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  if (rename(temporary.c_str(), path.c_str()) != 0) {
    if (had_current)
      rename(previous.c_str(), path.c_str());
    unlink(temporary.c_str());
    fsdevCommitDevice("sdmc");
    return false;
  }
  if (had_current)
    unlink(previous.c_str());
  fsdevCommitDevice("sdmc");
  return true;
}

int compatdb_update(const std::string &cacheDir, const std::atomic_bool *cancel) {
  const auto cancelled = [&] {
    return cancel && cancel->load(std::memory_order_acquire);
  };

  std::string body;
  long code = 0;
  if (!http_get(kVersionUrl, true, body, &code, cancel))
    return cancelled() ? COMPATDB_CANCELLED : COMPATDB_NO_NET;
  if (code < 200 || code >= 300)
    return COMPATDB_NO_NET;

  const std::string latest = parse_version(body);
  if (latest.empty())
    return COMPATDB_BAD_DATA;
  if (compatdb_loaded() && latest == g_updatedAt)
    return COMPATDB_UP_TO_DATE;

  std::string zip;
  if (!http_get(kDbUrl, false, zip, &code, cancel))
    return cancelled() ? COMPATDB_CANCELLED : COMPATDB_NO_NET;
  if (code < 200 || code >= 300 || zip.size() < 22)
    return COMPATDB_NO_NET;

  std::string xml;
  if (!zip_extract(zip, kDbFile, xml) || xml.empty())
    return COMPATDB_BAD_DATA;

  // Keep the previous table until the payload is known to parse and persist.
  auto previous = g_db;
  const std::string previousVersion = g_updatedAt;
  if (!parse_xml(xml.data(), xml.size()))
    return COMPATDB_BAD_DATA;

  mkdir(cacheDir.c_str(), 0777);
  if (!write_file(cacheDir + "/" + kDbFile, xml)) {
    g_db = std::move(previous);
    g_updatedAt = previousVersion;
    return COMPATDB_WRITE_FAILED;
  }
  return COMPATDB_OK;
}
