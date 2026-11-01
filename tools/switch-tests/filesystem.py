#!/usr/bin/env python3
"""Test installer cleanup on the host and with the Horizon service mocked."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = root / "build/switch-tests/filesystem"
    mocks = output / "mock"
    (mocks / "util").mkdir(parents=True, exist_ok=True)
    (mocks / "switch.h").write_text(
        "#pragma once\n#include <cstdint>\nusing Result = uint32_t;\n"
        "constexpr bool R_FAILED(Result r) { return r != 0; }\n"
        "Result fsdevDeleteDirectoryRecursively(const char *path);\n")
    (mocks / "util/log.h").write_text("#pragma once\n#define LOG_ERROR(...) ((void)0)\n")
    for native in (False, True):
        executable = output / ("native" if native else "portable")
        command = [args.cxx, "-std=c++23", "-O1", "-g", "-fsanitize=address,undefined",
                   "-Wall", "-Wextra", "-Werror", "-I" + str(mocks),
                   "-I" + str(root / "vita3k/util/include")]
        if native:
            command += ["-D__SWITCH__"]
        command += [str(root / "tools/switch-tests/filesystem.cpp"),
                    str(root / "vita3k/util/src/fs_remove.cpp"), "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
