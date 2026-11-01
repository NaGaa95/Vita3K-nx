#include <mem/state.h>
#include <mem/ptr.h>
#include <np/common.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>

struct EmuEnv {
    MemState mem;
    struct { std::string user_name; } io;
};
bool is_valid_addr_range_size(const MemState &, Address start, uint64_t size) {
    return start >= 64 && static_cast<uint64_t>(start) + size <= 4096;
}
#define EXPORT(type, name, ...) type name(EmuEnv &emuenv __VA_OPT__(, ) __VA_ARGS__)
#define TRACY_FUNC(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define RET_ERROR(error) static_cast<int>(error)
#define CALL_EXPORT(name, ...) name(emuenv, __VA_ARGS__)
#include "np_cache.inc"

int main() {
    EmuEnv env;
    env.mem.memory = Memory(new uint8_t[4096], [](uint8_t *p) { delete[] p; });
    std::memset(env.mem.memory.get(), 0xA5, 4096);
    const Ptr<SceNpManagerCacheParam> output(256);
    assert(sceNpManagerGetCachedParam(env, {}) == static_cast<int>(SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT));
    assert(sceNpManagerGetCachedParam(env, Ptr<SceNpManagerCacheParam>(4090)) == static_cast<int>(SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT));
    for (const std::string name : { "Player", "1234567890ABCDEF" }) {
        env.io.user_name = name;
        assert(sceNpManagerGetCachedParam(env, output) == 0);
        assert(std::memcmp(output.get(env.mem)->npId.handle.data, name.data(), name.size()) == 0);
        assert(output.get(env.mem)->npId.isIdValid);
        assert(std::memcmp(output.get(env.mem)->npId.opt.platformType, "psp2", 4) == 0);
        const auto &avatar = output.get(env.mem)->avatarUrl.data;
        assert(std::all_of(std::begin(avatar), std::end(avatar), [](char c) { return c == 0; }));
        assert(env.mem.memory[255] == 0xA5 && env.mem.memory[256 + sizeof(SceNpManagerCacheParam)] == 0xA5);
    }
    env.io.user_name.assign(17, 'x');
    assert(sceNpManagerGetCachedParam(env, output) == static_cast<int>(SCE_NP_MANAGER_ERROR_ID_NOT_AVAIL));
    std::cout << "NP cache: output ABI, pointer bounds, account ID, cleared avatar and existing account errors passed\n";
}
