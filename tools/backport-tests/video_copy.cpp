#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

struct AVFrame {
    uint8_t *data[3];
    int linesize[3];
    int width, height;
};
#include "video_copy.inc"

int main() {
    std::array<uint8_t, 24> y;
    std::array<uint8_t, 8> u, v;
    for (size_t i = 0; i < y.size(); ++i)
        y[i] = static_cast<uint8_t>(i + 1);
    for (size_t i = 0; i < u.size(); ++i) {
        u[i] = static_cast<uint8_t>(i + 50);
        v[i] = static_cast<uint8_t>(i + 100);
    }
    AVFrame frame{ { y.data(), u.data(), v.data() }, { 6, 4, 4 }, 4, 4 };
    for (const uint32_t width : { 2u, 4u, 8u }) {
        for (const uint32_t height : { 2u, 4u, 8u }) {
            for (const bool planar : { false, true }) {
                const size_t size = width * height * 3 / 2;
                std::vector<uint8_t> actual(size + 2, 0xA5), expected = actual;
                const uint32_t cw = std::min(width, 4u), ch = std::min(height, 4u);
                for (uint32_t row = 0; row < ch; ++row)
                    for (uint32_t col = 0; col < cw; ++col)
                        expected[1 + row * width + col] = y[row * 6 + col];
                for (uint32_t row = 0; row < ch / 2; ++row) {
                    for (uint32_t col = 0; col < cw / 2; ++col) {
                        const size_t base = 1 + width * height;
                        if (planar) {
                            expected[base + row * width / 2 + col] = u[row * 4 + col];
                            expected[base + width * height / 4 + row * width / 2 + col] = v[row * 4 + col];
                        } else {
                            expected[base + row * width + 2 * col] = u[row * 4 + col];
                            expected[base + row * width + 2 * col + 1] = v[row * 4 + col];
                        }
                    }
                }
                copy_yuv_data_from_frame(&frame, actual.data() + 1, width, height, planar);
                assert(actual == expected);
            }
        }
    }
    std::cout << "Video copy: planar/interleaved output, padded source strides and smaller/larger pictures passed\n";
}
