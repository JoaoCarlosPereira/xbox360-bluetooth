# Arquitetura USB do Xbox 360 e Ambiente ABadAvatar

Este documento consolida os achados da **Fase 1** e **Fase 2**, detalhando como a pilha USB do Xbox 360 funciona e quais mecanismos estão disponíveis sob **ABadAvatar + XeUnshackle**.

---

## 1. Hardware USB do Xbox 360

- **[TERCEIROS]** O Xbox 360 utiliza controladores host **OHCI** (USB 1.1, Low/Full-Speed) e **EHCI** (USB 2.0, High-Speed) integrados ao Southbridge (confirmado pelos logs e drivers do `libxenon` em `0xea003000` / `0xea005000`).
- **[COMPROVADO]** A pilha do kernel (`Usbd`) expõe `UsbdGetDeviceSpeed` (@742) para distinguir velocidade do dispositivo (e.g., `speed == 0` para OHCI/Low-Full speed, conforme visto em `hiddriver360/hiddriver/main.cpp:1219-1220`).

---

## 2. Pilha USB do Kernel (`Usbd` em `xboxkrnl.exe`)

Diferente do modelo WDM/WDF do Windows NT, o kernel do Xbox 360 implementa uma pilha USB compacta e integrada dentro do próprio `xboxkrnl.exe` chamada **`Usbd`**.

### Exportações Relevantes (`xboxkrnl.exe` — `kernelext21250.0.def`)

| Ordinal | Símbolo | Papel Comprovado / Documentado |
| :--- | :--- | :--- |
| `740` | `UsbdAddDeviceComplete` | **[COMPROVADO]** Sinaliza ao USBD que o driver aceitou/concluiu a adição do `deviceHandle` (`hiddriver360:1280`). |
| `741` | `UsbdCancelAsyncTransfer` | **[TERCEIROS]** Cancela uma transferência `UsbTrb` pendente. |
| `742` | `UsbdGetDeviceSpeed` | **[COMPROVADO]** Retorna a velocidade do dispositivo USB (`hiddriver360:1219`). |
| `743` | `UsbdGetDeviceTopology` | **[TERCEIROS]** Consulta topologia/porta do dispositivo. |
| `744` | `UsbdGetEndpointDescriptor` | **[COMPROVADO]** Obtém ponteiro para `usb_endpoint_descriptor` por índice, tipo de transferência e direção (`hiddriver360:470`). |
| `745` | `UsbdIsDeviceAuthenticated` | **[COMPROVADO]** Verifica autenticação de segurança de controles Xbox (`UsbdSecPatchPlugin.cpp:95-118`). |
| `746` | `UsbdOpenDefaultEndpoint` | **[COMPROVADO]** Abre o Endpoint 0 (Control) associado a um `UsbControlTrb` (`hiddriver360:1282`). |
| `747` | `UsbdOpenEndpoint` | **[COMPROVADO]** Abre um endpoint (Interrupt/Bulk/Isoch) passando endereço, `wMaxPacketSize`, `bInterval` e `UsbTrb` (`hiddriver360:473`). |
| `748` | `UsbdQueueAsyncTransfer` | **[COMPROVADO]** Enfileira transferência assíncrona (`UsbTrb` ou `UsbControlTrb`) com callback de conclusão (`hiddriver360:375,389`). |
| `749` | `UsbdQueueCloseDefaultEndpoint` | **[COMPROVADO]** Fecha o endpoint de controle padrão. |
| `750` | `UsbdQueueCloseEndpoint` | **[COMPROVADO]** Fecha um endpoint aberto. |
| `751` | `UsbdRemoveDeviceComplete` | **[COMPROVADO]** Finaliza a remoção do dispositivo após desconexão. |
| `755` | `UsbdRegisterDriverObject` | **[TERCEIROS]** Registra um objeto de driver USB na pilha USBD. Layout exato da struct ainda **[NÃO INVESTIGADO]**. |
| `756` | `UsbdUnregisterDriverObject` | **[TERCEIROS]** Remove o registro do objeto de driver USB. |
| `758` | `UsbdResetDevice` | **[TERCEIROS]** Solicita reset do dispositivo USB. |
| `759` | `UsbdGetDeviceDescriptor` | **[COMPROVADO]** Retorna ponteiro para `usb_device_descriptor` (`hiddriver360:1213`). |
| `892` | `UsbdResetEndpoint` | **[TERCEIROS]** Reseta estado/halt de um endpoint. |
| `895` | `UsbdQueueIsochTransfer` | **[TERCEIROS]** Transferências isócronas (áudio SCO). |

> **Nota sobre Endianness (PowerPC Big-Endian):**
> **[COMPROVADO]** O Xbox 360 é Big-Endian, enquanto descritores e pacotes USB são Little-Endian. Como visto em `hiddriver360/hiddriver/main.cpp:367-369` e `1216-1217`, campos de 16 bits como `idVendor`, `idProduct`, `wMaxPacketSize`, `wValue`, `wIndex` e `wLength` **devem** sofrer `bswap16` (`swap_endianness_16`).

---

## 3. Estruturas de Transferência (`UsbTrb` e `UsbControlTrb`)

Extraído diretamente de `research/repos/hiddriver360/hiddriver/main.cpp:44-69`:

```cpp
struct UsbTrb {
    DWORD endpoint;
    DWORD callback;
    DWORD savedEndpoint;
    BYTE  padding[4];
    BYTE  flags;
    BYTE  controllerIndex;   // escrito por UsbdQueueAsyncTransfer
    BYTE  pad2;
    BYTE  endpointIndex;     // escrito por UsbdQueueAsyncTransfer
    void* buffer;
    DWORD length;
};

struct UsbPacket {
    BYTE  bmRequestType;
    BYTE  bRequest;
    WORD  wValue;   // big-endian na memória -> swap_endianness_16!
    WORD  wIndex;   // big-endian na memória -> swap_endianness_16!
    WORD  wLength;  // big-endian na memória -> swap_endianness_16!
};

struct UsbControlTrb {
    UsbTrb    trb;
    BYTE      pad[4];
    UsbPacket packet;
};
```

- **[COMPROVADO]** Para transferências **Control** (Endpoint 0): preenche-se `controlTrb->packet` (SETUP packet de 8 bytes), aponta `trb.buffer` e `trb.length` para os dados, define `trb.flags = 1`, `trb.callback`, copia `trb.endpoint` para `trb.savedEndpoint` e chama `UsbdQueueAsyncTransfer(deviceHandle, controlTrb)`.
- **[COMPROVADO]** Para transferências **Interrupt** (e **[HIPÓTESE]** para **Bulk**): usa-se `UsbTrb` após `UsbdOpenEndpoint(deviceHandle, transferType, bEndpointAddress, maxPacketSize, interval, &trb)`.

---

## 4. Limitações para Dispositivos Não Reconhecidos Nativamente

1. **[COMPROVADO]** A pilha `Usbd` do Xbox 360 só possui drivers nativos para classes específicas (XInput/WGC, Mass Storage, HID básico limitado, áudio/câmera proprietários).
2. **[COMPROVADO]** Dispositivos fora dessas classes (como Bluetooth `0xE0/0x01/0x01`) não possuem driver nativo associado no kernel retail.
3. **[COMPROVADO]** O `UsbdSecPatch` atua apenas em `UsbdIsDeviceAuthenticated` e `WgcAddDevice`, **não** resolvendo o reconhecimento de dispositivos não-XInput/não-HID.
4. **[COMPROVADO]** Em `hiddriver360`, como os dispositivos alvo eram HID (`0x03`), foi suficiente hookear `HidAddDevice` (`0x800E4D68` no retail `17559`) e `HidRemoveDevice` (`0x800E4D28`) e forçar uma re-enumeração USB via `UsbdPowerDownNotification()` + `UsbdDriverEntry()` (`main.cpp:1596-1602`). Para Bluetooth, precisaremos de `UsbdRegisterDriverObject` ou de um hook no nível de despacho do próprio `Usbd`.

---

## 5. Ambiente ABadAvatar, XeUnshackle e DashLaunch

- **[COMPROVADO]** **ABadAvatar** (`shutterbug2000/ABadAvatar`) explora a dashboard `2.0.17559.0` via dados de avatar/perfil em USB para obter execução no hypervisor sem modchip.
- **[COMPROVADO]** **XeUnshackle** (`Byrom90/XeUnshackle`):
  - Aplica o conjunto completo de patches freeBOOT/xeBuild em memória (HV + Kernel).
  - Suporta XEXs Retail e Devkit.
  - Carrega `launch.xex` (DashLaunch) em memória (`XexLoadImageFromMemory(..., XEX_LOADIMG_TYPE_SYSTEM_DLL, ...)` em `Dashlaunch.cpp:21`), copiando `lhelper.xex` para `Hdd:\` ou `Usb:\`.
  - Lê `[Plugins]` (`plugin1`..`plugin5`) do `launch.ini` para carregar módulos `.xex` em espaço de sistema.
- **[COMPROVADO]** Plugins carregados pelo DashLaunch executam como **DLLs de sistema** (`DllMain` com `DLL_PROCESS_ATTACH`), permanecem ativos durante a dashboard **Aurora** e durante jogos, e podem criar threads de sistema persistentes (`ExCreateThread` com `EX_CREATE_FLAG_SYSTEM`, conforme `hiddriver360/hiddriver/main.cpp:33-40`).
- **[COMPROVADO]** **Diferença para RGH:** No ABadAvatar, tudo é volátil (reverte ao reiniciar). Isso é uma **vantagem para P&D**: se um plugin causar crash ou travar no boot, basta remover o pendrive ou editar `launch.ini` no PC — sem risco de brick na NAND, desde que nunca se escreva em `\Device\Flash`.

---

## 6. Análise dos Projetos de Referência (Fase 2)

| Projeto | O que faz na prática | O que podemos reutilizar |
| :--- | :--- | :--- |
| **UsbdSecPatch** | Patcha `UsbdIsDeviceAuthenticated` e `WgcAddDevice`. | Nada para Bluetooth HCI (já incluso no XeUnshackle/FreeMyXe). |
| **hiddriver360** | Plugin DashLaunch que hooka `HidAddDevice` e usa `Usbd*` para falar com controles HID e injetar no `XamInput`. | **Muito alto:** definições de `UsbTrb`/`UsbControlTrb`, resolução por ordinal dos `Usbd*`, fluxo de callbacks assíncronos, `MakeThread`, re-enumeração USB. |
| **WirelessInput360** | Plugin DashLaunch que recebe inputs via TCP de um PC e injeta no `XamInput`. | Referência futura para injeção no `XamInput` (fora do escopo atual) e estrutura de plugin DashLaunch. |
| **XeUnshackle** | Loader pós-exploit com patches HV/Kernel + DashLaunch. | Ambiente de execução alvo no console com ABadAvatar. |
| **FreeMyXe** | Loader pós-exploit leve (sem DashLaunch). | Referência para `XexLoadImage` em thread de sistema (`FreeMyXe.c:298-323`). |
| **libxenon** | SDK bare-metal para XeLL Reloaded (drivers EHCI/OHCI próprios). | Não recomendado como alvo principal porque não roda sobre a dashboard/Aurora e tem histórico de incompatibilidade/limitações em Xbox 360 E Winchester. |
