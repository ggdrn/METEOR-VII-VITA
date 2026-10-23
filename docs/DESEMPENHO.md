# Desempenho: diagnóstico e plano

Análise feita sobre os logs da v0.00.36 (`logs_anterior.txt` e uma sessão de
~35 min passando pelo Setor 7) e sobre a desmontagem da `libjni_ff7.so` v1.0.29.

## O que os logs mostram

| Situação | FPS | Thread do jogo (core 0) | Cores 1 e 2 |
|---|---|---|---|
| Campo leve | 30 (limite do jogo) | ~40% trabalho + espera ativa | ~30% |
| Setor 7 / campos pesados | **17–22** | **91–93% trabalho**, 5–7% chamadas GL, 0% ocioso | ~31% |
| Batalha | 14,8 | ~50% trabalho, dorme o resto | ~30% |

- **Campos pesados são limitados pela CPU, numa única thread.** A GPU termina
  cada quadro em ~4 ms (`GPU tail`), e as chamadas GL são só 5–7% da thread.
  O resto é o próprio código do jogo.
- **Batalhas a 15 fps são o ritmo normal do motor**: a thread dorme e faz
  espera ativa entre quadros. Não é queda de desempenho.
- Os travamentos de 300–1300 ms são carregamentos de área (leitura do OBB,
  LZSS, conversão de texturas).

## Por que "usar mais núcleos" não resolve sozinho

O jogo é o `ff7.exe` de PC (x86) **recompilado estaticamente para ARM**, com
um único conjunto de registradores x86 em memória global. É uma só CPU x86
emulada: a lógica do jogo não pode ser dividida entre núcleos sem reescrevê-la.

O app tem 3 núcleos utilizáveis. O core 0 é exclusivo do jogo; o áudio (OpenSL
+ Vorbis), o decodificador de vídeo, a troca de buffers e o resto já estão nos
cores 1 e 2, que ficam ~70% ociosos. O que ainda dá para tirar do core 0:

| Trabalho | Peso na thread do jogo (Setor 7) | Ganho possível |
|---|---|---|
| Envio de comandos GL (vitaGL) | 5–7% | ≤ 7%, exige uma thread de render com fila de comandos (complexo) |
| Conversão de texturas | 0–4% (só quando há texturas novas) | reduz picos, quase nada no caso médio |
| LZSS / leitura do OBB | só em carregamentos | reduz travadas de troca de área |

Ou seja, mais núcleos dão no máximo ~10%. O ganho grande está em fazer
**menos trabalho** no core 0.

## O gargalo real: `mk_address`

O código recompilado acessa **toda** a memória x86 (inclusive a pilha, já que
`esp`/`ebp` ficam em memória) através de `mk_address(va)`, que consulta a
tabela de páginas `MEMMAP`:

- 669.918 chamadas a `mk_address` na `.so`, 89% de todas as chamadas de
  função do binário;
- ~5,6 milhões de instruções no total → **uma chamada a cada ~8 instruções**;
- cada chamada custa ~10 instruções + desvio e retorno, com duas leituras de
  memória dependentes (GOT → tabela).

Estimativa: mais da metade das instruções executadas pela thread do jogo está
dentro de `mk_address`. Todas as otimizações nativas anteriores (iluminação,
fundo de campo, animação, cos/sin) já apontavam esse custo no profiler.

### Feito na v0.00.37 (`source/ff7_memmap.c`)

1. **`mk_address` reescrita no lugar**: o endereço da tabela vira constante
   (`movw/movt`), eliminando a leitura dependente do GOT. Resultados idênticos;
   testada na inicialização, volta ao original se divergir.
2. **Endereços constantes resolvidos uma vez**: 71.246 chamadas que acessam
   variáveis globais do ff7.exe (`movw r0 / movt r0 / bl mk_address`) passam a
   carregar o endereço final direto, sem chamada. A lista vem de análise
   estática (`scripts/gen-memmap-sites.py`, regras no cabeçalho do script) e
   cada ponto é conferido instrução por instrução antes de ser alterado.

Ganho esperado: da ordem de 10–20% na thread do jogo. **Precisa ser medido no
Vita**: criar `ux0:data/ffvii-vita/memmap_off.txt` desliga as duas mudanças,
para comparar o `perf:` / `heartbeat:` do log no mesmo lugar, com e sem.

## Próximos passos (em ordem de retorno)

1. **Medir** v0.00.37 com e sem `memmap_off.txt` no Setor 7.
2. **Perfil com `ENABLE_SPIKE_PROFILER=ON`** no Setor 7 e enviar o
   `profile.bin`: as amostras com PC dentro de `mk_address` e LR no ponto de
   chamada dizem exatamente quanto custa cada tipo de acesso (global, pilha,
   ponteiro) e quais funções recompiladas valem uma versão nativa.
3. **Acessos à pilha sem chamada** (o maior grupo, por análise estática
   aproximada: ~200 mil pontos `ebp ± constante` e ~76 mil `esp ± constante`,
   41% de todas as chamadas). Se a pilha x86 for alocada de modo que
   `host = va + K` com `K` codificável em uma instrução Thumb, cada
   `bl mk_address` desses pontos vira `add.w r0, r0, #K`. Exige controlar o
   endereço da pilha x86 (hook em `start_ff7_core`/`VA_newMapper`) e provar
   que o ponto só acessa a pilha (acessos por `esp` sim; por `ebp` só em
   funções com quadro padrão). É a mudança com maior potencial (pode chegar
   perto de dobrar o desempenho do código recompilado), mas precisa de
   testes no aparelho a cada passo.
4. ~~Thread de render (fila de comandos GL no core 1): até ~7%.~~ Feito na
   v0.00.38, ver abaixo.

## Resultado da v0.00.37

Os dois patches funcionaram no Vita: `mk_address` rápida instalada e
71.246/71.246 chamadas removidas, sem divergência. Em cenas comparáveis
(~100–120 draws por quadro), o trabalho da thread do jogo caiu ~5–10%
(ex.: ~24 → ~21–22 ms por quadro). A sessão não passou pelos trechos
pesados do Setor 7, então o número ainda é indicativo.

## Thread de render (v0.00.38, `source/reimpl/gl_queue.c`)

Na thread do jogo, as chamadas GL viram registros numa fila de 8 MB; a
thread `gl_render` (cores 1–2) executa a fila em ordem, pelos mesmos wrappers
de antes. O jogo usa só 53 funções GL, sem VBOs nem `glDrawElements`:

- `glDrawArrays` copia para a fila os vértices que vai ler (o jogo reaproveita
  o buffer logo depois); `glTexImage2D`, `glUniformMatrix4fv` e
  `glDeleteTextures` copiam seus dados.
- Chamadas que devolvem algo (`glGen*`, consultas de shader, `glReadPixels`,
  `glGetIntegerv`) esperam a fila esvaziar e rodam direto. Fora da
  inicialização, só acontecem ao carregar texturas e na captura da batalha.
- `glGetError` (chamada após quase toda troca de estado) responde
  `GL_NO_ERROR` sem esperar.
- A thread principal (que compõe o quadro em `onDrawFrame` e faz o swap)
  espera a fila esvaziar e trava a thread de render enquanto usa GL.

No log, a linha `render thread:` mostra ocupação, chamadas e draws por
segundo, dados copiados e quanto a thread do jogo esperou. Ganho esperado:
~3–5% na thread do jogo (o GL dela, menos o custo de copiar os vértices).
`ux0:data/ffvii-vita/glthread_off.txt` desliga a thread de render para
comparar.

## Vídeo de abertura com glitches (corrigido na v0.00.37)

Desde a v0.00.25 cada quadro do vídeo era copiado **por cima** da memória da
textura YUV. O vitaGL desenha o quadro na GPU depois que a CPU já seguiu em
frente, então o quadro anterior ainda podia estar sendo lido pela GPU
enquanto o novo era escrito → quadros rasgados / meio atualizados. Agora cada
quadro recebe memória nova (`glCompressedTexImage2D`) e o vitaGL só libera a
antiga quando a GPU termina de usá-la. O custo é o mesmo (~2 ms por quadro,
15 quadros/s).

## Resultado da v0.00.38

No mesmo trecho pesado do Setor 7 (~190 draws e ~16.400 vértices por quadro):

| Versão | FPS | Trabalho da thread do jogo por quadro |
|---|---|---|
| v0.00.36 | 17,8 | ~52 ms + ~3 ms de GL |
| v0.00.38 | 19,6 | ~48,5 ms + ~1 ms de GL |

A thread de render ocupa ~9% de um núcleo e a thread do jogo não esperou
nenhuma vez por ela. O que sobra é o código recompilado do jogo (95% da
thread). Os travamentos de 660–820 ms são trocas de tela de campo: LZSS,
texturas e leitura do OBB somam só ~60 ms; o resto é código do jogo
carregando a área.

Corrigido na v0.00.39: um swap de 481 ms. A thread principal esperava a fila
ficar vazia enquanto a thread do jogo continuava enfileirando; agora espera
só o que foi enfileirado antes dela.

## Perfil sem build especial (v0.00.39)

Criar `ux0:data/ffvii-vita/profile_on.txt` liga o profiler de picos: ele só
amostra quadros lentos e trechos em que a thread do jogo não fica ociosa por
50 ms. As amostras vão para `ux0:data/ffvii-vita/profile.bin`;
`scripts/profile-report.py profile.bin libjni_ff7.so` gera o ranking de
funções. O jogo fica um pouco mais lento enquanto o profiler está ligado.

## Perfil do Setor 7 (v0.00.39) e iluminação escalada nativa (v0.00.40)

O profiler só dispara quando o código pula para outra página de memória, então
funções pequenas e muito chamadas (`mk_address`, entradas PLT) acumulam as
amostras de quem as chama. Reatribuindo essas amostras à função chamadora (LR),
a thread do jogo ficou assim:

| Função | % | O que é |
|---|---|---|
| `func_0x0068de75` | 23% | iluminação de vértice com escala por canal (x87, 116 `mk_address`) |
| `func_0x0068e92d` | 8% | chama a anterior duas vezes |
| `VIDEO_enableTex` + `VIDEO_drawBuffers` | 13% | renderizador do port, por draw (inclui gravar na fila GL) |
| `C_006A8002` + `glVertex3f` + `fake_glColor4ub` + `C_0066CE40` | 13,5% | triângulos transformados um vértice por vez |

`0x68DE75(cor, escala[3], m, n, rgb[3], src, out)` é a `light_vertex`
(`0x68DD1E`) com duas diferenças: cada canal é multiplicado por `escala[k]`, e
um canal saturado (≥ 255) recebe o byte `k` de `cor` em vez de 255. No caminho
do ambiente o valor fica em double, sem arredondar nem saturar.

A semântica foi levantada e conferida com o código recompilado rodando num
emulador ARM (`scripts/emu/`, Unicorn): saída, EAX/ECX/EDX e flags C0/C2/C3
idênticos em dezenas de milhares de vértices aleatórios, incluindo infinitos,
denormais, igualdades exatas e endereços desalinhados. Casos com NaN numa
comparação ficam com a função original. No jogo, os primeiros 2000 vértices
ainda são conferidos com o original (`ff7_light 0x68DE75 selftest ok`).

`scripts/emu/test_light2.py libjni_ff7.so` repete a comparação no PC.

## v0.00.41: os três próximos itens do perfil

**Renderizador de triângulos (`C_006A8002`, ~13,5%)** — `source/ff7_tri.c`.
É C++ dos porters: por vértice chama a transformação 4×4, `fake_glColor4ub` e
`glVertex3f` da emulação de GL 1.x, cada uma recarregando seus globais pelo
GOT. A versão nativa faz o mesmo em linha, entre os `glBegin`/`glEnd`
originais. Conferida no emulador (`scripts/emu/test_tri.py`: `VERTICES`,
`pCurVertex` e cor atual idênticos, inclusive buffer cheio, w ≤ 0, infinitos)
e nas 300 primeiras chamadas no jogo contra o original (`ff7_tri selftest`).

**Renderizador do port por desenho (~13%)** — `source/reimpl/gl_queue.c`.
`VIDEO_enableTex` + `VIDEO_drawBuffers` fazem ~12 chamadas GL por desenho. O
custo estava na fila: cada chamada acordava a thread de render (uma chamada
de kernel) e cada desenho lia o relógio duas vezes. Agora só os desenhos
acordam a thread de render (ela também espera ~10 µs antes de dormir), o
relógio saiu do caminho do desenho, e chamadas que não mudam nada não entram
na fila: `glEnableVertexAttribArray` de um array já ativo (o jogo nunca
desativa) e `glUseProgram`/`glUniform1i` com o valor já em vigor. A linha
`render thread:` mostra as chamadas puladas e quantas vezes a thread acordou.

**`0x68E92D` (~8%)** — `source/ff7_light.c`. 74% das amostras dessa função
caem num laço por vértice que chama a `0x68DE75`. A cabeça do laço desvia para
uma versão nativa que roda todas as iterações e volta ao teste do laço
original, com o mesmo estado x86 (contagem, ponteiros, EAX/ECX/EDX, flags do
FPU). Conferida no emulador (`scripts/emu/test_e92d_loop.py`). Enquanto a
`0x68DE75` está em autoteste, o laço passa pela chamada x86 normal.

## Resultado da v0.00.41

No mesmo trecho pesado do Setor 7 (~190 draws e ~16.400 vértices por quadro):

| Versão | FPS | Trabalho da thread do jogo por quadro |
|---|---|---|
| v0.00.36 | 17,8 | ~52 ms + ~3 ms de GL |
| v0.00.38 | 19,6 | ~48,5 ms + ~1 ms de GL |
| v0.00.41 | **28,7–29** | **~33 ms** |

Todos os autotestes passaram (triângulos, iluminação, `0x68DE75`, fundo,
cos/sin). O jogo ainda fica 96% ocupado nesse trecho, logo abaixo dos 30 fps
do próprio jogo.

Em parte dos trechos pesados, a linha `render thread:` mostrou só ~2 bytes
copiados por vértice desenhado (o normal é 24). A fila foi testada no PC e
copia certo em todos os casos; a v0.00.42 registra um aviso
(`gl_queue: draw of N vertices with no vertex array`) se algum desenho chegar
sem array de vértices, para confirmar se é só a estatística ou um desenho real.

## Travamentos na troca de música e autotestes (depois da v0.2)

Nos logs da v0.2, o jogo fica quase sempre no limite de quadros dele (30 fps
no campo, 15 na batalha, 60 nos menus). O que se nota são travamentos de
0,3 a 1,1 s nas trocas de área e de música. Uma das causas é a leitura da
música: `SND_play` lê o `.akb` inteiro (2 a 4 MB) na thread do jogo, e uma
faixa fora do cache em RAM levava de 170 a 600 ms
(`OBB slow read [dokubo.akb] ... 277 ms`).

**Música em segundo plano** (`source/ff7_music.c`). Quando a faixa não está
no cache, a música atual para (como o `SND_play` já fazia) e uma thread
`music_loader`, nos cores 1–2, lê o arquivo para o cache em RAM. O jogo segue
rodando. Quando a leitura termina, a thread do jogo chama o `SND_play`
original, que encontra o arquivo em RAM. Toda chamada ao SdSoundSystem
continua na thread do jogo e na mesma ordem. Enquanto a faixa carrega,
`SND_getStatus` responde "tocando", `SND_stop` cancela o carregamento e
pausar/retomar é aplicado quando a faixa começa. A música começa alguns
centésimos depois, em vez de o jogo parar. O log mostra
`music: <faixa> loaded in the background in N ms`. Criar
`ux0:data/ffvii-vita/music_async_off.txt` volta ao comportamento antigo.

**Autotestes desligados por padrão.** Os autotestes das funções nativas
(iluminação, `0x68DE75`, fundo, cos/sin, triângulos, decodificador de
animação) rodavam a cada boot e caíam perto de quadros lentos. Agora só rodam
se existir `ux0:data/ffvii-vita/selftest_on.txt`. O log diz
`selftests: on/off` no boot.

## Faixa preta no modo de exploração (v0.2.02)

O jogo desenha cada quadro numa textura de 640×480, e o `VIDEO_update` a
estica na altura da tela (725×544 no Vita). No campo, o jogo deixa vazias as
últimas 16 das 480 linhas, o que aparecia como uma faixa preta de 18 px
embaixo. Não há arte para essa faixa na maioria dos cenários.

`source/ff7_screen.c` aumenta a imagem em 480/464 (+3,4%) a partir do topo,
mas só enquanto o renderizador de cenário roda (modo de exploração). O cenário
ocupa a altura toda, com 750 px de largura e sem distorção, e as bordas
laterais encolhem junto. Os botões "Encounters ON" e "A" continuam no mesmo
lugar, por cima da imagem. Batalha, menus, vídeos e mapa-múndi não mudam. O log
mostra `field zoom: on/off` e cada área de desenho nova que o jogo pede
(`drawing area: ... bottom at line N of 480 - field`), para conferir as 464
linhas em outros mapas. Criar `ux0:data/ffvii-vita/field_zoom_off.txt` desliga
o zoom.

## Acessos à pilha x86 sem chamada (v0.2.04, `source/ff7_stack.c`)

Metade das 669.918 chamadas a `mk_address` traduz um endereço da pilha x86:
`ESP + c` (push, pop, argumentos) ou `EBP + c` (variáveis locais e argumentos
de funções com ponteiro de quadro). A pilha é uma região só: VA
`0x170000–0x190000` (128 KB), que o `main_ff7` passa ao `start_ff7_core`.

**Como funciona.** Antes de o `start_ff7_core` mapear a pilha, um hook aloca
um bloco num endereço fixo `H` tal que `K = H − 0x170000` seja um imediato
Thumb-2 (`0xXY000000`) e preenche com ele as 32 entradas da pilha no `MEMMAP`.
O `VA_newMapper` só preenche entradas vazias, então mantém as nossas, e vale
`host = va + K` para toda a pilha. Cada ponto da lista vira:

    bl mk_address   ->   add.w r0, r0, #K

Há uma folga de 32 KB de cada lado da pilha dentro do bloco.

**Quais pontos** (`scripts/gen-stack-sites.py`, regras no cabeçalho). Uma
análise de fluxo de dados no código ARM prova, ponto a ponto, que `r0` no
`bl` é `ESP + c` ou `EBP + c` (|c| ≤ 0x8000):

| Tipo | Pontos | Condição |
|---|---|---|
| `ESP + c` | 114.575 | sempre na pilha |
| `EBP + c` | 218.878 | só em funções onde o EBP é ponteiro de quadro: a primeira escrita no EBP é `mov ebp, esp` no bloco de entrada, e toda outra escrita (o `pop ebp` do epílogo) é seguida direto da saída |
| `EBP + c` sem prova | 851 | ficam com a chamada |

São **333.453 chamadas removidas, 50% de todas as chamadas a `mk_address`**.
Somadas às 71.246 de globais da v0.00.37, 61% das chamadas deixam de existir.

**Chaves (pasta de dados):**
- `stack_off.txt`: volta ao original.
- `stack_check.txt`: modo de conferência. Em vez do `add`, cada ponto chama
  uma função que devolve o mesmo que `mk_address` e registra no log todo ponto
  cujo endereço não está na pilha (`ff7_stack: site so+0x... accessed VA ...`).
  É mais lento, mas sempre correto: serve para validar a lista no aparelho.

**Como testar:** primeiro uma sessão com `stack_check.txt` (o log não deve ter
nenhum `not on the stack`), depois sem o arquivo, comparando o `work` das
linhas `perf:` nos mesmos lugares (na v0.2.03, campo a 30 fps: trabalho real
de 35% a 79%).

### Resultado da v0.2.04

No Vita: pilha em `0x9b170000` (`K = 0x9b000000`), 333.452 de 333.453 pontos
trocados por `add.w` em 370 ms no boot (1 ponto não confere com o padrão e
mantém a chamada). Nos trechos de campo a 30 fps, o trabalho real médio da
thread do jogo caiu de ~47–53% (v0.2 e v0.2.03) para ~35%. As sessões não
passaram pelos mesmos lugares, então o número é indicativo.

## Menos despertares da thread de render (v0.2.05)

Na cena pesada do Setor 7, a fila GL acordava a thread de render ~4.000 vezes
por segundo (para ~5.000 desenhos), e cada despertar custa uma chamada ao
kernel na thread do jogo. Agora a thread do jogo a acorda a cada 8 desenhos,
quando vai dormir (`usleep`, fim do quadro) ou bloquear (`pthread_cond_wait`,
`sem_wait`), e sempre que algo espera a fila (fila cheia, `gl_queue_finish`,
chamadas GL diretas). A linha `render thread:` do log mostra os `wakes/s`;
`ux0:data/ffvii-vita/glbatch_off.txt` volta a acordar a cada desenho.

### Resultado da v0.2.05

No Vita, na cena mais pesada (~4.770 desenhos e ~460 mil vértices por
segundo, a mesma que na v0.2 ficava em 28–29 fps com a thread do jogo 96%
ocupada): 615 despertares por segundo em vez de ~4.000, e 30 fps estáveis com
74–76% de trabalho real. A queda vem principalmente da pilha sem chamada
(v0.2.04); os despertares agrupados somam pouco.

## Campos que recriam texturas a cada quadro (v0.5.01)

Nos dois trechos lentos dos logs da v0.5 (entrada da base da Shinra e uma
área que ficava em 25,8 fps), a thread do jogo fica 87–92% ocupada e o
padrão é o mesmo: o jogo recria 2–3 texturas de 256×256 por quadro
(44–76 por segundo). Cada uma custava:

| Custo (por segundo, cena a 25,8 fps) | Antes |
|---|---|
| Conversão paletada → RGBA (`textures/s: ... ms converting`) | 155 ms |
| `glGenTextures` esperando a fila GL esvaziar (`game thread waited: 66 syncs/s`) | 63 ms |

Juntos, ~22% da thread do jogo.

- **Conversão** (`source/ff7_texture.c`): o laço por pixel decidia o formato
  de destino (`switch`) a cada pixel. Agora há laços separados para destino
  de 32 e 16 bits (uma leitura de tabela e uma escrita por pixel); outros
  formatos e destinos desalinhados seguem pelo laço antigo.
- **Nomes de textura** (`source/reimpl/gl_queue.c`): a thread de render gera
  64 nomes adiantados; o `glGenTextures` da thread do jogo pega do estoque
  sem esperar a fila. Os nomes já ficam reservados no vitaGL ao serem
  gerados. A linha `render thread:` mostra `texture names/s from the pool`;
  o número de `syncs/s` nesses trechos deve cair para perto de zero.

## Faixa preta no campo, de novo (v0.5.01)

O log `drawing area: x 0 y 0 w 640 h 448 - field` mostra que o campo usa 448
das 480 linhas, não 464: o zoom da v0.2.02 ainda deixava ~18 px pretos
embaixo. Agora o zoom segue a área de desenho de largura total que o jogo
define no modo de campo (448 por padrão): o campo ocupa 777×544, sem
distorção e sem cortar nada nas laterais.

Esticar para 960×544 distorceria a imagem em 23%, e preencher a largura sem
distorcer cortaria 128 das 544 linhas (diálogos e personagens nas bordas),
então nenhum dos dois é o padrão. Quem preferir a tela toda esticada pode
criar `ux0:data/ffvii-vita/field_stretch_on.txt`.

## Batalha sem faixa preta (v0.5.02)

Na batalha, a cena 3D e o menu de comandos também terminam na linha 448.
O zoom do campo agora vale também quando o modo de jogo do ff7.exe (word em
`0xCBF9DC`: 1 campo, 2 batalha, 3 mapa-múndi…) é batalha: a imagem preenche
a altura e o menu fica colado na borda de baixo. A câmera da batalha é a do
jogo, feita para 448 linhas; mostrar mais cena embaixo do menu exigiria outra
projeção e mover todas as janelas da batalha, por isso o zoom amplia a imagem
em vez disso. O log mostra `game mode: N` a cada troca de modo e
`field zoom: on (battle)`. `field_stretch_on.txt` também estica a batalha.

Na v0.5.03 a batalha passou a usar um zoom mais leve (472 linhas): os
submenus (Magia, Item…) descem até ~465, abaixo da área 3D, e com 448 a
última linha deles era cortada. A faixa preta sob o menu principal fica bem
menor, mas não some de todo.

## Profiler reiniciava o console (v0.5.04, `source/utils/profiler.c`)

Na v0.5.03, com `profile_on.txt`, o console reiniciava logo no boot e o
`profile.bin` ficava vazio (o log parava no `onResume`: o resto não chegou ao
cartão). Desde a v0.00.39, última vez em que o profiler rodou, o boot passou a
reescrever 333 mil pontos do texto da `.so` (`ff7_stack`, v0.2.04) e a
esvaziar o cache do texto inteiro, na thread do jogo, exatamente quando o
profiler já tirava e devolvia a permissão de execução desse mesmo texto em
outro núcleo. Duas operações de kernel sobre os mesmos 15 MB ao mesmo tempo.

- O profiler só começa a amostrar 20 s depois de iniciado (tela-título),
  depois de todos os patches de boot.
- As trocas de proteção são uma de cada vez (estado IDLE → ARMING → ARMED →
  RESTORING): só quem tira o texto de ARMED devolve a execução, e o sampler
  só arma a partir de IDLE.
- Quem reescreve o texto em jogo (gancho do swirl, toque, fundo de campo,
  `ff7_memmap`, `ff7_stack`) chama `profiler_hold()`/`profiler_release()`,
  que desarmam e seguram o profiler enquanto isso.
- Só páginas inteiras do texto são protegidas: a última página parcial tem o
  começo da code cave do so_util (trampolins dos ganchos), que antes podia
  perder a execução sem estar na faixa tratada pelo handler.
- Uma falha que não é do profiler aparece no log
  (`profiler: fault not caused by the profiler at ...`).

## Mapa-múndi: primeiro perfil e profiler preciso (v0.5.05)

Logs da v0.5.04 (save carregado direto no mapa-múndi): 18–22 fps, thread do
jogo 92–93% ocupada, ~50 ms por quadro, GPU ~3 ms, GL 0%. Por segundo:
~1.300 desenhos, ~66 mil vértices, 7.260 vértices iluminados, 296 listas de
triângulos. O GL e a GPU não pesam; é código recompilado do jogo.

O perfil (profile_on.txt) deu metade das amostras do mapa em
`func_0x0074c85d_ex`, mas essa função é só a espera de quadro do mapa
(`usleep((alvo - agora) / 100)`, depois `_check_event`). Não é custo: é a
volta do `usleep`. Com o sampler nos cores 1–2, a nova proteção do texto só
valia no core 0 quando o TLB dele perdia a entrada antiga (troca de página,
volta de uma espera). Laços quentes nunca eram amostrados. As ~190 amostras
restantes ficaram espalhadas demais para apontar uma função.

**Profiler preciso.** O sampler agora roda no core 0, com prioridade maior
que a da thread do jogo: interrompe o jogo, troca a proteção nesse núcleo e o
jogo falha exatamente na instrução em que parou. Não arma enquanto a thread
do jogo dorme ou espera (`game_idle`, marcado em `utils/glstats.c`).

**Amostragem contínua.** `ux0:data/ffvii-vita/profile_all.txt` amostra a
cada 5 ms sempre que a thread do jogo está rodando, não só em quadros acima
de 100 ms ou sem pausa por 50 ms (o mapa dorme um pouco a cada quadro, então
o modo "picos" quase não amostrava).

**`game mode` no mapa.** Com o save carregado direto no mapa, o log ficava em
`game mode: 0` (o modo só era lido depois do primeiro fundo de campo), e o
zoom da batalha não ligava. `ff7_screen.c` agora lê a página do modo direto
da tabela `MEMMAP` assim que ela está mapeada.

## Mapa-múndi nativo, parte 1 (v0.5.06, `source/ff7_world.c`)

Perfil da v0.5.05 com `profile_all.txt` (~5.000 amostras da thread do jogo
andando no mapa, 19–23 fps, ~50 ms por quadro), com as amostras de
`mk_address` reatribuídas a quem chama:

| Função | % | O que é |
|---|---|---|
| `0x75F68C` | 28,9% | desenha os triângulos de uma malha do terreno (visibilidade, recorte, cor) |
| `0x75F263` | 11,6% | transforma 126 vértices da malha, projeta 123 e marca os fora da tela |
| `0x68DAE1` | 9,7% | cor de um vértice (luz × rgb, 0..255); 3 por triângulo |
| `0x75F0AD` | 8,0% | monta os 122 vértices da malha (int16 → float, z projetado, curvatura do planeta) |
| `0x66E272`, `0x66CE40`, `_check_event`, `0x7592E0`, `0x760CC4` | ~17% | resto |

O terreno é desenhado malha por malha por `0x751EFC`: para cada malha,
`0x75F0AD` → `0x75F263` → `0x75F68C`, que chama `0x68DAE1` três vezes por
triângulo.

Viraram nativas `0x75F263`, `0x68DAE1` e `0x75F0AD` (~29% da thread no
mapa). A matemática está em `source/ff7_world_core.h` e foi conferida contra
o código recompilado rodando no emulador (`scripts/emu/test_world.py
libjni_ff7.so [chamadas] [vértices] ff7_en.exe`): memória escrita,
EAX/ECX/EDX, ESP/EBP, topo da pilha x87, flag de divisão por zero e C0/C2/C3
idênticos em ~75 mil vértices de cada função, com infinitos, NaN, `w == 0`,
conversões que saturam e as bordas de recorte. Detalhes que o recompilador
impõe:

- `fistp` do x87 vira `vcvt.s32` (trunca e satura, NaN → 0), não arredonda;
- a divisão por zero não dá infinito: o valor fica como estava e o byte
  `0x56` do estado x87 (flag ZE) vai a 1;
- `0x68DAE1` deixa em EDX `m`, `out` ou `rgb` conforme o caminho de cada
  canal; casos com NaN numa comparação ficam com a função original.

As 4 chamadas do começo de `0x75F0AD` (montagem das matrizes) continuam no
código recompilado, chamadas pela versão nativa com a mesma pilha x86.

Chaves: `world_off.txt` desliga as três; `selftest_on.txt` confere as
primeiras 300 chamadas de cada uma contra o original no jogo. O log mostra
`world map meshes/s: built N native ...; projected N native ... | colored
vertices/s (0x68DAE1): N native, N original`.

Ganho esperado: ~20–25% da thread no mapa (de ~50 para ~38–40 ms por quadro,
~25 fps). Para os 30 fps falta `0x75F68C` (29%), a próxima.

O `profile_all.txt` reiniciou o console duas vezes após 20–40 s (~170 trocas
de proteção do texto inteiro por segundo). Agora amostra a cada 20 ms.

### Resultado da v0.5.06

Mapa-múndi andando: de 19–23 fps para **23–29 fps** (média ~25,5; thread do
jogo 85–90% ocupada, ~39 ms por quadro). Os três autotestes passaram no Vita
(`selftest ok`), e nenhuma chamada caiu na versão original (~410 malhas e
~37 mil vértices coloridos por segundo).

## Mapa-múndi nativo, parte 2: os triângulos (v0.5.07, `source/ff7_world_tri.h`)

`0x75F68C` (~29% no perfil da v0.5.05) desenha os triângulos de uma malha.
Por triângulo:

1. descarta se nenhum vértice está na área de recorte em x ou em y (flags
   de `0x75F263`); calcula a área com `0x760CC4`; limpa o slot de seleção
   `0xE36110` se ele aponta para este triângulo;
2. descarta faces de costas; escolhe o z mais distante (com as diferenças
   truncadas, como o x86);
3. perto da câmera, marca `0xE37520` se o triângulo cruza o centro da tela
   nos dois eixos (o x86 compara o y com o centro horizontal também);
4. descarta se algum vértice tem `w` ≤ 0;
5. névoa: `0x760D1B` ordena os vértices por z; cores e palavras de brilho
   vêm de tabelas pela faixa de profundidade;
6. pede espaço no buffer de vértices da textura (`0x66E272`), calcula UV e
   normais, colore cada vértice com `0x68DAE1` e grava x/w, y/w, z/w, 1/w,
   cor, u e v.

`0x760CC4`, `0x760D1B`, `0x74C969` e o caminho comum de `0x66E272` (há espaço
no buffer) viraram código nativo junto. `0x68DAE1` é chamada pela entrada
enganchada (a versão nativa da v0.5.06), e o caminho lento de `0x66E272`
(alocar um buffer novo) chama a função original, com a mesma pilha x86.

**Teste.** Como a função chama outras funções do jogo e passa variáveis
locais da pilha x86 por ponteiro, o teste compila o próprio
`ff7_world_tri.h` num blob ARM (`scripts/emu/blob_world_tri.c`, com o
arm-vita-eabi-gcc) e roda o blob e a função recompilada no mesmo emulador,
a partir do mesmo estado, chamando as mesmas funções recompiladas
(`scripts/emu/test_world_tri.py libjni_ff7.so ff7_en.exe [malhas]`). Compara
toda a memória x86 (menos a pilha abaixo da chamada), EAX/ECX/EDX/ESP/EBP/ESI,
C0/C2/C3, flag de divisão por zero, topo do x87 e o número de chamadas a
`_check_event`. O caminho lento de `0x66E272` não roda no emulador (aloca
com funções externas).

Detalhes que só o teste mostrou:

- uma comparação com NaN no `fcomp` recompilado **não muda** C0/C2/C3: o
  `test ah, 1` seguinte vê o C0 da comparação anterior;
- os dois primeiros descartes (flags e face de costas) pulam o incremento do
  4º argumento, e o EDX final depende de qual caminho o último triângulo
  tomou.

Sem autoteste no jogo: rodar as duas versões alocaria o buffer de vértices
duas vezes. `world_tri_off.txt` desliga só esta função; `world_off.txt`,
todas. O log mostra `world map triangles/s: N seen, N drawn (...)`.

Ganho esperado: a maior parte dos ~11 ms que a função custava por quadro na
v0.5.06, o que deve levar o mapa aos 30 fps.

## Minigame da moto (highway, modo 6) — diagnóstico dos logs v0.5.07

Logs da sessão de v0.5.07 (bike.webm → modo 6, t ≈ 141–163 s):

| Métrica | Valor |
|---|---|
| FPS | 22–23 |
| Work da thread | 95–97% |
| Draw/frame | ~72–77 |
| Verts/frame | ~3.500 |
| `triangle lists/s native` | 2.100–2.740 |
| `lit vertices/s` | 0 |
| `field background tiles/s` | 0 |
| `cos/sin native/s` | 4.000–8.000 |
| GPU tail | ~3 ms |
| GL da thread | 0% |

O padrão é idêntico ao mapa-múndi antes da v0.5.06: CPU-bound, sem ociosidade,
GPU trivial, sem campo de fundo nem iluminação de vértice.

**`ff7_tri` já cobre o highway**: as 2.100–2.740 listas de triângulos por
segundo na linha `triangle lists/s native` do log confirmam que `C_006A8002`
é chamado no highway e a substituição nativa está ativa. O que resta é o
código recompilado do jogo que prepara a geometria a cada frame — as funções
equivalentes às que foram nativas no mapa-múndi (`0x75F263`, `0x75F0AD`,
`0x75F68C`), mas para o módulo highway (endereços provavelmente na faixa
`0x74xxxx`).

**Não existe perfil do highway ainda.** Para identificar essas funções é
preciso rodar `profile_all.txt` durante o minigame e transferir o
`profile.bin` resultante.

## Perfil do minigame da moto — próximos passos

1. **Coletar o perfil**: criar `ux0:data/ffvii-vita/profile_all.txt` antes de
   entrar no highway. O profiler demora 20 s para armar após o boot, então
   passar pela tela-título e menu normalmente antes da perseguição. Jogar o
   minigame por 30–60 s e transferir `profile.bin` e `logs.txt`.

2. **Gerar o ranking**:
   ```
   scripts/profile-report.py profile.bin libjni_ff7.so \
       --from T_ENTRADA --to T_SAIDA --top 30
   ```
   Onde `T_ENTRADA` e `T_SAIDA` (em segundos) isolam o trecho do modo 6 no
   log (`game mode: 6` / próximo `game mode:`). Reatribuir as amostras de
   `mk_address` ao LR (já feito pelo script) revela as funções recompiladas
   que dominam o tempo.

3. **Identificar o padrão**: espera-se algo similar ao mapa-múndi:
   - Uma função de transformação de vértice por frame (~10–15%)
   - Uma função de desenho de triângulo por objeto (~20–30%)
   - Possivelmente uma função de cor/iluminação (~8–10%)
   Todas com símbolo `_Z15func_0x0074XXXXv` na faixa do módulo highway.

4. **Implementar nativo** — mesma metodologia das funções do mapa-múndi:
   - Levantar a semântica pelo código ARM descompilado + emulador Unicorn
     (`scripts/emu/ff7emu.py`)
   - Escrever `source/ff7_highway_core.h` com a matemática
   - Verificar com script `scripts/emu/test_highway.py` (memória x86,
     registradores, flags x87 idênticos em dezenas de milhares de casos)
   - Instalar via `hook_addr` em `source/ff7_highway.c`
   - Autoteste nas primeiras N chamadas no jogo com `selftest_on.txt`

5. **Ganho esperado**: pelo perfil do mapa-múndi, as 3–4 funções mais quentes
   somavam ~57% da thread. No highway, com `work` de 95–97% e ~44 ms por
   quadro (a 23 fps), o mesmo percentual representa ~22–25 ms — suficiente
   para atingir os 30 fps.

## Perfil do highway (v0.6.0, profile_all.txt) — 8.265 amostras

O profiler rodou durante toda a sessão. Isolando o trecho do modo 6
(t = 203–375 s no log), 7.162 amostras da thread do jogo (86,7% do total):

| Função | Amostras | % | Callers principais (por LR) |
|---|---|---|---|
| `mk_address` | 1.012 | 14,1% | `0x65d9c2` 16%, `0x65d1de` 15% |
| `func_0x00651a6d` | 682 | 9,5% | si mesma 58%, `__ashldi3` 16% |
| `func_0x0040b470` | 477 | 6,7% | `0x651a6d` 35%, `__fixunsdfdi` 31% |
| `func_0x0065d9c2` | 412 | 5,8% | si mesma 58%, `__ashldi3` 41% |
| `func_0x00651a2b` | 344 | 4,8% | si mesma 56%, `__ashldi3` 32% |
| `glEnd` | 335 | 4,7% | `__ashldi3` 100% |
| `func_0x0065caf8` | 287 | 4,0% | si mesma 46%, `0x65d9a9` 16% |
| `func_0x006519d3` | 258 | 3,6% | `__ashldi3` 62%, `0x651a6d` 16% |
| `func_0x00661465` | 188 | 2,6% | si mesma 70% |
| `func_0x0065d1de` | 152 | 2,1% | si mesma 73% |
| `VIDEO_drawBuffers` | 142 | 2,0% | `__ashldi3` 66% |
| `func_0x0068e2f9` | 141 | 2,0% | `0x651a6d` 45% |
| `func_0x0066e272` | 95 | 1,3% | alocador de buffer de vértices |
| `func_0x006840da` | 84 | 1,2% | si mesma 80% |

**Reatribuindo `mk_address` ao LR:** `0x65d9c2` sobe para ~574 amostras
(8,0%); `0x65d1de` para ~304 (4,2%). Os helpers de runtime ARM
(`__ashldi3`, `__fixunsdfdi`, `__muldf3`) acumulam-se dentro de
`0x651a6d`, `0x65d9c2` etc. — são parte do custo dessas funções.

### O que cada cluster é

O profiler também mostra `C_006A8A3E` (no LR de `glVertex3f`) e
`C_006A87AC`/`C_006A7DEE` (no LR de `C_0066CE40`). Essas são funções dos
porters com nome demanglado: **o highway usa um renderizador de triângulos
diferente do campo** — `C_006A8A3E` em vez de `C_006A8002`. O `ff7_tri`
instalado não cobre esse renderizador.

| Cluster | Funções | % total | O que é |
|---|---|---|---|
| A | `0x651a6d`, `0x40b470`, `0x6519d3`, `0x68e2f9` | **21,8%** | loop de render do highway (equivalente ao `0x75F68C` do mapa) |
| B | `0x65d9c2` (+mk), `0x65d1de` (+mk), `0x65caf8`, `0x65da4c` | **~22%** | transformação de vértices (equivalente ao `0x75F263`/`0x75F0AD`) |
| C | `0x651a2b` | **4,8%** | segundo laço de vértice |
| D | `glEnd` + `VIDEO_drawBuffers` + `VIDEO_enableTex` | **7,2%** | renderer do port por draw |

### `C_006A8A3E` — o triangle renderer do highway

O profiler mostra `glVertex3f <- C_006A8A3E` (39% das amostras de
`glVertex3f`). `C_006A8A3E` é outra função dos porters — análoga ao
`C_006A8002` (já coberto por `ff7_tri`) mas para geometria 3D do highway.
Ela está **fora do hook atual** do `ff7_tri`, que só cobre `C_006A8002`.

### Próximos passos para o highway

1. **`C_006A8A3E`**: verificar prologue e hookear com a mesma lógica de
   `ff7_tri.c`. O símbolo ARM é
   `_Z10C_006A8A3EiPtP19t_dx_rend_vertex_20P8t_matrix` — verificar com
   `arm-vita-eabi-nm`. Se o prologue bater, um hook direto cobre ~23% dos
   draws (os 0,32% de `glVertex3f` com LR `C_006A8A3E` são o custo
   residual; o bulk está na própria função).

2. **Cluster B (`0x65d9c2`, `0x65d1de`, `0x65caf8`)**: são as funções de
   transformação de vértice do highway (~22% + parte do mk_address).
   Levantar a semântica com o emulador Unicorn e implementar nativas, como
   feito para `0x75F263`/`0x75F0AD` do mapa.

3. **Cluster A (`0x651a6d`, `0x40b470`, `0x6519d3`, `0x68e2f9`)**: o loop
   de render propriamente dito (~22%). O endereço `0x651a6d` é quase certo
   o loop de objetos do highway (chama a si mesmo = laço), com `0x40b470`
   sendo o per-vértice (cor/normal). Candidato principal para uma versão
   nativa completa.

Ganho esperado se os três clusters forem nativos: ~45–50% da thread do
highway. O minigame fica em 94–96% CPU-bound a 21–27 fps; com metade da
thread liberada, ~30 fps é viável.

## Plano de ação novo para o highway (perfil de 20/10/2026)

Esta sequência foi reavaliada a partir de `build/VPK/logs.txt`,
`build/VPK/profile.bin` e `build/VPK/logs_moto_erro.txt`. O log antigo mostra
a moto em 22–23 fps; no perfil mais recente, o modo 6 mantém a thread do jogo
em 95–97%, com GPU tail de 3,2–3,4 ms. Os travamentos de 1,3 s aparecem ao
abrir `high-us.lgp` e são custo de carregamento; não entram na otimização por
quadro.

1. **Reduzir conversões double→uint64** (`__fixunsdfdi` / `__aeabi_d2ulz`). O
   chamador `func_0x00651a6d` ocupa 9,6% do perfil e o helper de conversão
   aparece diretamente em 2,65%, além das rotinas de aritmética double da
   mesma cadeia. Implementado em
   `source/ff7_d2ulz.c`: decomposição IEEE-754 por inteiros substitui a cadeia
   de multiplicações, subtrações e conversões double do libgcc. A semântica de
   NaN, negativos e overflow é comparada com o original no autoteste opcional
   (`ux0:data/ffvii-vita/selftest_on.txt`, primeiras 512 chamadas). Para
   desligar sem rebuild, criar `ux0:data/ffvii-vita/d2ulz_off.txt`.
2. **Reperfilar a moto com o autoteste ligado**. Se houver divergência, o hook
   volta ao original automaticamente. Medir FPS e `unsigned double
   conversions/s` em um trecho repetível do highway, e comparar com o mesmo
   trecho sem `d2ulz_off.txt`.
3. **Atacar transformação de vértices** (`0x65d9c2`, `0x65d1de`, `0x65caf8`,
   ~22% incluindo `mk_address`). Antes de substituir, mapear os arrays e
   estado x87 que essas rotinas modificam; então fazer core C bit-exato e
   emulador com comparação de memória, registradores e flags x87. Hook só após
   essa equivalência.
4. **Atacar o loop de render do highway** (`0x651a6d` e helpers, ~22%).
   Preservar ordem dos eventos GL e estado de pilha/FPU; instrumentar amostras
   separadas por função antes de escolher o primeiro loop nativo.

O primeiro item reduz trabalho repetido de conversão sem reescrever a lógica
do minigame. O ganho real ainda depende da medição no Vita; os percentuais do
perfil indicam teto de custo removível, não uma previsão de FPS.

## v0.6.2: conversão x87 `_ftol` nativa

O usuário relatou 27 fps na moto com a primeira alteração. Ainda não há log ou
perfil dessa execução: os arquivos disponíveis continuam sendo os de antes
do patch. Para esta etapa foi reanalisado o intervalo 205–375 s do perfil
original, com 7.102 amostras da thread do jogo. `0x40B470` aparece diretamente
em 477 amostras (6,72%). Isso orienta a escolha do alvo, mas não mede seu peso
na execução que chegou a 27 fps.

A desmontagem confirma que `0x40B470` é o helper MSVC `_ftol`, que retira um
double da pilha x87 e devolve o inteiro em EDX:EAX. A hipótese anterior nesta
documentação de que ele seria uma operação de cor/normal não se confirmou.
O helper não é exclusivo da moto; seus outros chamadores também usam a nova
implementação.

`source/ff7_ftol.c` substitui o wrapper inteiro. O core compartilhado em
`source/ff7_ftol_core.h` converte pelos bits IEEE-754, preservando os
comportamentos particulares do binário até em overflow, infinitos e NaNs.
Mantém os 16 bytes de escrita temporária na pilha x86, a normalização da
palavra de controle, o TOP e as demais flags/slots x87. `_check_event` continua
sendo chamado uma vez, antes de incrementar ESP para o retorno. Frames que
cruzam páginas não contíguas ou um TOP inesperado seguem pelo original.

Validação offline com `scripts/emu/test_ftol.py`, compilando o mesmo header
C para ARM Cortex-A9 e executando junto do código original em Unicorn:

- 27.288 entradas em cada helper (signed e unsigned), todas idênticas;
  incluem ambos os sinais, todos os expoentes, payloads NaN e valores de
  coordenadas. A conversão unsigned da etapa anterior também foi conferida.
- 12.000 estados de `_ftol`, comparando todo o bloco de registradores/flags,
  estado x87 e região da pilha, inclusive no instante de `_check_event`.
  Inclui pilha desalinhada, fronteiras de página, TOP 0–7 e callback alterando
  ESP para conferir a ordem do retorno.
- Build Vita e `check-vpk` concluídos. Arquivo novo:
  `build/VPK/ff7_vita_v0.6.2.vpk`; a numeração pula a tentativa 0.6.1 descartada.

No aparelho, as primeiras 64 chamadas são comparadas com o original
automaticamente (512 com `selftest_on.txt`). O log deve registrar
`ff7_ftol selftest ok` e depois `x87 integer conversions/s`. Em divergência,
o resultado original é mantido e a substituição é desativada. As chamadas
ao original suspendem o profiler enquanto o hook altera permissões/código.

Para A/B, repetir o mesmo trecho com e sem
`ux0:data/ffvii-vita/ftol_off.txt`, reiniciando o jogo entre execuções. Esse
arquivo desliga apenas `_ftol`; a conversão unsigned anterior permanece
ativa. O aumento de FPS desta etapa ainda precisa ser medido no Vita.

## Vídeos pré-renderizados e zoom (v0.6.3)

A queda contínua para 15 fps no trecho 102--143 s do `logs.txt` é a FMV
`bike.webm`, identificada por `MyDecoder.START`. Há somente 45 draws/s, GPU
tail de 2,2 ms e a thread `movie` ocupa 42--53% de um núcleo. Assim, ampliar
a imagem não altera o FPS dessa sequência: o custo é a decodificação VP8.

`ff7_screen` passa a consultar o estado do próprio `MyDecoder` entre `START`
e `RESET`. Se o vídeo tiver 448 linhas, recebe o mesmo zoom proporcional
480/448 do cenário de campo: preenche a altura do Vita sem distorcer. Vídeos
em outra resolução mantêm a apresentação original. O log marca
`field zoom: on (movie)` durante a FMV e volta para `field zoom: off` ao fim.

## Relatório consolidado: trabalho recente de desempenho

| Área | Alteração | Evidência disponível | Resultado e estado |
|---|---|---|---|
| Moto / highway | `ff7_d2ulz.c`: conversão double→uint64 por operações inteiras | Perfil original: cadeia do chamador em 9,6%; helper em 2,65% | Integrada; o usuário mediu melhora para cerca de 27 fps. `d2ulz_off.txt` permite A/B. |
| Moto / highway | `ff7_ftol.c`: substituição do helper x87 `_ftol` em `0x0040B470` | 477 de 7.102 amostras da thread do jogo (6,72%) | Integrada na v0.6.2; validação offline e autoteste no aparelho. Falta medir o ganho isolado. |
| Moto / highway | Render de triângulos `C_006A8A3E` | O log confirma execução nativa durante a moto; FPS atual varia perto de 24--30 | Já estava no projeto antes deste ciclo; foi mantido, sem reutilizar a implementação descartada como base para as duas conversões. |
| Campo com fogo | Diagnóstico do trecho 602--668 s de `logs_anterior.txt` | 17 fps, 94--95% da thread do jogo, ~250 conversões de textura/s, 6,3 milhões de pixels/s; GPU em 3,5 ms | É um gargalo de CPU independente da moto. O próximo perfil deve ser capturado nesse trecho. |
| Vídeos pré-renderizados | Detecção pelo estado do `MyDecoder` e zoom de vídeos 640×448 | `MyDecoder.START`/`RESET` são sinais diretos de reprodução | Integrada na v0.6.3. O zoom preserva proporção e usa a altura inteira; não acelera a decodificação VP8. |

### Garantias aplicadas às mudanças da moto

- A conversão unsigned preserva NaN, valores negativos e overflow, com
  autoteste opcional nas primeiras 512 chamadas.
- `_ftol` preserva registradores, flags, pilha x86, estado x87 e a chamada a
  `_check_event`. A comparação offline cobriu 27.288 entradas dos helpers e
  12.000 estados completos de `_ftol`; a primeira execução no Vita compara
  mais 64 chamadas com o original.
- A v0.6.3 foi compilada e o pacote passou em `check-vpk`.

### Próxima medição necessária

Para a cena de fogo, criar `profile_all.txt`, entrar no trecho que fica em
17 fps por pelo menos 20 segundos, sair do jogo e enviar o novo `profile.bin`
junto do `logs.txt`. Depois remover `profile_all.txt`: o profiler reduz o
desempenho enquanto está ativo. Esse perfil separará o custo de montar o efeito
do custo já medido de converter e enviar as texturas.

## v0.6.5: cor de vértice do highway nativa (`source/ff7_highway.c`)

Medição de referência: `logs.txt` da v0.6.2 (o `logs_moto_erro.txt` é da
v0.5.07 e o `profile.bin` disponível é da v0.6.0, anterior a `d2ulz`,
`_ftol` e `C_006A8A3E`). No modo 6: 23–32 fps (média ~26), thread do jogo
94–96% ocupada, ~38 ms por quadro; **~231 mil `_ftol` por segundo**, três por
vértice colorido.

Reatribuindo o perfil da v0.6.0 (7.162 amostras da thread do jogo,
203–375 s), depois de descontar o que já virou nativo (`_ftol` 7,4%,
`__fixunsdfdi` 5,2% e o `__muldf3` que ele chamava):

| Função | % | O que é |
|---|---|---|
| `0x651A6D` | 10,5% | cor do vértice (callback) |
| `0x65D9C2` | 8,0% | transformação de vértices |
| `0x651A2B` | 6,0% | z do vértice (chamada por `0x651A6D` e `0x65CAF8`) |
| `0x65CAF8` | 4,7% | transformação (chama `0x651A2B`/`0x6519D3`) |
| `0x65D1DE` | 4,2% | transformação |
| `0x6519D3` | 3,7% | fator de névoa (chamada por `0x651A6D` e `0x65CAF8`) |
| `0x661465` | 3,3% | — |

`0x651A6D (src, v, out)` é um callback por vértice: `0x659B40` guarda o
endereço numa estrutura e o highway o chama por ponteiro (`_call_func`), o
que o gancho na entrada ARM também pega. Ele calcula z pela matriz da câmera
(`0x651A2B`), o fator de névoa entre os planos perto/longe (`0x6519D3`) e cada
canal = byte × fator, saturado em 255, por três `_ftol`. A versão nativa
(`source/ff7_highway_core.h`) faz a cadeia inteira de uma vez: some uma
chamada x86 a `0x651A2B`, uma a `0x6519D3` e três a `_ftol` por vértice, mais
todo o x87 emulado entre elas.

**Teste.** Igual ao de `0x75F68C`: o header vira um blob ARM
(`scripts/emu/blob_highway.c`) que roda no emulador ao lado da função
recompilada, que chama os helpers recompilados de verdade
(`scripts/emu/test_highway.py libjni_ff7.so ff7_en.exe [vértices]`):
registradores x86, estado x87 (palavra de controle, flags de exceção, C0–C3,
TOP), memória e chamadas a `_check_event` idênticos em 20.000 vértices, com
NaN, infinitos, `far == near`, palavras de controle fora do padrão e o
caminho saturado. Cinco mutações do núcleo foram todas detectadas. Detalhes
que o teste mostrou:

- o `fnstsw ax` recompilado grava a palavra de status no EAX **inteiro**
  (extensão de zero), não só em AX;
- o primeiro teste de névoa é `test ah, 0x41` (C0 ou C3);
- `fidiv` por zero mantém o dividendo e marca ZE.

Chaves: `highway_off.txt` desliga; `selftest_on.txt` confere os primeiros
2.000 vértices contra o original no jogo. O log mostra
`highway colored vertices/s: N native, 0 original`.

Ganho esperado: a maior parte dos ~27% que a cadeia somava na v0.6.0 (hoje
menos, porque `_ftol` já é nativo). Precisa ser medido no Vita.

### Correções da revisão

- **Fallback sem reescrever o código a cada chamada.** `_ftol` e `d2ulz`,
  ao se desligarem por divergência no autoteste, chamavam o original por
  `SO_CONTINUE` em toda chamada: duas escritas no texto e dois flushes de
  cache por chamada (até ~230 mil por segundo). Agora o gancho é removido uma
  vez (`source/ff7_unhook.h`) e as chamadas seguintes vão direto ao original.
- **`d2ulz` sob `profiler_hold`.** O `SO_CONTINUE` do autoteste reescreve a
  entrada da função enquanto o profiler pode estar trocando a proteção do
  texto em outro núcleo; agora fica protegido, como já era no `_ftol`.
  Observação: `__fixunsdfdi` é libgcc dentro da `.so` e pode ser chamado por
  outras threads (áudio); o autoteste dele só deve ser usado para
  diagnóstico (`selftest_on.txt`).
- **Estado do vídeo entre threads.** `ff7_screen` (thread principal) lia
  `mv.playing`/`mv.height`, escritos pela thread do jogo. Agora lê uma
  palavra `volatile` (`active_height`), publicada no fim de `movie_start` e
  zerada no começo de `movie_stop`.

### Próximos alvos do highway

Transformação de vértices: `0x65D9C2` (8%), `0x65CAF8` (4,7%, que também
chama `0x651A2B`/`0x6519D3`) e `0x65D1DE` (4,2%). Um perfil novo com a v0.6.5
(`profile_all.txt`, agora a cada 20 ms) mostra o peso real de cada uma
depois desta etapa.
