#ifndef BT360_PPC_DETOUR_H
#define BT360_PPC_DETOUR_H

/*
 * Desvio de funcao PowerPC, portado em C do minimo de
 * research/repos/hiddriver360/hiddriver/Detours.h (WriteFarBranchEx, RelocateBranch,
 * CopyInstruction, Install): lis / ori / mtctr / bcctr sobre r0.
 *
 * Os enderecos entram como uint32_t explicito, separados dos ponteiros, para o mesmo
 * codigo rodar no host (src/linux_test/test_pad_inject.c) e no console. Nada aqui
 * conhece o alvo: quem chama resolve o ponteiro (XexGetProcedureAddress) e o entrega.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* O desvio ocupa 4 instrucoes: lis, ori, mtctr, bcctr. */
#define PPC_DETOUR_HOOK_WORDS 4u
/* Pior caso do trampolim: 4 instrucoes realocadas (6 palavras cada) + retorno que
 * preserva o CTR (10). O retorno antigo usava o CTR e derrubava funcoes curtas do sistema. */
#define PPC_DETOUR_TRAMPOLINE_WORDS 34u

#define PPC_BRANCH_OPTIONS_ALWAYS 20u

/* Esvazia o cache de instrucoes do trecho (dcbst/sync/icbi/sync/isync); NULL no host. */
typedef void (*ppc_flush_fn)(const void *start, size_t bytes);

typedef struct {
    uint32_t     *source;
    uint32_t      original[PPC_DETOUR_HOOK_WORDS];
    uint32_t     *trampoline;
    uint32_t      trampoline_addr;
    bool          installed;
} ppc_detour_t;

/*
 * Escreve em dst o desvio longo para target. reg e o registrador usado (0 no hiddriver).
 * preserve guarda/restaura reg em -0x30(r1) em volta do salto. Retorna o numero de palavras.
 */
size_t ppc_write_far_branch(uint32_t *dst, uint32_t target, bool linked, bool preserve,
                            uint32_t branch_options, uint8_t cr_bit, uint8_t reg);

/* Copia uma instrucao do inicio da funcao para o trampolim, corrigindo b/bc relativos. */
size_t ppc_copy_instruction(uint32_t *dst, const uint32_t *src, uint32_t src_addr);

/*
 * Instala o desvio em source (endereco de execucao source_addr) para target_addr.
 * O trampolim (tramp, executavel em tramp_addr, tramp_cap palavras livres) recebe as 4
 * primeiras instrucoes da funcao e o retorno para source_addr + 16; tramp_addr passa a ser
 * o "original" chamavel. O retorno usa LR e devolve o CTR, entao a funcao so e desviada se
 * uma das 4 instrucoes for mflr (o LR original ja esta num registrador). Primeiro o trampolim
 * e esvaziado do cache, depois o desvio e escrito.
 * Retorna false se ja instalado, se os argumentos forem invalidos, se faltar espaco ou se
 * a funcao nao guardar o LR nessas 4 instrucoes.
 * *tramp_used recebe as palavras consumidas.
 */
bool ppc_detour_install(ppc_detour_t *d, uint32_t *source, uint32_t source_addr,
                        uint32_t target_addr, uint32_t *tramp, uint32_t tramp_addr,
                        size_t tramp_cap, size_t *tramp_used, ppc_flush_fn flush);

#ifdef __cplusplus
}
#endif

#endif /* BT360_PPC_DETOUR_H */
