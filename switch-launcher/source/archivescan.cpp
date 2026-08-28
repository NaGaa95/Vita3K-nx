// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "archivescan.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint16_t rd16(const unsigned char *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return s;
}

bool ends_with(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads the tail of the file and returns the offset of the end-of-central-directory
// record within `tail`, or npos.
size_t find_eocd(const std::vector<unsigned char> &tail) {
  if (tail.size() < 22)
    return std::string::npos;
  for (size_t back = 22; back <= tail.size(); back++) {
    const size_t at = tail.size() - back;
    if (rd32(tail.data() + at) == 0x06054b50u)
      return at;
  }
  return std::string::npos;
}

} // namespace

ArchiveContents archive_inspect(const std::string &path) {
  ArchiveContents out;

  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return out;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return out;
  }
  const long size = ftell(f);
  if (size < 22) {
    fclose(f);
    return out;
  }

  // The EOCD sits at the end, after an optional comment of at most 64 KiB.
  const long tail_size = size < 66000 ? size : 66000;
  std::vector<unsigned char> tail((size_t)tail_size);
  if (fseek(f, size - tail_size, SEEK_SET) != 0
      || fread(tail.data(), 1, tail.size(), f) != tail.size()) {
    fclose(f);
    return out;
  }

  const size_t eocd = find_eocd(tail);
  if (eocd == std::string::npos) {
    fclose(f);
    return out;
  }

  const uint16_t entries = rd16(tail.data() + eocd + 10);
  const uint32_t dir_size = rd32(tail.data() + eocd + 12);
  const uint32_t dir_at = rd32(tail.data() + eocd + 16);
  if (dir_at == 0xFFFFFFFFu || dir_size == 0xFFFFFFFFu // zip64
      || (long)dir_at + (long)dir_size > size) {
    fclose(f);
    return out;
  }

  std::vector<unsigned char> dir(dir_size);
  const bool read_ok = fseek(f, (long)dir_at, SEEK_SET) == 0
      && fread(dir.data(), 1, dir.size(), f) == dir.size();
  fclose(f);
  if (!read_ok)
    return out;

  size_t at = 0;
  for (uint16_t i = 0; i < entries; i++) {
    if (at + 46 > dir.size() || rd32(dir.data() + at) != 0x02014b50u)
      return out;
    const uint16_t name_len = rd16(dir.data() + at + 28);
    const uint16_t extra_len = rd16(dir.data() + at + 30);
    const uint16_t comment_len = rd16(dir.data() + at + 32);
    if (at + 46 + name_len > dir.size())
      return out;

    const std::string name = lower(std::string((const char *)dir.data() + at + 46, name_len));
    if (name.find("sce_sys/param.sfo") != std::string::npos
        || name.find("theme.xml") != std::string::npos)
      out.app_content = true;
    else if (ends_with(name, ".pkg"))
      out.package = true;
    else if (ends_with(name, "work.bin") || ends_with(name, ".rif"))
      out.license = true;

    at += 46u + name_len + extra_len + comment_len;
  }

  out.readable = true;
  return out;
}
