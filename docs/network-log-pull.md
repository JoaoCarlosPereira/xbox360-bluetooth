# Como puxar logs do console pela rede

Instruções operacionais para a sessão **Xbox 360 Bluetooth driver** (e qualquer outra).

## Pré-requisitos

- Console com Aurora aberta após ABadAvatar + XeUnshackle
- HD Aurora com `plugin2 = Usb:\Aurora\Plugins\FtpDll.xex` no `launch.ini`
- Mesma LAN do PC (Ethernet preferível)

## Credenciais

| Campo | Valor |
|---|---|
| Porta | **21** |
| User / pass | **`xboxftp` / `xboxftp`** |
| Volume FTP | **`Usb0`** (= `Usb:\` do HD Aurora) |

`xbox`/`xbox` **não** funciona (resposta `332 User not OK`).

## Comando rápido

```bash
cd /mnt/Dados/dsv/xbox360-bluetooth
./scripts/pull-console-logs.sh 192.168.2.184   # ou o IP atual (Aurora → Back → System info)
```

Arquivos baixados em `build/console-logs-…/`:

- `bt360_trace.log`
- `bt360_diag.log`
- `btmanager.log`
- `launch.ini`
- `bt360_scan.bin`

## Caminhos no FTP

- Logs: `/Usb0/bt360_trace.log`, `/Usb0/bt360_diag.log`
- Plugin/UI: `/Usb0/Apps/BTManager/`
- Firmware: `/Usb0/rtl8761bu_fw.bin`

## Deploy remoto

`put` dos `.xex` em `/Usb0/Apps/BTManager/` → **reboot** do console para carregar. Sem hot-reload de plugin.

## Troubleshooting

- Porta 21 fechada → Modules → FTP Server enabled na Aurora
- Ping falha e FTP sobe → normal no 360
- LIST/RETR em modo ativo timeout → usar **PASV**
- Isolamento Wi‑Fi entre clientes → Ethernet ou desligar AP isolation

Ver também: `docs/feasibility-network-diag.md`
