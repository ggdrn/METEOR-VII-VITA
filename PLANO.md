# FINAL FANTASY VII — Port Android → PS Vita

Plano de ação e registro de progresso do port via *so-loader* (so_util + FalsoJNI + vitaGL).

**Regra inviolável:** o VPK não contém nenhum dado do jogo. Tudo vem da cópia do
próprio usuário (Play Store) e fica em `ux0:data/ffvii-vita/`. O build roda
`scripts/check-vpk.sh`, que falha se qualquer arquivo fora da allow-list entrar no VPK.

---

## 1. Análise técnica (APK v1.0.29)

| Item | Resultado |
|---|---|
| Pacote | `com.square_enix.android_googleplay.FFVII` |
| Motor | "materialg" (D4 Enterprise), encapsula o FF7 PC (`.lgp`, `ff7_xx.exe`, strings DirectInput) |
| Binário | uma única `lib/armeabi-v7a/libjni_ff7.so` (16 MB) |
| ABI | ARMv7 **softfp** (`VFPv3-D16`, sem `Tag_ABI_VFP_args`) → exige VitaSDK-softfp |
| Dependências | `libc`, `libm`, `libdl`, `libstdc++`, `liblog`, `libandroid` (AAsset), `libEGL`, `libGLESv2`, `libOpenSLES` |
| Gráficos | 45 funções GLES2 básicas, sem GLES3; shaders GLSL em `assets/Shaders/` |
| Áudio | músicas `.akb` (Vorbis decodificado **dentro** da .so) → saída por OpenSL ES |
| Vídeo | 114 `.webm` **VP8 640×448 + Vorbis** (606 MB), decodificados no Java (`MyDecoder`/MediaCodec) |
| OBB | `main.30....obb` (1,98 GB), **ZIP sem compressão**, 374 entradas em `assets/ff7_1.02/` |

### Pontos de entrada nativos (`jp.co.d4e.materialg.GLESJniWrapper`)
`JNI_OnLoad`, `setDataPath`, `setLang`, `setAssetManager`, `setBatteryLevel`,
`onSurfaceCreated`, `onSurfaceChanged`, `onDrawFrame`, `onPause`, `onResume`,
`onKey`, `onKeyBack`, `onTouchBegan/Moved/Ended`, `updateSaveDataFile`, `callUpdateTitlemenu`.

### Métodos Java chamados pela .so (a reimplementar no FalsoJNI)
| Classe | Métodos | Plano |
|---|---|---|
| `ExpansionFile` | `OPEN_FILE_DESCRIPTOR`, `CLOSE_FILE_DESCRIPTOR`, `GET_START_OFFSET`, `GET_LENGTH` | offsets direto do OBB original (stored) via `utils/zip.c` |
| `MyDecoder` | `START`, `FRAME`, `AFTER_FRAME`, `RESET`, `SET_TEXTURE`, `SET_VOLUME`, `GET_TOTALTIME`, `GET_POSITION`, `SET_POSITION`, `GET_YUV_PATCH` | nestegg + libvpx + libvorbis → textura YUV |
| `SEPlayer` | `PLAY`, `SETVOLUME` | decoder WAV + sceAudioOut/OpenSLES |
| `MainActivity` | `ImageGetData`, `ImageGetWidth`, `ImageGetHeight`, `postErrorMessage`, `postDeleteSaveFile`, `OpenURL`, `CloudLaunch` | stb_image; o resto vira stub/log |

---

## 2. Layout dos dados no Vita

```
ux0:data/ffvii-vita/
├── base.apk                                                  ← APK original (a .so é lida de dentro dele)
├── main.30.com.square_enix.android_googleplay.FFVII.obb      ← OBB original, sem modificação
├── Documents/                                                ← criado pelo loader: saves, ff7opt.cfg, APP.LOG do jogo
├── logs.txt                                                  ← log da execução atual
└── logs_anterior.txt                                         ← log da execução anterior
ur0:data/libshacccg.suprx                                     ← compilador de shaders (padrão vitaGL)
```
Plugin obrigatório: `kubridge.skprx` no `config.txt` do taiHEN (`*KERNEL`).

## Versões e logs

- Cada build entregue para teste sai em `build/VPK/ff7_vita_vX.XX.YY.vpk`; a versão fica em `PORT_VERSION` no `CMakeLists.txt` e sobe a cada build (v0.00.01, v0.00.02, … pode passar de 100).
- O loader grava `logs.txt` sem buffer (sobrevive a crash), com tempo e thread em cada linha. Mensagens do so_util, do FalsoJNI, `printf` e `__android_log_print` do jogo também vão para lá.

---

## 3. Fases

### ✅ Fase 0 — Ambiente (concluída)
- [x] Repositório git em `FFVII - VITA/`, `.gitignore` bloqueando qualquer dado do jogo
- [x] Base: [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate) (MIT)
- [x] Submódulos: `lib/so_util`, `lib/falso_jni`, `lib/vitagl` (vitaGL compilado com `SOFTFP_ABI=1` pelo CMake)
- [x] VitaSDK-softfp em `~/vitasdk-softfp` via `scripts/setup-softfp-sdk.sh`
- [x] Configuração FF7: `ux0:data/ffvii-vita/`, Title ID `FFSQ00007`
- [x] `utils/zip.c`: leitor ZIP próprio (stored + deflate, offsets 64-bit via sceIo)
- [x] Loader valida APK + OBB e carrega a `.so` **direto do APK** (`so_mem_load`)
- [x] `scripts/check-vpk.sh` ligado ao build: VPK só pode ter eboot, LiveArea e shaders
- [x] Build verificado: `ff7_vita.vpk` (1,9 MB, 10 arquivos, binário softfp)

### ⏳ Fase 1 — Boot até a primeira imagem (código pronto em v0.00.01, aguardando teste no Vita)
- [x] Logger em `ux0:data/ffvii-vita/logs.txt` (+ `logs_anterior.txt`)
- [x] `dynlib.c`: todos os 224 imports mapeados (verificado offline); `lseek64` real; IIDs OpenSL; `eglGetCurrentDisplay`/`eglSurfaceAttrib`
- [x] AAssetManager lendo `assets/` direto do `base.apk` (+ `openFileDescriptor64` com offset no APK)
- [x] `java.c`: `ExpansionFile` servindo fd/offset/tamanho do OBB original; `ImageGet*` com stb_image; vídeo/som como stubs com log
- [x] Shaders: remoção de `GL_OES_EGL_image_external` / `samplerExternalOES` → `sampler2D`
- [x] Boot igual ao Android (decompilado): `JNI_OnLoad` → `setLang` → `setDataPath` → `setAssetManager` → `onResume` → `onSurfaceChanged(960,544)` → `onSurfaceCreated(false,false)` → `onDrawFrame` a partir do 3º frame
- [x] Controles ligados (IDs de `MainActivity.getKeyId`), analógico esquerdo como direcional, touch frontal
- [ ] Teste no Vita e correções a partir do `logs.txt`
- [ ] **Spike de risco:** medir decodificação VP8 640×448 com libvpx no Vita (sample isolado)
- **Meta:** tela-título renderizando

### Fase 2 — Dados do jogo
- [ ] `ExpansionFile`: abrir o OBB, devolver fd + offset + tamanho de cada entrada
- [ ] Verificar como a .so usa o fd (`lseek64`/`read` nativos) e mapear para sceIo/newlib
- [ ] `ImageGetData/Width/Height` com stb_image
- [ ] Idioma via `sceAppUtilSystemParamGetInt(LANG)` → `setLang`

### Fase 3 — Áudio
- [ ] OpenSL ES: usar o pacote `opensles` softfp ou shim próprio (Engine, OutputMix, BufferQueue, Play, Volume) sobre sceAudioOut
- [ ] `SEPlayer` (efeitos)
- [ ] Checar estouro de buffer/latência nas músicas `.akb`

### Fase 4 — Controles
- [ ] Botões do Vita → `AKEYCODE_BUTTON_*`/DPAD em `onKey`; Start/Select → menus; Back → `onKeyBack`
- [ ] Analógico esquerdo → DPAD
- [ ] Touch frontal → `onTouchBegan/Moved/Ended` (escala 960×544)
- [ ] Ícones de botão: usar `button_jp_en.png` (layout Xbox/PlayStation?) — avaliar troca para símbolos PS

### Fase 5 — Vídeos (FMVs)
- [ ] `MyDecoder`: nestegg (demux WebM) + libvpx (VP8) + libvorbis, em thread separada
- [ ] Upload YUV como textura (`SET_TEXTURE`, `GET_YUV_PATCH`, `Shader_yuv.fsh`)
- [ ] Sincronia A/V (`GET_POSITION`)
- [ ] **Plano B** se o FPS não bastar: script no PC do usuário converte os `.webm` dele para H.264 e o port usa `sceAvPlayer` (decodificação por hardware)

### Fase 6 — Sistema
- [ ] Saves em `ux0:data/ffvii-vita/Documents/` (`save%02d.ff7`) (`updateSaveDataFile`, `postDeleteSaveFile`)
- [ ] Suspend/resume (`onPause`/`onResume`), bateria (`scePowerGetBatteryLifePercent`)
- [ ] `OpenURL`/`CloudLaunch` como no-op

### Fase 7 — Desempenho e release
Diagnóstico e plano detalhados em [docs/DESEMPENHO.md](docs/DESEMPENHO.md)
(campos pesados limitados pela thread única do jogo; `mk_address` é o gargalo).
- [x] v0.00.37: `mk_address` mais rápida e 71.246 acessos a globais sem chamada (`source/ff7_memmap.c`)
- [ ] Medir com/sem `memmap_off.txt` no Setor 7; perfil com `ENABLE_SPIKE_PROFILER`
- [x] v0.00.38: thread de render (GL da thread do jogo executado nos cores 1–2, `source/reimpl/gl_queue.c`)
- [ ] Acessos à pilha x86 (`esp/ebp ± constante`) sem chamada

- [ ] Perfil de memória (limite ~365 MB do app), cache de shaders compilados
- [ ] Clock 444/222/222/166 MHz já aplicado no init; medir se dá para baixar
- [ ] LiveArea própria (arte original do port, sem assets da Square Enix)
- [ ] README de instalação + release

---

## 4. Riscos

| Risco | Impacto | Mitigação |
|---|---|---|
| Decodificação VP8 em software | alto | spike na Fase 1; plano B H.264 + sceAvPlayer |
| Toolchain softfp de macOS é de abr/2024 (x86_64/Rosetta) | médio | headers/stubs atualizados a partir do VitaSDK 2026; vitaShaRK recompilado; alternativa: build Linux v2.23 em VM/Docker |
| Memória (motor carrega `.lgp` de até 130 MB?) | médio | medir na Fase 2; se preciso, hook em alocações/leitura sob demanda |
| Patches específicos do motor (threads, `syscall`) | baixo | `patch.c` com hooks do so_util |

---

## 5. Notas do ambiente (macOS ARM)

O `setup-softfp-sdk.sh` resolve automaticamente:
1. **libzstd**: o GCC x86_64 aponta para `/usr/local/opt/zstd` (Homebrew Intel). O script compila uma libzstd x86_64 e religa `cc1`/`cc1plus`/`lto1`.

2. **vita-headers**: os de 2024 não têm flags que o vitaGL atual usa (`SCE_GXM_INITIALIZE_FLAG_*`). Copiados `include/psp2*`, `vitasdk*` e `lib/*_stub*.a` do VitaSDK hardfp (`~/vitasdk`); são independentes de ABI de float. Backup em `~/vitasdk-softfp/backup-vita-headers-2024/`. Origem configurável via `VITASDK_HARDFP`.
3. **vitaShaRK**: pacote softfp antigo, sem `shark_set_shader_association_path`. Recompilado do [fonte](https://github.com/Rinnegatamante/vitaShaRK) com `make install`.

O caminho do projeto tem espaços, e o `vita_create_vpk()` do SDK quebra com isso. Por isso o empacotamento do VPK é uma regra própria no `CMakeLists.txt`.
