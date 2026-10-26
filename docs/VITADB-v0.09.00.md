# Name
FINAL FANTASY VII

# Version
0.09.00

# Changelog (Only if update)
- New port menu (L + R + Start; L/R switch tabs; each item explained on screen).
- Cheats: infinite HP/MP, battle EXP and gil x2-x4, add any materia or summon (level 1 or max).
- Gameplay: faster walking in fields and on foot on the world map (x2-x4), automatic dialog skip (not questions), dialog language (EN/FR/DE/ES/JA, next start), back to title.
- Debug: shortcuts to every 3D minigame and to the world map on a vehicle, profiler tools.
- Steady 30 fps in the submarine, roller coaster, chocobo race and snowboard; faster G-bike.
- Animated palettes rendered on the GPU (Chocobo Square 15 -> 27 fps); faster fields, world map and battles.
- Fixed: PS button prompts (X confirms, O cancels), missing first menu/save sounds, FMV zoom, submarine Square and on-screen L2/R2.
- Known issue: snowboard music is silent.

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
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v0.09.00/ff7_vita_v0.09.00.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v0.09.00

# Sourcecode Link (Optional)
https://github.com/ggdrn/METEOR-VII-VITA

# Data Files Link (Optional)
Not distributed. Use your own copy of the Android version (v1.0.29); see docs/DATA.md.

# Requirements (Optional)
- kubridge.skprx
- libshacccg.suprx
- Game Data Files: Android v1.0.29 (APK + OBB)
