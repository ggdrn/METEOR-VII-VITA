# FINAL FANTASY VII — PS Vita v0.09.00

Install `ff7_vita_v0.09.00.vpk` over any previous version. Game data stays in
`ux0:data/ffvii-vita/` ([DATA.md](DATA.md)); no game data is included.

## New: port menu

Hold **L + R** and press **Start**. **L / R** switch tabs; the bottom of the menu
explains the selected item. Choices are saved in `cheats.txt`.

- **Cheats:** infinite HP/MP, battle EXP and gil x2/x3/x4, add any materia
  (by color) or summon at level 1 or max.
- **Gameplay:** faster walking/running in fields and on foot on the world map
  (x2/x3/x4, nothing else speeds up), automatic dialog skip (questions are
  left to you), dialog language (English, French, German, Spanish, Japanese;
  applies at the next start), back to the title to load another save.
- **Debug:** minigame and world map vehicle shortcuts, scene markers, profiler
  and A/B switches, for testing and performance reports.

## Performance

- 3D minigames at a steady 30 fps: submarine, roller coaster, chocobo race;
  snowboard and G-bike faster (native drawing code).
- Faster world map, field lighting/backgrounds and battles (more native code,
  paletted textures uploaded to the GPU as indexes + palette).

## Fixes

- Button prompts show the right PS symbols (✕ confirms, ○ cancels).
- First menu and save/load sounds no longer missing.
- Submarine on-screen L2/R2 buttons.

## Switches

New in `ux0:data/ffvii-vita/`: `natives_off.txt` turns every native
replacement off (to tell a port bug from the game's behavior).

When reporting a problem, send `logs.txt` and `logs_anterior.txt`.
