# Acesso de desenvolvedor ao menu DEBUG

A aba DEBUG fica oculta por padrão: não é desenhada e L/R alternam apenas
entre CHEATS e GAMEPLAY. Nenhuma opção de ativação aparece no menu do port.

Para reativá-la, crie um arquivo vazio chamado `debug_menu.txt` em
`ux0:data/ffvii-vita/` e reinicie o jogo. Abra o menu com L + R + Start;
L/R passam a incluir DEBUG, com os atalhos, cenas, profiler e testes A/B.

Para ocultar novamente, remova o arquivo e reinicie o jogo. O arquivo não é
incluído no VPK. As opções salvas em `cheats.txt` não ativam essa aba.

Este é um acesso discreto para desenvolvimento, sem autenticação de usuário.
