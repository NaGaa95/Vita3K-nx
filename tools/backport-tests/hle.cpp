// Isolated tests of production HLE bodies. Codec output and texture creation
// are fixtures; guest pointers, surface types, swizzles and UID storage are real.
#include <gxm/functions.h>
#include <kernel/kernel_object.h>
#include <renderer/gxm_types.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <util/lock_and_find.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <set>

enum class DecoderQuery { CHANNELS };
struct DecoderSize {
    uint32_t samples;
};
struct DecoderState {
    uint32_t channels;
    uint32_t next_frame = 0;
    explicit DecoderState(uint32_t channels)
        : channels(channels) {}
    uint32_t get(DecoderQuery) const { return channels; }
    bool send(const uint8_t *, uint32_t) {
        ++next_frame;
        return true;
    }
    uint32_t get_es_size() const { return 1; }
    bool receive(uint8_t *output, DecoderSize *size) {
        size->samples = 4;
        if (output) {
            for (uint32_t i = 0; i < 4 * channels; ++i) {
                const int16_t sample = static_cast<int16_t>((next_frame - 1) * 4 * channels + i);
                memcpy(output + i * sizeof(sample), &sample, sizeof(sample));
            }
        }
        return true;
    }
};
constexpr int SCE_AUDIODEC_TYPE_AT9 = 0;
struct AudiodecState {
    std::mutex mutex;
    std::map<SceUID, std::shared_ptr<DecoderState>> decoders;
    std::map<int, std::set<SceUID>> codecs;
};
struct SceAudiodecInfo;
constexpr SceUID thread_id = 1;
static auto system_event_user_data(SceUID) { return 0; }
struct SimpleEvent : WithUidClass<KernelObject, UidClass::simple_event> {
    std::string name;
    SceUInt32 attr = SCE_KERNEL_ATTR_OPENABLE;
    int set_system_event(auto...) { return SCE_KERNEL_OK; }
    bool deleted = false;
    bool mark_deleted(SceUID) { return !std::exchange(deleted, true); }
};
struct EventFlag : WithUidClass<KernelObject, UidClass::event_flag> {
    std::string name;
};
enum { SAVEDATA_DIALOG = 1,
    OTHER_DIALOG = 2 };
enum { SCE_COMMON_DIALOG_STATUS_NONE = 0,
    SCE_COMMON_DIALOG_STATUS_RUNNING = 1,
    SCE_COMMON_DIALOG_STATUS_FINISHED = 2 };
struct EmuEnv {
    MemState mem;
    struct {
        KernelObjects objects;
        struct {
            std::shared_ptr<AudiodecState> audio = std::make_shared<AudiodecState>();
            template <typename T>
            std::shared_ptr<T> get() { return audio; }
        } obj_store;
    } kernel;
    struct {
        int type = 0, status = 0, substatus = 0;
    } common_dialog;
};
template <typename... Args>
int texture_init(Args...) { return 0; }
template <typename T>
T align(T value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
#define EXPORT(type, name, ...) type name(EmuEnv &emuenv __VA_OPT__(, ) __VA_ARGS__)
#define TRACY_FUNC(...) ((void)0)
#define RET_ERROR(error) static_cast<int>(error)
#define STUBBED(...) 0
#define UNIMPLEMENTED(...) 0
#define CALL_EXPORT(name, ...) texture_init(__VA_ARGS__)
#define LOG_WARN(...) ((void)0)
#include "hle_functions.inc"

static void test_pcm() {
    for (const uint32_t channels : { 1u, 2u }) {
        for (const uint32_t offset : { 0u, 1u, 3u, 4u, 7u }) {
            for (const uint32_t count : { 1u, 2u, 5u }) {
                EmuEnv emuenv;
                emuenv.mem.memory = Memory(new uint8_t[4096], [](uint8_t *p) { delete[] p; });
                std::fill_n(emuenv.mem.memory.get(), 4096, 0xCD);
                auto state = emuenv.kernel.obj_store.audio;
                state->decoders[1] = std::make_shared<DecoderState>(channels);
                state->codecs[SCE_AUDIODEC_TYPE_AT9].insert(1);
                SceAudiodecCtrl ctrl{};
                ctrl.size = sizeof(ctrl);
                ctrl.handle = 1;
                ctrl.es_data = Ptr<uint8_t>(64);
                ctrl.es_size_max = 16;
                ctrl.pcm_data = Ptr<uint8_t>(256);
                ctrl.pcm_size_max = count * channels * sizeof(int16_t);
                assert(sceAudiodecPartlyDecode(emuenv, &ctrl, offset, count) == 0);
                for (uint32_t i = 0; i < count * channels; ++i) {
                    int16_t sample;
                    memcpy(&sample, emuenv.mem.memory.get() + 256 + i * 2, 2);
                    assert(sample == static_cast<int16_t>(offset * channels + i));
                }
                assert(emuenv.mem.memory[255] == 0xCD);
                assert(emuenv.mem.memory[256 + ctrl.pcm_size_max] == 0xCD);
            }
        }
    }
    std::cout << "PCM: 30 offset/channel/count cases with destination guards passed\n";
}

static void test_surface() {
    static_assert(sizeof(SceGxmColorSurface) == 48);
    static_assert(offsetof(SceGxmColorSurface, outputRegisterSize) == 28);
    static_assert(offsetof(SceGxmColorSurface, backgroundTex) == 32);
    EmuEnv emuenv;
    struct {
        uint32_t before;
        SceGxmColorSurface surface;
        uint32_t after;
    } guarded{};
    guarded.before = guarded.after = 0xDEADBEEF;
    auto &surface = guarded.surface;
    for (uint32_t dimension : { 1u, 2u, 960u, 4096u }) {
        assert(sceGxmColorSurfaceInit(emuenv, &surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
                   SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                   SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, dimension, dimension, (dimension + 1) & ~1u, Ptr<void>(4))
            == 0);
        uint32_t x0 = ~0u, y0 = ~0u, x1 = ~0u, y1 = ~0u;
        sceGxmColorSurfaceGetClip(emuenv, &surface, &x0, &y0, &x1, &y1);
        assert(x0 == 0 && y0 == 0 && x1 == dimension - 1 && y1 == dimension - 1);
        assert(surface.width == dimension && surface.height == dimension);
    }
    surface.gamma = 2;
    surface.downscale = 1;
    sceGxmColorSurfaceSetClip(emuenv, &surface, 11, 23, 4094, 4095);
    assert(surface.gamma == 2 && surface.downscale && !surface.disabled);
    assert(surface.width == 4096 && surface.height == 4096 && surface.strideInPixels == 4096);
    assert(surface.data.address() == 4 && surface.colorFormat == SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR);
    SceGxmColorSurface copy;
    memcpy(&copy, &surface, sizeof(copy));
    uint32_t x0, y0, x1, y1;
    sceGxmColorSurfaceGetClip(emuenv, &copy, &x0, &y0, &x1, &y1);
    assert(x0 == 11 && y0 == 23 && x1 == 4094 && y1 == 4095);
    sceGxmColorSurfaceGetClip(emuenv, &copy, nullptr, nullptr, nullptr, nullptr);
    sceGxmColorSurfaceSetScaleMode(emuenv, &surface, SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE);
    assert(sceGxmColorSurfaceGetScaleMode(emuenv, &surface) == SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE);
    sceGxmColorSurfaceSetScaleMode(emuenv, &surface, SCE_GXM_COLOR_SURFACE_SCALE_NONE);
    assert(sceGxmColorSurfaceGetScaleMode(emuenv, &surface) == SCE_GXM_COLOR_SURFACE_SCALE_NONE);
    assert(surface.clip_x_min == 11 && surface.clip_y_max == 4095);
    assert(sceGxmColorSurfaceInitDisabled(emuenv, &surface) == 0);
    assert(surface.disabled && !surface.clip_x_min && !surface.clip_y_max);
    assert(guarded.before == 0xDEADBEEF && guarded.after == 0xDEADBEEF);
    std::cout << "GXM: dimensions, clip round trip, opaque copy, ABI guards and scale modes passed\n";
}

static void test_events_and_dialog() {
    EmuEnv emuenv;
    auto flag = std::make_shared<EventFlag>();
    flag->name = "named";
    emuenv.kernel.objects.add(flag, 1);
    assert(sceKernelOpenSimpleEvent(emuenv, "named") == static_cast<int>(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME));
    auto event = std::make_shared<SimpleEvent>();
    event->name = "named";
    emuenv.kernel.objects.add(event, 2);
    assert(sceKernelOpenSimpleEvent(emuenv, "named") == 2);
    assert(emuenv.kernel.objects.remove<SimpleEvent>(2, 1));
    assert(sceKernelOpenSimpleEvent(emuenv, "named") == static_cast<int>(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME));
    assert(sceKernelOpenSimpleEvent(emuenv, nullptr) == static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR));
    assert(sceKernelOpenSimpleEvent(emuenv, std::string(32, 'x').c_str()) == static_cast<int>(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG));
    emuenv.common_dialog.type = SAVEDATA_DIALOG;
    for (const int status : { SCE_COMMON_DIALOG_STATUS_NONE, SCE_COMMON_DIALOG_STATUS_RUNNING, SCE_COMMON_DIALOG_STATUS_FINISHED }) {
        emuenv.common_dialog.status = status;
        assert(sceSaveDataDialogGetStatus(emuenv) == status);
    }
    emuenv.common_dialog.type = OTHER_DIALOG;
    assert(sceSaveDataDialogGetStatus(emuenv) == SCE_COMMON_DIALOG_STATUS_NONE);
    std::cout << "HLE: event type/name/deletion/error checks and savedata statuses passed\n";
}

static void test_swizzles() {
    using renderer::vulkan::color::translate_swizzle;
    using renderer::vulkan::texture::translate_swizzle;
    const std::array<vk::ComponentMapping, 6> maps{
        translate_swizzle(SCE_GXM_COLOR_FORMAT_U1U5U5U5_ARGB), translate_swizzle(SCE_GXM_COLOR_FORMAT_U1U5U5U5_ABGR),
        translate_swizzle(SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ARGB), translate_swizzle(SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ABGR),
        translate_swizzle(SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1RGB), translate_swizzle(SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1BGR)
    };
    for (uint32_t word = 0; word <= 0xFFFF; ++word) {
        const std::array<uint32_t, 4> raw{ (word >> 10) & 31, (word >> 5) & 31, word & 31, (word >> 15) & 1 };
        for (size_t i = 0; i < maps.size(); ++i) {
            const auto component = [&](vk::ComponentSwizzle swizzle, size_t channel) {
                if (swizzle == vk::ComponentSwizzle::eIdentity)
                    return raw[channel];
                if (swizzle == vk::ComponentSwizzle::eZero)
                    return 0u;
                if (swizzle == vk::ComponentSwizzle::eOne)
                    return 1u;
                return raw[static_cast<uint32_t>(swizzle) - static_cast<uint32_t>(vk::ComponentSwizzle::eR)];
            };
            assert(component(maps[i].r, 0) == raw[(i & 1) ? 2 : 0]);
            assert(component(maps[i].g, 1) == raw[1]);
            assert(component(maps[i].b, 2) == raw[(i & 1) ? 0 : 2]);
            assert(component(maps[i].a, 3) == (i >= 4 ? 1u : raw[3]));
        }
    }
    // Unsupported alpha-at-bit-zero layouts keep the previous mapping.
    const auto rgba = translate_swizzle(SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA);
    const auto bgra = translate_swizzle(SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_BGRA);
    assert(rgba.r == vk::ComponentSwizzle::eR && rgba.a == vk::ComponentSwizzle::eA);
    assert(bgra.r == vk::ComponentSwizzle::eB && bgra.a == vk::ComponentSwizzle::eA);
    std::cout << "Swizzles: all 65536 packed words in six supported formats passed\n";
}

int main(int argc, char **argv) {
    const std::string group = argc == 3 ? argv[2] : "all";
    assert(group == "all" || group == "pcm" || group == "surface" || group == "events" || group == "swizzles");
    if (group == "all" || group == "pcm")
        test_pcm();
    if (group == "all" || group == "surface")
        test_surface();
    if (group == "all" || group == "events")
        test_events_and_dialog();
    if (group == "all" || group == "swizzles")
        test_swizzles();
}
