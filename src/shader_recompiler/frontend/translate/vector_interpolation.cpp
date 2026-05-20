// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/profile.h"
#include "common/logging/log.h"

namespace Shader::Gcn {

using Interpolation = Info::Interpolation;

static Interpolation GetInterpolation(IR::Attribute attribute) {
    switch (attribute) {
    case IR::Attribute::BaryCoordNoPersp:
        return {Qualifier::NoPerspective, Qualifier::None};
    case IR::Attribute::BaryCoordNoPerspCentroid:
        return {Qualifier::NoPerspective, Qualifier::Centroid};
    case IR::Attribute::BaryCoordNoPerspSample:
        return {Qualifier::NoPerspective, Qualifier::Sample};
    case IR::Attribute::BaryCoordSmooth:
        return {Qualifier::Smooth, Qualifier::None};
    case IR::Attribute::BaryCoordSmoothCentroid:
        return {Qualifier::Smooth, Qualifier::Centroid};
    case IR::Attribute::BaryCoordSmoothSample:
        return {Qualifier::Smooth, Qualifier::Sample};
    default:
        LOG_WARNING(Render_Recompiler, "Unhandled barycentric attribute {}; using smooth center",
                    NameOf(attribute));
        return {Qualifier::Smooth, Qualifier::None};
    }
}

static constexpr std::array DefaultValTable = {
    std::array{0.0f, 0.0f, 0.0f, 0.0f},
    std::array{0.0f, 0.0f, 0.0f, 1.0f},
    std::array{1.0f, 1.0f, 1.0f, 0.0f},
    std::array{1.0f, 1.0f, 1.0f, 1.0f},
};

void Translator::EmitVectorInterpolation(const GcnInst& inst) {
    switch (inst.opcode) {
        // VINTRP
    case Opcode::V_INTERP_P1_F32:
        return V_INTERP_P1_F32(inst);
    case Opcode::V_INTERP_P2_F32:
        return V_INTERP_P2_F32(inst);
    case Opcode::V_INTERP_MOV_F32:
        return V_INTERP_MOV_F32(inst);
    default:
        LogMissingOpcode(inst);
    }
}

// VINTRP

void Translator::V_INTERP_P1_F32(const GcnInst& inst) {
    if (!profile.needs_manual_interpolation) {
        return;
    }
    const u32 attr_index = inst.control.vintrp.attr;
    if (attr_index >= runtime_info.fs_info.num_inputs ||
        attr_index >= runtime_info.fs_info.inputs.size()) {
        LOG_WARNING(Render_Recompiler, "Skipping V_INTERP_P1 with invalid attr index {}",
                    attr_index);
        return;
    }
    const auto& attr = runtime_info.fs_info.inputs[attr_index];
    if (attr.IsDefault()) {
        return;
    }
    // VDST = P10 * VSRC + P0
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
    const IR::F32 p1 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 1);
    const IR::F32 i = GetSrc<IR::F32>(inst.src[0]);
    const IR::F32 result = ir.FPFma(ir.FPSub(p1, p0), i, p0);
    SetDst(inst.dst[0], result);
}

void Translator::V_INTERP_P2_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    if (attr_index >= runtime_info.fs_info.num_inputs ||
        attr_index >= runtime_info.fs_info.inputs.size()) {
        LOG_WARNING(Render_Recompiler, "Replacing V_INTERP_P2 with zero for invalid attr index {}",
                    attr_index);
        SetDst(inst.dst[0], ir.Imm32(0.f));
        return;
    }
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const auto& attr = runtime_info.fs_info.inputs[attr_index];
    auto& interp = info.fs_interpolation[attr_index];
    if (attr.IsDefault()) {
        SetDst(inst.dst[0],
               ir.Imm32(DefaultValTable[attr.default_value][inst.control.vintrp.chan]));
        return;
    }
    ASSERT(!attr.is_flat);
    if (!profile.needs_manual_interpolation) {
        if (inst.src[0].code >= vgpr_to_interp.size()) {
            LOG_WARNING(Render_Recompiler,
                        "V_INTERP_P2 source VGPR {} has no interpolation mapping; using smooth "
                        "center",
                        inst.src[0].code);
            interp = {Qualifier::Smooth, Qualifier::None};
        } else {
            interp = GetInterpolation(vgpr_to_interp[inst.src[0].code]);
        }
        SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));
        return;
    }
    // VDST = P20 * VSRC + VDST
    const IR::F32 p0 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 0);
    const IR::F32 p2 = ir.GetAttribute(attrib, inst.control.vintrp.chan, 2);
    const IR::F32 j = GetSrc<IR::F32>(inst.src[0]);
    const IR::F32 result = ir.FPFma(ir.FPSub(p2, p0), j, GetSrc<IR::F32>(inst.dst[0]));
    interp.primary = Qualifier::PerVertex;
    SetDst(inst.dst[0], result);
}

void Translator::V_INTERP_MOV_F32(const GcnInst& inst) {
    const u32 attr_index = inst.control.vintrp.attr;
    if (attr_index >= runtime_info.fs_info.num_inputs ||
        attr_index >= runtime_info.fs_info.inputs.size()) {
        LOG_WARNING(Render_Recompiler, "Replacing V_INTERP_MOV with zero for invalid attr index {}",
                    attr_index);
        SetDst(inst.dst[0], ir.Imm32(0.f));
        return;
    }
    const IR::Attribute attrib = IR::Attribute::Param0 + attr_index;
    const auto& attr = runtime_info.fs_info.inputs[attr_index];
    auto& interp = info.fs_interpolation[attr_index];
    ASSERT(attr.is_flat || inst.src[0].code == 2);
    if (profile.supports_amd_shader_explicit_vertex_parameter ||
        (profile.supports_fragment_shader_barycentric &&
         !profile.has_incomplete_fragment_shader_barycentric)) {
        // VSRC 0=P10, 1=P20, 2=P0
        interp.primary = Qualifier::PerVertex;
        SetDst(inst.dst[0],
               ir.GetAttribute(attrib, inst.control.vintrp.chan, (inst.src[0].code + 1) % 3));
    } else {
        interp.primary = Qualifier::Flat;
        SetDst(inst.dst[0], ir.GetAttribute(attrib, inst.control.vintrp.chan));
    }
}

} // namespace Shader::Gcn
