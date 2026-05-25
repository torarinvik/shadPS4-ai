// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include <magic_enum/magic_enum.hpp>

#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace Shader {

static std::string OperandTraceString(const Gcn::InstOperand& operand) {
    return fmt::format("{}:{}:type={}:code={}",
                       magic_enum::enum_name(operand.field),
                       std::to_underlying(operand.field), std::to_underlying(operand.type),
                       operand.code);
}

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    u32 decode_pc = 0;
    while (!slice.atEnd()) {
        const u32 token = slice.at(0);
        if (Gcn::GetInstructionEncoding(token) == Gcn::InstEncoding::ILLEGAL &&
            !program.ins_list.empty()) {
            LOG_WARNING(Render_Recompiler,
                        "Stopping shader decode at pc={:#x}: illegal encoding token {:#x} after "
                        "{} instructions. Treating remaining dwords as trailing metadata.",
                        decode_pc, token, program.ins_list.size());
            break;
        }
        auto inst = decoder.decodeInstruction(slice);
        if (inst.category == Gcn::InstCategory::Undefined && !program.ins_list.empty()) {
            LOG_WARNING(Render_Recompiler,
                        "Stopping shader decode at pc={:#x}: undefined opcode {} after {} "
                        "instructions. Treating remaining dwords as trailing metadata.",
                        decode_pc, static_cast<u32>(inst.opcode), program.ins_list.size());
            break;
        }
        decode_pc += inst.length;
        program.ins_list.emplace_back(inst);
    }
    if (std::getenv("SHADPS4_TRACE_SHADER_DECODE") != nullptr) {
        u32 pc = 0;
        for (size_t i = 0; i < program.ins_list.size(); ++i) {
            const auto& inst = program.ins_list[i];
            LOG_WARNING(Render_Recompiler,
                        "Decoded shader inst index={} pc={:#x} len={} opcode={} ({}) category={} "
                        "src=[{}, {}, {}, {}] dst=[{}, {}]",
                        i, pc, inst.length, static_cast<u32>(inst.opcode),
                        magic_enum::enum_name(inst.opcode), static_cast<u32>(inst.category),
                        OperandTraceString(inst.src[0]), OperandTraceString(inst.src[1]),
                        OperandTraceString(inst.src[2]), OperandTraceString(inst.src[3]),
                        OperandTraceString(inst.dst[0]), OperandTraceString(inst.dst[1]));
            pc += inst.length;
        }
    }

    // Clear any previous pooled data.
    pools.ReleaseContents();

    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};

    // Structurize control flow graph and create program.
    program.syntax_list =
        Shader::Gcn::BuildASL(pools.inst_pool, pools.block_pool, cfg, info, runtime_info, profile);
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front());

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    Shader::InjectClipDistanceAttributes(program, runtime_info);

    // Run optimization passes
    if (!profile.support_float64) {
        Shader::Optimization::LowerFp64ToFp32(program);
    }
    Shader::Optimization::SsaRewritePass(program.post_order_blocks);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Shader::Optimization::IdentityRemovalPass(program.blocks);
    if (info.l_stage == LogicalStage::TessellationControl) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::HullShaderTransform(program, runtime_info);
    } else if (info.l_stage == LogicalStage::TessellationEval) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::DomainShaderTransform(program, runtime_info);
    }
    Shader::Optimization::RingAccessElimination(program, runtime_info);
    Shader::Optimization::ReadLaneEliminationPass(program);
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    Shader::Optimization::ResourceTrackingPass(program, profile);
    Shader::Optimization::LowerBufferFormatToRaw(program);
    Shader::Optimization::SharedMemorySimplifyPass(program, profile);
    Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile);
    Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);
    Shader::Optimization::IdentityRemovalPass(program.blocks);
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Shader::Optimization::CollectShaderInfoPass(program, profile);

    Shader::IR::DumpProgram(program, info);

    return program;
}

} // namespace Shader
