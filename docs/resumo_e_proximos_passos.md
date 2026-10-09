# khdays-vita: resumo do trabalho e próximos passos

Versão atual: **0.5.0** (tag `v0.5.0`, notas em `docs/releases/v0.5.0.md`). Versão anterior publicada: **v0.4.0**.

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

### Outros
- **Tela única:** os painéis passam a seguir o zoom da câmera, a visão aproximada do Select (0.1.24).
- **Release v0.4.0:** publicado com tag no GitHub e changelog em `docs/releases/v0.4.0.md`.
- **Release v0.5.0:** notas em `docs/releases/v0.5.0.md`.

## Pendências e plano de ação (depois da 0.5.0)

| Prioridade | Item | Ação |
|---|---|---|
| 1 | Validar a 0.5.0 no Vita | Possessor sem flash; botões do Vita em todas as telas visitadas; Confirm button nos dois modos; Mission balance |
| 2 | Engasgos ao entrar em área nova | Pior quadro: ~70 texturas, ~11–17 ms de decodificação + envio à GPU. Otimizar `texels_smooth` (duas passadas, 9 vizinhos) e espalhar o envio por 2–3 quadros durante o fade |
| 3 | Ícones do DS restantes | Varrer telas ainda não vistas (loja, tutoriais `UI/btlttr`, resultados de missão, manual `UI/mnl`) com dumps; acrescentar ao gerador |
| 4 | Vídeos a ~51 fps | Entrega de quadros nos vídeos (swap ~19 ms): travar a tela no ritmo do vídeo, como no modo 30 fps |
| 5 | Folga de CPU no 60 fps | Batalhas cheias: jogo 70–80%, vídeo 45–50%. Medir o envio do 3D (3–4 ms/quadro) e reduzir |
| 6 | 2D a 60 fps | Interpolar a rolagem dos fundos 2D que acompanham a câmera |
| 7 | Áudio, mais um passo (opcional) | PCM direto em 16 bits e soma com NEON (estimativa 1,5–2×) |
| 8 | Distribuição | Auditoria que falhe o build se bytes da ROM entrarem no ELF; atualizar o `ROADMAP.md` |
