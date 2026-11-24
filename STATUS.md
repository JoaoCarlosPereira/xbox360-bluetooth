# Status do Projeto — Xbox 360 Generic USB Bluetooth (`xbox-blutooth`)

> Documento de handoff para continuidade em **PC x86 (Linux/WSL)** e posterior validação no **Xbox 360 E (ABadAvatar + XeUnshackle + Aurora)**.

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
- [ ] **Compilar o toolchain `xsdk-gcc` no PC x86:**
  ```bash
  bash research/repos/xsdk/tools/toolchain/build.sh
  cd research/repos/xsdk && make
  ```
- [ ] **Compilar `BTManager.xex` e `bt360_proto.xex`** usando `cmake -DCMAKE_TOOLCHAIN_FILE=.../xsdk/cmake/xsdk.cmake`.
- [ ] **Testar `BTManager.xex` no Xenia Canary (PC x86):** validar layout 720p, fontes, cores, navegação D-pad e tela de mapeamento de botões.

### Fase B — Integração do Plugin Residente (`bt360_plugin.xex`)
- [ ] **Conectar `bt360_plugin.xex` com toda a pilha `src/`:**
  - Adicionar `submit_bulk_in` / `submit_bulk_out` em `xbox360_usbd.c` (`UsbdOpenEndpoint` com `BT_USB_EP_TYPE_BULK = 2`).
  - Conectar `rtl8761b.c` lendo `rtl8761bu_fw.bin` e `rtl8761bu_config.bin` de `Usb:\bt_fw\` via `NtCreateFile`/`NtReadFile` (no Xbox 360 o dongle começa em cold-boot ROM!).
  - Conectar `l2cap.c` + `ds4_bt.c` + `bt_manager.c` em uma thread de sistema (`ExCreateThread` com `EX_CREATE_FLAG_SYSTEM`), mantendo callbacks DPC livres de FPU/VMX.
- [ ] **Canal IPC real entre `bt360_plugin.xex` e `BTManager.xex`:**
  - Expor `bt_ipc_state_t` via memória compartilhada / export de DLL do plugin (ou socket loopback / seção mapeada) para que o `BTManager.xex` controle o plugin ao vivo.
- [ ] **Persistência de `Link Key` + Reconexão automática:**
  - Salvar `Link_Key_Notification` (`0x18`) em `Usb:\bt_devices.ini` e responder `Link_Key_Request` (`0x17`) com `Link_Key_Request_Reply` (`0x0B` / `0x040B`), além de habilitar `Write_Scan_Enable` (`0x0C1A = 0x03`) para o DS4 reconectar apenas apertando o botão **PS**.
  - Persistir também o `bt_button_map_t` customizado de cada controle no `bt_devices.ini`.
- [ ] **Injeção de controles no `XamInput`:**
  - Portar os hooks de `XamInputGetCapabilitiesEx`, `XamInputSetState` (rumble -> saída DS4 `0x11`!), `XInputdReadState` e `XamUserBindDeviceCallback` (usando `research/repos/hiddriver360/hiddriver/main.cpp` como referência exata para retail `17559`).

### Fase C — Validação no Xbox 360 E Real (ABadAvatar + XeUnshackle + Aurora)
- [ ] **Teste 1 (Smoke):** Carregar `bt360_proto.xex` via `launch.ini` do `XeUnshackle` e confirmar geração de `Usb:\bt360_diag.log` com resolução dos ordinais `Usbd*`.
- [ ] **Teste 2 (Descoberta USB `0xE0/0x01/0x01` no kernel `17559`):**
  - Validar `UsbdRegisterDriverObject` (@755) ou identificar o ponto de despacho de dispositivos não-HID no `Usbd` do `xboxkrnl.exe` `17559`.
- [ ] **Teste 3 (Bulk + Firmware + DS4 no console):**
  - Confirmar `UsbdOpenEndpoint(..., BULK)` + upload de `rtl8761bu_fw.bin` + pareamento DS4 + navegação na Aurora/jogos.

---

## 4. Comandos Rápidos no PC x86

```bash
# Rodar todos os testes unitários no Linux/WSL:
cd src/linux_test && make clean && make all && make test

# Se o repo foi movido do Orange Pi, lembre-se de clonar os submódulos/repos de referência se não copiou research/repos/:
# (research/repos/ está no .gitignore)
```
