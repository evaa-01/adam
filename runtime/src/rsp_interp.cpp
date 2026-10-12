// RSP microcode interpreter for the live shell: runs the game's audio microcode straight from RDRAM instead of the
// C++ that RSPRecomp generates from the ROM at build time (rsp\aspMain.cpp, which can't be shipped).
//
// Every instruction does exactly what RSPRecomp (N64Recomp\RSPRecomp\src\rsp_recomp.cpp) emits for it, using the same
// macros (rsp.hpp) and the same vector unit (rsp_vu_impl.hpp); decoding uses the same library (rabbitizer), so both
// see the same instructions, operands and element numbers. Control flow as in the generated code: a conditional
// branch evaluates its condition before the delay slot runs; `jal`/`jalr` store the full vram + 8; indirect jumps go
// to (target | 0x1000) & 0x1FFF. The DMA engine and status registers behave as RSPRecomp assumes (DMA instant,
// mfc0 reads 0). Microcode overlays (DMA into IMEM) aren't supported - the audio microcode has none.
// Verified against the generated code by `adam.exe --rsp-test <ramdump>` (main.cpp).

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "librecomp/rsp.hpp"
#include "librecomp/rsp_vu_impl.hpp"
#include "rabbitizer.hpp"
#include "common/RabbitizerConfig.h"

namespace {
    using InstrId = rabbitizer::InstrId::UniqueId;
    using Cop0Reg = rabbitizer::Registers::Rsp::Cop0;

    constexpr uint32_t IMEM_TEXT_START = 0x1080; // rspboot owns 0x1000-0x107F
    constexpr uint32_t IMEM_END = 0x2000;
    constexpr uint32_t VRAM_BASE = 0x04000000;    // RSPRecomp's instruction vram is 0x04001xxx
    constexpr uint64_t MAX_STEPS = 50'000'000;    // runaway guard

    struct Decoded {
        bool valid = false;
        InstrId id{};
        uint8_t rd, rs, rt, sa;
        uint8_t vd, vs, vt, de;
        int element;            // -1 = none
        uint16_t imm;
        int32_t simm;           // sign-extended 16-bit immediate
        int32_t imm7;           // sign-extended 7-bit vector load/store offset
        uint32_t branch_target; // IMEM address (0x1000-0x1FFF)
        bool has_delay_slot;
    };

    std::optional<int> rsp_element(const rabbitizer::InstructionRsp& instr) {
        if (instr.hasOperand(rabbitizer::OperandType::rsp_vt_elementhigh)) {
            return instr.GetRsp_elementhigh();
        }
        if (instr.hasOperand(rabbitizer::OperandType::rsp_vt_elementlow) || instr.hasOperand(rabbitizer::OperandType::rsp_vs_index)) {
            return instr.GetRsp_elementlow();
        }
        return std::nullopt;
    }

    Decoded decode_word(uint32_t word, uint32_t pc) {
        rabbitizer::InstructionRsp instr{ word, VRAM_BASE | pc };
        Decoded d;
        d.valid = true;
        d.id = instr.getUniqueId();
        d.rd = (uint8_t)instr.GetO32_rd();
        d.rs = (uint8_t)instr.GetO32_rs();
        d.rt = (uint8_t)instr.GetO32_rt();
        d.sa = (uint8_t)instr.Get_sa();
        d.vd = (uint8_t)instr.GetRsp_vd();
        d.vs = (uint8_t)instr.GetRsp_vs();
        d.vt = (uint8_t)instr.GetRsp_vt();
        d.de = (uint8_t)(instr.GetRsp_de() & 7);
        auto element = rsp_element(instr);
        d.element = element ? *element : -1;
        d.imm = instr.Get_immediate();
        d.simm = (int16_t)d.imm;
        d.imm7 = ((int8_t)(d.imm << 1)) >> 1;
        d.branch_target = instr.getBranchVramGeneric() & 0x1FFF;
        d.has_delay_slot = instr.hasDelaySlot();
        return d;
    }

    // Decoded instructions per microcode (by its RDRAM address), filled lazily.
    std::unordered_map<uint32_t, std::vector<Decoded>> decode_cache;

    std::vector<Decoded>& cache_for(uint32_t ucode_addr) {
        std::vector<Decoded>& cache = decode_cache[ucode_addr];
        if (cache.empty()) {
            cache.resize((IMEM_END - IMEM_TEXT_START) / 4);
        }
        return cache;
    }

    const Decoded& fetch(std::vector<Decoded>& cache, uint8_t* rdram, uint32_t ucode_addr, uint32_t pc) {
        Decoded& d = cache[(pc - IMEM_TEXT_START) / 4];
        if (!d.valid) {
            // ucode_addr may be a KSEG0 or a physical address
            uint32_t vaddr = 0x80000000u | ((ucode_addr & 0x1FFFFFFF) + (pc - IMEM_TEXT_START));
            uint32_t word = (uint32_t)MEM_W(0, (gpr)(int32_t)vaddr);
            d = decode_word(word, pc);
        }
        return d;
    }

    // Calls rsp.NAME<element>(args...) with the element known only at runtime.
#define RSP_ELEMENT_CALL(NAME, ...) \
    switch (d.element) { \
        case 0: rsp.NAME<0>(__VA_ARGS__); break;   case 1: rsp.NAME<1>(__VA_ARGS__); break; \
        case 2: rsp.NAME<2>(__VA_ARGS__); break;   case 3: rsp.NAME<3>(__VA_ARGS__); break; \
        case 4: rsp.NAME<4>(__VA_ARGS__); break;   case 5: rsp.NAME<5>(__VA_ARGS__); break; \
        case 6: rsp.NAME<6>(__VA_ARGS__); break;   case 7: rsp.NAME<7>(__VA_ARGS__); break; \
        case 8: rsp.NAME<8>(__VA_ARGS__); break;   case 9: rsp.NAME<9>(__VA_ARGS__); break; \
        case 10: rsp.NAME<10>(__VA_ARGS__); break; case 11: rsp.NAME<11>(__VA_ARGS__); break; \
        case 12: rsp.NAME<12>(__VA_ARGS__); break; case 13: rsp.NAME<13>(__VA_ARGS__); break; \
        case 14: rsp.NAME<14>(__VA_ARGS__); break; case 15: rsp.NAME<15>(__VA_ARGS__); break; \
        default: return false; \
    }

// Vector operand shapes, as in RSPRecomp's vector_operands table.
#define VEC_LOADSTORE(ID, NAME) case InstrId::ID: RSP_ELEMENT_CALL(NAME, rsp.vpu.r[d.vt], r[d.rs], d.imm7) return true;
#define VEC_LOADSTORE_IDX(ID, NAME) case InstrId::ID: RSP_ELEMENT_CALL(NAME, d.vt, r[d.rs], d.imm7) return true;
#define VEC_VVV(ID, NAME) case InstrId::ID: RSP_ELEMENT_CALL(NAME, rsp.vpu.r[d.vd], rsp.vpu.r[d.vs], rsp.vpu.r[d.vt]) return true;
#define VEC_VDEVT(ID, NAME) case InstrId::ID: RSP_ELEMENT_CALL(NAME, rsp.vpu.r[d.vd], d.de, rsp.vpu.r[d.vt]) return true;

    // Executes one non-control-flow instruction. Returns false if it isn't supported.
    bool execute(const Decoded& d, uint8_t* rdram, uint32_t* r, uint32_t& dma_mem_address, uint32_t& dma_dram_address, RSP& rsp) {
        switch (d.id) {
            // Vector loads/stores: Vt, Rs, Imm7 (STV/LTV take the register index)
            VEC_LOADSTORE(rsp_lbv, LBV) VEC_LOADSTORE(rsp_ldv, LDV) VEC_LOADSTORE(rsp_lfv, LFV)
            VEC_LOADSTORE(rsp_lhv, LHV) VEC_LOADSTORE(rsp_llv, LLV) VEC_LOADSTORE(rsp_lpv, LPV)
            VEC_LOADSTORE(rsp_lqv, LQV) VEC_LOADSTORE(rsp_lrv, LRV) VEC_LOADSTORE(rsp_lsv, LSV)
            VEC_LOADSTORE(rsp_luv, LUV) VEC_LOADSTORE(rsp_sbv, SBV) VEC_LOADSTORE(rsp_sdv, SDV)
            VEC_LOADSTORE(rsp_sfv, SFV) VEC_LOADSTORE(rsp_shv, SHV) VEC_LOADSTORE(rsp_slv, SLV)
            VEC_LOADSTORE(rsp_spv, SPV) VEC_LOADSTORE(rsp_sqv, SQV) VEC_LOADSTORE(rsp_srv, SRV)
            VEC_LOADSTORE(rsp_ssv, SSV) VEC_LOADSTORE(rsp_suv, SUV) VEC_LOADSTORE(rsp_swv, SWV)
            VEC_LOADSTORE_IDX(rsp_stv, STV) VEC_LOADSTORE_IDX(rsp_ltv, LTV)
            // Vd, Vs, Vt
            VEC_VVV(rsp_vabs, VABS) VEC_VVV(rsp_vadd, VADD) VEC_VVV(rsp_vaddc, VADDC) VEC_VVV(rsp_vand, VAND)
            VEC_VVV(rsp_vch, VCH) VEC_VVV(rsp_vcl, VCL) VEC_VVV(rsp_vcr, VCR) VEC_VVV(rsp_veq, VEQ)
            VEC_VVV(rsp_vge, VGE) VEC_VVV(rsp_vlt, VLT) VEC_VVV(rsp_vmacf, VMACF) VEC_VVV(rsp_vmacu, VMACU)
            VEC_VVV(rsp_vmadh, VMADH) VEC_VVV(rsp_vmadl, VMADL) VEC_VVV(rsp_vmadm, VMADM) VEC_VVV(rsp_vmadn, VMADN)
            VEC_VVV(rsp_vmrg, VMRG) VEC_VVV(rsp_vmudh, VMUDH) VEC_VVV(rsp_vmudl, VMUDL) VEC_VVV(rsp_vmudm, VMUDM)
            VEC_VVV(rsp_vmudn, VMUDN) VEC_VVV(rsp_vne, VNE) VEC_VVV(rsp_vnor, VNOR) VEC_VVV(rsp_vnxor, VNXOR)
            VEC_VVV(rsp_vor, VOR) VEC_VVV(rsp_vsub, VSUB) VEC_VVV(rsp_vsubc, VSUBC) VEC_VVV(rsp_vmulf, VMULF)
            VEC_VVV(rsp_vmulu, VMULU) VEC_VVV(rsp_vmulq, VMULQ) VEC_VVV(rsp_vnand, VNAND) VEC_VVV(rsp_vxor, VXOR)
            case InstrId::rsp_vsar: RSP_ELEMENT_CALL(VSAR, rsp.vpu.r[d.vd], rsp.vpu.r[d.vs]) return true;
            case InstrId::rsp_vmacq: rsp.VMACQ(rsp.vpu.r[d.vd]); return true;
            case InstrId::rsp_vrndn: RSP_ELEMENT_CALL(VRNDN, rsp.vpu.r[d.vd], d.vs, rsp.vpu.r[d.vt]) return true;
            case InstrId::rsp_vrndp: RSP_ELEMENT_CALL(VRNDP, rsp.vpu.r[d.vd], d.vs, rsp.vpu.r[d.vt]) return true;
            // Vd, De, Vt
            VEC_VDEVT(rsp_vmov, VMOV) VEC_VDEVT(rsp_vrcp, VRCP) VEC_VDEVT(rsp_vrcpl, VRCPL) VEC_VDEVT(rsp_vrcph, VRCPH)
            VEC_VDEVT(rsp_vrsq, VRSQ) VEC_VDEVT(rsp_vrsql, VRSQL) VEC_VDEVT(rsp_vrsqh, VRSQH)
            // Rt, Vs
            case InstrId::rsp_mfc2: RSP_ELEMENT_CALL(MFC2, r[d.rt], rsp.vpu.r[d.vs]) return true;
            case InstrId::rsp_mtc2: RSP_ELEMENT_CALL(MTC2, r[d.rt], rsp.vpu.r[d.vs]) return true;
            case InstrId::rsp_vnop: return true;

            // Scalar instructions (rsp_recomp.cpp's "implement the instruction directly" part)
            case InstrId::rsp_nop: return true;
            case InstrId::rsp_lui: r[d.rt] = S32((uint32_t)d.imm << 16); return true;
            case InstrId::rsp_add: case InstrId::rsp_addu: r[d.rd] = RSP_ADD32(r[d.rs], r[d.rt]); return true;
            case InstrId::rsp_negu: case InstrId::rsp_sub: case InstrId::rsp_subu: r[d.rd] = RSP_SUB32(r[d.rs], r[d.rt]); return true;
            case InstrId::rsp_addi: case InstrId::rsp_addiu: r[d.rt] = RSP_ADD32(r[d.rs], d.simm); return true;
            case InstrId::rsp_and: r[d.rd] = r[d.rs] & r[d.rt]; return true;
            case InstrId::rsp_andi: r[d.rt] = r[d.rs] & d.imm; return true;
            case InstrId::rsp_or: r[d.rd] = r[d.rs] | r[d.rt]; return true;
            case InstrId::rsp_ori: r[d.rt] = r[d.rs] | d.imm; return true;
            case InstrId::rsp_nor: r[d.rd] = ~(r[d.rs] | r[d.rt]); return true;
            case InstrId::rsp_xor: r[d.rd] = r[d.rs] ^ r[d.rt]; return true;
            case InstrId::rsp_xori: r[d.rt] = r[d.rs] ^ d.imm; return true;
            case InstrId::rsp_sll: r[d.rd] = S32(r[d.rt]) << d.sa; return true;
            case InstrId::rsp_sllv: r[d.rd] = S32(r[d.rt]) << (r[d.rs] & 31); return true;
            case InstrId::rsp_sra: r[d.rd] = S32(RSP_SIGNED(r[d.rt]) >> d.sa); return true;
            case InstrId::rsp_srav: r[d.rd] = S32(RSP_SIGNED(r[d.rt]) >> (r[d.rs] & 31)); return true;
            case InstrId::rsp_srl: r[d.rd] = S32(U32(r[d.rt]) >> d.sa); return true;
            case InstrId::rsp_srlv: r[d.rd] = S32(U32(r[d.rt]) >> (r[d.rs] & 31)); return true;
            case InstrId::rsp_slt: r[d.rd] = RSP_SIGNED(r[d.rs]) < RSP_SIGNED(r[d.rt]) ? 1 : 0; return true;
            case InstrId::rsp_slti: r[d.rt] = RSP_SIGNED(r[d.rs]) < d.simm ? 1 : 0; return true;
            case InstrId::rsp_sltu: r[d.rd] = r[d.rs] < r[d.rt] ? 1 : 0; return true;
            case InstrId::rsp_sltiu: r[d.rt] = r[d.rs] < (uint32_t)d.simm ? 1 : 0; return true;
            case InstrId::rsp_lw: r[d.rt] = RSP_MEM_W_LOAD(d.simm, r[d.rs]); return true;
            case InstrId::rsp_lh: r[d.rt] = RSP_MEM_H_LOAD(d.simm, r[d.rs]); return true;
            case InstrId::rsp_lb: r[d.rt] = RSP_MEM_B(d.simm, r[d.rs]); return true;
            case InstrId::rsp_lhu: r[d.rt] = RSP_MEM_HU_LOAD(d.simm, r[d.rs]); return true;
            case InstrId::rsp_lbu: r[d.rt] = RSP_MEM_BU(d.simm, r[d.rs]); return true;
            case InstrId::rsp_sw: RSP_MEM_W_STORE(d.simm, r[d.rs], r[d.rt]); return true;
            case InstrId::rsp_sh: RSP_MEM_H_STORE(d.simm, r[d.rs], r[d.rt]); return true;
            case InstrId::rsp_sb: RSP_MEM_B(d.simm, r[d.rs]) = (int8_t)r[d.rt]; return true;
            case InstrId::rsp_mfc0:
                switch (static_cast<Cop0Reg>(d.rd)) {
                    case Cop0Reg::RSP_COP0_SP_STATUS: case Cop0Reg::RSP_COP0_SP_DMA_FULL: case Cop0Reg::RSP_COP0_SP_DMA_BUSY:
                    case Cop0Reg::RSP_COP0_SP_SEMAPHORE: case Cop0Reg::RSP_COP0_DPC_STATUS:
                        r[d.rt] = 0;
                        return true;
                    default:
                        return false;
                }
            case InstrId::rsp_mtc0:
                switch (static_cast<Cop0Reg>(d.rd)) {
                    case Cop0Reg::RSP_COP0_SP_SEMAPHORE: case Cop0Reg::RSP_COP0_SP_STATUS: return true;
                    case Cop0Reg::RSP_COP0_SP_DRAM_ADDR: SET_DMA_DRAM(r[d.rt]); return true;
                    case Cop0Reg::RSP_COP0_SP_MEM_ADDR: SET_DMA_MEM(r[d.rt]); return true;
                    case Cop0Reg::RSP_COP0_SP_RD_LEN:
                        if (dma_mem_address & 0x1000) {
                            return false; // DMA into IMEM = microcode overlay: not supported
                        }
                        DO_DMA_READ(r[d.rt]);
                        return true;
                    case Cop0Reg::RSP_COP0_SP_WR_LEN: DO_DMA_WRITE(r[d.rt]); return true;
                    default: return false;
                }
            default:
                return false;
        }
    }

#undef VEC_LOADSTORE
#undef VEC_LOADSTORE_IDX
#undef VEC_VVV
#undef VEC_VDEVT
#undef RSP_ELEMENT_CALL

    bool is_branch(InstrId id) {
        switch (id) {
            case InstrId::rsp_j: case InstrId::rsp_b: case InstrId::rsp_jal: case InstrId::rsp_jr: case InstrId::rsp_jalr:
            case InstrId::rsp_bne: case InstrId::rsp_beq: case InstrId::rsp_bgez: case InstrId::rsp_bgtz:
            case InstrId::rsp_blez: case InstrId::rsp_bltz:
                return true;
            default:
                return false;
        }
    }
}

// Same signature as the RSPRecomp output (RspUcodeFunc); the microcode text starts at IMEM 0x1080.
RspExitReason adam_rsp_interpret(uint8_t* rdram, uint32_t ucode_addr) {
    // Decode like RSPRecomp: no pseudo-instructions for these (N64Recomp's live recompiler sets the same).
    RabbitizerConfig_Cfg.pseudos.pseudoMove = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBeqz = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBnez = false;
    RabbitizerConfig_Cfg.pseudos.pseudoNot = false;

    uint32_t r[32] = {};
    r[1] = 0xFC0; // as RSPRecomp's generated code starts (the OSTask in DMEM)
    uint32_t dma_mem_address = 0, dma_dram_address = 0;
    auto rsp_ptr = std::make_unique<RSP>(); // large (vector registers + accumulators): keep it off the stack
    RSP& rsp = *rsp_ptr;

    auto unsupported = [&](const Decoded& d, uint32_t pc) {
        fprintf(stderr, "[rsp-interp] unsupported instruction (id %d) at 0x%04X\n", (int)d.id, pc);
        return RspExitReason::Unsupported;
    };

    std::vector<Decoded>& cache = cache_for(ucode_addr); // looked up once per task
    uint32_t pc = IMEM_TEXT_START;
    for (uint64_t step = 0; step < MAX_STEPS; step++) {
        if (pc < IMEM_TEXT_START || pc >= IMEM_END) {
            return RspExitReason::ImemOverrun;
        }
        const Decoded& d = fetch(cache, rdram, ucode_addr, pc);
        if (d.id == InstrId::rsp_break) {
            return RspExitReason::Broke;
        }
        if (!is_branch(d.id)) {
            if (!execute(d, rdram, r, dma_mem_address, dma_dram_address, rsp)) {
                return unsupported(d, pc);
            }
            r[0] = 0;
            pc += 4;
            continue;
        }

        // Control flow: decide (with the registers before the delay slot), run the delay slot, then jump.
        bool taken = true;
        uint32_t target = d.branch_target;
        switch (d.id) {
            case InstrId::rsp_jal: r[31] = VRAM_BASE | (pc + 8); break;
            case InstrId::rsp_jr: target = (r[d.rs] | 0x1000) & 0x1FFF; break;
            case InstrId::rsp_jalr: target = (r[d.rs] | 0x1000) & 0x1FFF; r[d.rd] = VRAM_BASE | (pc + 8); break;
            case InstrId::rsp_bne: taken = r[d.rs] != r[d.rt]; break;
            case InstrId::rsp_beq: taken = r[d.rs] == r[d.rt]; break;
            case InstrId::rsp_bgez: taken = RSP_SIGNED(r[d.rs]) >= 0; break;
            case InstrId::rsp_bgtz: taken = RSP_SIGNED(r[d.rs]) > 0; break;
            case InstrId::rsp_blez: taken = RSP_SIGNED(r[d.rs]) <= 0; break;
            case InstrId::rsp_bltz: taken = RSP_SIGNED(r[d.rs]) < 0; break;
            default: break; // j, b
        }
        r[0] = 0;
        uint32_t delay_pc = pc + 4;
        const Decoded& delay = fetch(cache, rdram, ucode_addr, delay_pc);
        if (is_branch(delay.id) || delay.id == InstrId::rsp_break) {
            return unsupported(delay, delay_pc); // RSPRecomp doesn't handle these in delay slots either
        }
        if (!execute(delay, rdram, r, dma_mem_address, dma_dram_address, rsp)) {
            return unsupported(delay, delay_pc);
        }
        r[0] = 0;
        pc = taken ? target : pc + 8;
    }
    fprintf(stderr, "[rsp-interp] no break after %llu instructions\n", (unsigned long long)MAX_STEPS);
    return RspExitReason::Invalid;
}

// `adam.exe --rsp-test <ramdump.bin> [...]`: runs every audio task found in big-endian RAM dumps (Project64 or F9) both
// through the recompiled aspMain (rsp\aspMain.cpp) and through the interpreter, each on its own copy of memory, and
// compares RDRAM and DMEM afterwards. No window. Returns 0 when everything matches.
#ifndef ADAM_LIVE // the live shell has no recompiled aspMain to compare with
extern RspUcodeFunc aspMain;

int adam_rsp_test(int count, char** paths) {
    constexpr size_t RDRAM_SIZE = 8 * 1024 * 1024;
    constexpr uint32_t ASP_UCODE = 0x800CB7A0, ASP_UCODE_DATA = 0x800F80A0; // see rsp\aspMain.toml
    recomp::rsp::constants_init();
    int failures = 0, tasks_run = 0;

    for (int p = 0; p < count; p++) {
        FILE* f = fopen(paths[p], "rb");
        if (f == nullptr) {
            fprintf(stderr, "cannot open %s\n", paths[p]);
            return 1;
        }
        std::vector<uint8_t> dump(RDRAM_SIZE);
        size_t dump_size = fread(dump.data(), 1, RDRAM_SIZE, f);
        fclose(f);
        // The runtime keeps each 32-bit word byte-swapped (host order).
        std::vector<uint8_t> base(RDRAM_SIZE);
        for (size_t i = 0; i < RDRAM_SIZE; i++) {
            base[i] = dump[i ^ 3];
        }
        auto word = [&](size_t off) { uint32_t v; memcpy(&v, &base[off], 4); return v; };

        int found = 0;
        for (size_t off = 0; off + sizeof(OSTask) <= dump_size; off += 4) {
            if (word(off) != 2 /* M_AUDTASK */ || word(off + 16) != ASP_UCODE || word(off + 24) != ASP_UCODE_DATA) {
                continue;
            }
            found++;
            OSTask task;
            memcpy(&task, &base[off], sizeof(OSTask));

            std::vector<uint8_t> out_rdram[2];
            std::vector<uint8_t> out_dmem[2];
            RspExitReason reasons[2];
            double micros[2]; // the interpreter's first task also decodes the microcode (cached afterwards)
            RspUcodeFunc* impls[2] = { aspMain, adam_rsp_interpret };
            for (int i = 0; i < 2; i++) {
                out_rdram[i] = base;
                uint8_t* rdram = out_rdram[i].data();
                memset(dmem, 0, 0x1000);
                memcpy(&dmem[0xFC0], &task, sizeof(OSTask));
                dma_rdram_to_dmem(rdram, 0x0000, task.t.ucode_data, 0xF80 - 1);
                auto t0 = std::chrono::steady_clock::now();
                reasons[i] = impls[i](rdram, task.t.ucode);
                micros[i] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                out_dmem[i].assign(dmem, dmem + 0x1000);
            }
            size_t rdram_diffs = 0, dmem_diffs = 0, first_rdram = 0;
            for (size_t i = 0; i < RDRAM_SIZE; i++) {
                if (out_rdram[0][i] != out_rdram[1][i]) {
                    if (rdram_diffs++ == 0) first_rdram = i;
                }
            }
            for (size_t i = 0; i < 0x1000; i++) {
                dmem_diffs += out_dmem[0][i] != out_dmem[1][i];
            }
            size_t changed = 0;
            for (size_t i = 0; i < RDRAM_SIZE; i++) {
                changed += out_rdram[0][i] != base[i];
            }
            bool ok = rdram_diffs == 0 && dmem_diffs == 0 && reasons[0] == reasons[1];
            failures += ok ? 0 : 1;
            tasks_run++;
            printf("%s task @%08X (data %08X, %u bytes): %.0f us recompiled / %.0f us interpreted, exit %d/%d, aspMain changed %zu RDRAM bytes; differences: RDRAM %zu%s, DMEM %zu -> %s\n",
                paths[p], 0x80000000u + (uint32_t)off, task.t.data_ptr, task.t.data_size, micros[0], micros[1], (int)reasons[0], (int)reasons[1], changed,
                rdram_diffs, rdram_diffs ? (" (first at " + std::to_string(first_rdram ^ 3) + ")").c_str() : "", dmem_diffs, ok ? "MATCH" : "DIFFERENT");
        }
        if (found == 0) {
            printf("%s: no audio task found\n", paths[p]);
        }
    }
    printf("%d audio tasks compared, %d different\n", tasks_run, failures);
    return (failures == 0 && tasks_run > 0) ? 0 : 1;
}
#endif
