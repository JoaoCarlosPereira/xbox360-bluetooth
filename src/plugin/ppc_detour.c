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
/* mfspr rd, 9 (CTR). O campo SPR vai com as metades trocadas, como no mtctr. */
#define PPC_MFCTR(rd)          ((31u << 26) | ((uint32_t)(rd) << 21) | ((9u << 5) << 11) | (339u << 1))
/* mtspr 8 (LR). */
#define PPC_MTLR(rs)           ((31u << 26) | ((uint32_t)(rs) << 21) | ((8u << 5) << 11) | (467u << 1))
#define PPC_BLR()              0x4E800020u

#define PRESERVE_SLOT (-0x30)
#define CTR_SLOT       (-0x38)

/* mfspr rd, 8: o LR ja foi copiado para um registrador. Sem isso o retorno nao pode usar o LR. */
static bool insn_is_mflr(uint32_t insn) {
    return (insn & 0xFC1FFFFFu) == 0x7C0802A6u;
}

static bool prologue_saved_lr(const uint32_t *insns) {
    for (size_t i = 0; i < PPC_DETOUR_HOOK_WORDS; i++) {
        if (insn_is_mflr(insns[i])) return true;
    }
    return false;
}

/* blr, bctr ou bctrl: a funcao acaba nessas 4 instrucoes. Desviar cai na seguinte. */
static bool insn_is_return(uint32_t insn) {
    if (insn == 0x4E800020u) return true;
    return (insn & 0xFC00FFFEu) == (0x4E800420u & 0xFC00FFFEu);
}

static bool prologue_returns(const uint32_t *insns) {
    for (size_t i = 0; i < PPC_DETOUR_HOOK_WORDS; i++) {
        if (insn_is_return(insns[i])) return true;
    }
    return false;
}

/* Volta para o resto da funcao sem perder o CTR. O mflr das instrucoes copiadas ja guardou o LR. */
static size_t ppc_write_lr_return(uint32_t *dst, uint32_t target) {
    const uint32_t hi = target >> 16;
    const uint32_t lo = target & 0xFFFFu;
    size_t n = 0;
    if (!dst) return 10u;
    dst[n++] = PPC_STD(0u, CTR_SLOT, 1u);
    dst[n++] = PPC_MFCTR(0u);
    dst[n++] = PPC_STD(0u, PRESERVE_SLOT, 1u);
    dst[n++] = PPC_LIS(0u, hi);
    dst[n++] = PPC_ORI(0u, 0u, lo);
    dst[n++] = PPC_MTLR(0u);
    dst[n++] = PPC_LD(0u, PRESERVE_SLOT, 1u);
    dst[n++] = PPC_MTCTR(0u);
    dst[n++] = PPC_LD(0u, CTR_SLOT, 1u);
    dst[n++] = PPC_BLR();
    return n;
}

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
    /* Thunk que termina em bctr/blr nessas 4 instrucoes: desviar cai na funcao seguinte. */
    const bool saved_lr = prologue_saved_lr(source);
    if (!saved_lr && prologue_returns(source)) return false;

    size_t used = 0;
    for (size_t i = 0; i < PPC_DETOUR_HOOK_WORDS; i++) {
        d->original[i] = source[i];
        used += ppc_copy_instruction(tramp + used, source + i, source_addr + (uint32_t)(4u * i));
    }
    /* Com mflr, o LR original ja esta num registrador: o retorno devolve o CTR.
     * Sem mflr, o LR ainda e o de quem chamou: o retorno usa o CTR e nao mexe no LR. */
    if (saved_lr)
        used += ppc_write_lr_return(tramp + used, source_addr + 4u * PPC_DETOUR_HOOK_WORDS);
    else
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

/* b incondicional (não bl): o corpo curto termina saltando. */
static bool insn_is_tail_branch(uint32_t insn) {
    return (insn & OP_MASK) == OP_B && (insn & 1u) == 0u;
}

bool ppc_detour_install_body(ppc_detour_t *d, uint32_t *source, uint32_t source_addr,
                             uint32_t target_addr, uint32_t *tramp, uint32_t tramp_addr,
                             size_t tramp_cap, size_t *tramp_used, ppc_flush_fn flush) {
    size_t count = 0;
    size_t used = 0;
    if (!d || !source || !tramp || d->installed) return false;
    if (tramp_cap < PPC_DETOUR_TRAMPOLINE_WORDS) return false;
    for (size_t i = 0; i < 16u; i++) {
        count = i + 1u;
        if (insn_is_return(source[i]) || insn_is_tail_branch(source[i])) break;
        if (i == 15u) return false;
    }
    for (size_t i = 0; i < PPC_DETOUR_HOOK_WORDS && i < count; i++) d->original[i] = source[i];
    for (size_t i = 0; i < count; i++) {
        const size_t n = ppc_copy_instruction(tramp + used, source + i,
                                              source_addr + (uint32_t)(4u * i));
        used += n;
        if (used > tramp_cap) return false;
    }
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
