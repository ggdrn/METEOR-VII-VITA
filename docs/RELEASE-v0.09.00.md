# FINAL FANTASY VII — PS Vita v0.09.00

Install `ff7_vita_v0.09.00.vpk` over any previous version. Game data stays in
`ux0:data/ffvii-vita/` ([DATA.md](DATA.md)); no game data is included.

## New: port menu

Hold **L + R** and press **Start**. **L / R** switch tabs; the bottom of the menu
explains the selected item. Choices are saved in `cheats.txt`.

- **Cheats:** infinite HP/MP; battle EXP and gil x2/x3/x4; add any materia (picked
  by color) or summon, at level 1 or max.
- **Gameplay:** faster walking/running in fields and on foot on the world map
  (x2/x3/x4, nothing else speeds up); automatic dialog skip (questions are left to
  you); dialog language (English, French, German, Spanish, Japanese, applied at the
  next start); back to the title to load another save.

## Performance

- **Minigames:** steady 30 fps in the submarine (was 27.5), roller coaster and
  chocobo race (was 16–24); snowboard capped at a steady 30 fps; faster G-bike.
  Most of their drawing and math now runs as native code checked against the
  original.
- **Animated palettes:** paletted textures go to the GPU as indexes + palette
  instead of being converted on the CPU every time a palette changes (Chocobo
  Square 15 → 27 fps, chocobo ranch faster). Repeated conversions are cached.
- **Fields:** native vertex lighting and background tile lists, faster triangle
  submission for every 3D scene.
- **World map:** triangle and projection code faster again (no calls back into
  the game library in the hot loops).
- **Battles:** native vertex color scaling (~12% of the game thread).

## Fixes

- Button prompts show the right PS symbols: ✕ confirms, ○ cancels.
- First menu sound and save/load sounds are no longer missing.
- 448-line FMVs (e.g. the G-bike intro) fill the screen without distortion.
- Submarine: Square and the on-screen L2/R2 and R1+R2 buttons work.
- Less audio stutter when minigames stop and restart sounds.

## Switches

New in `ux0:data/ffvii-vita/`:

| File | Effect |
|---|---|
| `natives_off.txt` | every native replacement off (tells a port bug from the game) |
| `vcolor_off.txt`, `tri_off.txt`, `tri2_off.txt`, `math_off.txt`, `light_off.txt`, `texture_off.txt` | one native module off |
| `snowboard_60fps.txt`, `coaster_60fps.txt` | leave that minigame uncapped |
| `submarine_limiter_off.txt`, `chocobo_limiter_off.txt` | keep the game's own limiter |

## Known issues

- Snowboard music (the WIND track) is silent.
- Leaving the profiler on into a battle can crash the game; record short profiles.

When reporting a problem, send `logs.txt` and `logs_anterior.txt`.
