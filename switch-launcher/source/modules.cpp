// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "modules.h"

#include <curl/curl.h>

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef VITA3K_NX_VERSION
#define VITA3K_NX_VERSION "dev"
#endif

namespace {

constexpr const char *VITA_DIR = "sdmc:/switch/vita3k/vita";
constexpr const char *EXTERNAL_DIR = "sdmc:/switch/vita3k/vita/vs0/sys/external";
constexpr const char *TAI_DIR = "sdmc:/switch/vita3k/vita/ur0/tai";
// A ledger of what the launcher installed, so a module can report the release
// it came from rather than only that some file of the right name exists.
constexpr const char *STATE_PATH = "sdmc:/switch/vita3k/vita/ur0/tai/installed_modules.txt";
constexpr curl_off_t MAX_DOWNLOAD = 64ll * 1024 * 1024;

const ModuleEntry ENTRIES[] = {
  { "SceShaccCg", "Runtime shader compiler some titles load at boot",
    "AnimMouse/SceShaccCg", "libshacccg.suprx", "main", EXTERNAL_DIR, nullptr },
  { "kubridge", "Kernel bridge the homebrew ports link against",
    "TheOfficialFloW/kubridge", "kubridge.skprx", nullptr, TAI_DIR, "*KERNEL" },
  { "FdFix", "File-descriptor fixes those ports also expect",
    "TheOfficialFloW/FdFix", "FdFix.skprx", nullptr, TAI_DIR, "*KERNEL" },
};
constexpr int ENTRY_COUNT = (int)(sizeof(ENTRIES) / sizeof(ENTRIES[0]));

std::string g_tags[ENTRY_COUNT];
bool g_present[ENTRY_COUNT];

std::string install_path(int index) {
  return std::string(ENTRIES[index].directory) + "/" + ENTRIES[index].asset;
}

bool file_exists(const std::string &path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

bool ensure_dir(const std::string &path) {
  struct stat info {};
  if (stat(path.c_str(), &info) == 0)
    return S_ISDIR(info.st_mode);
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !ensure_dir(path.substr(0, slash)))
    return false;
  return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

std::string read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return {};
  std::string data;
  char buffer[1024];
  size_t got;
  while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
    data.append(buffer, got);
  fclose(file);
  return data;
}

bool write_file(const std::string &path, const std::string &data) {
  FILE *file = fopen(path.c_str(), "wb");
  if (!file)
    return false;
  const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), file) == data.size();
  return fclose(file) == 0 && ok;
}

void load_state() {
  const std::string text = read_file(STATE_PATH);
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    const size_t split = line.find('=');
    if (split == std::string::npos)
      continue;
    const std::string name = line.substr(0, split);
    for (int i = 0; i < ENTRY_COUNT; i++)
      if (name == ENTRIES[i].name)
        g_tags[i] = line.substr(split + 1);
  }
}

void save_state() {
  std::string text;
  for (int i = 0; i < ENTRY_COUNT; i++)
    if (!g_tags[i].empty())
      text += std::string(ENTRIES[i].name) + "=" + g_tags[i] + "\n";
  ensure_dir(TAI_DIR);
  write_file(STATE_PATH, text);
}

// taiHEN only loads what config.txt lists, so a freshly copied plugin has to be
// added to its section. Rewritten rather than appended so installing twice does
// not list the same plugin twice.
bool register_with_taihen(const ModuleEntry &entry) {
  const std::string configPath = std::string(TAI_DIR) + "/config.txt";
  const std::string wanted = std::string("ur0:tai/") + entry.asset;

  std::vector<std::string> lines;
  const std::string existing = read_file(configPath.c_str());
  size_t at = 0;
  while (at < existing.size()) {
    size_t end = existing.find('\n', at);
    if (end == std::string::npos)
      end = existing.size();
    std::string line = existing.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line != wanted)
      lines.push_back(line);
  }

  // Insert directly under the section header, creating the section if the file
  // does not have one yet.
  std::vector<std::string> output;
  bool placed = false;
  for (const std::string &line : lines) {
    output.push_back(line);
    if (!placed && line == entry.taiSection) {
      output.push_back(wanted);
      placed = true;
    }
  }
  if (!placed) {
    output.push_back(entry.taiSection);
    output.push_back(wanted);
  }

  std::string text;
  for (const std::string &line : output)
    text += line + "\n";
  return ensure_dir(TAI_DIR) && write_file(configPath, text);
}

size_t write_to_string(void *pointer, size_t size, size_t count, void *userdata) {
  auto *out = (std::string *)userdata;
  const size_t bytes = size * count;
  if (out->size() + bytes > (size_t)MAX_DOWNLOAD)
    return 0;
  out->append((const char *)pointer, bytes);
  return bytes;
}

int progress_cb(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto *cancel = (const std::atomic_bool *)user;
  return cancel && cancel->load(std::memory_order_acquire) ? 1 : 0;
}

bool http_get(const std::string &url, bool githubApi, std::string &out,
              long *code, const std::atomic_bool *cancel) {
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
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_string);
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
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, MAX_DOWNLOAD);
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

// The releases payload only has to give up two fields per asset, and both are
// plain ASCII, so the value is read directly rather than through a parser. The
// one escape that appears in practice is the slash in a URL.
std::string json_value(const std::string &json, size_t from, const char *field) {
  const std::string needle = std::string("\"") + field + "\"";
  const size_t key = json.find(needle, from);
  if (key == std::string::npos)
    return {};
  size_t at = json.find(':', key + needle.size());
  if (at == std::string::npos)
    return {};
  at = json.find('"', at);
  if (at == std::string::npos)
    return {};
  std::string value;
  for (size_t i = at + 1; i < json.size(); i++) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      value += json[++i];
      continue;
    }
    if (json[i] == '"')
      return value;
    value += json[i];
  }
  return {};
}

// The asset whose name matches what this module installs, falling back to the
// only asset when a release publishes a single differently-named file.
bool find_asset(const std::string &json, const ModuleEntry &entry, std::string &url) {
  const size_t assets = json.find("\"assets\"");
  if (assets == std::string::npos)
    return false;

  size_t at = assets;
  std::string firstUrl;
  for (;;) {
    const size_t name = json.find("\"name\"", at);
    if (name == std::string::npos)
      break;
    const std::string assetName = json_value(json, name, "name");
    const std::string assetUrl = json_value(json, name, "browser_download_url");
    if (assetUrl.empty())
      break;
    if (firstUrl.empty())
      firstUrl = assetUrl;
    if (assetName == entry.asset) {
      url = assetUrl;
      return true;
    }
    at = json.find("browser_download_url", name) + 1;
  }
  if (firstUrl.empty())
    return false;
  url = firstUrl;
  return true;
}

} // namespace

int modules_count() {
  return ENTRY_COUNT;
}

const ModuleEntry &modules_entry(int index) {
  return ENTRIES[index < 0 || index >= ENTRY_COUNT ? 0 : index];
}

void modules_refresh() {
  static bool loaded = false;
  if (!loaded) {
    load_state();
    loaded = true;
  }
  for (int i = 0; i < ENTRY_COUNT; i++) {
    g_present[i] = file_exists(install_path(i));
    if (!g_present[i])
      g_tags[i].clear();
  }
}

bool modules_installed(int index) {
  return index >= 0 && index < ENTRY_COUNT && g_present[index];
}

const char *modules_installed_tag(int index) {
  return index >= 0 && index < ENTRY_COUNT ? g_tags[index].c_str() : "";
}

int modules_install(int index, const std::atomic_bool *cancel, std::string &error) {
  if (index < 0 || index >= ENTRY_COUNT)
    return MODULE_BAD_DATA;
  const ModuleEntry &entry = ENTRIES[index];

  std::string tag;
  std::string url;
  long code = 0;

  if (entry.branch) {
    // Committed to the repository, so the version is the commit that last
    // touched the file. A failure here only costs the version label, not the
    // install, so it is not treated as fatal.
    std::string commits;
    const std::string api = std::string("https://api.github.com/repos/") + entry.repo
      + "/commits?path=" + entry.asset + "&per_page=1";
    if (http_get(api, true, commits, &code, cancel) && code == 200)
      tag = json_value(commits, 0, "sha").substr(0, 7);
    else if (cancel && cancel->load(std::memory_order_acquire))
      return MODULE_CANCELLED;
    url = std::string("https://raw.githubusercontent.com/") + entry.repo + "/"
      + entry.branch + "/" + entry.asset;
  } else {
    std::string json;
    const std::string api = std::string("https://api.github.com/repos/") + entry.repo + "/releases/latest";
    if (!http_get(api, true, json, &code, cancel)) {
      if (cancel && cancel->load(std::memory_order_acquire))
        return MODULE_CANCELLED;
      error = "Could not reach GitHub.";
      return MODULE_NO_NET;
    }
    // A repository whose only releases are pre-releases answers 404 here, so
    // fall back to the full list and take the newest entry it reports.
    if (code == 404) {
      json.clear();
      const std::string list = std::string("https://api.github.com/repos/") + entry.repo + "/releases";
      http_get(list, true, json, &code, cancel);
    }
    if (code != 200 || json.empty() || json == "[]") {
      error = std::string(entry.repo) + " has no published release (HTTP " + std::to_string(code) + ").";
      return MODULE_NO_RELEASE;
    }
    tag = json_value(json, 0, "tag_name");
    if (!find_asset(json, entry, url)) {
      error = std::string("The latest ") + entry.repo + " release has no downloadable asset.";
      return MODULE_NO_RELEASE;
    }
  }
  if (!tag.empty() && g_present[index] && g_tags[index] == tag)
    return MODULE_UP_TO_DATE;

  std::string payload;
  if (!http_get(url, false, payload, &code, cancel)) {
    if (cancel && cancel->load(std::memory_order_acquire))
      return MODULE_CANCELLED;
    error = "The download did not complete.";
    return MODULE_NO_NET;
  }
  if (code != 200 || payload.empty()) {
    error = "The download returned HTTP " + std::to_string(code) + ".";
    return MODULE_BAD_DATA;
  }

  // Written beside the target and renamed, so an interrupted download cannot
  // leave a half-written module where a whole one used to be.
  const std::string target = install_path(index);
  const std::string temporary = target + ".part";
  if (!ensure_dir(entry.directory) || !write_file(temporary, payload)) {
    error = "Could not write to " + std::string(entry.directory) + ".";
    return MODULE_WRITE_FAILED;
  }
  remove(target.c_str());
  if (rename(temporary.c_str(), target.c_str()) != 0) {
    remove(temporary.c_str());
    error = "Could not replace " + target + ".";
    return MODULE_WRITE_FAILED;
  }

  if (entry.taiSection && !register_with_taihen(entry)) {
    error = "Installed, but ur0:tai/config.txt could not be updated.";
    return MODULE_WRITE_FAILED;
  }

  g_tags[index] = tag;
  g_present[index] = true;
  save_state();
  return MODULE_OK;
}

const char *modules_error_text(int result) {
  switch (result) {
  case MODULE_OK: return "Installed";
  case MODULE_UP_TO_DATE: return "Already up to date";
  case MODULE_NO_NET: return "No connection";
  case MODULE_NO_RELEASE: return "No release found";
  case MODULE_BAD_DATA: return "Bad download";
  case MODULE_WRITE_FAILED: return "Could not write";
  case MODULE_CANCELLED: return "Cancelled";
  default: return "Failed";
  }
}
