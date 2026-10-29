# Widescreen 16:9 nativo (sem esticar): estudo de viabilidade

Estudo de 29/10/2026, feito sobre a v0.09.00 e a leitura da `libjni_ff7.so`
(v1.0.29). Nada foi implementado ainda.

**Conclusão:** é viável, mas não da mesma forma em todos os modos.
A parte gráfica tem um ponto único de controle (o `glOrtho` que o jogo chama
em `gfx_setviewport`), então abrir a imagem para os lados custa pouco. O
trabalho de verdade está em convencer cada módulo do jogo a **desenhar** o que
fica fora dos 640 px originais: hoje ele recorta tudo nessa largura. Ordem de
dificuldade, da mais fácil para a mais difícil: batalha, mapa-múndi,
minigames, campo. Menus e vídeos ficam em 4:3, centralizados, como em
qualquer port widescreen do FF7.

---

## 1. O que "16:9 nativo" significa aqui

A tela do Vita é 960×544, que é **30:17** (1,765), não exatamente 16:9
(1,778). "Nativo" quer dizer mostrar **mais cena nas laterais**, com a mesma
escala vertical e sem distorção, em vez de:

| Modo atual | Resultado na tela | Problema |
|---|---|---|
| Padrão fora do campo | 725×544 + bordas pretas | 235 px de borda |
| Zoom do campo (v0.5.01) | 777×544 + bordas | 183 px de borda |
| `field_stretch_on.txt` | 960×544 | imagem esticada 23% |

Larguras necessárias, no espaço de coordenadas do jogo (640×480):

| Área que preenche a altura | Largura para 960×544 | Extra por lado | Ganho de campo de visão |
|---|---|---|---|
| 480 linhas (mapa-múndi, minigames) | 847 | 103,5 | +32% |
| 472 linhas (batalha, zoom atual) | 833 | 96,5 | +30% |
| 448 linhas (campo, zoom atual) | 791 | 75,3 | +24% |

No campo, 75 px de cada lado equivalem a ~38 px da arte original de PSX
(o campo é 320 px de largura, desenhado ×2).

## 2. Como a imagem é montada hoje

Lido na `.so` (endereços ARM da `libjni_ff7.so`):

1. **`VIDEO_init`** (`0x9521D8`) cria o framebuffer fora da tela: textura
   RGBA + renderbuffer de profundidade, 640×480.
2. **`gfx_setviewport`** (`0xC33298`), chamado pelo `ff7.exe` a cada troca de
   área de desenho, **ignora** a área pedida (`fake_glViewport` é um `bx lr`)
   e chama `glOrtho(x, x + w, y + h, y)` com o retângulo da janela do jogo
   (campos `+0x848..+0x854` do objeto do jogo, 0,0,640,480).
3. **`glOrtho`** → **`VIDEO_setOrthoMatrix`** (`0x9527C0`) monta a matriz
   ortográfica com `GLKMatrix4MakeOrtho`, guarda em `orthoInfo` e ajusta o
   `glViewport` do framebuffer.
4. O jogo envia **vértices já em coordenadas de tela** (estilo
   `D3DTLVERTEX`: x, y em pixels de 0..640 × 0..480, z, 1/w). O shader
   (`assets/Shaders/Shader.vsh`) só multiplica pela matriz.
5. **`VIDEO_update`** desenha a textura de 640×480 num quad da altura da tela
   e as bordas laterais (o `ff7_screen.c` já mexe nesses dois quads).

Consequência importante: **uma única matriz** leva todas as coordenadas do
jogo para o framebuffer. Trocar o ortho por
`glOrtho(-E, 640 + E, 480, 0)` e o framebuffer por `(640 + 2E)×480` deixa
tudo que o jogo já desenha **centralizado e com o mesmo tamanho**, e qualquer
coisa que ele desenhe em x < 0 ou x > 640 passa a aparecer. Menus, janelas,
texto e cursores continuam no centro em 4:3 sem nenhuma mudança.

## 3. O problema real: o jogo recorta em 640

Abrir o ortho não faz aparecer cena nova sozinho. Cada módulo do `ff7.exe`
descarta antes o que estaria fora da tela original:

### 3.1 Batalha (modo 2): viável, risco médio

- A cena 3D passa por `C_006A8002` (triângulos com matriz, já nativo em
  `ff7_tri.c`): a projeção leva a pixels de 640×480. Com o ortho aberto, um
  triângulo que cai em x = −50 aparece na borda, **se** o jogo o enviar.
- Os cenários de batalha são modelos 3D que normalmente envolvem a câmera,
  então há arte para mostrar nas laterais.
- **A medir:** se o jogo descarta objetos/triângulos fora da tela antes de
  enviá-los (culling por objeto ou por triângulo contra a área 0..640). Se
  sim, é preciso achar e alargar esses limites.
- Janelas de batalha (comandos, HP, submenus) ficam em 4:3 no centro. Fica
  coerente, mas a barra de comandos não vai de ponta a ponta.
- Efeitos 2D de tela cheia (flashes, fades, o swirl de entrada) cobrem só
  640 px. Precisam ser esticados para a largura nova (ver 3.5).
- Custo: a batalha usa só 31–38% da thread do jogo (BATALHA-30FPS.md), então
  sobra CPU para os triângulos extras.

### 3.2 Mapa-múndi (modo 3): viável, risco médio, custo de CPU

- A projeção e o recorte já são **código nosso**: `world_core_project`
  (`ff7_world_core.h`) recebe os limites de recorte `a, b, c, d` e marca as
  flags por vértice; `ff7_world_tri.h` descarta o triângulo se nenhum
  vértice cai dentro na horizontal. Alargar `a`/`c` em E é uma mudança local.
- **A medir:** se o jogo escolhe quais blocos de terreno montar pelo cone
  de visão da câmera (0x751EFC percorre as malhas). Se escolher, as laterais
  podem mostrar o terreno sumindo, e o seletor de blocos também precisa ser
  alargado. Modelos (personagem, veículos, árvores) têm o mesmo problema.
- Custo: o mapa-múndi ainda é limitado pela CPU (~25 fps andando). Mais ~30%
  de largura significa mais triângulos aprovados no recorte; espera-se perda
  de alguns fps aqui. É o principal risco de desempenho.

### 3.3 Campo (modo 1): o mais difícil, depende da arte

- O fundo é arte pré-renderizada em blocos (tiles). `0x64085C` (nativo em
  `ff7_fieldbg_core.h`) só põe na lista os tiles que caem dentro da janela;
  dá para alargar essa janela.
- **O limite é a arte:** muitos cenários têm fundo do tamanho exato da tela,
  ou a câmera para de rolar exatamente onde a arte acaba. Nesses, as laterais
  mostrariam preto ou tiles faltando. Não há como inventar essa arte, e o
  VPK não pode levar dados do jogo nem arte derivada.
- Os personagens e objetos 3D do campo usam a mesma projeção; vão aparecer
  nas laterais se o jogo não os recortar.
- Os limites de rolagem da câmera (o jogo para de rolar na borda do fundo)
  continuam calculados para 320 px de PSX.
- Caminho realista: **decidir por cenário**. Onde o fundo cobre a área extra
  na posição atual da câmera, mostrar 16:9; onde não cobre, ficar no zoom
  atual (777×544) ou preencher só o que existe. Isso pode ser calculado em
  tempo real (extensão dos tiles da camada × posição da câmera) em vez de
  uma lista feita à mão.
- **A medir primeiro (offline):** para todos os cenários do OBB, a largura
  do fundo comparada com a área de rolagem. Esse número decide se o 16:9 no
  campo vale o esforço.

### 3.4 Menus, vídeos e telas 2D: ficam em 4:3

- Menu principal, lojas, tela de título, saves: arte 2D feita para 640×480.
  Com o ortho aberto, ficam centralizados com bordas, como hoje.
- Vídeos (VP8 640×448): 4:3. Ficam como hoje (zoom de altura, com bordas). O
  quad do vídeo é desenhado pelo jogo com coordenadas de 640, então continua
  no centro.
- O framebuffer mais largo precisa ser **limpo em preto** fora dos 640 px
  centrais nesses modos; hoje o `glClear` já cobre o framebuffer todo.

### 3.5 Coisas do port que assumem 640×480

| Onde | O que muda |
|---|---|
| `ff7_screen.c` | o quad final passa a ser a textura larga em 960×544; o zoom do campo e da batalha vira "escolha da faixa vertical" |
| `ff7_swirl.c` / `reimpl/readpixels.c` | o swirl lê a tela com `glReadPixels(0, y, 640, 1)`: lê só o começo da textura larga. Precisa de deslocamento de E px (ou ler a largura nova) |
| Objeto do jogo `+0x954/+0x958` | largura/altura de tela usados pelo jogo (limites do swirl, inversão de y no `gfx_setviewport`) — **não** devem mudar, o jogo continua achando que a tela é 640×480 |
| `ff7_interp.c` | interpola vértices em coordenadas de tela; não depende da largura |
| Toque (`UIScreenTouch`) | a interface de toque é desenhada em 960×544 pelo `VIDEO_update`; não muda |
| `ff7_tri.c`, `ff7_bg.c` | trabalham em coordenadas do jogo; não mudam |

## 4. Custo de GPU e memória

- Framebuffer de 848×480 em vez de 640×480: +32% de pixels. A GPU termina o
  quadro em 3–4 ms hoje; fica em ~4–5 ms, folga enorme para os 33 ms de 30 fps.
- Memória: +~0,8 MB (cor + profundidade). Irrelevante.
- O quad final passa de 640 para 848 texels de largura, escala uniforme de
  1,133. Sem perda de nitidez em relação ao zoom atual.

## 5. Plano proposto

Cada fase é pequena e testável sozinha. Tudo atrás de uma opção no menu do
port (Gráficos → Proporção: 4:3 / 16:9), desligada por padrão até a fase 3.

**Fase 0 — medições, sem mudar a imagem (1 build)**
- Framebuffer e ortho largos, mas com o quad final ainda cortando os 640 px
  centrais: a imagem fica idêntica à atual.
- Log por modo de jogo: quantos vértices caem em x < 0 ou x > 640 por quadro.
  Se for ~0 em batalha e campo, o jogo recorta antes e precisamos achar onde;
  se não for, a cena extra já vem de graça.
- Script offline (`scripts/`) que lê os cenários do OBB do próprio usuário
  e lista largura do fundo × área de rolagem por cenário.

**Fase 1 — batalha 16:9**
- Mostrar a área larga na batalha; alargar recortes encontrados na fase 0;
  esticar efeitos de tela cheia; corrigir o swirl.

**Fase 2 — mapa-múndi 16:9**
- Alargar `a`/`c` no `world_core_project`; investigar o seletor de malhas;
  medir fps andando, de chocobo e de avião.

**Fase 3 — campo 16:9 adaptativo**
- Alargar a janela de tiles; decidir por quadro se o fundo cobre a área
  extra; fallback para o zoom atual. Testar câmeras que rolam.

**Fase 4 — minigames**
- Um a um (moto, snowboard, montanha-russa, submarino, chocobo, Fort
  Condor). Cada um tem seu próprio renderizador; vários já são nativos no
  port. Os que não funcionarem ficam em 4:3.

## 6. Riscos

| Risco | Impacto | Mitigação |
|---|---|---|
| O jogo recorta objetos fora de 0..640 em lugares ainda não achados | objetos "aparecem" ao entrar na borda | fase 0 mede; achar os limites como foi feito no mapa-múndi |
| Fundos de campo estreitos | preto nas laterais | modo adaptativo com fallback para 4:3 com zoom |
| Mapa-múndi mais lento | perda de fps | é opcional; medir antes de ligar por padrão |
| Efeitos 2D de tela cheia (fades, flashes) só em 640 px | faixa sem efeito nas laterais | detectar quads que cobrem 0..640 inteiro e esticá-los para a largura nova |
| Janelas 4:3 numa cena 16:9 | aparência menos "nativa" | aceitável; é o que fazem os mods de PC |

## 7. Veredito

| Modo | Viável? | Esforço | Observação |
|---|---|---|---|
| Batalha | Sim | médio | depende do recorte do jogo (fase 0) |
| Mapa-múndi | Sim | médio | recorte já é código nosso; custo de CPU |
| Campo | Parcial | alto | limitado pela arte de cada cenário |
| Minigames | Caso a caso | médio a alto | |
| Menus, vídeos | Não (fica 4:3) | nenhum | centralizados, como hoje |

O primeiro passo concreto é a **fase 0**: um build que não muda nada na
imagem e responde, com números, as duas perguntas que decidem o resto (o
jogo envia o que fica fora da tela? quantos cenários têm fundo largo o
suficiente?).
