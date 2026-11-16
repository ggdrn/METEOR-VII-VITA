# Símbolos de botão do PlayStation

O FF7 de Android herdou do relançamento de PC os nomes de botão do controle
de Xbox: as falas dizem "Aperte **(A)**", "**(B)** para cancelar" etc. O port
troca esses rótulos pelos símbolos do PlayStation, mantendo a função de cada
botão (o mapeamento físico do port):

| Original | Função no jogo | Botão do Vita | Símbolo | Cor |
|---|---|---|---|---|
| (B) | confirmar | Cruz | ✕ | azul (`{FE}{D3}`) |
| (A) | cancelar | Círculo | ○ | vermelho (`{FE}{D4}`) |
| (X) | □ do PS1 | Quadrado | □ | roxo (`{FE}{D5}`) |
| (Y) | menu | Triângulo | △ | verde (`{FE}{D6}`) |

**Correção da v0.6.34.** Os textos nomeiam os botões pela posição no
controle de Xbox, que o lançamento de PC sobrepôs ao do PlayStation: (B), à
direita, é o ○ do PS1, que confirma; (A), embaixo, é o ✕, que cancela
("selecione com o botão (B)", "(Y) para o Menu"). No Vita, a Cruz é o botão
A do Android, e o `updateGamepad()` da `.so` transforma esse A na tecla
`0x9C`, que o `ff7input.cfg` do jogo liga ao ○ (OK). Ou seja: a Cruz
confirma. Até a v0.6.33 o port desenhava (A) como ✕ e (B) como ○, e o texto
mostrava ○ onde a Cruz era quem funcionava. Agora (B) vira ✕ e (A) vira ○,
e o símbolo mostrado é o do botão que faz a ação. Os controles não mudaram.

Nenhum dado do jogo vai no VPK: os símbolos são desenhados pelo próprio port
em tempo de execução, nos dados que o jogo já carregou do OBB/APK do usuário.

**Para voltar ao original**: criar `ux0:data/ffvii-vita/psbuttons_off.txt`
(o conteúdo não importa) e reabrir o jogo. Vale para tudo abaixo.

## Como o jogo desenha esses rótulos

Há dois lugares diferentes:

1. **Botões de toque** (`assets/button_*.png` do APK, carregado por
   `MainActivity.ImageGetData`): um atlas 1024×1024 com os círculos A/B/X/Y
   do gamepad virtual. Já era tratado desde a v0.00.26 (`ff7_buttons_patch_atlas`):
   as letras são apagadas e os símbolos desenhados por cima.
2. **Texto** (falas, ajuda do mapa-múndi): os rótulos são **texto comum** na
   fonte de menu, quase sempre precedidos de um código de cor, imitando as
   cores do controle de Xbox: `{FE}{D6}(A){FE}{D9}`. A tradução pt-BR usa
   principalmente esse formato; os dados originais de outros idiomas também
   podem usar os opcodes de botão do PSX `F6`–`F9` (○ △ □ ✕), que o Android
   mostra como B/Y/X/A. Ocorrências na v1.0.29 com a tradução pt-BR do usuário:
   - `field/flevel.lgp`: 192 rótulos em 702 campos (190 com cor antes);
   - `wm/world_us.lgp`, arquivo `mes`: 8 rótulos (ajuda do buggy, Tiny
     Bronco, chocobo, submarino);
   - `ff7_en.exe`: 2 textos do menu de debug ("Pressione (A) para
     terminar"), não alterados;
   - batalha (`kernel2.bin`, `scene.bin`) e menus: nenhum.

A fonte de menu é `menu/menu_us.lgp` → `usfont_a_h.tex` + `usfont_b_h.tex`
(ou as versões `_l`): duas texturas 256×256 de 8 bits que formam uma grade de
21 células de 24×24 por linha; o caractere `c` fica na coluna `c % 21`, linha
`c / 21` (`draw_char`, x86 `0x6F564E`; falas: `0x6E706D`). Nos índices, 3 é o
corpo da letra e 1 a sombra (o corpo dilatado 0–2 px para a direita e para
baixo); a cor do texto vem do renderizador. As larguras ficam numa tabela de
bytes (ponteiro em `0xDB958C`, preenchida a partir do `window.bin`): bits 0–4
= avanço em unidades de 2 px, bits 5–7 = recuo à esquerda.

## O que o port faz (`source/ff7_buttons.c`, `source/ff7_buttons_core.h`)

1. **Fonte**: no início de `convert_image_data` (`ff7_texture.c`), toda
   textura 256×256 de 1 byte/pixel tem o hash FNV-1a das células dos
   caracteres `0x96`–`0x99` comparado com o da fonte original (`_h` ou `_l`,
   igual em todos os `menu_*.lgp`). Se bater, essas quatro células — símbolos
   matemáticos (∂ Σ ∏ π) que nenhum texto do jogo usa — são redesenhadas com
   ✕ ○ □ △, nos mesmos índices da fonte (traço de 2 px e sombra iguais aos das
   letras). Assim o símbolo pega qualquer cor de texto.
2. **Larguras**: as entradas de `0x96`–`0x99` passam de 7/7/9/8 para 10
   (20 px). Só são alteradas se ainda tiverem o valor original.
3. **Texto**: `{FE}{Dx}(B)` vira `{FE}{D3}✕` (e assim por diante), dentro da
   própria string, que encurta 2 bytes; os bytes liberados no fim viram `FF`.
   Desde a v1.2.1, o formato sem cor `(A)` também vira `{FE}{D4}○` no mesmo
   espaço de 3 bytes. Isso cobre os textos ingleses e traduções que não usam o
   código de cor antes do rótulo. Desde a v1.3.1, `F6`–`F9` são convertidos
   diretamente nos glifos do Vita; isso cobre os campos originais sem depender
   de uma tradução.
   É feito tanto na origem quanto no texto efetivamente entregue à janela:
   - campos: logo depois de o descompressor LZSS (`0x6305C0`, nativo em
     `ff7_lzss.c`) descompactar um arquivo de campo (reconhecido pelo
     cabeçalho: `u16 0`, 9 seções, primeira em `0x2A`, script versão `0x0502`);
     a tabela de strings da seção 0 é percorrida quando estiver embutida;
   - desde a v1.3.5, a entrada do parser final de texto de campo (`0x6E706D`)
     é interceptada. O quarto argumento x86 é o ponteiro exato que será
     desenhado; isso cobre qualquer idioma e tutoriais/minigames que copiam o
     texto. A alteração ocorre sincronamente na thread do jogo e os bytes
     originais são restaurados assim que o parser termina, sem deixar escritas
     nos blocos que uma transição de campo pode liberar/reutilizar;
   - mapa-múndi: gancho em `func_0x0074e1e9` (inicialização do mapa), que
     carrega `mes` em `0xE36120`; depois do original, a tabela é percorrida.

   A troca só acontece depois que a fonte recebeu os símbolos (ela é
   convertida no menu inicial, antes do primeiro campo). Se a fonte não for a
   esperada, o texto fica como no original — nunca aparece ∂ no lugar de (A).
   Textos de debug armazenados no executável não são percorridos por estes hooks.

Outros nomes de botão (`[LB]`, `[RB]`, `[BACK]`, `[START]`, `[RS]`) continuam
como texto; trocar `[BACK]` por `[SELECT]` aumentaria a string, o que a troca
no lugar não comporta.

## Log

Com a função ligada, o `logs.txt` mostra:

```
ff7_buttons: PlayStation button symbols on (ux0:data/ffvii-vita/psbuttons_off.txt disables them)
ff7_buttons: PlayStation symbols drawn in the _h font
ff7_buttons: renderer text at 02XXXXXX: 1 button labels -> PlayStation symbols
ff7_buttons: world map text: 8 button labels -> PlayStation symbols
ff7_buttons: button_jp_en.png: 7 PlayStation glyphs drawn
```

Se a linha "symbols drawn" não aparecer, a fonte não foi reconhecida e o
texto ficou com A/B/X/Y.

## Teste

A lógica pura (`ff7_buttons_core.h`) e o `ff7_buttons.c` real foram testados
no Mac, com a memória x86 emulada em páginas de 4 KB espalhadas, contra o OBB
v1.0.29: as 192 trocas em 12.952 strings de campo, a fonte pintada (só as 4
células mudam) e nenhuma mudança fora da tabela de strings. Falta conferir
no Vita (falas do Setor 1, ajuda do buggy no mapa-múndi).

## Correção v1.3.6 — dados originais de Android

O log da v1.3.5 dizia `strings offset equals section size` e pulava todos os
campos. A comparação com o OBB original mostrou que o tamanho armazenado em
`0x2A` pode cobrir só o script: a tabela de diálogos começa imediatamente
após ele e termina antes da próxima seção. O limite passa a ser o segundo
offset do cabeçalho, validado contra a quantidade de bytes descompactados.
Isso também aceita mods que incluem os diálogos no tamanho armazenado.

Os prompts originais analisados usam texto com **colchetes**, por exemplo
`{FE}{D5}[A]{FE}{D9}`, não os parênteses da tradução. Eles seguem o mapeamento
Android: `[A]` → ✕ azul, `[B]` → ○ vermelho, `[X]` → □ roxo, `[Y]` → △ verde.
A troca ocupa os mesmos três bytes e preserva o reset de cor seguinte.
O mapeamento antigo dos parênteses permanece para a tradução. Letras soltas,
`[SAVE]`, `[L1]` e `[R1]` não são substituídos.

O binário também contém um renderer japonês `_ex`, mas os formatos acima
foram confirmados diretamente nas tabelas do OBB original. Não é necessário
alterar esse renderer para corrigir estes prompts.

Teste de regressão: `python3 scripts/test_buttons_fields.py original.obb mod.obb`.
Ele executa as funções de patch extraídas do C real com páginas x86 separadas,
cobre os dois formatos de tamanho, rejeita limites inválidos e verifica que
nenhum byte fora da tabela de diálogos muda. Os testes não substituem a
validação visual no Vita.
