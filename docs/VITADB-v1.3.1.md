# Name
FINAL FANTASY VII

# Version
1.3.1

# Changelog (Only if update)
- Added Gameplay > SIDE BORDERS: GRADIENT/BLACK for solid-black borders on centered 4:3 screens, including menus and movies.
- Fixed PlayStation button prompts that were missing with the original Android game data without a modified field archive.
- Stock `[A]`, `[B]`, `[X]` and `[Y]` labels and field dialog layouts are now converted correctly; translated or modified data keeps the existing prompt conversion support.
- Existing saves, settings, APK and OBB remain compatible with this update.

# Author
Gustavo Garcia (ggdrn)

# AI status
AI-assisted development.

# Description
An unofficial port of the Android version of FINAL FANTASY VII (v1.0.29) to PlayStation Vita. The original ARMv7 game library runs directly on the Vita through a compatibility layer, with vitaGL rendering and native optimizations for fields, battles, the world map and 3D minigames. Features include solid-black or gradient 4:3 side borders, selectable encounter calculation multipliers, an optional encounter touch button, optional 16:9 widescreen, physical Vita controls, right-stick world map camera rotation, gameplay settings and optional cheats. The port reads the original APK and OBB directly. Game data is not included and must come from the user's own legally purchased Android copy.

# Short Description
Android FINAL FANTASY VII port for PS Vita with black side borders and PlayStation prompts for stock game data.

# Download Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/download/v1.3.1/ff7_vita_v1.3.1.vpk

# Release Link
https://github.com/ggdrn/METEOR-VII-VITA/releases/tag/v1.3.1

# Sourcecode Link (Optional)
https://github.com/ggdrn/METEOR-VII-VITA

# Data Files Link (Optional)
Not distributed. Supply your own Android v1.0.29 APK and OBB. Installation guide: https://github.com/ggdrn/METEOR-VII-VITA/blob/HEAD/docs/DATA.md

# Requirements (Optional)
- A homebrew-enabled PS Vita.
- kubridge.skprx installed and enabled.
- libshacccg.suprx in ur0:data/ or ur0:data/external/.
- Android FINAL FANTASY VII v1.0.29 game data.
- Approximately 2.1 GB free for game data, plus space for the port.

# Installation
Install `ff7_vita_v1.3.1.vpk` with VitaShell, then copy `base.apk` and `main.30.com.square_enix.android_googleplay.FFVII.obb` intact to `ux0:data/ffvii-vita/`. Existing users can install over the previous version and keep their data and settings; back up `ux0:data/ffvii-vita/save/` first.

# Controls
Hold L + R and press Start to open the port menu. Use L/R to switch tabs, Up/Down to select, Left/Right or Cross to change a setting and Circle to close. In Gameplay, select SIDE BORDERS for GRADIENT/BLACK, ENCOUNTER RATE for OFF/NORMAL/2X/4X, ENCOUNTERS BUTTON for SHOW/HIDE, or SCREEN for 4:3/16:9.
