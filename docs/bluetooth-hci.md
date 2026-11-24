# Transporte Bluetooth USB HCI (Fase 3)

Este documento descreve o funcionamento mínimo necessário do transporte **Bluetooth HCI sobre USB** (*Bluetooth Core Specification, Vol. 4, Part B — USB Transport Layer*), tendo como referência o driver `drivers/bluetooth/btusb.c` do kernel Linux e implementações embarcadas minimalistas.

> **Princípio de projeto:** Não portar o BlueZ nem o `btusb.c` do Linux integralmente. O Linux acopla `btusb.c` ao `sk_buff`, `net/bluetooth/hci_core.c`, `urb`, `sysfs` e `udev`. Para o Xbox 360, precisamos apenas de uma **camada de transporte HCI USB mínima e autocontida** em C/C++.

---

## 1. Identificação e Enumeração USB

Um adaptador Bluetooth USB compatível com a especificação padrão apresenta:

- **Device Descriptor (ou Interface 0):**
  - `bDeviceClass = 0xE0` (Wireless Controller)
  - `bDeviceSubClass = 0x01` (RF Controller)
  - `bDeviceProtocol = 0x01` (Bluetooth Programming Interface)
- *Nota:* Alguns adaptadores compostos (ou em modo bootloader) anunciam `bDeviceClass = 0x00` (Defined at Interface level) ou `0xEF` (Miscellaneous), colocando `0xE0 / 0x01 / 0x01` na **Interface 0**, ou usando `0xFF` (Vendor-Specific) antes do upload de firmware.

---

## 2. Mapeamento de Endpoints HCI (Interface 0, AltSetting 0)

Conforme a especificação USB Transport Layer e o `btusb_probe()` do Linux:

| Canal HCI | Tipo de Transferência USB | Direção | Uso |
| :--- | :--- | :--- | :--- |
| **HCI Command** | **Control** (Endpoint `0x00`) | Host $\rightarrow$ Device | Envio de comandos HCI (`bmRequestType = 0x20`, `bRequest = 0x00`, `wValue = 0`, `wIndex = 0`). |
| **HCI Event** | **Interrupt** (`0x03`) | IN (`0x81`, etc.) | Recebimento assíncrono de respostas (`Command Complete`, `Command Status`, erros, eventos). |
| **ACL Data OUT** | **Bulk** (`0x02`) | OUT (`0x02`, etc.) | Envio de dados L2CAP/ACL (etapa futura). |
| **ACL Data IN** | **Bulk** (`0x02`) | IN (`0x82`, etc.) | Recebimento de dados L2CAP/ACL (etapa futura). |
| **SCO Audio** | **Isochronous** (`0x01`) | IN / OUT (AltSetting 1..5) | Áudio de voz síncrono (**não utilizado** no protótipo). |

### Mapeamento no Xbox 360 (`Usbd`)
- **[COMPROVADO]** `UsbdOpenDefaultEndpoint` abre o Control Endpoint `0x00`.
- **[COMPROVADO]** `UsbdGetEndpointDescriptor(deviceHandle, index, transferType, direction)` localiza endpoints por tipo (`0x02` Bulk, `0x03` Interrupt) e direção (`0` OUT, `1` IN).
- **[HIPÓTESE]** `UsbdOpenEndpoint` abre Interrupt IN (`transfertype = 3`) e Bulk IN/OUT (`transfertype = 2`) usando `wMaxPacketSize` (com `bswap16` e máscara `& 0x7FF`) e `bInterval`.

---

## 3. Formato dos Pacotes HCI no Protótipo Inicial

### Envio de Comando HCI (via Control Transfer EP0)
No transporte USB, **não** se envia o byte indicador de tipo H4 (`0x01`); envia-se o pacote de comando diretamente no payload da transferência Control:

- **USB Setup Packet:**
  - `bmRequestType = 0x20` (Host-to-Device | Class | Interface)
  - `bRequest = 0x00`
  - `wValue = 0x0000`
  - `wIndex = 0x0000` (ou número da interface HCI)
  - `wLength = 3 + param_len`
- **Payload (`hci_cmd_hdr` + parâmetros):**
  - `uint16_t opcode` (**Little-Endian** — atenção no PowerPC Big-Endian!)
  - `uint8_t param_len`
  - `uint8_t params[param_len]`

### Comandos Essenciais do Protótipo

1. **`HCI_Reset` (`Opcode = 0x0C03`, `OGF = 0x03`, `OCF = 0x0003`)**
   - Bytes na wire (LE): `03 0C 00` (`param_len = 0`).
   - Resposta esperada no Interrupt IN: `Command Complete` (`Event Code = 0x0E`).

2. **`HCI_Read_Local_Version_Information` (`Opcode = 0x1001`, `OGF = 0x04`, `OCF = 0x0001`)**
   - Bytes na wire (LE): `01 10 00`.
   - Resposta esperada (`Event Code = 0x0E`, `Parameter Total Length = 12`):
     - `Num_HCI_Command_Packets` (1 byte)
     - `Command_Opcode` (`01 10`, 2 bytes LE)
     - `Status` (`0x00` = sucesso, 1 byte)
     - `HCI_Version` (1 byte: ex. `0x09`=5.0, `0x0B`=5.2, `0x0C`=5.3, `0x0D`=5.4)
     - `HCI_Revision` (2 bytes LE)
     - `LMP_PAL_Version` (1 byte)
     - `Manufacturer_Name` (2 bytes LE — Company Identifier Bluetooth SIG, ex. `0x005D` = Realtek, `0x000F` = Broadcom, `0x0002` = Intel, `0x000A` = CSR/Qualcomm, `0x08E7` = Barrot)
     - `LMP_PAL_Subversion` (2 bytes LE)

3. **`HCI_Read_BD_ADDR` (`Opcode = 0x1009`, `OGF = 0x04`, `OCF = 0x0009`)** *(opcional no protótipo)*
   - Bytes na wire (LE): `09 10 00`.
   - Retorna o endereço MAC Bluetooth de 6 bytes do adaptador.

### Recebimento de Eventos HCI (via Interrupt IN)
- O host deve manter uma leitura assíncrona pendente no Interrupt IN (`UsbdQueueAsyncTransfer`).
- Quando o callback dispara:
  - `uint8_t event_code` (ex: `0x0E` = Command Complete, `0x0F` = Command Status, `0x10` = Hardware Error)
  - `uint8_t param_len`
  - `uint8_t params[param_len]`
- Re-submeter imediatamente o `UsbTrb` de Interrupt IN após processar o evento (ou quando o comprimento recebido for menor que `wMaxPacketSize` / pacote completo remontado).

---

## 4. Timeouts, Erros e Desconexões

- **Remontagem de eventos:** Alguns eventos maiores ou dongles OHCI podem fragmentar pacotes em múltiplos frames de `wMaxPacketSize` (tipicamente 16 bytes em dongles USB 1.1/2.0). Acumular até atingir `2 + param_len`.
- **Timeouts:** Em bare-metal/kernel plugin, usar uma fila/sinalização com timeout (ex: 1000–2000 ms para `HCI_Reset`) via thread de controle para não travar a thread de DPC/callback do USB.
- **Desconexão física:** Ao receber callback de remoção (`RemoveDevice`) ou erro de transferência (status NT de dispositivo desconectado), cancelar/fechar endpoints com `UsbdQueueCloseEndpoint` e `UsbdQueueCloseDefaultEndpoint`, liberar buffers e chamar `UsbdRemoveDeviceComplete`.
