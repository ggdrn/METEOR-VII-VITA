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
