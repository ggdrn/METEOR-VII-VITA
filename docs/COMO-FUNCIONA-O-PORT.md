# Como o FF7 de Android funciona e por que o port reescreve partes em código nativo

Este documento registra o que aprendemos sobre a versão Android do FINAL
FANTASY VII (v1.0.29) ao longo do port, e explica por que trazer esse jogo ao
PS Vita não é "só carregar a `.so`": boa parte do trabalho tem sido reescrever,
em C nativo, funções que o jogo já tem.

Detalhes de cada otimização, com medições, estão em
[DESEMPENHO.md](DESEMPENHO.md) e [MINIGAMES.md](MINIGAMES.md). O plano geral
está em [../PLANO.md](../PLANO.md).

---

## 1. Resumo em um parágrafo

O FF7 de Android **não é um jogo escrito para ARM**. É o `ff7.exe` do PC
(x86, Windows, 1998) passado por um **recompilador estático x86 → ARM** e
embrulhado num motor chamado "materialg" (D4 Enterprise). O código gerado
simula uma CPU x86 inteira em memória: registradores, pilha, FPU x87 e cada
acesso à memória passam por estruturas e funções auxiliares. Num celular de
2 GHz isso sobra; no Vita (Cortex-A9 a 444 MHz, uma única thread para o
jogo) o mesmo código fica 2 a 3 vezes mais lento do que precisaria. Por isso,
para chegar aos 30 fps, o port identifica as funções mais pesadas e as
substitui por versões nativas em C que fazem exatamente a mesma coisa, mas
sem a "CPU x86 de mentira" no meio.

---

## 2. Como a versão Android é construída

### 2.1 As camadas

```
┌──────────────────────────────────────────────────────────────────┐
│ APK (Java)  jp.co.d4e.materialg.*                                │
│   MainActivity, GLSurfaceView, MyDecoder (vídeo via MediaCodec), │
│   SEPlayer (efeitos), ExpansionFile (acesso ao OBB)              │
└───────────────▲──────────────────────────────┬───────────────────┘
                │ JNI (chamadas Java ← .so)    │ JNI (onDrawFrame, onKey, onTouch...)
┌───────────────┴──────────────────────────────▼───────────────────┐
│ libjni_ff7.so  (ARMv7 softfp, 16 MB)                             │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ ff7.exe recompilado (x86 → ARM)                            │  │
│  │   func_0x0068de75(), func_0x0075f263() ...                 │  │
│  │   registradores x86 em memória global, x87 emulado,        │  │
│  │   todo acesso à memória via mk_address(va)                 │  │
│  └───────────────┬────────────────────────────────────────────┘  │
│                  │ chamadas a "Windows" / DirectX / DirectSound  │
│  ┌───────────────▼────────────────────────────────────────────┐  │
│  │ Camada dos porters (C++)                                   │  │
│  │   Direct3D/DirectDraw → emulação de GL 1.x → GLES2         │  │
│  │   (C_006A8002, glBegin/glVertex3f/glEnd, VIDEO_drawBuffers)│  │
│  │   DirectSound → OpenSL ES, músicas .akb (Vorbis)           │  │
│  │   arquivos .lgp lidos do OBB, timers, _check_event         │  │
│  └───────────────┬────────────────────────────────────────────┘  │
└──────────────────┼───────────────────────────────────────────────┘
                   ▼
        libc, libm, libEGL, libGLESv2, libOpenSLES, libandroid
```

### 2.2 O lado Java

O Java é fino. Ele cria a superfície OpenGL, repassa toque e botões para a
`.so` (`onKey`, `onTouchBegan/Moved/Ended`), chama `onDrawFrame` a cada
quadro e oferece alguns serviços que a `.so` pede de volta via JNI:

| Classe | O que faz no Android |
|---|---|
| `ExpansionFile` | abre o OBB (ZIP sem compressão de 1,98 GB) e devolve fd + offset + tamanho de cada arquivo |
| `MyDecoder` | toca os 114 vídeos `.webm` (VP8 640×448 + Vorbis) com o decodificador de hardware do celular, numa `SurfaceTexture` |
| `SEPlayer` | efeitos sonoros |
| `MainActivity` | decodifica imagens (`ImageGetData`), mensagens de erro, saves, URL |

### 2.3 O lado nativo: o `ff7.exe` recompilado

A `libjni_ff7.so` contém dezenas de milhares de funções com nomes como
`_Z15func_0x0068de75v`: cada uma é uma função do `ff7.exe` de PC no endereço
x86 indicado, traduzida instrução por instrução para ARM. O recompilador
preservou a semântica x86 ao pé da letra, e isso tem consequências:

- **Registradores x86 vivem em memória global.** `EAX`, `ECX`, `EDX`,
  `ESP`, `EBP`… são variáveis num bloco de memória, não registradores do ARM.
  Cada `mov eax, [ebp+8]` vira várias leituras e escritas na RAM.
- **A memória x86 é paginada por software.** Todo acesso a um endereço x86
  (global, ponteiro, pilha) passa por `mk_address(va)`, que consulta a tabela
  `MEMMAP` (páginas de 4 KB) e devolve o endereço real. Há **669.918
  chamadas** a `mk_address` no binário, cerca de **uma a cada 8 instruções**,
  89% de todas as chamadas de função da `.so`.
- **A pilha x86 também é memória emulada.** Até um `push ebp` passa por
  `mk_address`, porque `ESP` é uma variável.
- **A FPU x87 é emulada.** O FF7 de PC faz toda a matemática 3D na x87
  (pilha de 8 registradores, palavra de controle, flags C0–C3, exceções). O
  código recompilado mantém esse estado numa estrutura em memória e o
  atualiza a cada `fld`, `fmul`, `fcomp`, `fistp`… Conversões como `_ftol`
  (double → inteiro do MSVC) são funções inteiras chamadas milhares de vezes
  por quadro.
- **`_check_event` espalhado pelo código.** O recompilador insere chamadas a
  `_check_event` (timers, eventos) em pontos do código x86, para emular o
  comportamento cooperativo do Windows.
- **Uma CPU, uma thread.** É uma só CPU x86 emulada, com um só conjunto de
  registradores globais. A lógica do jogo não pode ser dividida entre
  núcleos sem ser reescrita.

### 2.4 A camada dos porters

Entre o código recompilado e o Android há código C++ escrito à mão pelos
porters, que finge ser o Windows e o DirectX:

- **Gráficos:** o FF7 de PC desenha com Direct3D. Os porters reimplementaram
  isso como uma emulação de OpenGL 1.x imediato (`glBegin`, `glVertex3f`,
  `glColor4ub`, `glEnd`) sobre GLES2. Funções como `C_006A8002` e
  `C_006A8A3E` transformam e enviam triângulos **um vértice por vez**. O jogo
  usa só ~53 funções GL, sem VBOs nem `glDrawElements`.
- **Texturas:** o FF7 guarda texturas paletadas (8 bits + paleta); a
  conversão para RGBA é feita na CPU por `convert_image_data`, pixel a pixel.
- **Áudio:** DirectSound emulado sobre OpenSL ES; as músicas `.akb`
  (Vorbis) são decodificadas dentro da `.so`. Quatro faixas usam o caminho
  WAV original do PC.
- **Dados:** os arquivos `.lgp` do PC ficam dentro do OBB.

---

## 3. O que o port faz (so-loader)

O port não emula nada além do que o próprio jogo já emula. Ele é um
*so-loader*: carrega a `libjni_ff7.so` original diretamente do `base.apk` do
usuário e a executa na CPU do Vita, trocando o "Android" por equivalentes do
Vita.

| Peça | Papel |
|---|---|
| `so_util` | carrega a `.so` na memória, resolve relocações, permite ganchos (`hook_addr`) |
| `source/dynlib.c` | liga os 224 imports da `.so` (libc, GL, OpenSL, AAsset…) a funções do Vita |
| `FalsoJNI` + `source/java.c` | um "JNI falso": reimplementa em C os métodos Java que a `.so` chama |
| `vitaGL` | implementa GLES2 sobre o GXM (a API gráfica do Vita) |
| `source/reimpl/` | AAssetManager, EGL, pthread, OBB, vídeo, leitura de pixels… |
| `kubridge` | plugin de kernel para mapear memória executável e tratar exceções |
| VitaSDK **softfp** | a `.so` usa a ABI softfp (floats em registradores inteiros), diferente do VitaSDK padrão (hardfp) |

Nenhum dado do jogo vai no VPK: APK e OBB vêm da cópia do próprio usuário em
`ux0:data/ffvii-vita/` (ver [DATA.md](DATA.md)).

Só isso já põe o jogo para rodar. O problema é **a que velocidade**.

---

## 4. Por que o jogo fica lento no Vita

### 4.1 O hardware

| | Celular típico em que o port Android foi pensado | PS Vita |
|---|---|---|
| CPU | ARM moderno, 1,5–3 GHz, 4–8 núcleos, execução fora de ordem | Cortex-A9, **444 MHz** (com overclock), 4 núcleos, 3 usáveis pelo app |
| RAM disponível | GBs | ~365 MB para o app |
| Vídeo | decodificador de hardware via MediaCodec | VP8 só em software |

O código recompilado é ineficiente por natureza, mas num celular isso não
aparece. No Vita, cada instrução extra pesa.

### 4.2 O que os logs mostraram

Medições da v0.00.36, antes das otimizações (ver DESEMPENHO.md):

| Situação | FPS | Thread do jogo (core 0) |
|---|---|---|
| Campo leve | 30 (limite do jogo) | ~40% trabalho |
| Setor 7 / campos pesados | **17–22** | **91–93% trabalho**, 5–7% GL, 0% ocioso |
| Mapa-múndi | 18–22 | 92–93% |
| Moto (highway) | 22–23 | 95–97% |

E, ao mesmo tempo:

- a **GPU terminava cada quadro em ~3–4 ms** — ela não é o gargalo;
- os **cores 1 e 2 ficavam ~70% ociosos**;
- **mais da metade das instruções executadas** pela thread do jogo estava
  dentro de `mk_address`.

Ou seja: o jogo é limitado pela CPU, **numa única thread**, e o custo é o
código recompilado em si.

### 4.3 Por que "usar mais núcleos" não resolve

Tudo que dava para tirar da thread do jogo já foi tirado: áudio, vídeo,
carregamento de música em segundo plano, e uma **thread de render** que
executa as chamadas GL nos cores 1–2 (`source/reimpl/gl_queue.c`). Isso deu
no máximo ~10%. O resto é a lógica do jogo, uma CPU x86 emulada que não se
divide. O único caminho é fazer **menos trabalho** no core 0.

---

## 5. Por que reescrever em código nativo

### 5.1 O que "nativo" significa aqui

Uma função recompilada, como a iluminação de vértice `0x68DE75`, faz algo
simples (multiplicar cores por luzes e saturar), mas cada operação passa por:

```
ler EBP da memória → mk_address(EBP+8) → ler argumento
→ fld (empurra na pilha x87 emulada, atualiza TOP)
→ fmul (lê dois slots, escreve um, atualiza flags)
→ fcomp (compara, grava C0/C2/C3 em memória)
→ fnstsw ax / test ah, 0x41 (lê as flags de volta)
→ _ftol (função inteira: salva palavra de controle na pilha x86, troca
  arredondamento, converte, restaura) ...
```

A versão nativa (`source/ff7_light.c`) faz a mesma conta com `float` e
registradores do ARM, em linha. O ganho vem de eliminar a emulação, não de
mudar o algoritmo.

### 5.2 O detalhe que torna isso difícil: tem que ser **idêntico**

A função nativa entra no lugar da recompilada através de um gancho na
entrada ARM dela. O resto do jogo continua sendo o código recompilado, e
ele **lê o estado que a função deixou**. Então a versão nativa precisa
deixar exatamente o mesmo:

- a memória x86 escrita (inclusive escritas temporárias na pilha);
- `EAX`, `ECX`, `EDX`, `ESP`, `EBP`… — mesmo os que o C chamaria de "lixo",
  porque às vezes o chamador usa;
- o estado x87: topo da pilha, slots, palavra de controle, **C0/C2/C3**, a
  flag de divisão por zero;
- o número e a ordem das chamadas a `_check_event`.

E precisa reproduzir os comportamentos esquisitos do recompilador, que
**não são** os do x86 real nem os do C. Alguns que só os testes revelaram:

| Comportamento do código recompilado | Onde apareceu |
|---|---|
| `fistp` trunca e satura (`vcvt.s32`), NaN vira 0 — não arredonda como o x87 | mapa-múndi |
| divisão por zero não dá infinito: o valor fica como estava e a flag ZE vai a 1 | mapa-múndi, motor comum |
| comparação com NaN em `fcomp` **não altera** C0/C2/C3 (o teste seguinte vê a comparação anterior) | triângulos do mapa |
| `fnstsw ax` escreve no `EAX` inteiro, não só em `AX` | moto |
| `__fixdfdi(-infinito)` devolve 1 nesta `.so` | `_ftol` / `d2ulz` |
| o `EDX` final depende de qual caminho o último triângulo tomou | triângulos do mapa |

Se qualquer um desses detalhes divergir, o resultado não é um crash
imediato: é um polígono errado, uma cor estourada, um minigame que trava
dez minutos depois.

### 5.3 A metodologia

Cada função nativa segue o mesmo processo:

1. **Perfil no Vita.** Um profiler próprio (`source/utils/profiler.c`,
   ligado por `profile_on.txt` / `profile_all.txt`) amostra a thread do
   jogo; `scripts/profile-report.py` gera o ranking. As amostras dentro de
   `mk_address` são reatribuídas a quem chamou, senão tudo parece ser
   `mk_address`.
2. **Entender a função.** Desmontagem do ARM recompilado e do `ff7_en.exe`
   x86 original lado a lado.
3. **Núcleo em C** num header (`ff7_*_core.h`), sem dependências do Vita.
4. **Equivalência no emulador.** `scripts/emu/` roda o código ARM
   recompilado original no Unicorn e, ao lado, o mesmo header compilado como
   blob ARM com o compilador do Vita. Ambos partem do mesmo estado e chamam
   as mesmas funções recompiladas. Compara-se memória, registradores, x87 e
   `_check_event` em dezenas de milhares de casos aleatórios, incluindo NaN,
   infinitos, denormais, `w == 0` e acessos que cruzam páginas. Mutações
   propositais no núcleo precisam ser detectadas pelo teste.
5. **Gancho no jogo**, com:
   - **autoteste**: as primeiras N chamadas rodam as duas versões e
     comparam; se divergir, o gancho é removido e o jogo segue com o
     original (`selftest_on.txt` amplia o teste);
   - **chave de desligar** `ux0:data/ffvii-vita/<módulo>_off.txt`, e
     `natives_off.txt` desliga todas, para separar bug do port de
     comportamento do jogo e fazer A/B no aparelho;
   - **contador no log** (`... native, ... original`) para provar que o
     caminho nativo está mesmo sendo usado.
6. **Medir no Vita** antes e depois, no mesmo trecho.

### 5.4 Além das funções: remendos no próprio binário

Algumas otimizações não substituem funções, mas reescrevem instruções da
`.so` no boot:

- **`mk_address` reescrita** (`ff7_memmap.c`): o endereço da tabela vira
  constante, sem a leitura dependente do GOT.
- **71.246 acessos a globais** x86 (`movw/movt/bl mk_address` com endereço
  constante) têm o endereço final calculado uma vez e a chamada vira `nop`.
- **333.453 acessos à pilha** (`ESP + c`, `EBP + c`, `ff7_stack.c`): a pilha
  x86 é alocada num endereço escolhido para que `host = va + K`, com `K`
  codificável numa instrução Thumb-2. Cada `bl mk_address` vira
  `add.w r0, r0, #K`. Uma análise de fluxo de dados
  (`scripts/gen-stack-sites.py`) prova, ponto a ponto, que o registrador é
  mesmo `ESP`/`EBP` e que o `EBP` é ponteiro de quadro naquela função.

Juntas, essas duas mudanças eliminam **61% das chamadas a `mk_address`**.

### 5.5 O que já é nativo

| Área | Funções / módulo | Resultado medido |
|---|---|---|
| Endereçamento | `mk_address`, globais, pilha (`ff7_memmap.c`, `ff7_stack.c`) | campo a 30 fps: trabalho real da thread de ~50% para ~35% |
| Campo | iluminação de vértice `0x68DD1E`/`0x68DE75`/laço de `0x68E92D` (`ff7_light.c`), fundo de campo (`ff7_bg.c`, `ff7_fieldbg.c`), triângulos `C_006A8002` (`ff7_tri.c`), cos/sin (`ff7_math.c`) | Setor 7: **17,8 → 30 fps** (v0.00.36 → v0.2.05) |
| Mapa-múndi | malhas, projeção, cor e triângulos do terreno `0x75F0AD`/`0x75F263`/`0x68DAE1`/`0x75F68C` (`ff7_world.c`) | 19–23 → 23–29 fps só com a parte 1 (v0.5.06) |
| Moto | `_ftol` (`ff7_ftol.c`), `double → uint64` (`ff7_d2ulz.c`), cor de vértice `0x651A6D` (`ff7_highway.c`), triângulos `C_006A8A3E` | 22–23 → ~26–27 fps (v0.6.2–v0.6.5) |
| Motor comum | conversões de ponto fixo `0x661465`/`0x6611DA`/`0x661098` (`ff7_engine.c`) | vale para todos os minigames |
| Minigames | chocobo, snowboard, submarino, montanha-russa (`ff7_*_core.h`) + limitador de 30 fps (`ff7_framecap.c`) | cadência estável de 30 fps |
| Carregamentos | LZSS (`ff7_lzss.c`), conversão de texturas (`ff7_texture.c`, paleta na GPU como textura P8), decodificador de animação de batalha (`ff7_anim.c`), captura do "swirl" (`ff7_swirl.c`) | menos travadas em troca de área |

---

## 6. O que precisou ser reescrito por outros motivos

Nem tudo é desempenho. Partes do jogo dependem de coisas que só existem no
Android e foram refeitas do zero:

- **Vídeos (FMV).** No Android, quem decodifica é o `MyDecoder` em Java com
  o hardware do celular. No Vita não há decodificador VP8 em hardware: o
  port lê o `.webm` direto do OBB (nestegg), decodifica VP8 (libvpx) e
  Vorbis (libvorbis) numa thread separada, e envia o quadro como textura YUV
  que a GPU converte para RGB (`source/reimpl/movie.c`). Decodificar 640×448
  em software ocupa ~42–53% de um núcleo.
- **Áudio.** OpenSL ES compilada do fonte e adaptada ao `sceAudioOut`,
  misturando todos os engines (`source/opensles/`). A música troca de faixa
  em segundo plano (`ff7_music.c`), e um bug do DirectSound dos porters que
  reiniciava a música do snowboard a cada quadro foi corrigido
  (`ff7_dsound.c`).
- **Arquivos.** O OBB é lido sem extrair, com cache de blocos e offsets de
  64 bits (`utils/zip.c`, `reimpl/obb_fd.c`); arquivos temporários `*.P` ficam
  em RAM.
- **Controles.** Botões e analógicos viram os key codes Android que o jogo
  espera; toque traseiro faz L2/R2; o analógico direito gira a câmera do
  mapa; ícones de botão do Xbox/Android trocados pelos símbolos PlayStation
  (`ff7_buttons.c`, ver [BOTOES-PS.md](BOTOES-PS.md)).
- **Tela.** O jogo desenha em 640×480 e usa só 448 linhas no campo e na
  batalha; o port amplia sem distorcer para ocupar os 544 pixels do Vita
  (`ff7_screen.c`).
- **Shaders.** `samplerExternalOES` (textura de vídeo do Android) trocado
  por `sampler2D`; variantes especializadas para o compilador do Vita.

---

## 7. Resumo das dificuldades técnicas

1. **Código de terceira mão.** O que roda é um executável de PC de 1998,
   recompilado automaticamente para ARM, sobre uma camada que imita Windows
   e DirectX, sobre uma camada que imita Android. Não há código-fonte de
   nenhuma delas.
2. **A emulação está dentro do binário.** Não dá para "desligar o
   emulador": cada função carrega o custo de registradores em memória, x87
   emulado e `mk_address`. Só reescrevendo função por função.
3. **Uma thread só.** O jogo é uma CPU x86 emulada; os outros núcleos do
   Vita não ajudam na lógica.
4. **Equivalência bit a bit.** Cada substituição precisa reproduzir os
   efeitos colaterais do recompilador, inclusive os que parecem bugs. Isso
   exigiu montar um emulador ARM de teste ao lado do código original.
5. **Testar no aparelho é caro.** Cada ciclo é: build, VPK, instalar,
   jogar até o trecho, coletar log e perfil. Daí os autotestes e as chaves
   `*_off.txt`, que permitem diagnosticar sem recompilar.
6. **Reescrever o binário em execução.** Centenas de milhares de instruções
   são alteradas no boot, enquanto o profiler troca a proteção das mesmas
   páginas em outro núcleo — isso chegou a reiniciar o console até as duas
   coisas serem sincronizadas.
7. **ABI e toolchain.** A `.so` é softfp; o VitaSDK padrão é hardfp. Foi
   preciso um VitaSDK-softfp, vitaGL e vitaShaRK recompilados nessa ABI, e
   headers atualizados a partir do SDK novo.
8. **Hardware sem as facilidades do Android.** Sem decodificador de vídeo
   VP8, com ~365 MB de RAM e GPU acessada só via vitaGL/GXM.
9. **O ritmo do próprio jogo.** Batalhas a 15 fps e alguns minigames a
   60 Hz de lógica são decisões do motor original; mudar isso exige mexer na
   lógica, não só em desempenho (ver [BATALHA-30FPS.md](BATALHA-30FPS.md)).

---

## 8. Onde ler mais

| Documento | Conteúdo |
|---|---|
| [DESEMPENHO.md](DESEMPENHO.md) | diagnóstico, perfis e cada otimização com números |
| [MINIGAMES.md](MINIGAMES.md) | modos de jogo, minigames, motor comum, menu do port |
| [BATALHA-30FPS.md](BATALHA-30FPS.md) | por que a batalha continua a 15 fps |
| [BOTOES-PS.md](BOTOES-PS.md) | troca dos ícones de botão |
| [DATA.md](DATA.md) | como copiar os dados do jogo |
| [../PLANO.md](../PLANO.md) | análise do APK, fases e riscos |
