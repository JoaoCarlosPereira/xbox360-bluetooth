# Arquitetura Proposta (Fase 4)

Este documento compara as quatro alternativas arquiteturais solicitadas para o suporte genérico a Bluetooth USB no Xbox 360 sob **ABadAvatar + XeUnshackle + Aurora**.

---

## 1. Comparação das Alternativas

| Critério | Alt. A — Plugin XEX (DashLaunch) | Alt. B — Patch / Extensão Direta do Kernel | Alt. C — App Homebrew Independente (`.xex`) | Alt. D — Híbrida (Núcleo USB baixo nível + Camada BT superior) |
| :--- | :--- | :--- | :--- | :--- |
| **Viabilidade técnica** | **Alta.** Comprovada pelo `hiddriver360` e `WirelessInput360`. | **Média/Baixa.** Exige binário/assembly injetado em espaço de kernel sem infraestrutura de módulo. | **Média.** App XEX normal roda em user-mode de título e tem restrições/conflitos com o USBD ativo do kernel; ao sair do app, o driver morre. | **Muito Alta.** Combina um driver/serviço USB residente (Plugin XEX) com camada HCI/BT modular (e permite app de diagnóstico acoplado ou IPC). |
| **Necessidade de R.E.** | **Moderada/Baixa.** Apenas descobrir o ponto de *binding* para `0xE0/0x01/0x01` (`UsbdRegisterDriverObject` ou hook no USBD). | **Muito Alta.** Exige patches em binário `xboxkrnl` por versão. | **Moderada.** Mesma necessidade de acesso ao USBD, porém sem persistência entre títulos. | **Moderada/Baixa.** Isola o único ponto de R.E. (binding USB) dentro de um módulo HAL pequeno e bem definido. |
| **Compatibilidade com ABadAvatar** | **Total.** `XeUnshackle` carrega DashLaunch (`launch.xex`) em RAM e lê `launch.ini`. | **Arriscada.** Patches estáticos em kernel podem conflitar com os patches dinâmicos do `XeUnshackle`. | **Total.** Roda na Aurora ou XeXMenu como qualquer homebrew. | **Total.** Plugin carrega via DashLaunch; ferramenta de diagnóstico pode rodar na Aurora falando com o plugin. |
| **Riscos de travamento** | **Médio** (roda em contexto de sistema/DPC, mas 100% reversível removendo o USB). | **Alto** (erro em patch de kernel causa panic imediato). | **Baixo** para o sistema, mas pode conflitar com USBD se tentar resetar a pilha USB durante a dashboard. | **Médio e controlado.** O núcleo USB fica mínimo e estável; lógica de protocolo BT roda em thread dedicada (`MakeThread`). |
| **Complexidade de desenvolvimento** | **Média.** | **Alta.** | **Baixa** para UI/diagnóstico, **Inviável** para uso em jogos. | **Média.** Separação clara facilita testes e depuração. |
| **Funciona durante jogos?** | **Sim** (residente via DashLaunch + thread de sistema). | **Sim.** | **Não** (encerra quando o jogo inicia). | **Sim** (o daemon/plugin fica residente). |
| **Reutilização por outros devs** | **Boa.** | **Baixa.** | **Média.** | **Excelente.** Outros projetos (ex: controles, áudio, rede) podem consumir a API HCI do serviço. |

---

## 2. Arquitetura Recomendada: Alternativa D (Híbrida baseada em Plugin XEX DashLaunch)

A arquitetura recomendada é uma **Arquitetura Híbrida em Camadas**, executada inicialmente como **Plugin XEX de sistema (via XeUnshackle / DashLaunch)** com interface de diagnóstico desacoplada:

```
+---------------------------------------------------------------+
|  Camada 4 (Futuro): Perfis / Clientes                         |
|  (HID-over-BT -> XamInput, Teclado/Mouse, Ferramenta UI)      |
+---------------------------------------------------------------+
|  Camada 3: Core HCI Mínimo (Platform-Independent C/C++)       |
|  - Máquina de estados HCI (Reset, Read Local Version, BD_ADDR)|
|  - Montagem de comandos / Parser de eventos / Tabela Quirks   |
+---------------------------------------------------------------+
|  Camada 2: Transporte USB HCI (`bt_usb_transport`)            |
|  - Mapeamento EP0 (Control), Interrupt IN, Bulk IN/OUT        |
|  - Conversão Endianness (Big-Endian PPC <-> Little-Endian USB)|
|  - Fila de comandos e despacho para thread de trabalho        |
+---------------------------------------------------------------+
|  Camada 1: HAL USB Xbox 360 (`xbox360_usbd_hal`)              |
|  - Resolução de ordinais `xboxkrnl.exe` (`Usbd*`)             |
|  - Descoberta/Binding de dispositivo (`0xE0/0x01/0x01`)      |
|  - Wrappers `UsbTrb` / `UsbControlTrb` + Callbacks DPC        |
+---------------------------------------------------------------+
```

### Por que esta é a escolha ideal?
1. **Isolamento de risco e portabilidade:** A Camada 3 (HCI puro) pode ser compilada e testada unitariamente no **Linux** antes mesmo de ir para o Xbox 360.
2. **Thread-safety no Kernel:** Callbacks do `UsbdQueueAsyncTransfer` executam em contexto de interrupção/DPC. A Camada 2 copia os bytes recebidos para um ring buffer e acorda uma thread de sistema (`ExCreateThread` com `EX_CREATE_FLAG_SYSTEM`), evitando travamentos e respeitando as restrições de ponto flutuante/memória em DPCs.
3. **Evolução natural do protótipo:** No **Protótipo 1**, o plugin não precisa de UI complexa — ele pode gravar um log detalhado em `Usb:\bt_diag.log` (e `DbgPrint` / `XNotifyQueueUI` na tela da Aurora com o resultado do `HCI_Read_Local_Version_Information`), ou responder a um app de diagnóstico simples em `Aurora`.
4. **Caminho para uso em jogos:** Por já nascer como plugin de sistema carregado pelo DashLaunch no `XeUnshackle`, a mesma base servirá sem reescrita quando futuros módulos de controle (`XamInput`) forem adicionados.

---

## 3. Interface de Conexão (Inspirada na Interface Original do Xbox 360)

Para permitir que o usuário gerencie adaptadores, faça varredura (`Inquiry` / Scan), pareie e conecte/desconecte dispositivos sem editar arquivos manualmente, a Arquitetura Híbrida se divide em **dois binários complementares**:

1. **Serviço Residente (`bt360_plugin.xex` — Plugin DashLaunch):**
   - Permanece em execução em segundo plano (Aurora e jogos).
   - Gerencia o hardware USB, a pilha HCI/L2CAP, reconexão automática e persistência de dispositivos pareados em `Usb:\bt_devices.ini` (ou `Hdd:\`).
   - Expõe um canal de controle IPC leve (ex.: bloco de memória compartilhada / objeto de sincronização no kernel ou socket loopback) para comandos de UI: `GET_STATUS`, `START_SCAN`, `STOP_SCAN`, `PAIR_DEVICE`, `CONNECT_DEVICE`, `DISCONNECT_DEVICE`, `FORGET_DEVICE`.

2. **Aplicativo Gerenciador (`BTManager.xex` — Executável na Aurora):**
   - Lançado sob demanda a partir da **Aurora** (sem consumir memória de vídeo quando você estiver jogando).
   - Visual **inspirado na interface original do Xbox 360 (NXE / Guide)**: fundo escuro translúcido, acentos em verde Xbox (`#107C10` / `#9BC848`), abas/cards limpos em 720p e dicas de botões no rodapé (`A` Conectar/Parear, `Y` Buscar Dispositivos, `X` Esquecer, `B` Sair).
   - Navegável com o próprio controle do Xbox 360 (ou teclado USB), construído com a camada gráfica open-source do `xenosworks/xsdk` (`gpu.h` / `text.h` ou `imgui-xenon`), sem uso de SDK proprietário.
