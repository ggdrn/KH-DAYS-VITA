# khdays-vita: resumo do trabalho e próximos passos

Versão atual: **0.7.0** (release, tag `v0.7.0`, notas em `docs/releases/v0.7.0.md`). Release anterior: **v0.5.0**.

## O que foi feito

### Gráficos 3D
- **Cabelo com buracos (Larxene, Marluxia, Xion):** os texels opacos de texturas A3I5/A5I3 passaram a gravar profundidade, como no DS. A causa foi confirmada por um dump de polígonos (0.1.25).
- **Polígonos opacos:** usam um shader sem `discard`, o que deu mais folga à GPU no campo (0.1.18).
- **Tentativa revertida:** a 0.1.21 juntava três mudanças (thread extra para ordenar o 3D, encaixe de partículas e a correção do cabelo) e causou glitches. Foi revertida na 0.1.22, e a correção do cabelo voltou sozinha na 0.1.25.

### Desempenho
- **Contador de FPS:** custava ~10 ms por quadro, porque era redesenhado e reenviado em todo quadro e o vitaGL copiava a textura inteira a cada envio. A thread de vídeo caiu de 65–90% para 5–25% do núcleo (0.4.0).
- **Texturas das telas:** passaram a usar um anel de 6, para que o vitaGL não as copie antes de atualizar (0.1.25).
- **30 fps em paralelo:** o 2D é desenhado ao longo de dois quadros também a 30 fps (0.4.0).
- **Mixer de áudio:** reorganizado por canal e em blocos, com ponto fixo, inteiros e ADPCM decodificado uma vez por amostra (0.4.2). Validado no Mac pelas 37 músicas: diferença máxima de 4/32768 e 1,7× mais rápido no Mac. O ganho no Vita ainda não foi medido.
- **Ferramentas de medição no log:** CPU por thread, tempos da entrega de quadro e estatísticas da interpolação.

### Vídeos (Mobiclip)
- **Congelamento nas cutscenes (relato do Reddit):** um ajudante do 2D segurava um pedaço do trabalho enquanto o jogo ocupava o núcleo. Corrigido na 0.4.1.
- **Leitor de bits e compensação de movimento do decodificador:** reescritos de forma mais rápida e testados contra a versão antiga, com resultados idênticos (0.4.1).
- **Espera ativa dos players de vídeo:** dormem até a próxima interrupção. O núcleo do jogo caiu de 100% para ~10% durante os vídeos (0.4.3 e 0.4.5).

### 60 fps (experimental, desligado por padrão)
- **0.4.6:** mistura modelo a modelo, por grupos de material e tamanho, em vez de exigir que a lista de vértices batesse desde o começo.
- **0.4.7:** fração calculada pelo vblank; pareamento também exige as mesmas coordenadas de textura. Resultado: ~300 de 300 quadros interpolados a cada 10 s e 98% dos vértices pareados.
- **0.4.8:** as regras de salto só valem para grupos pequenos (partículas).
- **0.4.9:** cada modelo é pareado com o candidato mais próximo do mesmo tipo (peças do mapa não esticam mais); texturas que rolam (água, céu) passam a ser interpoladas na posição.
- **0.4.10:** pedaços pequenos e opacos do cenário sempre interpolados (a regra de salto ficou só para partículas translúcidas); a fração da mistura usa o contador de VBlank do hardware. **Testado: tremor bem menor.**

### 30 fps
- **0.4.9:** tentativa de segurar a imagem por dois VBlanks pelo contador da thread de VBlank: deu 20 trocas/s (o contador chega atrasado).
- **0.4.10:** intervalo de troca 2 na fila de exibição do vitaGL: a tela fica em 30 exatos (log: 1200 VBlanks para 600 quadros, 29,9 fps). O 2D de cada quadro é desenhado junto com o 3D. **Testado: estável.**

### Controles
- **0.4.11:** com a mira travada, o analógico direito troca de alvo (o mais próximo à esquerda ou à direita, a mesma função do L/R do controle Type B). **Testado: funcionando.**

### Áudio
- **0.4.12:** medição por etapa: a mixagem era ~2,2 s de cada 10 s (quase todo o custo de ~25% de um núcleo), por decodificar ADPCM amostra a amostra dentro do laço.
- **0.4.13:** as ondas ADPCM do banco de som são decodificadas uma vez (cache, decodificado logo à frente do que toca). Saída idêntica bit a bit nas 37 músicas no Mac. **Testado no Vita: áudio de ~25% para ~12%.**

### Texturas
- **0.4.12:** a checagem das texturas (hash da VRAM) passou para os dois núcleos; log por etapa. Pior quadro ao entrar em área: ~11–17 ms de decodificação para ~70 texturas, o gargalo restante.

### Modo Missão
- Na Missão o HP do inimigo é ×3 (feito para 4 jogadores, solo também) contra ×0,8 na história, e o golpe de inimigo leva ×1,5 e as regras da partida.
- **0.4.13–0.4.18:** opção **SYSTEM → Mission balance = Story**. Foram testados ×1,5 e ×2,35; ficou o padrão da história: HP ×0,8 (o da história) e dano recebido pela dificuldade do save (Standard para convidado).

### Botões do Vita na tela (0.4.16–0.4.25)
- **Texto:** os glifos A/B/X/Y das fontes (0x3349/0x3314/0x3322/0x334d e a segunda cópia 0xE000–0xE003 das fontes "all") são trocados na leitura da ROM (`nitro/button_glyphs.c`).
- **Sprites e texturas:** HUD de batalha (`UI/btl/main.p2`), painel e menu de acampamento (`UI/cm/cm.p2`, `UI/cm/cmo_*.p2`, inclusive as linhas de ajuda "Grab Panel / Exit") e o aviso "Y-COMBO" (textura 3D). O gerador `tools/button_sprites.py` lê a ROM do usuário e grava só hashes dos tiles originais e os pixels novos; o port aplica ao descomprimir (`nitro/button_sprites.c`).
- **Confirmar com ×:** opção **Confirm button**: A do DS no × e B na ○, em todo o jogo; os ícones A/B acompanham.
- **Ferramentas:** o dump (L+R+Triângulo com `debug = 1`) grava as texturas também em bytes brutos e avisa no log quando terminou.

### Flash branco do Possessor (0.4.16–0.4.18)
- Rastreamento por derrota no log mostrou ~150 polígonos translúcidos a mais (a explosão). Implementada a regra do DS: pixel translúcido não é desenhado sobre outro do mesmo ID de polígono (bit 7 do stencil).

### Luta com 3D nas duas telas (Sora em Olympus, 0.5.1–0.5.5)
O jogo alterna o 3D entre as telas a cada VBlank (cada tela a 30 Hz).
- **0.5.1–0.5.2, one screen:** os frames da tela de baixo são descartados na hora e só os de cima são trocados na tela.
- **0.5.3:**
  - o one screen desliga sozinho nos primeiros 10 s da luta (Sora na tela de baixo) e volta depois;
  - o motor B deixa de ser desenhado nos frames de baixo (de 9,4 ms para 0);
  - cada toggle guarda o próprio 3D e os próprios registradores, e o 3D não pula mais de tela.

  **Testado:** jogo de 51 para 60 VBlanks/s.
- **0.5.4:** em duas telas, a tela volta a ser travada em 2 VBlanks por troca. O log mostra ~97% das trocas exatas.
- **0.5.5, tremor do cenário só em duas telas:** o fator de widescreen do 3D era um valor só, trocado pelo display a cada toggle, e o jogo montava frames com a largura da outra tela. Agora cada frame usa o fator da tela para a qual é montado. **Testado: resolvido** (log: 1 frame trocado só no início da luta).

### Desempenho e vídeos (0.5.6–0.6.9)
- **0.5.6, leitura da ROM:**
  - cache de 16 MiB com leitura antecipada em outra thread;
  - durante o jogo, quase nenhuma leitura no cartão (antes até ~900 ms a cada 10 s);
  - linha `hitch:` no log para os engasgos do lado do jogo.
- **0.5.7, menu de pausa:** o 2D da captura (desfoque) é desenhado em faixas nos dois núcleos, junto com o 2D do quadro. De 47 quadros lentos para 1.
- **0.5.8–0.5.9, medições:**
  - a 60 fps, quadros atrasados separados entre CPU e GPU;
  - modo de teste de GPU (`debug = 3`): a soma de 3D em 3x e composição passava de uma VBlank.
- **0.6.0, resolução dinâmica do 3D a 60 fps:** desce de 3x até 2x em passos de 0,5x enquanto há atrasos e volta a subir depois.
- **0.6.1, composição:** coordenadas vindas do vertex shader (sem leituras dependentes) e compilador de shaders em O3 com matemática rápida.
- **0.6.2, vídeos:** a 30 fps também no modo 60.
- **0.6.3–0.6.9, vídeos em wide sem as faixas pretas** (removido na 0.7.1, a pedido):
  - corte medido por vídeo (16+16 linhas, mais 4 de folga);
  - legendas desenhadas sobre a imagem, 28 px acima da borda;
  - a tela de baixo preta fica oculta.

**Testado: ok.**

### Áudio e release (0.7.0–0.7.2, release 0.7.0)
- **0.7.0/0.7.1:**
  - laços de mixagem sem testes de fim longe do fim da onda;
  - limitador em inteiros abaixo do joelho;
  - saída em blocos maiores.
- **0.7.2:** mixagem a 32 kHz (a do DS) na porta BGM, com o sistema convertendo para 48 kHz. Áudio de 13% (0.6.9) para ~8% de um núcleo; validado nas 37 músicas no Mac.
- **Release 0.7.0:** inclui tudo acima e o detector de engasgo sem o tempo do port menu.

### Outros
- **Tela única:** os painéis passam a seguir o zoom da câmera, a visão aproximada do Select (0.1.24).
- **Release v0.4.0:** publicado com tag no GitHub e changelog em `docs/releases/v0.4.0.md`.
- **Release v0.5.0:** notas em `docs/releases/v0.5.0.md`; versão VitaDB em `docs/releases/vitadb_v0.5.0.md`.

## Pendências e plano de ação (depois da 0.7.0)

Estado na 0.7.0:
- sessão de 19 min sem erro;
- entrada em área sem engasgo visível;
- 30 fps exatos;
- 60 fps com 2–11% de quadros atrasados nas lutas mais cheias;
- vídeos a 30 fps;
- áudio a ~8% de um núcleo.

| Prioridade | Item | Ação |
|---|---|---|
| 1 | Missões mais pesadas | Núcleo do jogo a 80–87% (ov277): engasgos de 50–80 ms. Levar o processamento dos comandos 3D (gx3d, na thread do jogo) para NEON ou para outra thread |
| 2 | Outras cenas com 3D nas duas telas | Conferir chefes e cutscenes dual 3D fora de Olympus |
| 3 | 60 fps: custo restante | Envio do 3D duas vezes por quadro do jogo e ~4 ms de CPU no swap do vitaGL; interpolar a rolagem dos fundos 2D |
| 4 | Distribuição | Auditoria que falhe o build se bytes da ROM entrarem no ELF; atualizar o `ROADMAP.md` |
