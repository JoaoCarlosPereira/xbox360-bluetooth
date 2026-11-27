# Compatibilidade de Chipsets e Adaptadores Bluetooth USB

Este documento analisa o comportamento de diferentes famílias de chipsets Bluetooth USB e o impacto no objetivo de criar uma camada **genérica** no Xbox 360.

> **Regra de evidência:** Nenhum chipset será declarado compatível no Xbox 360 sem teste real em console. Os dados abaixo refletem comportamento documentado no driver `btusb.c` do Linux e relatos da comunidade Linux para dongles **UGREEN** e similares.

---

## 1. Classificação por Necessidade de Firmware

| Família / Fabricante | VID típico | Precisa de Firmware Externo para HCI Básico? | Observações Técnicas (`btusb.c` / Linux) |
| :--- | :--- | :--- | :--- |
| **CSR / Qualcomm (genéricos, CSR8510 A10)** | `0x0A12` | **Não** (ROM funcional) | Responde diretamente a `HCI_Reset` e `HCI_Read_Local_Version_Information`. Excelente candidato para prototipagem inicial. *(Atenção: clones baratos chineses com VID `0x0A12:0x0001` às vezes têm quirks de SCO/suspend, mas HCI básico funciona).* |
| **Barrot (BR8051 / BR8554 / BR8654)** | `0x33FA` (`33fa:0010`) | **Não** (ROM/Flash interna) | Muito comum em dongles **UGREEN BT 5.3 / 5.4** (CM748/CM749). **[TERCEIROS]** Possui bug conhecido no comando `HCI_Read_Local_Extended_Features` (`0x1005`), causando timeout `-110` no Linux antigo, mas responde a `HCI_Reset` e `HCI_Read_Local_Version_Information`. |
| **Actions Semiconductor (ATS2851)** | `0x10D7` (`10d7:b012`) | **Depende da revisão** | Encontrado em alguns **UGREEN BT 5.3** (CM591). Pode operar como HCI padrão ou exigir quirk de inicialização. |
| **Realtek (RTL8761B / RTL8761BU / RTL8821C)** | `0x0BDA` | **Recomendado/Parcial** | Muito comum em **UGREEN BT 5.0**. Geralmente responde a `HCI_Reset` e `HCI_Read_Local_Version` com o firmware da ROM de fábrica, mas precisa de patch de firmware (`rtl_bt/*.bin`) via comandos vendor-specific para operar com estabilidade total em conexões/BLE. |
| **Broadcom (BCM20702 / BCM2045)** | `0x0A5C` / `0x0489` | **Frequentemente Sim** | Alguns dongles funcionam na ROM, mas muitos exigem upload de arquivo `.hcd` via comando HCI vendor-specific antes de parear/conectar dispositivos. |
| **Intel (AX200/AX210/8265 - geralmente M.2/USB interno)** | `0x8087` | **Sim (obrigatório em quase todos)** | Inicia em modo bootloader (`Intel_Read_Version`), exigindo download de `.sfi`/`.ddc` antes de virar controlador HCI completo. |

---

## 2. Adaptador Inicial UGREEN Identificado (`2B89:8761` — Realtek RTL8761BU)

**[COMPROVADO]** Inspeção direta no Linux (`lsusb -v -d 2b89:8761`, `dmesg` e `hciconfig -a`):

- **USB ID:** `2B89:8761` (`Realtek Bluetooth Radio`, `bcdUSB 1.10`, Full Speed 12 Mbps, `bcdDevice 2.00`).
- **Classe USB:** `bDeviceClass = 0xE0` (Wireless), `bDeviceSubClass = 0x01` (RF), `bDeviceProtocol = 0x01` (Bluetooth).
- **Interface 0 (AltSetting 0):** `0xE0 / 0x01 / 0x01` com 3 endpoints:
  - `0x81` (EP 1 IN) — **Interrupt**, `wMaxPacketSize = 16`, `bInterval = 1`
  - `0x02` (EP 2 OUT) — **Bulk**, `wMaxPacketSize = 64`
  - `0x82` (EP 2 IN) — **Bulk**, `wMaxPacketSize = 64`
- **Chipset e Firmware:** **Realtek RTL8761BU** (`Manufacturer = 0x005D` / `93`).
  - **ROM de fábrica:** `hci_ver=0x0A` (BT 5.1), `hci_rev=0x000B`, `lmp_subver=0x8761`, `rom_version=1` — suficiente para `HCI_Reset` e `HCI_Read_Local_Version_Information` no Protótipo 1.
  - **Firmware completo (Linux `btrtl`):** carrega `rtl_bt/rtl8761bu_fw.bin` + `rtl_bt/rtl8761bu_config.bin` (`27.814 bytes` total), elevando para `fw version 0x09a98a6b` (`HCI Revision 0x09A9`, `LMP Subversion 0x8A6B`).
  - **Validado no harness `hci_probe` / `test_usb_transport`:** `Status: 0x00`, `Bluetooth 5.1 (0x0A)`, `Manufacturer: 0x005D`, `BD_ADDR: 0A:62:30:51:10:EB`.

---

## 3. Estratégia de Arquitetura Implementada (`src/chipset/bt_chipset.c`)

Para não ficar preso a um único fabricante:

1. **Núcleo Genérico (Camada 1 — Padrão USB HCI):**
   - `bt_chipset_match_usb()` aceita qualquer dispositivo padrão `0xE0 / 0x01 / 0x01` na Interface 0 / AltSetting 0, protegendo dispositivos Microsoft (`0x045E`) e recusando bootloaders Intel (`0x8087`).
   - Executa `SET_CONFIGURATION`, abre EP0 (Control), Interrupt IN, Bulk IN e Bulk OUT, envia `HCI_Reset` (com até 3 tentativas em caso de timeout), `HCI_Read_Local_Version_Information` e `HCI_Read_BD_ADDR`.
   - Usa máscara de eventos Bluetooth Classic + SSP + EIR sem bits reservados/LE (`FF FF FB FF 07 F8 B7 00`) com fallback automático para clones antigos, além de fallback `Write_Inquiry_Mode(0x02 -> 0x01)`.
2. **Camada de Quirks e Firmware Loaders (Camada 2 — `bt_chipset_identify` + `rtl_boot.c`):**
   - Classifica automaticamente Realtek (`0x005D`), CSR/Qualcomm (`0x000A` / `0x0A12`), Barrot (`0x08E7` / `0x33FA`), Actions (`0x03E0` / `0x10D7`), Broadcom (`0x000F`) e genéricos.
   - Dongles Plug & Play (CSR, Barrot, Actions, genéricos) inicializam **sem nenhum `.bin` no pendrive**.
   - Dongles Realtek EPATCH v1 (`RTL8761BU`, `RTL8761A`, `RTL8761BTV`, `RTL8821C`) buscam o firmware em `Usb:\bt_fw\` com fallback na raiz `Usb:\`. Caso ausente, o console exibe `"Firmware do adaptador ausente no USB."`.

---

## 4. Matriz de Evidência no Xbox 360

| Adaptador / Chipset | VID:PID | Precisa de Firmware? | Status no Xbox 360 |
| :--- | :--- | :--- | :--- |
| **UGREEN (Realtek RTL8761BU)** | `2B89:8761` | Sim (`rtl8761bu_fw.bin`) | **[COMPROVADO XBOX]** Pareamento, reconexão e 2x DS4 validados |
| **TP-Link UB500 / Orico / Baseus / Genérico (RTL8761BU)** | `2357:0604`, `0BDA:8771`, etc. | Sim (`rtl8761bu_fw.bin`) | **[AGUARDANDO TESTE XBOX]** Mesmo chipset/firmware do UGREEN |
| **CSR8510 A10 e clones baratos** | `0A12:0001` | **Não** | **[AGUARDANDO TESTE XBOX]** Suporte genérico + fallbacks HCI ativos |
| **Barrot BR8051 / BR8554 / BR8654 (BT 5.3/5.4)** | `33FA:0010` | **Não** | **[AGUARDANDO TESTE XBOX]** Suporte genérico ativo (`0x1005` não é chamado) |
| **Actions ATS2851 (BT 5.3)** | `10D7:B012` | **Não** | **[AGUARDANDO TESTE XBOX]** Suporte genérico ativo |
| **Intel (AX200/AX210/8265)** | `8087:*` | Sim (`.sfi`/`.ddc`) | **[NÃO SUPORTADO]** Recusado explicitamente no match USB |
