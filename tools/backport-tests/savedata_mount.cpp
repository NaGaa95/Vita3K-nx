#include <sys/stat.h>

#include <emuenv/app_util.h>
#include <io/device.h>
#include <io/io.h>
#include <io/state.h>
#include <mem/state.h>
#include <util/log.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

struct EmuEnv {
    MemState mem;
    IOState io;
    fs::path vita_fs_path;
    std::map<std::string, std::string> sfo_handle;
};
namespace sfo {
bool get_data_by_key(std::string &out, const std::map<std::string, std::string> &sfo, const std::string &key) {
    const auto it = sfo.find(key);
    if (it == sfo.end())
        return false;
    out = it->second;
    return true;
}
}
namespace fs_utils {
fs::path construct_file_name(const fs::path &, const fs::path &, const fs::path &, const fs::path &) {
    std::abort();
}
}
namespace string_utils {
void replace(std::string &s, std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}
}
bool is_valid_addr_range_size(const MemState &, Address start, uint64_t size) {
    return start >= 64 && static_cast<uint64_t>(start) + size <= 4096;
}
#define EXPORT(type, name, ...) type name(EmuEnv &emuenv __VA_OPT__(, ) __VA_ARGS__)
#undef TRACY_FUNC
#undef RET_ERROR
#define TRACY_FUNC(...) ((void)0)
#define RET_ERROR(error) static_cast<int>(error)
#include "savedata_mount.inc"

int main(int argc, char **argv) {
    assert(argc == 2);
    EmuEnv env;
    env.mem.memory = Memory(new uint8_t[4096]{}, [](uint8_t *p) { delete[] p; });
    env.io.user_id = "00";
    env.io.device_paths.savedata0 = "user/00/savedata/PCSF00001";
    env.vita_fs_path = fs::absolute(argv[1]) / "savedata-fixture";
    fs::create_directories(env.vita_fs_path / "ux0/user/00/savedata/PCSF00002");
    const Ptr<SceAppUtilTitleId> title(128);
    const Ptr<SceAppUtilPassCode> passcode(256);
    std::memcpy(title.get(env.mem)->data, "PCSF00002", 10);
    const auto mount = [&] { return sceAppUtilSaveDataMount(env, title, passcode); };
    assert(sceAppUtilSaveDataUmount(env) == static_cast<int>(SCE_APPUTIL_ERROR_NOT_MOUNTED));
    assert(sceAppUtilSaveDataMount(env, {}, passcode) == static_cast<int>(SCE_APPUTIL_ERROR_PARAMETER));
    assert(mount() == static_cast<int>(SCE_APPUTIL_ERROR_PARAMETER));
    env.sfo_handle["INSTALL_DIR_SAVEDATA_ADD_1"] = "PCSF00003";
    std::memcpy(title.get(env.mem)->data, "PCSF00003", 10);
    assert(mount() == static_cast<int>(SCE_ERROR_ERRNO_ENOENT));
    std::memcpy(title.get(env.mem)->data, "../bad/id", 10);
    assert(mount() == static_cast<int>(SCE_APPUTIL_ERROR_PARAMETER));
    std::memcpy(title.get(env.mem)->data, "PCSF00002", 10);
    env.sfo_handle["INSTALL_DIR_SAVEDATA_ADD_1"] = "PCSF00002";
    assert(mount() == 0);
    assert(mount() == static_cast<int>(SCE_APPUTIL_ERROR_BUSY));
    auto device = VitaIoDevice::savedata1;
    assert(translate_path("savedata1:/file.dat", device, env.io.device_paths) == "user/00/savedata/PCSF00002/file.dat");
    assert(device == VitaIoDevice::ux0);
    device = VitaIoDevice::savedata0;
    assert(translate_path("savedata0:/file.dat", device, env.io.device_paths) == "user/00/savedata/PCSF00001/file.dat");
    assert(sceAppUtilSaveDataUmount(env) == 0);
    device = VitaIoDevice::savedata1;
    assert(translate_path("savedata1:/file.dat", device, env.io.device_paths).empty());
    env.sfo_handle.clear();
    env.sfo_handle["INSTALL_DIR_SAVEDATA_ADD1"] = "PCSF00002";
    int results[2];
    std::thread first([&] { results[0] = mount(); });
    std::thread second([&] { results[1] = mount(); });
    first.join();
    second.join();
    assert((results[0] == 0) != (results[1] == 0));
    assert(results[0] == static_cast<int>(SCE_APPUTIL_ERROR_BUSY) || results[1] == static_cast<int>(SCE_APPUTIL_ERROR_BUSY));
    assert(sceAppUtilSaveDataUmount(env) == 0);
    std::cout << "Savedata mount: authorization, missing saves, path translation, unmount and concurrent mount passed\n";
}
