# khdays-vita: resumo da sessão (0.7.22 a 0.7.41) e o que checar depois

Branch `FEAT-1.0`, PR [ggdrn/KH-DAYS-VITA#10](https://github.com/ggdrn/KH-DAYS-VITA/pull/10). Versão atual: **0.7.41** (test build).

## Como buildar

- **Mudou o decomp** (`patches/decomp.patch`):
  ```
  git pull
  ./setup.sh
  python3 tools/rewrite_decomp.py
  python3 configure_vita.py --with-game --rom ../days.nds
  ninja
  ```
- **Mudou só o port** (`platform/`): `git pull` e `ninja`.
- **Se o link falhar com símbolo do port faltando:** `rm -rf build/decomp` uma vez e repita a sequência completa (corrigido no `setup.sh` da 0.7.27).

## Resolvido e confirmado no jogo

| Assunto | O que foi feito | Versão |
|---|---|---|
| HUD em widescreen | `hud = 2`: o 2D fica em 4:3 e comandos, HP e chain vão para as bordas da tela | 0.7.22 |
| Câmera com o analógico direito | Não gira mais sozinha atrás de uma caminhada de lado (`camera_follow = 0`, como no acampamento) | 0.7.22–0.7.23 |
| Flash branco do Possessor (issue 9) | Removido sempre, sem opção no menu: o véu de tela cheia e a explosão de polígonos translúcidos novos durante 3 s após a morte | 0.7.23–0.7.25 |
| Ruído alto no áudio do acampamento | Cada alarme do som dispara quantas vezes venceu em cada passo de mixagem (antes, um no máximo); os anéis de stream deixam de tocar blocos não preenchidos | 0.7.32 |
| Layout novo | Tela 4:3 à esquerda, na altura toda (725x544), com a outra pequena na faixa ao lado (`layout = left` / `left_bottom`) | 0.7.30 |
| Tela única antes do jogo | Logos e menus do título numa tela só, com o menu à esquerda sobre a imagem `ux0:data/khdays/menu_bg.png` ou `.jpg` (stb_image) | 0.7.31, fixo na 0.7.33 |
| `setup.sh` | Atualiza `build/decomp` sozinho quando a árvore só tem o patch anterior aplicado | 0.7.27 |

## Troca de personagem (partner swap, experimental)

**Fechada por enquanto.** O que vale hoje:

- **Ativação:** um toque no HUD do parceiro dá o parceiro da missão para o jogador; o Roxas passa a ser guiado pela IA de parceiro.
- **HP:** cada um mantém o seu. Se o parceiro controlado cai, o controle volta para o Roxas como ele estava (0.7.26).
- **Deck:** os registros dos dois são trocados. Deck, itens, magias, habilidades e stats do Roxas valem para o parceiro controlado, e o gasto sai do deck do Roxas (0.7.15, 0.7.29).
- **Arma:** o parceiro usa os stats da arma equipada no Roxas, mas mantém o próprio tipo de arma. Com o tipo do Roxas os ataques ficavam sem animação (0.7.31, 0.7.34).
- **Fica de fora (arriscado demais):** o modelo da arma e os combos e animações da arma do Roxas. A reconstrução de arma travou o jogo da 0.7.11 à 0.7.24.
- **Troca de zona:** só conta como "parceiro sumiu" depois de ~5 s fora de qualquer zona. Antes, a troca no meio da mudança de zona repetia a entrada ou travava (0.7.28).
- **Comandos travados e Limit inutilizável:** a run do Limit (`Ov022_StepRun`) marcava o remetente com o índice do parceiro (máscara 2 contra 1 da sessão) e ficava presa no estado 3. Com isso o deck ficava bloqueado até o fim da missão. Agora ela usa o slot 0 da sessão, e uma run presa é liberada (0.7.36). **Confirmado: comandos voltaram.**
- **Limit do Roxas na IA:**
  - Bloqueado enquanto ele está na IA (0.7.36).
  - A flag 44 (o pedido de Limit da IA, que dispensa segurar o botão) é limpa a cada troca. Antes, o Limit disparava sozinho ao voltar para o Roxas (0.7.38).
- **Entrar numa zona nova com o parceiro:** o controle volta para o Roxas quando a entrada termina, porque os inimigos não apareciam (0.7.37). O log tem a linha `swap: zone X -> Y (Roxas in zone Z)`.
- **Troca de posição do HUD (barra grande ↔ pequena): não implementada.**
  - O jogo fixa slot, retrato e HP máximo de cada barra quando cria o HUD (`Ov002_CreateAndRestoreHud`, tabela `anSlotForId`).
  - Trocar no meio da missão exige recriar o HUD, a mesma reconstrução em tempo real que travava antes.
  - Redirecionar só o HP deixaria o rosto e o máximo errados.

## Desempenho

- **4º núcleo:** é do sistema do Vita; o port já usa os três núcleos disponíveis.
- **Shader cache:** já existe (`.gxp` em cache por hash).
- **Picos a cada 2 s:** eram a medição da GPU do modo debug (`glFinish` a cada 60 quadros). **Com `debug = 0` melhorou (confirmado).**
- **Ritmo do modo 30 fps:**
  - **0.7.39:** diagnóstico `display: 30 fps pacing:` no log a cada 600 quadros: quadros novos por troca, VBlanks entre trocas e VBlanks entre quadros do jogo.
  - **0.7.40:** a 30 fps a tela espera o próximo quadro do jogo (até ~17 ms), para pegar cada um logo depois de pronto. A linha `waited for the game's frame` mostra as esperas.
  - **0.7.41:** a contagem passou a usar o quadro realmente desenhado. A medição anterior lia o contador depois da troca e inflava os "0 novos".

## O que checar depois

1. **Ritmo a 30 fps (0.7.41):**
   - **Teste:** jogar um trecho com `debug = 1` e olhar a linha `30 fps pacing`.
     - **Esperado:** cerca de 0/600/0/0 em "0/1/2/3+ new game frames" e quase tudo em 2 VBlanks entre trocas.
     - **Se os "0" continuarem altos:** a espera não resolve; investigar a fase entre jogo e tela.
     - **Se "VBlanks entre trocas" com 3 ou mais subir:** a espera está atrasando a tela; reduzir o limite ou desligar a espera.
   - **Dizer também se o engasgo melhorou na sensação.**
   - **Ideia pendente:** com o debug ligado, a medição da GPU ainda gera picos. Dá para movê-la para um modo de diagnóstico próprio.
2. **Troca de personagem: confirmar no jogo.**
   - O Limit do Roxas não dispara sozinho ao voltar (0.7.38).
   - O Limit continua utilizável segurando o botão, com e sem troca.
   - Os inimigos aparecem depois da volta automática ao entrar numa zona (0.7.37). Ver no log se o Roxas estava na mesma zona do parceiro (`Roxas in zone Z`); se não estava, a câmera pode voltar para trás.
   - O parceiro controlado consegue usar o próprio Limit.
3. **Diagnósticos a remover quando a troca estiver estável:**
   - `deck: actor ...` (`Ov022_UpdateCommandInput`).
   - `swap: commands held 3 s` (`Ov022_RefreshSlotRows`).
   - `swap pickup: ...`.
4. **Engasgos do 2D (menores, até ~20 ms):** o ajudante do 2D divide o núcleo 2 com o áudio. Avaliar mudá-lo de núcleo se os engasgos persistirem com `debug = 0`.
5. **Carregamentos:** os `hitch:` de 100 a 1100 ms aparecem em trocas de área e no começo das missões (leitura do cartucho e montagem da cena). Não fazem parte do ritmo normal, mas podem ser investigados à parte.
