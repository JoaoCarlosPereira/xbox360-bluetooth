# Referências Técnicas e Repositórios Analisados

Os repositórios abaixo foram clonados localmente em `research/repos/` (ignorados pelo Git via `.gitignore`) para análise primária de código-fonte durante a Fase 2:

| Diretório | Repositório Original | Propósito da Análise |
| :--- | :--- | :--- |
| `research/repos/UsbdSecPatch` | https://github.com/InvoxiPlayGames/UsbdSecPatch | Verificar o escopo real do patch (`UsbdIsDeviceAuthenticated` e `WgcAddDevice`). |
| `research/repos/hiddriver360` | https://github.com/EinTim23/hiddriver360 | Referência primária de uso das APIs `Usbd*` do `xboxkrnl.exe` em plugin DashLaunch no retail `17559`, incluindo `UsbTrb`, `UsbControlTrb`, `UsbdQueueAsyncTransfer`, `UsbdOpenEndpoint` e `xkelib`. |
| `research/repos/WirelessInput360` | https://github.com/UncreativeXenon/WirelessInput360 | Referência de estrutura de plugin DashLaunch e futura integração com `XamInput`. |
| `research/repos/FreeMyXe` | https://github.com/FreeMyXe/FreeMyXe | Análise das capacidades e limitações do loader FreeMyXe sob BadUpdate/ABadAvatar. |
| `research/repos/XeUnshackle` | https://github.com/Byrom90/XeUnshackle | Análise do carregamento de `launch.xex` (DashLaunch) e patches HV/Kernel sob ABadAvatar. |

## Outras Referências Primárias
- **xenosworks/xsdk:** https://github.com/xenosworks/xsdk — Toolchain GCC open-source para Linux capaz de gerar `.xex` sem o XDK proprietário da Microsoft.
- **ABadAvatar:** https://github.com/shutterbug2000/ABadAvatar — Exploit de software para dashboard `2.0.17559.0`.
- **Linux Kernel `btusb.c`:** `drivers/bluetooth/btusb.c` — Referência padrão de transporte Bluetooth USB HCI.
- **Bluetooth Core Specification (Vol. 4, Part B):** Especificação oficial da camada de transporte USB para HCI.
