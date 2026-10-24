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

1. **Sincronização com a thread do jogo.** É preciso saber se a thread do
   jogo espera o `sem_post` de `ff7_DrawFrame` antes de começar o próximo
   quadro. Se esperar, o passo 4 de 4.3 tem de vir antes da espera de
   33 ms. Senão o jogo cai para ~10 fps. Por isso a cópia R no passo 1: o
   próximo quadro do jogo sobrescreve o framebuffer.
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
