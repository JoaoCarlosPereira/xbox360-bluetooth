# Código Experimental (`src/`)

Este diretório está reservado para o código-fonte do protótipo mínimo (Protótipo 1), a ser iniciado após:

1. Identificação do `VID:PID` e descritores do dongle UGREEN via `lsusb -v` no Linux.
2. Validação do toolchain de build no Linux (`xenosworks/xsdk`) para geração de `.xex`.
3. Validação do ponto de *binding* de dispositivos `0xE0/0x01/0x01` no kernel `17559`.

## Estrutura Planejada

- `hal/` — Wrapper `Usbd*` do Xbox 360 (`UsbTrb`, `UsbControlTrb`, resolução de ordinais).
- `transport/` — Transporte Bluetooth USB HCI (Control EP0, Interrupt IN, Bulk IN/OUT).
- `hci/` — Comandos e parser de eventos HCI mínimos (`HCI_Reset`, `HCI_Read_Local_Version_Information`, `HCI_Read_BD_ADDR`).
- `plugin/` — Ponto de entrada `DllMain` (DashLaunch) + log diagnóstico em `Usb:\bt_diag.log`.
