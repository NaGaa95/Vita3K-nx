#!/usr/bin/env python3
"""Compile isolated HLE and shader regressions against the repository sources.

Requires a native clang++ or g++, and initialized Boost, fmt, spdlog, glslang,
and Vulkan-Headers submodules. No console SDK or game files are needed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    # Ignore braces inside strings and comments while retaining source offsets.
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', text[opening:])
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[start:opening + token.end()]
    raise ValueError(f"Unclosed definition: {marker}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--spirv-val", default=shutil.which("spirv-val"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    fixtures = Path(__file__).resolve().parent
    output = (args.build_dir or root / "build/backport-tests").resolve()
    output.mkdir(parents=True, exist_ok=True)
    read = lambda path: (root / path).read_text(encoding="utf-8")
    (output / "io_read.inc").write_text(block(read("vita3k/io/src/state_functions.cpp"), "SceOff FileStats::read("), encoding="utf-8")
    gxm = read("vita3k/modules/SceGxm/SceGxm.cpp")
    audio = read("vita3k/modules/SceAudiodec/SceAudiodecUser.cpp")
    api = [block(audio, "enum {") + ";", block(audio, "struct SceAudiodecCtrl") + ";"]
    for name in ("GetClip", "SetClip", "GetScaleMode", "SetScaleMode", "Init", "InitDisabled"):
        marker = re.search(r"EXPORT\([^\n]*, sceGxmColorSurface" + name + r"(?:,|\))", gxm).group()
        api.append(block(gxm, marker))
    api.append(block(audio, "EXPORT(int, sceAudiodecPartlyDecode"))
    api.append(block(read("vita3k/modules/SceCommonDialog/SceCommonDialog.cpp"), "EXPORT(int, sceSaveDataDialogGetStatus"))
    api.append(block(read("vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp"), "EXPORT(SceUID, sceKernelOpenSimpleEvent"))
    (output / "hle_functions.inc").write_text("\n\n".join(api), encoding="utf-8")
    constructor = block(read("vita3k/shader/src/usse_translator_entry.cpp"), "USSERecompiler::USSERecompiler(")
    (output / "shader_constructor.inc").write_text(constructor, encoding="utf-8")
    source = read("vita3k/shader/src/spirv_recompiler.cpp")
    start = source.index("                    const spv::Id bool_type = b.makeBoolType();", source.index("if (emit_eye_clip)"))
    end = source.index("                    b.createStore(b.createBinOp(spv::OpFAdd", start)
    (output / "eye_clip.inc").write_text(source[start:end], encoding="utf-8")

    includes = [output, root / "external/glslang", root / "external/boost",
                root / "external/fmt/include", root / "external/spdlog/include",
                root / "external/VulkanMemoryAllocator-Hpp/Vulkan-Headers/include"]
    includes += [root / "vita3k" / module / "include" for module in
                 ("shader", "gxm", "renderer", "mem", "util", "kernel", "features", "module", "cpu", "audio", "np", "rtc", "io", "emuenv", "ngs", "touch")]
    common = [args.cxx, "-std=c++23", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-include", "algorithm",
              "-DFMT_HEADER_ONLY", "-DSPDLOG_FMT_EXTERNAL", "-Wno-deprecated-declarations", "-pthread"]
    common += ["-I" + str(path) for path in includes]
    for switch in (False, True):
        executable = output / ("io-read" + ("-switch" if switch else "") + (".exe" if os.name == "nt" else ""))
        command = common + (["-D__SWITCH__"] if switch else [])
        subprocess.run(command + [str(fixtures / "io_read.cpp"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    executable = output / ("vargs" + (".exe" if os.name == "nt" else ""))
    subprocess.run(common + [str(fixtures / "vargs.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    def run_fixture(name, definitions, sources=()):
        (output / (name + ".inc")).write_text("\n\n".join(definitions), encoding="utf-8")
        executable = output / (name + (".exe" if os.name == "nt" else ""))
        command = common + [str(fixtures / (name + ".cpp"))] + [str(root / path) for path in sources]
        subprocess.run(command + ["-Wl,--gc-sections", "-o", str(executable)], check=True)
        subprocess.run([str(executable), str(output)], check=True)

    run_fixture("touch_mapping", [])
    run_fixture("vertex_upload", [
        block(read("vita3k/vkutil/src/objects.cpp"), "void RingBuffer::allocate(const uint32_t data_size)"),
        block(read("vita3k/renderer/src/vulkan/scene.cpp"), "static void bind_vertex_streams(")])

    (output / "ngs_lifecycle.inc").write_text("namespace ngs {\n"
        + block(read("vita3k/ngs/src/ngs.cpp"), "void Voice::transition(") + "\n"
        + block(read("vita3k/ngs/src/scheduler.cpp"), "bool VoiceScheduler::off(") + "\n}", encoding="utf-8")
    run_fixture("ngs_dsp", [], ["vita3k/ngs/src/modules/" + name + ".cpp"
        for name in ("filter", "equalizer", "distortion", "envelope")])
    run_fixture("vertex_layout", [block(read("vita3k/renderer/src/vulkan/pipeline_cache.cpp"), "static std::shared_ptr<const VertexInputLayout> make_vertex_input_layout(")],
        ["vita3k/renderer/src/vulkan/gxm_to_vulkan.cpp", "vita3k/gxm/src/attributes.cpp", "vita3k/gxm/src/stream.cpp"])

    run_fixture("video_copy", [block(read("vita3k/codec/src/h264.cpp"), "void copy_yuv_data_from_frame(")])
    audio_config = read("vita3k/modules/SceAudio/SceAudio.cpp")
    run_fixture("audio_config",
                [block(audio_config, "enum " + name) + ";" for name in
                 ("SceAudioOutMode", "SceAudioOutPortType", "SceAudioOutErrorCode", "SceAudioOutConfigType")]
                + [block(audio_config, "EXPORT(int, " + name + ",") for name in
                   ("sceAudioOutGetConfig", "sceAudioOutReleasePort", "sceAudioOutSetConfig")])
    np_manager = read("vita3k/modules/SceNpManager/SceNpManager.cpp")
    run_fixture("np_cache", [block(np_manager, "enum SceNpManagerErrorCode") + ";",
                            block(np_manager, "struct SceNpAvatarUrl") + ";",
                            block(np_manager, "struct SceNpManagerCacheParam") + ";",
                            block(np_manager, "EXPORT(int, sceNpManagerGetNpId,"),
                            block(np_manager, "EXPORT(int, sceNpManagerGetCachedParam,")])
    app_util = read("vita3k/modules/SceAppUtil/SceAppUtil.cpp")
    run_fixture("savedata_mount", [block(read("vita3k/io/src/io.cpp"), "std::string translate_path(")]
                + [block(app_util, "EXPORT(SceInt32, " + name) for name in
                   ("sceAppUtilSaveDataMount", "sceAppUtilSaveDataUmount")], ["vita3k/io/src/device.cpp"])
    shared = [root / "vita3k/gxm/src" / path for path in ("color.cpp", "textures.cpp")]
    shader = [root / "vita3k/shader/src" / path for path in
              ("translator/ialu.cpp", "translator/alu.cpp", "translator/texture.cpp", "translator/data.cpp", "translator/utils.cpp", "usse_utilities.cpp",
               "usse_decode_helpers.cpp", "usse_disasm.cpp", "usse_program_analyzer.cpp", "gxp_parser.cpp")]
    shader.append(root / "vita3k/gxm/src/gxp.cpp")
    shader += [root / "external/glslang/SPIRV" / path for path in
               ("SpvBuilder.cpp", "SpvPostProcess.cpp", "InReadableOrder.cpp", "Logger.cpp", "doc.cpp")]
    for name, sources in (("hle", shared + [root / "vita3k/renderer/src/vulkan/gxm_to_vulkan.cpp"]),
                          ("shader", shared + shader)):
        executable = output / (name + (".exe" if os.name == "nt" else ""))
        command = common + [str(fixtures / (name + ".cpp"))] + [str(path) for path in sources]
        command += ["-Wl,--gc-sections", "-o", str(executable)]
        (output / (name + "-command.json")).write_text(json.dumps(command), encoding="utf-8")
        subprocess.run(command, check=True)
        subprocess.run([str(executable), str(output)], check=True)
    if args.spirv_val:
        for module in sorted(output.glob("i16mad-*.spv")) + sorted(output.glob("sop2m-*.spv")) + sorted(output.glob("vmov-*.spv")) + sorted(output.glob("index-*.spv")) + sorted(output.glob("clip-*.spv")) + sorted(output.glob("cast-fetch-*.spv")):
            subprocess.run([args.spirv_val, "--target-env", "vulkan1.2", str(module)], check=True)
        print("SPIR-V: all arithmetic and sampling fixtures passed Vulkan 1.2 validation", flush=True)
    else:
        print("SPIR-V validation skipped; pass --spirv-val /path/to/spirv-val to enable it", flush=True)


if __name__ == "__main__":
    main()
