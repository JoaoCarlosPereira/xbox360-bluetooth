# Xbox 360 Generic USB Bluetooth (P&D)

Projeto experimental de pesquisa e desenvolvimento para adicionar suporte genérico a adaptadores **Bluetooth USB (HCI)** diretamente no **Xbox 360** (alvo inicial: Xbox 360 E com desbloqueio **ABadAvatar** + **XeUnshackle** + **Aurora**).

> **Estado em 26/11/2026:** o protótipo roda no Xbox 360 E. O plugin `bt360_proto.xex` sobe com o DashLaunch, o UGREEN `2B89:8761` (RTL8761BU) fica pronto e até dois DualShock 4 conectam ao mesmo tempo. O controle original permanece no jogador 1. O código está em `main`, commit `7784beb`. A tela nativa de perfil ainda precisa ser confirmada no próximo boot: ela pede teclas (`XamInputGetKeystroke`, ordinal 403), e esse caminho acabou de ir para o HD.

---

## Resposta à Pergunta Central

> **É possível, no ambiente de desbloqueio ABadAvatar do Xbox 360, desenvolver uma camada genérica capaz de acessar adaptadores Bluetooth USB e trocar comandos HCI com eles?**

**Sim — é tecnicamente viável**, com uma ressalva importante de engenharia reversa pontual para a etapa de *descoberta/bind* do dispositivo no kernel.

### Evidências principais
1. **Acesso completo ao barramento USB via kernel (`xboxkrnl.exe`):** O kernel retail `2.0.17559.0` exporta nativamente a pilha `Usbd*` (ordinais 740–759, 892, 895), incluindo `UsbdOpenDefaultEndpoint`, `UsbdOpenEndpoint` (com suporte a Control `0x00`, Isochronous `0x01`, Bulk `0x02` e Interrupt `0x03`), `UsbdQueueAsyncTransfer`, `UsbdGetDeviceDescriptor`, `UsbdGetEndpointDescriptor`, `UsbdAddDeviceComplete` e `UsbdRegisterDriverObject`. O projeto [`hiddriver360`](research/repos/hiddriver360/hiddriver/main.cpp) comprova em código C++ funcional no retail `17559` que plugins `.xex` conseguem enumerar dispositivos USB, abrir endpoints e executar transferências assíncronas Control e Interrupt.
2. **Ambiente ABadAvatar + XeUnshackle suporta plugins `.xex` em runtime:** Diferente de um RGH permanente, o **ABadAvatar** executa em RAM via exploit de hypervisor (compatível inclusive com placas **Winchester** do Xbox 360 E). Quando pareado ao **XeUnshackle**, ele aplica os patches de kernel/HV em memória e carrega o **DashLaunch (`launch.xex`)**, permitindo carregar plugins `.xex` de sistema via `launch.ini` que permanecem residentes na Aurora e durante jogos.
3. **Gap técnico mapeado (não impeditivo):** O `hiddriver360` intercepta `HidAddDevice`, que só é chamado para classe USB HID (`bInterfaceClass == 0x03`). Um dongle Bluetooth HCI padrão usa classe Wireless Controller (`0xE0 / 0x01 / 0x01`) ou Vendor-Specific (`0xFF`). Portanto, o protótipo precisará ou utilizar `UsbdRegisterDriverObject` (@755) — cuja estrutura exata de entrada precisa ser verificada no kernel `17559` — ou hookear o ponto de despacho de dispositivos não reconhecidos / tabela de drivers do `Usbd`.

---

## Estado do Projeto

| Fase | Descrição | Status |
| :--- | :--- | :--- |
| Fase 1 | Pesquisa da pilha USB do Xbox 360 e ambiente ABadAvatar | Concluída |
| Fase 2 | Análise de projetos existentes (`UsbdSecPatch`, `hiddriver360`, `WirelessInput360`, `FreeMyXe`, `XeUnshackle`) | Concluída |
| Fase 3 | Estudo do transporte Bluetooth USB HCI (`btusb`, BlueZ, especificações) | Concluída |
| Fase 4 | Comparação e recomendação de arquitetura | Concluída |
| Fase 5 | Planejamento do primeiro protótipo mínimo (HCI Reset + Read Local Version) | Concluída |
| Fase 6 | Estruturação documental e preparação para primeiro experimento | Concluída |
| Fase 7 | Plugin residente, firmware RTL8761BU, dois DualShock 4 e injeção no XamInput | No console; jogador 1 e telas nativas em confirmação |
| Fase 8 | Telas nativas do sistema (perfil e guia) pelo DualShock, sem tirar o controle original | No HD (`7784beb`); falta o boot que carrega esse xex |

---

## Estrutura do Repositório

- [`docs/feasibility.md`](docs/feasibility.md) — Análise de viabilidade técnica, matriz de evidências e riscos.
- [`docs/xbox360-usb.md`](docs/xbox360-usb.md) — Arquitetura da pilha USB (`Usbd`) do Xbox 360, DashLaunch e ABadAvatar/XeUnshackle.
- [`docs/bluetooth-hci.md`](docs/bluetooth-hci.md) — Transporte Bluetooth USB HCI (endpoints, comandos, eventos, ACL e inicialização).
- [`docs/compatibility.md`](docs/compatibility.md) — Análise de chipsets (Realtek, Broadcom, Intel, Barrot, Actions, CSR) e firmware.
- [`docs/architecture.md`](docs/architecture.md) — Comparação das alternativas arquiteturais (A, B, C, D) e recomendação.
- [`docs/roadmap.md`](docs/roadmap.md) — Plano incremental e desenho do primeiro experimento.
- [`research/`](research/README.md) — Referências técnicas e repositórios analisados.
- [`src/`](src/README.md) — Pilha HCI/USB, sessão de pareamento, HID, injeção no XamInput, plugin e BTManager.
- [`STATUS.md`](STATUS.md) — Handoff do que já foi visto no console e do que ainda falta confirmar.
