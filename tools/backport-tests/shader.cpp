// Exercise production instruction translators and register packing with a real
// SPIR-V builder and explicit-LOD texture fetches.
#include <shader/usse_translator.h>
#include <shader/uniform_block.h>
#include <util/log.h>

#include <cassert>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace shader::usse {
#include "shader_constructor.inc"
} // namespace shader::usse

using namespace shader::usse;

struct Fixture {
    spv::SpvBuildLogger logger;
    spv::Builder builder{ 0x00010500, 0, &logger };
    SceGxmProgram program{};
    FeatureState features{};
    SpirvShaderParameters params{};
    utils::SpirvUtilFunctions helpers{};
    spv::Block *entry;
    spv::Instruction *interface;
    std::unique_ptr<USSERecompiler> recompiler;

    Fixture() {
        spdlog::set_level(spdlog::level::off);
        builder.addCapability(spv::CapabilityShader);
        builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
        helpers.std_builtins = builder.import("GLSL.std.450");
        const auto main = builder.makeEntryPoint("main");
        auto *entry_point = builder.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
        interface = entry_point;
        builder.addExecutionMode(main, spv::ExecutionModeLocalSize, 1, 1, 1);
        entry = builder.getBuildPoint();
        const auto vec4 = builder.makeVectorType(builder.makeFloatType(32), 4);
        const auto bank = builder.makeArrayType(vec4, builder.makeUintConstant(32), 0);
        params.temps = builder.createVariable(spv::NoPrecision, spv::StorageClassPrivate, bank, "temps");
        params.ins = builder.createVariable(spv::NoPrecision, spv::StorageClassPrivate, bank, "pa");
        params.uniforms = builder.createVariable(spv::NoPrecision, spv::StorageClassPrivate, bank, "sa");
        params.outs = builder.createVariable(spv::NoPrecision, spv::StorageClassPrivate, bank, "outputs");
        params.internals = builder.createVariable(spv::NoPrecision, spv::StorageClassPrivate, bank, "internals");
        params.indexes = builder.createVariable(spv::NoPrecision, spv::StorageClassPrivate,
            builder.makeArrayType(builder.makeIntType(32), builder.makeUintConstant(2), 0), "indexes");
        for (const auto variable : { params.temps, params.ins, params.uniforms, params.outs, params.internals, params.indexes })
            entry_point->addIdOperand(variable);
        recompiler = std::make_unique<USSERecompiler>(builder, program, features, params, helpers, nullptr, NonDependentTextureQueryCallInfos{}, 0);
        recompiler->cur_instr = 0;
        recompiler->cur_pc = 0;
    }

    void save(const std::filesystem::path &path) {
        builder.makeReturn(true);
        builder.leaveFunction();
        std::vector<unsigned int> words;
        builder.dump(words);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(words.data()), words.size() * sizeof(words[0]));
        assert(file.good());
    }
};

static void test_i16mad(const std::filesystem::path &output) {
    int cases = 0;
    for (uint8_t mode : { 0, 2 }) {
        for (uint8_t format : { 0, 1 }) {
            for (uint8_t negate : { 0, 1 }) {
                for (uint8_t repeat = 0; repeat < 4; ++repeat) {
                    Fixture fixture;
                    assert(fixture.recompiler->visitor.i16mad(ShortPredicate::NONE,
                        0, false, false, negate, 1, 0, 0, 0, 0, static_cast<RepeatCount>(repeat), mode, format, format,
                        1, 0, 0, 0, 0, 0, 12, 0, 4, 8));
                    int multiplies = 0, additions = 0, subtractions = 0;
                    for (const auto &instruction : fixture.entry->getInstructions()) {
                        const auto op = instruction->getOpCode();
                        multiplies += op == spv::OpIMul;
                        additions += op == spv::OpIAdd;
                        subtractions += op == spv::OpISub;
                        if (op == spv::OpIMul || op == spv::OpIAdd || op == spv::OpISub) {
                            const auto scalar = fixture.builder.getContainedTypeId(instruction->getTypeId());
                            assert(scalar == (mode == 0 ? fixture.builder.makeUintType(32) : fixture.builder.makeIntType(32)));
                        }
                    }
                    assert(multiplies == repeat + 1);
                    assert(additions == (negate ? 0 : repeat + 1));
                    assert(subtractions == (negate ? repeat + 1 : 0));
                    fixture.save(output / ("i16mad-" + std::to_string(cases++) + ".spv"));
                }
            }
        }
    }
    std::cout << "I16MAD: 32 signedness/format/negation/repeat cases passed\n";
}

static void test_sop2m(const std::filesystem::path &output) {
    for (uint8_t mask = 1; mask < 16; ++mask) {
        Fixture fixture;
        const uint8_t encoded_mask = ((mask & 7) << 1) | ((mask >> 3) & 1);
        assert(fixture.recompiler->visitor.sop2m(0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            encoded_mask, 0, 2, 0, 0, 0, 0, 8, 0, 4));
        std::unordered_map<spv::Id, spv::Instruction *> instructions;
        spv::Id converted = 0, packed_input = 0;
        const auto pack = fixture.helpers.pack_funcs.at(DataType::UINT8)->getId();
        for (const auto &instruction : fixture.entry->getInstructions()) {
            instructions[instruction->getResultId()] = instruction.get();
            if (instruction->getOpCode() == spv::OpConvertFToU)
                converted = instruction->getResultId();
            if (instruction->getOpCode() == spv::OpFunctionCall && instruction->getIdOperand(0) == pack)
                packed_input = instruction->getIdOperand(1);
        }
        assert(converted && packed_input);
        // Follow the generated scalar extraction through the compacted vector.
        // A selected destination byte must originate from the same result lane.
        const auto lane = [&](auto &&self, spv::Id value) -> int {
            const auto found = instructions.find(value);
            if (found == instructions.end())
                return -1;
            const auto *instruction = found->second;
            if (instruction->getOpCode() != spv::OpCompositeExtract)
                return -1;
            const spv::Id vector = instruction->getIdOperand(0);
            const unsigned index = instruction->getImmediateOperand(1);
            if (vector == converted)
                return index;
            const auto parent = instructions.find(vector);
            if (parent != instructions.end() && parent->second->getOpCode() == spv::OpCompositeConstruct)
                return self(self, parent->second->getIdOperand(index));
            return -1;
        };
        const auto *packed = instructions.at(packed_input);
        assert(packed->getOpCode() == spv::OpCompositeConstruct && packed->getNumOperands() == 4);
        for (int channel = 0; channel < 4; ++channel) {
            const int source_lane = lane(lane, packed->getIdOperand(channel));
            assert(source_lane == ((mask & (1 << channel)) ? channel : -1));
        }
        fixture.save(output / ("sop2m-" + std::to_string(mask) + ".spv"));
    }
    std::cout << "SOP2M: all 15 nonempty masks select the correct bytes and preserve other lanes\n";
}

static void test_vmov(const std::filesystem::path &output) {
    int cases = 0;
    for (const MoveType move : { MoveType::UNCONDITIONAL, MoveType::CONDITIONAL }) {
        for (const uint8_t mask : { 1, 4, 5 }) {
            Fixture fixture;
            assert(fixture.recompiler->visitor.vmov(ExtPredicate::NONE, false, 0, 0, false,
                0, 0, 0, 0, move, RepeatCount::REPEAT_0, false, DataType::F16, 0, 4, 0,
                0, 0, 0, mask, 12, 0, 4, 8));
            assert(fixture.helpers.pack_funcs.contains(DataType::UINT16));
            assert(!fixture.helpers.pack_funcs.contains(DataType::F16));
            assert(!fixture.helpers.unpack_funcs.contains(DataType::F16) || move != MoveType::UNCONDITIONAL);
            fixture.save(output / ("vmov-" + std::to_string(cases++) + ".spv"));
        }
    }
    for (uint8_t mask = 1; mask < 16; ++mask) {
        Fixture fixture;
        auto &b = fixture.builder;
        std::vector<spv::Id> lanes;
        for (int i = 0; i < 4; ++i) {
            if (mask & (1 << i))
                lanes.push_back(b.makeUintConstant(0xFC01u + i));
        }
        const spv::Id value = lanes.size() == 1 ? lanes[0]
            : b.createCompositeConstruct(b.makeVectorType(b.makeUintType(32), lanes.size()), lanes);
        Operand dest{};
        dest.bank = RegisterBank::TEMP;
        dest.type = DataType::F16;
        utils::store(b, fixture.params, fixture.helpers, fixture.features, dest, value, mask, 0, true);
        assert(fixture.helpers.pack_funcs.contains(DataType::UINT16));
        assert(!fixture.helpers.pack_funcs.contains(DataType::F16));
        fixture.save(output / ("vmov-partial-" + std::to_string(mask) + ".spv"));
    }
    std::cout << "VMOV: partial masks and conditional F16 payloads use integer packing\n";
}

static void test_index(const std::filesystem::path &output) {
    int cases = 0;
    for (const DataType type : { DataType::UINT8, DataType::INT8, DataType::UINT16, DataType::INT16,
             DataType::UINT32, DataType::INT32, DataType::F16, DataType::F32 }) {
        Fixture fixture;
        auto &b = fixture.builder;
        Operand dest{};
        dest.bank = RegisterBank::INDEX;
        dest.type = type;
        dest.num = 1;
        const auto value = b.makeIntConstant(0x1234ABCD);
        utils::store(b, fixture.params, fixture.helpers, fixture.features, dest, value, 1, 0);
        const spv::Instruction *masked = nullptr;
        for (const auto &inst : fixture.entry->getInstructions()) {
            if (inst->getOpCode() == spv::OpBitwiseAnd)
                masked = inst.get();
            if (inst->getOpCode() == spv::OpStore) {
                assert(masked && inst->getIdOperand(1) == masked->getResultId());
                const uint32_t expected = get_data_type_size(type) == 1 ? 0xFF : 0xFFFF;
                assert(b.getConstantScalar(masked->getIdOperand(1)) == expected);
            }
        }
        assert(masked);
        fixture.save(output / ("index-" + std::to_string(cases++) + ".spv"));
    }
    std::cout << "Index registers: 8/16/32-bit and floating writes truncate before storage\n";
}

static void test_eye_clip(const std::filesystem::path &output) {
    int cases = 0;
    for (const float w : { 0.0f, -0.0f, 1.0f, -1.0f, std::numeric_limits<float>::min() }) {
        for (const bool accepted : { false, true }) {
            Fixture fixture;
            auto &b = fixture.builder;
            const auto f32 = b.makeFloatType(32);
            const auto zero = b.makeFloatConstant(0.0f);
            const auto clip_w = b.makeFloatConstant(w);
            const auto pred = b.makeBoolConstant(accepted);
            const auto clip_dist0_ref = utils::create_access_chain(b, spv::StorageClassPrivate, fixture.params.temps,
                { b.makeIntConstant(0), b.makeIntConstant(0) });
            #include "eye_clip.inc"
            std::unordered_map<spv::Id, const spv::Instruction *> instructions;
            spv::Id stored = 0;
            for (const auto &inst : fixture.entry->getInstructions()) {
                instructions[inst->getResultId()] = inst.get();
                if (inst->getOpCode() == spv::OpStore)
                    stored = inst->getIdOperand(1);
            }
            const auto evaluate = [&](auto &&self, spv::Id id) -> uint32_t {
                if (id == b.makeBoolConstant(true))
                    return 1;
                if (id == b.makeBoolConstant(false))
                    return 0;
                if (b.isConstant(id))
                    return b.getConstantScalar(id);
                const auto *inst = instructions.at(id);
                const auto get = [&](int i) { return self(self, inst->getIdOperand(i)); };
                switch (inst->getOpCode()) {
                case spv::OpLogicalNot: return !get(0);
                case spv::OpLogicalAnd: return get(0) && get(1);
                case spv::OpFOrdEqual: return std::bit_cast<float>(get(0)) == std::bit_cast<float>(get(1));
                case spv::OpSelect: return get(get(0) ? 1 : 2);
                default: assert(false); return 0;
                }
            };
            const float expected = w == 0.0f && !accepted ? -std::numeric_limits<float>::min() : w;
            assert(evaluate(evaluate, stored) == std::bit_cast<uint32_t>(expected));
            fixture.save(output / ("clip-" + std::to_string(cases++) + ".spv"));
        }
    }
    std::cout << "Eye clipping: signed zero, accepted/rejected vertices and unchanged positive/negative w passed\n";
}

static void test_surface_cast_fetch(const std::filesystem::path &output) {
    for (const bool casts : {false, true}) {
        Fixture fixture;
        auto &b = fixture.builder;
        const auto f32 = b.makeFloatType(32), v2 = b.makeVectorType(f32, 2), v4 = b.makeVectorType(f32, 4);
        const auto image = b.makeImageType(f32, spv::Dim2D, false, false, false, 1, spv::ImageFormatUnknown);
        const auto texture = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, b.makeSampledImageType(image), "tex");
        b.addDecoration(texture, spv::DecorationDescriptorSet, 1);
        b.addDecoration(texture, spv::DecorationBinding, 0);
        fixture.interface->addIdOperand(texture);
        b.addCapability(spv::CapabilityImageQuery);
        if (casts) {
            fixture.params.frag_coord_id = b.createVariable(spv::NoPrecision, spv::StorageClassPrivate, v4, "frag_coord");
            fixture.interface->addIdOperand(fixture.params.frag_coord_id);
            const auto type = b.makeStructType(std::vector<spv::Id>(shader::FRAG_UNIFORM_is_srgb + 1, f32), "RenderInfo");
            b.addDecoration(type, spv::DecorationBlock);
            for (unsigned i = 0; i <= shader::FRAG_UNIFORM_is_srgb; ++i)
                b.addMemberDecoration(type, i, spv::DecorationOffset, i * 4);
            fixture.params.render_info_id = b.createVariable(spv::NoPrecision, spv::StorageClassUniform, type, "render_info");
            b.addDecoration(fixture.params.render_info_id, spv::DecorationDescriptorSet, 0);
            b.addDecoration(fixture.params.render_info_id, spv::DecorationBinding, 0);
            fixture.interface->addIdOperand(fixture.params.render_info_id);
            fixture.params.has_surface_casts_constant = b.makeBoolConstant(false, true);
            b.addDecoration(fixture.params.has_surface_casts_constant, spv::DecorationSpecId, 1);
        }
        const auto coord = b.makeCompositeConstant(v2, {b.makeFloatConstant(.5f), b.makeFloatConstant(.25f)});
        const auto value = fixture.recompiler->visitor.do_fetch_texture(b.createLoad(texture, spv::NoPrecision), 3, 2,
            {coord, int(DataType::F32)}, DataType::F32, 2, b.makeFloatConstant(0.f));
        assert(b.getTypeId(value) == v4);
        fixture.save(output / (std::string("cast-fetch-") + (casts ? "on" : "off") + ".spv"));
    }
    std::cout << "Surface casts: explicit-LOD fetches with and without phase correction generated\n";
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const std::string group = argc == 3 ? argv[2] : "all";
    assert(group == "all" || group == "i16mad" || group == "sop2m" || group == "vmov" || group == "index" || group == "clip" || group == "texture");
    if (group == "all" || group == "i16mad")
        test_i16mad(argv[1]);
    if (group == "all" || group == "sop2m")
        test_sop2m(argv[1]);
    if (group == "all" || group == "vmov")
        test_vmov(argv[1]);
    if (group == "all" || group == "index")
        test_index(argv[1]);
    if (group == "all" || group == "clip")
        test_eye_clip(argv[1]);
    if (group == "all" || group == "texture")
        test_surface_cast_fetch(argv[1]);
}
