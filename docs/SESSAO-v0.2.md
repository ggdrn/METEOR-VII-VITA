# Resumo da sessão: v0.00.36 → v0.2

Trabalho feito na branch `claude/vigilant-maxwell-04trh8`, do commit `7bcc8ce` (v0.00.37) ao
`8f18a0f` (correção da LiveArea). Os detalhes técnicos e as medições estão em
[DESEMPENHO.md](DESEMPENHO.md); este arquivo resume o que foi feito e o que falta checar.

## O que foi feito

### Vídeo de abertura (v0.00.37)
- **Problema:** rasgos e glitches no vídeo. O quadro era copiado com `memcpy` para a textura
  enquanto a GPU (que executa de forma adiada no vitaGL) ainda lia o quadro anterior.
- **Correção:** cada quadro é enviado com `glCompressedTexImage2D` (`VGL_YUV420P_BT601`), o que
  aloca memória nova para a textura. `upload()` chama `gl_queue_finish()` antes de enviar.
- Arquivo: `source/reimpl/movie.c`.

### Desempenho

| Versão | Mudança | Cena pesada (Setor 7) |
|---|---|---|
| v0.00.36 | ponto de partida | 17,8 fps |
| v0.00.37 | `mk_address` mais rápida; 71.246 acessos a globais x86 sem chamada | — |
| v0.00.38 | thread de render: o GL da thread do jogo roda nos cores 1–2 | 19,6 fps |
| v0.00.39 | swap sem esperar a fila inteira (corrigiu um swap de 481 ms); profiler ligável por arquivo | — |
| v0.00.40 | iluminação escalada de vértices (`0x68DE75`) nativa | — |
| v0.00.41 | renderizador de triângulos (`C_006A8002`) nativo; fila GL mais leve; laço de `0x68E92D` nativo | ~29 fps |
| v0.00.42 | diagnóstico de desenhos sem array de vértices (resultado: 0) | ~29–30 fps |

- **Thread de render** (`source/reimpl/gl_queue.c`): as 53 chamadas GL do jogo entram num ring
  buffer de 8 MB e são executadas por outro core. A thread de render só acorda em desenhos,
  espera ativamente um pouco antes de dormir e descarta chamadas redundantes (`glUseProgram`,
  `glUniform1i` e enable de atributo que não mudam nada).
- **Funções nativas** (`ff7_light.c`, `ff7_tri.c`, `ff7_light_core.h`, `ff7_tri_core.h`): versões
  em C das funções x86 recompiladas, com resultado idêntico ao original. Cada uma tem um
  auto-teste no aparelho, nas primeiras chamadas, que compara o resultado com o código original,
  e testes offline em `scripts/emu/` (Unicorn): `test_light2.py`, `test_tri.py`,
  `test_e92d_loop.py`, todos com 0 divergências.
- **4º core do Vita:** não dá ganho. Ele é reservado ao sistema, e o jogo já usa o core 0 para a
  thread do jogo e os cores 1–2 para o render.
- **Shader cache / pós-processamento:** avaliados e descartados. Não atacam o gargalo, que é a CPU
  na thread do jogo.

### Chaves de desligar (arquivos na pasta de dados)
Se algo der errado, criar o arquivo volta ao comportamento antigo daquela parte:
- `glthread_off.txt`: desliga a thread de render.
- `memmap_off.txt`: desliga a `mk_address` rápida e o patch das globais.
- `profile_on.txt`: liga o profiler (gera `profile.bin`).

### Documentação e release
- `README.md` traduzido para o inglês.
- `docs/DADOS.md` virou `docs/DATA.md`, em inglês.
- Versão 0.2 (`PORT_VERSION "0.2"`, `VITA_VERSION "00.20"`) e texto curto do release em inglês.

### Erro 0x8011013D na instalação
- **Causa:** as novas imagens da LiveArea eram PNG RGBA de 32 bits em tamanhos errados.
- **Correção:** a mesma arte foi ajustada sem corte, sobre fundo branco, e convertida para PNG de
  8 bits com paleta: `icon0` 128×128, `bg0` 840×500 e `startup` 280×158 (`pic0` continua
  960×544).
- `scripts/check-vpk.sh` agora confere tamanho e formato dessas imagens e falha o build se
  alguma estiver fora do padrão.

## O que precisa ser checado

### Antes de publicar o v0.2
- [ ] `git pull`, recompilar e confirmar que o `check-vpk.sh` passa.
- [ ] Instalar o novo VPK no Vita e confirmar que o erro 0x8011013D sumiu e que a LiveArea
      aparece certa (ícone, fundo, tela de início).
- [ ] Criar a tag `v0.2` pela interface do GitHub (o proxy desta sessão bloqueou o push de tags)
      e anexar **o VPK novo** ao release, não o que deu erro.
- [ ] Decidir sobre a arte da LiveArea. As imagens atuais são o logo e a arte oficial da Square
      Enix; o `PLANO.md` previa arte original do port.

### Testes no jogo
- [ ] Vídeo de abertura inteiro, sem rasgos.
- [ ] Nos logs, conferir que os auto-testes passam (iluminação, triângulos, laço `0x68E92D`,
      `mk_address`) sem nenhuma divergência.
- [ ] Linha "render thread:" do log: os desenhos sem array de vértices (bare draws) devem
      continuar em 0.
- [ ] Áreas além do Setor 7: batalhas, mapa-mundo, cenas com muitos modelos, menus e
      transições de tela (swirl).
- [ ] Save/load e sessões longas (estabilidade e memória com a thread de render).

### Pontos em aberto conhecidos
- **Travadas nas trocas de área:** cerca de 11 travadas de 0,7–0,8 s por sessão. Vêm
  principalmente do código do jogo. O próximo passo seria perfilar essas transições com
  `profile_on.txt`.
- **Estatística "KB/s copied":** parece baixa demais nas cenas pesadas. O v0.00.42 mostrou que
  não há desenhos sem vértices, então é só um problema de contagem; não afeta o jogo.
- **Opção `no_clock.txt`:** sugerida, ainda não implementada.
