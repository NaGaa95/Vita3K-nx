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

#include <renderer/shaders.h>

#include <renderer/vulkan/state.h>

#include <gxm/types.h>
#include <renderer/state.h>
#include <renderer/types.h>
#include <shader/spirv_recompiler.h>
#include <util/fs.h>
#include <util/log.h>

#include <string>
#include <util/switch_storage.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef __SWITCH__
#include <map>
#include <unordered_set>
#endif

namespace renderer {

#ifdef __SWITCH__
class ShaderCacheIndex {
    struct Directory {
        std::unordered_set<fs::path::string_type> files;
        bool scanned = false;
        bool complete = false;
        bool prepared = false;
    };

    std::mutex mutex;
    std::map<fs::path, Directory> directories;

public:
    void reset() {
        std::lock_guard lock(mutex);
        directories.clear();
    }

    void invalidate(const fs::path &path) {
        std::lock_guard lock(mutex);
        directories.erase(path);
    }

    bool may_contain(const fs::path &path) {
        std::lock_guard lock(mutex);
        auto &directory = directories[path.parent_path()];
        if (!directory.scanned) {
            std::error_code ec;
            fs::directory_iterator entry(path.parent_path(), ec), end;
            if (ec == std::errc::no_such_file_or_directory) {
                directory.complete = true;
            } else if (!ec) {
                for (; entry != end && !ec; entry.increment(ec))
                    directory.files.insert(entry->path().filename().native());
                directory.complete = !ec;
                directory.prepared = !ec;
            }
            directory.scanned = true;
            if (!directory.complete)
                directory.files.clear();
        }
        return !directory.complete || directory.files.contains(path.filename().native());
    }

    bool prepare(const fs::path &path) {
        std::lock_guard lock(mutex);
        auto &directory = directories[path];
        if (directory.prepared)
            return true;

        std::error_code ec;
        const bool created = fs::create_directories(path, ec);
        if (ec) {
            LOG_WARN("Failed to create directory {}: {}", path, ec.message());
            return false;
        }
        directory.prepared = true;
        if (created) {
            directory.files.clear();
            directory.scanned = directory.complete = true;
        }
        return true;
    }

    void written(const fs::path &path, bool success) {
        std::lock_guard lock(mutex);
        auto &directory = directories[path.parent_path()];
        if (success) {
            if (directory.complete)
                directory.files.insert(path.filename().native());
        } else {
            directory.prepared = false;
            directory.complete = false;
        }
    }
};

static ShaderCacheIndex shader_cache_index;
#endif

static bool create_directories_noexcept(const fs::path &path) {
#ifdef __SWITCH__
    return shader_cache_index.prepare(path);
#else
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        LOG_WARN("Failed to create directory {}: {}", path, ec.message());
        return false;
    }
    return true;
#endif
}

static void remove_all_noexcept(const fs::path &path) {
#ifdef __SWITCH__
    shader_cache_index.invalidate(path);
#endif
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec)
        LOG_WARN("Failed to remove stale shader path {}: {}", path, ec.message());
}

bool get_shaders_cache_hashs(State &renderer) {
#ifdef __SWITCH__
    shader_cache_index.reset();
#endif
    const std::string hash_file_name = fmt::format("hashs-{}.dat", (renderer.current_backend == Backend::OpenGL) ? "gl" : "vk");

    const auto hash_path = renderer.shaders_path / hash_file_name;
    std::ifstream shaders_hashs(hash_path, std::ios::in | std::ios::binary);
    if (!shaders_hashs.is_open())
        return false;

    renderer.shaders_cache_hashs.clear();
    // Read size of hashes list
    size_t size = 0;
    shaders_hashs.read((char *)&size, sizeof(size));

    // Check version of cache
    uint32_t versionInFile = 0;
    shaders_hashs.read((char *)&versionInFile, sizeof(uint32_t));
    uint32_t features_mask = 0;
    shaders_hashs.read((char *)&features_mask, sizeof(uint32_t));
    if (!shaders_hashs)
        return false;
    const auto entries_begin = shaders_hashs.tellg();
    shaders_hashs.seekg(0, std::ios::end);
    const auto remaining = shaders_hashs.tellg() - entries_begin;
    if (remaining < 0 || size > static_cast<size_t>(remaining) / (2 * sizeof(Sha256Hash)))
        return false;
    shaders_hashs.seekg(entries_begin);
    if (versionInFile != shader::CURRENT_VERSION || features_mask != renderer.get_features_mask()) {
        shaders_hashs.close();
        remove_all_noexcept(renderer.shaders_path);
        remove_all_noexcept(renderer.shaders_log_path);
        if (versionInFile != shader::CURRENT_VERSION)
            LOG_WARN("Current version of cache: {}, is outdated, recreate it.", versionInFile);
        else
            LOG_WARN("Incompatible GPU features enabled, recreating shader cache");
        return false;
    }

    if (renderer.current_backend == Backend::Vulkan) {
        // Read the pipeline cache
        dynamic_cast<vulkan::VKState &>(renderer).pipeline_cache.read_pipeline_cache();
    }

    // Read Hashs info value
    for (size_t a = 0; a < size; a++) {
        auto read = [&shaders_hashs]() {
            Sha256Hash hash{};

            shaders_hashs.read(reinterpret_cast<char *>(hash.data()), sizeof(Sha256Hash));

            return hash;
        };

        ShadersHash hash;
        hash.frag = read();
        hash.vert = read();
        if (!shaders_hashs) {
            renderer.shaders_cache_hashs.clear();
            return false;
        }

        renderer.shaders_cache_hashs.push_back({ hash.frag, hash.vert });
    }

    shaders_hashs.close();

    return !renderer.shaders_cache_hashs.empty();
}

static std::vector<uint8_t> serialize_shader_hashes(State &renderer, const std::vector<ShadersHash> &hashes) {
    const size_t count = hashes.size();
    const uint32_t version = shader::CURRENT_VERSION;
    const uint32_t features = renderer.get_features_mask();
    std::vector<uint8_t> data(sizeof(count) + sizeof(version) + sizeof(features) + count * 2 * sizeof(Sha256Hash));
    auto cursor = data.data();
    const auto append = [&](const void *value, size_t size) {
        std::memcpy(cursor, value, size);
        cursor += size;
    };
    append(&count, sizeof(count));
    append(&version, sizeof(version));
    append(&features, sizeof(features));
    for (const auto &hash : hashes) {
        append(hash.frag.data(), sizeof(Sha256Hash));
        append(hash.vert.data(), sizeof(Sha256Hash));
    }
    return data;
}

void save_shaders_cache_hashs(State &renderer, std::vector<ShadersHash> &shaders_cache_hashs) {
    if (!create_directories_noexcept(renderer.shaders_path))
        return;
    std::string hash_file_name = fmt::format("hashs-{}.dat", (renderer.current_backend == Backend::OpenGL) ? "gl" : "vk");
    std::ofstream shaders_hashs(renderer.shaders_path / hash_file_name, std::ios::out | std::ios::binary);

    if (shaders_hashs.is_open()) {
        const auto data = serialize_shader_hashes(renderer, shaders_cache_hashs);
        shaders_hashs.write(reinterpret_cast<const char *>(data.data()), data.size());
        shaders_hashs.close();
    }
}

template <typename R>
static R load_shader_generic(const fs::path &shader_path) {
#ifdef __SWITCH__
    if (!shader_cache_index.may_contain(shader_path)) {
        return {};
    }
#endif
    std::ifstream file(shader_path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};

    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0 || size % sizeof(typename R::value_type) != 0)
        return {};

    R source;
    source.resize(static_cast<size_t>(size) / sizeof(typename R::value_type));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(source.data()), size))
        return {};

    return source;
}

static void save_shader_data(const fs::path &path, const void *data, size_t size) {
#ifdef __SWITCH__
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char *>(data), size);
    file.close();
    const bool success = !file.fail();
    shader_cache_index.written(path, success);
    if (!success)
        LOG_WARN("Failed to write shader cache {}", path);
#else
    fs_utils::dump_data(path, data, size);
#endif
}

static shader::GeneratedShader load_shader_generic(shader::Target target, const SceGxmProgram &program, const Sha256Hash &hash, const FeatureState &features, const shader::Hints &hints, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shaderlog_path, const char *shader_type_str, const std::string &shader_version, bool shader_cache, bool shader_debug_dump, bool lookup_cache = true) {
    const std::string hash_text = hex_string(hash);
    // Set Shader Hash with Version
    const std::string hash_hex_ver = fmt::format("{}-{}", shader_version, hash_text);
    const auto get_shader_path = [&](const char *ext) {
        return shader_cache_path / fmt::format("{}.{}", hash_hex_ver, ext);
    };
    const auto get_shaderlog_path = [&](const char *ext) {
        return shaderlog_path / fmt::format("{}.{}", hash_hex_ver, ext);
    };

    const auto shader_path = get_shader_path(shader_type_str);
    if (shader_cache && lookup_cache) {
        if (target == shader::Target::GLSLOpenGL) {
            std::string source = load_shader_generic<std::string>(shader_path);
            if (!source.empty()) {
                return { source, std::vector<uint32_t>() };
            }
        } else {
            std::vector<uint32_t> source = load_shader_generic<std::vector<uint32_t>>(get_shader_path("spv"));
            if (!source.empty())
                return { "", source };
        }
    }

    LOG_INFO("Generating {} shader {}", shader_type_str, hash_text);

    const bool cache_directory_ready = !shader_cache || create_directories_noexcept(shader_cache_path);
    bool write_shader_debug_files = shader_debug_dump;
    fs::path shader_log_path;
    if (write_shader_debug_files) {
        write_shader_debug_files = create_directories_noexcept(shaderlog_path);
        if (write_shader_debug_files) {
            shader_log_path = get_shaderlog_path("gxp");
            fs_utils::dump_data(shader_log_path, &program, program.size);
        }
    }

    const auto write_data_with_ext = [&](const std::string &ext, const std::string &data) {
        fs::path out_path;
        if (ext == shader_type_str && target == shader::Target::GLSLOpenGL) {
            if (!shader_cache || !cache_directory_ready)
                return true;
            out_path = shader_path;
        } else {
            if (!write_shader_debug_files)
                return true;
            out_path = shader_log_path;
            out_path.replace_extension(ext);
        }
        save_shader_data(out_path, data.c_str(), data.size());
        return true;
    };

    std::function<bool(const std::string &, const std::string &)> dumper;
    if (write_shader_debug_files || target == shader::Target::GLSLOpenGL)
        dumper = write_data_with_ext;

    // Keep the embedded source name independent of cache variants.
    const auto source_hash = hex_string(sha256(&program, program.size));
    auto source = shader::convert_gxp(program, source_hash, features, target, hints, maskupdate, false, dumper);

    // The binary SPIR-V cache is required for performance; textual disassembly
    // and GXP dumps above are optional diagnostics.
    if (shader_cache && cache_directory_ready && target != shader::Target::GLSLOpenGL) {
        const auto shader_dst_path = get_shader_path("spv");
        save_shader_data(shader_dst_path, source.spirv.data(), sizeof(uint32_t) * source.spirv.size());
    }

    // Commit storage periodically instead of after every shader.
    if (shader_cache && cache_directory_ready) {
        static std::mutex commit_mutex;
        static std::chrono::steady_clock::time_point next_commit{};
        const auto now = std::chrono::steady_clock::now();
        const std::lock_guard<std::mutex> lock(commit_mutex);
        if (now >= next_commit) {
            next_commit = now + std::chrono::seconds(5);
            switch_commit_storage();
        }
    }

    return source;
}

std::string load_glsl_shader(const SceGxmProgram &program, const Sha256Hash &hash, const FeatureState &features, const shader::Hints &hints, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shader_log_path, const std::string &shader_version, bool shader_cache, bool shader_debug_dump) {
    SceGxmProgramType program_type = program.get_type();

    auto shader_type_to_str = [](SceGxmProgramType type) {
        return (type == SceGxmProgramType::Vertex) ? "vert" : ((type == SceGxmProgramType::Fragment) ? "frag" : "unknown");
    };

    const char *shader_type_str = shader_type_to_str(program_type);

    return load_shader_generic(shader::Target::GLSLOpenGL, program, hash, features, hints, maskupdate, shader_cache_path, shader_log_path, shader_type_str, shader_version, shader_cache, shader_debug_dump).glsl;
}

std::vector<uint32_t> load_spirv_shader(const SceGxmProgram &program, const Sha256Hash &hash, const FeatureState &features, bool is_vulkan, const shader::Hints &hints, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shader_log_path, const std::string &shader_version, bool shader_cache, bool shader_debug_dump, bool lookup_cache) {
    const shader::Target target = is_vulkan ? shader::Target::SpirVVulkan : shader::Target::SpirVOpenGL;
    auto shader_type_to_str = [](SceGxmProgramType type) {
        return (type == SceGxmProgramType::Vertex) ? "vert.spv.txt" : ((type == SceGxmProgramType::Fragment) ? "frag.spv.txt" : "unknown.spv.txt");
    };
    const char *shader_type_str = shader_type_to_str(program.get_type());

    return load_shader_generic(target, program, hash, features, hints, maskupdate, shader_cache_path, shader_log_path, shader_type_str, shader_version, shader_cache, shader_debug_dump, lookup_cache).spirv;
}

std::string pre_load_shader_glsl(const fs::path &shader_path) {
    return load_shader_generic<std::string>(shader_path);
}

std::vector<uint32_t> pre_load_shader_spirv(const fs::path &shader_path) {
    return load_shader_generic<std::vector<uint32_t>>(shader_path);
}

} // namespace renderer
