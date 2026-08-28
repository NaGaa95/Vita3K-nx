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

#include "switch_installer.h"

#include <archive.h>            // install_archive (.vpk / .zip / .vci)
#include <emuenv/state.h>
#include <packages/functions.h> // install_pup
#include <packages/license.h>   // copy_license, validate_zrif
#include <packages/pkg.h>       // install_pkg, find_pkg_zrif
#include <packages/sfo.h>       // SfoAppInfo, for the post-install check
#include <util/fs.h>
#include <util/log.h>

#include <miniz.h>
#include <openssl/aes.h>
#include <openssl/evp.h>

#include <switch.h>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <streambuf>
#include <string>
#include <vector>

namespace {

// psvpfstools (PfsFilesystem / execute()) reports all of its progress and errors
// to std::cout, which on Switch lands on the transient libnx console and never
// reaches vita3k.log. This streambuf tees std::cout: every completed line is
// forwarded to the logger (so PFS mount/decrypt/keystone failures are captured)
// while still passing through to the original console for the on-screen UI.
class TeeLogBuf : public std::streambuf {
public:
    explicit TeeLogBuf(std::streambuf *chain)
        : chain_(chain) {}

protected:
    int overflow(int ch) override {
        if (ch == std::streambuf::traits_type::eof())
            return ch;
        if (chain_)
            chain_->sputc(static_cast<char>(ch));
        if (ch == '\n') {
            if (!line_.empty()) {
                LOG_INFO("[pfs] {}", line_);
                line_.clear();
            }
        } else if (ch != '\r') {
            line_.push_back(static_cast<char>(ch));
        }
        return ch;
    }

private:
    std::streambuf *chain_;
    std::string line_;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return s;
}

// Verify the cross-built OpenSSL AES against the FIPS-197 AES-128-ECB test
// vector. If this fails, every SCE/PKG decryption will produce garbage.
bool aes_selftest() {
    const uint8_t key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    const uint8_t expected[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    // (1) Low-level AES (aes_core.c) - bypasses the EVP/provider layer entirely.
    AES_KEY aeskey;
    uint8_t out_ll[16] = {};
    const int ks_rc = AES_set_encrypt_key(key, 128, &aeskey);
    AES_encrypt(pt, out_ll, &aeskey);
    const bool ll_ok = std::memcmp(out_ll, expected, 16) == 0;
    LOG_INFO("AES low-level: {} (ks_rc={}) out={:02x}{:02x}{:02x}{:02x} exp={:02x}{:02x}{:02x}{:02x}",
        ll_ok ? "PASS" : "FAIL", ks_rc, out_ll[0], out_ll[1], out_ll[2], out_ll[3],
        expected[0], expected[1], expected[2], expected[3]);

    // (2) EVP path (what pkg.cpp / sce_utils actually use).
    uint8_t out[32] = {};
    int len = 0, flen = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER *ecb = EVP_CIPHER_fetch(nullptr, "AES-128-ECB", nullptr);
    const bool have = ctx && ecb;
    const int i_rc = have ? EVP_EncryptInit_ex(ctx, ecb, nullptr, key, nullptr) : 0;
    if (i_rc == 1)
        EVP_CIPHER_CTX_set_padding(ctx, 0);
    const int u_rc = (i_rc == 1) ? EVP_EncryptUpdate(ctx, out, &len, pt, 16) : 0;
    const bool evp_ok = u_rc == 1 && std::memcmp(out, expected, 16) == 0;
    LOG_INFO("AES EVP: {} (fetch={} init={} upd={}) out={:02x}{:02x}{:02x}{:02x}",
        evp_ok ? "PASS" : "FAIL", ecb != nullptr, i_rc, u_rc, out[0], out[1], out[2], out[3]);
    if (ecb)
        EVP_CIPHER_free(ecb);
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);

    LOG_INFO("AES-128-ECB self-test: {}", (ll_ok && evp_ok) ? "PASS" : "FAIL");
    return ll_ok && evp_ok;
}

// ---------------------------------------------------------------------------
// Graphical (framebuffer) installer UI
//
// At the --install stage the emulator has NOT created its Vulkan surface yet and
// no console has been initialised, so the default window's framebuffer is free.
// We draw a simple console-free progress screen straight into it. If the
// framebuffer cannot be created for any reason we fall back to the old libnx
// text console (ConsoleUI) so installation still works.
// ---------------------------------------------------------------------------

constexpr int kScreenW = 1280;
constexpr int kScreenH = 720;
const char *const kTitle = "Vita3K - Installer";

// Colours are 0xRRGGBBAA (matches how the framebuffer bytes are laid out for
// PIXEL_FORMAT_RGBA_8888 after framebufferMakeLinear).
constexpr uint32_t kColBg = 0x16181effu;      // dark background
constexpr uint32_t kColHeader = 0xededf2ffu;  // header text
constexpr uint32_t kColSub = 0xc9cdd6ffu;     // subtitle / step text
constexpr uint32_t kColTrack = 0x21252effu;   // progress bar track
constexpr uint32_t kColFrame = 0x3b414fffu;   // progress bar border
constexpr uint32_t kColFill = 0x3aa0ffffu;    // progress bar fill (accent)
constexpr uint32_t kColPct = 0xededf2ffu;     // "NN%" text

// Public-domain 8x8 bitmap font (the standard `font8x8_basic` table), ASCII
// 0x20-0x7E. Row-major; bit 0 (LSB) is the leftmost pixel of each 8px row.
const uint8_t kFont8x8[95][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x20 ' '
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 }, // 0x21 '!'
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x22 '"'
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 }, // 0x23 '#'
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00 }, // 0x24 '$'
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00 }, // 0x25 '%'
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00 }, // 0x26 '&'
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x27 '''
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00 }, // 0x28 '('
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00 }, // 0x29 ')'
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, // 0x2A '*'
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00 }, // 0x2B '+'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // 0x2C ','
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00 }, // 0x2D '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // 0x2E '.'
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00 }, // 0x2F '/'
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00 }, // 0x30 '0'
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00 }, // 0x31 '1'
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00 }, // 0x32 '2'
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00 }, // 0x33 '3'
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00 }, // 0x34 '4'
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00 }, // 0x35 '5'
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00 }, // 0x36 '6'
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00 }, // 0x37 '7'
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00 }, // 0x38 '8'
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00 }, // 0x39 '9'
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // 0x3A ':'
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // 0x3B ';'
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00 }, // 0x3C '<'
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00 }, // 0x3D '='
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00 }, // 0x3E '>'
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00 }, // 0x3F '?'
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00 }, // 0x40 '@'
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00 }, // 0x41 'A'
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00 }, // 0x42 'B'
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00 }, // 0x43 'C'
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00 }, // 0x44 'D'
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00 }, // 0x45 'E'
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00 }, // 0x46 'F'
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00 }, // 0x47 'G'
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00 }, // 0x48 'H'
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 0x49 'I'
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00 }, // 0x4A 'J'
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00 }, // 0x4B 'K'
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00 }, // 0x4C 'L'
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00 }, // 0x4D 'M'
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00 }, // 0x4E 'N'
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00 }, // 0x4F 'O'
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00 }, // 0x50 'P'
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00 }, // 0x51 'Q'
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00 }, // 0x52 'R'
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00 }, // 0x53 'S'
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 0x54 'T'
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00 }, // 0x55 'U'
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // 0x56 'V'
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, // 0x57 'W'
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00 }, // 0x58 'X'
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00 }, // 0x59 'Y'
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00 }, // 0x5A 'Z'
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00 }, // 0x5B '['
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 }, // 0x5C '\'
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00 }, // 0x5D ']'
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00 }, // 0x5E '^'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF }, // 0x5F '_'
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x60 '`'
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00 }, // 0x61 'a'
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00 }, // 0x62 'b'
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00 }, // 0x63 'c'
    { 0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00 }, // 0x64 'd'
    { 0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00 }, // 0x65 'e'
    { 0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00 }, // 0x66 'f'
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // 0x67 'g'
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00 }, // 0x68 'h'
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 0x69 'i'
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E }, // 0x6A 'j'
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00 }, // 0x6B 'k'
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 0x6C 'l'
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00 }, // 0x6D 'm'
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00 }, // 0x6E 'n'
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00 }, // 0x6F 'o'
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F }, // 0x70 'p'
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78 }, // 0x71 'q'
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00 }, // 0x72 'r'
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00 }, // 0x73 's'
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00 }, // 0x74 't'
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00 }, // 0x75 'u'
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // 0x76 'v'
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00 }, // 0x77 'w'
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00 }, // 0x78 'x'
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // 0x79 'y'
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00 }, // 0x7A 'z'
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00 }, // 0x7B '{'
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 }, // 0x7C '|'
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00 }, // 0x7D '}'
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x7E '~'
};

int clamp_pct(int pct) {
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

inline void put4(uint8_t *p, uint32_t rgba) {
    p[0] = static_cast<uint8_t>(rgba >> 24); // R
    p[1] = static_cast<uint8_t>(rgba >> 16); // G
    p[2] = static_cast<uint8_t>(rgba >> 8);  // B
    p[3] = static_cast<uint8_t>(rgba);       // A
}

inline void putpixel(uint8_t *px, uint32_t stride, int x, int y, uint32_t rgba) {
    if (static_cast<unsigned>(x) >= static_cast<unsigned>(kScreenW) || static_cast<unsigned>(y) >= static_cast<unsigned>(kScreenH))
        return;
    put4(px + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4, rgba);
}

void fillRect(uint8_t *px, uint32_t stride, int x, int y, int w, int h, uint32_t rgba) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > kScreenW) w = kScreenW - x;
    if (y + h > kScreenH) h = kScreenH - y;
    for (int j = 0; j < h; j++) {
        uint8_t *row = px + static_cast<size_t>(y + j) * stride + static_cast<size_t>(x) * 4;
        for (int i = 0; i < w; i++)
            put4(row + static_cast<size_t>(i) * 4, rgba);
    }
}

// Filled rectangle with rounded corners of radius r (simple per-pixel corner
// mask). Used for the progress bar frame / track / fill so the UI reads as a
// modern rounded bar rather than a hard box.
void fillRoundRect(uint8_t *px, uint32_t stride, int x, int y, int w, int h, int r, uint32_t rgba) {
    if (w <= 0 || h <= 0)
        return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = -1, dy = -1;
            if (i < r && j < r) { dx = r - 1 - i; dy = r - 1 - j; }
            else if (i >= w - r && j < r) { dx = i - (w - r); dy = r - 1 - j; }
            else if (i < r && j >= h - r) { dx = r - 1 - i; dy = j - (h - r); }
            else if (i >= w - r && j >= h - r) { dx = i - (w - r); dy = j - (h - r); }
            if (dx >= 0 && dx * dx + dy * dy > r * r)
                continue;
            putpixel(px, stride, x + i, y + j, rgba);
        }
    }
}

void clear(uint8_t *px, uint32_t stride, uint32_t rgba) {
    fillRect(px, stride, 0, 0, kScreenW, kScreenH, rgba);
}

void drawGlyph(uint8_t *px, uint32_t stride, int x, int y, char ch, uint32_t rgba, int scale) {
    unsigned uc = static_cast<unsigned char>(ch);
    if (uc < 0x20 || uc > 0x7E)
        uc = 0x20;
    const uint8_t *g = kFont8x8[uc - 0x20];
    for (int row = 0; row < 8; row++) {
        const uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col))
                fillRect(px, stride, x + col * scale, y + row * scale, scale, scale, rgba);
        }
    }
}

void drawText(uint8_t *px, uint32_t stride, int x, int y, const char *str, uint32_t rgba, int scale) {
    for (const char *s = str; *s; s++) {
        drawGlyph(px, stride, x, y, *s, rgba, scale);
        x += 8 * scale;
    }
}

void drawTextCentered(uint8_t *px, uint32_t stride, int y, const char *str, uint32_t rgba, int scale) {
    const int w = static_cast<int>(std::strlen(str)) * 8 * scale;
    int x = (kScreenW - w) / 2;
    if (x < 8)
        x = 8;
    drawText(px, stride, x, y, str, rgba, scale);
}

// The font is fixed width, so a line holds exactly (usable width)/(8*scale)
// characters. Break on spaces where possible and split anywhere otherwise: a
// package file name is routinely 64 unbroken characters, which used to run off
// both edges of the screen and get cut.
std::vector<std::string> wrapText(const std::string &text, int scale, size_t max_lines) {
    constexpr int margin = 48;
    const size_t per_line = static_cast<size_t>(std::max(8, (kScreenW - 2 * margin) / (8 * scale)));
    std::vector<std::string> lines;

    size_t pos = 0;
    while (pos < text.size() && lines.size() < max_lines) {
        size_t take = std::min(per_line, text.size() - pos);
        if (pos + take < text.size()) {
            // Prefer the last space in the window, but only if it is not so early
            // that it would leave a nearly empty line.
            const size_t space = text.rfind(' ', pos + take);
            if (space != std::string::npos && space > pos && space - pos >= per_line / 3)
                take = space - pos;
        }
        lines.push_back(text.substr(pos, take));
        pos += take;
        while (pos < text.size() && text[pos] == ' ')
            pos++;
    }

    if (pos < text.size() && !lines.empty()) {
        std::string &last = lines.back();
        if (last.size() > 3)
            last.erase(last.size() - 3);
        last += "...";
    }
    return lines;
}

// Draws a wrapped block centred vertically on `centre_y`. Returns its height.
int drawTextBlockCentered(uint8_t *px, uint32_t stride, int centre_y, const std::string &text,
    uint32_t rgba, int scale, size_t max_lines) {
    const std::vector<std::string> lines = wrapText(text, scale, max_lines);
    if (lines.empty())
        return 0;
    const int line_h = 8 * scale + 6;
    const int height = static_cast<int>(lines.size()) * line_h;
    int y = centre_y - height / 2;
    for (const std::string &line : lines) {
        drawTextCentered(px, stride, y, line.c_str(), rgba, scale);
        y += line_h;
    }
    return height;
}

// Horizon expects an application to drain its applet message queue regularly.
// The install is a long stretch of crypto and SD I/O with no event loop of its
// own, so without this the HOME button, overlays and sleep requests all go
// unanswered until it finishes, which looks like a system-wide hang.
inline void pump_applet() {
    appletMainLoop();
}

// Abstract UI so the install flow below is written once and can drive either the
// framebuffer (GfxUI) or the legacy text console (ConsoleUI, used only as a
// fallback when the framebuffer cannot be created).
struct InstallerUI {
    virtual ~InstallerUI() = default;
    // Begin work on a new file/step; `label` is the on-screen line.
    virtual void step(const std::string &label) = 0;
    // Throttled 0..100 progress for the current step.
    virtual void progress(int pct) = 0;
    // One-off informational/result line (also logged).
    virtual void note(const std::string &line) = 0;
    // Final screen, showing the run's outcome; returns when the user continues
    // or after a timeout.
    virtual void done(const std::string &summary) = 0;
    // Ask a yes/no question: A = yes, B = no, with the on-screen legend for each.
    // Returns true only on an explicit A; defaults to false (no) on timeout, so a
    // destructive action is never taken without a deliberate press.
    virtual bool confirm(const std::string &prompt, const char *yes_label, const char *no_label) = 0;
};

// Console-free framebuffer UI.
class GfxUI final : public InstallerUI {
public:
    GfxUI() {
        NWindow *win = nwindowGetDefault();
        if (win && R_SUCCEEDED(framebufferCreate(&m_fb, win, kScreenW, kScreenH, PIXEL_FORMAT_RGBA_8888, 2))) {
            framebufferMakeLinear(&m_fb);
            m_ok = true;
            render(); // paint an initial frame immediately
        }
    }

    ~GfxUI() override {
        if (m_ok)
            framebufferClose(&m_fb);
    }

    bool ok() const { return m_ok; }

    void step(const std::string &label) override {
        pump_applet();
        LOG_WARN("[installer] == {}", label);
        m_sub = label;
        m_pct = 0;
        m_last_pct = -1;
        render();
    }

    void progress(int pct) override {
        pump_applet();
        pct = clamp_pct(pct);
        if (pct == m_last_pct)
            return;
        m_last_pct = pct;
        m_pct = pct;
        render();
    }

    void note(const std::string &line) override {
        pump_applet();
        LOG_WARN("[installer] {}", line);
        // Surface the first line on screen so results ("OK", "SKIPPED", ...) are
        // visible; the full text is in the log.
        const std::string first = line.substr(0, line.find('\n'));
        if (!first.empty()) {
            m_sub = first;
            render();
        }
    }

    void done(const std::string &summary) override {
        m_sub = summary;
        m_pct = 100;
        render();

        // Keep polling the pad (as the console version did) but also auto-continue
        // after ~2 seconds so the installer returns to the launcher without
        // requiring a button press.
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        constexpr int kMaxFrames = 125; // ~2s at 16ms/frame
        for (int f = 0; f < kMaxFrames && appletMainLoop(); f++) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            svcSleepThread(16000000ULL); // 16 ms
        }
    }

    bool confirm(const std::string &prompt, const char *yes_label, const char *no_label) override {
        render_confirm(prompt, yes_label, no_label);
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        constexpr int kMaxFrames = 60 * 62; // ~60 s, then default to "no" (keep)
        for (int f = 0; f < kMaxFrames && appletMainLoop(); f++) {
            padUpdate(&pad);
            const u64 down = padGetButtonsDown(&pad);
            if (down & HidNpadButton_A)
                return true;
            if (down & HidNpadButton_B)
                return false;
            svcSleepThread(16000000ULL);
        }
        return false;
    }

private:
    void render_confirm(const std::string &prompt, const char *yes_label, const char *no_label) {
        if (!m_ok)
            return;
        uint32_t stride = 0;
        uint8_t *px = static_cast<uint8_t *>(framebufferBegin(&m_fb, &stride));
        if (!px)
            return;
        clear(px, stride, kColBg);
        drawTextCentered(px, stride, 96, kTitle, kColHeader, 3);
        drawTextBlockCentered(px, stride, 318, prompt, kColSub, 2, 4);
        drawTextCentered(px, stride, 430, (std::string("A  =  ") + yes_label).c_str(), kColFill, 2);
        drawTextCentered(px, stride, 486, (std::string("B  =  ") + no_label).c_str(), kColSub, 2);
        framebufferEnd(&m_fb);
    }

    void render() {
        if (!m_ok)
            return;
        uint32_t stride = 0;
        uint8_t *px = static_cast<uint8_t *>(framebufferBegin(&m_fb, &stride));
        if (!px)
            return;

        clear(px, stride, kColBg);

        // Header near the top (scale 3).
        drawTextCentered(px, stride, 96, kTitle, kColHeader, 3);

        // Current step / file (scale 2), above the bar. Wrapped: step labels carry
        // the file name, which is far wider than the screen.
        drawTextBlockCentered(px, stride, 330, m_sub, kColSub, 2, 3);

        // Centered rounded progress bar (~66% of the screen width).
        const int barW = (kScreenW * 66) / 100;
        const int barH = 46;
        const int barX = (kScreenW - barW) / 2;
        const int barY = 396;
        fillRoundRect(px, stride, barX - 3, barY - 3, barW + 6, barH + 6, 14, kColFrame); // border
        fillRoundRect(px, stride, barX, barY, barW, barH, 12, kColTrack);                 // track
        const int p = clamp_pct(m_pct);
        const int innerW = ((barW - 10) * p) / 100;
        if (innerW > 0)
            fillRoundRect(px, stride, barX + 5, barY + 5, innerW, barH - 10, 8, kColFill);

        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", p);
        drawTextCentered(px, stride, barY + barH + 24, buf, kColPct, 2);

        framebufferEnd(&m_fb);
    }

    Framebuffer m_fb{};
    bool m_ok = false;
    std::string m_sub;
    int m_pct = 0;
    int m_last_pct = -1;
};

// Legacy libnx text-console UI. Only used if the framebuffer is unavailable.
class ConsoleUI final : public InstallerUI {
public:
    ConsoleUI() {
        consoleInit(nullptr);
        printf("\x1b[2J\x1b[H=== Vita3K on-device installer ===\n\n");
        consoleUpdate(nullptr);
    }

    ~ConsoleUI() override {
        consoleExit(nullptr);
    }

    void step(const std::string &label) override {
        pump_applet();
        m_last_pct = -1;
        printf("%s ...\n", label.c_str());
        consoleUpdate(nullptr);
    }

    void progress(int pct) override {
        pump_applet();
        pct = clamp_pct(pct);
        if (pct == m_last_pct)
            return;
        m_last_pct = pct;
        printf("\r  %3d%%   ", pct);
        consoleUpdate(nullptr);
    }

    void note(const std::string &line) override {
        pump_applet();
        LOG_WARN("[installer] {}", line);
        printf("%s\n", line.c_str());
        consoleUpdate(nullptr);
    }

    void done(const std::string &summary) override {
        printf("\n%s Press + to continue.\n", summary.c_str());
        consoleUpdate(nullptr);
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(nullptr);
        }
    }

    bool confirm(const std::string &prompt, const char *yes_label, const char *no_label) override {
        printf("\n%s\n  A = %s   B = %s\n", prompt.c_str(), yes_label, no_label);
        consoleUpdate(nullptr);
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            const u64 down = padGetButtonsDown(&pad);
            if (down & HidNpadButton_A)
                return true;
            if (down & HidNpadButton_B)
                return false;
            consoleUpdate(nullptr);
        }
        return false;
    }

private:
    int m_last_pct = -1;
};

// Shared install flow. UI-agnostic: all on-screen output goes through `ui`, so
// this same logic drives the graphical or the console front-end. This keeps the
// firmware/license/package install logic (scan, categorise, mark_done, cout tee)
// in a single place.
// A "rom" .zip usually holds a .pkg and its work.bin, or a .vpk - not the app
// content itself. install_archive only understands the latter (it looks for
// sce_sys/param.sfo), so such a zip is rejected as "not readable" even though it
// is perfectly good. Unpack the installables it contains and feed them back in
// as ordinary inputs.
struct NestedScan {
    bool direct_content = false; // has sce_sys/param.sfo or theme.xml
    std::vector<std::string> installables; // member names worth extracting
    std::vector<std::string> other_extensions; // for the failure message
    bool has_package = false; // a .pkg, which cannot install without a licence
    bool has_license = false; // work.bin / .rif shipped alongside it
};

static bool is_installable_member(const std::string &name) {
    const std::string leaf = lower(fs::path(name).filename().string());
    const std::string ext = lower(fs::path(name).extension().string());
    // Deliberately not every ".bin": a game archive routinely carries unrelated
    // ones, and each would be attempted as a license and reported as a failure.
    return leaf == "work.bin" || ext == ".pkg" || ext == ".vpk" || ext == ".vci" || ext == ".rif";
}

static NestedScan scan_archive(const fs::path &archive_path) {
    NestedScan scan;
    mz_zip_archive zip{};
    std::memset(&zip, 0, sizeof(zip));

    FILE *fp = std::fopen(archive_path.string().c_str(), "rb");
    if (!fp)
        return scan;
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        std::fclose(fp);
        return scan;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        const std::string name = stat.m_filename;
        if (name.contains("sce_sys/param.sfo") || name.contains("theme.xml")) {
            scan.direct_content = true;
            break;
        }
        if (is_installable_member(name)) {
            const std::string leaf = lower(fs::path(name).filename().string());
            const std::string ext = lower(fs::path(name).extension().string());
            scan.has_package |= ext == ".pkg";
            scan.has_license |= leaf == "work.bin" || ext == ".rif";
            scan.installables.push_back(name);
        } else {
            const std::string ext = lower(fs::path(name).extension().string());
            if (!ext.empty() && std::ranges::find(scan.other_extensions, ext) == scan.other_extensions.end()
                && scan.other_extensions.size() < 6)
                scan.other_extensions.push_back(ext);
        }
    }

    mz_zip_reader_end(&zip);
    std::fclose(fp);
    if (scan.direct_content)
        scan.installables.clear();
    return scan;
}

// Extracts the named members into `dest`, flattened to their base names so a
// crafted archive cannot write outside it. Returns what landed on disk.
static std::vector<fs::path> extract_nested(const fs::path &archive_path, const fs::path &dest,
    const std::vector<std::string> &members, InstallerUI &ui) {
    std::vector<fs::path> written;
    mz_zip_archive zip{};
    std::memset(&zip, 0, sizeof(zip));

    FILE *fp = std::fopen(archive_path.string().c_str(), "rb");
    if (!fp)
        return written;
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        std::fclose(fp);
        return written;
    }

    for (const std::string &member : members) {
        const mz_uint index = static_cast<mz_uint>(mz_zip_reader_locate_file(&zip, member.c_str(), nullptr, 0));
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat))
            continue;

        const std::string leaf = fs::path(member).filename().string();
        if (leaf.empty() || leaf == "." || leaf == "..")
            continue;
        const fs::path out = dest / leaf;
        ui.step("Unpacking: " + leaf);

        struct Sink {
            FILE *fp;
            uint64_t written;
            uint64_t total;
            InstallerUI *ui;
        } sink{ std::fopen(out.string().c_str(), "wb"), 0, stat.m_uncomp_size, &ui };
        if (!sink.fp) {
            ui.note("FAILED: cannot write " + leaf);
            continue;
        }
        std::vector<char> io_buffer(4u * 1024 * 1024);
        std::setvbuf(sink.fp, io_buffer.data(), _IOFBF, io_buffer.size());

        const auto write_cb = [](void *user, mz_uint64, const void *buf, size_t n) -> size_t {
            Sink *s = static_cast<Sink *>(user);
            if (std::fwrite(buf, 1, n, s->fp) != n)
                return 0;
            s->written += n;
            if (s->total)
                s->ui->progress(static_cast<int>(s->written * 100 / s->total));
            return n;
        };
        const bool ok = mz_zip_reader_extract_to_callback(&zip, index, write_cb, &sink, 0);
        const bool flushed = std::fflush(sink.fp) == 0 && fsync(fileno(sink.fp)) == 0;
        std::fclose(sink.fp);

        if (!ok || !flushed || sink.written != stat.m_uncomp_size) {
            LOG_ERROR("Installer: failed to unpack '{}' from '{}'", member, archive_path.string());
            ui.note("FAILED: could not unpack " + leaf);
            boost::system::error_code ec;
            fs::remove(out, ec);
            continue;
        }
        written.push_back(out);
    }

    mz_zip_reader_end(&zip);
    std::fclose(fp);
    if (!written.empty())
        fsdevCommitDevice("sdmc");
    return written;
}

void run_install(EmuEnvState &emuenv, InstallerUI &ui, const fs::path &target = {}) {
    // A non-empty `target` is a single file the user picked in the launcher's SD file
    // browser: install just that, taking its license (work.bin/.rif/.bin) from the
    // same folder so find_pkg_zrif can place it in ux0/license. Otherwise install
    // everything dropped in sdmc:/switch/vita3k/install.
    boost::system::error_code single_ec;
    const bool single = !target.empty() && fs::is_regular_file(target, single_ec) && !single_ec;
    // Where the launcher stages imports. A single target can now live outside it:
    // the launcher installs an SD-resident archive in place rather than copying it.
    const fs::path staging_dir = fs::path("sdmc:/switch/vita3k/install");
    const fs::path install_dir = single ? target.parent_path() : staging_dir;

    const bool aes_ok = aes_selftest();
    LOG_INFO("Installer AES self-test: {}", aes_ok ? "PASS" : "FAIL (crypto broken!)");
    if (!aes_ok) {
        ui.step("Installer unavailable");
        ui.note("AES self-test failed. Package installation was stopped to avoid writing corrupt data.");
        return;
    }
    if (single)
        LOG_WARN("Installer target: single file '{}'", target.string());

    try {
        if (!fs::exists(install_dir))
            fs::create_directories(install_dir);
    } catch (const std::exception &e) {
        ui.note("Cannot access " + install_dir.string() + ": " + e.what());
    }

    // A zRIF typed into the launcher for the staged package, used when the package
    // has no work.bin next to it. Read once and consumed, so a stale key can never
    // be applied to a later, unrelated install.
    std::string staged_zrif;
    {
        const char *const zrif_marker = "sdmc:/switch/vita3k/install_zrif.txt";
        if (FILE *zf = std::fopen(zrif_marker, "r")) {
            char buf[1024] = { 0 };
            if (std::fgets(buf, sizeof(buf), zf)) {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'
                    || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (!line.empty()) {
                    if (validate_zrif(line))
                        staged_zrif = std::move(line);
                    else
                        ui.note("Ignoring the staged zRIF: it is not a valid license key.");
                }
            }
            std::fclose(zf);
            std::remove(zrif_marker);
        }
    }

    // Categorise the files to install. Order of processing matters: firmware first,
    // then licenses (so find_pkg_zrif can locate them), then packages, then
    // archives (which carry their own contents and need no license).
    std::vector<fs::path> pups, licenses, pkgs, archives;
    const auto categorise = [&](const fs::path &p) {
        const std::string ext = lower(p.extension().string());
        const std::string fn = lower(p.filename().string());
        if (ext == ".pup")
            pups.push_back(p);
        else if (fn == "work.bin" || ext == ".rif" || ext == ".bin")
            licenses.push_back(p);
        else if (ext == ".pkg")
            pkgs.push_back(p);
        else if (ext == ".vpk" || ext == ".zip" || ext == ".vci")
            archives.push_back(p);
    };
    try {
        if (single) {
            categorise(target); // the picked file itself...
            // ...plus a license sitting next to it, but only for a package. An
            // archive carries its own content, and when it is installed in place
            // this folder belongs to the user and may hold unrelated .bin files.
            if (!pkgs.empty()) {
                for (const auto &entry : fs::directory_iterator(install_dir)) {
                    if (!entry.is_regular_file())
                        continue;
                    const std::string fn = lower(entry.path().filename().string());
                    const std::string ext = lower(entry.path().extension().string());
                    if (fn == "work.bin" || ext == ".rif" || ext == ".bin")
                        licenses.push_back(entry.path());
                }
            }
        } else {
            for (const auto &entry : fs::directory_iterator(install_dir)) {
                if (entry.is_regular_file())
                    categorise(entry.path());
            }
        }
    } catch (const std::exception &e) {
        ui.note(std::string("Scan failed: ") + e.what());
    }

    // A .zip holding a .pkg (+ work.bin) or a .vpk rather than the app content
    // itself: unpack those and treat them as ordinary inputs. Anything that has
    // real content is left for install_archive.
    bool pkg_failed = false; // if any package failed/was skipped, keep the work.bin(s) for a retry
    int items_ok = 0, items_failed = 0; // drives the closing summary
    int items_no_license = 0; // of those, the ones a work.bin/zRIF would fix

    std::map<std::string, std::string> archive_hint; // why an archive had nothing to install
    if (!archives.empty()) {
        std::vector<fs::path> direct, unpacked;
        for (const auto &archive : archives) {
            if (lower(archive.extension().string()) == ".vci") {
                direct.push_back(archive);
                continue;
            }
            const NestedScan scan = scan_archive(archive);
            if (scan.direct_content || scan.installables.empty()) {
                if (!scan.direct_content) {
                    std::string kinds;
                    for (const auto &ext : scan.other_extensions)
                        kinds += (kinds.empty() ? "" : " ") + ext;
                    LOG_WARN("Installer: '{}' has no Vita content; it contains: {}",
                        archive.filename().string(), kinds.empty() ? "(nothing recognisable)" : kinds);
                    archive_hint[archive.string()] = kinds.empty()
                        ? "it holds no Vita content"
                        : "it holds only " + kinds;
                }
                direct.push_back(archive);
                continue;
            }
            // Unpacking a multi-gigabyte package takes minutes, and a .pkg without
            // a licence cannot be installed at the end of it. Ask first rather than
            // spending that time and then reporting SKIPPED.
            if (scan.has_package && !scan.has_license && staged_zrif.empty()) {
                bool license_beside = false;
                boost::system::error_code scan_ec;
                for (fs::directory_iterator it(archive.parent_path(), scan_ec), end;
                     !scan_ec && it != end; it.increment(scan_ec)) {
                    const std::string leaf = lower(it->path().filename().string());
                    const std::string ext = lower(it->path().extension().string());
                    if (leaf == "work.bin" || ext == ".rif") {
                        license_beside = true;
                        break;
                    }
                }
                if (!license_beside
                    && !ui.confirm(archive.filename().string()
                            + " holds a .pkg but no licence, and none was found next to it. "
                              "Unpacking takes several minutes and will fail without one.",
                        "Unpack anyway", "Skip and enter a zRIF first")) {
                    ui.note("SKIPPED: no licence. Enter its zRIF in the launcher, or put the work.bin next to the archive.");
                    items_failed++;
                    items_no_license++;
                    pkg_failed = true;
                    continue;
                }
            }

            ui.step("Archive: " + archive.filename().string());
            const auto extracted = extract_nested(archive, staging_dir, scan.installables, ui);
            if (extracted.empty()) {
                direct.push_back(archive);
                continue;
            }
            unpacked.insert(unpacked.end(), extracted.begin(), extracted.end());
        }
        archives = std::move(direct);
        for (const auto &file : unpacked)
            categorise(file);
    }

    if (pups.empty() && licenses.empty() && pkgs.empty() && archives.empty()) {
        ui.step("Nothing to install");
        ui.note("Nothing to install. Put files in " + install_dir.string()
            + " :\n  firmware  -> PSP2UPDAT.PUP\n  game      -> <name>.pkg  (+ its work.bin next to it)"
            + "\n  homebrew  -> <name>.vpk / .zip / .vci");
    }

    // Sources that installed successfully, offered for deletion at the end to free
    // space (firmware PUPs total ~350 MB; a game PKG can be several GB).
    std::vector<fs::path> to_delete;
    std::vector<fs::path> copied_licenses;

    // Rename a processed drop-folder file so the next scan skips it (avoids
    // re-extracting the multi-GB PKG / firmware on every run). Returns the resulting
    // path so the caller can offer it for deletion. A browser-picked single file is
    // left where the user keeps it (the delete prompt still offers to remove it).
    // A source the launcher did not stage into install/ is the user's own file,
    // sitting wherever they keep it. Installing it in place must not rename it or
    // offer to delete it.
    const auto is_staged = [&staging_dir](const fs::path &p) {
        return p.parent_path() == staging_dir;
    };
    const auto mark_done = [single](const fs::path &p) -> fs::path {
        if (single)
            return p;
        const fs::path dst = p.string() + ".installed";
        boost::system::error_code ec;
        fs::rename(p, dst, ec);
        return ec ? p : dst;
    };

    // Each step is wrapped so a single failure (e.g. a license that is already
    // installed) logs and continues instead of aborting with an unhandled
    // exception.
    for (const auto &pup : pups) {
        ui.step("Firmware: " + pup.filename().string());
        try {
            const std::string ver = install_pup(emuenv.vita_fs_path, pup,
                [&](uint32_t pct) { ui.progress(static_cast<int>(pct)); });
            if (ver.empty()) {
                ui.note("FAILED");
                items_failed++;
            } else {
                ui.note("OK (" + ver + ")");
                items_ok++;
                const fs::path done = mark_done(pup);
                if (is_staged(done))
                    to_delete.push_back(done);
            }
        } catch (const std::exception &e) {
            ui.note(std::string("ERROR: ") + e.what());
            items_failed++;
        }
    }

    for (const auto &lic : licenses) {
        ui.step("License: " + lic.filename().string());
        try {
            const bool ok = copy_license(emuenv, lic);
            if (ok) {
                copied_licenses.push_back(lic);
                ui.note("OK");
                items_ok++;
            } else {
                ui.note("FAILED (invalid license or storage error)");
                items_failed++;
            }
        } catch (const std::exception &e) {
            ui.note(std::string("ERROR: ") + e.what());
            items_failed++;
        }
    }

    // Tee std::cout -> logger for the whole PKG stage so psvpfstools' PFS
    // mount/decrypt/keystone diagnostics (and any failure) are recorded in
    // vita3k.log instead of vanishing onto the console.
    std::streambuf *const cout_old = std::cout.rdbuf();
    TeeLogBuf cout_tee(cout_old);
    std::cout.rdbuf(&cout_tee);
    for (const auto &pkg : pkgs) {
        ui.step("Package: " + pkg.filename().string() + " (decrypting, please wait)");
        try {
            std::string zrif = find_pkg_zrif(pkg, emuenv.vita_fs_path);
            if (zrif.empty())
                zrif = staged_zrif; // typed into the launcher for this package
            if (zrif.empty()) {
                LOG_ERROR("No license or zRIF for {}; skipping before extraction.", pkg.filename().string());
                ui.note("SKIPPED: no license. Put the game's work.bin next to the .pkg, or enter its zRIF in the launcher.");
                items_failed++;
                items_no_license++;
                pkg_failed = true;
                continue;
            }
            bool ok = install_pkg(pkg, emuenv, zrif,
                [&](float pct) { ui.progress(static_cast<int>(pct)); });
            LOG_WARN("install_pkg('{}') returned {}", pkg.filename().string(), ok);

            // Same check as for archives: the launcher lists a game by finding
            // ux0/app/<id>/sce_sys/param.sfo, so confirm it is really there.
            const std::string &title_id = emuenv.app_info.app_title_id;
            if (ok && !title_id.empty() && !emuenv.app_info.app_category.contains("gp")) {
                const fs::path sfo = emuenv.vita_fs_path / "ux0/app" / title_id / "sce_sys" / "param.sfo";
                boost::system::error_code ec;
                if (!fs::is_regular_file(sfo, ec) || ec) {
                    LOG_ERROR("Installer: {} reported success but {} is missing", title_id, sfo.string());
                    ui.note("FAILED: did not land in ux0/app/" + title_id);
                    ok = false;
                }
            }

            // The license was already resolved above, so a failure here is not
            // about the license; say what it is instead of guessing.
            if (ok) {
                ui.note("OK" + (title_id.empty() ? std::string() : ": " + title_id));
                items_ok++;
                const fs::path done = mark_done(pkg);
                if (is_staged(done))
                    to_delete.push_back(done);
            } else {
                ui.note("FAILED (decryption or extraction error - see the log)");
                items_failed++;
                pkg_failed = true;
            }
        } catch (const std::exception &e) {
            LOG_ERROR("install_pkg('{}') threw: {}", pkg.filename().string(), e.what());
            ui.note(std::string("ERROR: ") + e.what());
            items_failed++;
            pkg_failed = true;
        }
    }
    std::cout.rdbuf(cout_old);

    // Archives (.vpk / .zip / .vci) carry their own contents and need no license.
    // One archive can hold several of them - a game plus its patch and DLC - so
    // report each installed content by name rather than as a single bar.
    for (const auto &archive : archives) {
        ui.step("Archive: " + archive.filename().string());
        try {
            float content_total = 0.f;
            float content_done = 0.f;
            const auto on_progress = [&](ArchiveContents c) {
                if (c.count)
                    content_total = *c.count;
                if (c.current)
                    content_done = *c.current;
                // Weight the per-content percentage into the archive as a whole so the
                // bar advances across every content instead of restarting each time.
                const float within = c.progress ? *c.progress : 0.f;
                if (content_total > 0.f) {
                    const float base = (content_done > 0.f ? content_done - 1.f : 0.f) / content_total;
                    ui.progress(static_cast<int>((base + (within / 100.f) / content_total) * 100.f));
                } else {
                    ui.progress(static_cast<int>(within));
                }
            };
            // Only the archive knows what it contains, so the overwrite question can
            // only be asked here, once per already-installed content.
            const auto on_reinstall = [&](const std::string &title, const std::string &title_id) {
                return ui.confirm(title + " (" + title_id + ") is already installed.",
                    "Reinstall it", "Keep the installed copy");
            };

            const std::vector<ContentInfo> installed = install_archive(emuenv, archive, on_progress, on_reinstall);
            if (installed.empty()) {
                const auto hint = archive_hint.find(archive.string());
                ui.note(hint == archive_hint.end()
                        ? std::string("FAILED (not a readable .vpk/.zip/.vci)")
                        : "FAILED: " + hint->second + ". A game .zip must contain the app folder, a .pkg, or a .vpk.");
                items_failed++;
                pkg_failed = true;
            } else {
                std::size_t ok_count = 0;
                for (const auto &content : installed) {
                    const std::string label = content.title.empty() ? content.title_id : content.title;
                    const std::string suffix = content.category.empty() ? "" : " [" + content.category + "]";
                    bool state = content.state;
                    // The launcher lists a game by finding ux0/app/<id>/sce_sys/param.sfo.
                    // Check the same thing here so "installed" cannot mean an app that
                    // never shows up, and so the log names the path when it does not.
                    const bool is_app = state && !content.title_id.empty()
                        && content.category != "ac" && !content.category.contains("gp");
                    if (is_app) {
                        const fs::path sfo = emuenv.vita_fs_path / "ux0/app" / content.title_id / "sce_sys" / "param.sfo";
                        boost::system::error_code ec;
                        if (!fs::is_regular_file(sfo, ec) || ec) {
                            LOG_ERROR("Installer: {} reported success but {} is missing",
                                content.title_id, sfo.string());
                            ui.note("FAILED: " + label + " did not land in ux0/app/" + content.title_id);
                            state = false;
                        }
                    }
                    if (state)
                        ui.note("OK: " + label + suffix);
                    else if (content.state == state)
                        ui.note("FAILED: " + label + suffix);
                    ok_count += state ? 1 : 0;
                    (state ? items_ok : items_failed)++;
                }
                if (ok_count == installed.size()) {
                    const fs::path done = mark_done(archive);
                    if (is_staged(done))
                        to_delete.push_back(done);
                } else {
                    pkg_failed = true;
                }
            }
        } catch (const std::exception &e) {
            LOG_ERROR("install_archive('{}') threw: {}", archive.filename().string(), e.what());
            ui.note(std::string("ERROR: ") + e.what());
            items_failed++;
            pkg_failed = true;
        }
    }

    // Once every package succeeded, the license/work.bin sources are redundant (the
    // license was copied into ux0/license). Keep them if anything failed so a retry
    // still has them. Firmware-only runs have no licenses, so this adds nothing.
    if (!pkg_failed)
        for (const auto &lic : copied_licenses)
            if (is_staged(lic))
                to_delete.push_back(lic);

    // Offer to delete the now-redundant source files to reclaim SD space.
    if (!to_delete.empty()) {
        uintmax_t total = 0;
        for (const auto &f : to_delete) {
            boost::system::error_code ec;
            const uintmax_t s = fs::file_size(f, ec);
            if (!ec)
                total += s;
        }
        char szbuf[32];
        if (total >= (1ull << 30))
            std::snprintf(szbuf, sizeof(szbuf), "%.1f GB", static_cast<double>(total) / (1ull << 30));
        else
            std::snprintf(szbuf, sizeof(szbuf), "%.0f MB", static_cast<double>(total) / (1ull << 20));
        const std::string prompt = "Delete " + std::to_string(to_delete.size())
            + " source file(s) (" + szbuf + ") to free space?";
        if (ui.confirm(prompt, "Delete and free space", "Keep the files")) {
            int deleted = 0;
            for (const auto &f : to_delete) {
                boost::system::error_code ec;
                if (fs::remove(f, ec))
                    deleted++;
            }
            ui.note("Deleted " + std::to_string(deleted) + " source file(s).");
        } else {
            ui.note("Kept source files.");
        }
    }

    // Say what actually happened. Reporting "Done." after a silent failure is how
    // an install that produced nothing still looked like it had worked.
    std::string summary;
    if (items_ok == 0 && items_failed == 0)
        summary = "Nothing to install.";
    else if (items_failed == 0)
        summary = "Done. " + std::to_string(items_ok) + (items_ok == 1 ? " item installed." : " items installed.");
    else if (items_ok == 0)
        summary = "Failed. Nothing was installed - see the log.";
    else
        summary = "Done. " + std::to_string(items_ok) + " installed, "
            + std::to_string(items_failed) + " not.";
    if (items_no_license > 0)
        summary += " " + std::to_string(items_no_license)
            + (items_no_license == 1 ? " needs its own work.bin or zRIF." : " need their own work.bin or zRIF.");
    ui.done(summary);
}

} // namespace

void run_switch_installer(EmuEnvState &emuenv) {
    // Faster install: enable CPU boost (overclock) for the whole install. FastLoad
    // also throttles the GPU, which is fine here - the install is crypto + SD-IO
    // bound and only paints a tiny progress UI.
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

    // Keep the work on the application cores (0/1/2) and OFF core 3, which is
    // reserved for the OS on homebrew. mask 0b0111 = cores 0,1,2; a running thread
    // can only occupy one core at a time, but this lets the scheduler pick the
    // least-loaded application core instead of pinning to the system core.
    const Result core_rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0b0111u);
    if (R_FAILED(core_rc))
        LOG_WARN("Installer: could not restrict to cores 0/1/2 (rc=0x{:X}); using default affinity.", core_rc);

    // The launcher's SD file browser writes the picked file's path here; install just
    // that file. Read+consume it (a stale marker must not hijack a later folder
    // install). Absent => install everything in sdmc:/switch/vita3k/install. Read via
    // C stdio so a path containing spaces survives intact (unlike argv parsing).
    fs::path target;
    const char *marker = "sdmc:/switch/vita3k/install_target.txt";
    if (FILE *mf = std::fopen(marker, "r")) {
        char buf[1024] = { 0 };
        if (std::fgets(buf, sizeof(buf), mf)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty())
                target = fs::path(line);
        }
        std::fclose(mf);
        std::remove(marker); // consume so it applies exactly once
        if (!target.empty())
            LOG_WARN("Installer: file browser target = '{}'", target.string());
    }

    // Prefer the console-free graphical screen. The emulator has not created its
    // Vulkan surface yet at the --install stage, so the default window's
    // framebuffer is free. If it cannot be created, fall back to the old libnx
    // text console so installation still works.
    GfxUI gfx;
    if (gfx.ok()) {
        run_install(emuenv, gfx, target);
    } else {
        LOG_WARN("Installer: framebuffer unavailable, falling back to text console.");
        ConsoleUI console;
        run_install(emuenv, console, target);
    }

    // Restore the default clocks so the returning launcher / next game is not left
    // in a throttled-GPU boost state.
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}
