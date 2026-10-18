# Installing the game data

This port **does not include any game files**. You need to have bought
FINAL FANTASY VII on Google Play and copy the files from **your own** installation.

Supported version: **1.0.29** (`com.square_enix.android_googleplay.FFVII`).

## 1. Get the files from your Android device

With the game installed and opened at least once (so it downloads the additional data):

| File | Where it is on Android |
|---|---|
| APK | `adb shell pm path com.square_enix.android_googleplay.FFVII` → `adb pull <path>/base.apk` |
| OBB | `/sdcard/Android/obb/com.square_enix.android_googleplay.FFVII/main.30.com.square_enix.android_googleplay.FFVII.obb` |

## 2. Copy them to the PS Vita

```
ux0:data/ffvii-vita/base.apk
ux0:data/ffvii-vita/main.30.com.square_enix.android_googleplay.FFVII.obb
```

Do not extract or rename the OBB: the port reads the original file directly.

## 3. Requirements on the Vita

- **kubridge.skprx** in the `*KERNEL` section of `ux0:tai/config.txt` (or `ur0:tai/config.txt`)
- **libshacccg.suprx** in `ur0:data/` or `ur0:data/external/` ([how to extract it](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx))
- ~2.1 GB free on `ux0:`

If a file is missing, the port shows an error message with the expected path.

## 4. Saves

Saves are kept in `ux0:data/ffvii-vita/save/` (`save00.ff7` to `save09.ff7`);
back up that folder to keep your progress. Options stay in
`ux0:data/ffvii-vita/Documents/`. Before v0.2.03 saves were written to
`Documents/`: on the first run of a newer version they are copied to `save/`
(the originals are left in place).

## 5. Logs

On every run the port writes `ux0:data/ffvii-vita/logs.txt`. The previous run
is kept in `logs_anterior.txt` (useful when the game crashed and was reopened).
When reporting a problem, send both files.
