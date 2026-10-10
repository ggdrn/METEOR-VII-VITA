# Instalando os dados do jogo

Este port **não inclui nenhum arquivo do jogo**. Você precisa ter comprado
FINAL FANTASY VII na Google Play e copiar os arquivos da **sua** instalação.

Versão suportada: **1.0.29** (`com.square_enix.android_googleplay.FFVII`).

## 1. Obter os arquivos no seu Android

Com o jogo instalado e aberto ao menos uma vez (para baixar os dados adicionais):

| Arquivo | Onde fica no Android |
|---|---|
| APK | `adb shell pm path com.square_enix.android_googleplay.FFVII` → `adb pull <caminho>/base.apk` |
| OBB | `/sdcard/Android/obb/com.square_enix.android_googleplay.FFVII/main.30.com.square_enix.android_googleplay.FFVII.obb` |

## 2. Copiar para o PS Vita

```
ux0:data/ffvii-vita/base.apk
ux0:data/ffvii-vita/main.30.com.square_enix.android_googleplay.FFVII.obb
```

Não extraia nem renomeie o OBB: o port lê o arquivo original diretamente.

## 3. Requisitos no Vita

- **kubridge.skprx** na seção `*KERNEL` do `ux0:tai/config.txt` (ou `ur0:tai/config.txt`)
- **libshacccg.suprx** em `ux0:data/external/` ([como extrair](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx))
- ~2,1 GB livres em `ux0:`

Se faltar algum arquivo, o port mostra uma mensagem de erro com o caminho esperado.

## 4. Logs

A cada execução o port grava `ux0:data/ffvii-vita/logs.txt`. A execução
anterior fica em `logs_anterior.txt` (útil quando o jogo travou e foi reaberto).
Ao reportar um problema, envie os dois arquivos.
