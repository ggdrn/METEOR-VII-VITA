# Widescreen 16:9 nativo (sem esticar): estudo de viabilidade

Estudo de 29/10/2026, feito sobre a v0.09.00 e a leitura da `libjni_ff7.so`
(v1.0.29). As fases 0 a 3 foram implementadas em seguida (seção 8), ainda
sem teste no Vita.

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

---

## 8. Implementação (fases 0 a 3)

Código em `source/ff7_wide.c`, com ganchos em `ff7_screen.c`,
`ff7_fieldbg_core.h`, `ff7_bg.c`, `ff7_world.c`, `reimpl/gl_queue.c` e
`utils/glstats.c`. **Ainda não testado no Vita.**

### Como ligar

Menu do port (L + R + Start) → aba **GAMEPLAY** → `SCREEN < 4:3 / 16:9 >`.
Vale na hora (o jogo aplica na próxima área de desenho) e fica salvo em
`cheats.txt` (`widescreen 1`). Desligado por padrão.

### O que a leitura da `.so` mostrou (e mudou o plano)

- O framebuffer fora da tela **já tem 1024×512** (`VIDEO_init`: largura =
  escala << 10, altura = escala << 9); o jogo só usava 640×480 dele. Não foi
  preciso recriá-lo: o desenho largo ocupa 848×480 dele.
- `VIDEO_setOrthoMatrix` monta a matriz **e** o `glViewport` a partir do
  mesmo retângulo. O gancho troca (0, 640) por (−104, 744) e refaz o viewport a
  partir de x 0, com 848 pixels.
- A cena 3D chega em pixels de tela (`C_006A8002` divide por w e manda x, y
  em pixels). Por isso, diferente do FFNx, **não é preciso mexer na matriz
  de projeção**: alargar o ortho já mostra mais cena sem distorcer.
- O jogo não usa `glScissor`. O vídeo (`AVI_draw`) e a restauração do ortho
  no `VIDEO_update` passam pelo mesmo gancho.
- O FFNx (driver do FF7 de PC) tem widescreen para o mesmo `ff7.exe` 1.02, e
  os endereços batem com os da `.so` (`0x6438F6`, `0x639252`, `0x75F263`…).
  Ele serviu de **mapa** do que o jogo recorta. Nenhum código foi copiado (o
  FFNx é GPL-3): cada ponto foi achado e conferido no ARM recompilado.

### Fase 0: quadro largo e medição

| Peça | Como |
|---|---|
| Quadro de 848×480 | gancho em `VIDEO_setOrthoMatrix` (`ff7_wide.c`) |
| Parte mostrada | `ff7_screen.c` escolhe as coordenadas de textura do quad final: 640 centrais em menus, vídeos, transições e minigames; mais em campo, batalha e mapa-múndi |
| `glReadPixels` (swirl da batalha) | lê os 640 centrais (`+104` em x) |
| Medição | a cada 5 s: `widescreen: mode N, field W px; Q full-width quads widened/s; P% of sampled vertices beyond 0..640 px` |

Com 16:9 ligado em menus e vídeos, a imagem tem que ficar **idêntica** à de
4:3. Se não ficar, o problema está no quadro largo, não nos módulos.

### Fase 1: batalha

- Mostra 833 pixels de jogo (472 linhas, o mesmo zoom de antes).
- Fades e flashes de tela cheia: a fila GL (`gq_glDrawArrays`) alarga, na
  cópia dos vértices, todo quad de 4 ou 6 vértices com x exatamente 0 e 640,
  uma cor só e sem textura (um texel só). Pega também o fade do campo e do
  mapa-múndi, sem um gancho por função.
- Nada mais foi alterado na batalha: o que aparecer nas laterais é o que o
  jogo já desenha. A linha `P% of sampled vertices beyond` diz quanto é.

### Fase 2: mapa-múndi

- Mostra 847 pixels (480 linhas).
- A projeção nativa de `0x75F263` (`ff7_world.c`) usa uma área de recorte
  104 pixels mais larga de cada lado (as globais `0xE2C424`/`0xDE68F8` não
  mudam: elas também dão o centro da tela). O autoteste continua comparando
  com a área original.
- Se a projeção nativa estiver desligada (`world_off.txt` ou páginas não
  contíguas), o recorte volta a 640 e as laterais ficam vazias.

### Fase 3: campo adaptativo

- A cada quadro de campo (`bg_native`, thread do jogo), a meia largura da
  câmera vira `min(198, (direita − esquerda) / 2)` do intervalo de câmera do
  cenário (`[0xCFF454]` +0x0C/+0x10). Abaixo de 160 o campo fica em 4:3. As
  24 constantes 160 de `0x6438F6` e `0x643628` são reescritas quando o valor
  muda (log `widescreen: field camera half width N`).
- A janela de tiles do fundo (`0x64085C`, nativo) cresce o mesmo tanto.
- O recorte de modelos 3D (`0x639252`: −40 / +400) cresce 44 dos dois lados.
- A parte mostrada é `4 × meia largura` pixels (até 791). Um cenário estreito
  mostra menos e fica com bordas pretas menores.
- Só funciona com o fundo nativo e o zoom de campo ligados
  (`fieldbg_off.txt`, `natives_off.txt` ou `field_zoom_off.txt` deixam os
  campos em 4:3).

### Ainda não tratado (pela lista do FFNx)

- Campo: camadas de parallax 3/4 (`0x641…`, cenários com fundo que rola em
  velocidade própria), cursor da mão, rolagens de script que passam do
  limite da câmera, posição máxima das janelas de diálogo.
- Mapa-múndi: nuvens (`0x7547A6`) e o recorte de malhas em
  `0x751EFC` (terreno pode faltar nas laterais).
- Batalha: efeitos de tela cheia **com textura** de algumas invocações
  (Bahamut ZERO, Ifrit, Neo Bahamut, Typhon…) e o swirl de entrada, que
  continua com 640 pixels.
- Minigames: ficam em 4:3 de propósito.

### O que observar no teste

1. Menus, vídeos e tela-título com 16:9 ligado: idênticos aos de 4:3.
2. Campo largo (ex.: ruas do Setor 7): laterais com fundo, personagens
   entrando pela borda sem "aparecer do nada", câmera parando na borda da
   arte. Log: `field camera half width`.
3. Campo estreito: bordas pretas, nada de fundo faltando.
4. Batalha: cenário nas laterais, fades cobrindo a tela toda, janelas no
   centro. Log: `% of sampled vertices beyond`.
5. Mapa-múndi: terreno até a borda; céu e nuvens provavelmente cortados.
6. Trocar 4:3 ↔ 16:9 no meio do jogo.

## 9. Correções após o teste da v0.09.01 (v0.09.02)

O teste no Vita relatou campo e batalha pretos, com o mapa-múndi visível e
uma linha azul no horizonte. O log mostra que a lógica e os envios de
geometria continuaram ativos durante o problema.

- **Projeção:** o argumento direito de `ortho_hooked` se chamava `r`.
  `SO_CONTINUE` também declara `int r` e, no caminho que preserva uma matriz
  diferente de 640×480, seu inicializador lia essa variável local ainda não
  inicializada em vez do argumento. O gancho agora usa `right_bits`, mantendo
  os seis argumentos originais nesse caminho. O teste de regressão reproduz
  a largura corrompida na versão anterior e passa na corrigida. A confirmação
  de que isso resolve todas as telas pretas depende do próximo teste no Vita.
- **Horizonte:** a geometria do céu (`0x754100`) ainda usava um plano de
  −180 a +180. As instruções ARM que constroem suas coordenadas horizontais
  agora usam −240 e +240 em 16:9. A projeção, a altura e as cores do céu são
  preservadas; ao voltar a 4:3, as duas instruções originais são restauradas.
  O patch só é habilitado se os bytes originais conferirem. Nuvens continuam
  como uma limitação separada.

Validação local: `python3 scripts/test_wide.py` verifica o encaminhamento
dos seis argumentos em matrizes de alturas diferentes, o viewport largo,
o caminho 4:3 e os fades. Também executa no Unicorn as instruções ARM
produzidas pelo código real de alternância para conferir −240/+240 e a
restauração byte a byte de −180/+180 da `.so`. O build e o verificador do
VPK passam. A imagem final ainda precisa de validação no hardware.

## 10. Enquadramento e preenchimento após os prints da v0.09.02 (v0.09.03)

Os prints mostram a cena e o HUD deslocados à esquerda, com uma faixa vazia
à direita, e as nuvens presas ao retângulo central. O gancho da matriz só
reconhecia a altura de 480 linhas e mantinha o indicador de quadro largo
mesmo ao receber outra matriz. A composição final podia então amostrar o
quadro como se ele tivesse 104 pixels de margem interna, sem essa margem
ter sido desenhada pela matriz mais recente.

- O reconhecimento agora inclui áreas verticais menores dentro das 480
  linhas. A origem e a altura vertical do viewport são preservadas; somente
  a faixa horizontal cresce. Matrizes incompatíveis limpam o indicador de
  quadro largo. O log registra as primeiras 24 combinações de matriz/modo
  para conferir as chamadas reais no próximo teste do Vita.
- Somente os modos 1, 2 e 3 usam a matriz larga. Menus, save/load, resultados,
  transições e minigames usam a matriz original. O compositor também exige
  **modo 1** para aplicar o zoom do campo: a marca de tempo do último fundo
  não pode mais ampliar o menu de save durante a troca de modo.
- A cena de batalha usa a largura adicional; as coordenadas do HUD permanecem
  em 0..640, com escala uniforme e centro em x=480 da tela do Vita. O ajuste
  corrige a combinação de matriz e recorte que podia deslocar a imagem. Não
  foi adicionada arte nova nem preenchimento artificial de buracos internos
  de malhas de batalha; eventuais recortes de geometria ainda precisam de
  confirmação visual no console.
- Campos largos mostram a arte extra já selecionada pela câmera e pela
  janela de tiles. Campos estreitos também ocupam a largura toda, ajustando
  horizontalmente a imagem disponível, sem amostrar a área além dessa arte.
  As limitações anteriores das camadas de parallax continuam valendo.
- A rotina de nuvens `0x754493` mantém a conversão original e amplia os quatro
  vértices TL que ela acabou de gerar, em torno de x=320, por 848/640. Altura,
  UVs, cores e profundidade não mudam. Isso amplia horizontalmente os blocos
  existentes de nuvens para a nova proporção. Nenhum dado de terreno ou HUD
  passa por essa transformação. Falhas de alocação não alteram o bloco
  anterior; a reutilização do buffer no quadro seguinte considera sua geração.

Testes locais:

- `scripts/test_wide.py`: projeções de alturas diferentes e restauração do
  ortho, seleção de modos, nuvens (inclusive reutilização e falha do buffer),
  fades e instruções ARM do céu.
- `scripts/test_screen.py`: compositor real com um quad de 960×544; verifica
  os extremos da tela, os UVs, o centro e a escala do HUD de batalha, saves e
  demais menus com a marca de fundo ainda recente, e prioridade de FMVs.
- `scripts/emu/test_wide_cloud.py`: executa a conversão original da `.so` no
  Unicorn, simulando apenas a alocação e a consulta de capacidade do renderer;
  confirma os quatro vértices de 32 bytes no endereço apontado por buffer+0x70.

Compilação e verificação do VPK passam. Estas verificações não substituem o
novo teste visual no Vita, especialmente para recortes de batalha e parallax.

## 11. HUD, diálogos e segunda camada de fundo (v0.09.04)

O teste da v0.09.03 confirmou o mapa-múndi. Seus ajustes de terreno, céu e
nuvens foram preservados. O novo log identificou viewports locais da HUD
(como x=320..322, y=388..390 durante a animação de abertura) que ainda eram
colocados no framebuffer sem os 104 pixels de deslocamento usados pela cena.

- Janelas locais conservam **matriz, largura, altura e recorte originais**;
  apenas a origem horizontal do viewport recebe +104 enquanto a cena está
  larga. Isso alinha comandos, submenus, mão e diálogos ao HUD original de
  640 pixels, centralizado. O indicador do framebuffer largo continua válido
  durante essas janelas. Menus fora dos modos 1/2/3 continuam em 4:3.
- Foi removido o esticamento horizontal do composto final dos campos,
  introduzido como fallback na v0.09.03: ele também afetava os diálogos.
  Cenário e caixas usam escala uniforme. A janela de tiles da camada nativa
  cresce 38 unidades por lado mesmo se a câmera tiver um intervalo estreito.
- A segunda camada (`0x640C49`) ainda recortava em 4:3. Ela agora usa o mesmo
  acréscimo de 38 unidades: o argumento x recebe +38 temporariamente, a origem
  passa de 320 a 358 para compensar esse deslocamento, e a largura usada no
  limite esquerdo passa de 336 a 412. As posições dos tiles não mudam; o
  argumento do chamador e as instruções originais são restaurados quando
  apropriado. As instruções são verificadas antes da instalação.
- O viewport 3D da batalha (640×332) passa a permitir desenho até a linha
  480 em widescreen, mantendo a relação de um pixel projetado por pixel do
  framebuffer. Isso deixa passar geometria já enviada abaixo do recorte.
  A detecção de fades exige agora que o quad comece no topo e cubra pelo
  menos 400 linhas: um painel preto inferior da HUD conserva sua largura
  original. Se o jogo não enviar geometria para a área inferior, ela fica
  preta; não foi criado preenchimento artificial.
- DEBUG voltou a ser uma aba normal, sem exigir `debug_menu.txt`.

Validação: testes dos viewports locais e da preservação de seus seis
argumentos em todos os modos; HUD inferior não classificado como fade;
caixas de diálogo com escala uniforme; argumento da segunda camada
restaurado após a chamada; decodificação das instruções produzidas pelo
patch e restauração dos bytes originais. `scripts/emu/test_wide_field2.py`
executa `0x640C49` original e modificado: novos tiles aparecem nas duas
laterais e todas as posições originais permanecem iguais. Os testes de
nuvens anteriores continuam passando. O resultado visual precisa do teste
no Vita; ausência de arte e camadas de parallax específicas podem continuar
exigindo tratamento por cenário.

## 12. Transição de entrada da batalha (v0.09.05)

Com 16:9 selecionado, o compositor apresenta o modo 23 (swirl) em toda a
largura de 960 pixels, vindo tanto do campo quanto do mapa-múndi. O efeito
continua sendo produzido em 640 pixels e é esticado horizontalmente apenas
na apresentação final. A amostragem conserva esses 640 pixels: não tenta
revelar laterais que o módulo de transição não desenha. Também reconhece o
quadro ainda largo na troca de modo, amostrando nesse caso os 640 centrais.

A opção 4:3 mantém a apresentação anterior. HUD, diálogos, menus e o renderer
do mapa-múndi não recebem o esticamento da transição. O teste do compositor
cobre as duas origens, os dois layouts do framebuffer na troca de modo e a
opção widescreen desligada.
