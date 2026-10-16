# Plano: interfaces de tela única vindas do khdays-port

Objetivo: levar para o Vita o modo de tela única do khdays-port, do início (logos, título) até a
interface dentro da missão: acampamento, painéis/inventário, menu de missão (seletor de mundos),
seleção de personagem/multijogador, campo, pausa e resultados.

Referência copiada em [`assets/onescreen_ref/`](../assets/onescreen_ref/README.md).

## Como o khdays-port faz

São duas camadas independentes.

### 1. Composições (base, sem imagem nenhuma)

O port não redesenha os menus. Ele **recorta pedaços das duas telas do DS** e os monta numa
imagem só, com uma regra por situação do jogo.

1. **Situação** (`port/ui/situation_take.c` + `situation_decide.c`, ~540 linhas). A cada VBlank
   copia do jogo:
   - a cena (`gSceneCtl`);
   - os overlays carregados;
   - quais funções de update de objetos rodaram (`situation_updates.h`);
   - `gPauseMode`, a tela do acampamento, o tutorial, o painel de início de missão, o status do
     evento (ov023) e o contexto do menu do título (nível, linha, idioma);
   - POWCNT1, as trocas do motor A, os registradores de brilho.

   Com isso decide uma situação e seus estados:
   - situações: `intro`, `title`, `load-menu`, `save-menu`, `settings`, `camp-menu`, `shop`,
     `world-selector`, `field`, `field-3d-both`, `pause`, `tutorial`, `mission-review`,
     `game-over`, `day-counter`, `thoughts`, `the-end`;
   - estados: `hud`, `dialog`, `info-new`, `mission-panel`, `party-4`, `event`…
2. **Composição** (`port/ui/composition.c`, 1.731 linhas). Lê `compositions.txt`
   ([`port_days.txt`](../assets/onescreen_ref/compositions/port_days.txt)). Para cada situação, o
   arquivo diz:
   - o fundo (`screens upper|lower|side-by-side`) e de quem é o brilho;
   - as `shape`s: um retângulo de uma tela, um canto de destino, margem, escala e efeitos (`key`,
     `see-through`, `fade`, `dim`, `round`, `mirror`, `repeat`);
   - as `shape3d` (o 3D do HUD reposicionado).
3. **Passo de GPU** (`port/gfx/onescreen.h` + `shaders/onescreen.hlsl`). Para cada texel da
   imagem final, a primeira shape que o cobre e tem texel visível fornece a cor. É C puro com
   `#define`s, compartilhado com o shader.

O que cada menu usa hoje no port, sem o pack HD:

| Tela | Situação | Composição |
|---|---|---|
| Logos e título | `intro`, `title` | as duas telas lado a lado |
| Lista de saves (título) | `load-menu` | tela de baixo no centro em 1:1, a de cima atrás cobrindo e escurecida (0,35) |
| Acampamento: menu, Painéis (inventário), status | `camp-menu` | lado a lado |
| Loja do moogle | `shop` | lado a lado |
| Salvar / Preferências | `save-menu`, `settings` | tela de cima no centro, a de baixo atrás escurecida |
| Menu de missão (Holo-Missões) | `world-selector` | o 3D dos mundos inteiro; o cabeçalho e o nome do mundo em recortes; a tela de baixo inteira à direita, com cantos arredondados, de tamanho conforme o aspecto (`wide`) |
| Seleção de personagem (Modo Missão) | cena 7 → `title` | lado a lado |
| Campo (missão) | `field` | HUD em recortes: painel da missão (topo esq.), mapa e HP do inimigo (topo dir.), deck (baixo esq.), grupo (baixo dir.), faixa INFORMATION temporária, 3D do contador de corações |
| Pausa | `pause` | menu no centro e mapa, painel e INFORMATION recortados (exceto na pausa de evento) |
| Tutorial | `tutorial` | página no centro, campo escurecido atrás |
| Resultados | `mission-review` | lado a lado |

### 2. Telas HD (opcional, por cima)

Com o mod `HD PACK`, o port desenha algumas telas por conta própria (`port/host/screens/*.c`) com
peças das coleções HD, lendo o estado dos menus do jogo (`port/runtime/menus/*.c`):

| Tela | Desenho | Estado lido | Gerador | Fonte das peças |
|---|---|---|---|---|
| Título (imagem animada) | `composition.c` (`backdrop *.txt`) | contexto do título | `theater_title.py` | 1.5+2.5, teatro de Days |
| Diálogos de New Game | idem | `newgame.c` | `newgame_dialog.py` | 2.8 (DDD) + KH2 (luva) |
| Lista de saves | `save_screen.c` | `savelist.c` | `save_screen.py` | DDD |
| Menu do acampamento e Preferências | `camp_screen.c` | `camp.c` | `ddd_camp.py` | DDD |
| MODO CINE | `cine_screen.c` | `cinelist.c` | `theater_cine.py` | teatro |
| Salas do multijogador | `room_screen.c` | `runtime/net/roomlist.c` (rede própria do port) | `theater_mission.py` | teatro |
| Seleção de personagem | `select_screen.c` | `charselect.c` | `theater_mission.py` | teatro |
| Configurações | `host/settings` | — | `theater_settings.py` | 1.5+2.5 SettingMenu |
| Pausa | `ui/pause.cpp` | — | `hd_pause.py` | fontes do Re:Collection |

Não existe tela HD para Painéis (inventário), loja ou seletor de mundos: esses ficam sempre na
composição.

### Assets

- **Composições:** são texto e só usam pixels da ROM. Copiados.
- **Fonte das telas próprias:** a da ROM (`text/font_eu_10.nftr`). Nada a copiar.
- **Imagens HD:** não existem no repositório. São geradas na hora a partir das cópias do usuário
  (lista exata em `installer/hd_inputs.txt`), e as coleções não estão neste Mac. Os geradores
  foram copiados. **Nunca entram no VPK.**

## Onde o Vita está hoje

`platform/hw/game.c` (~l. 690–725) decide a tela única com booleanos próprios:

- `single`: campo com ov022;
- `world_select`: `kh_mission_select_pages`, ov006;
- `menus`: ov000;
- `single_scene`: tela de baixo preta por 30 VBlanks;
- `tutorial`: DISPCNT do motor B.

`video.c` desenha painéis configuráveis (`kh_config.panel[4]`) e um fundo do usuário no título
(`menu_bg.png`). Funciona, mas cada tela é um caso especial, e acampamento, loja, listas,
resultados e pausa não têm layout próprio.

## Status (branch `feat_one_screen`)

Fases 1 a 3 implementadas e compiladas (VPK gerado). **Ainda não foram testadas no console.**

Para ligar: no port menu, **Single screen (beta) → Compositions**, ou `single_screen = 2` no
`config.ini`. O modo 1 (painéis) continua como estava.

| Peça | Arquivo | O que faz |
|---|---|---|
| Situação | `platform/ui/situation.{c,h}` | Regras do `situation_decide.c` do port, com os mesmos nomes de situações e estados |
| Entradas | `platform/hw/situation_take.{c,h}` | Lê `gSceneCtl`, `gPauseMode`, overlays, acampamento (`data_ov008_02090f0c`, `data_ov025_020b574c`), tutorial, painel da missão, evento, grupo e logos, conferindo cada ponteiro na arena (`kh_is_game_heap`, `nitro/os.c`) |
| Hook | `patches/decomp.patch` (`Obj_UpdateAll.c`) | Marca as 5 funções de update observadas (lista de saves, loja) |
| Composições | `platform/ui/composition.{c,h}` | Leitor do formato do port: `screens`, `brightness`, `shape` com todos os efeitos, `shape3d hud`, `hide3d`, `backdrop` (PNG estático), `menu`, `stop`, condições `wide`/`narrow` |
| Arquivo | `platform/ui/compositions/port_days.txt` | O do port, sem mudanças; vai no VPK como `app0:layouts/compositions.txt`. `ux0:data/khdays/layouts/compositions.txt` o substitui |
| Pixels e cola | `platform/hw/onescreen.{c,h}` | Estados por pixel do port (`info-lower`, `info-new`, `mission-panel`, conteúdo das telas), decisão, composição, log `one screen: vblank N, <situação>: <estados>` |
| Desenho | `platform/core/video.c` | O chão (3D sozinho sobre o backdrop, ou as duas telas inteiras), as shapes num shader próprio (do último ao primeiro), o backdrop PNG e o toque mapeado de volta pela shape que está por cima |
| HUD 3D | `platform/hw/gx3d.c`, `gpu3d.c` | Polígonos de projeção ortográfica marcados (`flat`) e reposicionados pelo `shape3d hud`, ou escondidos pelo `hide3d hud` |

Diferenças em relação ao port:

- **`shape3d all`** (a janela do modelo no acampamento): não implementado.
- **Imagens animadas** (`backdrop *.txt`) e nomes com `{lang}`: ignorados até a Fase 4. As
  seções que dependem delas são puladas, e o título cai no lado a lado.
- **HUD 3D:** o port marca os polígonos que o próprio HUD envia. Aqui vale "projeção
  ortográfica", e só os que cabem inteiros no retângulo da `shape3d` são movidos. O
  `hide3d hud` só esconde os pequenos (menos de 1/4 da tela).
- **Brilho:** cada shape usa o brilho da própria tela. As regras `upper`/`lower`/`halves` não
  são aplicadas, porque o 2D da tela sem 3D já sai do `gpu2d` com o brilho aplicado.
- **Tamanho do HUD:** o fator é `hud_size / 100` (60–100 %). O port vai até 1,75.

O que conferir no console, em ordem:

1. **Logos, título e lista de saves:** lado a lado, e a lista no centro com a tela de cima
   escurecida atrás.
2. **Acampamento, Painéis, loja e Preferências:** lado a lado. Testar o toque nos painéis.
3. **Holo-Missões:** o mundo inteiro, o cabeçalho, o nome do mundo e a lista à direita.
   Testar o toque na lista.
4. **Seleção de personagem (Modo Missão):** lado a lado.
5. **Missão:**
   - o HUD nos cantos;
   - o contador de corações (3D) abaixo do painel da missão;
   - a faixa INFORMATION por 5 s;
   - o mapa e o painel recortados;
   - diálogos, pausa, tutorial, resultados e game over.
6. **Desempenho:** a linha de GPU no log com `debug = 1`, e o motor B desenhado em quadros
   alternados no campo.

## Plano de ação

### Fase 0: referência ✅
- Copiado para `assets/onescreen_ref/`: composições, geradores HD, documentação, lista de
  entradas e licença.

### Fase 1: detector de situação ✅ (falta testar no console)
1. Criar `platform/ui/situation.{c,h}` portando `situation.h`, `situation_decide.c` e a parte de
   `situation_take.c` que lê o estado (sem SDL nem threads do port).
2. Entradas que o Vita precisa fornecer:
   - **cena**: `gSceneCtl` +8 e +0xc;
   - **overlays**: já existe `kh_overlay_loaded`;
   - **updates que rodaram**: hook em `Obj_UpdateAll` no `decomp.patch`, sob `PLATFORM_VITA`,
     marcando as 6 funções de `situation_updates.h`;
   - **pausa**: `gPauseMode`;
   - **acampamento**: `khdays_camp_screen` de `runtime/menus/camp.c`;
   - **tutorial e painel de missão**: `data_ov002_0207f9fc` e `data_ov002_0207f614`;
   - **evento**: `data_ov023_0208a784`;
   - **título**: o contexto de `Ov000_RefreshMenuLayout`;
   - **3D nas duas telas**: `engine_swaps`, que já existe como `dual`.
3. **Risco: versão do decomp.** O port fixa `a2b0478b`, o Vita usa `ea39ead5`. Para cada
   símbolo e offset da lista acima, conferir no decomp do Vita (o `RUNTIME_MAP.md` do port, seção
   15, documenta cada um).
4. Log `situation: X -> Y` quando muda (como `KHDAYS_TRACE_SITUATION`), para validar jogando.
5. Substituir os booleanos de `game.c` pela situação, mantendo o comportamento atual por trás da
   config `single_screen = 1`.

### Fase 2: motor de composição no vitaGL ✅ (falta testar no console)
1. Portar o leitor de `composition.c` (só a parte de texto/shapes, sem os `backdrop` animados).
   O arquivo é lido de `ux0:data/khdays/layouts/compositions.txt` se existir; senão, do
   `port_days.txt` embutido no VPK (`app0:`).
2. Portar o passo de pixel de `gfx/onescreen.h` (`KO_STEPS`) para um fragment shader CG no
   vitaGL:
   - entradas: as duas texturas de 256×192 e o FBO do 3D;
   - as shapes vão como uniform array (limitar a ~24).
   - `gfx/compose_cpu.c` serve de referência para testar no Mac (`tools/`), como já se faz com
     o gpu2d.
3. **Unidades:**
   - a imagem tem C × 192 pontos DS, com C = 192 × 960/544 ≈ 338,8 (16:9: `wide`);
   - o fator de HUD fica numa config `hud_size` (como `installer/default_settings.ini`);
   - a tabela de fatores está em `composition.c`.
4. **`shape3d hud`** (contador de corações): exige marcar no `gpu3d` os polígonos de HUD em espaço
   de tela. Fica para o fim da fase; até lá, `hide3d hud` + o recorte 2D.
5. `config single_screen`:
   - 0: desligado;
   - 1: modo atual (painéis);
   - 2: composições.

   O valor 2 passa a ser o padrão quando a Fase 3 fechar.

### Fase 3: telas, na ordem do pedido 🔶 (ligadas; ajustes finos dependem do console)
Cada item: ligar a situação → aplicar a seção do `port_days.txt` → testar no console → ajustar
margens e escalas para 960×544.

1. **Logos e título** (`intro`, `title`, `load-menu`): lado a lado e a lista no centro com fundo
   escurecido. Manter o `menu_bg.png` atual como `backdrop` opcional (a sintaxe já existe).
2. **Menu do acampamento** (`camp-menu`, `save-menu`, `settings`, `shop`): lado a lado ou
   centralizado. Avaliar no Vita se o lado a lado (cada tela ≈ 480×360) fica legível. Se não,
   criar seções próprias com recortes do menu e do status.
3. **Painéis / inventário**: é `camp-menu` com a tela 2 do acampamento. No Vita, o toque arrasta
   painéis na tela de baixo, então o mapeamento de toque precisa seguir a shape onde a tela de
   baixo foi parar (inverter a transformação da shape em `input.c`).
4. **Menu de missão** (`world-selector`): substituir o layout `world_select` atual pela seção do
   port (`if wide`), que já foi desenhada para 16:9.
5. **Seleção de personagem e multijogador**:
   - a seleção (cena 7) usa composição lado a lado;
   - o multijogador do port depende da rede própria dele (`runtime/net`, salas). No Vita
     corresponde a ad hoc, que ainda não existe ("⬜ wireless" no ROADMAP). Fica só a tela de
     seleção; as salas entram quando houver rede.
6. **Dentro da missão**:
   - `field`/`field-3d-both`: HUD em recortes, migrando os 4 painéis atuais para shapes;
   - `pause`, `tutorial`, diálogos (`dialog`), `mission-review`, `game-over`, `day-counter`,
     `thoughts`, `the-end`.

   Conferir a faixa INFORMATION (`info-new`) e o `mission-panel`, que no port vêm de checagem de
   pixels em `host/render/onescreen.c`.

### Fase 4: pack HD (opcional, depois da Fase 3)
1. Num PC com KH 1.5+2.5 e 2.8 do usuário:
   - extrair com OpenKh ou `installer/collection_archive.py`;
   - rodar `tools/hd/*.py` → `mods/HD PACK/layouts/`.
2. **Conversão para o Vita** (ferramenta nova, `tools/hd_vita.py`):
   - atlas 1920×1080 → 50% (960×540 cabe em 960×544);
   - PNG → textura comprimida (DXT5) para caber na memória;
   - coordenadas mantidas, com UV escalada no carregador.
3. Renderizar, por ordem de valor:
   1. imagem animada do título (`piece`/`key`/`clip`/`loop`, ~400 linhas de `composition.c`);
   2. lista de saves;
   3. menu do acampamento;
   4. seleção de personagem;
   5. MODO CINE.

   O estado de cada menu vem de `runtime/menus/*.c`, que precisa dos mesmos símbolos do decomp.
4. Ícones de botão: os geradores já extraem a família `playstation` do `Icon.tm2` do teatro, o
   que encaixa direto no Vita (trocar `button` de mouse por toque).
5. Copiar para `ux0:data/khdays/mods/HD PACK/layouts/`. **Nunca no VPK nem no repositório.**

### Fase 5: validação
- Para cada situação, uma captura (o dump L+R+Triângulo) comparada com a do port no PC.
- Log de situação durante uma sessão completa: título → nova partida → acampamento → painéis →
  Holo-Missões → missão → pausa → resultado → Modo Missão → seleção.
- Medir o custo do shader de composição: fill de 960×544 com até ~24 shapes. A meta é menos de
  1 ms de GPU.

## Riscos
- **Símbolos e offsets do decomp diferentes entre as versões.** Mitigação: conferir cada um na
  Fase 1.
- **Legibilidade a 960×544** das telas lado a lado: os menus têm texto pequeno. Pode exigir
  seções próprias do Vita.
- **Toque:** precisa seguir a posição da tela de baixo em cada composição.
- **Memória** dos atlas HD (Fase 4): medir antes de carregar todos os idiomas. Carregar só o
  idioma ativo.
