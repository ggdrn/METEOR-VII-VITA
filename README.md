# FINAL FANTASY VII — PS Vita

*Loader* que executa a biblioteca nativa da versão Android de FINAL FANTASY VII
(v1.0.29) no PlayStation Vita, sem emulação: a `.so` ARMv7 original roda direto
na CPU, com as chamadas de sistema Android redirecionadas para equivalentes do Vita.

> **Nenhum dado do jogo é distribuído.** É preciso ter o jogo comprado na
> Google Play e copiar os arquivos da sua instalação. Veja [docs/DADOS.md](docs/DADOS.md).

Status e roteiro: [PLANO.md](PLANO.md).

## Build

Requisitos: macOS ou Linux x86_64, `cmake`, `git`, VitaSDK (hardfp) em `~/vitasdk`
(usado só como fonte de headers atualizados no macOS).

```sh
git clone --recurse-submodules <repo> && cd <repo>
scripts/setup-softfp-sdk.sh        # instala VitaSDK-softfp em ~/vitasdk-softfp
source scripts/env.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8            # gera build/ff7_vita.vpk
```

O build termina com `scripts/check-vpk.sh`, que falha se o VPK tiver qualquer
arquivo além do loader e da LiveArea.

Alvos úteis (defina `-DPSVITAIP=<ip>`): `send`, `send_kvdb`, `dump`, `reboot`.

## Estrutura

```
source/main.c        laço principal e controles
source/dynlib.c      tabela de imports da .so → funções nativas do Vita
source/java.c        métodos Java reimplementados (FalsoJNI)
source/patch.c       patches específicos do jogo
source/reimpl/       reimplementações de APIs Android (AAsset, EGL, pthread...)
source/utils/zip.c   leitor do APK/OBB originais, sem extração
lib/                 so_util, FalsoJNI, vitaGL (submódulos), kubridge, libc_bridge
scripts/             setup do SDK, ambiente, verificação do VPK
```

## Créditos

- Base: [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate) por Volodymyr Atamanenko (gl33ntwine)
- [TheFloW](https://github.com/TheOfficialFloW): so-loader original, gtasa_vita
- [Rinnegatamante](https://github.com/Rinnegatamante): vitaGL, so_util, vitaShaRK
- [fgsfds](https://github.com/fgsfds): diversas contribuições

FINAL FANTASY VII © SQUARE ENIX. Este projeto não é afiliado à Square Enix.

## Licença

MIT. Veja [LICENSE](LICENSE).
