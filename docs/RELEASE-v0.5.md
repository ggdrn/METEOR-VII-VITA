# FINAL FANTASY VII — PS Vita v0.5

Install `ff7_vita_v0.5.vpk` over any previous version. Game data stays where it was
(`ux0:data/ffvii-vita/`, see [DATA.md](DATA.md)); no game data is included.

## Performance

- **x86 stack accesses without address translation.** Half of all memory accesses of
  the recompiled game code went to the x86 stack through a page-table lookup. The stack
  is now mapped so that each of those 333,453 accesses is a single instruction. In the
  heaviest Sector 7 scene the game thread went from 96% busy at 28–29 fps (v0.2) to
  74–76% busy at a steady 30 fps.
- **Fewer render thread wake-ups**: from ~4,000 to ~600 kernel calls per second in busy
  scenes.
- **Music loads in the background.** Area and battle tracks that were not cached froze
  the game for 170–600 ms while being read; now the game keeps running and the music
  starts once it is loaded.
- Self-tests of the native code no longer run on every boot.

## Changes

- **Field screens fill the whole height.** The empty strip the game leaves under field
  screens (16 of 480 lines) is no longer shown: the picture is scaled by 3.4%, without
  distortion. Battles, menus, movies and the world map are unchanged.
- **Saves have their own folder**: `ux0:data/ffvii-vita/save/` (`save00.ff7`–`save09.ff7`).
  Saves from earlier versions are copied there on the first run; the originals stay in
  `Documents/`.
- The floating **Escape** button in battles was removed. Hold **L + R** to run away, as in
  the original game.
- New LiveArea artwork.

## Switches

Creating one of these empty files in `ux0:data/ffvii-vita/` turns a change off, for
comparisons or if something goes wrong:

| File | Effect |
|---|---|
| `stack_off.txt` | stack accesses through the original translation |
| `stack_check.txt` | checks every stack access and logs any outside the stack (slower) |
| `music_async_off.txt` | music read on the game thread, as before |
| `field_zoom_off.txt` | field screens with the bottom strip, as before |
| `glbatch_off.txt` | render thread woken on every draw, as before |
| `selftest_on.txt` | runs the native code self-tests |
| `profile_on.txt` | writes `profile.bin` for performance analysis |

When reporting a problem, send `logs.txt` and `logs_anterior.txt` from the same folder.
