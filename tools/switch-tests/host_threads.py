#!/usr/bin/env python3
"""Test native SDL thread cleanup under address and undefined-behavior sanitizers."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = root / "build-switch-host-thread-tests"
    sdl = output / "sdl"
    options = ["cmake", "-S", str(root / "external/sdl"), "-B", str(sdl), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Debug", "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON",
               "-DSDL_TESTS=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_INSTALL=OFF", "-DSDL_ASAN=ON",
               "-DSDL_UNIX_CONSOLE_BUILD=ON", "-DSDL_DBUS=OFF", "-DSDL_IBUS=OFF"]
    for subsystem in ("VIDEO", "AUDIO", "CAMERA", "JOYSTICK", "HAPTIC", "HIDAPI", "SENSOR",
                      "POWER", "DIALOG", "GPU", "RENDER", "TRAY"):
        options.append(f"-DSDL_{subsystem}=OFF")
    subprocess.run(options, check=True)
    subprocess.run(["cmake", "--build", str(sdl), "-j4"], check=True)
    executable = output / "host_threads"
    subprocess.run([args.cxx, "-std=c++23", "-O1", "-g", "-fsanitize=address,undefined",
                    "-Wall", "-Wextra", "-Werror", "-DSDL_STATIC_LIB",
                    "-I" + str(root / "external/sdl/include"),
                    "-I" + str(root / "vita3k/kernel/include"),
                    str(root / "tools/switch-tests/host_threads.cpp"),
                    str(root / "vita3k/kernel/src/host_threads.cpp"), str(sdl / "libSDL3.a"),
                    "-pthread", "-ldl", "-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
