# Roadmap e Plano do Primeiro Experimento (Fases 5 e 6)

---

## 1. Roadmap de Implementação

### Etapa 0 — Preparação de Ambiente e Identificação do Dongle (Linux)
- [ ] Plugar o adaptador **UGREEN Bluetooth USB** no Linux e capturar `lsusb -v` + `dmesg` para identificar `VID:PID`, chipset, classe USB e descritores de endpoints.
- [ ] Avaliar e configurar o toolchain open-source no Linux:
  - Testar [`xenosworks/xsdk`](https://github.com/xenosworks/xsdk) para compilação PowerPC (`powerpc-xsdk-elf`) e geração de `.xex`.
  - Verificar se `elf2xex.py` gera DLL/plugin `.xex` carregável pelo **DashLaunch** no `XeUnshackle` (ou se o primeiro teste de leitura de descritores será feito como app `.xex` standalone caso necessário).
- [ ] Validar em console Xbox 360 E que **ABadAvatar + XeUnshackle + Aurora** carregam um plugin `.xex` "Hello World" mínimo (apenas `DbgPrint` + `XNotifyQueueUI` + escrita de `Usb:\bt_test.log`).

### Etapa 1 — Descoberta / Binding USB no Kernel `17559`
- [ ] Investigar e validar o mecanismo de binding para dispositivos classe `0xE0 / 0x01 / 0x01`:
  - Opção primária: testar/inspecionar assinatura e struct de `UsbdRegisterDriverObject` (@755).
  - Opção alternativa: identificar o ponto de despacho/enumeração de dispositivos no `Usbd` do `xboxkrnl.exe` retail `17559` (análogo ao que `hiddriver360` fez com `HidAddDevice` / `UsbdDriverEntry`).
- [ ] **Marco 1:** Plugin detecta o dongle UGREEN conectado/reconectado, lê `usb_device_descriptor` e `usb_interface_descriptor`, e registra `VID`, `PID`, `bcdDevice` e `bInterfaceClass/SubClass/Protocol` em `Usb:\bt_diag.log`.

### Etapa 2 — Protótipo 1: Endpoints HCI + `HCI_Reset` + `HCI_Read_Local_Version_Information`
- [ ] Abrir Endpoint 0 (`UsbdOpenDefaultEndpoint`) e enviar `SET_CONFIGURATION (1)`.
- [ ] Localizar via `UsbdGetEndpointDescriptor` e abrir via `UsbdOpenEndpoint`:
  - Interrupt IN (`0x03`, IN)
  - Bulk IN (`0x02`, IN) e Bulk OUT (`0x02`, OUT) *(validação empírica do suporte a Bulk no `UsbdOpenEndpoint`)*.
- [ ] Iniciar leitura assíncrona contínua no Interrupt IN (`UsbdQueueAsyncTransfer`).
- [ ] Enviar comando **`HCI_Reset` (`0x0C03`)** via Control Transfer (`bmRequestType = 0x20, bRequest = 0x00`).
- [ ] Aguardar evento **`Command Complete` (`0x0E`)** no Interrupt IN com `status == 0x00`.
- [ ] Enviar **`HCI_Read_Local_Version_Information` (`0x1001`)** e **`HCI_Read_BD_ADDR` (`0x1009`)**.
- [ ] Decodificar e gravar em `Usb:\bt_diag.log` + exibir resumo via `XNotifyQueueUI` na Aurora:
  - Versão HCI, Revisão, Manufacturer ID (Bluetooth SIG), Endereço MAC Bluetooth (`BD_ADDR`).
- [ ] **Marco de Sucesso Inicial:** Dongle Bluetooth USB genérico respondendo corretamente a comandos HCI executados diretamente pelo Xbox 360.

### Etapa 3 — Robustez, Desconexão e Quirks de Chipsets
- [ ] Tratar desconexão física do dongle (`UsbdQueueCloseEndpoint`, `UsbdQueueCloseDefaultEndpoint`, `UsbdRemoveDeviceComplete`) sem crashar o console.
- [ ] Implementar timeouts em comandos HCI e remontagem de eventos fragmentados.
- [ ] Testar com pelo menos 2–3 dongles de chipsets distintos (ex: CSR8510, Realtek RTL8761B, Barrot BR8654) e adicionar camada extensível de quirks/firmware loader se necessário.

### Etapa 4 — Camada L2CAP / Perfis + Interface de Conexão (Futuro)
- Comunicação ACL sobre Bulk IN/OUT, HCI Inquiry/Connect, L2CAP, persistência de dispositivos pareados e posteriormente integração com `XamInput` para controles.
- **Interface `BTManager.xex` (inspirada na UI original do Xbox 360):** App lançável pela Aurora comunicando via IPC com o plugin residente (`bt360_plugin.xex`) para exibir status do dongle, buscar dispositivos, parear, conectar e remover dispositivos.

---

## 2. Proposta de Primeiro Experimento (Passo a Passo Seguro)

Para minimizar riscos no **Xbox 360 E (ABadAvatar)**, o primeiro experimento deve ser **100% não-destrutivo e isolado**:

1. **Nunca tocar na NAND/Flash:** Todo arquivo de teste reside apenas no pendrive (`Usb:\`).
2. **Experimento 1A (PC Linux):** Rodar `lsusb -v` no dongle UGREEN para documentar seus descritores reais antes de escrever uma linha de código de Xbox.
3. **Experimento 1B (Console — Plugin Mínimo):** Configurar `launch.ini` no pendrive do XeUnshackle com um plugin mínimo que:
   - Verifica `XboxKrnlVersion->Build == 17559` e `IsTrayOpen() == FALSE` (failsafe usado pelo `hiddriver360`: abrir a bandeja impede o carregamento do plugin em caso de problema).
   - Resolve os ordinais `Usbd*` (`740`..`759`) e grava os ponteiros resolvidos em `Usb:\bt_diag.log`.
   - Emite um `XNotifyQueueUI` na Aurora confirmando que o plugin carregou.
4. **Experimento 1C (Console — Enumeração + HCI Reset):** Adicionar o binding do dispositivo `0xE0/0x01/0x01`, `SET_CONFIGURATION`, abertura do Interrupt IN, envio de `HCI_Reset` (`0x0C03`) e `HCI_Read_Local_Version_Information` (`0x1001`), registrando o hex dump bruto da resposta no log.
