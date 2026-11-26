#include "ppc_detour.h"

#define OP_B   (18u << 26)
#define OP_BC  (16u << 26)
#define OP_MASK (63u << 26)

#define PPC_LIS(rd, imm)       ((15u << 26) | ((uint32_t)(rd) << 21) | ((imm) & 0xFFFFu))
#define PPC_ORI(rs, ra, imm)   ((24u << 26) | ((uint32_t)(rs) << 21) | ((uint32_t)(ra) << 16) | ((imm) & 0xFFFFu))
/* mtspr 9 (CTR): o campo SPR vai com as duas metades trocadas (9 << 5), como POWERPC_SPR. */
#define PPC_MTCTR(rs)          ((31u << 26) | ((uint32_t)(rs) << 21) | ((9u << 5) << 11) | (467u << 1))
#define PPC_BCCTR(bo, bi, lk)  ((19u << 26) | ((uint32_t)(bo) << 21) | ((uint32_t)(bi) << 16) | (528u << 1) | ((uint32_t)(lk) & 1u))
#define PPC_STD(rs, disp, ra)  ((62u << 26) | ((uint32_t)(rs) << 21) | ((uint32_t)(ra) << 16) | ((uint32_t)(disp) & 0xFFFFu))
#define PPC_LD(rd, disp, ra)   ((58u << 26) | ((uint32_t)(rd) << 21) | ((uint32_t)(ra) << 16) | ((uint32_t)(disp) & 0xFFFFu))

#define PRESERVE_SLOT (-0x30)

size_t ppc_write_far_branch(uint32_t *dst, uint32_t target, bool linked, bool preserve,
                            uint32_t branch_options, uint8_t cr_bit, uint8_t reg) {
    const uint32_t hi = target >> 16;
    const uint32_t lo = target & 0xFFFFu;
    const uint32_t lk = linked ? 1u : 0u;
    size_t n = 0;
    if (!dst) return preserve ? 6u : 4u;
    if (preserve) dst[n++] = PPC_STD(reg, PRESERVE_SLOT, 1u);
    dst[n++] = PPC_LIS(reg, hi);
    dst[n++] = PPC_ORI(reg, reg, lo);
    dst[n++] = PPC_MTCTR(reg);
    if (preserve) dst[n++] = PPC_LD(reg, PRESERVE_SLOT, 1u);
    dst[n++] = PPC_BCCTR(branch_options, cr_bit, lk);
    return n;
}

size_t ppc_copy_instruction(uint32_t *dst, const uint32_t *src, uint32_t src_addr) {
    const uint32_t insn = *src;
    const uint32_t op = insn & OP_MASK;

    if (op != OP_B && op != OP_BC) {
        *dst = insn;
        return 1u;
    }
    /* Desvio absoluto (AA) nao depende de onde a instrucao esta. */
    if (insn & 2u) {
        *dst = insn;
        return 1u;
    }

    uint32_t bits, bo = PPC_BRANCH_OPTIONS_ALWAYS, bi = 0;
    if (op == OP_B) {
        bits = 24u;
    } else {
        bits = 14u;
        bo = (insn >> 21) & 0x1Fu;
        bi = (insn >> 16) & 0x1Fu;
    }
    const uint32_t field_mask = ((1u << bits) - 1u) << 2;
    uint32_t offset = insn & field_mask;
    if (offset >> (bits + 1u)) offset |= ~((1u << (bits + 2u)) - 1u); /* extensao de sinal */
    const uint32_t target = src_addr + offset;

    return ppc_write_far_branch(dst, target, (insn & 1u) != 0u, true, bo, (uint8_t)bi, 0u);
}

bool ppc_detour_install(ppc_detour_t *d, uint32_t *source, uint32_t source_addr,
                        uint32_t target_addr, uint32_t *tramp, uint32_t tramp_addr,
                        size_t tramp_cap, size_t *tramp_used, ppc_flush_fn flush) {
    if (!d || !source || !tramp || d->installed) return false;
    if (tramp_cap < PPC_DETOUR_TRAMPOLINE_WORDS) return false;

    size_t used = 0;
    for (size_t i = 0; i < PPC_DETOUR_HOOK_WORDS; i++) {
        d->original[i] = source[i];
        used += ppc_copy_instruction(tramp + used, source + i, source_addr + (uint32_t)(4u * i));
    }
    /* O trampolim volta para depois do desvio; r0 e preservado, como no hiddriver. */
    used += ppc_write_far_branch(tramp + used, source_addr + 4u * PPC_DETOUR_HOOK_WORDS,
                                 false, true, PPC_BRANCH_OPTIONS_ALWAYS, 0u, 0u);
    if (flush) flush(tramp, used * sizeof(uint32_t));

    d->source = source;
    d->trampoline = tramp;
    d->trampoline_addr = tramp_addr;
    ppc_write_far_branch(source, target_addr, false, false, PPC_BRANCH_OPTIONS_ALWAYS, 0u, 0u);
    if (flush) flush(source, PPC_DETOUR_HOOK_WORDS * sizeof(uint32_t));
    d->installed = true;
    if (tramp_used) *tramp_used = used;
    return true;
}
