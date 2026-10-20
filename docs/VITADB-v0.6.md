# Name
FINAL FANTASY VII

# Version
0.6

# Changelog (Only if update)
- Right analog stick rotates the world map camera (same rate as L/R buttons, no effect outside world map).
- World map runs at a steady 30 fps. The four hottest world map routines were replaced with native ARM code that produces identical results (was 19–23 fps in v0.5).
- Fields that recreate textures every frame are faster: paletted conversion is split by destination format, and GL texture names are pre-generated so the game thread never stalls.
- Field screen zoom corrected to 448 lines (777×544, no distortion). `field_stretch_on.txt` enables full 960×544 stretch.
- Battle screens now use the same zoom, removing the black strip under the command menu.
- Profiler fixed: no more console reset on boot with `profile_on.txt`. Sampler runs on core 0 for exact instruction-level samples.
- New `profile_all.txt` switch for continuous profiling (every 20 ms while running).

# Screenshots (Optional)
-- Insert images here --

# Author
Gustavo Garcia (ggdrn)

# AI status
AI assisted (developed with Claude)

# Description
A port of the Android version of FINAL FANTASY VII (v1.0.29) to the PlayStation Vita. It is a loader, not an emulator: the game's original ARMv7 native library runs directly on the Vita CPU, and Android system calls (OpenGL ES, OpenSL ES, JNI, file and asset access) are redirected to Vita equivalents. Rendering goes through vitaGL on a dedicated render thread. Several hot routines of the recompiled game code are replaced with native implementations that produce identical results. Game data is read straight from the original APK and OBB files, with no extraction. No game data is included; the files must come from a legally purchased copy of the Android version.

# Short Description
Port of the Android version of FINAL FANTASY VII (loader, no emulation)

# Download Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v0.6/ff7_vita_v0.6.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v0.6

# Sourcecode Link (Optional)
https://github.com/ggdrn/METEOR-VII-VITA

# Data Files Link (Optional)
Not distributed. Use your own copy of the Android version (v1.0.29); see docs/DATA.md.

# Requirements (Optional)
- kubridge.skprx
- libshacccg.suprx
- Game Data Files: Android v1.0.29 (APK + OBB)
