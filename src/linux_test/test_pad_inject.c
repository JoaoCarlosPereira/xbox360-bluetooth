#include "../hid/ds4_bt.h"
#include "../hid/x360_pad_inject.h"
#include "../manager/bt_phase1.h"
#include "../plugin/ppc_detour.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Quatro slots XInput simulados; o oficial, quando existe, e preenchido com um padrao. */
typedef struct {
    x360_gamepad_state_t slots[4];
    int                  writes[4];
    uint8_t              report[DS4_BT_OUTPUT_REPORT_LEN];
    size_t               report_len;
    int                  reports;
} env_t;

static const x360_gamepad_state_t OFFICIAL = {
    .buttons = 0x1234u, .left_trigger = 7u, .right_trigger = 9u,
    .thumb_lx = 111, .thumb_ly = -222, .thumb_rx = 333, .thumb_ry = -444,
};

static void write_slot(void *ctx, uint8_t user_index, const x360_gamepad_state_t *state) {
    env_t *e = (env_t *)ctx;
    assert(user_index < 4u);
    e->slots[user_index] = *state;
    e->writes[user_index]++;
}

static void send_report(void *ctx, const uint8_t *report, size_t len) {
    env_t *e = (env_t *)ctx;
    assert(len == DS4_BT_OUTPUT_REPORT_LEN);
    memcpy(e->report, report, len);
    e->report_len = len;
    e->reports++;
}

static void env_init(env_t *e, x360_pad_inject_t *p, bool official) {
    memset(e, 0, sizeof *e);
    if (official) e->slots[0] = OFFICIAL;
    const x360_pad_sink_t sink = { .ctx = e, .write_slot = write_slot, .send_report = send_report };
    x360_pad_inject_init(p, &sink);
}

static const x360_gamepad_state_t DS4_STATE = {
    .buttons = X360_BTN_A | X360_BTN_DPAD_UP, .left_trigger = 200u, .right_trigger = 10u,
    .thumb_lx = 1000, .thumb_ly = -2000, .thumb_rx = 3000, .thumb_ry = -4000,
};

static bool same(const x360_gamepad_state_t *a, const x360_gamepad_state_t *b) {
    return memcmp(a, b, sizeof *a) == 0;
}

static void test_pad_01_no_pairing_no_write(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);
    const env_t before = e;

    assert(!x360_pad_inject_publish(&p, false, 1u, &DS4_STATE, false)); /* nao autenticado */
    assert(!x360_pad_inject_publish(&p, true, 0u, &DS4_STATE, false));  /* sem jogador decidido */
    assert(!x360_pad_inject_publish(&p, true, 5u, &DS4_STATE, false));  /* fora de 1..4 */
    assert(memcmp(&e, &before, sizeof e) == 0);
    assert(!p.active);
    printf("[PASS] PAD-01 sem pareamento publish nao altera nenhum slot\n");
}

static void test_pad_02_official_on_1_goes_to_2(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);

    /* O jogador vem de bt_phase1_decide: oficial no 1 => DS4 no 2. */
    const bt_phase1_context_t ctx = {
        .event = BT_PHASE1_EV_PAIR_ATTEMPT, .official_on_player1 = true,
    };
    const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.player == 2u);

    assert(x360_pad_inject_publish(&p, true, d.player, &DS4_STATE, false));
    assert(same(&e.slots[1], &DS4_STATE));
    assert(e.writes[1] == 1);
    assert(same(&e.slots[0], &OFFICIAL)); /* oficial intocado */
    assert(e.writes[0] == 0);
    assert(e.writes[2] == 0 && e.writes[3] == 0);
    printf("[PASS] PAD-02 oficial no 1: estado cai no slot 2 e o slot 1 fica intocado\n");
}

static void test_pad_03_no_official_goes_to_1(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, false);

    const bt_phase1_context_t ctx = {
        .event = BT_PHASE1_EV_PAIR_ATTEMPT, .official_on_player1 = false,
    };
    const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.player == 1u);

    assert(x360_pad_inject_publish(&p, true, d.player, &DS4_STATE, false));
    assert(same(&e.slots[0], &DS4_STATE));
    assert(e.writes[0] == 1);
    assert(e.writes[1] == 0 && e.writes[2] == 0 && e.writes[3] == 0);
    printf("[PASS] PAD-03 sem oficial: estado cai no slot 1\n");
}

static void test_pad_04_ps_sets_only_guide(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);
    const x360_gamepad_state_t idle = { 0 };

    assert(x360_pad_inject_publish(&p, true, 2u, &idle, true));
    assert(e.slots[1].buttons == 0x0400u);
    assert(e.slots[1].left_trigger == 0u && e.slots[1].right_trigger == 0u);
    assert(e.slots[1].thumb_lx == 0 && e.slots[1].thumb_ly == 0);
    assert(e.slots[1].thumb_rx == 0 && e.slots[1].thumb_ry == 0);
    assert(same(&e.slots[0], &OFFICIAL) && e.writes[0] == 0); /* oficial sem Guide */

    /* PS com outros botoes: so soma o bit 0x0400. */
    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, true));
    assert(e.slots[1].buttons == (uint16_t)(0x1000u | 0x0001u | 0x0400u));
    /* PS solto: o bit sai. */
    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, false));
    assert(e.slots[1].buttons == 0x1001u);
    printf("[PASS] PAD-04 PS liga so o bit 0x0400 no slot do DS4\n");
}

static void test_pad_05_drop_clears_only_ds4_slot(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);

    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, true));
    x360_pad_inject_drop(&p);
    const x360_gamepad_state_t zero = { 0 };
    assert(same(&e.slots[1], &zero));
    assert(e.writes[1] == 2);
    assert(same(&e.slots[0], &OFFICIAL) && e.writes[0] == 0);
    assert(e.writes[2] == 0 && e.writes[3] == 0);
    assert(!p.active);

    /* Segunda queda ou queda sem nunca ter publicado nao escreve nada. */
    x360_pad_inject_drop(&p);
    assert(e.writes[1] == 2 && e.writes[0] == 0);
    env_t e2; x360_pad_inject_t p2;
    env_init(&e2, &p2, true);
    x360_pad_inject_drop(&p2);
    assert(e2.writes[0] == 0 && e2.writes[1] == 0);
    printf("[PASS] PAD-05 queda zera so o slot do DS4\n");
}

static void test_pad_06_player_change_releases_old_slot(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, false);

    assert(x360_pad_inject_publish(&p, true, 1u, &DS4_STATE, false));
    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, false));
    const x360_gamepad_state_t zero = { 0 };
    assert(same(&e.slots[0], &zero)); /* slot antigo liberado */
    assert(same(&e.slots[1], &DS4_STATE));
    printf("[PASS] PAD-06 troca de jogador libera o slot antigo\n");
}

static void test_pad_07_rumble_bytes(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);

    /* Sem DS4 ativo nada e enviado. */
    assert(x360_pad_inject_rumble(&p, 10u, 200u) == 0u);
    assert(e.reports == 0);

    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, false));

    /* Mesmos bytes da tarefa 04 (RUM-01): 0x52 0x11 ... flags 0x07, pequeno em 7, grande em 8. */
    assert(x360_pad_inject_rumble(&p, 10u, 200u) == 79u);
    assert(e.reports == 1 && e.report_len == 79u);
    assert(e.report[0] == 0x52u && e.report[1] == 0x11u && e.report[4] == 0x07u);
    assert(e.report[7] == 10u && e.report[8] == 200u);
    uint8_t ref[DS4_BT_OUTPUT_REPORT_LEN];
    assert(ds4_bt_build_rumble_report(ref, sizeof ref, 10u, 200u) == 79u);
    assert(memcmp(ref, e.report, sizeof ref) == 0);

    memset(e.report, 0xEE, sizeof e.report);
    assert(x360_pad_inject_rumble(&p, 0u, 0u) == 79u);
    assert(e.reports == 2);
    assert(e.report[0] == 0x52u && e.report[1] == 0x11u && e.report[4] == 0x07u);
    assert(e.report[7] == 0u && e.report[8] == 0u);
    assert(ds4_bt_build_rumble_report(ref, sizeof ref, 0u, 0u) == 79u);
    assert(memcmp(ref, e.report, sizeof ref) == 0);

    /* Depois da queda nao ha para quem enviar. */
    x360_pad_inject_drop(&p);
    assert(x360_pad_inject_rumble(&p, 10u, 200u) == 0u);
    assert(e.reports == 2);
    printf("[PASS] PAD-07 vibracao 200/10 e 0/0 batem com os bytes da tarefa 04\n");
}

static void test_pad_08_null_safety(void) {
    x360_pad_inject_init(NULL, NULL);
    x360_pad_inject_drop(NULL);
    assert(!x360_pad_inject_publish(NULL, true, 1u, &DS4_STATE, false));
    assert(x360_pad_inject_rumble(NULL, 1u, 1u) == 0u);

    x360_pad_inject_t p;
    x360_pad_inject_init(&p, NULL); /* sink vazio */
    assert(!x360_pad_inject_publish(&p, true, 1u, &DS4_STATE, false));
    assert(!x360_pad_inject_publish(&p, true, 1u, NULL, false));
    assert(x360_pad_inject_rumble(&p, 1u, 1u) == 0u);
    printf("[PASS] PAD-08 ponteiros nulos e sink vazio nao escrevem\n");
}

/* ---- Ganchos do XamInput (regras puras; o original ja foi chamado) ---------------------- */

#define NOT_CONNECTED 0x48Fu

static void test_pad_09_capabilities_filter(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);
    x360_pad_caps_t caps;
    assert(sizeof(x360_pad_caps_t) == 20u);

    /* Sem DS4 ativo: resultado do original, caps intocado. */
    memset(&caps, 0xEE, sizeof caps);
    assert(x360_pad_inject_filter_capabilities(&p, 1u, NOT_CONNECTED, &caps) == NOT_CONNECTED);
    assert(caps.type == 0xEEu);

    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, true)); /* DS4 no jogador 2 = user 1 */

    /* user 1 sem controle no original: responde como gamepad. */
    assert(x360_pad_inject_filter_capabilities(&p, 1u, NOT_CONNECTED, &caps) == 0u);
    assert(caps.type == 1u && caps.subtype == 1u && caps.flags == 0u);
    assert(caps.gamepad.buttons == (uint16_t)(0x1000u | 0x0001u | 0x0400u));
    assert(caps.vibration_left == 0u && caps.vibration_right == 0u);

    /* user 0 (oficial) e user 2: fica o resultado do original, nunca tocado. */
    memset(&caps, 0xEE, sizeof caps);
    assert(x360_pad_inject_filter_capabilities(&p, 0u, 0u, &caps) == 0u);
    assert(x360_pad_inject_filter_capabilities(&p, 2u, NOT_CONNECTED, &caps) == NOT_CONNECTED);
    assert(caps.type == 0xEEu);
    /* O original achou controle no user do DS4: vale o original. */
    assert(x360_pad_inject_filter_capabilities(&p, 1u, 0u, &caps) == 0u);
    assert(caps.type == 0xEEu);
    /* Qualquer outro erro do original tambem fica. */
    assert(x360_pad_inject_filter_capabilities(&p, 1u, 87u, &caps) == 87u);
    assert(caps.type == 0xEEu);
    assert(e.writes[0] == 0); /* nenhum gancho escreve no slot do oficial */

    /* 0xFF (usuario "qualquer") vira 0, como no hiddriver: o DS4 no jogador 1 responde. */
    env_t e2; x360_pad_inject_t p2;
    env_init(&e2, &p2, false);
    assert(x360_pad_inject_publish(&p2, true, 1u, &DS4_STATE, false));
    assert(x360_pad_inject_filter_capabilities(&p2, 0xFFu, NOT_CONNECTED, &caps) == 0u);

    x360_pad_inject_drop(&p);
    memset(&caps, 0xEE, sizeof caps);
    assert(x360_pad_inject_filter_capabilities(&p, 1u, NOT_CONNECTED, &caps) == NOT_CONNECTED);
    assert(caps.type == 0xEEu);
    printf("[PASS] PAD-09 capabilities: DS4 ativo responde como gamepad so no seu user\n");
}

static void test_pad_10_set_state_filter(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);

    /* Sem DS4 ativo: nada e entregue. */
    assert(x360_pad_inject_filter_set_state(&p, 1u, NOT_CONNECTED, 0xC800u, 0x0A00u) == NOT_CONNECTED);
    assert(e.reports == 0);

    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, false));

    /* esquerdo 0xC8xx = motor grande 200, direito 0x0Axx = pequeno 10. */
    assert(x360_pad_inject_filter_set_state(&p, 1u, NOT_CONNECTED, 0xC8FFu, 0x0A12u) == 0u);
    assert(e.reports == 1 && e.report_len == 79u);
    assert(e.report[0] == 0x52u && e.report[1] == 0x11u && e.report[4] == 0x07u);
    assert(e.report[7] == 10u && e.report[8] == 200u);

    /* Parar: 0/0 gera o quadro com os dois motores em 0. */
    assert(x360_pad_inject_filter_set_state(&p, 1u, NOT_CONNECTED, 0u, 0u) == 0u);
    assert(e.reports == 2 && e.report[7] == 0u && e.report[8] == 0u);

    /* Outro user, ou original com controle: nada e entregue. */
    assert(x360_pad_inject_filter_set_state(&p, 0u, 0u, 0xFFFFu, 0xFFFFu) == 0u);
    assert(x360_pad_inject_filter_set_state(&p, 0u, NOT_CONNECTED, 0xFFFFu, 0xFFFFu) == NOT_CONNECTED);
    assert(x360_pad_inject_filter_set_state(&p, 1u, 0u, 0xFFFFu, 0xFFFFu) == 0u);
    assert(e.reports == 2);
    printf("[PASS] PAD-10 set_state: vibracao so no user do DS4 e so se o original nao achou controle\n");
}

static void test_pad_11_read_state_filter(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);
    x360_gamepad_state_t gp;
    uint32_t packet = 77u;
    memset(&gp, 0xEE, sizeof gp);

    /* DS4 inativo: devolve o original, ponteiros intocados. */
    assert(x360_pad_inject_filter_read_state(&p, true, 0x57u, &packet, &gp) == 0x57u);
    assert(packet == 77u && gp.buttons == 0xEEEEu);

    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, true));
    /* Contexto que nao e o do DS4: original. */
    assert(x360_pad_inject_filter_read_state(&p, false, 0x57u, &packet, &gp) == 0x57u);
    assert(packet == 77u);

    assert(x360_pad_inject_filter_read_state(&p, true, 0x57u, &packet, &gp) == 0u);
    assert(gp.buttons == (uint16_t)(0x1000u | 0x0001u | 0x0400u)); /* com Guide */
    assert(gp.left_trigger == 200u && gp.right_trigger == 10u);
    assert(gp.thumb_lx == 1000 && gp.thumb_ly == -2000 && gp.thumb_rx == 3000 && gp.thumb_ry == -4000);
    assert(packet == 1u);

    /* Novo publish: o pacote sobe e o estado acompanha; PS solto tira o Guide. */
    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, false));
    assert(x360_pad_inject_filter_read_state(&p, true, 0x57u, &packet, &gp) == 0u);
    assert(packet == 2u && gp.buttons == 0x1001u);

    /* Sem packet_number ainda funciona; a queda volta ao original. */
    assert(x360_pad_inject_filter_read_state(&p, true, 0x57u, NULL, &gp) == 0u);
    x360_pad_inject_drop(&p);
    assert(x360_pad_inject_filter_read_state(&p, true, 0x57u, &packet, &gp) == 0x57u);
    assert(same(&e.slots[0], &OFFICIAL) && e.writes[0] == 0);
    printf("[PASS] PAD-11 read_state: ultimo estado publicado, com Guide, so para o DS4 ativo\n");
}

static void test_pad_12_bind_verdict_and_context(void) {
    /* Valores copiados de main.cpp:498-501 / 1204 para o indice 0. */
    assert(x360_pad_controller_id(0u) == 0xA7553952u);
    assert(x360_pad_device_context(0u) == 0x10000005u);
    assert(X360_PAD_USER_INDEX_NONE == 0xFFu);

    /* Oficial no 1: DS4 e o jogador 2 (user 1). */
    assert(x360_pad_inject_check_bind(2u, 1u) == X360_PAD_BIND_OK);
    assert(x360_pad_inject_check_bind(2u, 0u) == X360_PAD_BIND_OFFICIAL_SLOT);
    assert(x360_pad_inject_check_bind(2u, 0xFFu) == X360_PAD_BIND_MISMATCH);
    assert(x360_pad_inject_check_bind(2u, 2u) == X360_PAD_BIND_MISMATCH);
    /* Sem oficial: DS4 e o jogador 1 (user 0); outro user nao serve. */
    assert(x360_pad_inject_check_bind(1u, 0u) == X360_PAD_BIND_OK);
    assert(x360_pad_inject_check_bind(1u, 1u) == X360_PAD_BIND_MISMATCH);
    assert(x360_pad_inject_check_bind(1u, 0xFFu) == X360_PAD_BIND_MISMATCH);
    /* Sem jogador decidido nada confere. */
    assert(x360_pad_inject_check_bind(0u, 0u) == X360_PAD_BIND_MISMATCH);
    assert(x360_pad_inject_check_bind(3u, 2u) == X360_PAD_BIND_OK);
    assert(x360_pad_inject_check_bind(4u, 3u) == X360_PAD_BIND_OK);
    assert(x360_pad_inject_check_bind(3u, 0u) == X360_PAD_BIND_OFFICIAL_SLOT);
    assert(x360_pad_inject_check_bind(5u, 4u) == X360_PAD_BIND_MISMATCH);
    printf("[PASS] PAD-12 alta: so o userIndex do jogador decidido vale; o do oficial e recusado\n");
}

static void test_pad_13_context_rule(void) {
    env_t e; x360_pad_inject_t p;
    env_init(&e, &p, true);
    const uint32_t ds4_ctx = x360_pad_device_context(0u);
    const uint32_t other_ctx = 0x10000001u;
    uint32_t packet = 5u;
    x360_gamepad_state_t gp;

    /* Alta recusada (userIndex do oficial): nada publicado, o contexto nunca vira o do DS4. */
    assert(x360_pad_inject_check_bind(2u, 0u) == X360_PAD_BIND_OFFICIAL_SLOT);
    memset(&gp, 0xEE, sizeof gp);
    assert(x360_pad_inject_filter_read_state(&p, false, 0x57u, &packet, &gp) == 0x57u);
    assert(packet == 5u && gp.buttons == 0xEEEEu);
    assert(same(&e.slots[0], &OFFICIAL) && e.writes[0] == 0 && e.writes[1] == 0);

    /* Alta aceita (user 1) e DS4 publicado com PS: o contexto do DS4 recebe o estado. */
    assert(x360_pad_inject_check_bind(2u, 1u) == X360_PAD_BIND_OK);
    assert(x360_pad_inject_publish(&p, true, 2u, &DS4_STATE, true));
    uint32_t configured = ds4_ctx; /* o que o plugin guarda em g_pad_device_context */
    memset(&gp, 0xEE, sizeof gp);
    assert(x360_pad_inject_filter_read_state(&p, configured == ds4_ctx, 0x57u, &packet, &gp) == 0u);
    assert(gp.buttons == (uint16_t)(0x1000u | 0x0001u | 0x0400u));
    assert(packet == 1u);

    /* Outro contexto: o resultado e o buffer do original ficam como estavam. */
    packet = 99u;
    memset(&gp, 0xEE, sizeof gp);
    assert(x360_pad_inject_filter_read_state(&p, other_ctx == ds4_ctx, 0x57u, &packet, &gp) == 0x57u);
    assert(packet == 99u && gp.buttons == 0xEEEEu && gp.thumb_ry == (int16_t)0xEEEE);

    /* Baixa: contexto zerado no plugin (nunca "e o do DS4"), slot do DS4 zerado, oficial intocado. */
    configured = 0u;
    x360_pad_inject_drop(&p);
    assert(x360_pad_inject_filter_read_state(&p, configured != 0u && configured == ds4_ctx, 0x57u,
                                             &packet, &gp) == 0x57u);
    const x360_gamepad_state_t zero = { 0 };
    assert(same(&e.slots[1], &zero));
    assert(same(&e.slots[0], &OFFICIAL) && e.writes[0] == 0);
    printf("[PASS] PAD-13 contexto do DS4 recebe o estado; outro contexto e o oficial nao sao tocados\n");
}

/* ---- Desvio PowerPC (porte de Detours.h) ------------------------------------------------ */

static void test_ppc_01_far_branch_encoding(void) {
    uint32_t w[6];
    /* lis r0,0x9144 / ori r0,r0,0x1234 / mtctr r0 / bctr */
    assert(ppc_write_far_branch(w, 0x91441234u, false, false, 20u, 0u, 0u) == 4u);
    assert(w[0] == 0x3C009144u && w[1] == 0x60001234u && w[2] == 0x7C0903A6u && w[3] == 0x4E800420u);
    /* Com preserve: std r0,-0x30(r1) ... ld r0,-0x30(r1) antes do bctrl. */
    assert(ppc_write_far_branch(w, 0x80130530u, true, true, 20u, 0u, 0u) == 6u);
    assert(w[0] == 0xF801FFD0u && w[1] == 0x3C008013u && w[2] == 0x60000530u);
    assert(w[3] == 0x7C0903A6u && w[4] == 0xE801FFD0u && w[5] == 0x4E800421u);
    /* Tamanho sem destino. */
    assert(ppc_write_far_branch(NULL, 0u, false, false, 20u, 0u, 0u) == 4u);
    assert(ppc_write_far_branch(NULL, 0u, false, true, 20u, 0u, 0u) == 6u);
    printf("[PASS] PPC-01 lis/ori/mtctr/bcctr com os mesmos opcodes do Detours.h\n");
}

static void test_ppc_02_install_plain_function(void) {
    uint32_t fn[8] = { 0x7D8802A6u, 0x9421FFC0u, 0x7C7B1B78u, 0x7C9C2378u, 0xAAAAAAAAu, 0xBBBBBBBBu, 0, 0 };
    uint32_t tramp[PPC_DETOUR_TRAMPOLINE_WORDS];
    ppc_detour_t d;
    size_t used = 0;
    memset(&d, 0, sizeof d);
    memset(tramp, 0, sizeof tramp);

    assert(ppc_detour_install(&d, fn, 0x80130530u, 0x914412A0u, tramp, 0x91450000u,
                              PPC_DETOUR_TRAMPOLINE_WORDS, &used, NULL));
    /* A funcao agora salta para o gancho e o resto fica intocado. */
    assert(fn[0] == 0x3C009144u && fn[1] == 0x600012A0u && fn[2] == 0x7C0903A6u && fn[3] == 0x4E800420u);
    assert(fn[4] == 0xAAAAAAAAu && fn[5] == 0xBBBBBBBBu);
    assert(d.original[0] == 0x7D8802A6u && d.original[3] == 0x7C9C2378u);
    /* Trampolim: as 4 instrucoes originais e o retorno para fn + 16 (0x80130540). */
    assert(used == 4u + 6u);
    assert(tramp[0] == 0x7D8802A6u && tramp[1] == 0x9421FFC0u);
    assert(tramp[2] == 0x7C7B1B78u && tramp[3] == 0x7C9C2378u);
    assert(tramp[4] == 0xF801FFD0u && tramp[5] == 0x3C008013u && tramp[6] == 0x60000540u);
    assert(tramp[7] == 0x7C0903A6u && tramp[8] == 0xE801FFD0u && tramp[9] == 0x4E800420u);
    assert(d.trampoline_addr == 0x91450000u && d.installed);
    /* Segunda instalacao no mesmo desvio e recusada. */
    assert(!ppc_detour_install(&d, fn, 0x80130530u, 0x914412A0u, tramp, 0x91450000u,
                               PPC_DETOUR_TRAMPOLINE_WORDS, &used, NULL));
    printf("[PASS] PPC-02 instalacao grava o desvio e monta o trampolim de retorno\n");
}

static void test_ppc_03_relocates_relative_branches(void) {
    /* [0] normal  [1] bl +0x100  [2] bne +0x10  [3] b -8 */
    uint32_t fn[4] = { 0x7C0802A6u, 0x48000101u, 0x40820010u, 0x4BFFFFF8u };
    uint32_t tramp[PPC_DETOUR_TRAMPOLINE_WORDS];
    ppc_detour_t d;
    size_t used = 0;
    memset(&d, 0, sizeof d);
    memset(tramp, 0, sizeof tramp);
    const uint32_t base = 0x80130530u;

    assert(ppc_detour_install(&d, fn, base, 0x91441000u, tramp, 0x91450000u,
                              PPC_DETOUR_TRAMPOLINE_WORDS, &used, NULL));
    assert(used == 1u + 6u + 6u + 6u + 6u);
    assert(tramp[0] == 0x7C0802A6u);
    /* bl base+4+0x100 = 0x80130634 -> far branch ligado (bctrl). */
    assert(tramp[1] == 0xF801FFD0u && tramp[2] == 0x3C008013u && tramp[3] == 0x60000634u);
    assert(tramp[4] == 0x7C0903A6u && tramp[5] == 0xE801FFD0u && tramp[6] == 0x4E800421u);
    /* bne base+8+0x10 = 0x80130548 -> bcctr 4,2 (bnectr), sem ligar. */
    assert(tramp[7] == 0xF801FFD0u && tramp[8] == 0x3C008013u && tramp[9] == 0x60000548u);
    assert(tramp[10] == 0x7C0903A6u && tramp[11] == 0xE801FFD0u && tramp[12] == 0x4C820420u);
    /* b base+12-8 = 0x80130534 -> bctr. */
    assert(tramp[13] == 0xF801FFD0u && tramp[14] == 0x3C008013u && tramp[15] == 0x60000534u);
    assert(tramp[16] == 0x7C0903A6u && tramp[17] == 0xE801FFD0u && tramp[18] == 0x4E800420u);
    /* Retorno para base + 16 = 0x80130540. */
    assert(tramp[21] == 0x60000540u);
    printf("[PASS] PPC-03 b, bl e bc relativos sao realocados para saltos absolutos\n");
}

static void test_guide_keystroke(void) {
    x360_key_tracker_t tracker;
    x360_keystroke_t key;
    x360_gamepad_state_t state;
    memset(&tracker, 0, sizeof tracker);
    memset(&state, 0, sizeof state);

    state.buttons = X360_BTN_A;
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 1000u, &key) == X360_PAD_ERROR_SUCCESS);
    assert(key.virtual_key == 0x5800u && key.flags == X360_KEY_DOWN && key.user_index == 0u);
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 1100u, &key) == X360_PAD_ERROR_EMPTY);

    state.buttons = 0u;
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 1200u, &key) == X360_PAD_ERROR_SUCCESS);
    assert(key.virtual_key == 0x5800u && key.flags == X360_KEY_UP);

    memset(&state, 0, sizeof state);
    state.thumb_ly = 25000;
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 1300u, &key) == X360_PAD_ERROR_SUCCESS);
    assert(key.virtual_key == 0x5810u && (key.flags & X360_KEY_DOWN) != 0u);
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 1700u, &key) == X360_PAD_ERROR_EMPTY);
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 1760u, &key) == X360_PAD_ERROR_SUCCESS);
    assert(key.virtual_key == 0x5810u && (key.flags & X360_KEY_REPEAT) != 0u);

    state.buttons = X360_BTN_GUIDE;
    state.thumb_ly = 0;
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 2000u, &key) == X360_PAD_ERROR_SUCCESS);
    assert(key.flags == X360_KEY_UP);
    assert(x360_pad_keystroke_next(&tracker, &state, 0u, 2010u, &key) == X360_PAD_ERROR_EMPTY);
    printf("[PASS] painel: A, direcional do analogico e o botao central sem tecla\n");
}

static void test_ppc_04_install_guards(void) {
    uint32_t fn[4] = { 1, 2, 3, 4 };
    uint32_t tramp[PPC_DETOUR_TRAMPOLINE_WORDS];
    ppc_detour_t d;
    memset(&d, 0, sizeof d);
    assert(!ppc_detour_install(NULL, fn, 0u, 0u, tramp, 0u, PPC_DETOUR_TRAMPOLINE_WORDS, NULL, NULL));
    assert(!ppc_detour_install(&d, NULL, 0u, 0u, tramp, 0u, PPC_DETOUR_TRAMPOLINE_WORDS, NULL, NULL));
    assert(!ppc_detour_install(&d, fn, 0u, 0u, NULL, 0u, PPC_DETOUR_TRAMPOLINE_WORDS, NULL, NULL));
    assert(!ppc_detour_install(&d, fn, 0u, 0u, tramp, 0u, PPC_DETOUR_TRAMPOLINE_WORDS - 1u, NULL, NULL));
    assert(fn[0] == 1u && fn[3] == 4u && !d.installed); /* nada escrito em caso de recusa */
    printf("[PASS] PPC-04 argumentos invalidos ou trampolim pequeno nao tocam a funcao\n");
}

int main(void) {
    test_pad_01_no_pairing_no_write();
    test_pad_02_official_on_1_goes_to_2();
    test_pad_03_no_official_goes_to_1();
    test_pad_04_ps_sets_only_guide();
    test_pad_05_drop_clears_only_ds4_slot();
    test_pad_06_player_change_releases_old_slot();
    test_pad_07_rumble_bytes();
    test_pad_08_null_safety();
    test_pad_09_capabilities_filter();
    test_pad_10_set_state_filter();
    test_pad_11_read_state_filter();
    test_pad_12_bind_verdict_and_context();
    test_pad_13_context_rule();
    test_guide_keystroke();
    test_ppc_01_far_branch_encoding();
    test_ppc_02_install_plain_function();
    test_ppc_03_relocates_relative_branches();
    test_ppc_04_install_guards();
    return 0;
}
