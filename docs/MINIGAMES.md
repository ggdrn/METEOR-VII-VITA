# Mini-games 3D: mapa e plano de otimização

Levantamento estático do `ff7_en.exe` (o código que a `libjni_ff7.so`
recompila), feito em 22/10/2026 para aplicar aos outros mini-games o que foi
feito na moto (`docs/DESEMPENHO.md`, v0.6.x).

## Como os modos foram achados

O modo de jogo é a word em `0xCBF9DC`. A troca de modo (`0x409C5A`, tabela
de saltos de 28 entradas) registra, para cada módulo, uma estrutura de
ponteiros (inicialização, laço principal, saída…) e a passa a `0x666CF2`.
Os arquivos de dados confirmam cada módulo (`high-us.lgp` é referenciado em
`0x6507C9`, `coaster.lgp` em `0x5E9339`, `snowboard-us.lgp` em `0x72311F`).

| Modo | Mini-game | Laço principal | Init | Faixa do módulo | Dados |
|---|---|---|---|---|---|
| 6 | Moto (highway / G-Bike) | `0x650412` | `0x650310` | `0x650000–0x660000` | `high-us.lgp` |
| 7 | Corrida de chocobo | `0x76DBD1` | `0x76D597` | `0x76Dxxx–` | `chocobo.lgp` |
| 8 e 14 | Snowboard | `0x722CCF` | `0x722C10` | `0x722xxx–` | `snowboard-us.lgp` |
| 9 | Fort Condor | `0x5F4A47` | `0x5F47B5` | `0x5F4xxx–` | `condor.lgp` |
| 10 | Submarino | `0x77DF72` | `0x77D030` | `0x77Dxxx–` | `sub.lgp` |
| 11 | Montanha-russa (Speed Square) | `0x5E8E7E` | `0x5E8D49` | `0x5E8xxx–` | `coaster.lgp` |

Para comparação: campo (modo 1) `0x60E5B7`, batalha (modo 2) `0x41BAB3`,
mapa-múndi (modo 3) `0x74BE49`. (Uma primeira versão deste documento trocou
campo e batalha; a coluna "batalha" da tabela abaixo é, na verdade, o
campo.)

## O que já vale para todos

Pelo grafo de chamadas x86 a partir de cada laço (chamadas diretas e
ponteiros de função gravados no código; callbacks indiretos não aparecem):

- **`_ftol` nativo** (`ff7_ftol.c`): alcançado por todos os mini-games.
- **Renderizadores de triângulos** (`C_006A8002`, `C_006A8A3E`, `ff7_tri.c`)
  e **cos/sin** (`ff7_math.c`): estão no driver gráfico dos porters, comum
  a todos os modos.
- **`d2ulz`**: helper do libgcc, usado por qualquer código.

Ou seja, todo mini-game já ganhou alguma coisa com o trabalho da moto. O que
não está coberto é o código próprio de cada módulo e algumas funções do motor
comum.

## Funções do motor comum (`0x660000–0x6B0000`) usadas por vários

Alcance estático a partir dos laços principais.

| Função | Moto | Chocobo | Snowboard | Condor | Submarino | M.-russa | O que é | Peso na moto (v0.6.0) |
|---|---|---|---|---|---|---|---|---|
| `0x66C984` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | multiplicação de matrizes 4×4 (x87) | 0,9% |
| `0x66E272` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | espaço no buffer de vértices (caminho rápido já nativo dentro do `0x75F68C`) | 1,8% |
| `0x661465` | ✓ | ✓ |  | ✓ | ✓ | ✓ | matriz de ponto fixo → float (9 divisões; usa `0x6611DA`) | 3,3% |
| `0x6611DA` | ✓ | ✓ |  | ✓ | ✓ | ✓ | auxiliar de `0x661465` (fild/fdiv) | 0,7% |
| `0x661098` | ✓ | ✓ |  | ✓ | ✓ |  | vetor de ponto fixo → float | 0,6% |
| `0x6840DA` | ✓ | ✓ |  |  |  |  | — | 1,7% |
| `0x683B33` |  | ✓ |  |  |  |  | — | (1,7% na moto, por callback) |

São matemática pura, fáceis de fazer nativas e de conferir no emulador (mesmo
método do `0x651A6D`), e ajudam vários mini-games de uma vez. O peso em cada
um só um perfil diz.

## Plano

1. **Medir cada mini-game.** Sem perfil não dá para saber o que pesa em cada
   módulo (na moto, o maior custo era um callback chamado por ponteiro, que
   a análise estática não enxerga). Para cada um: criar
   `ux0:data/ffvii-vita/profile_all.txt`, jogar 30–60 s, sair e enviar
   `logs.txt` + `profile.bin` (um mini-game por sessão; o log mostra
   `game mode: N`). Disponibilidade no jogo:
   - Gold Saucer (disco 1): G-Bike (moto), Speed Square (montanha-russa),
     corrida de chocobo;
   - Fort Condor (disco 1);
   - submarino e snowboard (disco 2; o snowboard também fica no Gold Saucer
     depois).
   Anotar também o FPS sem o profiler (linhas `perf:`), para saber quais
   ficam abaixo dos 30.
2. **Motor comum** (pode começar já): versões nativas de `0x66C984`,
   `0x661465`/`0x6611DA`/`0x661098` e do caminho rápido de `0x66E272` como
   gancho próprio. Ganho pequeno por função, mas vale para todos os modos,
   inclusive batalha e mapa.
3. **Código próprio de cada mini-game**, na ordem do que estiver mais longe
   dos 30 fps, com o mesmo processo da moto: perfil → função mais pesada →
   núcleo em C → teste no emulador (blob ARM ao lado do original) → gancho
   com `*_off.txt` e autoteste.

## v0.6.7: funções comuns do motor nativas (`source/ff7_engine.c`)

Lendo o ARM recompilado, `0x66C984` (matrizes 4×4) já tinha sido reescrita
pelos porters em `float` comum: fica como está. Viraram nativas as
conversões de ponto fixo que passam pelo x87 emulado:

| Função | O que é | Chamadas diretas no exe |
|---|---|---|
| `0x661465` | matriz PSX (`int16 r[9]` + `int32 t[3]` em +0x12, empacotados) → matriz 4×4 float: `r / 4096`, `t` por `0x6611DA`, última linha (0,0,0,1) | 31 |
| `0x6611DA` | `(double) x / [0x7B7A54]` (1,0), devolvido na pilha x87 | 3 + 6 |
| `0x661098` | `int16[3]` → `float[3]` | 8 |

Núcleo em `source/ff7_engine_core.h`; teste
`scripts/emu/test_engine.py libjni_ff7.so ff7_en.exe [chamadas]`, com o
header compilado em blob ARM rodando ao lado das funções recompiladas:
9.000 chamadas idênticas (registradores, estado x87, valor empilhado,
memória, `_check_event`), incluindo divisão por zero e valores cruzando
páginas; quatro mutações detectadas. Chaves: `engine_off.txt` desliga;
`selftest_on.txt` confere as primeiras 500 chamadas de cada uma no jogo. O
log mostra `engine conversions/s`.

`0x66E272` (espaço no buffer de vértices) ficou de fora como gancho próprio:
o caminho lento aloca, e chamar o original por `SO_CONTINUE` a cada alocação
reescreveria o código centenas de vezes por segundo. O caminho rápido já é
nativo dentro do `0x75F68C` (mapa).

## Como o jogo inicia um mini-game

Pela instrução de script de campo **0x20 (`MINIGAME`)**, tratador `0x6134C0`
(tabela de instruções em `0x9055A0`). Argumentos (11 bytes): mapa de volta,
x, y, z, parâmetro, tipo. Primeira passagem: grava no bloco de controle do
campo (`[0xCBF9D8]`): +2 mapa, +4 x, +6 y, +0x22 z, +0x24 parâmetro, +0xF2
tipo, zera +0x26 e põe o estado de saída +1 = 0xC; o motor do campo troca de
modo. Segunda passagem (de volta, com +0x26 = 2): limpa +1 e avança o script.

Tipos: 0 moto, 1 chocobo, 2 snowboard, 3 Fort Condor, 4 submarino, 5
montanha-russa, 6 snowboard do Gold Saucer. Extraindo as instruções 0x20
dos scripts do `flevel.lgp` (só as que voltam para o próprio campo, que é o
padrão dos mini-games; o snowboard da geleira volta para `hyou1`):

| Mini-game | Tipo | Parâmetro | Campo | Volta para (x, y, z) |
|---|---|---|---|---|
| G-Bike | 0 | 0 | `games_2` (507) | 507 (243, 176, 92) |
| Corrida de chocobo | 1 | 0 | `crcin_1` (511) | 511 (−372, 33, 81) |
| Snowboard (geleira) | 2 | 64 | `snow` (654) | 658 `hyou1` (−201, 3116, 71) |
| Fort Condor | 3 | 128 | `convil_2` (356) | 356 (9, 114, 17) |
| Submarino | 4 | 0 | `subin_1b` (406) | 406 (−101, −156, 8) |
| Montanha-russa | 5 | 0 | `jetin1` (487) | 487 (411, −12, 50) |
| Snowboard (fliperama) | 6 | 0 | `games_2` (507) | 507 (−37, 273, 26) |

A moto da fuga de Midgar usa o mesmo módulo (modo 6) que o G-Bike.

## Menu do port (v0.6.7, `source/port_menu.c`)

**Segure L + R e aperte Start** (com o menu aberto, o jogo não recebe
nenhum botão; O ou Start + L + R fecha).

- **Mini-games:** os sete acima. Funcionam a partir de qualquer campo (modo
  1): o menu preenche o bloco de controle como a primeira passagem da
  instrução 0x20, na thread do jogo, entre quadros (`usleep` do limitador).
  Terminado o mini-game, o jogo vai para o mapa de volta da tabela. Se o
  bloco ainda estiver no estado do mini-game com +0x26 = 2, o menu o limpa
  como a segunda passagem faria. Se o campo estiver ocupado (evento, menu),
  o pedido espera até ~20 s e desiste (log: `not launched`).
- **Perfil automático nos mini-games** (ligado por padrão): liga o profiler
  no modo `profile_all` quando um mini-game começa e pausa quando termina.
- **Profiler agora:** liga ou pausa à mão (sem a espera de 20 s do boot).

O log marca cada passo (`port menu: launching ...`, `profiler on
(automatic)`, `profiler paused`), para recortar o `profile.bin` por trecho.

**Como medir um mini-game:** em qualquer campo, abrir o menu, escolher o
mini-game, jogar 30–60 s, sair do jogo e enviar `logs.txt` e `profile.bin`.
O `profile.bin` é recriado a cada boot em que o profiler é ligado: um
mini-game por sessão, ou vários seguidos na mesma sessão (o log separa).

O desenho é uma única chamada do pipeline fixo do vitaGL (retângulos
coloridos; fonte 5×7 própria), feita na thread principal com a fila GL
travada, logo antes do swap; o estado GL alterado (programa, blend,
profundidade, cull, scissor, viewport, framebuffer, textura) é salvo e
restaurado.

**Ainda não testado no Vita.** Pontos a observar no primeiro teste: se cada
mini-game abre, se a volta cai no lugar certo e se o campo não reabre o
mini-game sozinho.

## Medição dos mini-games (v0.6.7, pelo menu do port)

Os sete atalhos do menu funcionaram (o log mostra `port menu: launching ...`
e o modo certo). Observação: havia um `profile_on.txt` no cartão, então o
profiler já começou no modo "picos" e o perfil automático do menu não trocou
para o contínuo; os perfis valem para os trechos sem folga da thread do jogo
(o caso que interessa). FPS com o profiler ligado:

| Mini-game | Modo | FPS | Thread do jogo | Situação |
|---|---|---|---|---|
| Fort Condor | 9 | ~58 | 10–30% | ok |
| Submarino | 10 | ~14 | 60% + espera do limitador | no ritmo do motor (~15 fps, como a batalha) |
| Corrida de chocobo | 7 | 16–18 | 95% | limitada pela CPU |
| Montanha-russa | 11 | 15–33 | 94% | limitada pela CPU; ~218 mil `_ftol`/s |
| Snowboard (geleira) | 8 | 11–13 | 80% + espera do áudio | limitada pela CPU |
| Snowboard (fliperama) | 14 | 26–36 | 60–79% | trecho curto |

Ranking da thread do jogo (amostras de `mk_address`/helpers atribuídas a quem chama):

| Mini-game | Funções |
|---|---|
| Snowboard | `0x73488D` 23%, `0x7335FB` 21%, `SL_startPlayer` 16% (ver abaixo), `0x66E272` 11%, `0x7334DA` 7% |
| Chocobo | `0x77B9D4` 25%, `0x66E272` 19%, `0x77BCA4` 12%, `0x66CE40` 8% |
| Montanha-russa | `0x5F04D7` 18%, `0x5F2639` 16%, `0x5F2759` 12%, `0x66E272` 11%, `_check_event` 8%, `0x5F2897` 6%, `0x5F2838` 5% |
| Submarino | `0x79F903` 24%, `0x79FB20` 19%, `0x79A61E` 11% |

## v0.6.8: duas correções que valem para vários modos

**`0x66E272` nativa** (`source/ff7_engine.c`, 11–19% em três mini-games): o
caminho comum (o buffer de vértices da textura é o do quadro atual e tem
espaço) é nativo; reiniciar o buffer num quadro novo ou alocar um novo quando
enche continuam no original (poucas vezes por textura e quadro; o log mostra
quantas em `engine/s: ... buffer allocations by the original`). Teste:
`scripts/emu/test_engine.py` (caminho comum idêntico ao original; nos casos
que precisam do original, a versão nativa recusa sem mexer em nada; duas
mutações detectadas).

**Espera do áudio (snowboard).** As amostras em `SL_startPlayer` caíam no
retorno de duas chamadas ao nosso OpenSL ES: `BufferQueue::Clear` e
`Play::SetPlayState(STOPPED)`. As duas esperavam o mixer confirmar na passagem
seguinte (até um período de saída, ~23 ms), e o snowboard para e reinicia
efeitos sonoros o tempo todo: a thread do jogo passava ~16–20% esperando. O
mixer só tenta (`trylock`) o lock de cada player e pula o que está ocupado;
então, com o lock do player na mão e o mixer parado (`opensles_mixer_idle`,
flag publicada pela thread de áudio em volta da mixagem), a thread do jogo
faz na hora os mesmos passos que o mixer faria (`source/opensles/IPlay.c`,
`source/opensles/IBufferQueue.c`, `source/opensles/Vita.c`). Se o mixer
estiver no meio de uma passagem, a espera antiga continua. Vale também para
os efeitos sonoros de todos os outros modos.

## Próximos alvos (funções próprias)

| Mini-game | Função | Tamanho (x86) | O que tem |
|---|---|---|---|
| Montanha-russa | `0x5F04D7` | 339 instr. | desenho; chama `0x5F2897` (3 `_ftol`), `0x5F26EB`, `0x5F2639`, `0x66E272` |
| Montanha-russa | `0x5F2639`, `0x5F2759`, `0x5F2838`, `0x5F2897` | 28–60 | transformação/projeção x87, pequenas |
| Chocobo | `0x77B9D4`, `0x77BCA4` | 126, 164 | desenho de primitiva: 3–4 vértices por `0x66CE40` + `0x66E272` |
| Snowboard | `0x7334DA` | 59 | x87 puro |
| Snowboard | `0x73488D`, `0x7335FB` | 235, 775 | desenho; `0x7335FB` chama `0x73404C` 4× |

Ordem sugerida: montanha-russa (funções pequenas e puras), chocobo, snowboard
(a maior). Antes, medir a v0.6.8: a `0x66E272` e o áudio já mexem nos três.

## v0.6.9: o gancho esquecido em `0x66E272` (causa do custo e dos reinícios)

Log da v0.6.8 na corrida de chocobo: 16–18 fps, e `ff7_engine: unexpected code
at 0x66E272, left alone` — a versão nativa nem chegou a ser instalada. Causa:
`ff7_bg.c` instalava a cada boot um gancho de gravação em `0x66E272` para o
autoteste e, com autotestes desligados (o padrão), nunca o removia. Toda
chamada a `0x66E272`, em todos os modos, passava por `SO_CONTINUE` (duas
reescritas da entrada e dois flushes de cache): é o "`0x66E272` 11–19%" dos
perfis. Essas milhares de reescritas por segundo também disputavam com o
profiler (sem `profiler_hold`), o padrão que já reiniciou o console antes —
provável causa dos reinícios ao usar o menu com o profiler ligado.

Correções: `ff7_bg` só engancha `0x66E272` se o autoteste for rodar; com isso
a versão nativa da v0.6.8 passa a ser instalada. Os ganchos permanentes que
chamam o original a cada quadro (`ff7_screen`, `ff7_ui`) agora usam
`SO_CONTINUE_HELD` (sob `profiler_hold`).

Perfil da corrida (v0.6.8, antes da correção): `0x77B9D4` 25%, `0x66E272`
20%, `0x77BCA4` 12%, `0x66CE40` 8%. Próximo passo: medir a v0.6.9 (sem
`profile_on.txt` no cartão para o FPS real) e então fazer `0x77B9D4` e
`0x77BCA4` nativas.

## v0.6.10: primitivas da corrida de chocobo nativas (`source/ff7_chocobo.c`)

`0x77B9D4` (triângulo) e `0x77BCA4` (quadrilátero), 25% e 12% da thread do
jogo na corrida: transformam 3 ou 4 vértices pela matriz da câmera
(`0x66CE40`), descartam a primitiva se um vértice está atrás da câmera
(`w ≤ [0x7B7F50]` = 0,0, `test ah, 0x41`), pedem espaço com `0x66E272` e gravam
x/w, y/w, z/w, 1/w e a cor no buffer da textura. Núcleo único em
`source/ff7_chocobo_core.h`; teste `scripts/emu/test_chocobo.py`
(blob ARM ao lado das funções recompiladas, com o `0x66E272` recompilado no
caminho comum): 6.000 primitivas idênticas (registradores, x87, memória,
`_check_event`), com NaN, infinitos, vértices atrás da câmera e textura nula;
três mutações detectadas (a quarta trocava a ordem de uma soma: equivalente).
Chave: `chocobo_off.txt`. Log: `chocobo race primitives/s`.

Junto com a correção do `0x66E272` da v0.6.9, as duas maiores fatias do perfil
da corrida (~57%) deixam de ser código recompilado. Para medir: log da corrida
sem `profile_on.txt`, conferindo `engine/s: ... vertex buffer room native` e
`chocobo race primitives/s` (ambos > 0).

### Resultado da v0.6.10 e v0.6.11

Corrida de chocobo na v0.6.10 (log com o profiler automático ligado): **29,3
fps (o limite) na primeira metade**, antes 16–18; no meio, 18–24 fps. As
versões nativas rodam (até 61 mil triângulos e 32 mil quadriláteros/s, 75 mil
`0x66E272`/s, nenhuma volta ao original). Nos trechos de 21–22 fps o trabalho
é ~36 ms por quadro — logo acima dos 33 ms — e o limitador do jogo espera o
degrau seguinte (~45 ms), por isso aparece espera ativa abaixo de 30 fps: pouca
economia já devolve os 30. Aos 123 s a cena é a mais pesada (77 mil primitivas
desenhadas/s, thread 95%).

No perfil desse trecho, 45% das amostras caíram em `mk_address`: o profiler só
amostra código da `.so`, então o tempo gasto no código nativo do port aparece
na próxima função da `.so` que ele chama — `mk_address`, chamada pelo nativo a
cada palavra lida ou escrita (milhões de vezes por segundo). **v0.6.11:** o
núcleo do chocobo e o `0x66E272` resolvem o endereço uma vez por bloco
contíguo (argumentos, matriz, vértice, estrutura da textura, vértice gravado)
quando ele não cruza página; mesmo resultado (testes do emulador repetidos:
0 divergências), muito menos chamadas.

Restante do perfil da corrida: `_check_event` 6%, `0x770767` 4%,
`0x770305` 4%, `0x77BB50` 4%, `C_006A83AE` 4%, `glEnd` 4%, `0x6840DA` 3,5%,
`0x77BE95` 2,4%, `0x7706D7` 2%.

## v0.6.12: lista de desenho da corrida inteira em nativo

Medição da v0.6.11 (sem profiler): 19–22 fps em boa parte da corrida. O
limitador (`0x779CC6`) roda dois passos de lógica por quadro desenhado quando um
quadro passa de 33 ms, então qualquer quadro acima disso derruba o desenho para
~21 fps com a lógica ainda a 30. No perfil, a cadeia que leva até as primitivas
ainda era x86 recompilado: `0x770305` 4,5%, `0x77BB50` 4,3%, `0x770767` 3,6%,
`0x77BE95` 2,9%, `0x7706D7` 2,8%, além de boa parte dos 24,7% de `mk_address`
(cada `push`/local da pilha x86 dessas funções resolve um endereço).

Agora essa cadeia roda em C (`ff7_chocobo_core.h`):

- `0x770305 (primeira, última)`: percorre as listas de objetos
  (`[0xE7112C]+0x20/+0x1c`, separadores negativos, flag em
  `[0xE715FC][0x18 lista + 0x13] & 0x80`, total em `[0xE71108]`). O tipo do
  objeto (byte 2 da entrada) escolhe triângulo (0), quadrilátero (1) ou
  `0x770434` (0x21, continua x86, chamado com a mesma pilha e os mesmos
  registradores). A flag da primeira lista é o lixo de pilha que o original lê;
  o nativo lê o mesmo lugar e deixa o quadro da pilha como o original deixa.
- `0x770767` / `0x7706D7`: escolhem a textura (`[0xE71C28]`/`[0xE71C44]`,
  `[0xE71BF0]`/`[0xE71C0C]`) e marcam a usada (`+0x14`).
- `0x77BB50` / `0x77BE95`: montam os argumentos e chamam o núcleo das
  primitivas direto, sem passar argumentos pela pilha x86.

A página de globais `0xE71000` é resolvida uma vez por chamada. Ficam como
chamadas x86 só `0x66E272` (nativo do `ff7_engine`) e `0x770434`.

Teste: `scripts/emu/test_chocobo_list.py` (listas aleatórias com os três tipos,
tipos desconhecidos, separadores, flags, textura nula, vértices atrás da
câmera; `0x770434` trocado por um stub que lê argumentos e registradores nos
dois lados): 3.000 chamadas, 0 divergências em registradores, x87, memória,
quadro da pilha, globais, `_check_event` e chamadas a `0x770434`; sete
mutações, todas detectadas. `test_chocobo.py` repetido após a refatoração: 0
divergências. Log: `chocobo race/s: ... lists, ... objects`.

Próximos alvos se ainda faltar: `C_006A83AE` (renderizador de triângulos do
port, ~4%, ainda fora do `ff7_tri`) e `0x6840DA` (~4,5%).

## v0.6.13: `mk_address` e `_check_event` embutidos no nativo

Medição da v0.6.12: a lista nativa roda (até 78 mil triângulos e 79 mil
objetos/s, nenhuma volta ao original), mas a corrida ficou em 22–25 fps.
Separando o perfil pelo endereço de retorno das amostras (o código do eboot não
é amostrado: o tempo dele cai na próxima função da `.so` que ele chama), as
amostras em `mk_address` (36%) e `_check_event` (16%) vieram quase todas do
nativo: `list_native` 20%, `chocobo_prim_do` 20%, `wrap_tri_native` 4%,
`engine_alloc_run` 5%. O nativo chamava as duas funções da `.so` ~40 vezes por
primitiva.

Agora as duas são embutidas em `ff7_chocobo_core.h` e `ff7_engine_core.h`:

- `mk_address(va)`: entrada da tabela de páginas (array estático em
  `.so + 0x101DE5C`) + deslocamento; página não mapeada ou `va` 0 vão para a
  função real (log / página padrão).
- `_check_event()`: incrementa o contador de chamadas (`.so + 0x1492B78`); só a
  2048ª chamada, a que avança os timers, vai para a função real. A sequência de
  eventos é a mesma.

Os endereços são lidos dos `ldr` relativos ao PC no início das duas funções e
conferidos (`source/ff7_so_inline.h`); se não baterem, os nativos ficam
desligados com aviso no log. Testes: o stub de `_check_event` do emulador agora
conta como o real, e os testes conferem o contador final e que o nativo só
chama a função na 2048ª contagem (`test_chocobo.py`, `test_chocobo_list.py`,
`test_engine.py`: 0 divergências; mutações no contador e no endereço
detectadas). Log novo: `draw list X ms/s` (tempo dentro de `0x770305`).

## v0.6.14: correção da v0.6.13 (que desligou os nativos)

A v0.6.13 não melhorou nada porque **não rodou**: o log do Vita mostra
`ff7_engine: unexpected .so layout` e `ff7_chocobo: unexpected .so layout`, e a
corrida ficou sem nenhum nativo (17–19 fps, trabalho 85–95%). A busca da tabela
de páginas em `ff7_so_inline.h` somava 4 bytes a mais ao PC do `add r3, pc` do
`mk_address`, então a conferência falhava e cada módulo se desligava inteiro. Os
testes do emulador não pegaram: recebem os ponteiros prontos e não rodavam a
busca.

Correções:

- O cálculo do endereço (`mk + 6 + 4` + literal).
- `scripts/emu/test_so_inline.py`: roda a busca de verdade (blob ARM sobre a
  `.so` carregada) e confere a tabela e o contador.
- Uma falha da busca não desliga mais os nativos: eles voltam a chamar
  `mk_address` / `_check_event` (como na v0.6.12), com o aviso
  `mk_address/_check_event not as expected, calling them (slower)`.

No log da v0.6.14 não deve aparecer nenhum `unexpected .so layout` nem esse
aviso, e a linha `chocobo race/s` deve voltar com o `draw list X ms/s`.

## v0.6.15: caminho lento do `0x66E272` sem `SO_CONTINUE`; área da corrida

**Corrida (log da v0.6.14).** Os nativos voltaram: 29,3 fps na maior parte da
corrida, caindo para ~22 nas cenas mais cheias. Nelas a lista de desenho
custa 300–430 ms/s (`draw list`), com ~77 mil objetos/s. O `0x66E272` cai no
caminho lento (buffer novo) ~2.800 vezes/s, e cada vez voltava ao original
via `SO_CONTINUE`: duas reescritas da entrada e duas limpezas de cache. Agora
um trampolim (`alloc_original` em `ff7_engine.c`) faz o que os 8 bytes cobertos
pelo hook fariam (`push {r3-r5, lr}`; r4 = literal + PC do `add r4, pc`) e
salta para `entrada + 8`, sem reescrever código. As instruções são conferidas
ao instalar (senão volta ao `SO_CONTINUE`; o log diz qual). Teste:
`scripts/emu/test_alloc_tramp.py` (entrada sobrescrita como pelo hook,
chamada pelo trampolim × função intacta: 400 chamadas, 0 divergências; erro de
4 bytes no r4 detectado).

**Área depois da corrida (campo, 16 fps parado).** Trabalho 88%, e o log
mostra `textures/s: 156 fast, 227 K pixels, 273 ms converting`: ~10 texturas
por quadro reconvertidas (paleta animada) e reenviadas (`tex 156 uploads,
40 MB, 190 ms`). No perfil manual, `convert_image_data` tem 27,5%. Os pixels
são poucos (~1.450 por textura); o custo era por chamada:

- a tabela de cores do destino (1.024 divisões em software) era refeita a cada
  textura: agora só quando o formato muda;
- as 256 entradas da paleta passavam por `C_006865F0` (um `mk_address` cada):
  agora a paleta é lida direto (mesma regra: entrada `stride × paleta + i`,
  `0xffff0000` fora do tamanho ou sem paleta). Conferido contra a função
  original no emulador: 3.000 chamadas, 0 divergências.

O log ganhou `ms 8-bit setup`, `format reuses`, `>= 256x256` e `largest WxH`,
para medir quanto sobra e se o resto está no envio das texturas
(`gl/s: tex ... uploads`).

## v0.6.16: cenas gravadas no menu do port (marcador + profiler)

Dois itens novos no menu (Start + L + R):

- **GRAVAR CENA (MARCADOR + PROFILER)**: salva onde o jogador está e liga o
  profiler; apertar de novo pausa o profiler. O log marca o início e o fim com o
  número da cena (`port menu: scene N recording ...` / `... stopped`), e eu
  recorto o `profile.bin` por esses tempos. Basta mandar `profile.bin` e
  `logs.txt` da sessão. O `profile.bin` é recriado a cada sessão do jogo; as
  cenas ficam.
- **IR PARA CENA < N/M >**: escolhe a cena com esquerda/direita e X leva até
  ela, para medir de novo depois de uma mudança.

As cenas ficam em `ux0:data/ffvii-vita/cenas.txt`, uma por linha (`cena modo
mapa x y triângulo direção`), e podem ser apagadas ou editadas à mão (até 16).

Como funciona:

- **Posição do jogador:** é lida da entidade do jogador (`[bloco + 0x2A]`,
  gravada pelo opcode `PC`). As entidades ficam em `[0xCC0B60]`, 0x88 bytes
  cada: x/y/z com <<12 em +0xC/+0x10/+0x14, direção em +0x38 e triângulo do
  walkmesh em +0x78, como os opcodes `XYZI` e `DIR` escrevem. O mapa atual vem
  de `[0xCC15D0]`.
- **Ida a um campo:** faz o que o opcode `MAPJUMP` (0x60, handler 0x6131C4)
  faz: grava mapa, x, y, triângulo e direção no bloco de controle (+2, +4, +6,
  +0x22, +0x24), +0x26 = 0 e estado de saída +1 = 1. Quando o pulo termina
  (+0x26 = 2), limpa o estado, como o segundo passo do opcode.
- **Ida a uma cena de mini-game:** abre o mini-game pelo atalho que já existia.
- **Restrições:** só funciona a partir de um campo. Mapa-múndi e batalha
  podem ser gravados (o profiler liga), mas não dá para voltar a eles.
- **Só o lugar é restaurado**, não o estado do jogo: história, grupo e itens
  continuam os do save atual, então NPCs e eventos podem estar diferentes.

**Ainda não testado no Vita.** Observar se o pulo cai na posição certa e se
o campo continua respondendo depois dele.

## v0.6.17: rancho de chocobos (paleta animada) e snowboard

### Rancho de chocobos: reconversão incremental das texturas

Log da v0.6.16 parado na área do rancho: 16 fps, com o jogo trabalhando 85% do
tempo. São ~155 conversões de páginas de 256×256 por segundo (~10 por quadro),
238 ms/s convertendo (só 15 ms disso na preparação). O perfil mostra
`gfx_palette_changed`: a paleta animada muda o conteúdo das cores, e
`gfx_palette_changed` → `gfx_load_texture` reconverte a página inteira no mesmo
buffer (`+0xD8` do conjunto de texturas) e cria uma textura GL nova. O envio
(`glTexImage2D`, 190 ms/s) roda na thread de render, em outro núcleo.

`source/ff7_texture_cache.h`: para cada buffer de destino guarda as 256 cores
da última conversão, um hash dos índices e quantos pixels usam cada índice.
Quando a mesma página volta ao mesmo buffer (mesma origem, tamanho, pitch e
hash) e uma amostra de 32 pixels confirma que o buffer ainda tem a última
conversão:

- só os pixels cujas cores mudaram são reescritos;
- nada é escrito se nenhuma cor usada mudou;
- se a mudança atinge mais de 1/4 da página, a conversão é completa.

O resultado é sempre o da conversão completa. Teste no Mac
(`scripts/test_texture_cache.c`): 20.000 conversões com animação de poucas e de
muitas cores, índices alterados, buffer reescrito por fora, páginas invertidas
e mais buffers que entradas do cache, 0 divergências; 4 mutações detectadas.
Chave: `texcache_off.txt`. O log `textures/s` agora mostra pixels médios por
conversão e as reconversões completas / parciais / sem mudança.

### Snowboard: transformação e recorte de triângulos nativos (`source/ff7_snowboard.c`)

No perfil de uma descida inteira da v0.6.16 (thread do jogo 96% ocupada, 16–24
fps), três funções e os `mk_address` delas somavam ~65% da thread:

- `0x7335FB` (recorte do triângulo no plano z = 0,5): 14,5% + 12,9%;
- `0x73488D` (recorte + gravação no buffer da textura): 13,1% + 13,6%;
- `0x7334DA` (vértice × matriz): 8,3% + 2,9%.

Agora rodam em C (`ff7_snowboard_core.h`), assim como o invólucro `0x734CAB`
(2,2% + 0,9%):

- `0x73488D` faz o recorte de `0x7335FB` (0, 1, 2 ou 3 vértices atrás do
  plano), a interpolação de cores de `0x73404C` com o `_ftol` do jogo e grava 1
  ou 2 triângulos (x/w, y/w, z/w, 1/w, cor) com espaço pedido ao `0x66E272`.
- `0x7334DA` multiplica em double, como o x87 recompilado, e grava em float.

Teste: `scripts/emu/test_snowboard.py` (blob ARM ao lado das funções
recompiladas): 6.000 chamadas, 0 divergências em registradores, x87, memória e
`_check_event`, com todos os casos de recorte, NaN, infinitos, w = 0 e textura
nula. 14 de 16 mutações detectadas; as 2 restantes são equivalentes. Chave:
`snowboard_off.txt`. Log: `snowboard/s: ... transforms, ... triangles (...
clipped, ... drawn) native`.

Próximos candidatos se ainda faltar: `0x7322D6` (2,8%), `0x72700C` (2,3% +
1,2%, também chama `0x73488D`) e `0x732159` (1,9%).

## v0.6.18: áudio do snowboard (diagnóstico)

Log enviado com o áudio do snowboard travando e sem música: é da **v0.6.16**,
então sem os nativos da v0.6.17.

**Como a música do snowboard toca.** Quatro faixas sempre tocam como WAV, e
não pelo player `.akb` do Android (`SND_play`): `SATO` (15), `WIND` (65, o
snowboard), `SENSUI` (88, o submarino) e `HEART` (97). Quem classifica é
`0x7416A3`. O próprio código do PC faz o streaming:

- `0x7478BD` abre `<nome>.wav` (`wind.wav`, ADPCM, convertido pelo ACM
  emulado);
- um timer multimídia (`timeSetEvent` de 80 ms, callback `0x74656C`, avançado
  pelo `timeEvent_step` dentro do `_check_event`) chama `0x748950(stream)`
  para reabastecer um buffer DirectSound em loop;
- se `0x748950` devolve ≠ 0, o callback desliga o timer e o arquivo não é mais
  lido.

**O que o log mostra.** `wind.wav` foi lido por ~9 s (540 KB de 1,36 MB) e
depois nunca mais (`obb/s: 0 KB` até o fim da descida). O stream terminou cedo.
`0x748950` dá o stream por acabado quando liga `[stream + 0x34]`; ela calcula o
avanço pela posição de reprodução do buffer (`GetCurrentPosition`, que na
`.so` vem de `SL_getCurrentPosition` sobre o nosso OpenSL) e pelos pontos de
loop. A suspeita é uma posição que não avança ou volta errado.

`source/ff7_sound_diag.c` (ligado nesta versão; `sound_diag_off.txt`
desliga) registra no log, com até 400 linhas por sessão:

- cada buffer criado (flags, tamanho, formato), `Play` e `Stop`;
- as primeiras 40 chamadas de `0x748950` de cada stream e toda chamada que
  muda as flags ou encerra o stream: modo, flags de loop e fim, tamanho,
  posição, loop e formato;
- posições do `GetCurrentPosition` que voltam sem dar a volta no buffer.

Com um log de uma descida dá para ver em que passo o stream acaba e corrigir.

**Glitches gráficos** (mesmo log, v0.6.16): os nativos que rodam no snowboard
são o renderizador de triângulos, o `0x66E272` do `ff7_engine` (~900 caminhos
lentos/s, pelo trampolim desde a v0.6.15) e, a partir da v0.6.17, os do
snowboard. Para isolar: repetir com `snowboard_off.txt`, depois com
`engine_off.txt`, depois com `highway_off.txt` em `ux0:data/ffvii-vita/`.

## v0.6.19: música do snowboard, 30 fps no snowboard

Log da v0.6.18: com os nativos o snowboard chega a 60 fps; a música continua
travando.

**O diagnóstico mostrou a causa provável do travamento.**

- O buffer do stream (`wind.wav` decodificado: 90.112 bytes, estéreo, 44,1
  kHz, 16 bits) é criado e tocado em loop.
- Durante a descida, o jogo envia o comando de som "resume" a cada quadro.
  `resume_sound` (`0x745995`) chama `Play` em todo buffer cuja posição não é 0
  (`0x74A758` = `GetCurrentPosition`): o da música e o de um efeito, a cada
  quadro.
- No DirectSound, `Play` num buffer que já toca não faz nada. O
  `fw_IDirectSoundBuffer_Play` da `.so` passa toda chamada ao `SL_startPlayer`
  com a posição guardada do último `GetCurrentPosition` (degraus de 16 KB), e a
  música recomeçava de posições velhas.

**Correção** (`source/ff7_dsound.c`): o `Play` só chega ao código da `.so`
quando o `GetStatus` dela diz que o buffer não está tocando, ou quando a flag de
loop muda; um `Stop` libera o próximo `Play`. Chave:
`dsound_play_fix_off.txt`. O diagnóstico continua ligado (`sound_diag_off.txt`
desliga), agora sem as linhas dos `Play` ignorados, com o fim do stream (`refill
END`) e um resumo a cada 5 s (refills, `Play` repassados e ignorados, estado do
stream).

**30 fps no snowboard** (`ff7_snowboard.c`). O quadro do snowboard
(`0x72381C`) roda passos fixos de lógica de 60 Hz pelo tempo medido
(`0x723960`: tempo × 60 em `[[0x926290] + 0x268]`, até 32 passos) e desenha uma
vez. Desenhando a 30 fps, são 2 passos por quadro e a velocidade do jogo é a
mesma. A entrada de `0x72381C` espera até completar 33,3 ms do quadro anterior.
Chave para voltar a 60: `snowboard_60fps.txt`. Log: `snowboard cap: N
frames/s, N ms/s waiting`.

**Árvores** (prints): a árvore "deitada" aparece com o Cloud caído depois de
bater, e a câmera entra na pedra. O nativo do snowboard reproduz o recorte do
original bit a bit (teste do emulador), e glitches já tinham sido vistos na
v0.6.16, sem ele. Para confirmar se é do jogo: repetir a cena com
`snowboard_off.txt`.

## v0.6.20: árvores do snowboard (renderizador C_006A8A3E) e volume da música

**Árvores esticadas** (prints: textura de árvore ampliada sobre paredes e chão).
O nativo de `C_006A8A3E` (renderizador de triângulos da moto, também usado no
snowboard, nativo desde as versões da moto) diferia do original em duas coisas:

- o original chama `fake_glColor4ub` uma vez por triângulo, com a cor do
  **primeiro** vértice (triângulo de cor sólida), e o nativo usava a cor de
  cada vértice;
- o original chama `glTexCoord2f` por vértice, então a UV corrente fica com a do
  último vértice; o nativo não atualizava `CUR_U`/`CUR_V`. Os desenhos
  seguintes que emitem vértices sem mandar UV herdavam uma UV velha, e a textura
  esticava pelo cenário.

O teste antigo (`test_tri2.py`) não comparava cores nem a UV corrente: o
emulador não resolvia as entradas do GOT de símbolos da própria `.so`. Agora
`Emu(resolve_defined=True)` resolve essas entradas, e
`scripts/emu/test_tri2_exact.py` roda a função original de verdade
(`fake_glColor4ub`, `glTexCoord2f`, `glVertex3f`) contra o núcleo num blob ARM
e compara tudo: vértices com cor, `pCurVertex`, cor e UV correntes. O núcleo
novo dá 0 divergências em 600 chamadas; o da v0.6.19, 196 em 200.

**Música do snowboard** (log da v0.6.19). Com a correção do `Play`, o stream
anda: reabastece o tempo todo, lê o arquivo em tempo real (~44 KB/s de ADPCM)
e volta ao ponto de loop. Mesmo assim não se ouve nada, então o problema está
depois dos dados. O volume que o código do PC dá ao buffer é a tabela `0x967660`
(-10000 = mudo no índice 0), indexada por `[0xDE6558] × [0xDE6584] / 100`
(volume mestre de música × volume atual, que os fades mudam). O diagnóstico
agora registra `SetVolume`, `SetPan` e `SetFrequency` do buffer da música e
essas variáveis no resumo de 5 s. A chave de teste `wavmusic_fullvolume.txt`
força volume cheio no buffer da música.

## v0.6.21: isolar o glitch das árvores; nível do som da música

Log da v0.6.20: o glitch das árvores continua e a música também.

**Música.** O volume não é a causa. O jogo põe o buffer da música (o único
estéreo) em 0 dB (`SetVolume 0`, mestre 100, atual 127) e nunca muda pan nem
frequência. O player OpenSL é criado normalmente, e um buffer estéreo de 16 bits
a 44,1 kHz é enfileirado sem cópia. O stream anda e lê o arquivo em tempo real.
Sobra o conteúdo do buffer: o `wind.wav` é MS-ADPCM (4 bits), decodificado pelo
ACM emulado da `.so` e escrito pelo código x86 nos ponteiros que o `Lock` da
`.so` devolve (endereços de host). O diagnóstico agora registra os `Lock` do
buffer da música: ponteiros, tamanhos e se o endereço x86 é a mesma memória que
o host. O resumo de 5 s traz também o pico do PCM no buffer (0 = silêncio).

**Árvores.** A correção do `C_006A8A3E` não resolveu, então a causa está em
outro lugar. `0x72700C` desenha os blocos de terreno sem textura, não as
árvores. Para separar um defeito do port de um comportamento do próprio jogo:

- `natives_off.txt` (novo, `source/ff7_switch.h`) desliga todos os nativos de
  desenho e matemática de uma vez: motor, snowboard, triângulos, seno/cosseno,
  iluminação, `ftol`, texturas, chocobo e moto;
- chaves novas por módulo: `tri_off.txt` (`C_006A8002`), `tri2_off.txt`
  (`C_006A8A3E`), `math_off.txt` (seno/cosseno), `light_off.txt`
  (iluminação), `texture_off.txt` (conversão de texturas). As antigas
  continuam (`engine_off.txt`, `snowboard_off.txt`, `ftol_off.txt`,
  `texcache_off.txt`...).

Com `natives_off.txt`, se o glitch continuar, é do jogo; se sumir, as chaves
individuais apontam o módulo. O log de cada sessão mostra quais estão
desligados (`<módulo>: disabled by ...`).

## v0.6.22: crash do diagnóstico de som da v0.6.21

A v0.6.21 travava ao entrar no snowboard, com ou sem `natives_off.txt`. Os
dois core dumps (`scripts/parse_core.py`, novo) mostram a thread do jogo parada
em `0x81023CB0`, dentro do `refill_native` de `ff7_dsound.c` (o `music_peak`
inline). O `Lock` da `.so` devolve ao código x86 **endereços x86** (o buffer de
som fica na memória x86, ex.: `p1 029c550c`), e o diagnóstico lia esse valor
como ponteiro do host. A mensagem "OTHER memory" do log também estava errada
pelo mesmo motivo: os dados vão para o buffer certo.

Agora o pico do PCM é lido pela memória x86 (`mk_address`), e o log só mostra o
endereço x86 e o endereço de host correspondente. Os testes da v0.6.21 continuam
valendo: uma descida normal para o diagnóstico da música (pico do PCM no resumo
de 5 s) e `natives_off.txt` para as árvores.

## v0.6.23: a música chega ao buffer; diagnóstico no OpenSL

Log da v0.6.22 (com `natives_off.txt`): árvores ainda com glitch e música muda.

**Música: o lado do jogo está certo.**

- O PCM decodificado chega ao buffer da música: pico de 900 a 6.350 (de 32.767)
  a cada 5 s, ou seja, som de verdade.
- O jogo faz ~54 `Lock` de 16 KB a cada 5 s, 176 KB/s, exatamente estéreo 16
  bits a 44,1 kHz.
- O buffer fica na memória x86. A `.so` cria o player com o endereço de host
  dele (`mk_address`, memória contígua) e o declara como 16 bits, 44,1 kHz,
  máscara de canais 3.
- O volume é 0 dB.

O som some depois, entre o buffer e a saída. A única diferença em relação aos
efeitos, que tocam, é que este é o único buffer estéreo.

`source/opensles/IBufferQueue.c` agora registra (`bq diag`, a cada 5 s, só
filas estéreo de 16 bits): buffers e bytes enfileirados, o pico das amostras e,
do player, estado, mudo, nível em mB, os ganhos que o mixer aplica e a trilha
do mixer. Assim fica claro se a `.so` entrega a música ao OpenSL e com que
ganho.

**Árvores.** Com os nativos de desenho e matemática desligados, o glitch
continua. Sobram as reescritas de endereço do port, que mudam o próprio código
recompilado: `ff7_memmap` (endereços constantes; `memmap_off.txt`) e
`ff7_stack` (endereços da pilha x86; `stack_off.txt`). Próximo teste:
`natives_off.txt` + `memmap_off.txt` + `stack_off.txt` juntos. Se o glitch
continuar, ele não vem de nenhuma otimização do port; resta o próprio jogo
(versão Android) ou o vitaGL.

## Snowboard: estado em aberto (pausado na v0.6.24)

O que já se sabe e o que foi tentado, para retomar depois.

**Música (faixa WIND, `wind.wav`, a única estéreo)** — ainda muda.

| Versão | Tentativa | Resultado |
|---|---|---|
| 0.6.18 | diagnóstico do stream (`ff7_dsound.c`) | stream lido por ~9 s e parado |
| 0.6.19 | `Play` repetido ignorado (o jogo manda "resume" a cada quadro) | stream passa a andar em tempo real, em loop |
| 0.6.20 | log de volume/pan/frequência | volume 0 dB, pan e frequência nunca mudam |
| 0.6.21–22 | `Lock` e pico do PCM (crash na 0.6.21, corrigido) | o PCM decodificado chega ao buffer (pico 900–6.350), 176 KB/s |
| 0.6.23 | diagnóstico no `Enqueue` do OpenSL (`bq diag`) | ainda sem log de snowboard |

Descartado: volume, fade, player OpenSL não criado, formato errado, endereço
errado do buffer, stream parado. Falta ver, com o `bq diag`, se os blocos do
buffer da música chegam ao `Enqueue` do OpenSL e com que ganho. A chave
`wavmusic_fullvolume.txt` força volume cheio (teste).

**Árvores esticadas** — continuam.

- O nativo `C_006A8A3E` foi corrigido (cor e UV corrente: `test_tri2_exact.py`),
  mas não era esta causa.
- Com `natives_off.txt` (todos os nativos de desenho e matemática desligados) o
  glitch continua: não vem deles.
- Ainda não testado: `natives_off.txt` + `memmap_off.txt` + `stack_off.txt` (as
  reescritas de endereço, únicas otimizações que ficam ligadas).
- Pista (v0.6.24, abaixo): a mesma família de glitch aparece na montanha-russa,
  em triângulos que encostam no plano próximo e vão para o caminho de recorte
  (polígonos adiados, desenhados pelo renderizador do próprio port Android).

## v0.6.24: montanha-russa a 30 fps e nativa

Log da v0.6.23: 18–60 fps, thread do jogo 96% ocupada, sem espera de
limitador (roda o mais rápido possível).

**30 fps** (`source/ff7_framecap.c`, novo; o limite do snowboard foi para lá
também). O quadro da montanha-russa segue o esquema do snowboard: `0x5E8F9B`
soma o tempo × 60 em `[0xC3F6E8]` (até 32 passos), e `0x5E8E7E` (a função
principal do modo, uma vez por quadro) roda os passos de lógica de 60 Hz e
desenha só no último (`[0x9014A8]`). A entrada de `0x5E8E7E` espera até
completar 33,3 ms: 2 passos por quadro, mesma velocidade de jogo. Chaves:
`coaster_60fps.txt`, `snowboard_60fps.txt`. Log: `coaster cap: N frames/s`.

**Nativos** (`source/ff7_coaster.c`, `ff7_coaster_core.h`). No perfil:
`0x5F2639` 16,6% + 2,4%, `0x5F04D7` 15,4% + 3%, `_ftol` 14,8%, `0x5F2759` 11,5%
+ 2,4%, `0x5F2838` 4,6%, `0x5F2897` 2,6%. Tudo vira C:

- `0x5F04D7`, um triângulo: profundidade e fade dos 3 vértices; nada se nenhum
  está na frente; se algum está a menos de -100, o caminho de recorte vai para o
  original (por trampolim, sem `SO_CONTINUE`); senão, espaço pelo `0x66E272` e 3
  vértices projetados com cor esmaecida (2º vértice no slot 2, 3º no slot 1);
- auxiliares, também nativos sozinhos: `0x5F2639` (projeção, `0x66CE40` em float,
  w = 0 vira 0,1), `0x5F2759` (profundidade), `0x5F2838` (fade entre
  `[0xC3F750]` e `[0xC3F754]`) e `0x5F2897` (cor × fade, 3 `_ftol` embutidos).

Teste: `scripts/emu/test_coaster.py`: 3.000 chamadas, 0 divergências em
registradores, x87 (incluindo o valor deixado na pilha), memória e
`_check_event`. Quando o nativo recusa (recorte ou NaN), o teste confere que ele
não mudou nada. 9 de 10 mutações detectadas; a restante é equivalente (ordem de
uma soma comutativa). Chave: `coaster_off.txt`. Log: `coaster/s: ...`.

**Glitches ("efeitos 3D puxados do nada").** Não vêm destes nativos: o caminho
desenhado direto reproduz o original bit a bit, e os glitches já existiam na
v0.6.23, sem eles. Pelo código de `0x5F04D7`:

- triângulos com um vértice perto da câmera (profundidade < -100) não são
  desenhados ali: viram polígonos adiados (`0x672C61` aloca o registro,
  `0x66C4F0` põe uma matriz identidade, vértices em espaço de câmera, z do
  plano 0,5 em +0x6C) para o recorte do motor;
- esses polígonos são desenhados depois pelo renderizador do próprio port
  Android. A família `C_006A8xxx` (`C_006A8002` descarta o triângulo com
  w ≤ 0; `C_006A83AE` desenha vértices já projetados sem UV própria) é código C
  do port, não do PC.

A mesma família de glitch aparece nas árvores do snowboard. Próximo passo:
seguir o caminho dos polígonos adiados até o desenho e comparar com o
`natives_off.txt` + `memmap_off.txt` + `stack_off.txt`.

## v0.6.25: som do menu na primeira vez; testes A/B no menu do port

Log da v0.6.24: montanha-russa a 30 fps firmes (folga de 360–960 ms/s; 19–32
mil triângulos nativos/s, recorte pelo original 4–97/s), mas com texturas 3D
piscando. E o primeiro som de um menu, ou o de carregar o save, não sai.

**Som do menu.** O efeito do cursor (`0214c4e4`) toca em pares `Stop`/`Play`, e
o resumo do diagnóstico mostra `Play` ignorados nesse trecho. A correção da
v0.6.19 (ignorar `Play` num buffer que a `.so` diz estar tocando) valia para
todo buffer. Num efeito curto que terminou, o estado "tocando" da `.so` só muda
quando ela consulta a posição, então o `Play` seguinte era engolido. Agora só
buffers em loop (a música em stream) têm o `Play` repetido ignorado; efeitos de
uma vez passam sempre, como antes da v0.6.19.

**Texturas piscando: testes A/B no menu do port** (Start + L + R). Três itens
novos ligam e desligam na hora, sem reiniciar:

- `A/B NATIVOS DOS MINI-GAMES`: os nativos da montanha-russa e do snowboard
  (hooks tirados e repostos, `ff7_coaster_set` / `ff7_snowboard_set`);
- `A/B LIMITE DE 30 FPS` (`ff7_framecap_set`);
- `A/B CACHE DE TEXTURAS`: a reconversão incremental (`ff7_texture_cache_set`;
  religado, o cache começa vazio).

As mudanças são aplicadas na thread do jogo entre quadros
(`port_menu_apply_ab`: no `usleep` do limitador e na entrada do quadro dos
minigames em `ff7_framecap.c`, já que eles nunca chamam `usleep`). O log
registra cada troca (`port menu: A/B ...`). Para achar a causa do piscar: na
montanha-russa, desligar um de cada vez e ver qual faz parar.

## v0.6.26: núcleo do submarino integrado, alvo de 30 fps

Continuação dos três arquivos deixados em andamento pela sessão anterior
(`ff7_submarine_core.h`, `blob_submarine.c`, `test_submarine.py`). Agora o
núcleo é instalado por `source/ff7_submarine.c`, registrado no build, na
inicialização e no relatório periódico:

- `0x79F903`: atualização das 4.096 células do mar, com as alturas copiadas
  aos vizinhos, flags e contadores. Resolve o bloco inteiro uma vez quando
  as páginas são contíguas; mantém o caminho para páginas separadas.
- `0x7A2EF8`: contagem dos bits iniciais pelas duas tabelas do jogo.
- `0x79A61E`: triângulos dos respingos e quatro paredes das colunas d'água,
  transformação/projeção e cores. A preparação da matriz e a reserva do
  buffer continuam chamando as funções do jogo; `0x66E272` já tem o caminho
  comum nativo do motor. Memória e eventos usam os mesmos atalhos verificados
  dos outros mini-games, com fallback se o layout não corresponder.

**30 fps.** O submarino já limita a lógica em 30 Hz: `0x77F56D` inicializa
`[0xE74350]` com `[game + 0x30] / 30.0 - 10000.0` (as constantes estão em
`0x7B7F60` e `0x7B7F68`); `0x78C9E1` compara o tempo decorrido com esse
período e controla `[0xE73F18]`, repetindo a lógica e pulando o desenho
quando atrasa. Portanto conserva-se esse limitador; não se acrescenta a
espera usada pelo snowboard e pela montanha-russa, que têm lógica a 60 Hz.
A otimização busca fazer o trabalho caber no período existente de ~33 ms.
Ainda é preciso medir no Vita para afirmar 30 fps sustentados.

**Correções durante a validação.** O gerador do teste usava um inteiro com
sinal para uma flag aleatória de 32 bits, falhando antes da comparação.
Na tradução, faltava uma sobrescrita peculiar do original na última coluna:
o z do canto C inferior da terceira parede recebe o z de A, exceto na última
linha, que o sobrescreve novamente. O canto D inferior conserva seu z de
antes dos ajustes da borda. A comparação ampliada detectou a divergência e
a tradução agora reproduz ambos. Também foi removido um deslocamento à
esquerda de valor negativo em C no cálculo da altura inferior.

**Validação.** `scripts/emu/test_submarine.py` compila o núcleo como blob ARM
e roda o original recompilado a partir do mesmo estado. Confere os oito
registradores, estado de controle x87, memória observável e `_check_event`.
A rodada aleatória após a correção passou em 240 chamadas (30 da grade, 30
da contagem, 180 dos respingos), sem divergências. Os casos dirigidos
acrescentam desenho garantido nos quatro cantos e no interior da grade,
texturas nulas, fade, vértices atrás da câmera/NaN, páginas contíguas ou
separadas e os caminhos com e sem os atalhos de memória/eventos. A rodada
final com esses casos passou em outras 240 chamadas (30/30/180), com 4.224
triângulos e 477 colunas desenhados e nenhuma divergência. O teste
agora também falha se o emulador esgotar o limite de instruções sem retornar.
Três mutações em cópias temporárias do header foram detectadas: deslocar a
gravação de um vizinho, zerar o ECX da contagem de bits e retirar a correção
do canto C inferior na última coluna.

```sh
source scripts/env.sh
python3 scripts/emu/test_submarine.py build/libjni_ff7.so /caminho/ff7_en.exe 240
cmake --build build -j8
```

No macOS ARM, o Unicorn ARM local encerrou com `Illegal instruction` ao
mapear memória; a validação foi executada com Python e Unicorn x86_64 via
Rosetta. O `ff7_en.exe` usado é o da própria cópia local do OBB.

**Teste no Vita.** VPK: `build/VPK/ff7_vita_v0.6.26.vpk`, compilado com a
checagem de conteúdo aprovada. Abrir o submarino pelo menu (L + R + Start),
jogar 30–60 s com respingos/explosões, testar a volta ao campo e comparar
`perf:` e `submarine/s:`. Para o FPS real, desligar o perfil automático no
menu e remover `profile_on.txt`/`profile_all.txt` se existirem. O item
`A/B NATIVOS DOS MINI-GAMES` também passa a alternar os três nativos do
submarino entre quadros; `submarine_off.txt` desliga só este módulo no boot e
`natives_off.txt` continua valendo. O item A/B do limite de 30 fps continua
restrito aos limitadores adicionais do snowboard/montanha-russa.

**Próximo alvo, após medir:** `0x79FB20` (outra fatia grande do perfil do
submarino) ainda roda no original. Os glitches da montanha-russa/snowboard
e a investigação do áudio não foram alterados nesta versão.

## v0.6.27: superfície do mar nativa e Quadrado entregue na thread do jogo

**Medição da v0.6.26 enviada pelo usuário.** `build/VPK/logs.txt` mostra os
nativos instalados e, na maior parte do submarino, 27–28 fps com o perfil
ligado. O perfil contém 10.705 amostras, 10.341 da thread do jogo:

| Função | Amostras da thread do jogo |
|---|---:|
| `mk_address` | 28,01% (61% chamadas por `0x7A0190`) |
| `0x7A0190` | 23,91% |
| `0x66CE40` | 5,25% |
| `_check_event` | 4,97% |
| `0x79FB20` | 4,35% |
| `0x77DF72` | 4,25% |

Logo, o maior alvo atual é `0x7A0190`: ~41% somando sua própria execução e
suas chamadas a `mk_address`, além das transformações. `0x79FB20` continua
original, mas não é mais a prioridade do perfil recebido.

**Superfície (`ff7_submarine_core.h`, `ff7_submarine.c`).** `0x7A0190`
agora percorre em C a região de 24×24 células ao redor da câmera. Faz as
quatro transformações por célula em float, preserva a ordem das contas,
a comparação do plano próximo e os caminhos de cor por vértice/cor média.
Grava seis vértices por quadrado: projetados quando todos estão à frente;
coordenadas originais no buffer adiado quando há recorte ou projeção
ortográfica; descarta quando os quatro estão atrás. A preparação das
matrizes das texturas e a reserva do buffer mantêm as funções do jogo.
Resolve os dados contíguos de cada ponto sem passar pelo resolvedor x86 a
cada campo. O modo de depuração de grade (`0xE7475C & 0x40`) continua no
original; o núcleo recusa antes de alterar estado. O limitador de 30 Hz
continua intacto. A/B dos nativos e `submarine_off.txt` incluem a superfície.

**Quadrado (`ff7_submarine_input.c`).** O mapeamento original foi conferido
no emulador: botão Android 6 → `st_keyboard[0x53]` (Delete) → ação `0x80`
pelo `ff7input.cfg` original. `0x798580` testa a borda dessa ação e chama
`0x79F114`, o disparo. O log antigo não registrava entrada nem tentativas de
tiro, portanto não permite afirmar em qual etapa as pressões relatadas se
perderam ou se o jogo recusou os disparos.

Agora, somente no modo 10, o Quadrado físico usa uma fila de pressões
publicada pela thread principal e consumida pela thread do jogo na entrada
de `0x798580`. Um toque curto continua pendente mesmo depois de soltar;
pressões seguidas são entregues uma por passagem. Ele não percorre também
o caminho do teclado Android, evitando duplicar a mesma pressão. Os outros
botões/modos e o controle virtual conservam o caminho original. A fila é
limpa ao sair do modo 10. Não há repetição automática ao segurar.

O disparo continua passando pelas verificações originais: um dos quatro
tubos em `0x9873E4` deve estar em `0x20000`, deve haver uma das 16 entradas
livre (`0xEC18B4 + 0xC8*i == 1`) e uma entidade elegível no conjunto de 12
registros (`0x98A1E0 + 0x70*i`, flag `0x800`). O código não força um torpedo
quando essas condições falham. Os ganchos de entrada/disparo usam
trampolins que reproduzem os primeiros oito bytes verificados da `.so`, sem
reescrever o código a cada quadro. A correção de entrada é independente do
A/B de desenho e de `natives_off.txt`; `submarine_input_off.txt` restaura
somente o caminho de entrada antigo no boot.

**Diagnósticos, a cada heartbeat (~5 s):**

- `submarine surface:` quadros/s, ms/quadro da rotina, quadrados projetados,
  adiados e descartados por segundo, quadros de depuração pelo original;
- `submarine fire:` pressões de Quadrado, bordas entregues, chamadas de
  disparo, torpedos criados (tubos consumidos), recusas por recarga, falta de
  entrada livre ou entidade elegível, fila restante e fase (`0xE74768`).
  Os contadores de tiro usam operações atômicas entre as duas threads.

**Validação:** 40 quadros da superfície (23.040 células), sem divergências
em memória, registradores, controle x87 ou eventos. Regressão conjunta de
80 chamadas: 10 grade, 10 bits, 50 respingos e 10 superfície, sem
divergências. Outra rodada de 24 chamadas da superfície verificou 22 quadros
nativos (8.064 quadrados projetados, 1.152 adiados, 1.728 descartados) e duas
recusas de depuração sem alterar estado. O teste cobre cores, posições nos
limites do mapa, projeção ortográfica, texturas nulas e atalhos ligados ou
desligados. A `memcpy` de 64 bytes usada pelo `rep movsd` de `0x66E70A` é
atendida por um stub de cópia no emulador, nos dois lados da comparação.

`test_submarine_input.py` compila as funções reais da fila extraídas do
módulo e testa toque rápido, três pressões seguidas, ausência de duplicação,
controle virtual, saída do modo e fallback. Também compila a montagem real
dos dois trampolins e compara os registradores e a pilha ARM com os
prólogos originais; executa `ff7_onKey`/`updateGamepad` da `.so` e confere o
mapeamento com o CFG do OBB e a instrução que testa o tiro no executável.

```sh
source scripts/env.sh
python3 scripts/emu/test_submarine.py build/libjni_ff7.so /caminho/ff7_en.exe 80
python3 scripts/emu/test_submarine_input.py build/libjni_ff7.so /caminho/ff7_en.exe /caminho/original.obb
cmake --build build -j8
```

VPK: `build/VPK/ff7_vita_v0.6.27.vpk`. **Pendente no Vita:** confirmar tiro
com Quadrado (toques curtos e repetidos) e FPS nos mesmos trechos. Comparar
os novos diagnósticos com `perf:`; o perfil anterior estava ligado, então
medir também sem o profiler para o FPS real. Ainda não há medição da
v0.6.27 no console nem confirmação do disparo em hardware.

## v0.6.28: submarino a 30 fps de verdade, modelos 3D nativos e botões de tela

**O que o log da v0.6.27 mostrou.** O submarino ficava travado em 27,5 fps,
mas a thread do jogo trabalhava só 36–46% do tempo (o resto: ~40% em espera
ativa e ~20% dormindo). Ou seja: não faltava CPU. O limitador do próprio jogo
(`0x78C9E1`) espera até `ticks/30 - 10000` (~32,3 ms) contados do *início do
último passo de lógica* e só então libera o desenho. O desenho e a entrega do
quadro à thread de render (que espera um vblank) vêm por cima. Cada quadro
dava ~36 ms.

**Limitador substituído (`ff7_framecap.c`).** Mesmo esquema do snowboard e
da montanha-russa: uma cadência fixa de 33,3 ms. Depois de um desenho
(`[0xE73F18]` = 1), o novo limitador dorme até a hora do próximo passo e pede
um passo de lógica (como o caminho "atrasado" do original: flag 0,
`[jogo + 0x40] += 1.0`, `[0xE747A0] = 0`). Depois de um passo de lógica, pede
o desenho; se o próximo passo já venceu, faz no máximo um passo extra antes
de desenhar (como o original, via `[0xE747A0]`). A lógica continua em 30 Hz.
Um quadro muito lento reinicia a cadência em vez de acelerar os seguintes. O
item A/B do limite de 30 fps no menu do port volta ao limitador original
(trampolim, sem reescrever código). `submarine_limiter_off.txt` mantém o
original desde o boot. Log: `submarine cadence: N frames/s, N logic steps/s`.

**Modelos 3D nativos (`0x79FB20`).** É a função que monta os modelos do
submarino (malha de triângulos: vértices int16, índices em bytes, uma cor
por canto). Era o maior trecho ainda no original no perfil da v0.6.27 (~15%
com o `mk_address` dela). Agora o laço dos triângulos é C: 3 transformações
`0x66CE40` em float, teste de w contra `[0x7B7FC8]` (com as flags do x87 como
o recompilado, inclusive NaN), espaço de `0x66E272 (1, [0xEDA668])` e os 3
vértices x/w, y/w, z/w, 1/w com a cor `0xFFRRGGBB`. O ponteiro de cores anda
12 bytes por triângulo, desenhado ou não. A matriz do modelo é montada
chamando as mesmas funções do jogo, na mesma ordem e com a mesma pilha
(rotações `0x6625AB`, `0x6627CD`, `0x6626BC`; pilha de matrizes `0xE996F8`;
`0x662ECC`; `0x6617E9`/`0x67D2BF`). Registradores finais e `_check_event`
iguais ao original. Entra no A/B de nativos e em `submarine_off.txt`.

**Validação:** `scripts/emu/test_submarine.py` ganhou o modo 5 (modelos).
`get_tick_now` e `sin`/`cos` da libm, chamados pelas rotações, viram stubs
iguais dos dois lados. A pilha de matrizes `0xE996F8` recebe uma matriz de
verdade, e metade dos modelos tem malha larga, com triângulos visíveis e
atrás da câmera no mesmo modelo. 320 chamadas: grade 40, bits 40, respingos
120, superfície 40, modelos 80; zero divergências; 446 triângulos de modelo
desenhados e 247 atrás da câmera. Mutações detectadas no núcleo dos modelos:
ordem dos ângulos, avanço das cores de um triângulo descartado, ordem dos
bytes da cor, ECX antes de `0x66E272`, ECX de um descarte no 2º canto, EDX
final e matriz identidade. Não exercitado pelo teste: o ECX quando o último
triângulo cai no 3º canto (`lea ecx, [ebp - 0x90]`, conferido no código).

**Botões de tela.** O port remove o gamepad virtual do Android dos layouts.
Isso tirava também os botões L2/R2 do submarino. Pior: os botões que ficavam,
como o R1+R2 da corrida de chocobo (tecla especial `0x85`), não funcionavam.
Depois que um botão físico é usado, `updateGamepad()` da `.so` reescreve a
cada leitura do teclado as 14 teclas do gamepad em `st_keyboard`, apagando o
que o toque acabou de marcar. Agora:

- no submarino (modo 10), os botões de tela L2/R2 (teclas `0x47`/`0x4F`)
  ficam no layout;
- `ff7_ui_touch_buttons()` informa quais botões de jogo estão presos por
  toque (objeto com id de toque em +24, sem ter deslizado para fora, +0x3C);
  R1+R2 vira R1 e R2, ESCAPE vira L1 e R1;
- `main.c` junta as fontes de cada botão (físico/painel traseiro, analógico,
  rotação do mapa, toque) e só solta o botão quando a última fonte solta.

**Pendente no Vita:** confirmar 30 fps estáveis no submarino (`perf:` e
`submarine cadence:`), os botões L2/R2 de tela no submarino e o R1+R2 de tela
na corrida de chocobo.

## v0.6.29: correções da v0.6.28 (limitador e L2/R2 de tela)

**Log da v0.6.28.** Os modelos nativos rodaram no console (`submarine/s:`
~43 modelos e ~5.500 triângulos de modelo por segundo). Mas o novo limitador
não entrou (`unexpected submarine limiter code, left as is`), então o
submarino seguiu em 27,4 fps, com a thread do jogo trabalhando só 34–37% do
tempo. O código da `.so` confere com o trampolim; quem falhou foi a checagem
das páginas x86 de `[0xE73F18]`/`[0xE74328]`/`[0xE747A0]` no boot, que ainda
não estão mapeadas nessa hora. Agora a instalação checa só o código, e cada
chamada confere as páginas (sem elas, roda o original).

**L2/R2 de tela.** O submarino usa o mesmo layout do campo (`_UI` entrada
11; `UI_determine()` dá ao modo 10 o mesmo índice do campo). Esse layout já
tinha sido filtrado no campo, antes de entrar no submarino, e por isso os
botões nunca voltaram. Agora a filtragem guarda à parte os L2/R2 de cada
layout. A cada desenho do layout (`UI_display`), eles entram na lista
enquanto o modo for 10 e saem fora dele (soltando o toque). Se o layout não
tiver L2/R2 próprios, usa os primeiros da tabela `_UI` (64 listas de 32
objetos). O log mostra `ui: layout ...: on-screen L2/R2 shown (submarine)`.
O toque passa pelo mesmo caminho da v0.6.28 (botões presos por toque viram
botões do gamepad), que vale também para o R1+R2 da corrida de chocobo.

**Pendente no Vita:** `ff7_framecap: submarine limiter 0x78C9E1 replaced`
no boot; `submarine cadence:` ~30 frames/s no submarino; L2/R2 visíveis e
funcionando; R1+R2 na corrida de chocobo.
