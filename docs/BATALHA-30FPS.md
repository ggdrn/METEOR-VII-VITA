# Batalha a 30 fps: viabilidade (adiado)

Avaliado em 22/10/2026. Decisão: **não fazer por enquanto.**

## Situação atual

- As batalhas rodam a **15 fps**, o ritmo do próprio motor (o mesmo do FF7
  de PC). Não é queda de desempenho.
- Nos logs, a batalha fica cravada em 14,8 fps com a thread do jogo
  trabalhando ~40% do tempo e dormindo o resto: há folga de CPU, o limite é
  proposital (~27 ms de trabalho por quadro).

## Por que não basta destravar o limitador

A lógica da batalha avança um passo por quadro. Com o limitador em 30 fps,
tudo roda no dobro da velocidade: animações, câmera, barras de ATB, magias
e invocações.

## Caminhos possíveis (ambos grandes)

1. **Meio passo por quadro.** Rodar a 30 fps e fazer cada quadro avançar
   metade. Exige achar e corrigir cada contador por quadro do módulo de
   batalha: centenas de pontos, porque cada magia, invocação e efeito tem os
   seus. É o caminho que o FFNx seguiu no PC para 30/60 fps em batalha, com
   bastante trabalho e correções efeito por efeito.
2. **Interpolação.** Manter a lógica a 15 fps e desenhar um quadro
   intermediário (modelos e câmera no meio do caminho). O jogo transforma os
   modelos em software a cada quadro, então seria preciso reproduzir esse
   pipeline para gerar o quadro extra. Efeitos 2D e partículas continuariam
   a 15 fps.

## Custo de processamento

Nos dois caminhos o trabalho por segundo quase dobra. Os ~27 ms por quadro de
hoje caberiam nos 33 ms de um quadro a 30 fps nas batalhas comuns, mas
invocações e cenas pesadas cairiam abaixo disso e precisariam de otimização
como a da moto e do mapa-múndi (`docs/DESEMPENHO.md`).

## Se for retomado

Começar por um teste de viabilidade, antes de qualquer implementação:

1. Achar o limitador de quadros da batalha.
2. Medir quantos contadores por quadro existem num combate simples.
3. Prototipar a interpolação só de câmera e modelos.

Isso diz com números se vale seguir. O risco principal é quebrar efeitos; o
ganho é só de fluidez, já que o jogo roda na velocidade certa.
