# FINAL FANTASY VII — PS Vita v0.6

Install `ff7_vita_v0.6.vpk` over any previous version. Game data stays where it was
(`ux0:data/ffvii-vita/`, see [DATA.md](DATA.md)); no game data is included.

## Performance

- **World map native renderer.** The four hottest routines of the world map renderer
  (`0x75F0AD`, `0x75F263`, `0x68DAE1`, `0x75F68C`) were replaced with native ARM
  implementations that produce identical results. Together they accounted for ~57% of
  the game thread on the world map. Frame rate went from 19–23 fps (v0.5) to a steady
  30 fps.
- **Faster paletted texture conversion.** Fields that recreate palettised textures every
  frame had separate conversion and GL sync costs of ~22% of the game thread. The
  conversion loop is now split by destination format (one table lookup and one write per
  pixel); GL texture names are pre-generated on the render thread so `glGenTextures`
  never stalls waiting for the queue.

## Controls

- **Right analog stick rotates the world map camera.** Deflect right to rotate
  right, left to rotate left — same speed as holding L/R. The buttons still work
  as before. The stick has no effect outside the world map.

## Changes

- **Field screen zoom corrected.** The zoom introduced in v0.2 was tracking 464 of 480
  lines; it now follows the actual drawing area the game sets (448 lines by default),
  filling 777×544 without distortion. `field_stretch_on.txt` stretches to the full
  960×544 instead.
- **Battle mode zoom.** The same zoom now applies to battles, removing the black strip
  under the command menu. The zoom uses 472 lines so that all submenus remain visible.
- **Profiler fixed.** With `profile_on.txt`, the console no longer resets on boot. The
  sampler now runs on core 0 at higher priority (exact instruction-level samples),
  activates 20 s after boot (after all startup patches finish), and coordinates safely
  with the stack-patching pass.
- **Continuous profiling mode.** `profile_all.txt` samples every 20 ms while the game
  thread is running, instead of only during slow frames. Useful for scenes like the
  world map that sleep briefly each frame.
- **World map game mode.** When loading a save directly on the world map, the screen
  zoom now activates correctly (the game mode was not read until the first field screen
  in earlier versions).

## Switches

Creating one of these empty files in `ux0:data/ffvii-vita/` controls optional behaviour:

| File | Effect |
|---|---|
| `stack_off.txt` | stack accesses through the original translation |
| `stack_check.txt` | checks every stack access and logs any outside the stack (slower) |
| `music_async_off.txt` | music read on the game thread, as before |
| `field_zoom_off.txt` | field and battle screens with the bottom strip, as before |
| `field_stretch_on.txt` | stretch field and battle to the full 960×544 (distorted) |
| `glbatch_off.txt` | render thread woken on every draw, as before |
| `world_off.txt` | all world map native routines disabled |
| `world_tri_off.txt` | world map triangle renderer native routine disabled only |
| `selftest_on.txt` | runs the native code self-tests on boot |
| `profile_on.txt` | spike profiler: writes `profile.bin` for slow frames |
| `profile_all.txt` | continuous profiler: samples every 20 ms while running |

When reporting a problem, send `logs.txt` and `logs_anterior.txt` from
`ux0:data/ffvii-vita/`.
