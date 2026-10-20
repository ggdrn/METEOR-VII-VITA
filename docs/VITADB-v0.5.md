# Name
FINAL FANTASY VII

# Version
0.5

# Changelog (Only if update)
- The x86 stack is accessed directly. The heaviest scenes now run at a steady 30 fps, where v0.2 ran at 28–29 fps.
- Fewer render thread wake-ups, which lowers the per-frame overhead.
- Music tracks load in the background, so area and battle transitions no longer freeze while a track loads.
- Field screens fill the whole screen height.
- Saves moved to `ux0:data/ffvii-vita/save/`. Saves from older versions are copied there automatically.
- The floating Escape button was removed; holding L + R escapes from battles.
- Native code self-tests are off by default.
- New LiveArea artwork.

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
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v0.5/ff7_vita_v0.5.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v0.5

# Sourcecode Link (Optional)
https://github.com/ggdrn/METEOR-VII-VITA

# Data Files Link (Optional)
Not distributed. Use your own copy of the Android version (v1.0.29); see docs/DATA.md.

# Requirements (Optional)
- kubridge.skprx
- libshacccg.suprx
- Game Data Files: Android v1.0.29 (APK + OBB)
