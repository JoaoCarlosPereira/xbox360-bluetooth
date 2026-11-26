# Estudo de Viabilidade — Diagnóstico por Rede (residente com Aurora)

> Segmento paralelo ao `bt-usb-hci`: eliminar o ciclo “ejetar HD → ler log no PC”.
> Requisitos do usuário: telemetria em **segundo plano com a Aurora**, cobrindo **todos os logs** do plugin; ouro seria **acesso tipo SSH** ao sistema do console.

---

## 1. Problema atual

Hoje `bt_trace` grava em:

- `\Device\Harddisk0\Partition1\bt360_trace.log`
- `\Device\Mass0\bt360_trace.log`
- `\Device\Mass0\bt360_diag.log`

(limite 2 MB/arquivo). Para analisar, o HD/USB precisa voltar ao PC. Isso atrasa o debug do estágio pós-firmware (`adapter_stage=3`) e de qualquer falha em boot.

Ambiente alvo: **Xbox 360 E + ABadAvatar + XeUnshackle + Aurora**, kernel `17559`. Rede local já foi usada no passado (ponte WDS/Wi‑Fi frágil); cabo Ethernet é fortemente recomendado.

---

## 2. O que “SSH” significa aqui (e o que existe de verdade)

| Expectativa | Realidade no Xbox 360 retail |
| :--- | :--- |
| Shell POSIX `ssh user@console` | **Não existe** homebrew SSH/OpenSSH para 360. O projeto [AnimaSSH](https://github.com/kwsimons/AnimaSSH) é **Xbox One/Series**, não 360. |
| Shell remota + filesystem | O substituto de facto é **XBDM** (TCP/UDP **731**): protocolo texto estilo FTP/SMTP, com clientes [XeShell](https://github.com/hyperbx/xeshell/), Neighborhood, Py-XBDM, xecli. |
| Transferência de arquivos em background | **FtpDll** (plugin DashLaunch, porta **7564**) ou FTP da Aurora (porta **21**, só enquanto a Aurora/módulo FTP está ativo). |

Conclusão: o “ouro SSH” no 360 é **XBDM + shell no PC (XeShell)** e/ou **FTP residente**. Não há caminho curto para OpenSSH no kernel retail.

---

## 3. Alternativas existentes (pesquisa)

### A. FtpDll (plugin DashLaunch) — pull sob demanda

- Plugin clássico do cOz: FTP **enquanto Aurora e jogos rodam**.
- Porta **7564**, user/pass tipicamente `xbox`/`xbox`, modo **ativo**.
- Encaixa no XeUnshackle (`launch.ini` → `[Plugins]`).
- **Não é stream em tempo real**: o PC baixa `bt360_trace.log` / `bt360_diag.log` quando quiser (curl/FileZilla/`lftp`).
- Esforço: **baixo** (instalar + testar rede). Risco: plugin extra em RAM; histórico de conflito de load address com alguns plugins antigos (v3 mudou endereço).

### B. FTP da Aurora / DashLaunch `ftpserv`

- Aurora: módulo FTP porta 21 (só com Aurora no ar).
- DashLaunch `ftpserv`: FTP enquanto o próprio DashLaunch está ativo — **não** cobre a sessão Aurora/jogo da mesma forma que FtpDll.
- Útil como fallback; **FtpDll é a peça certa** para “junto com Aurora”.

### C. XBDM (+ XeShell / Neighborhood / xecli) — “quase SSH”

- Plugin `xbdm.xex` (muitas builds Bad Update/XeUnshackle já o listam).
- Porta **731**: descoberta UDP, controle TCP (arquivos, reboot, launch, peek/poke).
- [XeShell](https://github.com/hyperbx/xeshell/): prompt no PC com `cd`, `download`, `upload`, `peek`, etc. — a experiência mais próxima de “SSH” no ecossistema 360.
- [BetterXBDM](https://github.com/theaperturecat/BetterXBDM) declara compatibilidade com Bad Update.
- Cuidado: relatos de **crash do stage2 ABadAvatar** com certos `xbdm` em builds BadUpdate 1.3 — validar no hardware antes de depender.
- Esforço: **baixo/médio** (ativar plugin + cliente no PC Linux/Windows). Não transmite `bt_trace` sozinho; baixa o arquivo ou usa DbgPrint se habilitado.

### D. `net_log` do xsdk (UDP) — stream em tempo real **de títulos**

- Já no toolchain do projeto: `research/repos/xsdk`.
- Título coloca `net_log.txt` (`IP_DO_PC porta`) ao lado do `.xex`; PC roda `tools/net_log.py` (UDP **7371**).
- Cada linha → 1 datagrama UDP com sequência; título **não bloqueia** se o PC estiver offline.
- Hoje isso está em `libxsdk` / `log.c` para **títulos** (`BTManager.xex`), **não** em `libxsdk_plugin` (`xsdk_plugin_log` só grava arquivo).
- Esforço para o **app**: **muito baixo**. Para o **plugin residente** (onde nascem os logs críticos USB/firmware/pair): **médio** — espelhar o protocolo UDP dentro de `bt_trace` (ou portar o trecho de `log.c` + `socket`/`NetDll_*` para o plugin).

### E. Servidor SSH próprio no plugin

- Portar OpenSSH/Dropbear para PowerPC + NetDll + auth + PTY: **esforço alto**, superfície de risco grande, pouco retorno vs XBDM+FTP+UDP.
- **Não recomendado** neste segmento.

### F. libxenon / XeLL / Linux no 360

- Rede rica (lwIP), mas **outro ambiente de boot**, incompatível com Aurora + plugin Usbd do retail `17559` no fluxo ABadAvatar atual.
- Fora de escopo.

---

## 4. Matriz (requisito: residente com Aurora + logs)

| Opção | Sobe com Aurora? | Tempo real? | “Tipo SSH”? | Esforço | Encaixa no projeto |
| :--- | :---: | :---: | :---: | :---: | :--- |
| FtpDll | Sim | Não (pull) | Não (só FS) | Baixo | **Sim — ganho imediato** |
| Aurora FTP | Só na Aurora | Não | Não | Baixo | Parcial |
| XBDM + XeShell | Sim (plugin) | Não (comandos) | **Mais próximo** | Baixo/médio | **Sim — shell remota** |
| net_log no BTManager | Só com o app | Sim (UDP) | Não | Muito baixo | Útil para UI |
| UDP mirror em `bt_trace` (plugin) | **Sim** | **Sim** | Não | Médio | **Sim — telemetria alvo** |
| OpenSSH no 360 | — | — | Ideal | Muito alto / inviável | Não |

---

## 5. Recomendação (segmento em camadas)

### Fase 0 — rede estável (pré-requisito)

Confirmar IP fixo/DHCP estável do 360 na LAN do PC de desenvolvimento (**Ethernet**). Sem isso, nenhuma opção acima vale o tempo.

### Fase 1 — acesso a arquivos sem ejetar (1–2 horas de setup)

1. Adicionar **FtpDll** em `launch.ini` (slot livre, sem atropelar `bt360_proto`).
2. No PC: `lftp`/`curl`/`FileZilla` → baixar `bt360_trace.log` / `bt360_diag.log` a qualquer momento com Aurora aberta.
3. Opcional: script `scripts/pull-console-logs.sh` que baixa e arquiva em `build/console-logs-…`.

Isso sozinho já remove o ciclo físico do HD.

### Fase 2 — “quase SSH” (opcional, paralelo)

1. Validar `xbdm.xex` estável no boot ABadAvatar/XeUnshackle desta máquina.
2. Usar **XeShell** (ou Neighborhood no Windows) para `download`/`upload`/`launch` sem FTP.
3. Não substituir FtpDll se o XBDM for instável no Bad Update; tratar como bônus.

### Fase 3 — telemetria em tempo real do plugin (entrega do requisito)

Estender `bt_trace` no **plugin residente** (já carrega com a Aurora):

1. Ler config tipo `Mass0:\bt_net_log.txt` → `IP porta` (mesmo formato do xsdk).
2. Abrir socket UDP via **NetDll** / API de sockets do xsdk **se** linkável no `libxsdk_plugin`; senão, imports diretos `xam.xex` `NetDll_*`.
3. Em cada linha já escrita no disco, espelhar o datagrama do protocolo `net_log` (4 bytes BE sequência + texto) para reutilizar `tools/net_log.py`.
4. Buffer ring + flush na thread do plugin (nunca em DPC/USB callback) — mesmo cuidado do restante da pilha BT.
5. Disco permanece fonte da verdade; rede é best-effort (perda de linha = gap de sequência).

Resultado: PC com `python research/repos/xsdk/tools/net_log.py --out build/net_log` vê o boot USB/firmware/pair **ao vivo**, sem abrir o BTManager.

### Fase 4 — ouro (só se ainda faltar)

Mini-serviço TCP no plugin (“btctl”) com comandos `tail`, `status`, `pull` — ainda **não** é SSH, mas cobre o dia a dia. Só depois das fases 1–3.

---

## 6. Riscos

- **Rede do console**: Wi‑Fi 2.4 / WDS já falhou neste ambiente; Ethernet primeiro.
- **RAM/plugins**: FtpDll + XBDM + `bt360_proto` competem por slots e memória; testar um a um.
- **Sockets em plugin**: `xsdk_plugin_log` hoje não faz rede; a Fase 3 é o único trabalho de código relevante.
- **Live / bans**: manter liveblock; serviços FTP/XBDM só na LAN.
- **USB reset do próprio projeto**: se o plugin derrubar o Mass0, FTP e log em Mass0 somem até o volume voltar — manter espelho no `Harddisk0\Partition1` (já feito) e preferir puxar pelo HD via FtpDll.

---

## 7. Veredito

| Pergunta | Resposta |
| :--- | :--- |
| Dá para telemetria em background com Aurora? | **Sim.** Plugin DashLaunch (já é o modelo do `bt360_proto`) + espelho UDP de `bt_trace`, e/ou FtpDll para pull. |
| Dá para SSH de verdade? | **Não no 360.** O substituto pragmático é **XBDM + XeShell**. |
| Menor caminho de valor? | **FtpDll agora** → **UDP `bt_trace` depois** → XBDM se o boot aceitar. |
| Vale um segundo segmento de P&D? | **Sim**, desacoplado do pareamento DS4: desbloqueia o debug do segmento USB/BT. |

---

## 8. Referências

- [FtpDll Digiex](https://digiex.net/threads/ftpdll-0-3-xbox-360-ftp-server-runs-in-the-background-on-the-dash-and-in-game.9524/)
- [ConsoleMods — Homebrew Apps / ftpdll / xbdm](https://consolemods.org/wiki/Xbox_360:Homebrew_Apps_List)
- [DashLaunch — ConsoleMods](https://consolemods.org/wiki/Xbox_360:DashLaunch)
- [XeUnshackle](https://github.com/Byrom90/XeUnshackle)
- [xsdk — net_log / GETTING_STARTED §7](https://github.com/xenosworks/xsdk/blob/main/docs/GETTING_STARTED.md)
- [xsdk issue #2 — Remote XBDM](https://github.com/xenosworks/xsdk/issues/2)
- [XeShell](https://github.com/hyperbx/xeshell/)
- [BetterXBDM](https://github.com/theaperturecat/BetterXBDM)
- [AnimaSSH](https://github.com/kwsimons/AnimaSSH) — *Xbox One/Series only* (não aplicável)
