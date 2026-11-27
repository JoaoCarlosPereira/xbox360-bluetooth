# Bluetooth no Xbox 360

Este projeto liga um DualShock 4 no Xbox 360 por um adaptador Bluetooth USB. O console passa a enxergar o controle como um controle de Xbox: a Aurora e os jogos recebem os botões, os analógicos, os gatilhos e a vibração. Até dois DualShock 4 funcionam ao mesmo tempo. O controle original do Xbox, quando está ligado, continua no jogador 1.

A pasta [`xbox/`](xbox/) já traz os arquivos prontos. Não é preciso compilar nada para usar no console.

## O que dá para fazer

- Parear um DualShock 4 pelo aplicativo Bluetooth e usá-lo na Aurora e nos jogos.
- Jogar com dois DualShock 4 ao mesmo tempo, cada um num jogador.
- Guardar até quatro DualShock 4. Nos próximos boots, o controle gravado volta ao apertar PS.
- Manter o controle original no jogador 1 e deixar os DualShock nos jogadores seguintes.
- Encaminhar a vibração que o jogo pede para o DualShock.
- Pedir o guia do Xbox pelo botão PS.
- Achar qual controle é qual: LB e, no DualShock, segurar SHARE.
- Trocar o jogador, desconectar ou esquecer um controle pela tela Bluetooth.
- Desligar sozinho um DualShock parado por 15 minutos. O botão PS liga de novo.

## Requisitos

- Xbox 360 com ABadAvatar, XeUnshackle, DashLaunch e Aurora. O teste foi feito no kernel retail `2.0.17559.0`.
- Disco USB em FAT32, o mesmo de onde a Aurora abre.
- Adaptador Bluetooth USB padrão (`0xE0/0x01/0x01`).
  - **Validado no Xbox 360:** UGREEN `2B89:8761` (Realtek RTL8761BU).
  - **Suporte genérico ativo (em validação pela comunidade):** dongles baratos Plug & Play sem firmware externo (CSR8510 / clones `0A12:0001`, Barrot `33FA:0010`, Actions `10D7:B012`) e outras marcas com chip Realtek RTL8761B/BU/A ou RTL8821C (como TP-Link UB500, Orico, Baseus e genéricos `0BDA:8771`). Adaptadores Intel (`8087:*`) não são suportados.
- Um ou dois controles DualShock 4.
- O controle original é opcional depois do pareamento. Na tela nativa de perfil ("Entrar") ele ainda é o caminho seguro: essa tela está em confirmação nesta versão.

Deixe de fora, ao mesmo tempo, o hiddriver360 e o X360Remap. Os dois também injetam controle e disputam a mesma leitura.

## Arquivos prontos

Baixe a pasta [`xbox/`](xbox/) deste repositório. São quatro arquivos:

| Arquivo | Tamanho | Para onde vai no Xbox |
| :--- | ---: | :--- |
| `bt360_proto.xex` | 139264 | `Usb:\Apps\BTManager\` |
| `BTManager.xex` | 1310720 | `Usb:\Apps\BTManager\` |
| `rtl8761bu_fw.bin` | 44484 | raiz de `Usb:\` |
| `rtl8761bu_config.bin` | 6 | raiz de `Usb:\` |

Os dois `.bin` são o firmware público do RTL8761BU (`rtl_bt/rtl8761bu_fw.bin` e `rtl8761bu_config.bin` do linux-firmware), sem alteração. O plugin procura o firmware primeiro em `Usb:\bt_fw\` e depois na raiz `Usb:\` (`\Device\Mass0\`). Adaptadores que não precisam de firmware externo (CSR8510, Barrot, Actions) funcionam sem copiar os `.bin`.

O `bt360_proto.xex` sobe com o console e fica residente. O `BTManager.xex` é o aplicativo "Bluetooth" que a Aurora abre quando você quer parear, desconectar ou esquecer.

## Instalação

1. No computador, se o seu adaptador usar chip **Realtek** (como UGREEN RTL8761BU, TP-Link UB500 ou genéricos `0BDA:8771`), copie os `.bin` correspondentes para `Usb:\bt_fw\` (ou para a raiz do disco USB). Para adaptadores **CSR8510**, **Barrot** ou **Actions**, pule este passo.
2. Copie os dois `.xex` para `Usb:\Apps\BTManager\`. Crie a pasta se ela não existir.
3. No `launch.ini` que o DashLaunch já lê, na seção `[Plugins]`, use um slot livre:

```ini
[Plugins]
plugin1 = Usb:\Apps\BTManager\bt360_proto.xex
```

Se `plugin1` já aponta para outro plugin seu, use `plugin2`, `plugin3`, `plugin4` ou `plugin5`. Mantenha os plugins que você já usa.

4. Conecte o adaptador Bluetooth USB em qualquer porta USB e deixe-o ligado.
5. Ligue o Xbox pelo ABadAvatar, como de costume, e espere a Aurora abrir.
6. Fique na Aurora por cerca de 15 segundos. O disco USB pisca uma vez: o plugin rearma o barramento USB para o kernel entregar o adaptador que já estava conectado. Espere o aviso "Adaptador Bluetooth pronto".
7. Na Aurora, abra o aplicativo Bluetooth.

O plugin só entra na memória num boot completo. Trocar o `bt360_proto.xex` pede desligar e ligar de novo. O `BTManager.xex` atualiza ao sair com B e abrir o aplicativo outra vez.

## Parear o DualShock 4

1. No aplicativo, abra a aba Buscar.
2. No DualShock 4, segure SHARE + PS até a barra de luz piscar rápido.
3. Aperte Y. A busca dura cerca de um minuto.
4. Selecione o DualShock 4 e aperte A.

O aviso "DualShock 4 pareado no Jogador N" confirma. O endereço fica em `Usb:\bt_devices.ini`, criado pelo plugin. Nos boots seguintes aperte PS no controle gravado: ele conecta sem abrir o aplicativo.

Com dois DualShock já ligados, a tela avisa "Limite de 2 controles BT ativos. Desconecte um primeiro." Desconecte um em Controles (X) para conectar outro que já esteja guardado. Um quinto controle novo é recusado.

## Limitações

- Esta versão atende o DualShock 4 em adaptadores Bluetooth USB padrão (`0xE0/0x01/0x01`). Teclado, mouse, fone, DualSense e adaptadores Intel (`8087:*`) ficam de fora.
- Dois DualShock ficam ligados ao mesmo tempo. A biblioteca guarda quatro.
- Com o controle original ligado, ele permanece isolado no jogador 1 e os DualShock ocupam os jogadores seguintes (cada controle comanda apenas o seu próprio jogador).
- A tela nativa de perfil ("Entrar") e o guia do sistema pedem tecla, e esse caminho está nesta versão ainda sem um boot de confirmação. Se o DualShock não mover essa lista, use o controle original para sair dela.
- O rearme do USB acontece uma vez, cerca de 15 segundos depois que a Aurora está na tela. Nesse instante os dispositivos USB caem e voltam. Deixe o adaptador e o disco no lugar.
- Em adaptadores Realtek que exigem patch externo, sem os `.bin` em `Usb:\bt_fw\` (ou na raiz) o plugin avisa `"Firmware do adaptador ausente no USB."`.

## Código e notas de desenvolvimento

Quem for alterar o projeto parte de [`src/`](src/README.md). O handoff do que o console já mostrou está em [`STATUS.md`](STATUS.md). A pesquisa de USB, HCI e arquitetura está em [`docs/`](docs/feasibility.md).
