#include <mem/state.h>
#include <mem/ptr.h>
#include <module/vargs.h>

#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

struct CPUState {
    std::array<uint32_t, 16> registers{};
};
uint32_t read_reg(CPUState &cpu, size_t index) { return cpu.registers.at(index); }
uint32_t read_sp(CPUState &cpu) { return read_reg(cpu, 13); }
float read_float_reg(CPUState &, size_t) { assert(false); return 0; }

int main() {
    MemState mem;
    mem.memory = Memory(new uint8_t[1024], [](uint8_t *p) { delete[] p; });
    std::memset(mem.memory.get(), 0xCD, 1024);
    CPUState cpu;
    cpu.registers[13] = 256;
    const auto store = [&](uint32_t address, auto value) {
        std::memcpy(mem.memory.get() + address, &value, sizeof(value));
    };
    for (const uint64_t bits : { 0x400921FB54442D18ull, 0x8000000000000000ull, 0x7FF8123456789ABCull }) {
        cpu.registers[1] = 0x12345678;
        cpu.registers[2] = static_cast<uint32_t>(bits);
        cpu.registers[3] = static_cast<uint32_t>(bits >> 32);
        store(256, bits);
        store(264, uint32_t{ 0x12345678 });
        for (const size_t first : { 1u, 2u, 3u, 4u }) {
            module::vargs args(LayoutArgsState{ first, 0, 0 });
            assert(std::bit_cast<uint64_t>(args.next<double>(cpu, mem)) == bits);
            if (first >= 3)
                assert(args.next<uint32_t>(cpu, mem) == 0x12345678);
        }
        store(512, uint32_t{ 0x12345678 });
        store(520, bits);
        store(528, uint64_t{ 0xFEDCBA9876543210ull });
        module::vargs args(Address{ 512 });
        assert(args.next<uint32_t>(cpu, mem) == 0x12345678);
        assert(std::bit_cast<uint64_t>(args.next<double>(cpu, mem)) == bits);
        assert(args.next<uint64_t>(cpu, mem) == 0xFEDCBA9876543210ull);
    }
    std::cout << "Varargs: register pairs, stack and va_list alignment, NaN payloads and signed zero passed\n";
}
