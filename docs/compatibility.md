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

## 3. Estratégia de Arquitetura para Compatibilidade Genérica

Para não ficar preso a um único fabricante:

1. **Núcleo Genérico (Camada 1 — Padrão USB HCI):**
   - Detecta qualquer dispositivo `0xE0 / 0x01 / 0x01` (ou lista de VID/PID conhecidos que usem `0xFF`).
   - Executa `SET_CONFIGURATION`, abre EP0 (Control), Interrupt IN, Bulk IN e Bulk OUT.
   - Envia `HCI_Reset` e `HCI_Read_Local_Version_Information`.
2. **Camada de Quirks e Firmware Loaders (Camada 2 — Extensível):**
   - Tabela de `(VID, PID)` $\rightarrow$ `bt_chipset_ops` (ex: `realtek_setup()`, `barrot_quirks()`, `broadcom_setup()`).
   - Se um chipset precisar de firmware (`.bin` / `.hcd`), ele poderá ser lido de `Usb:\bt_fw\` ou `Hdd:\bt_fw\` em etapas futuras, sem alterar o núcleo HCI USB.
