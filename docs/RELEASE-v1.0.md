# FINAL FANTASY VII for PS Vita — v1.0

An unofficial port of the Android version of FINAL FANTASY VII to PlayStation Vita.
The original ARMv7 game library runs on the Vita through a compatibility layer,
with rendering provided by vitaGL.

## Highlights

- Optional **16:9 widescreen** for fields, battles and the world map. Switch
  between 4:3 and 16:9 in the port menu. Menus and movies retain their original
  aspect ratio.
- Widescreen adjustments for field backgrounds, world map terrain, sky and
  clouds, battle viewports, HUD alignment and dialog windows.
- Battle entry transitions fill the widescreen display when 16:9 is selected.
- Native optimizations for field rendering, the world map, battles and 3D
  minigames, including texture caching and GPU palette rendering.
- Frame pacing and performance improvements for the submarine, chocobo racing,
  roller coaster, snowboard and G-bike minigames.
- Physical Vita controls, PlayStation button prompts and right-stick camera
  rotation on the world map.
- A port menu with gameplay settings and optional cheats: movement speed,
  automatic dialog advance, language selection, EXP/gil multipliers, infinite
  HP/MP and materia selection.
- Audio, movie presentation and minigame input fixes from earlier releases.
- Developer tools are hidden from the standard port menu.

## Requirements

- A PS Vita capable of running homebrew.
- `kubridge.skprx` installed and enabled in the `*KERNEL` section of your taiHEN
  configuration.
- `libshacccg.suprx` in `ur0:data/` or `ur0:data/external/`.
- Your own legally purchased Android copy of FINAL FANTASY VII, **version 1.0.29**.
- Approximately 2.1 GB of free space for the game data, plus space for the port.

## Installation

1. Install **`ff7_vita_v1.0.vpk`** with VitaShell.
2. Copy the following files from your Android installation to the Vita:

   ```text
   ux0:data/ffvii-vita/base.apk
   ux0:data/ffvii-vita/main.30.com.square_enix.android_googleplay.FFVII.obb
   ```

3. Keep the APK and OBB intact; the port reads them directly.
4. Launch FINAL FANTASY VII from LiveArea.

If you already use an earlier version, install the VPK over it and keep your
existing game data. Back up `ux0:data/ffvii-vita/save/` before updating.
See [the installation guide](https://github.com/ggdrn/METEOR-VII-VITA/blob/HEAD/docs/DATA.md)
for details.

## Port menu

Hold **L + R** and press **Start**. Use **L/R** to switch tabs, **Up/Down** to
select an item, **Left/Right** to change it, **Cross** to confirm and **Circle**
to close the menu.

The standard menu contains **Cheats** and **Gameplay**.

In **Gameplay**, choose **SCREEN** to toggle 4:3/16:9. Language choices are
English, French, German, Spanish and Japanese; language changes apply after
restarting the game. Gameplay and cheat settings are stored in `cheats.txt`.

## Known limitations

- Snowboard music remains silent.
- Some field backgrounds, parallax layers or battle geometry may expose
  limitations in widescreen. Switch back to 4:3 if a scene does not display
  correctly.

For bug reports, include the port version, location, selected screen mode,
steps to reproduce and `ux0:data/ffvii-vita/logs.txt` plus `logs_anterior.txt`.

## Credits

Port by **Gustavo Garcia (ggdrn)**, with AI-assisted development.
Thanks to the contributors behind soloader-boilerplate, so-util, FalsoJNI,
vitaGL, VitaSDK, kubridge and the Vita homebrew community.

The VPK contains the port only. Game data must be supplied by the user.
FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with or endorsed
by Square Enix.
