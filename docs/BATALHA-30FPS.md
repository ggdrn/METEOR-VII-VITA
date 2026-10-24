# Batalha a 30 fps: estudo de interpolação

Primeira avaliação em 22/10/2026 (adiada). Retomada em 24/10/2026 com o
profile da v0.6.37 e a leitura do laço de batalha, do driver gráfico dos
porters e da fila de render do port. Este documento substitui o anterior.

**Conclusão:** dá para chegar a 30 fps na batalha sem tocar na lógica do
jogo. A ideia é gerar um quadro intermediário na saída gráfica: as posições
dos vértices ficam no meio do caminho entre dois quadros do jogo. O custo de
processamento é pequeno. O risco está em casar os desenhos de um quadro com
os do próximo, e isso se mede antes de implementar (fase 0).

## 1. O que limita a batalha a 15 fps

Medido na v0.6.37 (`logs.txt`, batalha com magias e limits):

| | Valor |
|---|---|
| Quadros por segundo | 14,8, cravados |
| Thread do jogo trabalhando | 31–38% (o resto é a espera do limitador) |
| Thread de render | 5% |
| Desenhos (`glDrawArrays`) | ~2.600/s, ~175 por quadro |
| Vértices copiados para a fila | ~7,4 MB/s, ~490 KB (~20 mil vértices) por quadro |
| GPU | ~4 ms por quadro |

Não falta CPU: o jogo espera de propósito. No `ff7.exe`:

- **Laço da batalha:** `0x41BAB3` (modo 2), uma vez por quadro. Marca o
  início do quadro (`0x660370`: `rdtsc` em `[0x9ACB70]`), roda o passo da
  batalha `0x41C0BB` (lógica e lista de desenho, pelos ponteiros
  `[0x9AD1BC]`, `[0x9AD1C0]`, `[0x9AD1C4]`), abre e fecha a cena
  (`0x660EC0` / `0x660EEB`) e chama o limitador.
- **Limitador:** `0x41B965`. Gira até passar `[0x9AB090]` ticks desde o
  início do quadro. O período é calculado em `0x41B6CF`:
  `[0x9AB090] = [jogo + 0x30] / 15,0` (ticks por segundo ÷ 15).
- **Recuperação:** se o quadro passou do período, o limitador liga
  `[0x9AD1D4]` e guarda o excesso em `[0x9AD1D8]`. O laço então roda um
  segundo passo da batalha no mesmo quadro.

Mudar o 15,0 para 30,0 faria tudo andar no dobro da velocidade (animações,
ATB, câmera, efeitos), porque cada passo avança um quadro de lógica. Por
isso a lógica fica como está.

## 2. Como o quadro chega à tela

- **Driver gráfico dos porters** (`VIDEO_drawBuffers`): os vértices chegam
  ao GL **já transformados para a tela**. A projeção é ortográfica
  (`GLKMatrix4MakeOrtho`) e o jogo transforma os modelos em software
  (`C_006A8558`, `C_0066CE40`, vértices `t_dx_rend_vertex_20`).
- **Formato do vértice**, 24 bytes:

  | Bytes | Conteúdo |
  |---|---|
  | 0–11 | posição x, y, z (float) |
  | 12–15 | cor RGBA (bytes) |
  | 16–23 | u, v (float) |

  Sem `rhw`: a textura é afim, então interpolar x, y, z na tela dá o mesmo
  resultado de interpolar antes da projeção.
- **Envio:** só `glDrawArrays` com arrays do cliente, sem buffer objects
  nem `glDrawElements` (`source/reimpl/gl_queue.c`). A fila de render já
  copia os vértices de cada desenho.
- **Destino:** o jogo desenha num framebuffer fora da tela. O jogo roda numa
  thread própria. A cada quadro, `ff7_DrawFrame` (chamado por `onDrawFrame`
  na thread principal) espera essa thread com um semáforo e chama
  `VIDEO_update`. `VIDEO_update` copia o framebuffer para a tela num quad,
  com as bordas e os botões de toque (`UI_frame_refresh`). Depois vem o
  swap (`main.c`).

Ou seja: tudo o que forma a imagem de um quadro de batalha passa pela fila
de render como uma lista de desenhos com vértices em coordenadas de tela.
Dá para gerar um quadro intermediário só nessa camada.

## 3. Abordagens

| | Meio passo de lógica | Interpolação na saída gráfica (proposta) |
|---|---|---|
| Ideia | Lógica a 30 Hz, cada contador avançando metade | Lógica a 15 Hz; um quadro extra com as posições no meio do caminho |
| Onde mexe | Centenas de contadores de animação, câmera, ATB, magias e invocações | Fila de render e composição do port; nada no `ff7.exe` |
| Fluidez | Total, inclusive efeitos | Modelos, câmera e o que se move de forma contínua; partículas que mudam de número ficam a 15 fps |
| Risco | Quebrar efeitos e tempos, um a um (o que o FFNx enfrentou no PC) | Desenhos que não casam entre quadros; um quadro "borrado" em cortes de câmera |
| Latência | Nenhuma | +33 ms (meio quadro de lógica) |
| CPU extra | ~2× a thread do jogo (~70% no combate comum, acima em invocações) | Fila e composição: alguns ms por quadro intermediário, fora da thread do jogo |

**Recomendação:** interpolação na saída gráfica. Não toca na lógica (as
velocidades ficam certas por construção), dá para desligar com um arquivo e
o custo cai nas threads que estão sobrando.

## 4. Como funcionaria

### 4.1 Gravar o quadro

Em batalha (modo 2), a thread de render grava cada desenho do quadro
enquanto o executa normalmente:

- a chave de estado: textura, variante de shader, blend, profundidade,
  teste de alfa, viewport/scissor, matrizes;
- o modo de primitiva e o número de vértices;
- os vértices: posição e uv; cor só para o caso rápido de casamento.

São dois buffers de ~0,5 MB (quadro anterior e atual), alternados.

### 4.2 Casar com o quadro anterior

Quando o quadro N+1 termina, casa cada desenho de N+1 com um de N:

1. **Desenhos:** pela chave de estado, em ordem. O k-ésimo desenho com a
   chave K em N+1 corresponde ao k-ésimo com a chave K em N.
2. **Vértices, caso rápido:** mesmo número de vértices e uv idênticos
   (`memcmp`). É o caso esperado nos modelos, cujas uv não mudam. Vértice i
   ↔ vértice i.
3. **Triângulos, caso geral:** se a contagem mudou (triângulos recortados,
   ordenação por profundidade), um hash das uv dos 3 vértices casa
   triângulo com triângulo, com a ordem desempatando as repetições.
4. **Sem par:** o desenho aparece como em N+1, sem interpolação. Isso vale
   para efeitos que mudam de forma, números de dano que surgem e texto.

**Proteções:**

- Um deslocamento grande demais num desenho (por exemplo, mais de 1/4 da
  tela) desliga a interpolação daquele desenho.
- Se a maioria dos vértices pula ao mesmo tempo (corte de câmera, troca de
  cena, transição), o quadro intermediário vira uma cópia de N+1.
- Os elementos 2D parados (menu, barras, janelas) têm deslocamento zero e
  saem idênticos.

### 4.3 Desenhar e mostrar

No `VIDEO_update` do quadro N+1, na thread principal:

1. Copiar o framebuffer do jogo (N+1 real) para uma textura própria, R.
2. Desenhar o quadro intermediário numa segunda textura, M. São os
   desenhos gravados de N+1, cada um com o seu estado e as posições
   `(N + N+1) / 2`. As texturas são as de N+1, que estão carregadas nesse
   momento.
3. Compor M na tela (o mesmo quad, bordas e botões) e fazer o swap.
4. Liberar o jogo para o próximo quadro.
5. 33 ms depois do passo 3, compor R e fazer o swap.

Resultado: um quadro a cada ~33 ms, com N+1 aparecendo meio quadro de
lógica depois do que hoje.

### 4.4 Custo estimado

- ~20 mil vértices: interpolar 3 floats cada e casar ~7 mil triângulos
  leva poucos ms por quadro intermediário. Isso roda na thread principal
  ou na de render; as duas ficam ociosas na batalha.
- GPU: o dobro de quadros, ~4 ms cada, dentro de 33 ms.
- Thread do jogo: sem mudança (31–38%).
- Memória: ~1–2 MB (dois quadros gravados e duas texturas do tamanho do
  framebuffer).

## 5. Riscos e pontos a confirmar

1. **Sincronização com a thread do jogo** (respondido na fase 0, lendo o
   código). A thread do jogo fecha cada quadro em `SwapBuffers` (chamada
   pelo flip do driver, `C_006A7157`) →
   `VIDEO_update`, e ela mesma desenha o framebuffer fora da tela na tela.
   Depois dá `sem_post` (quadro pronto) e **espera** em `sem_wait` até o
   próximo `ff7_DrawFrame` da thread principal soltá-la. Ou seja, o jogo
   espera a tela. O passo 4 de 4.3 (liberar o jogo) tem de vir antes da
   espera de 33 ms, senão o jogo cai para ~10 fps. Por isso a cópia R no
   passo 1: o próximo quadro do jogo sobrescreve o framebuffer.
2. **Estabilidade dos desenhos entre quadros.** O jogo junta vértices por
   estado. Se os lotes mudam de composição a cada quadro (recorte de
   triângulos, ordenação de transparências), sobra o casamento por uv. Se
   nem isso casar, a interpolação não serve para aquele conteúdo. A fase 0
   mede exatamente isso.
3. **Desenhos sem textura** (shader "color", ~2.000/s): as uv não
   identificam nada. Só casam pela ordem e pela contagem.
4. **Estado do GL que o quadro intermediário precisa repetir.** Tem de
   cobrir tudo o que o jogo usa. A lista sai das funções GL que a `.so`
   importa (é finita).
5. **Efeitos de tela cheia, invocações e o "swirl" de entrada** (captura de
   tela, `glReadPixels`, texturas criadas no quadro): na dúvida, o quadro
   intermediário vira cópia.
6. **Modo de recuperação** (dois passos de lógica num quadro): o
   deslocamento dobra, mas a interpolação continua correta (fica no meio).
7. **Latência de +33 ms:** irrelevante numa batalha por turnos com ATB, mas
   fica documentada e com chave para desligar.

## 6. Plano em fases

**Fase 0, medição (sem mudar a imagem).** A thread de render grava e casa
os quadros de batalha e o log mostra a cada 5 s:

- desenhos casados;
- vértices casados pelo caso rápido, pelo hash e sem par;
- deslocamento médio e máximo;
- quadros que seriam cortes.

Também registra a ordem dos semáforos de `ff7_DrawFrame` (risco 1). Basta
uma batalha comum e uma com magias.

- **Se > ~85% dos vértices 3D casarem:** seguir.
- **Se não:** a resposta é "não dá assim", com números, sem ter gasto o
  trabalho grande.

**Fase 1, protótipo.** Quadro intermediário com interpolação de posições,
cópia R e composição em dois swaps, ligado por
`ux0:data/ffvii-vita/battle_30fps_on.txt`. Só modo 2. Proteções de 4.2.

**Fase 2, acabamento.**

- Ritmo exato de 33 ms (sem quadros em pares).
- Cortes de câmera.
- Efeitos de tela cheia e invocações.
- Medir no console os 30 fps estáveis e a CPU/GPU.
- Depois de testado: ligado por padrão, com `battle_30fps_off.txt`.

O que não muda em nenhuma fase: a lógica da batalha, as velocidades e o
save. Desligando o arquivo, tudo volta ao que é hoje.

## 7. Fase 0 implementada (v0.6.39)

`source/ff7_interp.c` e `source/ff7_interp_core.h`. Não muda nada na tela.

**Como funciona:**

- **Estado:** `gl_queue.c` anota, na thread do jogo, o estado GL de cada
  chamada: framebuffer, textura, blend, profundidade, programa, uniforms,
  matriz, viewport, cull e máscara de cor.
- **Gravação:** em batalha (modo 2), cada `glDrawArrays` da thread do jogo
  no framebuffer do jogo grava a chave de estado (um hash), a primitiva e o
  x, y, u, v de cada vértice.
- **Fim do quadro:** `SwapBuffers` é interceptada (ela só faz
  `VIDEO_update(); glGetError(); return 1;`), e `fw_SwapBuffers` (a mesma
  coisa, pela tabela de importações x86) também. A v0.6.39 interceptava só
  `fw_SwapBuffers`, que a batalha nunca chama: a medição não rodou. Corrigido
  na v0.6.40. Na entrada, o quadro que terminou é casado
  com o anterior. Os desenhos da composição na tela, que vêm logo depois
  (o primeiro framebuffer que `VIDEO_update` liga), ficam de fora.
- **Memória:** os buffers (1,4 MB, um bloco de memória) são alocados na
  primeira batalha. Cabem 2.048 desenhos e 32.768 vértices por quadro; um
  quadro maior conta como `overflows`.

**O que medir:** uma batalha comum e uma com magias, limits e
invocações. A cada 5 s em batalha, o log mostra:

```
interp probe (battle): F frames (P matched pairs, C cuts, O overflows) |
  draws/frame D, matched X% (T triangle lists, N other) |
  vertices/frame V: same mesh A%, by u/v B%, unmatched U% |
  moving M% of matched, mean m px, max x px, jumps > 96 px J |
  skipped S screen draws, K other formats (screen fb F) | cost c ms/frame
```

| Campo | O que é |
|---|---|
| `same mesh` | desenhos casados com os mesmos vértices na mesma ordem (o caso rápido, esperado nos modelos) |
| `by u/v` | triângulos casados pelo hash das uv (ordem ou contagem mudaram) |
| `unmatched` | vértices que, no quadro intermediário, ficariam como no quadro novo |
| `moving` | dos casados, quantos se movem; `mean`/`max` em pixels da tela do jogo |
| `cuts` | pares de quadros em que a maioria pula mais de 96 px (cortes de câmera) |
| `cost` | tempo do casamento por quadro, na thread do jogo (só nesta medição) |

**Como ler:**

- **Seguir para a fase 1:** `same mesh` + `by u/v` acima de ~85% e quase
  todo o movimento entre os casados.
- **Ruim:** muito `unmatched` em quadros com movimento. Nesse caso o log
  mostra quanto e em que tipo de desenho (`triangle lists` / `other`).

Desligar: `ux0:data/ffvii-vita/interp_probe_off.txt`.

Teste do casamento no Mac (quadros sintéticos com resposta conhecida):
`cc -O2 -I source scripts/test_interp.c -o /tmp/ti -lm && /tmp/ti`.

## 8. Resultado da fase 0 (v0.6.40, 24/10/2026)

Log de ~8 min com 6 batalhas: ataques, magias e invocações pesadas. Foram
85 janelas de 5 s e 5.363 pares de quadros casados. Nenhum quadro estourou
os buffers. A medição viu ~13,6 quadros/s, o ritmo da batalha.

| Situação | Vértices/quadro | Mesmo modelo | Por u/v | Sem par | Cortes por 5 s |
|---|---|---|---|---|---|
| Batalha parada ou com movimento normal | ~21.500 | 99–100% | 0–0,5% | 0–0,7% | 0 |
| Magias | 3.000–21.000 | 85–98% | 1–8% | 1–10% | 0–3 |
| Invocações pesadas | 1.000–20.000 | 57–90% | 3–24% | 5–37% | 3–23 |
| **Média ponderada** | | **90,7%** | **3,2%** | **6,2%** | 249 em 5.363 pares |

- **Casamento:** 93,9% dos vértices acham par, acima dos ~85% combinados
  para seguir. Na batalha comum, praticamente tudo casa no caso rápido
  (mesmo modelo, mesma ordem).
- **Movimento:** 83–96% dos vértices casados se movem de um quadro para o
  outro, porque a câmera da batalha nunca para. Na batalha comum, o
  deslocamento médio é de 1–6 px por quadro de 15 fps. É exatamente o
  movimento que fica "pulado" a 15 fps e que o quadro intermediário
  dividiria ao meio.
- **Cortes:** nas invocações, até 1/3 dos pares são cortes de câmera. Ali o
  quadro intermediário vira cópia (fica a 15 fps).
- **Vértices fora da tela:** o máximo chega a milhões de pixels e há
  centenas de milhares de saltos acima de 96 px nas magias e invocações. O
  jogo manda vértices com coordenadas enormes (triângulos atrás da câmera
  ou descartados). Isso distorce a média nessas janelas, mas o importante é
  o efeito na fase 1: um triângulo com um vértice normal num quadro e
  enorme no outro viraria uma faixa atravessando a tela. **Proteção
  obrigatória:** se qualquer vértice de um triângulo pular mais que o
  limite, o triângulo inteiro sai como no quadro novo, sem interpolar.
- **Custo:** 6–12 ms por quadro de batalha na thread do jogo (só a
  medição: cópia dos vértices e casamento). Na fase 1 isso sai da thread do
  jogo e precisa ficar mais barato: o caso rápido não precisa de hash, só
  da comparação das uv.

**Decisão: seguir para a fase 1.** A batalha comum é o caso mais frequente
e o mais favorável (~99% casado, movimento contínuo e pequeno). As magias
ganham quase tudo. As invocações ganham parcialmente e caem para cópia nos
cortes.

## 9. Fase 1 implementada (v0.6.41): protótipo do quadro intermediário

`source/ff7_interp.c`, `source/ff7_interp_core.h` e o bloco "Battle at 30
fps" no fim de `source/reimpl/gl_queue.c`.

**Como cada quadro de batalha sai:**

1. **Gravação.** Enquanto o jogo desenha o quadro N+1, a fila de render
   grava uma cópia de cada registro que a thread do jogo publica: mudanças
   de estado e desenhos com os vértices. Também guarda o estado GL do
   início do quadro, com os valores reais: framebuffer, texturas das
   unidades 0 e 1, blend, profundidade, máscaras, cull, viewport, cores de
   limpeza, enables, programa, uniforms e matrizes por programa.
2. **Casamento.** Em `SwapBuffers`, N+1 é casado com N como na fase 0, agora
   gerando o mapa vértice de N+1 → vértice de N.
3. **Quadro M.** Se o quadro serve, ele é desenhado de novo num framebuffer
   próprio, M (textura 1024×512 que divide o buffer de profundidade com o do
   jogo). A sequência: limpar M, repor o estado do início do quadro, repetir
   os registros com o framebuffer do jogo trocado por M e cada posição no
   meio do caminho. Um triângulo com vértice sem par, ou que salta mais de
   96 px, fica como em N+1.
4. **Apresentação.** `VIDEO_update` roda com a textura do jogo trocada pela
   de M na sua struct (`[GOT 0xF4C97C]` +12), e a thread principal mostra
   M. Meio quadro de batalha (33,3 ms) depois, `VIDEO_update` roda de novo,
   agora com a textura do jogo, e aparece N+1. A composição (zoom, bordas,
   botões de toque) é a do próprio jogo.

**Quando o quadro sai como está (15 fps naquele quadro):**

| Motivo | Quando acontece |
|---|---|
| `cut` | corte de câmera |
| `textures` | o quadro enviou ou apagou textura |
| `other state` | estado fora do que a sombra guarda, ou desenho grande demais para a fila |
| `overflow` | mais de 2.048 desenhos, 32.768 vértices ou 1 MB de registros |
| `not replayable` | uma chamada que não pode ser repetida |
| `late` | o quadro chegou tarde para mostrar M meio quadro depois do anterior |

**Limite desta fase.** A thread do jogo fica presa no gancho durante os 33
ms entre M e N+1, porque `VIDEO_update` só volta quando a thread principal
libera. Então só dá para interpolar quando o trabalho do quadro mais o
casamento e a repetição cabem em ~30 ms. Na batalha comum o trabalho é de
21–25 ms: fica apertado. O log mostra quantos quadros saem `late`. Se forem
muitos, a fase 2 libera o jogo durante a espera, o que exige refazer a
sincronização de `ff7_DrawFrame`.

**O log em batalha, a cada 5 s:**

```
battle 30 fps: F battle frames, I interpolated | not: C cut, L late, T textures, ... |
  in-between shown X ms after the last frame, new frame Y ms after it | hook ..., matching ..., replay ...
battle 30 fps matching: (os números da fase 0)
```

Com tudo certo, X e Y ficam perto de 33 ms, e `interpolated` perto do
total de quadros.

**Controles:**

- Menu do port: **"A/B BATALHA A 30 FPS (INTERPOLACAO): SIM/NAO"** liga e
  desliga na hora, inclusive no meio da batalha, para comparar.
- `ux0:data/ffvii-vita/battle_30fps_off.txt`: começa desligada (a medição
  continua no log).
- `ux0:data/ffvii-vita/interp_probe_off.txt`: desliga tudo.

**O que observar no teste:**

- triângulos faltando ou piscando no quadro M;
- modelos "esticados" por um quadro;
- efeitos e menus em lugares errados;
- se o ritmo fica regular (sem quadros em pares);
- se a batalha continua na velocidade certa.

## 10. v0.6.41 deixou a batalha em 13 fps; correções da v0.6.42

**O que o log da v0.6.41 mostrou:**

- **Nenhum quadro interpolado.** Todos foram recusados como `other state`:
  a sombra do estado só guardava uniforms e matrizes de programas e
  localizações com id abaixo de 16 (e 2 unidades de textura), e o jogo usa
  algum id acima disso.
- **O custo foi pago mesmo assim.** Casamento de ~6,8 ms, gravação e cópia
  dos vértices: o trabalho da thread do jogo subiu de ~35% para ~48% do
  quadro.
- **E esse custo entrou inteiro no quadro.** No laço da batalha
  (`0x41BAB3`), o limitador (`0x41B965`) roda **antes** do flip
  (`0x660EEB` → `SwapBuffers`), então o tempo gasto no gancho não é absorvido
  pela espera do limitador: soma-se a cada quadro. 66,7 + ~10 ms = ~77 ms,
  13 fps. Com a interpolação funcionando, a espera de 33 ms teria levado a
  batalha a ~10 fps.

**Correções:**

1. **Limitador compensado.** Com a interpolação ligada, o período do
   próximo quadro (`[0x9AB090]`, double, ticks ÷ 15) é encurtado pelo tempo
   que o gancho levou. De um flip ao outro fica um quadro de batalha (66,7
   ms), e a lógica continua um passo por quadro. Com a interpolação
   desligada ou fora da batalha, o valor do jogo volta. (Efeito colateral: com
   ela ligada, a batalha fica nos 15,0 fps exatos, em vez dos 14,8 de antes.)
2. **Sombra sem limite de id.** Uniforms (até 128) e matrizes (até 32) ficam
   em tabelas esparsas de (programa, localização, valor), com 8 unidades de
   textura. Se um quadro ainda for recusado, o log mostra o motivo exato
   (`ff7_interp: frame not interpolated: ...`), com os valores envolvidos, nas
   3 primeiras vezes de cada tipo.
3. **Núcleo sem cópias.** Os desenhos apontam para a própria gravação do
   quadro, que a repetição já precisava. Mesmo modelo = mesmo hash das uv
   (uma comparação por desenho); a medição de movimento e cortes olha 1
   vértice em cada 4. O casamento deve cair de ~6,8 ms para menos de 1 ms.
4. **Desligada = custo zero.** Com a interpolação desligada (menu ou
   `battle_30fps_off.txt`), nada é gravado nem casado.
5. **Sem recusa por atraso.** O quadro intermediário sai meio quadro depois
   do anterior, ou assim que fica pronto se esse momento já passou. `late`
   agora só conta esses atrasos; a cadência se ajusta sozinha a partir do
   quadro seguinte.

## 11. Resultado da v0.6.42 e decisão: desligada por padrão (v0.6.43)

A v0.6.42 interpolou de fato (~12 quadros/s com quadro intermediário,
quase nenhum recusado), mas não serviu:

| | Interpolação ligada | Desligada |
|---|---|---|
| Quadros na tela | 20–24 fps, irregulares | 14,8 fps, regulares |
| Intervalo entre quadros | intermediário ~57 ms depois do anterior, novo ~31 ms depois dele | 67 ms |
| Lógica da batalha | ~11,6 passos/s (mais lenta que os 15) | 14,8 passos/s |
| Custo na thread do jogo | casamento ~6 ms + repetição ~18 ms por quadro | — |

O que o jogador vê: movimento aos trancos, que parece acelerado, menu
tremendo às vezes e nunca 30 fps.

**Por quê:** neste desenho, a thread do jogo faz o casamento e a repetição
do quadro (~24 ms) e ainda espera os 33 ms entre os dois quadros
mostrados. Somados aos ~23 ms de trabalho do próprio quadro, passam dos
66,7 ms de um quadro de batalha. O tremor do menu vem de interpolar
também os elementos 2D (janelas e cursor) quando eles mudam entre um quadro
e outro.

**Decisão (v0.6.43):** a interpolação fica **desligada por padrão**, com
custo zero (nada é gravado nem casado). Liga com
`ux0:data/ffvii-vita/battle_30fps_on.txt` ou pelo item A/B do menu do port,
só para testes.

**Para funcionar de verdade, o desenho teria de mudar:**

1. O casamento e a repetição saem da thread do jogo e vão para a thread de
   render, que fica ociosa na batalha.
2. O segundo quadro passa a ser mostrado pela thread principal, sem prender
   a thread do jogo. Isso exige refazer a sincronização de `ff7_DrawFrame`
   (os semáforos entre jogo e tela).
3. O 2D (menus, janelas, números) fica fora da interpolação.

É um trabalho grande e com resultado incerto: a GPU passa a desenhar cada
quadro duas vezes, e a repetição de ~0,5 MB de comandos por quadro custa
caro mesmo fora da thread do jogo.
