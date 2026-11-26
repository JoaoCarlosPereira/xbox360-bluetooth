# Status do Projeto — Xbox 360 Generic USB Bluetooth (`xbox-blutooth`)

> Documento de handoff para continuidade em **PC x86 (Linux/WSL)** e posterior validação no **Xbox 360 E (ABadAvatar + XeUnshackle + Aurora)**.

## Atualização de 24/11/2026

- `make test` usa respostas HCI simuladas e não depende de `hci0`; os perfis
  listados são simulações, não evidência de compatibilidade com cada dongle.
- `make test-live HCI_INDEX=3` seleciona um controlador HCI e requer acesso HCI
  bruto; sem índice, usa `hci0`. Em 24/11/2026 o UGREEN foi identificado como
  `hci3` e respondeu aos comandos de leitura de versão e endereço. O Reset é
  simulado e os demais perfis são descritores simulados sobre o mesmo adaptador.
- O teste de firmware inclui um EPATCH sintético; uma imagem externa pode ser
  escolhida com `RTL_FIRMWARE`. A imagem local foi testada após descompressão:
  30.210 bytes de patch e configuração e 120 fragmentos.
- As referências foram recuperadas em `research/repos/`. O toolchain GCC 15.3
  e as bibliotecas do xsdk foram compilados localmente; o plugin e a UI geraram
  `build/plugin/bt360_proto.xex` e `build/ui/BTManager.xex`.
- O build `BTMANAGER_XENIA=ON` usa `VdSwap` para contornar a inicialização
  do controlador de vídeo que o Xenia não emula. A renderização em 720p e a
  navegação até o slot P2 foram verificadas no Xenia Canary Linux.
- O BTManager tem um painel gráfico em português, com abas de controles,
  dispositivos pareados e busca. O build da interface usa Python 3 com
  Pillow para gerar o atlas a partir das fontes incluídas em `src/ui/assets/`.
  Se necessário, selecione o interpretador com `-DBTMANAGER_PYTHON=/caminho/python3`.
- O gerenciador e o plugin de diagnóstico trocam mensagens por
  `bt360_request.bin` e `bt360_reply.bin` no mesmo USB que recebe o log.
  A resposta informa o resultado da resolução das funções USB do kernel.
  O teste dessa troca no Xbox e a execução dos comandos Bluetooth continuam
  pendentes, assim como a associação USB e a integração com XamInput.

---

## 1. Hardware Alvo Já Identificado e Validado

| Componente | Detalhes Confirmados |
| :--- | :--- |
| **Console** | Xbox 360 E (exploit **ABadAvatar** + **XeUnshackle** + **DashLaunch** em RAM, dashboard `2.0.17559.0`) |
| **Adaptador Bluetooth USB** | **UGREEN** — `VID:PID = 2B89:8761` (**Realtek RTL8761BU**, USB 1.1 Full Speed 12 Mbps, `0xE0/0x01/0x01`) |
| **Endpoints do Dongle** | Control EP0, Interrupt IN `0x81` (MPS=16, interval=1), Bulk OUT `0x02` (MPS=64), Bulk IN `0x82` (MPS=64) |
| **Firmware Realtek** | `rtl8761bu_fw.bin` + `rtl8761bu_config.bin` (`27.814 bytes`, `fw version 0x09a98a6b`; ROM de fábrica responde `HCI_Reset` e `Read_Local_Version` sem firmware externo) |
| **Controles Testados** | Sony **DualShock 4** (`20:29:92:82:2F:67` e `20:29:92:81:82:73`, `CoD=0x002508`) |

---

## 2. Tudo que Já Foi Avançado e Validado

### Documentação e Pesquisa (`docs/` e `research/`)
- [x] Viabilidade técnica completa ([`docs/feasibility.md`](docs/feasibility.md)) — confirmando acesso às APIs `Usbd*` do `xboxkrnl.exe` (`17559`), papel real do `UsbdSecPatch` (não é pilha BT) e suporte a plugins `.xex` no `XeUnshackle`.
- [x] Arquitetura USB do Xbox 360 ([`docs/xbox360-usb.md`](docs/xbox360-usb.md)) — ordinais `740–759`, `UsbTrb`, `UsbControlTrb`, endianness Big-Endian PPC.
- [x] Transporte Bluetooth USB HCI ([`docs/bluetooth-hci.md`](docs/bluetooth-hci.md)) e compatibilidade de chipsets ([`docs/compatibility.md`](docs/compatibility.md)).
- [x] Arquitetura Híbrida recomendada ([`docs/architecture.md`](docs/architecture.md)) — plugin residente + app visual `BTManager.xex` via IPC.
- [x] Repositórios de referência clonados em `research/repos/` (ignorados no `.gitignore`): `UsbdSecPatch`, `hiddriver360`, `WirelessInput360`, `FreeMyXe`, `XeUnshackle`, `xsdk`.

### Código Implementado em `src/` (C11 puro, zero `malloc`, zero FPU/VMX)
- [x] **Core HCI** ([`src/hci/hci_core.h`](src/hci/hci_core.h), [`src/hci/hci_core.c`](src/hci/hci_core.c)):
  - Builders/parsers endian-safe para `HCI_Reset` (`0x0C03`), `Read_Local_Version_Information` (`0x1001`), `Read_BD_ADDR` (`0x1009`), `Set_Event_Mask` (`0x0C01`), `Write_Inquiry_Mode` (`0x0C45`), `Inquiry` (`0x0401`), `Inquiry_Cancel` (`0x0402`), `Create_Connection` (`0x0405`), SSP (`0x0C56`, `0x042B`, `0x042C`), e decodificação de `Extended Inquiry Result` (`0x2F`) com nome EIR.
- [x] **Transporte USB HCI** ([`src/transport/bt_usb_transport.h`](src/transport/bt_usb_transport.h), [`src/transport/bt_usb_transport.c`](src/transport/bt_usb_transport.c)):
  - Filtro `0xE0/0x01/0x01`, máquina de estados de inicialização, remontagem de eventos Interrupt IN fragmentados (8/16 bytes) e varredura deduplicada por `BD_ADDR`.
- [x] **Carregador de Firmware Realtek RTL8761BU** ([`src/chipset/rtl8761b.h`](src/chipset/rtl8761b.h), [`src/chipset/rtl8761b.c`](src/chipset/rtl8761b.c)):
  - Parser zero-alloc do formato EPATCH v1 (`"Realtech"`), seleção por `rom_version` (`0xFC6D`), patch da versão e fragmentação em chunks de 252 bytes (`0xFC20`).
- [x] **L2CAP HID-over-Bluetooth** ([`src/l2cap/l2cap.h`](src/l2cap/l2cap.h), [`src/l2cap/l2cap.c`](src/l2cap/l2cap.c)):
  - Sinalização `CID 0x0001` (`CONN_REQ/RSP`, `CONF_REQ/RSP`, `DISCONN_REQ/RSP`) para **HID Control (`PSM 0x0011`)** e **HID Interrupt (`PSM 0x0013`)**.
- [x] **Parser DualShock 4 + Remapeamento Customizável** ([`src/hid/ds4_bt.h`](src/hid/ds4_bt.h), [`src/hid/ds4_bt.c`](src/hid/ds4_bt.c)):
  - Decodifica relatórios Bluetooth `0xA1 0x01` e `0xA1 0x11` para `x360_gamepad_state_t` (com inversão de eixo Y) e tabela `bt_button_map_t` (15 botões Xbox 360 + inversão LT/RT).
- [x] **Gerenciador Multi-Controle, Link Keys, IPC e Wizard de Remapeamento** ([`src/manager/bt_manager.h`](src/manager/bt_manager.h), [`src/manager/bt_manager.c`](src/manager/bt_manager.c)):
  - Slots P1–P4, persistência de `Link Key` em formato INI (`Usb:\bt_devices.ini`), mailbox IPC (`bt_ipc_state_t`), modelo de navegação UI (`bt_ui_model_t`) e assistente 1-a-1 de remapeamento (`bt_remap_wizard_t`).
- [x] **Interface `BTManager.xex` (NXE / Guide-inspired)** ([`src/ui/bt_manager_app.c`](src/ui/bt_manager_app.c), [`src/ui/CMakeLists.txt`](src/ui/CMakeLists.txt)):
  - UI 720p sobre `xsdk` (`gpu.h`, `present.h`, `text.h`, `XamInputGetState`) com painéis de Slots P1–P4, Pareados, Busca e tela de Mapeamento 1-a-1.
- [x] **Esqueleto HAL + Plugin Xbox 360** ([`src/hal/xbox360_usbd.h`](src/hal/xbox360_usbd.h), [`src/hal/xbox360_usbd.c`](src/hal/xbox360_usbd.c), [`src/plugin/plugin_main.c`](src/plugin/plugin_main.c), [`src/plugin/CMakeLists.txt`](src/plugin/CMakeLists.txt)).

### Validações Reais Já Executadas no Linux (`src/linux_test/`)
- [x] **`make test`** passando 100% (`-Wall -Wextra -Werror -O2 -std=c11`):
  - `test_usb_transport` (perfis `2B89:8761`, `0A12:0001`, `33FA:0010`, `0BDA:8771`).
  - `test_l2cap_ds4` (handshake L2CAP + parser DS4 `0x11` + parser EPATCH `27814 bytes / 111 chunks`).
  - `test_manager_ui` (roundtrip `Link Key` INI, slots P1–P4, navegação/IPC e wizard de remapeamento 1-a-1).
- [x] **`usbdevfs_probe`**: comunicação USB crua direta com o dongle `2B89:8761` (EP0 Control + EP1 Interrupt IN + `HCI_Inquiry`).
- [x] **`ds4_live`**: pareamento SSP real (`NoInputNoOutput`), criptografia, abertura L2CAP `0x0011`/`0x0013`, `Feature 0x02` e recebimento de pacotes `A1 01` e `A1 11` (79 bytes) do **DualShock 4 real** via USB crua.

---

## 3. Tudo que Falta Fazer (Checklist para o PC x86 e Xbox 360)

### Fase A — Build PowerPC e Teste de UI no PC x86
- [x] **Compilar o toolchain `xsdk-gcc` no PC x86:**
  ```bash
  source build/xsdk-env.sh
  PREFIX="$XSDK_TOOLCHAIN" WORK="$PWD/build/xsdk-gcc-work" JOBS=12 \
    bash research/repos/xsdk/tools/toolchain/build.sh
  make -C research/repos/xsdk TOOLCHAIN="$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-" \
    build/libxsdk.a build/libxsdk_plugin.a
  ```
- [x] **Compilar `BTManager.xex` e `bt360_proto.xex`** usando `cmake -DCMAKE_TOOLCHAIN_FILE=.../xsdk/cmake/xsdk.cmake`.
- [x] **Testar a tela principal no Xenia Canary (PC x86):** validar layout 720p, fontes, cores e navegação D-pad com o build de emulação.
- [ ] **Testar a tela de remapeamento com controle conectado.**

### Fase B — Integração do Plugin Residente (`bt360_plugin.xex`)
- [ ] **Conectar `bt360_plugin.xex` com toda a pilha `src/`:**
  - Adicionar `submit_bulk_in` / `submit_bulk_out` em `xbox360_usbd.c` (`UsbdOpenEndpoint` com `BT_USB_EP_TYPE_BULK = 2`).
  - Conectar `rtl8761b.c` lendo `rtl8761bu_fw.bin` e `rtl8761bu_config.bin` de `Usb:\bt_fw\` via `NtCreateFile`/`NtReadFile` (no Xbox 360 o dongle começa em cold-boot ROM!).
  - Conectar `l2cap.c` + `ds4_bt.c` + `bt_manager.c` em uma thread de sistema (`ExCreateThread` com `EX_CREATE_FLAG_SYSTEM`), mantendo callbacks DPC livres de FPU/VMX.
- [ ] **Completar o IPC entre o plugin e o gerenciador:**
  - Testar o canal de diagnóstico no console, depois publicar o estado do
    dongle e ligar os comandos do gerenciador à pilha USB e Bluetooth.
- [ ] **Persistência de `Link Key` + Reconexão automática:**
  - Salvar `Link_Key_Notification` (`0x18`) em `Usb:\bt_devices.ini` e responder `Link_Key_Request` (`0x17`) com `Link_Key_Request_Reply` (`0x0B` / `0x040B`), além de habilitar `Write_Scan_Enable` (`0x0C1A = 0x03`) para o DS4 reconectar apenas apertando o botão **PS**.
  - Persistir também o `bt_button_map_t` customizado de cada controle no `bt_devices.ini`.
- [ ] **Injeção de controles no `XamInput`:**
  - Portar os hooks de `XamInputGetCapabilitiesEx`, `XamInputSetState` (rumble -> saída DS4 `0x11`!), `XInputdReadState` e `XamUserBindDeviceCallback` (usando `research/repos/hiddriver360/hiddriver/main.cpp` como referência exata para retail `17559`).

### Fase C — Validação no Xbox 360 E Real (ABadAvatar + XeUnshackle + Aurora)

Após carregar o plugin de diagnóstico, abra o `BTManager.xex` de `build/ui/`
com o mesmo pendrive conectado. A mensagem esperada é
`Plugin ativo. Aguardando vínculo USB.`. Se ela não aparecer,
confira `bt360_diag.log` e se `bt360_request.bin` e `bt360_reply.bin` foram
criados no pendrive.

Para analisar falhas, guarde os dois logs gerados pelos XEX mais recentes:

- `bt360_diag.log` na raiz do pendrive de diagnóstico. Registra a versão do
  kernel, o resultado de cada busca de função USB, a presença do ordinal 755
  (consulta apenas), os comandos recebidos e falhas ao responder.
- `BTManager.log` ao lado do aplicativo quando o local permite escrita, ou
  em um dos locais alternativos do xsdk. Registra a inicialização gráfica,
  comandos enviados, respostas e tempos limite na comunicação com o plugin.

Anote também a mensagem exibida no rodapé e se o dongle já estava conectado
ao ligar o Xbox. A versão atual ainda não executa o vínculo USB nem o
pareamento, portanto esses eventos ainda não aparecerão no log.
- [x] **Teste 1 (Smoke):** No Xbox 360 E (kernel 17559), o app e a capa
  apareceram na Aurora, a UI abriu em 720p escalado para 1080p e o plugin
  resolveu todos os ordinais consultados, inclusive o 755. O gerenciador
  recebeu respostas do plugin para as solicitações de busca. Houve uma falha
  de escrita com recuperação e um tempo limite de resposta no fim do log.
  O UGREEN na USB frontal permaneceu offline, como esperado enquanto o
  vínculo USB estiver ausente.
- [ ] **Teste 2 (Descoberta USB `0xE0/0x01/0x01` no kernel `17559`):**
  - Validar `UsbdRegisterDriverObject` (@755) ou identificar o ponto de despacho de dispositivos não-HID no `Usbd` do `xboxkrnl.exe` `17559`.
- [ ] **Teste 3 (Bulk + Firmware + DS4 no console):**
  - Confirmar `UsbdOpenEndpoint(..., BULK)` + upload de `rtl8761bu_fw.bin` + pareamento DS4 + navegação na Aurora/jogos.

---

## 4. Comandos Rápidos no PC x86

```bash
# Rodar os testes sem hardware (o firmware externo é opcional):
cd src/linux_test && make all && make test

# Opcional: testar uma imagem de firmware descompactada:
RTL_FIRMWARE=/caminho/rtl8761bu_fw.bin make test

# Opcional: integração com o índice HCI escolhido e permissões apropriadas:
make test-live HCI_INDEX=3

# Para recompilar os XEX a partir da raiz do repositório:
source build/xsdk-env.sh
cmake --build build/plugin
cmake --build build/ui

# Para compilar e abrir a interface no Xenia, a partir da raiz:
cmake -S src/ui -B build/ui-xenia -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/research/repos/xsdk/cmake/xsdk.cmake" \
  -DXSDK_TOOLCHAIN="$XSDK_TOOLCHAIN" -DBTMANAGER_XENIA=ON
cmake --build build/ui-xenia
build/xenia/run-xenia.sh --async_shader_compilation=false --hid=winkey \
  --keyboard_mode=1 --keybind_dpad_left=A --keybind_dpad_right=D \
  --keybind_dpad_up=W --keybind_dpad_down=S "$PWD/build/ui-xenia/BTManager.xex"
```

As ferramentas em `build/` e os repositórios em `research/repos/` não são
versionados; mantenha essas pastas para reutilizar o ambiente local.
