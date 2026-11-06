// In-game Switch 2 shader translator: native NVN2 stages (SM86) -> SPIR-V through Eglantine's shader_recompiler
// (GPL-3.0-or-later, third_party/eglantine), with the C interface of nx2_xlate.h.
//
// A graphics stage is a 0x80-byte shader program header (SPH, Maxwell layout) followed by 16-byte Ampere
// instructions; a compute stage has no header. As Eglantine's GenericEnvironment::LowerNativeCode does
// (video_core/shader_environment.cpp), the native program is read until ProgramCaptureState finds its end, lowered
// to Maxwell instructions, and laid out as Maxwell code: the SPH, one empty scheduling word before every three
// instructions, then padding and the native address map (indirect branches resolve through it). Graphics stages are
// linked as Eglantine's vk_pipeline_cache.cpp does (MakeRuntimeInfo, one Bindings across the stages).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "nx2_xlate.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

namespace {

using Shader::Ampere::Instruction;

// Native code -> the lowered Maxwell code buffer the translator reads. Throws on failure.
std::vector<u64> Lower(const u8 *file, size_t size, Shader::ProgramHeader &sph, u32 &texture_bound, bool compute) {
    const size_t native_header = compute ? 0 : 0x80;
    if (size < native_header + sizeof(Instruction)) throw std::runtime_error("stage too small");
    if (compute) std::memset(&sph, 0, sizeof sph);
    else std::memcpy(&sph, file, sizeof sph);
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    const size_t count = (size - native_header) / sizeof(Instruction);
    bool complete = false;
    for (u32 i = 0; i < count && i < Shader::Ampere::ProgramCaptureState::MaximumInstructions; ++i) {
        Instruction in;
        std::memcpy(&in, file + native_header + i * sizeof in, sizeof in);
        native.push_back(in);
        const auto status = capture.Observe(in, i);
        if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget)
            throw std::runtime_error("branch outside the program");
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            complete = true;
            break;
        }
    }
    if (!complete) throw std::runtime_error("program end not found");
    // "full": matrix multiply (HMMA) expanded to shuffles + HFMA2; this title's upscaler needs it (Eglantine)
    auto lowered = Shader::Ampere::LowerToMaxwell(native, "full");
    if (!lowered) {
        const size_t at = lowered.error_offset / sizeof(Instruction);
        const auto w = at < native.size() ? native[at].words : std::array<u32, 4>{};
        throw std::runtime_error(fmt::format("lowering: {} at {:#x} [op {:03x}]", lowered.error,
                                             lowered.error_offset, w[0] & 0xfff));
    }
    texture_bound = lowered.required_texture_buffer;
    std::vector<u64> code(sizeof sph / sizeof(u64));
    std::memcpy(code.data(), &sph, sizeof sph);
    for (const u64 instruction : lowered.instructions) {
        if ((code.size() * sizeof(u64)) % 32 == 0) code.push_back(0);   // Maxwell scheduling word
        code.push_back(instruction);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    return code;
}

class Env final : public Shader::Environment {
public:
    Env(std::vector<u64> code_, const Shader::ProgramHeader &header, Shader::Stage stage_, u32 texture_bound_,
        const Nx2XOptions *opt_)
        : code{std::move(code_)}, texture_bound{texture_bound_}, opt{opt_} {
        sph = header;
        stage = stage_;
        start_address = 0;
        is_proprietary_driver = true;
    }

    u64 ReadInstruction(u32 address) override {
        const u32 i = address / sizeof(u64);
        if (i >= code.size()) throw Shader::RuntimeError("instruction address {:#x} past the program", address);
        return code[i];
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(code, address)) return *t;
        throw Shader::RuntimeError("invalid native indirect branch address {:#x}", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        return opt && opt->cbuf_value ? opt->cbuf_value(opt->user, static_cast<int>(stage), index, offset) : 0;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        const int t = opt && opt->texture_type ? opt->texture_type(opt->user, static_cast<int>(stage), handle)
                                               : NX2X_T_2D;
        return static_cast<Shader::TextureType>(t);
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return opt && opt->texture_integer ? opt->texture_integer(opt->user, static_cast<int>(stage), handle) != 0
                                           : false;
    }
    u32 ReadViewportTransformState() override { return 1; }   // NVN always transforms by the viewport
    u32 TextureBoundBuffer() const override { return texture_bound == ~0U ? 0 : texture_bound; }
    u32 LocalMemorySize() const override { return sph.LocalMemorySize(); }
    u32 SharedMemorySize() const override {
        return stage == Shader::Stage::Compute ? (opt && opt->shared_size ? opt->shared_size : 0xC000) : 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        if (stage != Shader::Stage::Compute) return {1, 1, 1};
        if (opt && opt->workgroup[0]) return {opt->workgroup[0], opt->workgroup[1], opt->workgroup[2]};
        return {64, 1, 1};
    }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

private:
    std::vector<u64> code;
    u32 texture_bound;
    const Nx2XOptions *opt;
};

Shader::Stage StageOf(const Shader::ProgramHeader &sph) {
    switch (sph.common0.shader_type.Value()) {
    case 1: return Shader::Stage::VertexB;
    case 2: return Shader::Stage::TessellationControl;
    case 3: return Shader::Stage::TessellationEval;
    case 4: return Shader::Stage::Geometry;
    case 5: return Shader::Stage::Fragment;
    default: throw std::runtime_error(fmt::format("SPH shader type {}", sph.common0.shader_type.Value()));
    }
}

Shader::Profile MakeProfile() {
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp16_denorm_preserve = p.support_fp32_denorm_preserve = true;
    p.support_fp16_denorm_flush = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_fp64_signed_zero_nan_preserve = true;
    p.support_vote = p.support_shuffle_relative = p.support_quad_shuffles = true;
    p.supported_subgroup_stages = 0xffffffff;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;   // depth conversion in the shader (no VK_EXT_depth_clip_control)
    p.support_scaled_attributes = true;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = false;
    p.has_broken_fp16_float_controls = true;   // NVIDIA proprietary, as Eglantine sets it
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = 16;
    p.max_user_clip_distances = 8;
    return p;
}

Shader::HostTranslateInfo MakeHostInfo() {
    Shader::HostTranslateInfo h{};
    h.min_ssbo_alignment = 16;
    h.max_per_stage_descriptor_sampled_images = 1u << 20;
    h.max_per_stage_resources = 1u << 20;
    h.max_descriptor_set_samplers = 1u << 20;
    h.max_descriptor_set_uniform_buffers = 1u << 20;
    h.max_descriptor_set_uniform_buffers_dynamic = 1u << 20;
    h.max_descriptor_set_storage_buffers = 1u << 20;
    h.max_descriptor_set_storage_buffers_dynamic = 1u << 20;
    h.max_descriptor_set_sampled_images = 1u << 20;
    h.max_descriptor_set_storage_images = 1u << 20;
    h.max_descriptor_set_input_attachements = 1u << 20;
    h.support_float64 = h.support_float16 = h.support_int64 = true;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    return h;
}

// Descriptors in the order EmitContext defines them (constant buffers, storage buffers, texture buffers, image
// buffers, textures, images), each taking `count` bindings from the shared counter.
void ListDescriptors(const Shader::Info &info, u32 &binding, Nx2XStage &o) {
    auto add = [&](Nx2XDesc d) {
        d.binding = binding;
        binding += d.count;
        if (o.ndesc < static_cast<int>(sizeof o.desc / sizeof *o.desc)) o.desc[o.ndesc++] = d;
    };
    for (const auto &c : info.constant_buffer_descriptors) {
        Nx2XDesc d{};
        d.kind = NX2X_CBUF, d.index = c.index, d.count = c.count;
        add(d);
    }
    for (const auto &s : info.storage_buffers_descriptors) {
        Nx2XDesc d{};
        d.kind = NX2X_SSBO, d.cbuf_index = s.cbuf_index, d.cbuf_offset = s.cbuf_offset, d.count = s.count;
        d.is_written = s.is_written;
        add(d);
    }
    for (const auto &t : info.texture_buffer_descriptors) {
        Nx2XDesc d{};
        d.kind = NX2X_TEXBUF, d.cbuf_index = t.cbuf_index, d.cbuf_offset = t.cbuf_offset, d.count = t.count;
        d.has_secondary = t.has_secondary, d.secondary_cbuf_index = t.secondary_cbuf_index;
        d.secondary_cbuf_offset = t.secondary_cbuf_offset, d.shift_left = t.shift_left;
        d.secondary_shift_left = t.secondary_shift_left, d.size_shift = t.size_shift;
        add(d);
    }
    for (const auto &t : info.image_buffer_descriptors) {
        Nx2XDesc d{};
        d.kind = NX2X_IMGBUF, d.cbuf_index = t.cbuf_index, d.cbuf_offset = t.cbuf_offset, d.count = t.count;
        d.is_written = t.is_written, d.is_integer = t.is_integer, d.image_format = static_cast<int>(t.format);
        d.size_shift = t.size_shift;
        add(d);
    }
    for (const auto &t : info.texture_descriptors) {
        Nx2XDesc d{};
        d.kind = NX2X_TEXTURE, d.cbuf_index = t.cbuf_index, d.cbuf_offset = t.cbuf_offset, d.count = t.count;
        d.type = static_cast<int>(t.type), d.is_depth = t.is_depth, d.is_integer = t.is_integer;
        d.is_multisample = t.is_multisample, d.has_secondary = t.has_secondary;
        d.secondary_cbuf_index = t.secondary_cbuf_index, d.secondary_cbuf_offset = t.secondary_cbuf_offset;
        d.shift_left = t.shift_left, d.secondary_shift_left = t.secondary_shift_left, d.size_shift = t.size_shift;
        add(d);
    }
    for (const auto &t : info.image_descriptors) {
        Nx2XDesc d{};
        d.kind = NX2X_IMAGE, d.cbuf_index = t.cbuf_index, d.cbuf_offset = t.cbuf_offset, d.count = t.count;
        d.type = static_cast<int>(t.type), d.is_written = t.is_written, d.is_integer = t.is_integer;
        d.image_format = static_cast<int>(t.format), d.size_shift = t.size_shift;
        add(d);
    }
}

}   // namespace

extern "C" int nx2_xlate_program(int n, const void *const *code, const size_t *size, int compute,
                                 const Nx2XOptions *opt, Nx2XStage *out) {
    static const Shader::Profile base_profile = MakeProfile();
    Shader::Profile profile = base_profile;
    if (opt && opt->float_controls_set) {
        profile.support_fp16_denorm_flush = opt->denorm_flush16 != 0;
        profile.support_fp32_denorm_flush = opt->denorm_flush32 != 0;
        profile.support_fp16_denorm_preserve = opt->denorm_preserve16 != 0;
        profile.support_fp32_denorm_preserve = opt->denorm_preserve32 != 0;
        profile.support_fp16_signed_zero_nan_preserve = opt->szinf_nan16 != 0;
        profile.support_fp32_signed_zero_nan_preserve = opt->szinf_nan32 != 0;
        profile.support_fp64_signed_zero_nan_preserve = opt->szinf_nan64 != 0;
    }
    static const Shader::HostTranslateInfo host_info = MakeHostInfo();
    for (int i = 0; i < n; ++i) std::memset(&out[i], 0, sizeof out[i]);
    if (n <= 0 || n > 5) return 1;

    struct Pools {
        Shader::ObjectPool<Shader::IR::Inst> inst{8192};
        Shader::ObjectPool<Shader::IR::Block> block{32};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow{32};
    };
    Pools pools;
    std::vector<Shader::IR::Program> programs;
    programs.reserve(n);
    int at = 0;
    try {
        for (at = 0; at < n; ++at) {
            Shader::ProgramHeader sph{};
            u32 texture_bound = ~0U;
            auto lowered = Lower(static_cast<const u8 *>(code[at]), size[at], sph, texture_bound, compute != 0);
            const auto stage = compute ? Shader::Stage::Compute : StageOf(sph);
            Env env{std::move(lowered), sph, stage, texture_bound, opt};
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow, sizeof(Shader::ProgramHeader), false);
            programs.push_back(Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info));
        }
        Shader::Backend::Bindings bindings;
        const Shader::IR::Program *previous = nullptr;
        for (at = 0; at < n; ++at) {
            auto &program = programs[at];
            Shader::RuntimeInfo rt{};
            if (previous) {
                rt.previous_stage_stores = previous->info.stores;
                rt.previous_stage_legacy_stores_mapping = previous->info.legacy_stores_mapping;
            } else {
                rt.previous_stage_stores.mask.set();
            }
            if (opt) {
                if (program.stage == Shader::Stage::VertexB) {
                    for (int k = 0; k < 32; ++k)
                        rt.generic_input_types[k] = static_cast<Shader::AttributeType>(opt->attrib_types[k]);
                    rt.convert_depth_mode = opt->convert_depth != 0;
                }
                if (program.stage == Shader::Stage::Fragment)
                    for (int k = 0; k < 8; ++k)
                        rt.color_output_types[k] = static_cast<Shader::AttributeType>(opt->color_types[k]);
                rt.input_topology = static_cast<Shader::InputTopology>(opt->topology);
                rt.y_negate = opt->y_negate != 0;
            }
            Shader::Maxwell::ConvertLegacyToGeneric(program, rt);
            Nx2XStage &o = out[at];
            u32 first = bindings.unified;
            const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
            u32 binding = first;
            ListDescriptors(program.info, binding, o);
            o.stage = static_cast<int>(program.stage);
            o.spirv = static_cast<uint32_t *>(std::malloc(spirv.size() * 4));
            if (!o.spirv) throw std::runtime_error("out of memory");
            std::memcpy(o.spirv, spirv.data(), spirv.size() * 4);
            o.words = spirv.size();
            for (int k = 0; k < 32; ++k)
                if (program.info.loads.Generic(k)) o.loads_generic |= 1u << k;
            for (int k = 0; k < 8; ++k)
                if (program.info.stores_frag_color[k]) o.stores_frag |= 1u << k;
            for (int k = 0; k < 18; ++k) o.cbuf_used_size[k] = program.info.constant_buffer_used_sizes[k];
            o.uses_rescaling = program.info.uses_rescaling_uniform;
            o.uses_render_area = program.info.uses_render_area;
            if (compute) {
                o.workgroup[0] = program.workgroup_size[0], o.workgroup[1] = program.workgroup_size[1];
                o.workgroup[2] = program.workgroup_size[2], o.shared_size = program.shared_memory_size;
            }
            previous = &program;
        }
    } catch (const std::exception &e) {
        if (at < n) std::snprintf(out[at].error, sizeof out[at].error, "%s", e.what());
        nx2_xlate_free(out, n);
        return at + 1;
    }
    return 0;
}

extern "C" void nx2_xlate_free(Nx2XStage *stages, int n) {
    for (int i = 0; i < n; ++i) {
        std::free(stages[i].spirv);
        stages[i].spirv = nullptr;
        stages[i].words = 0;
    }
}
