# Encounter rate — v1.2.0

O menu Gameplay oferece `ENCOUNTER RATE < 0 (OFF) / 1 (NORMAL) / 2X / 4X >`.
Esquerda/direita percorrem as quatro opções em ambas as direções. A opção vale
nos cenários e no mapa-múndi, sem alterar a velocidade de movimento.

A chave existente `encounters` em `ux0:data/ffvii-vita/cheats.txt` passa a aceitar
0, 1, 2 ou 4. Configurações anteriores com 0/1 continuam válidas; sem a chave,
o padrão é 1. Valores inválidos voltam a 1.

O botão Android continua mostrando ON/OFF. Um toque desliga os encontros;
outro retoma o multiplicador anterior durante a sessão. O menu mostra o valor
exato. Ao reiniciar com OFF salvo, a taxa para a próxima ativação é NORMAL.
Esconder o botão permanece independente da taxa.

## Cálculos identificados no Android 1.0.29

- `UI_frame_refresh` zera/restaura o par de bytes em VA x86 `0xDBCAD8` quando
  o botão ON/OFF muda. Não multiplicamos esses bytes a cada frame: isso
  acumularia o fator e contaminaria o valor guardado pelo Android.
- Cenários: `func_0x0060b2c6` compara o número aleatório com
  `(danger * byte[0xDBCAD9]) >> 12`. Um wrapper multiplica apenas esse byte
  durante a função e restaura seu valor ao retornar. Os bytes vizinhos de
  preemptive/battle type ficam intactos. O produto satura em 255 para evitar
  overflow se uma configuração externa fornecer uma taxa excessiva.
- Mapa-múndi: `func_0x00767c55` retorna a taxa efetiva em EAX emulado. A função
  original dobra valores acima de 16 por conta das matérias. O wrapper
  multiplica o resultado, preservando esse cálculo sem aplicar a dobra duas vezes.
- OFF também zera a taxa nos wrappers, inclusive antes do próximo refresh da UI.
- As rotinas originais mantêm os testes de movimento e de áreas permitidas.
  A mudança não chama a rotina de batalha diretamente. Os multiplicadores
  aumentam a taxa usada nas verificações aleatórias; não garantem exatamente
  duas ou quatro vezes o número de lutas num percurso curto.
- Os dois hooks dependem dos símbolos, registradores e prólogos esperados.
  Se indisponíveis, o menu continua oferecendo apenas OFF/NORMAL.

## Verificação realizada

- `python3 scripts/test_encounters.py`: wrappers C reais com taxas 0/1/2/4,
  taxas-base 0..255, saturação, restauração sem acumulação, isolamento dos bytes
  vizinhos, persistência, configuração antiga/inválida e botão de toque.
- `python3 scripts/emu/test_encounter_rate.py build/libjni_ff7.so`: 63 casos
  executando código ARM original no Unicorn. Confere a comparação de
  probabilidade dos cenários e o retorno do mapa-múndi com Enemy Lure/Away.
  A parte de cenário executa o trecho da comparação, sem iniciar uma batalha.
- Compilação Release e `scripts/check-vpk.sh`: passaram, sem dados do jogo.

## Pendente no Vita

1. Num cenário com encontros, caminhar pelo mesmo percurso em NORMAL, 2X e 4X,
   repetindo para reduzir a variação aleatória. Conferir também OFF.
2. Repetir no mapa-múndi, com e sem Enemy Lure/Away.
3. Conferir áreas sem encontros, veículos, transições para batalhas de história,
   retorno de batalha, menus e carregamento de saves.
4. Em 2X/4X, desligar e religar pelo toque; conferir o menu. Repetir escondendo
   e mostrando o botão e verificar que a visibilidade não altera a taxa.
5. Reiniciar em cada opção e conferir a persistência; atualizar uma configuração
   antiga com `encounters 0` ou `encounters 1`.

O VPK de v1.2.0 usa versão interna 1.2.0 e APP_VER 01.20. A validação de
frequência em hardware permanece pendente. Notas públicas em inglês:
[release v1.2.0](RELEASE-v1.2.0.md).

## Revisão após o primeiro teste no Vita

O log fornecido confirma a instalação dos hooks, mas a primeira build não
registrava a taxa selecionada nem quantas vezes o hook era executado. Por isso
não comprova que 4X chegou ao cálculo durante a caminhada.

O teste ARM foi estendido até a acumulação de perigo e a comparação aleatória
do mapa-múndi (mesmo terreno, RNG determinístico e 10 mil verificações para
cada fator, modelando o retorno da batalha pelo reset de perigo). Resultado:
1X = 1005, 2X = 1459, 4X = 2096 encontros. O fator atual multiplica o incremento
de perigo; a frequência resultante não cresce linearmente. Assim, 4X nesta
implementação não significa quatro vezes mais lutas.

A build de diagnóstico registra a seleção e, a cada heartbeat, contagem de
chamadas e taxa original/efetiva nos dois hooks. Em 4X no mapa, com taxa nativa
16, espera-se `world N (rate 16 -> 64)` com N maior que zero durante o jogo.
A frequência percebida no hardware continua pendente; este diagnóstico não
muda a intensidade dos multiplicadores.
