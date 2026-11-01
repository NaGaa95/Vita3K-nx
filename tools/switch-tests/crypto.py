#!/usr/bin/env python3
# Copyright (C) 2026 Vita3K team
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import hashlib
import random
import struct
import subprocess
from pathlib import Path

from Crypto.Cipher import AES
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X30


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--devkitpro", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/switch-tests"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    executable = args.devkitpro / "devkitA64/bin/aarch64-none-elf-gcc"
    if not executable.is_file():
        executable = executable.with_suffix(".exe")
    objects = [
        "crypto/libcrypto-lib-armcap.o",
        "crypto/aes/libcrypto-lib-aesv8-armx.o",
        "crypto/sha/libcrypto-lib-sha256-armv8.o",
        "crypto/sha/libcrypto-lib-sha1-armv8.o",
    ]
    elf_path = args.output / "crypto.elf"
    subprocess.run([
        str(executable), "-nostdlib", "-Wl,-e,OPENSSL_cpuid_setup", "-Wl,-Ttext=0x100000",
        "-Wl,-u,sha256CalculateHash", "-Wl,-u,sha1CalculateHash",
        *[str(args.openssl / name) for name in objects],
        "-L" + str(args.devkitpro / "libnx/lib"), "-lnx", "-lc", "-lgcc", "-o", str(elf_path),
    ], check=True)

    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    with elf_path.open("rb") as file:
        elf = ELFFile(file)
        segments = [segment for segment in elf.iter_segments() if segment["p_type"] == "PT_LOAD"]
        start = min(segment["p_vaddr"] for segment in segments) & ~4095
        end = (max(segment["p_vaddr"] + segment["p_memsz"] for segment in segments) + 4095) & ~4095
        cpu.mem_map(start, end - start)
        for segment in segments:
            cpu.mem_write(segment["p_vaddr"], segment.data())
        symbols = {symbol.name: symbol["st_value"] for symbol in elf.get_section_by_name(".symtab").iter_symbols()}

    data = 0x400000
    stop = 0x800000
    cpu.mem_map(data, 0x100000)
    cpu.mem_map(stop, 0x100000)

    def call(name, *values):
        cpu.reg_write(UC_ARM64_REG_SP, stop + 0x80000)
        cpu.reg_write(UC_ARM64_REG_X30, stop)
        for index, value in enumerate(values):
            cpu.reg_write(UC_ARM64_REG_X0 + index, value)
        cpu.emu_start(symbols[name], stop, count=20_000_000)
        assert cpu.reg_read(UC_ARM64_REG_PC) == stop, name
        return cpu.reg_read(UC_ARM64_REG_X0)

    call("OPENSSL_cpuid_setup")
    armcap = struct.unpack("<I", cpu.mem_read(symbols["OPENSSL_armcap_P"], 4))[0]
    assert armcap == 0x3D, hex(armcap)
    assert bytes(cpu.mem_read(symbols["OPENSSL_armv8_rsa_neonized"], 4)) == b"\0" * 4
    rng = random.Random(0xA57)
    source, output, key_address, iv_address, schedule = data + 1, data + 0x20003, data + 0x50000, data + 0x50100, data + 0x50200
    cases = 0
    for bits in (128, 192, 256):
        for size in (16, 32, 48, 64, 80, 128, 256, 4096, 65536):
            key = rng.randbytes(bits // 8)
            plaintext = rng.randbytes(size)
            iv = rng.randbytes(12) + b"\0\0\0\1"
            cpu.mem_write(key_address, key)
            assert call("aes_v8_set_encrypt_key", key_address, bits, schedule) == 0
            for mode, function in ((AES.MODE_ECB, "aes_v8_ecb_encrypt"), (AES.MODE_CBC, "aes_v8_cbc_encrypt"), (AES.MODE_CTR, "aes_v8_ctr32_encrypt_blocks")):
                cpu.mem_write(source, plaintext)
                cpu.mem_write(iv_address, iv)
                if mode == AES.MODE_CTR:
                    call(function, source, output, size // 16, schedule, iv_address)
                    expected = AES.new(key, mode, nonce=b"", initial_value=int.from_bytes(iv, "big")).encrypt(plaintext)
                elif mode == AES.MODE_CBC:
                    call(function, source, output, size, schedule, iv_address, 1)
                    expected = AES.new(key, mode, iv=iv).encrypt(plaintext)
                    assert bytes(cpu.mem_read(iv_address, 16)) == expected[-16:]
                else:
                    call(function, source, output, size, schedule, 1)
                    expected = AES.new(key, mode).encrypt(plaintext)
                assert bytes(cpu.mem_read(output, size)) == expected, (bits, size, function)
                if mode != AES.MODE_CTR:
                    assert call("aes_v8_set_decrypt_key", key_address, bits, schedule) == 0
                    cpu.mem_write(output, expected)
                    cpu.mem_write(iv_address, iv)
                    if mode == AES.MODE_CBC:
                        call(function, output, output, size, schedule, iv_address, 0)
                    else:
                        call(function, output, output, size, schedule, 0)
                    assert bytes(cpu.mem_read(output, size)) == plaintext, (bits, size, function, "in-place decrypt")
                    assert call("aes_v8_set_encrypt_key", key_address, bits, schedule) == 0
                cases += 1

    for algorithm, initial in (
        ("sha1", (0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0)),
        ("sha256", (0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19)),
    ):
        for size in (0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 4096, 65536):
            message = rng.randbytes(size)
            expected = getattr(hashlib, algorithm)(message).digest()
            padded = message + b"\x80"
            padded += b"\0" * ((56 - len(padded)) % 64) + struct.pack(">Q", size * 8)
            cpu.mem_write(source, padded)
            for capabilities in (armcap, 0):
                cpu.mem_write(symbols["OPENSSL_armcap_P"], struct.pack("<I", capabilities))
                cpu.mem_write(schedule, struct.pack("<" + "I" * len(initial), *initial))
                call(algorithm + "_block_data_order", schedule, source, len(padded) // 64)
                state = struct.unpack("<" + "I" * len(initial), cpu.mem_read(schedule, len(initial) * 4))
                assert struct.pack(">" + "I" * len(initial), *state) == expected, (algorithm, size, capabilities)
                cases += 1
            cpu.mem_write(source, message or b"\0")
            call(algorithm + "CalculateHash", output, source, size)
            assert bytes(cpu.mem_read(output, len(expected))) == expected, ("libnx", algorithm, size)
            cases += 1
    print(f"ARM64 crypto: {cases} AES-128/192/256 ECB/CBC/CTR and SHA-1/SHA-256 cases passed")


if __name__ == "__main__":
    main()
